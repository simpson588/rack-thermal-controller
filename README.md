# 機櫃溫度環控模組 (Edge Thermal & MTD Blackbox)

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Raspberry%20Pi%205%20%7C%20RP2040-red.svg)](https://www.raspberrypi.com/)
[![Kernel](https://img.shields.io/badge/Linux%20Kernel-6.6%2B%20(BCM2712)-green.svg)](https://kernel.org/)
[![Language](https://img.shields.io/badge/Language-C11%20%7C%20PIO%20ASM%20%7C%20DTS-orange.svg)]()
[![Communication](https://img.shields.io/badge/Protocol-MQTT%20%7C%20UART%20%7C%20I2C%20%7C%20SPI-lightgrey.svg)]()
[![Presentation](https://img.shields.io/badge/Presentation-專題簡報%20(PDF)-blue.svg?logo=adobeacrobatreader)](docs/專題簡報-機櫃溫度環控模組.pdf)
[![Demo Videos](https://img.shields.io/badge/YouTube-實機展示影片-red.svg?logo=youtube)](https://youtu.be/Q1XI1jcykzs)

針對機架伺服器散熱情境設計的**異質雙晶片協同架構 (Raspberry Pi 5 + RP2040) 溫度監控與斷網 Flash 黑盒子記錄系統**。

本系統結合 **Raspberry Pi 5 (Linux Kernel + Userspace Daemon)** 與 **RP2040 (即時 PIO 狀態機)**，整合底層 Linux 字元驅動開發、SPI-NOR Flash 斷網黑盒子暫存與重傳機制，以及微控制器硬體狀態機的即時聲光告警。

> 📌 **快速導覽與實機展示**：
> - 📄 **[點此線上預覽完整專案簡報 (PDF)](docs/專題簡報-機櫃溫度環控模組.pdf)**（涵蓋架構、Kernel 裁減數據、Root Cause 分析與邏輯分析儀波形）
> - 🎥 **[實作硬體運作 DEMO 影片 (YouTube)](https://youtu.be/Q1XI1jcykzs)**
> - 🌐 **[MQTT 雙向通訊連線終端機畫面 (YouTube)](https://youtu.be/MVUiem5-Jno)**
> - 💾 **[斷網黑盒子暫存與重連補傳終端機畫面 (YouTube)](https://youtu.be/BRNewQzBiq8)**

---

## 📑 目錄

- [一、 實機展示與 Demo 影片 (Live Demo)](#一-實機展示與-demo-影片-live-demo)
- [二、 專案技術簡報 (Presentation Slides)](#二-專案技術簡報-presentation-slides)
- [三、 系統架構與工作流程圖](#三-系統架構與工作流程圖)
- [四、 核心工程亮點與技術實現](#四-核心工程亮點與技術實現)
  - [1. Linux Kernel I2C 驅動模組與設備樹](#1-linux-kernel-i2c-驅動模組與設備樹)
  - [2. MTD 斷網黑盒子暫存與重傳機制 (Store-and-Forward)](#2-mtd-斷網黑盒子暫存與重傳機制-store-and-forward)
  - [3. 風扇轉速回授與防堵轉保護 (Stall Detection)](#3-風扇轉速回授與防堵轉保護-stall-detection)
  - [4. RP2040 PIO 時序控制與通訊逾時保護](#4-rp2040-pio-時序控制與通訊逾時保護)
- [五、 硬體清單與接線對照表 (Pinout)](#五-硬體清單與接線對照表-pinout)
- [六、 專案目錄結構](#六-專案目錄結構)
- [七、 編譯與部署指南](#七-編譯與部署指南)
  - [1. Device Tree Overlay 編譯與掛載](#1-device-tree-overlay-編譯與掛載)
  - [2. Linux 核心驅動編譯與載入](#2-linux-核心驅動編譯與載入)
  - [3. 使用者空間守護行程 (Daemon) 編譯與啟動](#3-使用者空間守護行程-daemon-編譯與啟動)
  - [4. RP2040 Pico 韌體編譯與燒錄](#4-rp2040-pico-韌體編譯與燒錄)
- [八、 MQTT 通訊協定與 JSON Payload 規範](#八-mqtt-通訊協定與-json-payload-規範)
- [九、 授權條款 (License)](#九-授權條款-license)

---

## 一、 實機展示與 Demo 影片 (Live Demo)

本專案提供實機硬體運作與雙向通訊測試影片，點擊下方預覽圖可在 YouTube 觀看展示：

| 實作硬體運作 DEMO | MQTT 雙向通訊正常連線 | 斷網黑盒子寫入與重連補傳 |
| :---: | :---: | :---: |
| [![實作DEMO影片](https://img.youtube.com/vi/Q1XI1jcykzs/hqdefault.jpg)](https://youtu.be/Q1XI1jcykzs) | [![MQTT正常連線](https://img.youtube.com/vi/MVUiem5-Jno/hqdefault.jpg)](https://youtu.be/MVUiem5-Jno) | [![斷線後重連黑盒子回傳](https://img.youtube.com/vi/BRNewQzBiq8/hqdefault.jpg)](https://youtu.be/BRNewQzBiq8) |
| **[▶️ 觀看實機動態展示](https://youtu.be/Q1XI1jcykzs)**<br>涵蓋 AHT10 感測、PWM 動態調速、WS2812B 燈條與聲光告警 | **[▶️ 觀看雙向通訊畫面](https://youtu.be/MVUiem5-Jno)**<br>左：溫控 RPi5 終端機<br>右：中控台伺服器終端機 | **[▶️ 觀看黑盒子回傳畫面](https://youtu.be/BRNewQzBiq8)**<br>離線時 W25Q64 Flash 暫存<br>網路重連後批次補傳至 MQTT Broker |

---

## 二、 專案技術簡報 (Presentation Slides)

完整專題技術簡報（PDF）已收錄於本專案目錄中：👉 **[專題簡報-機櫃溫度環控模組.pdf](docs/專題簡報-機櫃溫度環控模組.pdf)**

### 📑 簡報重點摘要
1. **系統架構與分工**：由 Linux 主機負責核心運算與網路傳輸，RP2040 專職即時控制與聲光告警。
2. **溫控策略與周邊聯動**：取機架進氣溫與 CPU 核心溫度的較高者（MAX Policy），動態調整風扇 PWM 轉速，並同步聯動 WS2812B 轉速燈條、狀態 LED 與蜂鳴器。
3. **Kernel 裁減優化成果**：針對 Debian Trixie (rpi-6.18.y) 移除多餘子系統，映像檔體積縮減 18.4%，開機耗時自 2.5s 縮短至 938ms (加速 63%)。
4. **5 大工程問題排查 (Root Cause 分析)**：包含 Device Tree 停用 spidev 解決 SPI 片選衝突與 10MHz 降頻提高時序裕度、Flash 抹除校驗、stdio 搶佔 UART0 中斷、newlib-nano 浮點數輕量解析、風扇啟動緩衝期 (spinup_grace)。
5. **邏輯分析儀實測驗證**：使用 PulseView 擷取 AHT10 I2C 20-bit 溫濕度封包，以及 RP2040 PIO 800kHz WS2812B 精準時序訊號。

---

## 三、 系統架構與工作流程圖

### 1. 系統架構與晶片分工 (System Architecture)
本系統採用雙晶片協同分工設計：由 Linux 主機（Raspberry Pi 5）負責執行 Userspace Daemon、感測器讀取與 MQTT 網路通訊；微控制器（RP2040）專職微秒級硬體時序控制與即時聲光告警：

![系統架構圖](docs/images/system_architecture.png)

<details>
<summary><b>點此展開查看 Mermaid 邏輯架構圖 (Textual Logic Graph)</b></summary>

```mermaid
graph TB
    subgraph Edge_Node ["Raspberry Pi 5 (Edge Linux Host)"]
        subgraph Kernel_Space ["Linux Kernel Space (Linux 6.6+)"]
            AHT10_DTS["aht10-overlay.dts\n(Device Tree)"] --> AHT10_DRV["aht10.ko (I2C Driver)\n- of_match_table\n- 定點整數除法 (div_u64)\n- Mutex 並行保護"]
            W25Q_DTS["w25q64-overlay.dts"] --> MTD_DRV["Linux MTD 子系統\n(/dev/mtd0 - 7MB Blackbox)"]
            HWMON["hwmon Sysfs (/sys/class/hwmon)\n- PWM 輸出 (pwm1)\n- TACH 回授 (fan1_input)"]
            CPU_TEMP["Thermal Zone (/sys/class/thermal)\nCPU 溫度感測"]
        end

        subgraph User_Space ["User Space (fan_daemon)"]
            DEV_AHT10["/dev/aht10"] -.-> DAEMON["fan_daemon\n(狀態機與動態溫控邏輯)"]
            CPU_TEMP -.-> DAEMON
            HWMON <--> DAEMON
            
            DAEMON <-->|MTD ioctl / MEMERASE / write| MTD_DRV
            DAEMON -->|UART 115200 bps\n同步指令 'S:pwm,temp'| UART_PORT["/dev/ttyUSB*"]
        end
    end

    subgraph MCU_Subsystem ["RP2040 Pico Subsystem (Real-Time Safety)"]
        UART_PORT ==>|UART TX/RX| PICO_MAIN["RP2040 Firmware (main.c)\n- UART 解析器\n- POST 自我檢測\n- 5s 通訊 Watchdog"]
        PICO_MAIN -->|硬體狀態機 PIO| PIO_WS2812["ws2812.pio (RP2040 PIO)\n8-LED 轉速狀態條"]
        PICO_MAIN -->|GPIO 控制| STATUS_LEDS["RGB 狀態指示燈 (紅/黃/綠)"]
        PICO_MAIN -->|GPIO 輸出| BUZZER["有源蜂鳴器 (告警聲響)"]
    end

    subgraph Cloud_Network ["雲端監控與物聯網 (MQTT Broker)"]
        DAEMON <==>|MQTT QoS 1 / TLS / JSON| BROKER[("MQTT Broker\n(EMQX / Mosquitto)")]
        BROKER <--> WEB_DASH["遠端監控中心 / Web Dashboard"]
    end

    classDef highlight fill:#2D3748,stroke:#4A5568,stroke-width:2px,color:#FFF;
    classDef kernel fill:#1A365D,stroke:#2B6CB0,stroke-width:2px,color:#FFF;
    classDef mcu fill:#744210,stroke:#D69E2E,stroke-width:2px,color:#FFF;
    class DAEMON,BROKER highlight;
    class AHT10_DRV,MTD_DRV kernel;
    class PICO_MAIN,PIO_WS2812 mcu;
```
</details>

---

### 2. 系統運作與資料流程圖 (Workflow)
系統涵蓋底層硬體感測、Kernel 驅動、Userspace 守護進程到遠端 MQTT Broker 的資料傳輸與控制流程：

![跨層工作流程圖](docs/images/workflow.png)

---

## 四、 核心工程亮點與技術實現

### 1. Linux Kernel I2C 驅動模組與設備樹
- **避免使用浮點運算**：在核心驅動（`driver/aht10.c`）中避免浮點數運算，改採 64 位元定點整數除法（`div_u64`）換算溫濕度數值，防止在 Kernel Space 產生浮點例外或污染 FPU 暫存器。
- **Device Tree 節點匹配**：撰寫 `dts/aht10-overlay.dts`，以 `compatible = "topsoft,aht10"` 觸發 I2C Probe，並自動註冊 `/dev/aht10` 字元裝置節點供使用者空間讀取。
- **並行保護與 Busy Bit 重試**：透過 `mutex_lock` 保護 I2C 傳輸流程，避免多行程同時存取裝置節點時發生衝突，並加入 Busy Bit 等待與重試判斷。

### 2. MTD 斷網黑盒子暫存與重傳機制 (Store-and-Forward)
當邊緣節點網路中斷時，系統會將遙測數據直接暫存至外部 SPI NOR Flash：
- **4KB 扇區對齊抹除與直接寫入**：直接操作 Raw MTD 裝置（`/dev/mtd0`），在跨越 4KB 邊界時呼叫 `ioctl(MEMERASE)` 執行扇區抹除，並以 `O_SYNC` 模式直寫晶片，降低突發斷電遺失資料的風險。
- **緊湊二進位格式 (Packed Struct)**：自定義 16-Byte 的 `blackbox_entry_t`（`__attribute__((packed))`），儲存時間戳記 (Unix Timestamp)、環境溫濕度與風扇轉速百分比，最大化利用 7MB Flash 空間。
- **網路重連自動批次補傳**：連線恢復後，Daemon 以每批 50 筆（`FLUSH_BATCH_SIZE=50`）將暫存封包補傳至 MQTT Broker，並標註 `"replayed": true`，確認回傳後自動抹除已處理的扇區。

### 3. 風扇轉速回授與防堵轉保護 (Stall Detection)
- **雙溫度來源動態調速**：綜合評估 CPU 核心溫度（Sysfs Thermal Zone）與 AHT10 機架進氣溫度，採 MAX Policy 取較高溫度所對應的 PWM 輸出。
- **風扇啟動緩衝期 (Spin-up Grace Period)**：考量無刷馬達由靜止啟動時的轉動慣量，設計 3 秒的 `spinup_grace` 緩衝時間，避免風扇剛啟動因轉速尚未拉起（TACH=0）而產生誤報。
- **風扇堵轉檢測 (Stall Detection)**：當 PWM 驅動輸出但實體轉速連續 2 秒為 0 RPM 時（已過啟動緩衝期），判定為風扇軸承堵轉（`FAN_STALL`），立即上報 MQTT 並通知 RP2040 觸發蜂鳴器告警。

### 4. RP2040 PIO 時序控制與通訊逾時保護
- **PIO 硬體狀態機驅動 LED**：WS2812B RGB LED 需要微秒級精準時序（800kHz）。透過 RP2040 的 Programmable I/O（`ws2812.pio`），由專用硬體狀態機輸出訊號，不佔用 CPU 運算資源，亦不受中斷延遲干擾。
- **通訊逾時安全保護**：若 Linux 主機當機或 UART 連線中斷超過 5 秒，RP2040 韌體自動進入逾時保護狀態，熄滅指示燈並關閉蜂鳴器，防止警報失控長鳴。
- **開機自我檢測 (Power-On Self-Test, POST)**：通電時依序點亮三色 LED、掃描燈條跑馬燈並觸發短音蜂鳴，確認周邊硬體正常後進入全滅待機狀態。

---

## 五、 硬體清單與接線對照表 (Pinout)

### 1. 主要元件清單 (BOM)
1. **主控制器**：Raspberry Pi 5 (4GB / 8GB)
2. **協同微控制器**：Raspberry Pi Pico (RP2040)
3. **環境溫濕度感測器**：AHT10 (I2C 介面)
4. **SPI NOR Flash**：Winbond W25Q64FV (8MB / 64M-bit)
5. **可定址 RGB 燈條**：WS2812B 8-Pixel LED Strip
6. **狀態指示燈與蜂鳴器**：紅/黃/綠 5mm LED 各一、有源蜂鳴器模組
7. **散熱風扇**：支援 PWM 控制與 TACH 轉速回授之 4-Wire 散熱風扇

---

### 2. 接線配置表

#### A. Raspberry Pi 5 腳位連接
| 周邊裝置 | 裝置腳位 | 樹莓派 5 實體 Pin | 訊號定義 / 備註 |
| :--- | :--- | :--- | :--- |
| **AHT10** | VCC / GND | Pin 1 (3.3V) / Pin 9 (GND) | 供電 |
| **AHT10** | SDA / SCL | Pin 3 (GPIO2) / Pin 5 (GPIO3) | I2C1 匯流排 |
| **W25Q64** | VCC / GND | Pin 17 (3.3V) / Pin 25 (GND) | 供電 |
| **W25Q64** | CS / CLK | Pin 24 (GPIO8) / Pin 23 (GPIO11) | SPI0 CE0 / SCLK |
| **W25Q64** | MOSI / MISO | Pin 19 (GPIO10) / Pin 21 (GPIO9) | SPI0 MOSI / MISO |
| **4-Wire Fan** | PWM / TACH | 原廠專用風扇插座 或 GPIO | 透過 `hwmon` 節點驅動 |
| **USB-UART** | TX / RX | 連接至 Pico 的 RX / TX | 115200-8N1 |

#### B. Raspberry Pi Pico (RP2040) 腳位連接
| 周邊裝置 | Pico GPIO | Pico 實體 Pin | 功能說明 |
| :--- | :--- | :--- | :--- |
| **UART0 RX** | GPIO 1 | Pin 2 | 接收樹莓派控制指令 |
| **UART0 TX** | GPIO 0 | Pin 1 | 回傳狀態（預留） |
| **WS2812B DIN**| GPIO 22 | Pin 29 | PIO0 驅動 8 顆 RGB LED 轉速條 |
| **綠色 LED** | GPIO 15 | Pin 20 | 正常工作 / 低溫指示燈 |
| **黃色 LED** | GPIO 14 | Pin 19 | 中溫警戒指示燈 |
| **紅色 LED** | GPIO 13 | Pin 17 | 高溫 / 警報指示燈 |
| **有源蜂鳴器** | GPIO 20 | Pin 26 | 故障 / 堵轉警報聲響 |

---

## 六、 專案目錄結構

```text
├── .gitignore                    # Git 忽略中間編譯產物
├── LICENSE                       # GNU GPL v2.0 授權條款
├── README.md                     # 專案詳細架構與操作文件
├── docs/                         # 專案文件、架構圖表與展示簡報
│   ├── 專題簡報-機櫃溫度環控模組.pdf # 完整專案架構、Kernel優化與儀器驗證簡報
│   └── images/                   # 高解析度架構圖與工作流程圖
│       ├── system_architecture.png
│       └── workflow.png
├── dts/                          # 設備樹原始碼與編譯檔
│   ├── aht10-overlay.dts         # AHT10 I2C 設備樹覆蓋層
│   ├── w25q64-overlay.dts        # W25Q64 SPI-NOR MTD 設備樹覆蓋層
│   └── Makefile                  # dtc 自動化編譯與安裝腳本
├── driver/                       # Linux 核心驅動模組
│   ├── aht10.c                   # AHT10 字元設備核心驅動
│   └── Makefile                  # Kbuild 核心模組編譯腳本
├── daemon/                       # 使用者空間溫控與黑盒子服務
│   ├── fan_daemon.c              # 主守護行程 (MQTT / MTD / UART / Thermal)
│   ├── mqtt_config.txt.example   # MQTT Broker 連線設定範本
│   └── Makefile                  # gcc 編譯腳本 (鏈結 mosquitto, cjson, m)
└── firmware_pico/                # RP2040 即時控制與告警韌體
    ├── CMakeLists.txt            # Pico C/C++ SDK CMake 建置檔
    ├── pico_sdk_import.cmake     # Pico SDK 導引檔
    ├── main.c                    # Pico 主控制邏輯與 Watchdog
    └── ws2812.pio                # RP2040 PIO 狀態機組合語言原始碼
```

---

## 七、 編譯與部署指南

### 1. Device Tree Overlay 編譯與掛載
在 Raspberry Pi 5 終端機執行：
```bash
cd dts
make
sudo make install
```
接著編輯 `/boot/firmware/config.txt`，在檔尾加入：
```ini
dtoverlay=aht10-overlay
dtoverlay=w25q64-overlay
```
重新開機使設備樹生效：
```bash
sudo reboot
```
開機後檢查節點是否建立：
```bash
ls -l /dev/aht10 /dev/mtd*
```

---

### 2. Linux 核心驅動編譯與載入
確保已安裝樹莓派核心標頭檔：
```bash
sudo apt update
sudo apt install -y raspberrypi-kernel-headers build-essential
```
編譯驅動並載入核心：
```bash
cd driver
make
sudo insmod aht10.ko
```
確認核心模組運作狀況：
```bash
dmesg | grep aht10
```

---

### 3. 使用者空間守護行程 (Daemon) 編譯與啟動
安裝相依通訊與 JSON 解析函式庫：
```bash
sudo apt install -y libmosquitto-dev libcjson-dev
```
設定 MQTT Broker 位址並編譯：
```bash
cd daemon
cp mqtt_config.txt.example mqtt_config.txt
# 編輯 mqtt_config.txt 輸入你的 MQTT Broker IP
nano mqtt_config.txt

make
sudo ./fan_daemon
```

#### (選配) 註冊為 Systemd 系統服務常駐執行
建立服務檔 `/etc/systemd/system/fan_daemon.service`：
```ini
[Unit]
Description=Edge Thermal Management and Blackbox Daemon
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/home/pi/daemon
ExecStart=/usr/local/bin/fan_daemon
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```
啟用服務：
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now fan_daemon
```

---

### 4. RP2040 Pico 韌體編譯與燒錄
需先在開發電腦上配置好 [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)。
```bash
cd firmware_pico
mkdir build && cd build
cmake ..
make -j4
```
將生成的 `thermal_pico_firmware.uf2` 拖曳至處於 BOOTSEL 模式的 Raspberry Pi Pico 即完成燒錄。

---

## 八、 MQTT 通訊協定與 JSON Payload 規範

系統支援遠端指令控制與即時狀態監控 (Publish / Subscribe)：

### 1. 連線心跳與在線狀態 (`rack/thermal/report-in`)
- **QoS**: 1 (Retained: `true`)
```json
{
  "online": true
}
```

### 2. 即時遙測數據 (`rack/thermal/telemetry`)
- **QoS**: 0 (即時遙測)
- **歷史補傳標籤**：若該筆資料來自 Flash 黑盒子暫存，將附加 `"replayed": true`。
```json
{
  "timestamp": 1726410500,
  "temp": 28.4,
  "hum": 62.5,
  "fan_speed_pct": 60,
  "fan_rpm": 2700,
  "replayed": false
}
```

### 3. 系統狀態與告警通報 (`rack/thermal/state`)
- **QoS**: 1 (Retained: `true`)
- **告警代碼**：`NONE`（正常）、`FAN_STALL`（風扇堵轉）、`SENSOR_ERROR`（感測器異常）、`HIGH_TEMP`（超溫警報）。
```json
{
  "timestamp": 1726410500,
  "mode": "AUTO",
  "alarm": true,
  "alarm_code": "FAN_STALL",
  "temp_threshold": 40.0
}
```

### 4. 下行控制指令 (`rack/thermal/cmd`)
- **控制模式切換與手動轉速設定**：
```json
{
  "mode": "MANUAL",
  "fan_speed_pct": 80
}
```
- **警報觸發門檻值調整**：
```json
{
  "temp_threshold": 45.0
}
```

---

## 九、 授權條款 (License)

本專案之 Linux 核心模組與整體原始碼採用 [GNU General Public License v2.0 (GPL-2.0)](LICENSE) 授權發布。
