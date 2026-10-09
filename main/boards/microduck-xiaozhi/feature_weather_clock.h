#pragma once

#include "mode_manager.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#include <lvgl.h>
#endif

/**
 * @brief 天气时钟子功能模块
 *
 * 功能描述：
 *   - 屏幕中央显示当前时间 HH:MM:SS（大字号）
 *   - 下方显示日期 YYYY-MM-DD + 周几
 *   - 顶部一行显示城市名 + 温度 + 天气图标字符
 *   - 右上角小字显示子菜单序号 "[N/M]"
 *
 * 数据来源：
 *   - 时钟：本地 RTC / gettimeofday（首次启动需 NTP 同步）
 *   - 天气：和风天气 v7 API
 *     URL: https://{host}/v7/weather/now?location={city}&key={key}
 *     API key 通过 Settings("microduck").weather_key 配置
 *     城市 ID 通过 Settings("microduck").weather_loc 配置（默认 101010100 = 北京）
 *
 * NVS 配置（首次使用需手动写入或后续配网页面写入）：
 *   namespace = "microduck"
 *   key       = "weather_key"  // string, 和风 API key
 *   key       = "weather_loc"  // string, 城市 ID (默认 "101010100")
 *
 * 资源：
 *   - 创建 3 个 LVGL label（时间/日期/天气）+ 1 个位置 label
 *   - HTTP 任务：xTaskCreate 在 OnEnter 中启动，OnExit 中删除
 *
 * 线程模型：
 *   - OnEnter/OnExit/OnTick 在 LVGL 主线程调用
 *   - HTTP 任务独立运行，回调通过 lvgl_async_call 或直接 lv_lock 写 label
 */
class WeatherClockModule : public SubFeatureModule {
 public:
  WeatherClockModule();
  ~WeatherClockModule() override;

  const char* Name() const override { return "weather_clock"; }
  int Priority() const override { return 10; }  // 第一个子功能

  void OnEnter() override;
  void OnExit() override;
  void OnTick() override;

 private:
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
  // LVGL 标签句柄（OnEnter 创建，OnExit 删除）
  lv_obj_t* label_bg_ = nullptr;       // 全屏背景，覆盖对话模式表情/聊天气泡
  lv_obj_t* label_time_ = nullptr;
  lv_obj_t* label_date_ = nullptr;
  lv_obj_t* label_weather_ = nullptr;
  lv_obj_t* label_position_ = nullptr;
#endif

  // 天气缓存
  char weather_text_[64] = "加载中...";

  // HTTP 任务
  TaskHandle_t http_task_ = nullptr;
  volatile bool http_task_should_exit_ = false;

  void CreateLabels();
  void DeleteLabels();
  void UpdateTimeLabel();
  void UpdateWeatherLabel(const char* text);

  void StartHttpTask();
  void StopHttpTask();
  static void HttpTaskEntry(void* arg);
  void RunHttpFetch();

  // 同步设置 weather_text_ 后通过 lvgl_async_call 安全更新 label
  static void LvglUpdateWeatherCb(void* user_data);
};
