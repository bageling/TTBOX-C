// RecoilClosedLoop.hpp — 开火期闭环纠偏引擎（压枪 v1，业主 2026-09-29 裁定方案 A）
//
// 定位
// ----
// 这是「有实时观测就压，没有实时观测就不猜」这句定案的直接实现：
// 开火后**每一帧**看实际偏了多少（error_y，像素），下一帧反向注入 count 把它拉回来，
// 连续纠偏，让弹道自己收敛在一个小区域里。不选枪、不录枪、不预采数据。
//
// 为什么是闭环而不是前馈
// ---------------------
// 前馈（"测出后坐力速度再按固定比例外推整段"）必须假设"所有枪的 vy → count 比例相同"，
// 而不同枪的后坐力速度/方向/节奏/水平偏移都不同 ⇒ 那个比例根本不存在
// （业主 2026-09-29 指出）。闭环不需要它：枪是哪把、几倍镜、什么节奏，
// 全部由**实时实测的偏差**自己表达。
//
// 观测量与它的边界（诚实说明，勿在文档/面板上过度承诺）
// ---------------------------------------------------
// 观测量 error_y = 瞄准点 − 画面固定参考点（core/src/aim/AimThread.cpp:435）。
// 它同时包含「目标自己移动」与「相机（枪口）运动」两个原因 —— 两者在画面里同号叠加，
// 单看这一个数**不可分离**。本模块因此**不声称能识别后坐力**，只做两件事：
//   ① 用观测合格性门控，把"目标在动而系统没咬住"的时段排除掉（obs_ok 的构造见 AimThread）；
//   ② 只在开火期、且偏差朝下（后坐力主方向）时累积，单向钳位。
// 目标自身运动那部分**仍归 PID**（PID 本来就在追目标），本模块不重复承担，
// 两者是"分工"而不是"叠加打架"。
//
// 一个必须讲清的固有特性：一旦闭环把偏差压到 0，观测量自己就消失了。
// 这是闭环控制的固有性质（误差可观测性随收敛下降），不是缺陷 —— 此时已无偏差可纠，
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
        kObserving = 1,  // 观测中（连续帧数未达 start_frames，或偏差为 0 暂不下压）
        kPressing = 2,   // 正在下压
    };

    struct Output {
        float add_y = 0.0f;    // 本帧下压量（count，>=0，直接叠加进 scaled_y）
        bool active = false;   // 本帧是否真的在下压
    };

    // enabled     : 配置开关
    // fire_active : 本帧是否处于开火态（热键 / 扳机）
    // obs_ok      : 观测是否合格（同目标 + 框高稳定 + 连续帧达标）—— 由调用方构造
    // error_y_px  : 实测偏差（像素；目标在准星下方为正 = 后坐力主方向）
    // dt_ms       : 本帧时长（ms）
    Output update(bool enabled, bool fire_active, bool obs_ok,
                  float error_y_px, float dt_ms, const RecoilClConfig& cfg) {
        Output out;
        // ★ 第一原则：有实时观测就压，没有实时观测就不猜。
        //   任一前提不成立 ⇒ 立即清零（不留跨开火记忆、不留保持窗）。
        if (!enabled || !fire_active || !obs_ok) {
            reset();
            return out;
        }
        ++obs_frames_;
        if (obs_frames_ < cfg.start_frames) {
            add_y_ = 0.0f;   // 观察期：前几发不压（设计内代价，不是缺陷）
            return out;
        }
        const float dt_s = dt_ms > 0.0f ? dt_ms / 1000.0f : 0.004f;
        integral_ += error_y_px * dt_s;
        // 单向钳位：偏差为负（目标在准星上方）时积分回落，归 0 即停 —— 只下压，永不上推。
        if (integral_ < 0.0f) integral_ = 0.0f;
        if (cfg.integral_max > 0.0f && integral_ > cfg.integral_max) integral_ = cfg.integral_max;
        float press = cfg.gain * integral_;
        if (press < 0.0f) press = 0.0f;
        if (cfg.press_max_count > 0.0f && press > cfg.press_max_count) press = cfg.press_max_count;
        add_y_ = press;
        out.add_y = press;
        out.active = press > 0.0f;
        return out;
    }

    void reset() {
        integral_ = 0.0f;
        obs_frames_ = 0;
        add_y_ = 0.0f;
    }

    // ---- 遥测 ----
    float integral_px_s() const { return integral_; }
    int obs_frames() const { return obs_frames_; }
    float add_y() const { return add_y_; }
    int state() const {
        if (obs_frames_ == 0) return kOff;
        return add_y_ > 0.0f ? kPressing : kObserving;
    }

private:
    float integral_ = 0.0f;   // 累积偏差（px·s）
    int obs_frames_ = 0;      // 本次开火内的连续有效观测帧数
    float add_y_ = 0.0f;      // 上一帧下压量（count）
};

}  // namespace ttbox::core::aim
