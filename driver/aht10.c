// SPDX-License-Identifier: GPL-2.0
/*
 * aht10.c - AHT10 溫濕度感測器 Kernel-space I2C 驅動
 *
 * 架構說明:
 *   1. i2c_driver 透過 of_match_table 與 Device Tree 中的
 *      compatible = "topsoft,aht10" 節點匹配 -> 觸發 probe()
 *   2. probe() 內註冊一個字元裝置 /dev/aht10
 *   3. User-space 對 /dev/aht10 執行 read() 時,
 *      驅動會在核心層向 AHT10 送出「觸發量測」指令、
 *      等待轉換完成、讀回 6 bytes 原始資料、解析成
 *      實際溫濕度數值,再透過 copy_to_user() 交給使用者。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/math64.h>

#define DRIVER_NAME		"aht10"
#define AHT10_CLASS_NAME	"aht10_class"

/* ---- AHT10 指令集 (參考 AHT10 datasheet) ---- */
#define AHT10_CMD_INIT		0xE1	/* 校正初始化指令 */
#define AHT10_CMD_TRIGGER	0xAC	/* 觸發量測指令 */
#define AHT10_CMD_SOFT_RESET	0xBA	/* 軟體重置 */

#define AHT10_STATUS_BUSY_BIT	0x80	/* status byte bit7 = 1 表示忙碌中 */
#define AHT10_MEAS_DELAY_MS	80	/* 觸發量測後至少等待 75~80ms */
#define AHT10_INIT_DELAY_MS	20

/* 自訂 ioctl:觸發一次軟體重置 */
#define AHT10_IOC_MAGIC		'A'
#define AHT10_IOC_RESET		_IO(AHT10_IOC_MAGIC, 0)

struct aht10_data {
	struct i2c_client *client;
	struct cdev cdev;
	struct mutex lock;
	dev_t devt;
};

static struct class *aht10_class;
static dev_t aht10_devt_base;
static atomic_t aht10_dev_count = ATOMIC_INIT(0);

/* ------------------------------------------------------------------ */
/* 核心層 I2C 傳輸函式                                                  */
/* ------------------------------------------------------------------ */

/* 送出 AHT10 校正/初始化指令 (0xE1 0x08 0x00) */
static int aht10_send_init_cmd(struct i2c_client *client)
{
	u8 buf[3] = { AHT10_CMD_INIT, 0x08, 0x00 };
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = sizeof(buf),
		.buf = buf,
	};
	int ret;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1) {
		dev_err(&client->dev, "init cmd failed, ret=%d\n", ret);
		return (ret < 0) ? ret : -EIO;
	}
	msleep(AHT10_INIT_DELAY_MS);
	return 0;
}

/* 觸發一次量測 (0xAC 0x33 0x00),並讀回 6 bytes 原始資料 */
static int aht10_trigger_and_read(struct i2c_client *client, u8 *raw6)
{
	u8 cmd[3] = { AHT10_CMD_TRIGGER, 0x33, 0x00 };
	struct i2c_msg tx_msg = {
		.addr = client->addr,
		.flags = 0,
		.len = sizeof(cmd),
		.buf = cmd,
	};
	struct i2c_msg rx_msg = {
		.addr = client->addr,
		.flags = I2C_M_RD,
		.len = 6,
		.buf = raw6,
	};
	int ret, retry;

	ret = i2c_transfer(client->adapter, &tx_msg, 1);
	if (ret != 1)
		return (ret < 0) ? ret : -EIO;

	msleep(AHT10_MEAS_DELAY_MS);

	/* 若 busy bit 仍為 1,最多再等待/重試幾次 */
	for (retry = 0; retry < 8; retry++) {
		ret = i2c_transfer(client->adapter, &rx_msg, 1);
		if (ret != 1)
			return (ret < 0) ? ret : -EIO;

		if (!(raw6[0] & AHT10_STATUS_BUSY_BIT))
			return 0;

		msleep(25);
	}

	return -ETIMEDOUT;
}

/* 將 6 bytes 原始資料解析為攝氏溫度(x100) 與相對濕度(x100) 的整數值,
 * 避免在 kernel space 使用浮點數運算
 */
static void aht10_parse_raw(const u8 *raw6, int *humi_x100, int *temp_x100)
{
	u32 raw_humidity, raw_temp;

	raw_humidity = ((u32)raw6[1] << 12) | ((u32)raw6[2] << 4) |
		       ((u32)raw6[3] >> 4);
	raw_temp = (((u32)raw6[3] & 0x0F) << 16) | ((u32)raw6[4] << 8) |
		   (u32)raw6[5];

	/* RH% = raw_humidity / 2^20 * 100 */
	*humi_x100 = (int)div_u64((u64)raw_humidity * 10000, 1048576);

	/* Temp(C) = raw_temp / 2^20 * 200 - 50 */
	*temp_x100 = (int)div_u64((u64)raw_temp * 20000, 1048576) - 5000;
}

/* ------------------------------------------------------------------ */
/* 字元裝置 file_operations                                            */
/* ------------------------------------------------------------------ */

static int aht10_open(struct inode *inode, struct file *file)
{
	struct aht10_data *data = container_of(inode->i_cdev,
						struct aht10_data, cdev);
	file->private_data = data;
	return 0;
}

static int aht10_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t aht10_read(struct file *file, char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct aht10_data *data = file->private_data;
	u8 raw6[6];
	int humi_x100, temp_x100;
	char kbuf[64];
	int len, ret;

	if (*ppos > 0)
		return 0; /* 每次 open 後只回傳一筆量測資料 */

	mutex_lock(&data->lock);
	ret = aht10_trigger_and_read(data->client, raw6);
	mutex_unlock(&data->lock);

	if (ret) {
		dev_err(&data->client->dev, "measurement failed: %d\n", ret);
		return ret;
	}

	aht10_parse_raw(raw6, &humi_x100, &temp_x100);

	/* 輸出格式: "TEMP=23.45 HUMI=56.12\n" 方便 user-space daemon 解析 */
	len = scnprintf(kbuf, sizeof(kbuf), "TEMP=%d.%02d HUMI=%d.%02d\n",
			temp_x100 / 100, abs(temp_x100 % 100),
			humi_x100 / 100, abs(humi_x100 % 100));

	if (len > count)
		len = count;

	if (copy_to_user(ubuf, kbuf, len))
		return -EFAULT;

	*ppos += len;
	return len;
}

static long aht10_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	struct aht10_data *data = file->private_data;
	u8 reset_cmd = AHT10_CMD_SOFT_RESET;
	struct i2c_msg msg = {
		.addr = data->client->addr,
		.flags = 0,
		.len = 1,
		.buf = &reset_cmd,
	};
	int ret;

	switch (cmd) {
	case AHT10_IOC_RESET:
		mutex_lock(&data->lock);
		ret = i2c_transfer(data->client->adapter, &msg, 1);
		mutex_unlock(&data->lock);
		if (ret != 1)
			return (ret < 0) ? ret : -EIO;
		msleep(20);
		return aht10_send_init_cmd(data->client);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations aht10_fops = {
	.owner = THIS_MODULE,
	.open = aht10_open,
	.release = aht10_release,
	.read = aht10_read,
	.unlocked_ioctl = aht10_ioctl,
};

/* ------------------------------------------------------------------ */
/* i2c_driver probe / remove                                          */
/* ------------------------------------------------------------------ */

static int aht10_probe(struct i2c_client *client)
{
	struct aht10_data *data;
	struct device *dev_node;
	int minor, ret;

	dev_info(&client->dev, "probing AHT10 at addr 0x%02x\n",
		 client->addr);

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);
	i2c_set_clientdata(client, data);

	minor = atomic_fetch_inc(&aht10_dev_count);
	data->devt = MKDEV(MAJOR(aht10_devt_base), minor);

	cdev_init(&data->cdev, &aht10_fops);
	data->cdev.owner = THIS_MODULE;

	ret = cdev_add(&data->cdev, data->devt, 1);
	if (ret) {
		dev_err(&client->dev, "cdev_add failed: %d\n", ret);
		return ret;
	}

	dev_node = device_create(aht10_class, &client->dev, data->devt,
				  NULL, "aht10");
	if (IS_ERR(dev_node)) {
		cdev_del(&data->cdev);
		return PTR_ERR(dev_node);
	}

	/* 開機時先做一次校正初始化,確保後續量測數值正確 */
	ret = aht10_send_init_cmd(client);
	if (ret)
		dev_warn(&client->dev, "init cmd failed, will retry on read\n");

	dev_info(&client->dev, "AHT10 ready at /dev/aht10\n");
	return 0;
}

static void aht10_remove(struct i2c_client *client)
{
	struct aht10_data *data = i2c_get_clientdata(client);

	device_destroy(aht10_class, data->devt);
	cdev_del(&data->cdev);
	dev_info(&client->dev, "AHT10 removed\n");
}

static const struct of_device_id aht10_of_match[] = {
	{ .compatible = "topsoft,aht10" },
	{ }
};
MODULE_DEVICE_TABLE(of, aht10_of_match);

static const struct i2c_device_id aht10_id[] = {
	{ "aht10", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aht10_id);

static struct i2c_driver aht10_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = aht10_of_match,
	},
	.probe = aht10_probe,
	.remove = aht10_remove,
	.id_table = aht10_id,
};

/* ------------------------------------------------------------------ */
/* 模組進入 / 退出                                                      */
/* ------------------------------------------------------------------ */

static int __init aht10_module_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&aht10_devt_base, 0, 8, DRIVER_NAME);
	if (ret) {
		pr_err("aht10: alloc_chrdev_region failed\n");
		return ret;
	}

	aht10_class = class_create(AHT10_CLASS_NAME);
	if (IS_ERR(aht10_class)) {
		unregister_chrdev_region(aht10_devt_base, 8);
		return PTR_ERR(aht10_class);
	}

	ret = i2c_add_driver(&aht10_driver);
	if (ret) {
		class_destroy(aht10_class);
		unregister_chrdev_region(aht10_devt_base, 8);
		return ret;
	}

	pr_info("aht10: module loaded\n");
	return 0;
}

static void __exit aht10_module_exit(void)
{
	i2c_del_driver(&aht10_driver);
	class_destroy(aht10_class);
	unregister_chrdev_region(aht10_devt_base, 8);
	pr_info("aht10: module unloaded\n");
}

module_init(aht10_module_init);
module_exit(aht10_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Team");
MODULE_DESCRIPTION("AHT10 Temperature/Humidity Sensor I2C Kernel Driver");
MODULE_VERSION("0.1");