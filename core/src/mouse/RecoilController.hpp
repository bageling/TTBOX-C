// RecoilController.hpp — 压枪引擎（recoil assist）
//
// 功能（小白理解）：
//   按住开火键（默认左键）时，游戏枪械会有后坐力把准星往上顶。
//   压枪 = 开火期间自动给鼠标一个持续向下的补偿移动，让弹着点不飘。
//
// 设计原则：
//   本引擎只负责"算压枪量"，不直接发鼠标命令。
//   算出的 y 压枪量（count）由 AimThread 注入自身输出链
//   （pull_curve 之后、deadzone 之前，与 PID 输出融合），
//   之后统一走 deadzone → remainder → int16 → 拟人化整形 → 热键安全门。
//   与独立 recoil 链路不同：这里所有输出都受 TTBOX 安全边界约束。
//
// 行为（对齐 TTBOX 压枪模块，参数语义一致）：
//   1. 热键按住（hotkey/hotkey2，any=任一 / all=同时）才开始计时
//   2. trigger_delay_ms：按住超过该时长才压（防单点误触），松开重新计时
//   3. only_when_target_visible：有目标才压；目标丢失后 target_lost_release_ms 内继续压（保持窗口）
//   4. 下压速率 = strength × speed（px/s）× 帧间隔 dt
//   5. humanize：缓入缓出拆步（curve_strength）+ X 轴微动（jitter）
//   6. 亚像素残差累计：压枪量与 PID remainder 同域，小数不丢精度
//
// 默认值全部保持"关闭/零输出"，不改变现有行为。
#pragma once

#include <cmath>
#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class RecoilController {
public:
    // 压枪输出（count 域）：y 为正下压量；x 为拟人微动量（可正可负）
    struct RecoilDelta {
        float y = 0.0f;
        float x = 0.0f;
    };

    // 每帧调用一次，返回本帧压枪量（count 域）。
    // hotkey_bits: 物理鼠标按键位图（左1 右2 中4 侧8 侧16）
    // target_visible: 当前是否有目标（selector 已选中）
    // cfg: 压枪配置（每帧从 RuntimeProfile 快照更新）
    // dt_ms: 帧间隔（ms）
    // px_per_count: 游戏灵敏度标定（gain_y_px_per_count 语义，默认 0.65 px/count）
    RecoilDelta update(uint16_t hotkey_bits, bool target_visible,
                       const RecoilConfig& cfg, float dt_ms, float px_per_count);

    // -----------------------------------------------------------------------
    // BB 三段查表引擎（2026-09-24 移植，见 bb-port/02-压枪与小件.md §1/§2/§3）
    //
    // 与上面的 update() 是**两套互斥模型**：
    //   · update()      ：TTBOX 原速率模型（strength × speed × dt，px/s）。
    //   · update_bb()   ：BB 三段查表（开火时长三等分查 vert/horiz 表）+ 漂移正弦
    //                     + 一阶平滑 + 垂直修正渐变。
    //   由 bb.enabled 决定跑哪套；bb.enabled=false（默认）时整套不跑，行为零变化。
    //
    // 返回 count 域（与 PID 输出同域）：
    //   recoil_x/recoil_y → 压枪位移，AimThread 在 pull_curve 之后注入 scaled_y/x
    //   vert_x/vert_y     → 垂直修正（直接叠加进最终位移，不是乘子）
    // -----------------------------------------------------------------------
    struct BbOutput {
        float recoil_x = 0.0f;
        float recoil_y = 0.0f;
        float vert_x = 0.0f;
        float vert_y = 0.0f;
    };

    BbOutput update_bb(uint16_t hotkey_bits, bool target_visible, float dist_to_target,
                       float target_y, float crosshair_y,
                       const RecoilConfig& cfg, const RecoilBbConfig& bb,
                       const VerticalCorrectionConfig& vcfg,
                       float dt_ms, float px_per_count,
                       float adv_mult, float simple_mult);

    // 目标切换/模型切换等重置：清计时与残差
    void reset() {
        fire_press_ms_ = 0.0f;
        target_lost_ms_ = 0.0f;
        recoil_ramp_ = 0.0f;
        recoil_residual_y_ = 0.0f;
        had_target_ = false;
        // BB 三段查表引擎状态
        bb_clock_ms_ = 0.0f;
        bb_start_ms_ = 0.0f;
        bb_firing_ = false;
        bb_last_x_ = 0.0f;
        bb_last_y_ = 0.0f;
    }

    // 当前是否处于压枪激活状态（供状态 API/日志用）
    bool active() const { return active_; }

    // 开火键是否按住：语义与 update() / update_bb() 内的判据**完全同源**
    // （同一 hotkey_hit、同一 hotkey/hotkey2/hotkey_mode），供其它模块复用。
    // 闭环压枪用它作为"本次开火中"的唯一判据，不另立一套热键语义。
    static bool fire_hotkey_active(uint16_t bits, const RecoilConfig& cfg) {
        return hotkey_hit(bits, cfg.hotkey, cfg.hotkey2, cfg.hotkey_mode);
    }

private:
    // 热键触发：any = 任一命中，all = 同时按下
    static bool hotkey_hit(uint16_t bits, int k1, int k2, int mode) {
        if (mode == 2) {  // all
            const bool a = k1 != 0 && (bits & k1) != 0;
            const bool b = k2 != 0 && (bits & k2) != 0;
            return a && b;
        }
        const bool a = k1 != 0 && (bits & k1) != 0;
        const bool b = k2 != 0 && (bits & k2) != 0;
        return a || b;
    }

    float fire_press_ms_ = 0.0f;      // 本次按住持续时长（ms，松开清零）
    float target_lost_ms_ = 0.0f;     // 目标丢失持续时长（ms，重新见目标清零）
    float recoil_ramp_ = 0.0f;        // 缓入缓出系数 [0,1]
    float recoil_residual_y_ = 0.0f;  // Y 亚像素残差（count）
    bool had_target_ = false;         // 是否曾经见过目标（丢失窗口仅对曾见目标生效）
    bool active_ = false;             // 本帧是否激活压枪

    // ---- BB 三段查表引擎状态 ----
    float bb_clock_ms_ = 0.0f;        // 自维护毫秒时钟（由 dt_ms 累加，不依赖外部时钟源）
    float bb_start_ms_ = 0.0f;        // 本次开火起始时刻（= 热键按下上升沿的 bb_clock_ms_）
    bool bb_firing_ = false;          // 上一帧热键是否按下（用于取上升沿）
    float bb_last_x_ = 0.0f;          // 一阶平滑的上一帧水平输出
    float bb_last_y_ = 0.0f;          // 一阶平滑的上一帧垂直输出
};

// clampRecoilDown：最大下压距离截断（见 02 号 §2.2）。
//   down = 本帧下压量（正 = 向下）；fy = 目标Y - 准星Y（正 = 目标在准星下方多少 px）。
//   max_dist ≤ 0、down ≤ 0、或没有目标（无 fy）时不做限制，原样返回。
inline float clamp_recoil_down(float down, bool has_fy, float fy, float max_dist) {
    if (max_dist <= 0.0f || down <= 0.0f || !has_fy) return down;
    const float budget = fy + max_dist;
    if (budget <= 0.0f) return 0.0f;
    return (down > budget) ? budget : down;
}

// ==================== 实现 ====================

inline RecoilController::RecoilDelta RecoilController::update(
    uint16_t hotkey_bits, bool target_visible,
    const RecoilConfig& cfg, float dt_ms, float px_per_count) {
    RecoilDelta out;
    active_ = false;
    if (!cfg.enabled) return out;

    const float dt = (dt_ms > 0.0f) ? (dt_ms * 0.001f) : 0.0f;  // s
    const bool firing = hotkey_hit(hotkey_bits, cfg.hotkey, cfg.hotkey2, cfg.hotkey_mode);

    // ---- 触发计时 ----
    if (firing) fire_press_ms_ += dt_ms;
    else fire_press_ms_ = 0.0f;

    // 延迟触发：按住超过 trigger_delay_ms 才算"开火中"
    bool pressing = firing;
    if (cfg.trigger_delay_enabled && cfg.trigger_delay_ms > 0.0f) {
        pressing = firing && (fire_press_ms_ >= cfg.trigger_delay_ms);
    }

    // ---- 目标门控 ----
    bool target_ok = !cfg.only_when_target_visible || target_visible;
    if (target_visible) { target_lost_ms_ = 0.0f; had_target_ = true; }
    else target_lost_ms_ += dt_ms;

    // 目标丢失保持窗口：仅当"曾经见过目标"时，丢失 target_lost_release_ms 内仍压。
    // （从未见过目标 → 窗口无效，防空压；压枪语义一致）
    if (cfg.only_when_target_visible && !target_visible && had_target_) {
        const float keep_ms = (cfg.target_lost_release_ms >= 0.0f) ? cfg.target_lost_release_ms : 0.0f;
        target_ok = target_lost_ms_ <= keep_ms;
    }

    // ---- 缓入缓出 ramp ----
    const bool want = pressing && target_ok;
    if (want) {
        recoil_ramp_ += dt * cfg.humanize_curve_strength * 4.0f;
        if (recoil_ramp_ > 1.0f) recoil_ramp_ = 1.0f;
    } else {
        recoil_ramp_ -= dt * cfg.humanize_curve_strength * 4.0f;
        if (recoil_ramp_ < 0.0f) recoil_ramp_ = 0.0f;
    }

    // ---- 下压量生成 ----
    if (recoil_ramp_ <= 0.0f) return out;

    const float rate_px_per_s = cfg.strength * cfg.speed;  // px/s
    if (rate_px_per_s <= 0.0f) return out;

    // px → count：复用标定响应（gain_y_px_per_count 语义，默认 0.65 px/count）
    const float ppc = (px_per_count > 0.05f) ? px_per_count : 0.65f;
    const float base_count = rate_px_per_s * dt / ppc;
    const float ramp_count = base_count * recoil_ramp_;

    // ---- X 轴微动：★ 2026-09-29 已删 ----
    //   原实现在压枪时叠加固定频率正弦 X 微动（humanize_jitter_px，默认 0.25px、
    //   8Hz），属 7 套重复抖动机制之一 ⇒ 按业主「合不了就删」口径删除。
    //   humanize_jitter_px / humanize_jitter_frequency 字段保留在结构体里（老配置可解析），
    //   本模块不再消费。out.x 保持默认 0。

    // ---- Y 残差累计（与 PID remainder 同域，不丢精度）----
    recoil_residual_y_ += ramp_count;
    out.y = recoil_residual_y_;
    recoil_residual_y_ = 0.0f;
    active_ = true;
    return out;
}

// ==================== BB 三段查表引擎实现 ====================

inline RecoilController::BbOutput RecoilController::update_bb(
    uint16_t hotkey_bits, bool target_visible, float dist_to_target,
    float target_y, float crosshair_y,
    const RecoilConfig& cfg, const RecoilBbConfig& bb,
    const VerticalCorrectionConfig& vcfg,
    float dt_ms, float px_per_count, float adv_mult, float simple_mult) {
    BbOutput out;
    if (!bb.enabled) {
        // 关掉时把引擎状态一并清干净，避免重新打开时残留旧的计时/平滑值
        bb_clock_ms_ = 0.0f;
        bb_start_ms_ = 0.0f;
        bb_firing_ = false;
        bb_last_x_ = 0.0f;
        bb_last_y_ = 0.0f;
        return out;
    }
    if (dt_ms > 0.0f) bb_clock_ms_ += dt_ms;

    const bool firing = hotkey_hit(hotkey_bits, cfg.hotkey, cfg.hotkey2, cfg.hotkey_mode);
    if (firing && !bb_firing_) bb_start_ms_ = bb_clock_ms_;  // 上升沿重置开火计时
    if (!firing) {
        bb_firing_ = false;
        bb_last_x_ = 0.0f;
        bb_last_y_ = 0.0f;
        return out;  // 热键没按：不压、不修正
    }
    bb_firing_ = true;

    // [3] 开火延迟
    const float el = bb_clock_ms_ - bb_start_ms_;
    if (el < bb.delay_ms) return out;

    // [4] 目标门：有目标且未超距离；或 允许"无目标也压"
    const bool has_target = target_visible;
    const bool ok = (has_target && (bb.distance_limit <= 0.0f || dist_to_target <= bb.distance_limit)) ||
                    (bb.no_target_always && !has_target);
    if (!ok) return out;

    // px → count 换算（复用 gain_y_px_per_count 语义）
    const float ppc = (px_per_count > 0.05f) ? px_per_count : 0.65f;
    const bool has_fy = has_target;
    const float fy = target_y - crosshair_y;

    // ---- [5..8] 三段查表 ----
    int pi = bb.preset;
    if (pi < 1) pi = 1;
    if (pi > 3) pi = 3;
    const int idx = pi - 1;
    const float total_time = bb.preset_total_time_ms[idx];
    if (total_time > 0.0f) {
        int seg = static_cast<int>(std::floor(el / (total_time / 3.0f))) + 1;
        if (seg > 3) seg = 3;
        if (seg < 1) seg = 1;
        if (el > total_time) seg = 3;  // 超出总时长固定用最后一段（仍持续压）
        float v = bb.preset_vert[idx][seg - 1] * bb.global_vert;
        float h = bb.preset_horiz[idx][seg - 1] * bb.global_horiz;

        // [9] 水平漂移正弦：★ 2026-09-29 已删
        //   原实现叠加固定频率正弦水平漂移（drift_amplitude 0.20、drift_freq），
        //   属 7 套重复抖动机制之一 ⇒ 删除。drift_enabled / drift_amplitude /
        //   drift_freq 字段保留在结构体里，本模块不再消费。

        // Y 路屏蔽（进阶压枪）
        if (bb.y_suppress_enabled) v *= bb.y_suppress_strength;

        // 最大下压截断（相对实际准星位置）
        v = clamp_recoil_down(v, has_fy, fy, bb.max_down_distance);

        // [10] 一阶平滑（自身回路）
        if (bb.smooth > 0.0f) {
            const float s = (bb.smooth > 0.99f) ? 0.99f : bb.smooth;
            h = h * (1.0f - s) + bb_last_x_ * s;
            v = v * (1.0f - s) + bb_last_y_ * s;
            bb_last_x_ = h;
            bb_last_y_ = v;
        }

        // [11] 扳机联动倍率
        const float am = (adv_mult > 0.0f) ? adv_mult : 1.0f;
        out.recoil_x = h * am / ppc;
        out.recoil_y = v * am / ppc;
    }

    // ---- 垂直修正 + 力度渐变（见 02 号 §3.3）----
    if (vcfg.enabled) {
        const bool tgt_ok = vcfg.no_target || has_target;
        const float v_el = el - vcfg.delay_ms;
        if (tgt_ok && v_el > 0.0f) {
            float vm = vcfg.strength;
            const float hm = vcfg.horiz;

            // 三档渐变互斥：都开则顺序靠前者生效
            bool ramp_on = false;
            float r_dur = 0.0f, r_start = 1.0f, r_mid = 1.0f, r_end = 1.0f;
            if (vcfg.ramp1_enabled) {
                ramp_on = true;
                r_dur = vcfg.ramp1_duration_ms;
                r_start = vcfg.ramp1_start;
                r_mid = vcfg.ramp1_middle;
                r_end = vcfg.ramp1_end;
            } else if (vcfg.ramp2_enabled) {
                ramp_on = true;
                r_dur = vcfg.ramp2_duration_ms;
                r_start = vcfg.ramp2_start;
                r_mid = vcfg.ramp2_middle;
                r_end = vcfg.ramp2_end;
            } else if (vcfg.ramp3_enabled) {
                ramp_on = true;
                r_dur = vcfg.ramp3_duration_ms;
                r_start = vcfg.ramp3_start;
                r_mid = vcfg.ramp3_middle;
                r_end = vcfg.ramp3_end;
            }
            if (ramp_on && r_dur > 0.0f) {
                float mult = r_end;
                if (v_el < r_dur) {
                    const float t = v_el / r_dur;
                    mult = (t < 0.5f) ? (r_start + (r_mid - r_start) * (t / 0.5f))
                                      : (r_mid + (r_end - r_mid) * ((t - 0.5f) / 0.5f));
                }
                vm *= mult;
            }

            // Y 路屏蔽（简易/自动压枪自己的那组键）
            if (vcfg.y_suppress_enabled) vm *= vcfg.y_suppress_strength;

            vm = clamp_recoil_down(vm, has_fy, fy, vcfg.max_down_distance);
            // 扳机联动倍率（简易压枪用的那一路，开关是 with_simple_recoil）
            const float sm = (simple_mult > 0.0f) ? simple_mult : 1.0f;
            out.vert_x = hm * sm / ppc;
            out.vert_y = vm * sm / ppc;
        }
    }

    return out;
}

}  // namespace ttbox::core::aim
