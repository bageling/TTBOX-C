// RecoilClosedLoop.hpp — 开火期闭环纠偏引擎（压枪 v1，业主 2026-09-29 裁定方案 A）
//
// 定位
// ----
// 这是「有实时偏移就纠偏，没有实时偏移就不猜」这句定案的直接实现：
// 开火后**每一帧**看实际偏了多少（error_y，像素），下一帧反向注入 count 把它拉回来，
// 连续纠偏，让弹道自己收敛在一个小区域里。不选枪、不录枪、不预采数据。
//
// 为什么是闭环而不是前馈
// ---------------------
// 前馈（"测出后坐力速度再按固定比例外推整段"）必须假设"所有枪的 vy → count 比例相同"，
// 而不同枪的后坐力速度/方向/节奏/水平偏移都不同 ⇒ 那个比例根本不存在
// （业主 2026-09-29 指出）。闭环不需要它：枪是哪把、几倍镜、什么节奏，
// 全部由**实时实测的偏移**自己表达。
//
// 比例 + 积分（2026-09-29 14:2x 业主令「先做 1」）
// ------------------------------------------------
// 纯积分要"从 0 爬起来"。短促的点射（按住 100ms 出头，主循环 250Hz ⇒ 约 25 帧）
// 每次松手清零后都要重新起步，累计下压量只有长按同长度的四分之一
// （仿真对照：点射 31.6 count vs 长按 120.0 count）。所以补一个比例项：
//
//     press = kp × error_y  +  gain × ∫error_y dt
//              └ 立刻纠 ┘      └ 补稳态残差 ┘
//
// 比例项**这一帧看到多少偏移就按比例给多少**，不等积分累积 ⇒ 点射的每一段开火里
// 第一帧就有输出。它同样单向（负偏差钳 0）、同样受 press_max_count 单帧安全阀约束。
// 纯比例会留稳态误差，所以积分项保留；两者相加后统一走限幅。
//
// ★ 过冲护栏：kp 与标定值 px_per_count 的乘积应 < 1（一帧内不把偏移全额拉回），
//   否则会来回摆。默认 kp 取保守值，实机整定时先动 kp 再动 gain。
//
// 观测量与它的边界（诚实说明，勿在文档/面板上过度承诺）
// ---------------------------------------------------
// 观测量 error_y = 瞄准点 − 画面固定参考点（core/src/aim/AimThread.cpp:435）。
// 它同时包含「目标自己移动」与「相机（枪口）运动」两个原因 —— 两者在画面里同号叠加，
// 单看这一个数**不可分离**。本模块因此**不声称能识别后坐力**，只做两件事：
//   ① 用观测合格性门控，把"目标在动而系统没咬住"的时段排除掉（obs_ok 的构造见 AimThread）；
//   ② 只在开火期、且偏移朝下（后坐力主方向）时累积，单向钳位。
// 目标自身运动那部分**仍归 PID**（PID 本来就在追目标），本模块不重复承担，
// 两者是"分工"而不是"叠加打架"。
//
// 一个必须讲清的固有特性：一旦闭环把偏移压到 0，观测量自己就消失了。
// 这是闭环控制的固有性质（误差可观测性随收敛下降），不是缺陷 —— 此时已无偏移可纠，
// "压住了"就是终态。所以本模块**不需要**、也**不应该**在收敛后继续加大输出。
//
// 与既有两套压枪引擎完全独立（RecoilConfig 老速率模型 / RecoilBbConfig BB 三段查表各自照旧）。
// 默认 enabled=false ⇒ 不开时输出链与加入前逐字节一致。
#pragma once

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class RecoilClosedLoop {
public:
    // 遥测状态（面板/IPC 消费）
    enum State : int {
        kOff = 0,        // 未开火 / 开关关 / 无实时观测（不猜）
        kObserving = 1,  // 观测中（连续帧数未达 start_frames，或偏移为 0 暂不下压）
        kPressing = 2,   // 正在下压
    };

    struct Output {
        float add_y = 0.0f;    // 本帧下压量（count，>=0，直接叠加进 scaled_y）
        bool active = false;   // 本帧是否真的在下压
    };

    // enabled     : 配置开关
    // fire_active : 本帧是否处于开火态（热键 / 扳机）
    // obs_ok      : 观测是否合格（同目标 + 框高稳定 + 连续帧达标）—— 由调用方构造
    // error_y_px  : 实测偏移（像素；目标在准星下方为正 = 后坐力主方向）
    // dt_ms       : 本帧时长（ms）
    Output update(bool enabled, bool fire_active, bool obs_ok,
                  float error_y_px, float dt_ms, const RecoilClConfig& cfg) {
        Output out;
        // ★ 第一原则：有实时观测就纠偏，没有实时观测就不猜。
        //   任一前提不成立 ⇒ 立即清零（不留跨开火记忆、不留保持窗）。
        if (!enabled || !fire_active || !obs_ok) {
            reset();
            return out;
        }
        ++obs_frames_;
        if (obs_frames_ < cfg.start_frames) {
            add_y_ = 0.0f;   // 观察期：前几发不压（第一发子弹出膛时枪口还没抬，本就不该压）
            return out;
        }

        // ---- 比例项：这一帧看到多少偏移就按比例给多少（点射第一帧即有输出）----
        float p_term = cfg.kp * error_y_px;
        if (p_term < 0.0f) p_term = 0.0f;   // 单向：偏移在上方时不下压也不上推

        // ---- 积分项：吃掉比例项留下的稳态残差 ----
        const float dt_s = dt_ms > 0.0f ? dt_ms / 1000.0f : 0.004f;
        integral_ += error_y_px * dt_s;
        if (integral_ < 0.0f) integral_ = 0.0f;
        if (cfg.integral_max > 0.0f && integral_ > cfg.integral_max) integral_ = cfg.integral_max;
        float i_term = cfg.gain * integral_;
        if (i_term < 0.0f) i_term = 0.0f;

        float press = p_term + i_term;
        if (press < 0.0f) press = 0.0f;
        if (cfg.press_max_count > 0.0f && press > cfg.press_max_count) press = cfg.press_max_count;

        add_y_ = press;
        // 遥测记的是**限幅前**的两项原始值（便于整定时看清"想给多少"）。
        // 被限幅时 p_term + i_term 会大于 add_y，这是正常的。
        p_term_ = p_term;
        i_term_ = i_term;
        out.add_y = press;
        out.active = press > 0.0f;
        return out;
    }

    void reset() {
        integral_ = 0.0f;
        obs_frames_ = 0;
        add_y_ = 0.0f;
        p_term_ = 0.0f;
        i_term_ = 0.0f;
    }

    // ---- 遥测 ----
    float integral_px_s() const { return integral_; }
    int obs_frames() const { return obs_frames_; }
    float add_y() const { return add_y_; }
    float p_term() const { return p_term_; }
    float i_term() const { return i_term_; }
    int state() const {
        if (obs_frames_ == 0) return kOff;
        return add_y_ > 0.0f ? kPressing : kObserving;
    }

private:
    float integral_ = 0.0f;   // 累积偏移（px·s）
    int obs_frames_ = 0;      // 本次开火内的连续有效观测帧数
    float add_y_ = 0.0f;      // 上一帧下压量（count，已限幅）
    float p_term_ = 0.0f;     // 上一帧比例项（限幅前，count）
    float i_term_ = 0.0f;     // 上一帧积分项（限幅前，count）
};

}  // namespace ttbox::core::aim
