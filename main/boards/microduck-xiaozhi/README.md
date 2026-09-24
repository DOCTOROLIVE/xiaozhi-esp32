# Microduck-xiaozhi

小智 AI 控制的**鸭子形态**双足机器人。在 otto-robot 基础上重新分配舵机用途：腿仍用于走路，左"手"舵机改为鸭脖（左右转头），右"手"舵机改为鸭嘴（上下开合）。

## 与 otto-robot 的差异

| 项 | otto-robot | microduck-xiaozhi |
|---|---|---|
| `Board` 类名 | `OttoRobot` | `MicroduckXiaozhi` |
| 目录 | `main/boards/otto-robot/` | `main/boards/microduck-xiaozhi/` |
| Kconfig | `CONFIG_BOARD_TYPE_OTTO_ROBOT` | `CONFIG_BOARD_TYPE_MICRODUCK_XIAOZHI` |
| Board type (`config.json`) | `otto-robot` | `microduck-xiaozhi` |
| 脖舵机 | 左手（左/右挥） | `left_hand_pin` GPIO：鸭脖（左右转头，90° 中位） |
| 嘴舵机 | 右手（左/右挥） | `right_hand_pin` GPIO：鸭嘴（开/合，闭合位 130°） |
| 构建命令 | `python scripts/build.py otto-robot --name otto-robot` | `python scripts/build.py microduck-xiaozhi --name microduck-xiaozhi` |

引脚配置 (`config.h`) 与 otto-robot 一致；不复用 Otto 内部的 "hands_up/hand_wave" 等手部动作，新增 `self.duck.*` 工具集。

## 舵机映射

| 引脚 | otto-robot 用途 | microduck-xiaozhi 用途 | 索引 |
|---|---|---|---|
| `left_leg_pin` | 左腿（内收/外展） | 左腿 | 0 |
| `right_leg_pin` | 右腿（内收/外展） | 右腿 | 1 |
| `left_foot_pin` | 左脚（上下翻转） | 左脚 | 2 |
| `right_foot_pin` | 右脚（上下翻转） | 右脚 | 3 |
| `left_hand_pin` | 左手 | **鸭脖**（左右转头） | 4 |
| `right_hand_pin` | 右手 | **鸭嘴**（上下开合） | 5 |

## MCP 工具

### 腿部（沿用 `self.otto.*`）

`self.otto.action` 包含 walk/turn/jump/swing/moonwalk/bend/shake_leg/updown/whirlwind_leg/sit/showcase/home 等。

### 颈部 / 嘴（新增 `self.duck.*`）

| 工具 | 作用 | 参数 |
|---|---|---|
| `self.duck.turn_head` | 转头 | `direction` (-1左/0中/1右), `amount` (10-80度), `speed` (暂未用) |
| `self.duck.shake_head` | 摇头（持续振荡） | `steps` (1-10), `speed` (300-1500ms), `amount` (10-60度) |
| `self.duck.open_beak` | 张嘴 | `amount` (10-70度), `speed` (100-1500) |
| `self.duck.close_beak` | 闭嘴 | `speed` (100-1500) |
| `self.duck.flap_beak` | 嘴扇动（持续开关） | `steps` (1-10), `speed` (200-1000ms), `amount` (10-60度) |
| `self.duck.neutral` | 脖归中+嘴闭合 | — |
| `self.duck.calibrate` | 舵机微调（持久化 NVS） | `servo` (neck/beak/left_leg/...), `offset` (-30..30) |
| `self.duck.get_trims` | 读取所有舵机微调 | — |

### 状态 / 信息

- `self.otto.get_status` — moving / idle
- `self.otto.get_ip` — WiFi IP
- `self.battery.get_level` — 电量 + 充电

## 初始姿态

启动时顺序入队两个动作：

1. `ACTION_HOME` —— Otto 标准 Home（左/右手分别到 45°/135°）
2. `ACTION_DUCK_RESET` —— 鸭子姿态：脖 90°（中位）+ 嘴 130°（闭合）

## LLM 角色提示词

> 你是 microduck，一只双足鸭子形态的 AI 机器人。
>
> 你有 4 条腿可以走路、转向、跳跃、坐下 —— 调用 `self.otto.action`，action ∈ {walk, turn, jump, sit, ...}。
> 你没有手臂；原"手部舵机"已被复用为：
>   - **脖子**：可左/右转头 + 摇头 + 回中位。调用 `self.duck.turn_head(direction, amount)` 或 `self.duck.shake_head` 或 `self.duck.neutral`。
>   - **鸭嘴**：可张开、闭合、扇动。调用 `self.duck.open_beak(amount)` / `self.duck.close_beak` / `self.duck.flap_beak`。
>
> 走路时也可以同时张嘴或转头；动作会自动排队执行，不会冲突。
>
> 机械校准：`self.duck.calibrate(servo, offset)` 调整某路舵机的零点偏置（如脖子装偏 5° 时设 `servo=neck, offset=5`），会持久化到 NVS。

## 板卡构建

```powershell
python scripts\build.py microduck-xiaozhi --name microduck-xiaozhi
```