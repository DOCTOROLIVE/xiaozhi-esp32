# 触摸电容切换子功能菜单

## Context

当前 microduck-xiaozhi 板的 GPIO2 电容触摸只用于"按下 → 脖摇头 2 次"的单一交互。需求把它升级为**多模式切换器**：

- 上电默认进入"对话机器人"模式（原行为完全保留）
- 按电容 → 进入"子菜单"，顺序循环切换已注册的功能（功能 1/2/3...）
- 功能 1（首发）= 天气时钟（实时显示时间 + 城市天气）
- 长按电容 1.5 秒 **或** 子菜单内 5 秒无操作 → 退出子菜单回到对话模式
- 模式状态 NVS 持久化：变频后记住上次所在模式
- 切换瞬间嘴点头 2 次作为反馈（保持原行为）
- 后续功能（3/4/5...）通过统一接口注册，零侵入

架构上**保持不变**：`Application` 仍跑主对话流，子功能模块是**独立的协程/任务**，与对话模式正交，不互相阻塞。触摸任务负责状态机调度 + 触发反馈动作。

---

## 设计：两层架构

### 第 1 层：模式管理器（新增 `mode_manager.h/cc`）

定义**通用接口 + 注册表**。每个子功能模块实现 `SubFeatureModule` 接口，注册进管理器。

```cpp
class SubFeatureModule {
 public:
  virtual ~SubFeatureModule() = default;
  virtual const char* Name() const = 0;          // "weather_clock" / "music_box" / ...
  virtual void OnEnter() = 0;                     // 子菜单切换到本功能时
  virtual void OnExit() = 0;                      // 退出本功能时（去对话或其他子功能）
  virtual void OnTick() {}                        // 1Hz 周期回调，可选
  virtual int Priority() const { return 0; }      // 排序权重，数字小排前
};

class ModeManager {
 public:
  void Register(std::unique_ptr<SubFeatureModule> mod);
  int  FeatureCount() const;
  SubFeatureModule* FeatureAt(int idx) const;

  // 状态切换
  void EnterSubMenu(int feature_index = 0);       // 从对话进入子菜单
  void NextFeature();                              // 顺序切换
  void ExitSubMenu();                              // 退出到对话

  // 持久化
  void LoadPersisted();                            // 上电从 NVS 读上次模式
  void SavePersisted();                            // 写 NVS

  bool InDialogue() const;                         // 当前是否在对话模式
  bool InSubMenu() const;
  int  CurrentFeatureIndex() const;
  void NotifyTick();                               // 每秒调一次，dispatch 到当前功能 OnTick
};
```

实现要点：
- `mode_manager.h/cc` 放 `main/boards/microduck-xiaozhi/`
- 注册表用 `std::vector<std::unique_ptr<SubFeatureModule>>`
- 持久化用 `Settings("microduck", false)`，键名 `last_mode`（"dialogue"/"submenu:0"/"submenu:1"...）

### 第 2 层：天气时钟模块（新增 `feature_weather_clock.h/cc`）

实现 `SubFeatureModule` 接口：

```cpp
class WeatherClockModule : public SubFeatureModule {
  // OnEnter: 启动 LVGL 时钟标签 + 启动 HTTP 任务拉天气（一次性）
  // OnExit:  隐藏标签、停止定时器
  // OnTick:  每秒更新时钟显示；每 10 分钟刷一次天气
};
```

LVGL 时钟界面：
- 屏幕中央 64×64 大字号显示 HH:MM
- 下方一行显示日期 YYYY-MM-DD / 周几
- 顶部一行显示城市名 + 温度 + 天气图标字符（如 ☀ ☁ ☂）
- 屏幕右上角小字显示"子菜单 2/3"（告知用户当前在哪）

天气 API：
- 使用**和风天气**（开发者版免费）`https://{host}/v7/weather/now?location={city}&key={key}`
- API key 通过 `Settings("microduck", false)` 持久化，键 `weather_key`
- 城市经纬度/ID 通过 `Settings("microduck", false)` 持久化，键 `weather_loc`
- 默认城市 = 北京 (`101010100`)
- HTTP 客户端用 ESP-IDF 内置 `esp_http_client`（无需新依赖）

天气任务：单独 `xTaskCreate` 跑 HTTP GET，**不阻塞触摸任务/对话流**

---

## 触摸任务改造

当前 [microduck_xiaozhi.cc:107-210](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/microduck_xiaozhi.cc#L107-L210) 是单次触发逻辑。改造为：

```cpp
enum TouchEvent { kTouchNone, kTouchShort, kTouchLong };
TouchEvent DetectTouchEvent(TickType_t* out_release_tick);  // 区分短按/长按

// 在触摸主循环中：
//   - 上升沿开始计时，下降沿产生事件
//   - 短按 (<1500ms) → kTouchShort
//   - 长按 (>=1500ms) → kTouchLong（按住期间立即触发，不需要等释放）
//   - 持续按住 1500ms+ 期间只触发一次长按
```

短按处理（kTouchShort）：
```cpp
if (mode_mgr.InDialogue()) {
  // 对话模式：进入子菜单第一个功能
  mode_mgr.EnterSubMenu(0);
  PlayNodFeedback();         // 点头 2 次
} else {
  // 子菜单：顺序切换到下一个
  mode_mgr.NextFeature();
  PlayNodFeedback();
}
```

长按处理（kTouchLong）：
```cpp
if (mode_mgr.InSubMenu()) {
  mode_mgr.ExitSubMenu();    // 退出到对话
  PlayNodFeedback();
}
// 对话模式下长按：暂时无操作（避免误触发）
```

子菜单无操作 5 秒超时：
- 由 `ModeManager::NotifyTick()` 在 LVGL tick 或独立 1Hz 任务里检测
- 跟踪 `last_user_action_tick`，5 秒后自动 `ExitSubMenu()`

---

## 改动文件清单

| 文件 | 改动 |
|---|---|
| [main/boards/microduck-xiaozhi/mode_manager.h](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/mode_manager.h)（新增） | SubFeatureModule 接口 + ModeManager 类 |
| [main/boards/microduck-xiaozhi/mode_manager.cc](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/mode_manager.cc)（新增） | 实现 |
| [main/boards/microduck-xiaozhi/feature_weather_clock.h](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/feature_weather_clock.h)（新增） | WeatherClockModule |
| [main/boards/microduck-xiaozhi/feature_weather_clock.cc](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/feature_weather_clock.cc)（新增） | 实现（含 LVGL 标签 + esp_http_client） |
| [main/boards/microduck-xiaozhi/microduck_xiaozhi.cc](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/microduck_xiaozhi.cc) | 触摸任务改为事件检测；构造函数注册 WeatherClockModule；启动 ModeManager |
| [main/boards/microduck-xiaozhi/microduck_xiaozhi.cc](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/microduck_xiaozhi.cc) | 头部加 #include "mode_manager.h" / "feature_weather_clock.h" |
| [main/boards/microduck-xiaozhi/CMakeLists.txt](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/CMakeLists.txt)（若不存在则新建） | 把 mode_manager.cc / feature_weather_clock.cc 加入 BOARD_SRC |

---

## 关键代码位置（实现参考）

- `ModeManager::LoadPersisted` → 读 NVS `last_mode`，按枚举解析
- `ModeManager::SavePersisted` → 每次状态变更后写 NVS
- `WeatherClockModule::OnTick` → 更新 LVGL 时间标签；天气缓存 > 10 分钟就重拉
- `WeatherClockModule::OnEnter` → 创建 lv_label × 3（时间/日期/天气），启动 1Hz tick
- `WeatherClockModule::OnExit` → 删除标签

---

## 兼容性保证

1. **对话模式完全保留**：子功能模块只接管子菜单状态；`Application` 主对话流不受影响
2. **原触摸触发摇头行为保留**：作为"点头反馈"被复用（点头 = ACTION_DUCK_NOD，用脖舵机而非头摇摆）
3. **不动设备状态机**：不调用 `SetDeviceState()`，避免与现有 kDeviceStateListening/Speaking 冲突
4. **不动现有 Otto 控制器/动作**：新功能调用既有动作 API（点头/嘴张合）作为反馈
5. **新增天气 HTTP 任务**：与触摸任务并行，使用独立 task，不阻塞

---

## 验证方法（构建烧录后）

1. **上电默认**：进入对话模式，屏幕显示原表情 + 状态文字
2. **短按电容**：脖点头 2 次 + 进入子菜单"天气时钟"，屏幕显示当前时间
3. **子菜单中再短按一次**：循环到下一个（如果只注册了 1 个功能，会循环回自己）
4. **长按电容 1.5 秒**：脖点头 2 次 + 回到对话模式
5. **子菜单中静置 5 秒**：自动回到对话模式
6. **重启设备**：上电后直接进入上次的模式（NVS 持久化验证）
7. **网络断开**：天气显示"加载中..."或上次缓存；时钟仍正常更新
8. **HTTP 调试**：串口日志 `天气 API 响应: temp=25 icon=☀`
9. **NVS 配置**：可通过串口命令写入 `weather_key` 和 `weather_loc`（后续可加配网页面）

---

## 后续扩展指南

注册新功能模块只需：

```cpp
mode_mgr.Register(std::make_unique<MyFeatureModule>());
```

放在 [microduck_xiaozhi.cc 构造函数](file:///d:/ROI/otto_duck/code/xiaozhi-esp32/main/boards/microduck-xiaozhi/microduck_xiaozhi.cc) 中 `WeatherClockModule` 注册代码之后即可，无需改其他文件。
