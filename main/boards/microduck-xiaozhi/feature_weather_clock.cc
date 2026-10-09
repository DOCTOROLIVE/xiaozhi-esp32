#include "feature_weather_clock.h"

#include <ctime>
#include <cstring>
#include <sys/time.h>

#include <esp_log.h>

#include <cJSON.h>
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "settings.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#include <lvgl.h>
#endif

#define TAG "WeatherClock"

namespace {
constexpr const char* kSettingsNs = "microduck";
constexpr const char* kKeyWeatherKey = "weather_key";
constexpr const char* kKeyWeatherLoc = "weather_loc";
constexpr const char* kDefaultLoc = "101010100";  // 北京
constexpr const char* kWeatherHost = "devapi.qweather.com";

// 天气刷新间隔（10 分钟）
constexpr int kWeatherRefreshSec = 600;

const char* kWeekdayName(int wday) {
  static const char* names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
  if (wday < 0 || wday > 6) return "?";
  return names[wday];
}

const char* WeatherIconFromCode(const char* code) {
  // 和风天气图标代码 → 字符
  // https://dev.qweather.com/docs/start/icon/
  if (!code) return "?";
  int c = atoi(code);
  if (c >= 100 && c <= 103) return "☀";   // 晴
  if (c >= 150 && c <= 153) return "⛅";  // 多云
  if (c >= 300 && c <= 399) return "🌧";  // 雨
  if (c >= 400 && c <= 499) return "❄";   // 雪
  if (c >= 500 && c <= 515) return "🌫";  // 雾/霾
  return "·";
}
}  // namespace

struct LvglWeatherUpdateCtx {
  WeatherClockModule* self;
  char text[64];
};

WeatherClockModule::WeatherClockModule() = default;
WeatherClockModule::~WeatherClockModule() { StopHttpTask(); }

void WeatherClockModule::CreateLabels() {
  auto display = Board::GetInstance().GetDisplay();
  if (!display) return;

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
  DisplayLockGuard lock(display);
  if (!lock) return;

  // 全屏黑色背景：覆盖对话模式下的表情/聊天气泡/状态栏，避免视觉叠加。
  // 子功能退出时 DeleteLabels 会一并删除，对话模式 UI 自然重新可见。
  label_bg_ = lv_obj_create(lv_scr_act());
  lv_obj_set_size(label_bg_, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_pos(label_bg_, 0, 0);
  lv_obj_set_style_bg_color(label_bg_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(label_bg_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(label_bg_, 0, 0);
  lv_obj_set_style_pad_all(label_bg_, 0, 0);

  // 时间（lvgl 默认只编译了 montserrat_14，使用它显示 HH:MM:SS，时间也清晰）
  label_time_ = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(label_time_, &lv_font_montserrat_14, 0);
  lv_label_set_text(label_time_, "--:--:--");
  lv_obj_align(label_time_, LV_ALIGN_CENTER, 0, -10);

  // 日期（时间下方）
  label_date_ = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(label_date_, &lv_font_montserrat_14, 0);
  lv_label_set_text(label_date_, "----/--/-- 周?");
  lv_obj_align(label_date_, LV_ALIGN_CENTER, 0, 30);

  // 天气（顶部）
  label_weather_ = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(label_weather_, &lv_font_montserrat_14, 0);
  lv_label_set_text(label_weather_, weather_text_);
  lv_obj_align(label_weather_, LV_ALIGN_TOP_MID, 0, 4);

  // 子菜单位置（右上角）
  label_position_ = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(label_position_, &lv_font_montserrat_14, 0);
  char pos[32];
  snprintf(pos, sizeof(pos), "[%d/%d]", ModeManager::GetInstance().CurrentFeatureIndex() + 1,
           ModeManager::GetInstance().FeatureCount());
  lv_label_set_text(label_position_, pos);
  lv_obj_align(label_position_, LV_ALIGN_TOP_RIGHT, -4, 4);
#endif
}

void WeatherClockModule::DeleteLabels() {
  auto display = Board::GetInstance().GetDisplay();
  if (!display) return;

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
  DisplayLockGuard lock(display);
  if (!lock) return;
  if (label_bg_) {
    lv_obj_del(label_bg_);
    label_bg_ = nullptr;
  }
  if (label_time_) {
    lv_obj_del(label_time_);
    label_time_ = nullptr;
  }
  if (label_date_) {
    lv_obj_del(label_date_);
    label_date_ = nullptr;
  }
  if (label_weather_) {
    lv_obj_del(label_weather_);
    label_weather_ = nullptr;
  }
  if (label_position_) {
    lv_obj_del(label_position_);
    label_position_ = nullptr;
  }
#endif
}

void WeatherClockModule::OnEnter() {
  ESP_LOGI(TAG, "进入天气时钟");
  CreateLabels();
  UpdateTimeLabel();
  StartHttpTask();
}

void WeatherClockModule::OnExit() {
  ESP_LOGI(TAG, "退出天气时钟");
  StopHttpTask();
  DeleteLabels();
}

void WeatherClockModule::UpdateTimeLabel() {
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
  if (!label_time_ || !label_date_) return;
  auto display = Board::GetInstance().GetDisplay();
  if (!display) return;

  DisplayLockGuard lock(display);
  if (!lock) return;

  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  lv_label_set_text(label_time_, buf);

  char datebuf[32];
  snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d %s", tm.tm_year + 1900,
           tm.tm_mon + 1, tm.tm_mday, kWeekdayName(tm.tm_wday));
  lv_label_set_text(label_date_, datebuf);
#endif
}

void WeatherClockModule::OnTick() {
  UpdateTimeLabel();
  // 天气刷新由 HTTP 任务内部循环控制（默认每 10 分钟拉一次）。
  // 此函数只负责更新时间显示；不再冗余判断 elapsed（避免编译器警告）。
}

void WeatherClockModule::UpdateWeatherLabel(const char* text) {
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
  if (!label_weather_) return;
  auto display = Board::GetInstance().GetDisplay();
  if (!display) return;
  DisplayLockGuard lock(display);
  if (!lock) return;
  lv_label_set_text(label_weather_, text);
#endif
}

void WeatherClockModule::LvglUpdateWeatherCb(void* user_data) {
  auto* ctx = static_cast<LvglWeatherUpdateCtx*>(user_data);
  if (ctx && ctx->self) {
    ctx->self->UpdateWeatherLabel(ctx->text);
  }
  delete ctx;
}

void WeatherClockModule::StartHttpTask() {
  if (http_task_ != nullptr) return;
  http_task_should_exit_ = false;
  xTaskCreate(&WeatherClockModule::HttpTaskEntry, "wc_http", 6144, this, 5,
              &http_task_);
}

void WeatherClockModule::StopHttpTask() {
  if (http_task_ == nullptr) return;
  http_task_should_exit_ = true;
  // HttpTaskEntry 的 RunHttpFetch 循环每帧检查 http_task_should_exit_，
  // 大约 500ms 内会自然退出 + vTaskDelete。
  // 我们仅做简单定时等待（最多 3 秒），避免对已删除任务句柄调用
  // eTaskGetState（UB）。若任务卡在 HTTP 请求中无法及时退出，
  // 下次 OnEnter 仍会通过 http_task_ != nullptr 判断避免重复创建。
  for (int i = 0; i < 30; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  http_task_ = nullptr;
  ESP_LOGI(TAG, "HTTP 任务已请求退出");
}

void WeatherClockModule::HttpTaskEntry(void* arg) {
  auto* self = static_cast<WeatherClockModule*>(arg);
  self->RunHttpFetch();
  vTaskDelete(nullptr);
}

void WeatherClockModule::RunHttpFetch() {
  // 读取配置
  Settings settings(kSettingsNs, false);
  std::string key = settings.GetString(kKeyWeatherKey, "");
  std::string loc = settings.GetString(kKeyWeatherLoc, kDefaultLoc);

  if (key.empty()) {
    ESP_LOGW(TAG, "weather_key 未配置（NVS key=%s），跳过天气拉取", kKeyWeatherKey);
    snprintf(weather_text_, sizeof(weather_text_), "未配置 API key");
    auto* ctx = new LvglWeatherUpdateCtx{this, {}};
    strncpy(ctx->text, weather_text_, sizeof(ctx->text) - 1);
    ctx->text[sizeof(ctx->text) - 1] = '\0';
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
    lv_async_call(LvglUpdateWeatherCb, ctx);
#endif
    return;
  }

  while (!http_task_should_exit_) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
      snprintf(weather_text_, sizeof(weather_text_), "网络未连接");
      auto* ctx = new LvglWeatherUpdateCtx{this, {}};
      strncpy(ctx->text, weather_text_, sizeof(ctx->text) - 1);
      ctx->text[sizeof(ctx->text) - 1] = '\0';
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
      lv_async_call(LvglUpdateWeatherCb, ctx);
#endif
      // 网络断开时每 30 秒重试
      for (int i = 0; i < 30 && !http_task_should_exit_; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
      continue;
    }

    auto http = network->CreateHttp(0);
    if (!http) {
      snprintf(weather_text_, sizeof(weather_text_), "网络未连接");
      auto* ctx = new LvglWeatherUpdateCtx{this, {}};
      strncpy(ctx->text, weather_text_, sizeof(ctx->text) - 1);
      ctx->text[sizeof(ctx->text) - 1] = '\0';
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
      lv_async_call(LvglUpdateWeatherCb, ctx);
#endif
      for (int i = 0; i < 30 && !http_task_should_exit_; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
      continue;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s/v7/weather/now?location=%s&key=%s&lang=zh",
             kWeatherHost, loc.c_str(), key.c_str());

    auto opened = http->Open("GET", url);
    if (!opened) {
      ESP_LOGW(TAG, "HTTP 打开失败: %s", opened.error().ToString().c_str());
      snprintf(weather_text_, sizeof(weather_text_), "连接失败");
    } else {
      auto status = http->GetStatusCode();
      if (!status || *status != 200) {
        ESP_LOGW(TAG, "HTTP 状态: %d", status ? *status : -1);
        snprintf(weather_text_, sizeof(weather_text_), "HTTP %d",
                 status ? *status : -1);
      } else {
        std::string body = http->ReadAll();

        // 解析 JSON: {"now":{"temp":"25","icon":"100","text":"晴"},"code":"200"}
        CJsonUniquePtr root(cJSON_Parse(body.c_str()));
        if (root) {
          cJSON* now_obj = cJSON_GetObjectItem(root.get(), "now");
          if (cJSON_IsObject(now_obj)) {
            cJSON* temp_j = cJSON_GetObjectItem(now_obj, "temp");
            cJSON* icon_j = cJSON_GetObjectItem(now_obj, "icon");
            cJSON* text_j = cJSON_GetObjectItem(now_obj, "text");
            const char* temp = cJSON_IsString(temp_j) ? temp_j->valuestring : "?";
            const char* icon = cJSON_IsString(icon_j) ? icon_j->valuestring : "?";
            const char* text = cJSON_IsString(text_j) ? text_j->valuestring : "?";
            const char* icon_char = WeatherIconFromCode(icon);
            snprintf(weather_text_, sizeof(weather_text_), "%s %s℃ %s",
                     icon_char, temp, text);
            ESP_LOGI(TAG, "天气更新: %s", weather_text_);
          }
        } else {
          ESP_LOGW(TAG, "JSON 解析失败");
          snprintf(weather_text_, sizeof(weather_text_), "解析失败");
        }
      }
    }
    http->Close();

    // 推送到 LVGL（异步安全）
    auto* ctx = new LvglWeatherUpdateCtx{this, {}};
    strncpy(ctx->text, weather_text_, sizeof(ctx->text) - 1);
    ctx->text[sizeof(ctx->text) - 1] = '\0';
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
    lv_async_call(LvglUpdateWeatherCb, ctx);
#endif

    // 10 分钟后重拉（每 5 秒检查退出标志以快速响应）
    for (int i = 0; i < kWeatherRefreshSec * 2 && !http_task_should_exit_; i++) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }

  ESP_LOGI(TAG, "HTTP 任务退出");
}
