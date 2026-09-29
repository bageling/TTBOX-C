// RecoilClosedLoop.hpp — 开火期闭环纠偏引擎（压枪 v1，业主 2026-09-29 裁定方案 A）
//
// 定位
// ----
// 这是「有实时偏移就纠偏，没有实时偏移就不猜」这句定案的直接实现：
// 开火后**每一帧**看实际偏了多少（error_y，像素），下一帧反向注入 count 把它拉回来，
// 连续纠偏，让弹道自己收敛在一个小区域里。不选枪、不录枪、不预采数据。
//
// 观测量：偏差减去慢基线（2026-09-29 15:3x，实机数据定案）
// ------------------------------------------------------
// 第一版直接拿 control_y 原始值当观测量，实机 27 秒只输出 17 count —— 数据定障：
//   · 瞄准系统常态带着静态负偏移（瞄准点取框顶 15%、Y 轴 PID 无 I 项 + 死区，
//     静止时 err_y ≈ -10~-20px）；
//   · 后坐力把偏差往正方向推，但被负底子抵住 ⇒「只在正偏差压」的门槛几乎永远过不去
//     （实测开火采样只有 41% 为正）。
// 修法：观测量换成 e_hp = err − baseline。baseline 是**没开火时**用慢 EMA 跟踪的
// 偏差底子（时间常数 baseline_tau_ms，默认 1.5s）；开火期间 baseline 冻结（不在开火中
// 学底子，防止长喷时把后坐力也学进去）。实机数据离线复算：e_hp 在开火采样中 98% 为正、
// 中位 +56px —— 后坐力信号从被埋住变成清晰可见。换目标/换世代时 baseline 一并清掉
// （不同目标的几何底子不同）；baseline 未就绪（刚换目标就开火）时以第一帧播种、该帧不压。
// baseline_tau_ms = 0 可退回第一版原始偏差口径（留作对照/回退开关）。
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
//     press = kp × e_hp  +  gain × ∫e_hp dt
//              └ 立刻纠 ┘      └ 补稳态残差 ┘
// 比例项这一帧看到多少偏移就按比例给多少，不等积分累积 ⇒ 点射每段开火第一帧就有输出。
// 单向：e_hp 为负（含 PID 开火瞬间向下过冲的 -100px 级深谷）一律不压、不上推。
// ★ 过冲护栏：kp 与标定值 px_per_count 的乘积应 < 1（一帧内不把偏移全额拉回）。
// e_hp 进入 P/I 前钳在 +100px（实测真信号 p90 ≈ 98px；+200px 级的是检测框跳变毛刺）。
//
// 观测边界（诚实说明，勿在文档/面板上过度承诺）
// ---------------------------------------------
// e_hp 仍同时包含「目标自己移动」与「相机（枪口）运动」两个原因 —— 单看这一个数
// 不可分离。本模块不声称能识别后坐力，只做：① 观测合格性门控（obs_ok 构造见 AimThread）；
// ② 只在开火期、且偏移朝下（后坐力主方向）时输出，单向钳位。目标自身运动仍归 PID。
//
// 固有特性：闭环把 e_hp 压到 0 后观测量自己消失 —— 这是收敛的终态，不是缺陷。
//
// 与既有两套压枪引擎互斥（2026-09-29 业主令）：本引擎 enabled 时老引擎整段不跑
// （互斥逻辑在 AimThread 的 recoil_cl_takeover），关掉则完全走老引擎。
// 默认 enabled=false ⇒ 不开时输出链与加入前逐字节一致。
#pragma once

#include <cmath>
#include <limits>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class RecoilClosedLoop {
public:
    enum State : int {
        kOff = 0,        // 未开火 / 开关关 / 无实时观测（不猜）
        kObserving = 1,  // 观测中（连续帧数未达 start_frames，或偏移为 0 暂不下压）
        kPressing = 2,   // 正在下压
    };

    struct Output {
        float add_y = 0.0f;    // 本帧下压量（count，>=0，直接叠加进 scaled_y）
        bool active = false;   // 本帧是否真的在下压
    };

    // 没开火（或未按开火键）但有目标时每帧调用：慢 EMA 学"偏差底子"。
    // 开火期间不调用（baseline 冻结）。target 丢失期间也不调用（基线保持）。
    void track_baseline(float err_y_px, float dt_ms, const RecoilClConfig& cfg) {
        if (!(cfg.baseline_tau_ms > 0.0f)) return;   // 基线关闭：原始偏差口径
        if (!std::isfinite(baseline_)) { baseline_ = err_y_px; return; }
        const float dt_s = dt_ms > 0.0f ? dt_ms / 1000.0f : 0.004f;
        const float alpha = dt_s / (cfg.baseline_tau_ms / 1000.0f);
        baseline_ += alpha * (err_y_px - baseline_);
    }

    // enabled / fire_active / obs_ok 任一不成立 ⇒ 清火控状态并返回零
    //（**不清 baseline** —— 底子是"当前目标几何"的测量，不是上一次开火的经验）。
    Output update(bool enabled, bool fire_active, bool obs_ok,
                  float error_y_px, float dt_ms, const RecoilClConfig& cfg) {
        Output out;
        if (!enabled || !fire_active || !obs_ok) {
            reset_fire();
            return out;
        }
        ++obs_frames_;
        if (obs_frames_ < cfg.start_frames) {
            add_y_ = 0.0f;   // 观察期：前几发不压（第一发子弹出膛时枪口还没抬，本就不该压）
            return out;
        }

        // ---- 观测量：减基线（第一版的教训，见文件头） ----
        float e = error_y_px;
        if (cfg.baseline_tau_ms > 0.0f) {
            if (!std::isfinite(baseline_)) {
                baseline_ = error_y_px;   // 刚换目标就开火：以本帧播种，本帧不压
                e = 0.0f;
            } else {
                e = error_y_px - baseline_;
            }
            if (e > 100.0f) e = 100.0f;   // 毛刺钳位（真信号 p90≈98px，+200px 级是检测跳变）
        }

        // ---- 比例项：这一帧看到多少偏移就按比例给多少 ----
        float p_term = cfg.kp * e;
        if (p_term < 0.0f) p_term = 0.0f;   // 单向：偏移在上方时不下压也不上推

        // ---- 积分项：吃掉比例项留下的稳态残差 ----
        const float dt_s = dt_ms > 0.0f ? dt_ms / 1000.0f : 0.004f;
        integral_ += e * dt_s;
        if (integral_ < 0.0f) integral_ = 0.0f;
        if (cfg.integral_max > 0.0f && integral_ > cfg.integral_max) integral_ = cfg.integral_max;
        float i_term = cfg.gain * integral_;
        if (i_term < 0.0f) i_term = 0.0f;

        float press = p_term + i_term;
        if (press < 0.0f) press = 0.0f;
        if (cfg.press_max_count > 0.0f && press > cfg.press_max_count) press = cfg.press_max_count;

        add_y_ = press;
        p_term_ = p_term;
        i_term_ = i_term;
        out.add_y = press;
        out.active = press > 0.0f;
        return out;
    }

    // 全量复位（换目标 / 换世代）：火控状态 + 基线一起清。
    void reset() {
        reset_fire();
        baseline_ = std::numeric_limits<float>::quiet_NaN();
    }

    // ---- 遥测 ----
    float integral_px_s() const { return integral_; }
    int obs_frames() const { return obs_frames_; }
    float add_y() const { return add_y_; }
    float p_term() const { return p_term_; }
    float i_term() const { return i_term_; }
    float baseline() const { return baseline_; }
    int state() const {
        if (obs_frames_ == 0) return kOff;
        return add_y_ > 0.0f ? kPressing : kObserving;
    }

private:
    void reset_fire() {
        integral_ = 0.0f;
        obs_frames_ = 0;
        add_y_ = 0.0f;
        p_term_ = 0.0f;
        i_term_ = 0.0f;
    }

    float integral_ = 0.0f;   // 累积偏移（px·s）
    int obs_frames_ = 0;      // 本次开火内的连续有效观测帧数
    float add_y_ = 0.0f;      // 上一帧下压量（count，已限幅）
    float p_term_ = 0.0f;     // 上一帧比例项（限幅前，count）
    float i_term_ = 0.0f;     // 上一帧积分项（限幅前，count）
    float baseline_ = std::numeric_limits<float>::quiet_NaN();  // 偏差底子（px；NaN=未就绪）
};

}  // namespace ttbox::core::aim
