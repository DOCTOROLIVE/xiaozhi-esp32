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
    .display_cs_pin = GPIO_NUM_12,

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

// 鸭头触摸电极：GPIO13 → TOUCH_PAD_NUM12（T12）
// 注意：TOUCH_PAD_NUM0（GPIO1）是 ESP32-S3 内部去噪通道，禁止作为触摸传感使用
// 无摄像头版 config 中 GPIO13 无任何占用，且不是 strapping pin
#define TOUCH_PAD_HEAD_GPIO      GPIO_NUM_13
// 阈值选择：基线 = 启动 idle 均值（约 51200）。
// - idle delta 在 ±50 噪声内（基线不动 + raw 围绕 idle 均值波动）
// - 触摸曲线：idle +119 → +119 → +119 → +150 → 释放
//   （注意：触摸峰值只 1 帧 =150；阈值必须 ≤ 触摸曲线第二帧 delta=119
//    才能让 over_cnt=2 帧连续累计）
// - 阈值 115：触摸 119、150 两帧连续 > 115 → 触发
//              idle 偶发 delta=+60~+80 < 115，余量 35+
// （v5=110 时偶发误触，上调到 115 略收紧）
#define TOUCH_PAD_HEAD_THRESHOLD 115
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
