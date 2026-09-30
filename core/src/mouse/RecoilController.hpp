// RecoilController.hpp — 压枪引擎（recoil assist）· 2026-09-30 按 yu 重做
//
// 小白理解：
//   按住开火键时枪会自己往上抬，准星不动但画面在跑。压枪 = 开火期间自动往下拉一点，
//   把弹着点按回原来的位置。
//
// 为什么重写（业主 2026-09-30 令：「压枪太臃肿、参数太多，算法有问题、逻辑也不对，
// 对照 yu 改」）：
//   旧版把「TTBOX 速率模型 + BB 三段查表 + 垂直修正渐变 + 开火期闭环」四套并在一起跑，
//   参数 50+ 个、互相打架，其中：
//     · 缓入 ramp：开火后从 0 爬到 1（≈200ms）⇒ **最需要压的前几发反而没压**，逻辑是反的；
//     · 残差处理是假的：`out.y = residual_; residual_ = 0;` —— 输出即清零，等于没结转；
//     · 闭环与瞄准 PID 抢同一个执行器 ⇒ v1 实机「乱晃」（业主已裁定删）。
//
// yu 的做法（aiassistance_daemon 汇编级还原，见
// .workbuddy/artifacts/yu-压枪深挖与TTBOX方案-2026-09-30.md §1）：
//   每帧拉量 = 3 · strength · speed · ramp · dt        ← 纯 Y，X 恒 0，无枪械表
//   ramp     = 1                                      （开火中，全量）
//            = 1 − curve_strength · smoothstep(t/80ms)（释放后渐出，t = 释放后时长）
//   没按过键 / 从没拿到有效量测 ⇒ 一帧都不输出，也不启动渐出（渐出窗最多 80ms）
//   钳制      = 累计补偿量夹在 roi_h 内
//   门控      = 开火按住 + **目标量测有效**；目标丢失后 target_lost_release_ms 内继续跑
//   残差      = 独立结转（与瞄准 remainder 互不污染）
//
// 关键：yu 压枪「稳」不在于公式（公式就是上面那行），而在于**它的输入端永远拿不到坏框**
// ——跟踪器量测门控（框突变/贴边/尺寸非法 → 拒绝量测 + 保持上一帧）先跑，
// 压枪只吃有效量测。所以本引擎的第二个入参是 `measurement_valid`，不是裸的 target_visible。
//
// 本引擎只算量，不发命令：算出的 y（count 域）由 AimThread 在
// deadzone 之前注入，统一走 deadzone → remainder → int16 → 热键安全门。
#pragma once

#include <cmath>
#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class RecoilController {
public:
    // 压枪输出（count 域）：y 为正下压量；x 恒 0（yu 的压枪是单轴引擎）
    struct RecoilDelta {
        float y = 0.0f;
        float x = 0.0f;
    };

    // 每帧调用一次，返回本帧压枪量（count 域）。
    // hotkey_bits      : 物理鼠标按键位图（左1 右2 中4 侧8 侧16）
    // measurement_valid: 本帧是否存在**有效量测**（非 hold-previous / 非贴边 / 尺寸合法）。
    //                    yu 语义：只有有效量测才让压枪推进；无效量测按丢失处理。
    // cfg              : 压枪配置（每帧从 RuntimeProfile 快照更新）
    // dt_ms            : 帧间隔（ms）
    // px_per_count     : 游戏灵敏度标定（gain_y_px_per_count 语义，默认 0.65 px/count）
    RecoilDelta update(uint16_t hotkey_bits, bool measurement_valid,
                       const RecoilConfig& cfg, float dt_ms, float px_per_count);

    // 目标切换/模型切换等重置：清计时、累计量与残差
    void reset() {
        fire_press_ms_ = 0.0f;
        lost_ms_ = 0.0f;
        release_ms_ = 0.0f;
        acc_px_ = 0.0f;
        residual_count_ = 0.0f;
        was_firing_ = false;
        had_target_ = false;
        ramping_ = false;
        active_ = false;
    }

    // 当前是否处于压枪激活状态（供状态 API/日志用）
    bool active() const { return active_; }

    // 本次开火累计下压量（px，供遥测）
    float acc_px() const { return acc_px_; }

    // 开火键是否按住：语义与 update() 内判据**完全同源**（同一 hotkey_hit、
    // 同一 hotkey/hotkey2/hotkey_mode），供其它模块复用。
    static bool fire_hotkey_active(uint16_t bits, const RecoilConfig& cfg) {
        return hotkey_hit(bits, cfg.hotkey, cfg.hotkey2, cfg.hotkey_mode);
    }

    // yu 的释放渐出核：C¹ 连续（两端一阶导为 0），t 已归一化到 [0,1]
    static float smoothstep(float t) {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t * t * (3.0f - 2.0f * t);
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
    float lost_ms_ = 0.0f;            // 目标量测无效持续时长（ms，有效时清零）
    float release_ms_ = 0.0f;         // 释放后经过时长（ms，用于 80ms 渐出）
    float acc_px_ = 0.0f;             // 本次开火累计下压量（px，受 roi_h 钳制）
    float residual_count_ = 0.0f;     // 独立亚像素残差（count）
    bool was_firing_ = false;         // 上一帧是否开火（取上升沿，重置累计量）
    bool had_target_ = false;         // 是否曾经有过有效量测（丢失窗口仅对曾见目标生效）
    bool ramping_ = false;            // 是否进入过开火态：没进过就既不压也不渐出
    bool active_ = false;             // 本帧是否激活压枪
};

// ==================== 实现 ====================

inline RecoilController::RecoilDelta RecoilController::update(
    uint16_t hotkey_bits, bool measurement_valid,
    const RecoilConfig& cfg, float dt_ms, float px_per_count) {
    RecoilDelta out;
    active_ = false;
    if (!cfg.enabled) return out;

    const float dt = (dt_ms > 0.0f) ? (dt_ms * 0.001f) : 0.0f;  // s
    const bool firing = hotkey_hit(hotkey_bits, cfg.hotkey, cfg.hotkey2, cfg.hotkey_mode);

    // ---- 触发计时 ----
    if (firing) fire_press_ms_ += dt_ms;
    else fire_press_ms_ = 0.0f;

    // 开火上升沿：本次开火重新起算累计下压量与残差（yu 的 roi 是对"这一轮喷射"的钳制）
    if (firing && !was_firing_) {
        acc_px_ = 0.0f;
        release_ms_ = 0.0f;
    }
    was_firing_ = firing;

    // 延迟触发：按住超过 trigger_delay_ms 才算"开火中"（防单点误触）
    bool pressing = firing;
    if (cfg.trigger_delay_enabled && cfg.trigger_delay_ms > 0.0f) {
        pressing = firing && (fire_press_ms_ >= cfg.trigger_delay_ms);
    }

    // ---- 目标量测门控（yu: only_when_target_visible + target_lost_release_ms）----
    if (measurement_valid) { lost_ms_ = 0.0f; had_target_ = true; }
    else lost_ms_ += dt_ms;

    bool target_ok = !cfg.only_when_target_visible || measurement_valid;
    if (cfg.only_when_target_visible && !measurement_valid && had_target_) {
        // 曾见目标：量测无效的 release 窗口内继续压（yu [0,3000]ms）
        const float keep_ms = (cfg.target_lost_release_ms >= 0.0f) ? cfg.target_lost_release_ms : 0.0f;
        target_ok = lost_ms_ <= keep_ms;
    }

    // ---- 释放渐出（yu：开火中 ramp=1，无缓入；只在释放段平滑衰减）----
    // ★ ramping_ 是「这一轮真的压过」的标记：没按过键、或从来没拿到有效量测时，
    //   既不压也不启动渐出（否则会凭空输出一段无来源的下压量）。
    const bool want = pressing && target_ok;
    float ramp;
    if (want) {
        release_ms_ = 0.0f;
        ramping_ = true;
        ramp = 1.0f;
    } else {
        if (!ramping_) return out;
        release_ms_ += dt_ms;
        // t ∈ [0,1]：80ms 内走完整个 smoothstep（yu 常数 0x3da3d70a = 0.08s）
        const float t = release_ms_ / 80.0f;
        if (t >= 1.0f) { ramping_ = false; return out; }  // 渐出窗最多 80ms（cs=0 也停）
        const float cs = (cfg.curve_strength < 0.0f) ? 0.0f
                         : (cfg.curve_strength > 1.0f ? 1.0f : cfg.curve_strength);
        ramp = 1.0f - cs * smoothstep(t);
        // release 窗口外（没有目标保持窗接力）就不再输出
        if (!target_ok && lost_ms_ > ((cfg.target_lost_release_ms > 0.0f) ? cfg.target_lost_release_ms : 0.0f)) {
            ramp = 0.0f;
        }
        if (ramp <= 0.0f) { ramping_ = false; return out; }
    }

    // ---- yu 速率公式：rate = 3 × strength × speed（px/s），纯 Y ----
    const float strength = (cfg.strength < 0.0f) ? 0.0f : (cfg.strength > 300.0f ? 300.0f : cfg.strength);
    float speed = cfg.speed;
    if (!(speed >= 0.1f)) speed = 0.1f;      // 出界（含 NaN）一律钉下限，对齐 yu
    if (speed > 3.0f) speed = 3.0f;
    const float rate_px_s = 3.0f * strength * speed;
    if (rate_px_s <= 0.0f || dt <= 0.0f) return out;

    // ---- ROI 钳制：累计下压量不超过 roi_h（yu 的 roi 上限保护）----
    const float roi = (cfg.roi_h > 0.0f) ? cfg.roi_h : 0.0f;
    float step_px = rate_px_s * dt * ramp;
    if (roi > 0.0f) {
        const float room = roi - acc_px_;
        if (room <= 0.0f) { active_ = true; return out; }  // 已顶到 ROI：不再出量
        if (step_px > room) step_px = room;
    }
    acc_px_ += step_px;

    // ---- px → count，独立亚像素残差结转（不丢小数，也不与瞄准 remainder 打架）----
    const float ppc = (px_per_count > 0.05f) ? px_per_count : 0.65f;
    const float want_count = step_px / ppc + residual_count_;
    const float whole = std::floor(want_count + 0.5f);   // 四舍五入到整数 count
    out.y = whole;
    out.x = 0.0f;                                        // yu：压枪无横向
    residual_count_ = want_count - whole;
    active_ = true;
    return out;
}

}  // namespace ttbox::core::aim
