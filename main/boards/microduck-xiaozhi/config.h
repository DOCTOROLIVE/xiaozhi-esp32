#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <esp_adc/adc_oneshot.h>
#include <driver/gpio.h>

#define OTTO_VERSION_AUTO 0
#define OTTO_VERSION_CAMERA 1
#define OTTO_VERSION_NO_CAMERA 2

#ifndef OTTO_HARDWARE_VERSION
#define OTTO_HARDWARE_VERSION OTTO_VERSION_AUTO
#endif

enum OttoCameraType {
    OTTO_CAMERA_NONE = 0,
    OTTO_CAMERA_OV2640 = 1,
    OTTO_CAMERA_OV3660 = 2,
    OTTO_CAMERA_UNKNOWN = 99,
};

#define OTTO_OV2640_PID_1 0x2640
#define OTTO_OV2640_PID_2 0x2626
#define OTTO_OV3660_PID 0x3660

struct HardwareConfig {
    gpio_num_t power_charge_detect_pin;
    adc_unit_t power_adc_unit;
    adc_channel_t power_adc_channel;

    gpio_num_t right_leg_pin;
    gpio_num_t right_foot_pin;
    gpio_num_t left_leg_pin;
    gpio_num_t left_foot_pin;
    gpio_num_t left_hand_pin;
    gpio_num_t right_hand_pin;

    int audio_input_sample_rate;
    int audio_output_sample_rate;
    bool audio_use_simplex;

    gpio_num_t audio_i2s_gpio_ws;
    gpio_num_t audio_i2s_gpio_bclk;
    gpio_num_t audio_i2s_gpio_din;
    gpio_num_t audio_i2s_gpio_dout;

    gpio_num_t audio_i2s_mic_gpio_ws;
    gpio_num_t audio_i2s_mic_gpio_sck;
    gpio_num_t audio_i2s_mic_gpio_din;
    gpio_num_t audio_i2s_spk_gpio_dout;
    gpio_num_t audio_i2s_spk_gpio_bclk;
    gpio_num_t audio_i2s_spk_gpio_lrck;

    gpio_num_t display_backlight_pin;
    gpio_num_t display_mosi_pin;
    gpio_num_t display_clk_pin;
    gpio_num_t display_dc_pin;
    gpio_num_t display_rst_pin;
    gpio_num_t display_cs_pin;

    gpio_num_t i2c_sda_pin;
    gpio_num_t i2c_scl_pin;
};

constexpr HardwareConfig CAMERA_VERSION_CONFIG = {
    .power_charge_detect_pin = GPIO_NUM_NC,
    .power_adc_unit = ADC_UNIT_1,
    .power_adc_channel = ADC_CHANNEL_1,

    .right_leg_pin = GPIO_NUM_43,
    .right_foot_pin = GPIO_NUM_44,
    .left_leg_pin = GPIO_NUM_5,
    .left_foot_pin = GPIO_NUM_6,
    .left_hand_pin = GPIO_NUM_4,
    .right_hand_pin = GPIO_NUM_7,

    .audio_input_sample_rate = 16000,
    .audio_output_sample_rate = 16000,
    .audio_use_simplex = false,

    .audio_i2s_gpio_ws = GPIO_NUM_40,
    .audio_i2s_gpio_bclk = GPIO_NUM_42,
    .audio_i2s_gpio_din = GPIO_NUM_41,
    .audio_i2s_gpio_dout = GPIO_NUM_39,

    .audio_i2s_mic_gpio_ws = GPIO_NUM_NC,
    .audio_i2s_mic_gpio_sck = GPIO_NUM_NC,
    .audio_i2s_mic_gpio_din = GPIO_NUM_NC,
    .audio_i2s_spk_gpio_dout = GPIO_NUM_NC,
    .audio_i2s_spk_gpio_bclk = GPIO_NUM_NC,
    .audio_i2s_spk_gpio_lrck = GPIO_NUM_NC,

    .display_backlight_pin = GPIO_NUM_38,
    .display_mosi_pin = GPIO_NUM_45,
    .display_clk_pin = GPIO_NUM_48,
    .display_dc_pin = GPIO_NUM_47,
    .display_rst_pin = GPIO_NUM_1,
    .display_cs_pin = GPIO_NUM_NC,

    .i2c_sda_pin = GPIO_NUM_15,
    .i2c_scl_pin = GPIO_NUM_16,
};

constexpr HardwareConfig NON_CAMERA_VERSION_CONFIG = {
    .power_charge_detect_pin = GPIO_NUM_21,
    .power_adc_unit = ADC_UNIT_2,
    .power_adc_channel = ADC_CHANNEL_3,

    .right_leg_pin = GPIO_NUM_39,
    .right_foot_pin = GPIO_NUM_38,
    .left_leg_pin = GPIO_NUM_17,
    .left_foot_pin = GPIO_NUM_18,
    .left_hand_pin = GPIO_NUM_8,
    .right_hand_pin = GPIO_NUM_12,

    .audio_input_sample_rate = 16000,
    .audio_output_sample_rate = 24000,
    .audio_use_simplex = true,

    .audio_i2s_gpio_ws = GPIO_NUM_NC,
    .audio_i2s_gpio_bclk = GPIO_NUM_NC,
    .audio_i2s_gpio_din = GPIO_NUM_NC,
    .audio_i2s_gpio_dout = GPIO_NUM_NC,

    .audio_i2s_mic_gpio_ws = GPIO_NUM_4,
    .audio_i2s_mic_gpio_sck = GPIO_NUM_5,
    .audio_i2s_mic_gpio_din = GPIO_NUM_6,
    .audio_i2s_spk_gpio_dout = GPIO_NUM_7,
    .audio_i2s_spk_gpio_bclk = GPIO_NUM_15,
    .audio_i2s_spk_gpio_lrck = GPIO_NUM_16,

    .display_backlight_pin = GPIO_NUM_3,
    .display_mosi_pin = GPIO_NUM_10,
    .display_clk_pin = GPIO_NUM_9,
    .display_dc_pin = GPIO_NUM_46,
    .display_rst_pin = GPIO_NUM_11,
    .display_cs_pin = GPIO_NUM_NC,  // 实际 PCB 上 CS 直接接 GND；非 NC 会与 right_hand_pin (GPIO12) 冲突导致鸭嘴抖动

    .i2c_sda_pin = GPIO_NUM_NC,
    .i2c_scl_pin = GPIO_NUM_NC,
};

#define CAMERA_XCLK (GPIO_NUM_3)
#define CAMERA_PCLK (GPIO_NUM_10)
#define CAMERA_VSYNC (GPIO_NUM_17)
#define CAMERA_HSYNC (GPIO_NUM_18)
#define CAMERA_D0 (GPIO_NUM_12)
#define CAMERA_D1 (GPIO_NUM_14)
#define CAMERA_D2 (GPIO_NUM_21)

// 鸭头触摸电极：GPIO2 → TOUCH_PAD_NUM2（T2）
// 硬件：薄铜片电极，2cm × 1.45cm。
// 迁移原因：原 GPIO13（TOUCH_PAD_NUM12）紧邻嘴舵机 PWM GPIO12，
//   触摸扫描的充放电噪声会耦合进 PWM 信号线导致嘴舵机抖动。
//   GPIO2 与所有舵机引脚（GPIO8/12/17/18/38/39）相距足够远，隔离性好。
// 无摄像头版 config 中 GPIO2 无任何占用，且不是 strapping pin，
//   可安全用作外部触摸电极。
// 注意：TOUCH_PAD_NUM0（GPIO1）是 ESP32-S3 内部去噪通道，禁止使用。
#define TOUCH_PAD_HEAD_GPIO      GPIO_NUM_2
// 阈值选择：
// - 电极 2x1.45cm（~2.9cm²），实测无摄像头版基线 ~66336，
//   idle delta 在 +130~±150% / -300~-500 噪声区间漂移。
// - 真实手指接触铜片实测 delta≈+7220（远距离手指靠近 +50~+150）。
// - 固定下限 5000 + 自适应公式 max(5000, baseline/300)：
//   · baseline=66336 时阈值 = max(5000, 221) = 5000；
//     idle delta ±180~+150 留 100x 余量；real touch delta +7220 > 5000 触发。
//   · 远距离手指靠近 delta +50~+150 < 5000，不会误触。
//   · 小基线场景（baseline=2500）→ 阈值 5000，仍能正常识别真实触摸。
// 历史参考：
//   GPIO13 + 大电极 → 基线 ~51200 → 阈值 115 → 触摸 +280 触发
//   GPIO2  + 小电极 → 基线 ~66336 → 阈值 5000 → 触摸 +7220 触发
#define TOUCH_PAD_HEAD_THRESHOLD 5000
#define TOUCH_PAD_HEAD_DEBOUNCE_MS 3000 // 两次触摸间的最小间隔，避免抖动
// 长基线跟踪：每帧 baseline 向 raw 移动 1/1024，约 50s 时间常数（τ）。
// 慢基线跟踪：每帧移动 1/4096，约 200s 时间常数，仅在 idle 怀疑有漂移时启用。
#define TOUCH_BASELINE_FAST_RATIO 1023
#define TOUCH_BASELINE_SLOW_RATIO 4095
#define CAMERA_D3 (GPIO_NUM_13)
#define CAMERA_D4 (GPIO_NUM_11)
#define CAMERA_D5 (GPIO_NUM_9)
#define CAMERA_D6 (GPIO_NUM_46)
#define CAMERA_D7 (GPIO_NUM_8)
#define CAMERA_PWDN (GPIO_NUM_NC)
#define CAMERA_RESET (GPIO_NUM_NC)
#define CAMERA_XCLK_FREQ (16000000)
#define LEDC_TIMER (LEDC_TIMER_0)
#define LEDC_CHANNEL (LEDC_CHANNEL_0)

#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR true
#define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 3

#define BOOT_BUTTON_GPIO GPIO_NUM_0

#endif
