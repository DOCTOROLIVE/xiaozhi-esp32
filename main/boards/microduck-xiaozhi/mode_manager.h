#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief 子功能模块抽象接口
 *
 * 每个实现该接口的类代表一个"子菜单"中的功能单元。
 * 子功能模块独立于主对话流，由 ModeManager 调度。
 *
 * 生命周期：
 *   - OnEnter()：ModeManager 切换到本功能时调用，必须创建/启动本功能所需的资源
 *                （如 LVGL 标签、定时器、HTTP 任务）
 *   - OnTick()：每秒由 ModeManager::NotifyTick() 调度一次（可选）
 *   - OnExit()：ModeManager 离开本功能时调用，必须释放资源、停止任务
 *
 * 线程模型：
 *   - OnEnter/OnExit/OnTick 都在 LVGL 主线程调用
 *   - HTTP 等耗时操作必须在模块内部 xTaskCreate 自己的任务
 */
class SubFeatureModule {
 public:
  virtual ~SubFeatureModule() = default;

  /** @brief 功能名（NVS 持久化、日志用），必须全局唯一 */
  virtual const char* Name() const = 0;

  /** @brief 排序权重（同优先级按注册顺序），数字小排前 */
  virtual int Priority() const { return 0; }

  /** @brief 进入本功能（ModeManager 状态变更后调用） */
  virtual void OnEnter() = 0;

  /** @brief 离开本功能 */
  virtual void OnExit() = 0;

  /** @brief 1Hz 周期回调，可选实现 */
  virtual void OnTick() {}
};

/**
 * @brief 模式管理器（对话模式 ↔ 子菜单模式）
 *
 * 单例模式。所有触摸事件回调都通过本类处理。
 * 状态在 NVS 中持久化（命名空间 "microduck"，键名 "last_mode"）。
 *
 * 状态机：
 *   - kDialogue：默认，触摸短按 → 进入子菜单
 *   - kSubMenu：触摸短按 → NextFeature()，触摸长按 → ExitSubMenu()
 *
 * 子菜单超时：进入子菜单后连续 5 秒无用户动作 → 自动 ExitSubMenu()
 */
class ModeManager {
 public:
  static ModeManager& GetInstance();

  /** @brief 注册子功能模块（构造时调用一次） */
  void Register(std::unique_ptr<SubFeatureModule> mod);

  /** @brief 已注册的功能数量 */
  int FeatureCount() const;

  /** @brief 索引访问已注册功能（用于调试/显示） */
  SubFeatureModule* FeatureAt(int idx) const;

  // 状态查询
  bool InDialogue() const { return state_ == kDialogue; }
  bool InSubMenu() const { return state_ == kSubMenu; }
  int CurrentFeatureIndex() const { return current_feature_; }

  // 状态切换
  void EnterSubMenu(int feature_index = 0);
  void NextFeature();
  void ExitSubMenu();

  // 周期通知（由 1Hz 定时器/任务调用）
  void NotifyTick();

  // 持久化
  void LoadPersisted();
  void SavePersisted();

  /** @brief 长按判定时间（ms）—— 由触摸任务使用 */
  static constexpr int kTouchLongPressMs = 1000;

 private:
  ModeManager() = default;

  enum State { kDialogue, kSubMenu };

  State state_ = kDialogue;
  int current_feature_ = -1;
  std::vector<std::unique_ptr<SubFeatureModule>> features_;
};
