#include "mode_manager.h"

#include <esp_log.h>

#include <algorithm>
#include <string>

#include "settings.h"

#define TAG "ModeManager"

namespace {
constexpr const char* kSettingsNs = "microduck";
constexpr const char* kKeyLastMode = "last_mode";

// NVS 中持久化的状态字符串
constexpr const char* kModeDialogue = "dialogue";
constexpr const char* kModeSubmenuPrefix = "submenu:";
}  // namespace

ModeManager& ModeManager::GetInstance() {
  static ModeManager inst;
  return inst;
}

void ModeManager::Register(std::unique_ptr<SubFeatureModule> mod) {
  if (!mod) return;
  ESP_LOGI(TAG, "注册子功能模块: %s (priority=%d)", mod->Name(), mod->Priority());
  features_.push_back(std::move(mod));
  // 注册后按 priority 稳定排序
  std::stable_sort(features_.begin(), features_.end(),
                   [](const std::unique_ptr<SubFeatureModule>& a,
                      const std::unique_ptr<SubFeatureModule>& b) {
                     return a->Priority() < b->Priority();
                   });
}

int ModeManager::FeatureCount() const { return (int)features_.size(); }

SubFeatureModule* ModeManager::FeatureAt(int idx) const {
  if (idx < 0 || idx >= (int)features_.size()) return nullptr;
  return features_[idx].get();
}

void ModeManager::EnterSubMenu(int feature_index) {
  if (features_.empty()) {
    ESP_LOGW(TAG, "EnterSubMenu 失败：未注册任何子功能");
    return;
  }
  // 边界裁剪
  if (feature_index < 0) feature_index = 0;
  if (feature_index >= (int)features_.size()) feature_index = (int)features_.size() - 1;

  // 退出旧状态
  if (state_ == kSubMenu && current_feature_ >= 0 &&
      current_feature_ < (int)features_.size()) {
    features_[current_feature_]->OnExit();
  }

  state_ = kSubMenu;
  current_feature_ = feature_index;

  ESP_LOGI(TAG, "进入子菜单 [%d/%d]: %s", current_feature_ + 1,
           (int)features_.size(), features_[current_feature_]->Name());
  features_[current_feature_]->OnEnter();
  SavePersisted();
}

void ModeManager::NextFeature() {
  if (state_ != kSubMenu || features_.empty()) return;
  features_[current_feature_]->OnExit();
  current_feature_ = (current_feature_ + 1) % (int)features_.size();
  ESP_LOGI(TAG, "切换到子功能 [%d/%d]: %s", current_feature_ + 1,
           (int)features_.size(), features_[current_feature_]->Name());
  features_[current_feature_]->OnEnter();
  SavePersisted();
}

void ModeManager::ExitSubMenu() {
  if (state_ != kSubMenu) return;
  if (current_feature_ >= 0 && current_feature_ < (int)features_.size()) {
    features_[current_feature_]->OnExit();
  }
  state_ = kDialogue;
  current_feature_ = 0;
  ESP_LOGI(TAG, "退出子菜单，回到对话模式");
  SavePersisted();
}

void ModeManager::NotifyTick() {
  // 仅在子菜单中转发 OnTick 给当前功能。
  // 自动退出策略改由各功能自行决定（默认不退出，靠触摸长按手动退出）。
  if (state_ != kSubMenu) return;
  if (current_feature_ >= 0 && current_feature_ < (int)features_.size()) {
    features_[current_feature_]->OnTick();
  }
}

void ModeManager::LoadPersisted() {
  Settings settings(kSettingsNs, false);
  std::string mode = settings.GetString(kKeyLastMode, kModeDialogue);
  ESP_LOGI(TAG, "从 NVS 恢复模式: '%s'", mode.c_str());

  if (mode == kModeDialogue || features_.empty()) {
    state_ = kDialogue;
    current_feature_ = 0;
    return;
  }
  // 解析 "submenu:N"
  if (mode.rfind(kModeSubmenuPrefix, 0) == 0) {
    int idx = 0;
    // ESP-IDF 默认禁用 C++ 异常，替代 try/catch：手动解析数字字符。
    const std::string num_str =
        mode.substr(std::string(kModeSubmenuPrefix).size());
    bool ok = !num_str.empty();
    for (char c : num_str) {
      if (c < '0' || c > '9') {
        ok = false;
        break;
      }
      idx = idx * 10 + (c - '0');
    }
    if (!ok) idx = 0;
    if (idx < 0 || idx >= (int)features_.size()) idx = 0;
    if (idx >= 0 && idx < (int)features_.size()) {
      // 上电后保留在子菜单中（用户偏好）
      EnterSubMenu(idx);
      return;
    }
  }
  // fallback：进入对话模式
  state_ = kDialogue;
  current_feature_ = 0;
}

void ModeManager::SavePersisted() {
  Settings settings(kSettingsNs, true);
  std::string value;
  if (state_ == kDialogue) {
    value = kModeDialogue;
  } else {
    value = std::string(kModeSubmenuPrefix) + std::to_string(current_feature_);
  }
  settings.SetString(kKeyLastMode, value);
  ESP_LOGD(TAG, "持久化模式: '%s'", value.c_str());
}
