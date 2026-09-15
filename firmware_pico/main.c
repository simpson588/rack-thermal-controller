#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "ws2812.pio.h"

// ===== 實體腳位配置 =====
#define WS2812_PIN      22      // WS2812B DIN (Pin 29)
#define NUM_PIXELS      8       // 8 顆 LED 燈條
#define PIN_20_GREEN    15      // 綠色 LED (Pin 20)
#define PIN_19_YELLOW   14      // 黃色 LED (Pin 19)
#define PIN_17_RED      13      // 紅色 LED (Pin 17)
#define BUZZER_PIN      20      // 有源蜂鳴器 (Pin 26 / GPIO 20)

// ===== UART 配置 =====
#define UART_ID         uart0
#define BAUD_RATE       115200
#define UART_TX_PIN     0       // Pin 1
#define UART_RX_PIN     1       // Pin 2

// ===== 斷線 Watchdog 設定 =====
#define UART_TIMEOUT_US  5000000   // 5 秒無任何指令視為斷線

static int is_alarm_mode = 0;

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(g) << 16) | ((uint32_t)(r) << 8) | (uint32_t)(b);
}

// 將 8 顆燈條統一設定為指定顏色 (全 0 即為全滅)
void set_strip_solid(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < NUM_PIXELS; i++) {
        put_pixel(urgb_u32(r, g, b));
    }
    sleep_us(60);
}

// 控制傳統 LED 與蜂鳴器
void set_io_state(int g, int y, int r, int buzzer) {
    gpio_put(PIN_20_GREEN, g);
    gpio_put(PIN_19_YELLOW, y);
    gpio_put(PIN_17_RED, r);
    gpio_put(BUZZER_PIN, buzzer);
}

// 🌟 根據轉速 PWM 更新 WS2812B
// 運行中 PWM == 0 時維持亮 1 顆白燈待機；PWM > 0 依段數點亮
void update_fan_strip(int pwm) {
    if (pwm < 0) pwm = 0;
    if (pwm > 255) pwm = 255;

    int lit_count = 1; // 預設 PWM 0 亮第 1 顆白燈待機

    if (pwm >= 255)      lit_count = 8; // PWM 255: 8 顆全亮
    else if (pwm >= 200) lit_count = 6; // PWM 200: 前 6 顆
    else if (pwm >= 150) lit_count = 4; // PWM 150: 前 4 顆
    else if (pwm >= 100) lit_count = 2; // PWM 100: 前 2 顆
    else                 lit_count = 1; // PWM 0 (停轉): 亮第 1 顆白燈待機

    for (int i = 0; i < NUM_PIXELS; i++) {
        if (i < lit_count) {
            if (i < 2) {
                put_pixel(urgb_u32(30, 30, 30)); // 1~2 顆：白燈
            } else if (i < 4) {
                put_pixel(urgb_u32(0, 35, 0));   // 3~4 顆：綠燈
            } else if (i < 6) {
                put_pixel(urgb_u32(35, 25, 0));  // 5~6 顆：黃燈
            } else {
                put_pixel(urgb_u32(45, 0, 0));   // 7~8 顆：紅燈
            }
        } else {
            put_pixel(urgb_u32(0, 0, 0));        // 滅燈
        }
    }
    sleep_us(60);
}

// 根據環境溫度更新三色 LED
void update_temp_leds(float temp) {
    if (temp < -40.0f || temp > 125.0f) {
        temp = 0.0f; // 異常值防呆
    }

    if (temp < 26.0f) {
        set_io_state(1, 0, 0, 0); // 低溫：綠燈
    } else if (temp < 37.0f) {
        set_io_state(1, 1, 0, 0); // 中溫：綠 + 黃燈
    } else {
        set_io_state(1, 1, 1, 0); // 高溫：綠 + 黃 + 紅燈
    }
}

// 開機通電自我檢測 (POST)：閃爍結束後「全滅靜態待機」，等待上位機啟動
void power_on_self_test(void) {
    set_io_state(1, 0, 0, 0); sleep_ms(400);
    set_io_state(0, 1, 0, 0); sleep_ms(400);
    set_io_state(0, 0, 1, 0); sleep_ms(400);
    set_io_state(0, 0, 0, 0); sleep_ms(100);

    uint32_t colors[8] = {
        urgb_u32(30, 30, 30), urgb_u32(30, 30, 30), // 1~2 顆：白燈
        urgb_u32(0, 35, 0),   urgb_u32(0, 35, 0),   // 3~4 顆：綠燈
        urgb_u32(35, 25, 0),  urgb_u32(35, 25, 0),  // 5~6 顆：黃燈
        urgb_u32(45, 0, 0),   urgb_u32(45, 0, 0)    // 7~8 顆：紅燈
    };

    for (int target = 0; target < NUM_PIXELS; target++) {
        for (int i = 0; i < NUM_PIXELS; i++) {
            put_pixel((i == target) ? colors[i] : urgb_u32(0, 0, 0));
        }
        sleep_us(60);
        sleep_ms(200);
    }

    gpio_put(BUZZER_PIN, 1);
    sleep_ms(50);
    gpio_put(BUZZER_PIN, 0);

    // POST 結束：維持全滅待機，直到 fan_daemon 送出指令才接管點亮
    set_io_state(0, 0, 0, 0);
    set_strip_solid(0, 0, 0);
}

int main(void) {
    stdio_init_all();

    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ID, true);

    gpio_init(PIN_20_GREEN);  gpio_set_dir(PIN_20_GREEN, GPIO_OUT);
    gpio_init(PIN_19_YELLOW); gpio_set_dir(PIN_19_YELLOW, GPIO_OUT);
    gpio_init(PIN_17_RED);    gpio_set_dir(PIN_17_RED, GPIO_OUT);
    gpio_init(BUZZER_PIN);    gpio_set_dir(BUZZER_PIN, GPIO_OUT);

    gpio_put(PIN_20_GREEN, 0);
    gpio_put(PIN_19_YELLOW, 0);
    gpio_put(PIN_17_RED, 0);
    gpio_put(BUZZER_PIN, 0);

    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, WS2812_PIN, 800000, false);

    power_on_self_test();

    char rx_buf[64];
    int buf_pos = 0;
    int line_overflow = 0;
    absolute_time_t last_rx_time = get_absolute_time();

    while (1) {
        while (uart_is_readable(UART_ID)) {
            char ch = uart_getc(UART_ID);

            if (ch == '\n' || ch == '\r') {
                if (line_overflow) {
                    line_overflow = 0;
                    buf_pos = 0;
                    last_rx_time = get_absolute_time();
                } else if (buf_pos > 0) {
                    rx_buf[buf_pos] = '\0';
                    last_rx_time = get_absolute_time();

                    // 1. 🌟 關機指令 (OFF 或 0)：所有 LED、蜂鳴器、燈條 100% 完全熄滅
                    if (strncmp(rx_buf, "OFF", 3) == 0 || strncmp(rx_buf, "0", 1) == 0) {
                        is_alarm_mode = 0;
                        set_io_state(0, 0, 0, 0);
                        set_strip_solid(0, 0, 0);
                    }
                    // 2. 警報開啟（三燈全亮 + 蜂鳴器 + 燈條 8 顆全紅）
                    else if (strncmp(rx_buf, "A:1", 3) == 0) {
                        is_alarm_mode = 1;
                        set_io_state(1, 1, 1, 1);
                        set_strip_solid(50, 0, 0);
                    }
                    // 3. 警報解除：關閉蜂鳴器，燈條暫時清空等待下一筆 S: 指令刷新
                    else if (strncmp(rx_buf, "A:0", 3) == 0) {
                        is_alarm_mode = 0;
                        gpio_put(BUZZER_PIN, 0);
                        set_io_state(1, 0, 0, 0);
                        set_strip_solid(0, 0, 0);
                    }
                    // 4. 🌟 主迴圈同步指令 "S:<pwm>,<temp>"
                    else if (!is_alarm_mode && strncmp(rx_buf, "S:", 2) == 0) {
                        char *comma = strchr(rx_buf, ',');
                        if (comma) {
                            *comma = '\0';
                            int pwm = atoi(rx_buf + 2);
                            float temp = (float)atof(comma + 1);

                            update_fan_strip(pwm);   // PWM 0 會亮 1 顆白燈待機
                            update_temp_leds(temp);
                        }
                    }
                    // 5. 向下相容個別指令
                    else if (!is_alarm_mode) {
                        if (strncmp(rx_buf, "P:", 2) == 0) {
                            update_fan_strip(atoi(rx_buf + 2));
                        } else if (strncmp(rx_buf, "T:", 2) == 0) {
                            update_temp_leds((float)atof(rx_buf + 2));
                        }
                    }
                    buf_pos = 0;
                } else {
                    last_rx_time = get_absolute_time();
                }
            } else if (ch >= 32 && ch <= 126) {
                if (!line_overflow) {
                    if (buf_pos < (int)sizeof(rx_buf) - 1) {
                        rx_buf[buf_pos++] = ch;
                    } else {
                        line_overflow = 1;
                        buf_pos = 0;
                    }
                }
            }
        }

        // 斷線 Watchdog 保護：超過 5 秒未收到指令，自動強制全滅保護
        if (is_alarm_mode && absolute_time_diff_us(last_rx_time, get_absolute_time()) > UART_TIMEOUT_US) {
            is_alarm_mode = 0;
            gpio_put(BUZZER_PIN, 0);
            set_io_state(0, 0, 0, 0);
            set_strip_solid(0, 0, 0);
        }

        sleep_ms(5);
    }
    return 0;
}