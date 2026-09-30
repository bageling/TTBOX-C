// TriggerController.hpp — 自动扳机（只剩 BB 扳机 2.0；v7.26 已于 2026-09-29 删除）
//
// 对标来源：竞品 BB_927 内部会员版 `13_auto_trigger.lua` / `14_bb_trigger.lua`，
// 算法结构与标定值见 `.workbuddy/artifacts/bb-port/01-选靶与扳机.md` §2/§3。
// 铁律：只取结构与标定值，实现全部自己写，不搬运一行 Lua。
//
// 设计要点
//   · **决策与注入分离**：本模块只产出 TriggerCmd（要不要开火、点哪个键、点几次），
//     真正的鼠标点击由 AimThread 拿到命令后调 OutputBackend::mouse_click 注入。
//     这样扳机逻辑可以脱离硬件单测。
//   · 只跑 trigger2（BB 2.0）；没开时 update() 立刻返回空命令 ⇒
//     输出链与本模块加入前逐字节一致。
//   · **外部时钟**：全部时间判断用调用方传入的 now_ms，模块内不自取时间（可测）。
//   · **可播种随机**：fire_random 抖动用内部 xorshift，seed 可指定 ⇒ 单测确定性。
//   · 依赖外部的两个判定（保持本模块纯净）：
//       center_covered   —— 中心点是否被任一检测框覆盖（crosshair_check 用）
//       stop_detect_found—— 中心是否出现指定准星颜色（BB2 急停检测用，颜色识别在采集侧）
//
// ★ 键位一律是**位掩码**，与 MouseProfile.aim_hotkey 同域：
//   1=left 2=right 4=middle 8=back 16=forward；BB 编号 1/2/3/5/6 → 0x01/0x02/0x04/0x08/0x10。
//
// TTBOX 文件说明
//
// 文件：TriggerController.hpp
//
// 作用：
//   根据"有没有锁定目标、目标离准星多远、热键按没按"决定要不要自动开火。
//
// 小白理解：
//   它像一个副手：你说"按住侧键、目标进圈了"，它就帮你扣扳机；目标跑了就松手。
//   它自己不碰鼠标，只张嘴说"现在该开枪"，真正扣扳机的是 AimThread。
//
// 注意：
//   算法默认关着，不开就等于这个文件不存在。
#pragma once

#include <cmath>
#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

// 一帧的扳机决策（由 AimThread 消费）
struct TriggerCmd {
    bool fire = false;             // 本帧是否开火
    uint8_t button = 0x00;         // 要点的键位掩码（0 = 不点击）
    int count = 1;                 // 连点次数
    float press_duration_ms = 0.0f; // 按下保持时长
    // 压枪联动（2026-09-30 收敛：yu 的 auto_trigger_spray_assist 只有一路）
    bool recoil_simple = false;   // 扳机连发期间自动附带压枪（AimThread 据此补开火键位）
    // 2026-09-29：recoil_y_offset_px（框高 × trigger.y_offset）随 v7.26 一并删除 ——
    //   2.0 没有 y_offset 字段，留着就是一个恒为 0 的死字段。
    // 这一枪是哪套扳机打的：移动节流（trigger2.move_throttle_frames）只对 2.0 生效。
    // v7.26 删除后此标记恒为 true，保留字段是为了不动 AimThread 的判定语句。
    bool fired_by_trigger2 = false;

    bool any() const { return fire; }
};

// 一帧的输入（全部由调用方准备好，模块不碰硬件）
struct TriggerInput {
    uint32_t now_ms = 0;            // 当前毫秒时钟
    float dt_ms = 16.667f;          // 距上一帧的间隔（ms）
    bool has_target = false;        // 本帧是否有锁定目标
    float dtt_px = 0.0f;            // 锁定目标到准星的距离（px）
    float target_conf = 0.0f;       // 锁定目标置信度
    uint16_t hotkey_bits = 0;       // 当前物理按键位图（位掩码，见文件头）
    bool center_covered = true;     // 准星中心是否被任一检测框覆盖
    bool stop_detect_found = true;  // 中心是否命中指定准星颜色（急停检测）
};

// 2026-09-29：自动扳机 v7.26（AutoTrigger）已整段删除 —— 业主裁定「自动开火只留 2.0」。
//   两套状态机字段高度重合，同时开时 v7.26 优先，只会让人选错。
//   旧配置里残留的 `mouse.trigger` 段会被 RuntimeProfile 忽略。
//   `key_down`（位掩码判定，mask==0 视为常满足）被 2.0 复用，已搬到 AutoTrigger2 里。

// BB 扳机 2.0（组合键按住即连发；首枪过后不限距离）
class AutoTrigger2 {
public:
    explicit AutoTrigger2(uint32_t seed = 0x87654321u) : rng_(seed ? seed : 1u) {}

    void reset() {
        activated_ = false;
        fired_ = false;
        has_first_enter_ = false;
        has_lost_ = false;
        has_fire_ = false;
        precision_count_ = 0;
        first_enter_ms_ = 0;
        last_fire_ms_ = 0;
        next_fire_ms_ = 0;
        lost_ms_ = 0;
    }
    // 只复位"首枪态"（目标丢失超时后重新走首枪门）
    void reset_first_shot() {
        fired_ = false;
        has_first_enter_ = false;
        has_lost_ = false;
        has_fire_ = false;
        precision_count_ = 0;
        next_fire_ms_ = 0;
    }

    TriggerCmd update(const Trigger2Config& cfg, const TriggerInput& in) {
        TriggerCmd cmd;
        if (!cfg.enabled) {
            reset();
            return cmd;
        }
        const bool combo = key_down(in.hotkey_bits, cfg.key1) &&
                           key_down(in.hotkey_bits, cfg.key2);
        if (!combo) {
            reset();
            return cmd;
        }
        activated_ = true;

        // 急停检测：中心没有准星颜色则本帧禁射（打狙急停）
        if (cfg.stop_detect_enabled && !in.stop_detect_found) return cmd;

        if (!fired_) {
            if (!in.has_target) {
                precision_count_ = 0;
                return cmd;
            }
            if (cfg.precision_enabled) {
                precision_count_ = (in.dtt_px < cfg.precision_range) ? precision_count_ + 1 : 0;
                if (precision_count_ < cfg.precision_frames) return cmd;
            }
            if (in.dtt_px < cfg.first_err) {
                if (!has_first_enter_) {
                    first_enter_ms_ = in.now_ms;
                    has_first_enter_ = true;
                }
                if (static_cast<float>(elapsed(in.now_ms, first_enter_ms_)) >= cfg.first_delay) {
                    return make_fire(cfg, in, cmd);
                }
            }
            return cmd;
        }

        // 已开首枪：有锁定就按间隔继续，不再校验距离
        if (in.has_target) {
            if (!has_fire_ || in.now_ms >= next_fire_ms_) return make_fire(cfg, in, cmd);
            return cmd;
        }
        // 目标丢失：超 retarget_reset_ms 后重置首枪态
        if (cfg.retarget_reset_ms > 0.0f) {
            if (!has_lost_) {
                lost_ms_ = in.now_ms;
                has_lost_ = true;
            } else if (static_cast<float>(elapsed(in.now_ms, lost_ms_)) >= cfg.retarget_reset_ms) {
                reset_first_shot();
            }
        }
        return cmd;
    }

    bool activated() const { return activated_; }
    bool fired() const { return fired_; }
    int precision_count() const { return precision_count_; }

    // 位掩码判定（mask == 0 表示「该键不参与判定」⇒ 恒真）。
    // 2026-09-29 从已删除的 AutoTrigger(v7.26) 搬过来 —— 原本是两套扳机共用的静态方法。
    static bool key_down(uint16_t bits, uint8_t mask) {
        return mask == 0 || (bits & static_cast<uint16_t>(mask)) != 0;
    }

private:
    static uint32_t elapsed(uint32_t now, uint32_t then) {
        return now >= then ? (now - then) : (then - now);
    }
    float rand_unit() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(rng_ & 0xFFFFFFu) / static_cast<float>(0x1000000u);
    }
    float rand_pm(float amp) { return (rand_unit() * 2.0f - 1.0f) * amp; }

    TriggerCmd make_fire(const Trigger2Config& cfg, const TriggerInput& in, TriggerCmd cmd) {
        cmd.fire = true;
        cmd.button = cfg.fire_button;
        cmd.count = cfg.fire_count > 1 ? cfg.fire_count : 1;
        cmd.press_duration_ms = cfg.press_duration;
        cmd.fired_by_trigger2 = true;
        if (cfg.with_simple_recoil) cmd.recoil_simple = true;
        fired_ = true;
        last_fire_ms_ = in.now_ms;
        // 连发间隔单位是帧，按 600Hz 折算成 ms（对齐 BB 宿主的 600Hz 刷新）
        const float jitter = cfg.fire_random > 0.0f ? rand_pm(cfg.fire_random) : 0.0f;
        const float frames = std::fmax(1.0f, cfg.fire_interval + jitter);
        next_fire_ms_ = in.now_ms + static_cast<uint32_t>(frames * (1000.0f / 600.0f) + 0.5f);
        has_fire_ = true;
        return cmd;
    }

    bool activated_ = false;
    bool fired_ = false;
    bool has_first_enter_ = false;
    bool has_lost_ = false;
    bool has_fire_ = false;
    int precision_count_ = 0;
    uint32_t first_enter_ms_ = 0;
    uint32_t last_fire_ms_ = 0;
    uint32_t next_fire_ms_ = 0;
    uint32_t lost_ms_ = 0;
    uint32_t rng_;
};

// 扳机门面。2026-09-29 起只剩 2.0 一套（v7.26 已整段删除）——
// 门面本身保留，是为了不动 AimThread 的调用点（`trigger_.update(...)` / `auto_trigger2()`）。
class TriggerController {
public:
    TriggerCmd update(const MouseProfile& mp, const TriggerInput& in) {
        return auto2_.update(mp.trigger2, in);
    }
    void reset() { auto2_.reset(); }

    const AutoTrigger2& auto_trigger2() const { return auto2_; }

private:
    AutoTrigger2 auto2_;
};

}  // namespace ttbox::core::aim
