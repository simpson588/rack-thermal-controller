#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>
#include <mosquitto.h>
#include <cjson/cJSON.h>

// ===== 設定檔與硬體節點定義 =====
#define CONFIG_FILE "/home/pi/projects/fan/mqtt_config.txt"
#define DEFAULT_BROKER_HOST "192.168.69.190"
#define AHT10_DEV "/dev/aht10"
#define CPU_TEMP_FILE "/sys/class/thermal/thermal_zone0/temp"
#define MTD_LOG_DEV "/dev/mtd0"       // Winbond W25Q64 7MB 黑盒子日誌區

// ===== MQTT 設定 =====
#define MQTT_BROKER_PORT 1883
#define MQTT_CLIENT_ID   "rack_thermal_node_01"
#define TOPIC_REPORT_IN  "rack/thermal/report-in"
#define TOPIC_STATE      "rack/thermal/state"
#define TOPIC_TELEMETRY  "rack/thermal/telemetry"
#define TOPIC_CMD        "rack/thermal/cmd"

#define SECTOR_SIZE       4096
#define MAX_LOG_SIZE     (7 * 1024 * 1024)
#define FLUSH_BATCH_SIZE 50

// ===== 黑盒子 timestamp 合理性檢查邊界 =====
#define BLACKBOX_MIN_VALID_TS 1577836800u       // 2020-01-01 00:00:00 UTC
#define BLACKBOX_FUTURE_TOLERANCE_SEC 3600u     // 容許未來 1 小時 (RTC 容錯)

// ===== 風扇啟動寬限期設定 =====
#define LOOP_INTERVAL_US    1000000              // 主迴圈週期 1000ms
#define SPINUP_GRACE_CYCLES 3                   // 3 秒寬限期

// ===== 黑盒子日誌二進位結構體 (16 Bytes) =====
typedef struct {
    uint32_t timestamp;
    float temp;
    float humi;
    int32_t fan_pct;
} __attribute__((packed)) blackbox_entry_t;

// ===== 全域變數 =====
int uart_fd = -1;
char saved_pwm_path[256] = {0};
char saved_tach_path[256] = {0};      // 實體 TACH 回授節點路徑
char active_broker_host[128] = DEFAULT_BROKER_HOST;
struct mosquitto *mosq = NULL;
volatile sig_atomic_t keep_running = 1;
volatile int mqtt_connected = 0;

uint32_t blackbox_write_offset = 0;
uint32_t blackbox_read_offset = 0;

char current_mode[16] = "AUTO";
int manual_fan_pct = 0;
float temp_threshold = 40.0;

int alarm_active = 0;
char alarm_code[32] = "NONE";

// ===== 函式宣告 =====
void load_broker_config(char *host_out, size_t max_len);
void handle_signal(int sig);
void cleanup_and_exit(void);
int init_uart(void);
float get_env_temp(float *humi_out);
float get_cpu_temp(void);
int find_fan_nodes(char *pwm_buf, char *tach_buf);
void set_fan_pwm(const char *pwm_path, int pwm_val);
int get_fan_rpm(const char *tach_path);
int calc_pwm_env(float temp);
int calc_pwm_cpu(float temp);

void flash_blackbox_init(void);
void flash_blackbox_write(float temp, float hum, int fan_pct);
void flash_blackbox_process_chunk(void);
int flash_blackbox_erase_range(uint32_t start_offset, uint32_t length);

void publish_report_in(int online);
void publish_state(void);
void publish_telemetry(float temp, float hum, int fan_pct, int rpm);
void publish_replayed_telemetry(uint32_t ts, float temp, float hum, int fan_pct, int rpm);
void on_connect(struct mosquitto *mosq, void *obj, int rc);
void on_disconnect(struct mosquitto *mosq, void *obj, int rc);
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);

// ===== 小工具：安全字串複製 =====
static void safe_strncpy(char *dst, const char *src, size_t dst_size) {
    if (dst_size == 0) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

// ===== 小工具：黑盒子 Entry 合法性檢查 =====
static inline int is_valid_blackbox_entry(const blackbox_entry_t *entry, uint32_t now) {
    if (entry->timestamp == 0xFFFFFFFF || entry->timestamp == 0) {
        return 0; // 空白區塊或未寫入
    }
    if (entry->timestamp < BLACKBOX_MIN_VALID_TS || 
        entry->timestamp > (now + BLACKBOX_FUTURE_TOLERANCE_SEC)) {
        return 0; // 異常 timestamp 或損毀紀錄
    }
    return 1;
}

// ===== MTD Flash 操作實作 =====
int flash_blackbox_erase_range(uint32_t start_offset, uint32_t length) {
    int fd = open(MTD_LOG_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "❌ [Flash] erase: open(%s) failed @0x%X: %s\n",
                MTD_LOG_DEV, start_offset, strerror(errno));
        fflush(stderr);
        return -1;
    }

    struct erase_info_user erase;
    erase.start = start_offset;
    erase.length = ((length + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;

    if (ioctl(fd, MEMERASE, &erase) < 0) {
        fprintf(stderr, "❌ [Flash] erase: MEMERASE failed @0x%X len=%u: %s\n",
                start_offset, erase.length, strerror(errno));
        fflush(stderr);
        close(fd);
        return -1;
    }
    close(fd);
    printf("✅ [Flash] Erased sector @0x%X (len=%u)\n", start_offset, erase.length);
    fflush(stdout);
    return 0;
}

void flash_blackbox_init(void) {
    blackbox_write_offset = 0;
    blackbox_read_offset = 0;

    int fd = open(MTD_LOG_DEV, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "❌ [Flash] init: open(%s) failed: %s\n", MTD_LOG_DEV, strerror(errno));
        fflush(stderr);
        return;
    }

    blackbox_entry_t entry;
    uint32_t now = (uint32_t)time(NULL);

    while (blackbox_write_offset + sizeof(entry) <= MAX_LOG_SIZE) {
        if (read(fd, &entry, sizeof(entry)) != sizeof(entry)) break;
        if (!is_valid_blackbox_entry(&entry, now)) {
            break;
        }
        blackbox_write_offset += sizeof(entry);
    }
    close(fd);

    printf("📦 [Flash] Blackbox initialized. Cached: %u bytes (%lu entries)\n",
           blackbox_write_offset, (unsigned long)(blackbox_write_offset / sizeof(blackbox_entry_t)));
    fflush(stdout);
}

void flash_blackbox_write(float temp, float hum, int fan_pct) {
    if (blackbox_write_offset + sizeof(blackbox_entry_t) > MAX_LOG_SIZE) {
        printf("⚠️ [Flash] Buffer full! Resetting offsets...\n");
        fflush(stdout);
        if (flash_blackbox_erase_range(0, SECTOR_SIZE) != 0) {
            fprintf(stderr, "❌ [Flash] write aborted: buffer-full erase failed\n");
            fflush(stderr);
            return;
        }
        blackbox_write_offset = 0;
        blackbox_read_offset = 0;
    } else if ((blackbox_write_offset % SECTOR_SIZE) == 0) {
        if (flash_blackbox_erase_range(blackbox_write_offset, SECTOR_SIZE) != 0) {
            fprintf(stderr, "❌ [Flash] write aborted: sector erase failed @0x%X\n",
                    blackbox_write_offset);
            fflush(stderr);
            return;
        }
    }

    // 🌟 使用 O_RDWR | O_SYNC 確保立即寫入實體 Flash
    int fd = open(MTD_LOG_DEV, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "❌ [Flash] write: open(%s) failed: %s\n", MTD_LOG_DEV, strerror(errno));
        fflush(stderr);
        return;
    }

    blackbox_entry_t entry;
    entry.timestamp = (uint32_t)time(NULL);
    entry.temp = temp;
    entry.humi = hum;
    entry.fan_pct = fan_pct;

    if (lseek(fd, blackbox_write_offset, SEEK_SET) == (off_t)-1) {
        fprintf(stderr, "❌ [Flash] write: lseek to 0x%X failed: %s\n",
                blackbox_write_offset, strerror(errno));
        fflush(stderr);
        close(fd);
        return;
    }

    ssize_t n = write(fd, &entry, sizeof(entry));
    if (n != (ssize_t)sizeof(entry)) {
        fprintf(stderr, "❌ [Flash] write: wrote %zd/%zu bytes @0x%X: %s\n",
                n, sizeof(entry), blackbox_write_offset,
                (n < 0) ? strerror(errno) : "short write");
        fflush(stderr);
        close(fd);
        return;
    }

    fsync(fd);
    close(fd);

    blackbox_write_offset += sizeof(entry);
    printf("✅ [Flash] Wrote entry @0x%X (ts=%u temp=%.1f) -> offset now %u bytes\n",
           blackbox_write_offset - (uint32_t)sizeof(entry), entry.timestamp, entry.temp,
           blackbox_write_offset);
    fflush(stdout);
}

void publish_replayed_telemetry(uint32_t ts, float temp, float hum, int fan_pct, int rpm) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "timestamp", (double)ts);
    cJSON_AddNumberToObject(root, "temp", (double)((int)(temp * 10 + 0.5)) / 10.0);
    cJSON_AddNumberToObject(root, "hum", (double)((int)(hum * 10 + 0.5)) / 10.0);
    cJSON_AddNumberToObject(root, "fan_speed_pct", fan_pct);
    cJSON_AddNumberToObject(root, "fan_rpm", rpm);
    cJSON_AddBoolToObject(root, "replayed", 1);
    char *json_str = cJSON_PrintUnformatted(root);
    mosquitto_publish(mosq, NULL, TOPIC_TELEMETRY, strlen(json_str), json_str, 0, false);
    printf("📡 [MQTT Replay] %s\n", json_str);
    fflush(stdout);
    free(json_str);
    cJSON_Delete(root);
}

void flash_blackbox_process_chunk(void) {
    if (blackbox_read_offset >= blackbox_write_offset) return;

    int fd = open(MTD_LOG_DEV, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "❌ [Flash] replay: open(%s) failed: %s\n", MTD_LOG_DEV, strerror(errno));
        fflush(stderr);
        return;
    }

    if (lseek(fd, blackbox_read_offset, SEEK_SET) == (off_t)-1) {
        fprintf(stderr, "❌ [Flash] replay: lseek to 0x%X failed: %s\n",
                blackbox_read_offset, strerror(errno));
        fflush(stderr);
        close(fd);
        return;
    }

    blackbox_entry_t entry;
    int processed = 0;
    uint32_t now = (uint32_t)time(NULL);

    while (processed < FLUSH_BATCH_SIZE && blackbox_read_offset < blackbox_write_offset) {
        if (read(fd, &entry, sizeof(entry)) != sizeof(entry)) break;
        if (is_valid_blackbox_entry(&entry, now)) {
            int rpm = entry.fan_pct * 45;
            publish_replayed_telemetry(entry.timestamp, entry.temp, entry.humi, entry.fan_pct, rpm);
        } else {
            printf("⚠️ [Flash Replay] Skipped corrupted entry @ offset 0x%X (ts=%u)\n",
                   blackbox_read_offset, entry.timestamp);
        }
        blackbox_read_offset += sizeof(entry);
        processed++;
    }
    close(fd);

    printf("🔄 [Flash] Replayed batch (%d entries). Progress: %u / %u bytes\n",
           processed, blackbox_read_offset, blackbox_write_offset);
    fflush(stdout);

    if (blackbox_read_offset >= blackbox_write_offset) {
        printf("✅ [Flash] All offline logs replayed. Erasing sectors & resetting buffer.\n");
        flash_blackbox_erase_range(0, blackbox_write_offset);
        blackbox_write_offset = 0;
        blackbox_read_offset = 0;
    }
}

void load_broker_config(char *host_out, size_t max_len) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) fp = fopen("mqtt_config.txt", "r");
    if (fp) {
        char temp_buf[128] = {0};
        if (fscanf(fp, "%127s", temp_buf) == 1) {
            safe_strncpy(host_out, temp_buf, max_len);
            printf("📁 [Config] Successfully loaded Broker Host: %s\n", host_out);
        }
        fclose(fp);
    }
}

void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

void cleanup_and_exit(void) {
    printf("\n🛑 [Daemon Exit] Shutting down cleanly...\n");
    fflush(stdout);

    if (uart_fd != -1) {
        for (int retry = 0; retry < 3; retry++) {
            write(uart_fd, "\nA:0\n", 5);
            write(uart_fd, "OFF\n", 4);
            write(uart_fd, "0\n", 2);
            tcdrain(uart_fd);
            usleep(20000);
        }
        close(uart_fd);
        uart_fd = -1;
    }

    if (strlen(saved_pwm_path) > 0) {
        set_fan_pwm(saved_pwm_path, 0);
    }

    if (mosq) {
        if (mqtt_connected) {
            publish_report_in(0);
            mosquitto_loop(mosq, 100, 1);
        }
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        mosq = NULL;
    }
    mosquitto_lib_cleanup();
    printf("✅ [Cleanup] All done.\n");
    fflush(stdout);
}

int init_uart(void) {
    const char *ports[] = {"/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2", NULL};
    int fd = -1;

    for (int i = 0; ports[i] != NULL; i++) {
        fd = open(ports[i], O_RDWR | O_NOCTTY | O_NDELAY);
        if (fd != -1) {
            printf("🔌 Found & Connected UART Device at %s\n", ports[i]);
            break;
        }
    }

    if (fd == -1) return -1;

    struct termios options;
    tcgetattr(fd, &options);

    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);

    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag |= (CLOCAL | CREAD);

    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_oflag &= ~OPOST;

    tcsetattr(fd, TCSANOW, &options);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

float get_env_temp(float *humi_out) {
    int fd = open(AHT10_DEV, O_RDONLY);
    if (fd < 0) return -1.0;
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1.0;
    float temp = 0.0, humi = 0.0;
    if (sscanf(buf, "TEMP=%f HUMI=%f", &temp, &humi) == 2) {
        if (humi_out) *humi_out = humi;
        return temp;
    }
    return -1.0;
}

float get_cpu_temp(void) {
    FILE *fp = fopen(CPU_TEMP_FILE, "r");
    if (!fp) return -1.0;
    int millideg = 0;
    if (fscanf(fp, "%d", &millideg) != 1) {
        fclose(fp);
        return -1.0;
    }
    fclose(fp);
    return millideg / 1000.0f;
}

int find_fan_nodes(char *pwm_buf, char *tach_buf) {
    DIR *dir = opendir("/sys/class/hwmon");
    struct dirent *ent;
    char name_path[256], name_buf[64];
    int found = 0;
    if (!dir) return 0;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) == 0) {
            snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/%s/name", ent->d_name);
            FILE *fp = fopen(name_path, "r");
            if (fp) {
                if (fgets(name_buf, sizeof(name_buf), fp)) {
                    if (strncmp(name_buf, "pwmfan", 6) == 0 || strncmp(name_buf, "rpi_poe_fan", 11) == 0) {
                        snprintf(pwm_buf, 256, "/sys/class/hwmon/%s/pwm1", ent->d_name);
                        snprintf(tach_buf, 256, "/sys/class/hwmon/%s/fan1_input", ent->d_name);
                        found = 1;
                        fclose(fp);
                        break;
                    }
                }
                fclose(fp);
            }
        }
    }
    closedir(dir);
    return found;
}

void set_fan_pwm(const char *pwm_path, int pwm_val) {
    FILE *fp = fopen(pwm_path, "w");
    if (fp) {
        fprintf(fp, "%d\n", pwm_val);
        fclose(fp);
    }
}

int get_fan_rpm(const char *tach_path) {
    if (strlen(tach_path) == 0) return 0;
    FILE *fp = fopen(tach_path, "r");
    if (!fp) return 0;
    int rpm = 0;
    if (fscanf(fp, "%d", &rpm) != 1) {
        rpm = 0;
    }
    fclose(fp);
    return rpm;
}

int calc_pwm_env(float temp) {
    if (temp < 26.0f) return 0;
    if (temp < 29.0f) return 100;
    if (temp < 33.0f) return 150;
    if (temp < 37.0f) return 200;
    return 255;
}

int calc_pwm_cpu(float temp) {
    if (temp < 50.0f) return 0;
    if (temp < 60.0f) return 150;
    if (temp < 75.0f) return 200;
    return 255;
}

void publish_report_in(int online) {
    if (!mqtt_connected) return;
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"online\": %s}", online ? "true" : "false");
    mosquitto_publish(mosq, NULL, TOPIC_REPORT_IN, strlen(payload), payload, 1, true);
    printf("📤 [MQTT Publish] %s -> %s (QoS=1, Retain=true)\n", TOPIC_REPORT_IN, payload);
    fflush(stdout);
}

void publish_state(void) {
    if (!mqtt_connected) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(root, "mode", current_mode);
    cJSON_AddBoolToObject(root, "alarm", alarm_active);
    cJSON_AddStringToObject(root, "alarm_code", alarm_code);
    cJSON_AddNumberToObject(root, "temp_threshold", temp_threshold);
    char *json_str = cJSON_PrintUnformatted(root);
    mosquitto_publish(mosq, NULL, TOPIC_STATE, strlen(json_str), json_str, 1, true);
    printf("📤 [MQTT Publish] %s -> %s (QoS=1, Retain=true)\n", TOPIC_STATE, json_str);
    fflush(stdout);
    free(json_str);
    cJSON_Delete(root);
}

void publish_telemetry(float temp, float hum, int fan_pct, int rpm) {
    printf("📊 [Local] %.1f°C | %.1f%% | Fan: %d%% (%d RPM) | %s\n",
            temp, hum, fan_pct, rpm, 
            mqtt_connected ? "📡 MQTT" : "💾 Flash");
    fflush(stdout);

    if (!mqtt_connected) {
        flash_blackbox_write(temp, hum, fan_pct);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "temp", (double)((int)(temp * 10 + 0.5)) / 10.0);
    cJSON_AddNumberToObject(root, "hum", (double)((int)(hum * 10 + 0.5)) / 10.0);
    cJSON_AddNumberToObject(root, "fan_speed_pct", fan_pct);
    cJSON_AddNumberToObject(root, "fan_rpm", rpm);
    char *json_str = cJSON_PrintUnformatted(root);
    mosquitto_publish(mosq, NULL, TOPIC_TELEMETRY, strlen(json_str), json_str, 0, false);
    printf("📡 [MQTT Telemetry] %s\n", json_str);
    fflush(stdout);
    free(json_str);
    cJSON_Delete(root);
}

void on_connect(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq; (void)obj;
    if (rc == 0) {
        printf("✅ Connected to MQTT Broker successfully!\n");
        mqtt_connected = 1;
        publish_report_in(1);
        mosquitto_subscribe(mosq, NULL, TOPIC_CMD, 1);
        publish_state();
        fflush(stdout);
    } else {
        mqtt_connected = 0;
    }
}

void on_disconnect(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq; (void)obj; (void)rc;
    mqtt_connected = 0;
    printf("🔌⚠️ [MQTT] Broker disconnected. Switched to Flash Blackbox mode.\n");
    fflush(stdout);
}

void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
    (void)mosq; (void)obj;
    if (strcmp(msg->topic, TOPIC_CMD) == 0) {
        cJSON *root = cJSON_Parse((char *)msg->payload);
        if (!root) return;
        int state_changed = 0;

        cJSON *item_mode = cJSON_GetObjectItem(root, "mode");
        if (cJSON_IsString(item_mode) && item_mode->valuestring) {
            if (strcmp(item_mode->valuestring, "AUTO") == 0 || strcmp(item_mode->valuestring, "MANUAL") == 0) {
                safe_strncpy(current_mode, item_mode->valuestring, sizeof(current_mode));
                state_changed = 1;
            }
        }

        cJSON *item_fan = cJSON_GetObjectItem(root, "fan_speed_pct");
        if (cJSON_IsNumber(item_fan)) {
            manual_fan_pct = item_fan->valueint;
            if (manual_fan_pct < 0) manual_fan_pct = 0;
            if (manual_fan_pct > 100) manual_fan_pct = 100;
        }

        cJSON *item_th = cJSON_GetObjectItem(root, "temp_threshold");
        if (cJSON_IsNumber(item_th)) {
            temp_threshold = (float)item_th->valuedouble;
            state_changed = 1;
        }

        cJSON_Delete(root);
        if (state_changed) publish_state();
    }
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    printf("====================================================\n");
    printf("🚀 Thermal Management Node & MTD Blackbox Daemon\n");
    printf("====================================================\n");

    load_broker_config(active_broker_host, sizeof(active_broker_host));

    uart_fd = init_uart();
    if (uart_fd != -1) {
        write(uart_fd, "\nA:0\n", 5);
        tcdrain(uart_fd);
        printf("💡 [UART] Connection established. Pico W initialized.\n");
    }

    if (find_fan_nodes(saved_pwm_path, saved_tach_path)) {
        printf("🌀 Fan PWM Path : %s\n", saved_pwm_path);
        printf("🔄 Fan TACH Path: %s\n", saved_tach_path);
    }

    flash_blackbox_init();

    mosquitto_lib_init();
    mosq = mosquitto_new(MQTT_CLIENT_ID, true, NULL);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_message_callback_set(mosq, on_message);

    mosquitto_connect_async(mosq, active_broker_host, MQTT_BROKER_PORT, 10);
    mosquitto_loop_start(mosq);

    // 🌟 稍微延遲讓 MQTT 連線回呼先行完成
    usleep(500000);

    static int stall_counter = 0;
    static int alarm_heartbeat_cnt = 0;
    static int prev_fan_running = 0;
    static int spinup_grace = 0;
    static int is_first_run = 1;

    while (keep_running) {
        if (mqtt_connected && blackbox_read_offset < blackbox_write_offset) {
            flash_blackbox_process_chunk();
        }

        float humi = 0.0;
        float env_temp = get_env_temp(&humi);
        float cpu_temp = get_cpu_temp();

        int target_pwm = 0;
        int fan_pct = 0;
        int previous_alarm = alarm_active;
        char prev_alarm_code[32];
        safe_strncpy(prev_alarm_code, alarm_code, sizeof(prev_alarm_code));

        if (strcmp(current_mode, "MANUAL") == 0) {
            fan_pct = manual_fan_pct;
            target_pwm = (int)((manual_fan_pct / 100.0) * 255.0);
        } else {
            int pwm_env = (env_temp > 0) ? calc_pwm_env(env_temp) : 200;
            int pwm_cpu = (cpu_temp > 0) ? calc_pwm_cpu(cpu_temp) : 0;
            target_pwm = (pwm_cpu > pwm_env) ? pwm_cpu : pwm_env;
            fan_pct = (int)((target_pwm / 255.0) * 100.0);
        }

        if (strlen(saved_pwm_path) > 0) {
            set_fan_pwm(saved_pwm_path, target_pwm);
        }

        int real_rpm = get_fan_rpm(saved_tach_path);
        if (strlen(saved_tach_path) == 0) {
            real_rpm = (int)(fan_pct * 45);
        }

        int fan_should_run = (target_pwm >= 100);

        if (is_first_run) {
            if (fan_should_run && strlen(saved_tach_path) > 0 && real_rpm > 0) {
                spinup_grace = 0;
            } else if (fan_should_run) {
                spinup_grace = SPINUP_GRACE_CYCLES;
            }
            is_first_run = 0;
        } else {
            if (fan_should_run && !prev_fan_running) {
                spinup_grace = SPINUP_GRACE_CYCLES;
            }
            if (!fan_should_run) {
                spinup_grace = 0;
            }
        }
        prev_fan_running = fan_should_run;

        int fan_stalled = 0;
        if (strlen(saved_tach_path) > 0 && fan_should_run) {
            if (spinup_grace > 0) {
                spinup_grace--;
                stall_counter = 0;
            } else if (real_rpm == 0) {
                stall_counter++;
                if (stall_counter >= 2) fan_stalled = 1;
            } else {
                stall_counter = 0;
            }
        } else {
            stall_counter = 0;
        }

        if (fan_stalled) {
            alarm_active = 1;
            safe_strncpy(alarm_code, "FAN_STALL", sizeof(alarm_code));
        } else if (env_temp < 0) {
            alarm_active = 1;
            safe_strncpy(alarm_code, "SENSOR_ERROR", sizeof(alarm_code));
        } else if (env_temp > 40.0f || env_temp >= temp_threshold || cpu_temp >= 75.0f) {
            alarm_active = 1;
            safe_strncpy(alarm_code, "HIGH_TEMP", sizeof(alarm_code));
        } else {
            alarm_active = 0;
            safe_strncpy(alarm_code, "NONE", sizeof(alarm_code));
        }

        if (alarm_active) {
            if (!previous_alarm || strcmp(prev_alarm_code, alarm_code) != 0) {
                printf("🔥 [ALARM TRIGGERED] Code: %s\n", alarm_code);
                if (uart_fd != -1) {
                    write(uart_fd, "A:1\n", 4);
                    tcdrain(uart_fd);
                }
                publish_state();
                alarm_heartbeat_cnt = 0;
            } else {
                if (++alarm_heartbeat_cnt >= 2) {
                    if (uart_fd != -1) {
                        write(uart_fd, "A:1\n", 4);
                        tcdrain(uart_fd);
                    }
                    alarm_heartbeat_cnt = 0;
                }
            }
        } else {
            if (previous_alarm) {
                printf("🟢 [NORMAL] System recovered. Alarm cleared.\n");
                if (uart_fd != -1) {
                    write(uart_fd, "A:0\n", 4);
                    tcdrain(uart_fd);
                }
                publish_state();
            }
            if (uart_fd != -1) {
                char sync_cmd[32];
                float safe_temp = (env_temp > 0) ? env_temp : 0.0f;
                snprintf(sync_cmd, sizeof(sync_cmd), "S:%d,%.1f\n", target_pwm, safe_temp);
                write(uart_fd, sync_cmd, strlen(sync_cmd));
            }
        }

        publish_telemetry(env_temp, humi, fan_pct, real_rpm);

        usleep(LOOP_INTERVAL_US);
    }

    cleanup_and_exit();
    return 0;
}