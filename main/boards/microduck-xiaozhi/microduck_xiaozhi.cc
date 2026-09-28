#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

#include <new>

#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp32_camera.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "otto_emoji_display.h"
#include "power_manager.h"
#include "system_reset.h"
#include "websocket_control_server.h"
#include "wifi_board.h"

#define TAG "MicroduckXiaozhi"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// ESP-IDF v6.x 新触摸驱动（handle-based），替代已废弃的 legacy touch_pad.h
#include "driver/touch_sens.h"

extern void InitializeOttoController(const HardwareConfig& hw_config);
extern void OttoQueueAction(int action_type, int steps, int speed,
                            int direction, int amount);

// 触摸任务：GPIO13（TOUCH_PAD_NUM12）检测到触摸 → 投递 ACTION_DUCK_SHAKE_HEAD
//
// 关键点：
// 1. 启动后必须等待 3 秒再测基线：舵机/WiFi/音频等外设上电后会改变触摸传感器的
//    电气环境，启动期的读数不能代表稳态基线。
// 2. 无条件滚动基线：始终缓慢跟踪当前 SMOOTH 值以吸收温漂/上电漂移；
//    超过阈值时改用更慢的比率，既不影响一次 1~2 秒的真实触摸，又能吸收持续性漂移。
// 3. 边沿触发：只有"回落到阈值以下重新武装"后才允许再次触发，
//    避免环境长期偏移导致每 3 秒重复触发。
// 4. 3 帧连续确认 + 3 秒冷却窗口，防止单点噪声误触发。
static void TouchHeadShakeTask(void* arg) {
    // ---- 新驱动（touch_sens.h, ESP32-S3 = Touch HW V2）初始化 ----
    // 采样配置：charge_times 决定读数大小（数据与 charge_times 正相关），
    // 电压摆幅取最大 2V7→0V5 以提高小电极灵敏度
    touch_sensor_sample_config_t sample_cfg = TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(
        1000, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V7);
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample_cfg);

    touch_sensor_handle_t sens_handle = nullptr;
    esp_err_t err = touch_sensor_new_controller(&sens_cfg, &sens_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸控制器创建失败: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    // 通道 12 → GPIO13（TOUCH_PAD_NUM12）
    // active_thresh 不用硬件中断判活（我们自己轮询 raw + 软件基线），置 0
    touch_channel_config_t chan_cfg = {
        .active_thresh = {0},
        .charge_speed = TOUCH_CHARGE_SPEED_7,       // 最快充放电，分辨率最高
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    touch_channel_handle_t chan_handle = nullptr;
    err = touch_sensor_new_channel(sens_handle, 12, &chan_cfg, &chan_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸通道 12 创建失败: %s", esp_err_to_name(err));
        touch_sensor_del_controller(sens_handle);
        vTaskDelete(nullptr);
        return;
    }

    err = touch_sensor_enable(sens_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸控制器使能失败: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    // 启动 FSM 连续扫描（等价于 legacy 的 fsm_start）
    err = touch_sensor_start_continuous_scanning(sens_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸连续扫描启动失败: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    // ★ 关键：等外设稳定后再测基线
    // 舵机 PWM、WiFi、音频等上电后会改变触摸传感器的电气环境，
    // 若立即测基线，会捕获到非稳态的瞬态值（历史上曾测到 ~1.86M 或 ~155K），
    // 导致后续 delta 一直很大，3 秒一次误触发摇头。
    const int kSettleMs = 3000;
    ESP_LOGI(TAG, "触摸传感器启动，等待外设稳定 %d ms...", kSettleMs);
    vTaskDelay(pdMS_TO_TICKS(kSettleMs));

    // 取稳态基线（32 样本均值，约 0.7 秒），与主循环一致使用 FILTER 值
    const int kBaselineSamples = 32;
    uint32_t sum = 0;
    uint32_t raw = 0;
    for (int i = 0; i < kBaselineSamples; i++) {
        touch_channel_read_data(chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &raw);
        sum += raw;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uint32_t baseline = sum / kBaselineSamples;

    // ★ 基线主存储改用浮点：消除整数 EWMA 的精度截断问题。
    // 1023:1 整数跟踪在 baseline≈51000 量级下，要 baseline 移动 1 单位需要
    // raw 高出 ≈1024 单位(2%)；当前 raw 仅偏离基线 0.25%，整数除法每帧
    // 截断为 baseline 不动 → idle delta 长期偏 +130~+200 不收敛。
    // 浮点 EWMA 每帧移动 0.1% × |raw-baseline|，τ≈50s 即可收敛到 raw 均值。
    double baseline_f = (double)baseline;

    // 合理性检查：基线应在 [100, 200000] 区间
    // 电极面积适中时一般 ~2000~50000
    if (baseline < 100 || baseline > 200000) {
        ESP_LOGE(TAG, "触摸基线异常 raw=%u，触摸检测禁用，请检查硬件/电极面积",
                 baseline);
        vTaskDelete(nullptr);
        return;
    }

    // 阈值：稳态漂移由滚动基线吸收后，空闲 delta 应趋近 0；
    // idle 实测峰值 ~220，触摸信号峰值 ~282(单帧)，故阈值取 240。
    int threshold = TOUCH_PAD_HEAD_THRESHOLD;
    ESP_LOGI(TAG, "触摸基线 raw=%u 阈值=%d", baseline, threshold);

    TickType_t last_trigger = 0;
    int debug_count = 0;      // 调试：每 20×50ms = 1s 打印一次 raw/delta/baseline
    int baseline_log = 0;     // 调试：每 200×50ms = 10s 打印一次 baseline 变化确认跟踪生效
    int over_cnt = 0;         // 连续超阈值帧计数，避免单点噪声触发
    bool armed = true;        // 边沿锁存：触发后置 false，仅 cooldown 结束后置 true
    while (true) {
        // SMOOTH 值：驱动内置 IIR 滤波，比 RAW 平滑
        touch_channel_read_data(chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &raw);
        const int delta = (int)raw - (int)baseline;
        const int adelta = delta < 0 ? -delta : delta;
        if (++debug_count >= 20) {
            ESP_LOGI(TAG, "raw=%u delta=%d baseline=%u", raw, delta, baseline);
            debug_count = 0;
        }
        if (++baseline_log >= 200) {
            // baseline 每 10s 必打一行：若发现 baseline 长时间纹丝不动，说明
            // 浮点跟踪逻辑没生效或被条件分支跳过。
            ESP_LOGI(TAG, "baseline=%u baseline_f=%.1f", baseline, baseline_f);
            baseline_log = 0;
        }

        TickType_t now = xTaskGetTickCount();
        const bool in_cooldown =
            (now - last_trigger) < pdMS_TO_TICKS(TOUCH_PAD_HEAD_DEBOUNCE_MS);

        if (adelta > threshold) {
            // ★ 边沿触发 + 短确认：阈值 240 时触摸峰值仅持续 2~3 帧，
            // 5 帧确认来不及累计。改为 2 帧确认（约 100ms）。
            if (armed && !in_cooldown) {
                if (++over_cnt >= 2) {  // 连续 2 帧确认（约 100ms）
                    ESP_LOGI(TAG, "触摸检测 delta=%d 触发摇头", delta);
                    // 52 = ACTION_DUCK_SHAKE_HEAD，4 步振荡、1.0s 周期、0 居中、30° 幅度
                    OttoQueueAction(52, 4, 1000, 0, 30);
                    last_trigger = now;
                    armed = false;
                    over_cnt = 0;
                }
            }
        } else {
            over_cnt = 0;
            // 关键：cooldown 期间即使 delta 回落到阈值以内，也保持 armed=false，
            // 避免一次触摸动作执行期间被反复触发。仅 cooldown 结束后才重新武装。
            if (!in_cooldown) {
                armed = true;
            }
        }

        // ★ 基线跟踪（**只向下**浮点 EWMA，保留触摸信号幅度）：
// 历史 bug（v3）：1023:1 双向跟踪最终让基线收敛到 idle raw 均值（~51200），
// 触摸峰值 raw=51350 → 真实 delta 缩水到 ~150，远低于阈值 → 永不触发。
// 现在基线**只允许向下跟踪**（raw < baseline 时慢速吸收漂移），
// **不允许向上收敛**（raw > baseline 时基线不动）。
// 这样：
// - idle 时 raw > baseline → 基线不动，idle delta 长期保持初始偏置 ~+130
// - 触摸时 raw 跳到峰值 → delta 达 ~280，远高于阈值 150
// - 触摸释放后 raw 回到 idle 均值，基线仍不动
// - 温度漂移让 raw 下降时，基线慢速跟随（4095:1, τ≈205s）
// 阈值 150 + over_cnt 2 帧：idle +130 < 150 安全，触摸 +280 > 150 触发。
if (!in_cooldown && (int)raw < (int)baseline) {
    const double alpha = 1.0 / (double)(TOUCH_BASELINE_SLOW_RATIO + 1);
    baseline_f = baseline_f * (1.0 - alpha) + (double)raw * alpha;
    baseline = (uint32_t)(baseline_f + 0.5);
}

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void StartTouchHeadShake() {
    xTaskCreate(TouchHeadShakeTask, "touch_head", 2048, nullptr, 5, nullptr);
}

class MicroduckXiaozhi : public WifiBoard {
private:
    LcdDisplay* display_;
    PowerManager* power_manager_;
    Button boot_button_;
    WebSocketControlServer* ws_control_server_;
    HardwareConfig hw_config_;
    AudioCodec* audio_codec_;
    i2c_master_bus_handle_t i2c_bus_;
    Camera* camera_;
    bool is_camera_board_;
    bool has_camera_;
    OttoCameraType camera_type_;

    bool DetectHardwareVersion() {
        constexpr gpio_num_t kDetectGpio15 = GPIO_NUM_15;
        constexpr gpio_num_t kDetectGpio16 = GPIO_NUM_16;
        constexpr int kStableSampleCount = 8;
        constexpr uint32_t kSettleTimeUs = 5000;
        constexpr uint32_t kSampleIntervalUs = 1000;

        gpio_config_t detect_config = {
            .pin_bit_mask = (1ULL << kDetectGpio15) | (1ULL << kDetectGpio16),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&detect_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "板型识别 GPIO 初始化失败: %s", esp_err_to_name(ret));
            return false;
        }

        esp_rom_delay_us(kSettleTimeUs);
        for (int sample = 0; sample < kStableSampleCount; ++sample) {
            int gpio15_level = gpio_get_level(kDetectGpio15);
            int gpio16_level = gpio_get_level(kDetectGpio16);
            if (gpio15_level == 0 || gpio16_level == 0) {
                ESP_LOGI(TAG, "板型识别: GPIO15=%d GPIO16=%d，判定为无摄像头版", gpio15_level,
                         gpio16_level);
                return false;
            }
            if (sample + 1 < kStableSampleCount) {
                esp_rom_delay_us(kSampleIntervalUs);
            }
        }

        ESP_LOGI(TAG, "板型识别: GPIO15/GPIO16 稳定为高，判定为摄像头版");
        return true;
    }

    bool DetectCamera() {
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_2_BIT,
            .timer_num = LEDC_TIMER,
            .freq_hz = CAMERA_XCLK_FREQ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t ret = ledc_timer_config(&ledc_timer);
        if (ret != ESP_OK) {
            return false;
        }

        ledc_channel_config_t ledc_channel = {
            .gpio_num = CAMERA_XCLK,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 2,
            .hpoint = 0,
        };
        ret = ledc_channel_config(&ledc_channel);
        if (ret != ESP_OK) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = CAMERA_VERSION_CONFIG.i2c_sda_pin,
            .scl_io_num = CAMERA_VERSION_CONFIG.i2c_scl_pin,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };

        ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_);
        if (ret != ESP_OK) {
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
            return false;
        }
        const uint8_t camera_addresses[] = {0x30, 0x3C, 0x21, 0x60};
        bool camera_found = false;
        uint16_t detected_pid = 0;

        for (size_t i = 0; i < sizeof(camera_addresses); i++) {
            uint8_t addr = camera_addresses[i];
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = 100000,
            };

            i2c_master_dev_handle_t dev_handle;
            ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev_handle);
            if (ret == ESP_OK) {
                uint8_t data[2] = {0, 0};

                uint8_t reg_addr_8bit = 0x0A;
                ret = i2c_master_transmit_receive(dev_handle, &reg_addr_8bit, 1, data, 2, 200);
                if (ret == ESP_OK && (data[0] != 0 || data[1] != 0)) {
                    detected_pid = (data[0] << 8) | data[1];
                    ESP_LOGI(TAG, "检测到摄像头 (OV2640方式) PID=0x%04X (地址=0x%02X)",
                             detected_pid, addr);
                    camera_found = true;
                    i2c_master_bus_rm_device(dev_handle);
                    break;
                }

                uint8_t reg_addr_high[2] = {0x30, 0x0A};
                uint8_t reg_addr_low[2] = {0x30, 0x0B};
                uint8_t pid_high = 0, pid_low = 0;

                ret = i2c_master_transmit_receive(dev_handle, reg_addr_high, 2, &pid_high, 1, 200);
                if (ret == ESP_OK) {
                    ret =
                        i2c_master_transmit_receive(dev_handle, reg_addr_low, 2, &pid_low, 1, 200);
                    if (ret == ESP_OK) {
                        detected_pid = (pid_high << 8) | pid_low;
                        if (detected_pid != 0) {
                            ESP_LOGI(TAG, "检测到摄像头 (OV3660方式) PID=0x%04X (地址=0x%02X)",
                                     detected_pid, addr);
                            camera_found = true;
                            i2c_master_bus_rm_device(dev_handle);
                            break;
                        }
                    }
                }

                i2c_master_bus_rm_device(dev_handle);
            }
        }

        if (!camera_found) {
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
            camera_type_ = OTTO_CAMERA_NONE;
        } else {
            // 根据 PID 判断摄像头类型
            if (detected_pid == OTTO_OV2640_PID_1 || detected_pid == OTTO_OV2640_PID_2) {
                camera_type_ = OTTO_CAMERA_OV2640;
                ESP_LOGI(TAG, "摄像头类型: OV2640 (PID=0x%04X)", detected_pid);
            } else if (detected_pid == OTTO_OV3660_PID) {
                camera_type_ = OTTO_CAMERA_OV3660;
                ESP_LOGI(TAG, "摄像头类型: OV3660 (PID=0x%04X)", detected_pid);
            } else {
                camera_type_ = OTTO_CAMERA_UNKNOWN;
                ESP_LOGW(TAG, "未知摄像头类型，PID=0x%04X", detected_pid);
            }
        }
        return camera_found;
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(hw_config_.power_charge_detect_pin,
                                          hw_config_.power_adc_unit, hw_config_.power_adc_channel);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = hw_config_.display_mosi_pin;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = hw_config_.display_clk_pin;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = hw_config_.display_cs_pin;
        io_config.dc_gpio_num = hw_config_.display_dc_pin;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = hw_config_.display_rst_pin;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new OttoEmojiDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeOttoController() { ::InitializeOttoController(hw_config_); }

public:
    const HardwareConfig& GetHardwareConfig() const { return hw_config_; }

    OttoCameraType GetCameraType() const { return camera_type_; }

private:
    void InitializeWebSocketControlServer() {
        ws_control_server_ = new WebSocketControlServer();
        if (!ws_control_server_->Start(8080)) {
            delete ws_control_server_;
            ws_control_server_ = nullptr;
            return;
        }
    }

    void StartNetwork() override {
        WifiBoard::StartNetwork();
        vTaskDelay(pdMS_TO_TICKS(1000));

        InitializeWebSocketControlServer();
    }

    bool InitializeCamera() {
        if (!has_camera_ || i2c_bus_ == nullptr) {
            return false;
        }

        // 释放检测阶段占用的 I2C 资源，避免与 esp_camera 初始化冲突。
        i2c_del_master_bus(i2c_bus_);
        i2c_bus_ = nullptr;
        // 停止检测阶段输出的 XCLK，交由 esp_camera 自行接管。
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_0;
        config.ledc_timer = LEDC_TIMER_0;
        config.pin_d0 = CAMERA_D0;
        config.pin_d1 = CAMERA_D1;
        config.pin_d2 = CAMERA_D2;
        config.pin_d3 = CAMERA_D3;
        config.pin_d4 = CAMERA_D4;
        config.pin_d5 = CAMERA_D5;
        config.pin_d6 = CAMERA_D6;
        config.pin_d7 = CAMERA_D7;
        config.pin_xclk = CAMERA_XCLK;
        config.pin_pclk = CAMERA_PCLK;
        config.pin_vsync = CAMERA_VSYNC;
        config.pin_href = CAMERA_HSYNC;
        config.pin_sccb_sda = CAMERA_VERSION_CONFIG.i2c_sda_pin;
        config.pin_sccb_scl = CAMERA_VERSION_CONFIG.i2c_scl_pin;
        config.sccb_i2c_port = 0;
        config.pin_pwdn = CAMERA_PWDN;
        config.pin_reset = CAMERA_RESET;
        config.xclk_freq_hz = CAMERA_XCLK_FREQ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_240X240;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new (std::nothrow) Esp32Camera(config);
        return camera_ != nullptr;
    }

    void InitializeAudioCodec() {
        if (hw_config_.audio_use_simplex) {
            audio_codec_ = new NoAudioCodecSimplex(
                hw_config_.audio_input_sample_rate, hw_config_.audio_output_sample_rate,
                hw_config_.audio_i2s_spk_gpio_bclk, hw_config_.audio_i2s_spk_gpio_lrck,
                hw_config_.audio_i2s_spk_gpio_dout, hw_config_.audio_i2s_mic_gpio_sck,
                hw_config_.audio_i2s_mic_gpio_ws, hw_config_.audio_i2s_mic_gpio_din);
        } else {
            audio_codec_ = new NoAudioCodecDuplex(
                hw_config_.audio_input_sample_rate, hw_config_.audio_output_sample_rate,
                hw_config_.audio_i2s_gpio_bclk, hw_config_.audio_i2s_gpio_ws,
                hw_config_.audio_i2s_gpio_dout, hw_config_.audio_i2s_gpio_din);
        }
    }

public:
    MicroduckXiaozhi()
        : boot_button_(BOOT_BUTTON_GPIO),
          audio_codec_(nullptr),
          i2c_bus_(nullptr),
          camera_(nullptr),
          is_camera_board_(false),
          has_camera_(false),
          camera_type_(OTTO_CAMERA_NONE) {
#if OTTO_HARDWARE_VERSION == OTTO_VERSION_AUTO
        // GPIO15/GPIO16 在摄像头版上有外部上拉；无摄像头版由内部弱下拉保持为低。
        is_camera_board_ = DetectHardwareVersion();
        ESP_LOGI(TAG, "自动检测硬件版本: %s", is_camera_board_ ? "摄像头版" : "无摄像头版");
#elif OTTO_HARDWARE_VERSION == OTTO_VERSION_CAMERA
        is_camera_board_ = true;
        ESP_LOGI(TAG, "强制使用摄像头版本配置");
#elif OTTO_HARDWARE_VERSION == OTTO_VERSION_NO_CAMERA
        is_camera_board_ = false;
        ESP_LOGI(TAG, "强制使用无摄像头版本配置");
#else
#error \
    "OTTO_HARDWARE_VERSION 设置无效，请使用 OTTO_VERSION_AUTO, OTTO_VERSION_CAMERA 或 OTTO_VERSION_NO_CAMERA"
#endif

        if (is_camera_board_)
            hw_config_ = CAMERA_VERSION_CONFIG;
        else
            hw_config_ = NON_CAMERA_VERSION_CONFIG;

        if (is_camera_board_) {
            has_camera_ = DetectCamera();
            if (!has_camera_) {
                ESP_LOGW(TAG, "摄像头版未检测到摄像头，将跳过摄像头初始化");
            }
        }

        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializePowerManager();
        InitializeAudioCodec();

        if (has_camera_) {
            if (!InitializeCamera()) {
                has_camera_ = false;
            }
        }

        InitializeOttoController();
        StartTouchHeadShake();
        ws_control_server_ = nullptr;
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override { return audio_codec_; }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight* backlight = nullptr;
        if (backlight == nullptr) {
            backlight =
                new PwmBacklight(hw_config_.display_backlight_pin, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        }
        return backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual Camera* GetCamera() override { return has_camera_ ? camera_ : nullptr; }
};

DECLARE_BOARD(MicroduckXiaozhi);