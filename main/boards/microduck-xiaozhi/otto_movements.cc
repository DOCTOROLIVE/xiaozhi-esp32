#include "otto_movements.h"

#include <algorithm>
#include <cmath>

#include "freertos/idf_additions.h"
#include "oscillator.h"

static const char* TAG = "OttoMovements";

// 鸭子版：脖/嘴中立位（替代原 Otto HAND_HOME_POSITION=45）
#define NECK_HOME 90
#define BEAK_HOME 130

namespace {
float EaseOutCubic(float t) {
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}
}  // namespace

Otto::Otto() {
    is_otto_resting_ = false;
    has_hands_ = false;
    // 初始化所有舵机管脚为-1（未连接）
    for (int i = 0; i < SERVO_COUNT; i++) {
        servo_pins_[i] = -1;
        servo_trim_[i] = 0;
    }
}

Otto::~Otto() {
    DetachServos();
}

unsigned long IRAM_ATTR millis() {
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

void Otto::Init(int left_leg, int right_leg, int left_foot, int right_foot, int left_hand,
                int right_hand) {
    servo_pins_[LEFT_LEG] = left_leg;
    servo_pins_[RIGHT_LEG] = right_leg;
    servo_pins_[LEFT_FOOT] = left_foot;
    servo_pins_[RIGHT_FOOT] = right_foot;
    servo_pins_[LEFT_HAND] = left_hand;
    servo_pins_[RIGHT_HAND] = right_hand;

    // 检查是否有手部舵机
    has_hands_ = (left_hand != -1 && right_hand != -1);

    AttachServos();
    is_otto_resting_ = false;
}

///////////////////////////////////////////////////////////////////
//-- ATTACH & DETACH FUNCTIONS ----------------------------------//
///////////////////////////////////////////////////////////////////
void Otto::AttachServos() {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].Attach(servo_pins_[i]);
        }
    }
}

void Otto::DetachServos() {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].Detach();
        }
    }
}

///////////////////////////////////////////////////////////////////
//-- OSCILLATORS TRIMS ------------------------------------------//
///////////////////////////////////////////////////////////////////
void Otto::SetTrims(int left_leg, int right_leg, int left_foot, int right_foot, int left_hand,
                    int right_hand) {
    servo_trim_[LEFT_LEG] = left_leg;
    servo_trim_[RIGHT_LEG] = right_leg;
    servo_trim_[LEFT_FOOT] = left_foot;
    servo_trim_[RIGHT_FOOT] = right_foot;

    if (has_hands_) {
        servo_trim_[LEFT_HAND] = left_hand;
        servo_trim_[RIGHT_HAND] = right_hand;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetTrim(servo_trim_[i]);
        }
    }
}

///////////////////////////////////////////////////////////////////
//-- BASIC MOTION FUNCTIONS -------------------------------------//
///////////////////////////////////////////////////////////////////
void Otto::MoveServos(int time, int servo_target[]) {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    // 鸭子版嘴（RIGHT_HAND）机械硬限位 [100°, 130°]
    if (servo_pins_[RIGHT_HAND] != -1) {
        if (servo_target[RIGHT_HAND] < 100) servo_target[RIGHT_HAND] = 100;
        if (servo_target[RIGHT_HAND] > 130) servo_target[RIGHT_HAND] = 130;
    }

    if (time <= 10) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (servo_pins_[i] != -1) {
                servo_[i].SetPosition(servo_target[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(time));
        return;
    }

    int start[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++) {
        start[i] = servo_[i].GetPosition();
    }

    int steps = std::max(1, time / 10);
    for (int step = 1; step <= steps; step++) {
        float t = static_cast<float>(step) / static_cast<float>(steps);
        float eased_t = EaseOutCubic(t);
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (servo_pins_[i] != -1) {
                float interpolated =
                    start[i] + (servo_target[i] - start[i]) * eased_t;
                servo_[i].SetPosition(static_cast<int>(std::round(interpolated)));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetPosition(servo_target[i]);
        }
    }
}

void Otto::MoveSingle(int position, int servo_number) {
    if (position > 180)
        position = 90;
    if (position < 0)
        position = 90;

    // 鸭子版嘴（RIGHT_HAND）机械硬限位 [100°, 130°]：
    //   闭合 130°，最大张开 100°。任何 MoveSingle 调用都被钳位在此区间。
    if (servo_number == RIGHT_HAND) {
        if (position < 100) position = 100;
        if (position > 130) position = 130;
    }

    if (GetRestState() == true) {
        SetRestState(false);
    }

    if (servo_number >= 0 && servo_number < SERVO_COUNT && servo_pins_[servo_number] != -1) {
        servo_[servo_number].SetPosition(position);
    }
}

void Otto::OscillateServos(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                           double phase_diff[SERVO_COUNT], float cycle = 1) {
    // 鸭子版嘴（RIGHT_HAND）机械限位硬裁剪：
    //   闭合 = 130°，最大张开 = 100°，振幅上限 = max(0, 130 - center)
    //   当 center < 100° 或 130 < center 时，强制 center 落到 [100°, 130°] 并相应缩减振幅
    //   防止任何调用方（shake_head / flap_beak / 序列工具 / showcase / Otto 内置 Walk/Turn）
    //   把嘴推到 < 100° 撞限位。
    constexpr int BEAK_MIN = 100;
    constexpr int BEAK_MAX = 130;
    if (servo_pins_[RIGHT_HAND] != -1) {
        int center = offset[RIGHT_HAND] + 90;
        int amp_rh = amplitude[RIGHT_HAND];
        if (center < BEAK_MIN) {
            offset[RIGHT_HAND] += (BEAK_MIN - center);
            center = BEAK_MIN;
        } else if (center > BEAK_MAX) {
            offset[RIGHT_HAND] -= (center - BEAK_MAX);
            center = BEAK_MAX;
        }
        int max_amp_up = center - BEAK_MIN;       // 中心向上（更小角度）能摆多少
        int max_amp_down = BEAK_MAX - center;     // 中心向下（更大角度）能摆多少
        if (amp_rh > std::min(max_amp_up, max_amp_down)) {
            amp_rh = std::min(max_amp_up, max_amp_down);
            if (amp_rh < 0) amp_rh = 0;
        }
        amplitude[RIGHT_HAND] = amp_rh;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetO(offset[i]);
            servo_[i].SetA(amplitude[i]);
            servo_[i].SetT(period);
            servo_[i].SetPh(phase_diff[i]);
        }
    }

    double ref = millis();
    double end_time = period * cycle + ref;

    while (millis() < end_time) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (servo_pins_[i] != -1) {
                servo_[i].Refresh();
            }
        }
        vTaskDelay(5);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

void Otto::Execute(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                   double phase_diff[SERVO_COUNT], float steps = 1.0) {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    int cycles = (int)steps;

    //-- Execute complete cycles
    if (cycles >= 1)
        for (int i = 0; i < cycles; i++)
            OscillateServos(amplitude, offset, period, phase_diff);

    //-- Execute the final not complete cycle
    OscillateServos(amplitude, offset, period, phase_diff, (float)steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

//---------------------------------------------------------
//-- Execute2: 使用绝对角度作为振荡中心
//--  Parameters:
//--    amplitude: 振幅数组（每个舵机的振荡幅度）
//--    center_angle: 绝对角度数组（0-180度），作为振荡中心位置
//--    period: 周期（毫秒）
//--    phase_diff: 相位差数组（弧度）
//--    steps: 步数/周期数（可为小数）
//---------------------------------------------------------
void Otto::Execute2(int amplitude[SERVO_COUNT], int center_angle[SERVO_COUNT], int period,
                    double phase_diff[SERVO_COUNT], float steps = 1.0) {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    // 将绝对角度转换为offset（offset = center_angle - 90）
    int offset[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++) {
        offset[i] = center_angle[i] - 90;
    }

    int cycles = (int)steps;

    //-- Execute complete cycles
    if (cycles >= 1)
        for (int i = 0; i < cycles; i++)
            OscillateServos(amplitude, offset, period, phase_diff);

    //-- Execute the final not complete cycle
    OscillateServos(amplitude, offset, period, phase_diff, (float)steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

///////////////////////////////////////////////////////////////////
//-- HOME = Otto at rest position -------------------------------//
///////////////////////////////////////////////////////////////////
void Otto::Home(bool hands_down) {
    if (is_otto_resting_ == false) {  // Go to rest position only if necessary
        // 为所有舵机准备初始位置值
        int homes[SERVO_COUNT];
        int max_delta = 0;
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (i == LEFT_HAND || i == RIGHT_HAND) {
                if (hands_down) {
                    // 鸭子版：复位到鸭中立位（脖 90° + 嘴 130°），不再是 Otto 手位 45°/135°
                    if (i == LEFT_HAND) {
                        homes[i] = 90;
                    } else {                                  // RIGHT_HAND
                        homes[i] = 130;
                    }
                } else {
                    // 如果不需要复位手部，保持当前位置
                    homes[i] = servo_[i].GetPosition();
                }
            } else {
                // 腿部和脚部舵机始终复位
                homes[i] = 90;
            }

            if (servo_pins_[i] != -1) {
                max_delta = std::max(max_delta, std::abs(homes[i] - servo_[i].GetPosition()));
            }
        }

        // 位移越大，归位时间越长，避免末端“急刹车”感
        int home_time = std::clamp(500 + max_delta * 9, 500, 1700);
        MoveServos(home_time, homes);
        is_otto_resting_ = true;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
}

bool Otto::GetRestState() {
    return is_otto_resting_;
}

void Otto::SetRestState(bool state) {
    is_otto_resting_ = state;
}

///////////////////////////////////////////////////////////////////
//-- PREDETERMINED MOTION SEQUENCES -----------------------------//
///////////////////////////////////////////////////////////////////
//-- Otto movement: Jump
//--  Parameters:
//--    steps: Number of steps
//--    T: Period
//---------------------------------------------------------
void Otto::Jump(float steps, int period) {
    int up[SERVO_COUNT] = {90, 90, 150, 30, 90, 130};
    MoveServos(period, up);
    int down[SERVO_COUNT] = {90, 90, 90, 90, 90, 130};
    MoveServos(period, down);
}

//---------------------------------------------------------
//-- Otto gait: Walking  (forward or backward)
//--  Parameters:
//--    * steps:  Number of steps
//--    * T : Period
//--    * Dir: Direction: FORWARD / BACKWARD
//--    * amount: 已忽略（鸭子版无手臂，保留参数仅为兼容调用方）
//---------------------------------------------------------
void Otto::Walk(float steps, int period, int dir, int amount) {
    //-- Oscillator parameters for walking
    //-- Hip sevos are in phase
    //-- Feet servos are in phase
    //-- Hip and feet are 90 degrees out of phase
    //--      -90 : Walk forward
    //--       90 : Walk backward
    //-- Feet servos also have the same offset (for tiptoe a little bit)
    int A[SERVO_COUNT] = {30, 30, 30, 30, 0, 0};
    int O[SERVO_COUNT] = {0, 0, 5, -5, 0, 40};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(dir * -90), DEG2RAD(dir * -90), 0, 0};

    // 鸭子版：忽略 amount（原手部摆动幅度，arm_swing 已废弃）。
    // 脖/嘴振幅保持 0（A[4]=A[5]=0），嘴稳态 = O[RIGHT_HAND]+90 = 130° 闭合。
    // 历史 bug：此处曾按 has_hands_（Otto 内部成员，Init 按引脚有效性置 true）
    // 启用摆臂，walk/showcase 时嘴被 ±amount 甩到 [80°, 180°]，远超
    // [100°, 130°] 机械限位；OttoController 的同名 has_hands_=false 挡不住这里。
    if (amount > 0) {
        ESP_LOGD(TAG, "鸭子版无手臂，忽略摆臂幅度 amount=%d", amount);
    }

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto gait: Turning (left or right)
//--  Parameters:
//--   * Steps: Number of steps
//--   * T: Period
//--   * Dir: Direction: LEFT / RIGHT
//--   * amount: 已忽略（鸭子版无手臂，保留参数仅为兼容调用方）
//---------------------------------------------------------
void Otto::Turn(float steps, int period, int dir, int amount) {
    //-- Same coordination than for walking (see Otto::walk)
    //-- The Amplitudes of the hip's oscillators are not igual
    //-- When the right hip servo amplitude is higher, the steps taken by
    //--   the right leg are bigger than the left. So, the robot describes an
    //--   left arc
    int A[SERVO_COUNT] = {30, 30, 30, 30, 0, 0};
    int O[SERVO_COUNT] = {0, 0, 5, -5, 0, 40};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(-90), DEG2RAD(-90), 0, 0};

    if (dir == LEFT) {
        A[0] = 30;  //-- Left hip servo
        A[1] = 0;   //-- Right hip servo
    } else {
        A[0] = 0;
        A[1] = 30;
    }

    // 鸭子版：忽略 amount（原手部摆动幅度），原因见 Walk() 内注释。
    // 脖/嘴振幅保持 0，嘴稳态 = 130° 闭合，不被甩出 [100°, 130°] 机械限位。
    if (amount > 0) {
        ESP_LOGD(TAG, "鸭子版无手臂，忽略摆臂幅度 amount=%d", amount);
    }

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto gait: Lateral bend
//--  Parameters:
//--    steps: Number of bends
//--    T: Period of one bend
//--    dir: RIGHT=Right bend LEFT=Left bend
//---------------------------------------------------------
void Otto::Bend(int steps, int period, int dir) {
    // Parameters of all the movements. Default: Left bend
    int bend1[SERVO_COUNT] = {90, 90, 62, 35, 90, 130};
    int bend2[SERVO_COUNT] = {90, 90, 62, 105, 90, 130};
    int homes[SERVO_COUNT] = {90, 90, 90, 90, 90, 130};

    // Time of one bend, constrained in order to avoid movements too fast.
    // T=max(T, 600);
    // Changes in the parameters if right direction is chosen
    if (dir == -1) {
        bend1[2] = 180 - 35;
        bend1[3] = 180 - 60;  // Not 65. Otto is unbalanced
        bend2[2] = 180 - 105;
        bend2[3] = 180 - 60;
    }

    // Time of the bend movement. Fixed parameter to avoid falls
    int T2 = 800;

    // Bend movement
    for (int i = 0; i < steps; i++) {
        MoveServos(T2 / 2, bend1);
        MoveServos(T2 / 2, bend2);
        vTaskDelay(pdMS_TO_TICKS(period * 0.8));
        MoveServos(500, homes);
    }
}

//---------------------------------------------------------
//-- Otto gait: Shake a leg
//--  Parameters:
//--    steps: Number of shakes
//--    T: Period of one shake
//--    dir: RIGHT=Right leg LEFT=Left leg
//---------------------------------------------------------
void Otto::ShakeLeg(int steps, int period, int dir) {
    // This variable change the amount of shakes
    int numberLegMoves = 2;

    // Parameters of all the movements. Default: Right leg
    int shake_leg1[SERVO_COUNT] = {90, 90, 58, 35, 90, 130};
    int shake_leg2[SERVO_COUNT] = {90, 90, 58, 120, 90, 130};
    int shake_leg3[SERVO_COUNT] = {90, 90, 58, 60, 90, 130};
    int homes[SERVO_COUNT] = {90, 90, 90, 90, 90, 130};

    // Changes in the parameters if left leg is chosen
    if (dir == LEFT) {
        shake_leg1[2] = 180 - 35;
        shake_leg1[3] = 180 - 58;
        shake_leg2[2] = 180 - 120;
        shake_leg2[3] = 180 - 58;
        shake_leg3[2] = 180 - 60;
        shake_leg3[3] = 180 - 58;
    }

    // Time of the bend movement. Fixed parameter to avoid falls
    int T2 = 1000;
    // Time of one shake, constrained in order to avoid movements too fast.
    period = period - T2;
    period = std::max(period, 200 * numberLegMoves);

    for (int j = 0; j < steps; j++) {
        // Bend movement
        MoveServos(T2 / 2, shake_leg1);
        MoveServos(T2 / 2, shake_leg2);

        // Shake movement
        for (int i = 0; i < numberLegMoves; i++) {
            MoveServos(period / (2 * numberLegMoves), shake_leg3);
            MoveServos(period / (2 * numberLegMoves), shake_leg2);
        }
        MoveServos(500, homes);  // Return to home position
    }

    vTaskDelay(pdMS_TO_TICKS(period));
}

//---------------------------------------------------------
//-- Otto movement: Sit (坐下)
//---------------------------------------------------------
void Otto::Sit() {
    // 鸭子版：脖/嘴保留鸭中立位（90°/130°），不再是 Otto 手位 45°/135°
    int target[SERVO_COUNT] = {120, 60, 0, 180, 90, 130};
    MoveServos(600, target);
}

//---------------------------------------------------------
//-- Otto movement: up & down
//--  Parameters:
//--    * steps: Number of jumps
//--    * T: Period
//--    * h: Jump height: SMALL / MEDIUM / BIG
//--              (or a number in degrees 0 - 90)
//---------------------------------------------------------
void Otto::UpDown(float steps, int period, int height) {
    //-- Both feet are 180 degrees out of phase
    //-- Feet amplitude and offset are the same
    //-- Initial phase for the right foot is -90, so that it starts
    //--   in one extreme position (not in the middle)
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height, -height, 0, 40};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(-90), DEG2RAD(90), 0, 0};

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto movement: swinging side to side
//--  Parameters:
//--     steps: Number of steps
//--     T : Period
//--     h : Amount of swing (from 0 to 50 aprox)
//---------------------------------------------------------
void Otto::Swing(float steps, int period, int height) {
    //-- Both feets are in phase. The offset is half the amplitude
    //-- It causes the robot to swing from side to side
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {
        0, 0, height / 2, -height / 2, 0, 40};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(0), DEG2RAD(0), 0, 0};

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto movement: swinging side to side without touching the floor with the heel
//--  Parameters:
//--     steps: Number of steps
//--     T : Period
//--     h : Amount of swing (from 0 to 50 aprox)
//---------------------------------------------------------
void Otto::TiptoeSwing(float steps, int period, int height) {
    //-- Both feets are in phase. The offset is not half the amplitude in order to tiptoe
    //-- It causes the robot to swing from side to side
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height, -height, 0, 40};
    double phase_diff[SERVO_COUNT] = {0, 0, 0, 0, 0, 0};

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto gait: Jitter
//--  Parameters:
//--    steps: Number of jitters
//--    T: Period of one jitter
//--    h: height (Values between 5 - 25)
//---------------------------------------------------------
void Otto::Jitter(float steps, int period, int height) {
    //-- Both feet are 180 degrees out of phase
    //-- Feet amplitude and offset are the same
    //-- Initial phase for the right foot is -90, so that it starts
    //--   in one extreme position (not in the middle)
    //-- h is constrained to avoid hit the feets
    height = std::min(25, height);
    int A[SERVO_COUNT] = {height, height, 0, 0, 0, 0};
    int O[SERVO_COUNT] = {0, 0, 0, 0, 0, 40};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(-90), DEG2RAD(90), 0, 0, 0, 0};

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto gait: Ascending & turn (Jitter while up&down)
//--  Parameters:
//--    steps: Number of bends
//--    T: Period of one bend
//--    h: height (Values between 5 - 15)
//---------------------------------------------------------
void Otto::AscendingTurn(float steps, int period, int height) {
    //-- Both feet and legs are 180 degrees out of phase
    //-- Initial phase for the right foot is -90, so that it starts
    //--   in one extreme position (not in the middle)
    //-- h is constrained to avoid hit the feets
    height = std::min(13, height);
    int A[SERVO_COUNT] = {height, height, height, height, 0, 0};
    int O[SERVO_COUNT] = {
        0, 0, height + 4, -height + 4, 0, 40};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(-90), DEG2RAD(90), DEG2RAD(-90), DEG2RAD(90), 0, 0};

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto gait: Moonwalker. Otto moves like Michael Jackson
//--  Parameters:
//--    Steps: Number of steps
//--    T: Period
//--    h: Height. Typical valures between 15 and 40
//--    dir: Direction: LEFT / RIGHT
//---------------------------------------------------------
void Otto::Moonwalker(float steps, int period, int height, int dir) {
    //-- This motion is similar to that of the caterpillar robots: A travelling
    //-- wave moving from one side to another
    //-- The two Otto's feet are equivalent to a minimal configuration. It is known
    //-- that 2 servos can move like a worm if they are 120 degrees out of phase
    //-- In the example of Otto, the two feet are mirrored so that we have:
    //--    180 - 120 = 60 degrees. The actual phase difference given to the oscillators
    //--  is 60 degrees.
    //--  Both amplitudes are equal. The offset is half the amplitud plus a little bit of
    //-   offset so that the robot tiptoe lightly

    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {
        0, 0, height / 2 + 2, -height / 2 - 2, 0, 40};
    int phi = -dir * 90;
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(phi), DEG2RAD(-60 * dir + phi), 0, 0};

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//----------------------------------------------------------
//-- Otto gait: Crusaito. A mixture between moonwalker and walk
//--   Parameters:
//--     steps: Number of steps
//--     T: Period
//--     h: height (Values between 20 - 50)
//--     dir:  Direction: LEFT / RIGHT
//-----------------------------------------------------------
void Otto::Crusaito(float steps, int period, int height, int dir) {
    int A[SERVO_COUNT] = {25, 25, height, height, 0, 0};
    int O[SERVO_COUNT] = {
        0, 0, height / 2 + 4, -height / 2 - 4, 0, 40};
    double phase_diff[SERVO_COUNT] = {90, 90, DEG2RAD(0), DEG2RAD(-60 * dir), 0, 0};

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto gait: Flapping
//--  Parameters:
//--    steps: Number of steps
//--    T: Period
//--    h: height (Values between 10 - 30)
//--    dir: direction: FOREWARD, BACKWARD
//---------------------------------------------------------
void Otto::Flapping(float steps, int period, int height, int dir) {
    int A[SERVO_COUNT] = {12, 12, height, height, 0, 0};
    int O[SERVO_COUNT] = {
        0, 0, height - 10, -height + 10, 0, 40};
    double phase_diff[SERVO_COUNT] = {
        DEG2RAD(0), DEG2RAD(180), DEG2RAD(-90 * dir), DEG2RAD(90 * dir), 0, 0};

    //-- Let's oscillate the servos!
    Execute(A, O, period, phase_diff, steps);
}

//---------------------------------------------------------
//-- Otto gait: WhirlwindLeg (旋风腿)
//--   Parameters:
//--     steps: Number of steps
//--     period: Period (建议100-800毫秒)
//--     amplitude: amplitude (Values between 20 - 40)
//---------------------------------------------------------
void Otto::WhirlwindLeg(float steps, int period, int amplitude) {


    // 鸭子版：脖/嘴保持鸭中立（脖 90° + 嘴 130°），不再用 Otto 人形位 45°/20°
    int target[SERVO_COUNT] = {90, 90, 180, 90, 90, 130};
    MoveServos(100, target);
    target[RIGHT_FOOT] = 160;
    MoveServos(500, target);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 振荡期：脖子小幅摆动（amp=amplitude，绕 90° 中心），嘴保持 130° 不动
    int C[SERVO_COUNT] = {90, 90, 180, 160, 90, 130};
    int A[SERVO_COUNT] = {amplitude, 0, 0, 0, amplitude, 0};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(20), 0, 0, 0, DEG2RAD(20), 0};
    Execute2(A, C, period, phase_diff, steps);

}

//---------------------------------------------------------
//-- 鸭子版适配：原手部动作函数 (HandsUp/HandsDown/HandWave/Windmill/Takeoff/
//--   Fitness/Greeting/Shy/RadioCalisthenics/MagicCircle) 已删除。
//--   原 left_hand/right_hand 舵机已复用为脖/嘴，对应 self.duck.* 工具。
//---------------------------------------------------------







//---------------------------------------------------------
//-- 展示动作：串联多个动作展示
//---------------------------------------------------------
void Otto::Showcase() {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    // 鸭子版 Showcase 仅保留腿部动作；手部步骤（挥手/广播体操/起飞/健身）已删除。
    // 1. 往前走3步
    Walk(3, 1000, FORWARD, 50);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 2. 太空步
    Moonwalker(3, 900, 25, LEFT);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 3. 摇摆
    Swing(3, 1000, 30);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 4. 往后走3步
    Walk(3, 1000, BACKWARD, 50);
}

void Otto::EnableServoLimit(int diff_limit) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetLimiter(diff_limit);
        }
    }
}

void Otto::DisableServoLimit() {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].DisableLimiter();
        }
    }
}
