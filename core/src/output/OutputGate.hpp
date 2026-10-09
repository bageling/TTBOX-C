// OutputGate.hpp — 输出前的统一放行判据（**单一权威源**）
//
// ★ 2026-09-25 抽出来的原因：此前 `AiboxHidOutput::send` 与 `IOutputBackend::gate_allows`
//   **各写了一遍**同一个判据，然后漂移了：
//     · aibox 侧没有「标定模式」豁免 ⇒ mouse.enabled=false 时跑标定一个 count 都发不出去；
//     · 两侧都在「有配置源但按键源没绑」时直接放行（fail-open），热键二次防线只剩 AimThread 一层。
//   两边注释里还互相写着「判定顺序完全一致」—— 靠注释保证一致是不可靠的，抽成一处才是根治。
//
// 小白理解：这是鼠标指令出门前的最后一道安检。任何一步拿不到必要信息，就一律不放行，
// 绝不"猜"一个默认行为（猜错就是"没按热键也在瞄准"）。
#pragma once

#include <atomic>
#include <cstdint>

#include "model/RuntimeProfile.hpp"
#include "mouse/MouseTypes.hpp"

namespace ttbox::core::output {

// 闸门的三个输入。两类后端（OutputBackend / AiboxHidOutput）的成员完全同型，
// 所以能共用同一份判据，不会再各自漂移。
struct OutputGateInputs {
    bool enabled = false;                            // 后端静态总闸（kill switch）
    RuntimeConfig* config_source = nullptr;          // 运行期配置（nullptr = 无）
    std::atomic<uint16_t>* button_source = nullptr;  // 物理按键位图（nullptr = 无）
};

// 统一放行判据（fail-closed：任何一步缺必要信息都拒绝，不猜默认键）。
//
// 判定顺序：
//   1) 静态总闸未开                         → 拒
//   2) 无配置源（不管有没有按键源）         → 拒（无从得知用户热键）
//   3) 标定模式                             → 放行（标定线程自己注入运动帧，物理鼠标不参与）
//   4) mouse.enabled 关                     → 拒
//   5) 放行掩码 = 所有瞄准档位键位并集；为 0（配置缺失）→ 拒
//   6) 按键源没绑                           → 拒（★ 此前这里被放过）
//   7) 按键位图与掩码无交集                 → 拒
inline bool output_gate_allows(const OutputGateInputs& in) {
    if (!in.enabled) return false;
    if (!in.config_source) return false;  // 无配置源 → 无从得知用户热键，拒绝（不猜）
    auto p = in.config_source->snapshot();
    if (!p) return false;
    // 标定模式优先：无视 mouse.enabled 与热键，标定帧必须能出门。
    if (p->mouse.calibrating) return true;
    if (!p->mouse.enabled) return false;
    // 放行掩码 = **所有档位键位的并集**：只看某一档会把其它档的键位整条拦掉
    // （表现为"换个键就不瞄了"）。
    const uint16_t mask = aim::aim_hotkey_mask(p->mouse);
    if (mask == 0) return false;  // 配置缺失 → 禁止注入
    // ★ 按键源没绑 = 不知道玩家按了什么 ⇒ 拒绝。此前两侧都在这里 fall-through 放行了，
    //   等于把热键这道防线整个拆掉（只剩 AimThread 一层）。
    if (!in.button_source) return false;
    if ((in.button_source->load(std::memory_order_acquire) & mask) == 0) return false;
    return true;
}

}  // namespace ttbox::core::output
