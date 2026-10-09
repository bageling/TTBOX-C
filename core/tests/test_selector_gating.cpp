// test_selector_gating.cpp — V1.0.11：选靶「量测门控（新骨架对齐）」单元测试。
//
// 定障依据（板端 2026-09-29 完整录制 341,859 帧，见
// .workbuddy/artifacts/yu-选靶与落点抖动方案-2026-09-30.md）：
//   · 稳态（已对准）误差每秒过零 3.67 次、帧间只动 0.13px ⇒ 幅度小、频率高，就是"瞄上后还晃"；
//   · corr(框高极差, 落点极差)=0.744，落点对 y1 敏感度 0.69（对 y2 只 0.31）⇒ 是框顶在摆；
//   · 落点尖峰（>20px，占 1.26% 帧）里 88.6% 是**突跳**（跳变量 ÷ 邻帧位移中位 ≥2）
//     ⇒ 连续性判据能拦，且不误杀跟枪（离线回放 p90 抖动 −38.2%、互相关滞后 0 帧）。
//
// 本文件锁死量测门控在新骨架（单层打分 + 单一 IoU 判定 + 锁定保持）下的真实行为：
//   (1) 突跳被 hold：输出沿用上一帧框（holding_previous）＋ 遥测计数增长；
//   (2) 连续快移不误杀：掉锁重获靠距离复用保 id，尾部 [跟上/滞后] 交替；
//   (3) 连续坏量测认输：hold 满 kMeasMaxHold=10 帧必须放行（防把锁定锁死）；
//   (4) 空帧后同位置重现 ⇒ id 延续，continuity 恒 false、reason 统一 kScore；
//   (5) 遥测累计量 reset 清零。
// 已删除（旧骨架行为，select() 不再消费）：尺寸一致性 track_size_ratio 候选过滤、
//   continuity 标记、切靶确认窗（switch_confirm_frames）。
#include <cmath>
#include <vector>

#include "mouse/TargetSelector.hpp"
#include "test_util.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

namespace {

TargetSelectorConfig make_cfg() {
    TargetSelectorConfig c;
    c.fov_range = 1.0f;      // 搜索半径 = min(640,480)/2 = 240px，中心 (320,240)
    c.confidence = 0.25f;
    c.roi_w = 640;
    c.roi_h = 480;
    c.lost_grace_ms = 30.0f;
    // 本文件测的是 V1.0.11 的新机制 ⇒ 先把已有的两种切靶防抖关掉，
    // 否则它们也会让结果变成 invalid，判不出到底是谁挡的。
    c.switch_cooldown_ms = 0.0f;
    c.switch_hysteresis = 0.0f;
    return c;
}

DetectionBox box(float cx, float cy, float w, float h) {
    DetectionBox b;
    b.x1 = cx - w * 0.5f; b.y1 = cy - h * 0.5f;
    b.x2 = cx + w * 0.5f; b.y2 = cy + h * 0.5f;
    b.score = 0.9f;
    b.class_id = 0;
    return b;
}

float center_x(const DetectionBox& b) { return (b.x1 + b.x2) * 0.5f; }

constexpr float kEps = 0.05f;

}  // namespace

// (1) 突跳被 hold + (3) hold 满上限认输
// 前 5 帧走 2px/帧 正常移动把局部速度基准建起来（d_hist=[2,2,2,2] ⇒ 阈值 max(8, 3×2)=8px），
// 第 6 帧同一尺寸的框突然跳 +12px ⇒ 判坏、沿用上一帧框；连续喂同一个坏量测到第 11 帧才放行。
// ★ 新骨架：单一「还是他吗」判定（同类别 + IoU≥0.6）跑在门控之前。60px 宽框能容忍的
//   同框位移上限 = 0.25×60 = 15px。旧用例 +72px 会让 IoU 直接跌破 0.6 ⇒ 根本走不到门控，
//   而是掉进「锁定保持/换目标」；改成 +12px（IoU=(60-12)/(60+12)=0.667）才真正测到门控。
TEST(selector_measure_jump_held_then_gives_in) {
    TargetSelector sel;
    auto cfg = make_cfg();
    for (int i = 0; i < 5; ++i) {
        const float x = 320.0f + 2.0f * static_cast<float>(i);
        auto r = sel.select({box(x, 240, 60, 120)}, cfg, static_cast<uint32_t>(i * 7));
        CHECK(r.valid);
        CHECK(!r.held);   // 正常小位移一律放行
    }
    // 第 6 帧：突跳（+12px ≫ 阈值 8px，但仍在 IoU≥0.6 内）
    auto r = sel.select({box(340, 240, 60, 120)}, cfg, 42);
    CHECK(r.valid);
    CHECK(r.held);                                     // 量测被判坏
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(TargetSelection::Reason::kTrackLock));
    CHECK(std::abs(center_x(r.box) - 328.0f) < kEps);  // 输出 = 上一帧框（未更新）
    CHECK_EQ(sel.selector_holds_total(), 1ull);
    // 继续喂同一个坏量测：hold 上限 kMeasMaxHold=10 ⇒ 第 2~10 次一律沿用旧框
    for (int i = 0; i < 9; ++i) {
        auto rh = sel.select({box(340, 240, 60, 120)}, cfg, static_cast<uint32_t>(49 + i * 7));
        CHECK(rh.valid);
        CHECK(rh.held);
        CHECK(std::abs(center_x(rh.box) - 328.0f) < kEps);
    }
    CHECK_EQ(sel.selector_holds_total(), 10ull);
    // 第 11 帧：认输放行 —— 门控会自愈，不会把锁定锁死
    auto r2 = sel.select({box(340, 240, 60, 120)}, cfg, 130);
    CHECK(r2.valid);
    CHECK(!r2.held);
    CHECK(std::abs(center_x(r2.box) - 340.0f) < kEps);
}

// (2) 连续快移不误杀：12px/帧（> 绝对下限 8px），60px 宽框。
// ★ 修复后：IoU「还是他吗」判定改用每帧更新的 ref_box（不经过门控冻结），
//   候选相对参考框永远只差本帧的 12px ⇒ IoU=(60-12)/(60+12)=0.667 ≥ 0.6，锁不掉。
//   门控仍按「候选 vs 冻结输出框」判突跳：前 10 帧 local_speed 还没建立，12 > max(8,0)
//   会连续 hold 满 kMeasMaxHold=10；随后认输放行、把这一跳记进速度基准 ⇒
//   之后 12px/帧 < max(8, 3×12)=36，全部放行。id 全程不变（上游不 reset PID），
//   尾部不再 [滞后/跟上] 交替，而是全部跟上。
//   （修复前旧行为：冻结框每帧漂 12px ⇒ 奇数帧 IoU 0.667、偶数帧 0.429 ⇒ 掉锁靠
//    距离复用保 id，尾部 [跟上/滞后] 交替——那正是"框宽<320px 时 IoU 先破"的掉 id 隐患。）
TEST(selector_continuous_fast_move_not_held) {
    TargetSelector sel;
    auto cfg = make_cfg();
    int id0 = -1;
    int held_tail = 0, follow_tail = 0;
    for (int i = 0; i < 28; ++i) {
        const float x = 200.0f + 12.0f * static_cast<float>(i);
        auto r = sel.select({box(x, 240, 60, 120)}, cfg, static_cast<uint32_t>(i * 7));
        CHECK(r.valid);
        if (i == 0) id0 = r.target_id;
        if (i >= 1) CHECK_EQ(r.target_id, id0);   // id 全程不变 ⇒ 不 reset PID
        if (i >= 18) {
            if (r.held) {
                ++held_tail;
            } else {
                ++follow_tail;
                CHECK(std::abs(center_x(r.box) - x) < kEps);            // 跟上帧：框 = 本帧
            }
        }
    }
    CHECK_EQ(held_tail, 0);
    CHECK_EQ(follow_tail, 10);
}

// (4) continuity：空帧后同位置重现 ⇒ track 复用、target_id 延续；但新骨架 continuity 恒 false、
//     且不再产出 kRectLock（统一 kScore）。上游按 target_id 是否变化 reset PID，
//     因此"id 不变"才是"不 reset"的关键信号（continuity 已不承载该语义）。
TEST(selector_continuity_for_same_box) {
    TargetSelector sel;
    auto cfg = make_cfg();
    auto r0 = sel.select({box(320, 240, 60, 120)}, cfg, 0);
    CHECK(r0.valid);
    CHECK(!r0.continuity);   // 首次锁定没有"上一帧"，不算延续
    // 空帧把 lost_grace(30ms) 耗尽 ⇒ active 掉，但 track 仍在（buffer 30 帧）
    for (int i = 1; i <= 5; ++i) {
        CHECK(!sel.select({}, cfg, static_cast<uint32_t>(i * 10)).valid);
    }
    // 同一位置重新出现：打分复用旧 track（IoU≥0.6）⇒ id 延续，continuity 恒 false
    auto r1 = sel.select({box(322, 242, 60, 120)}, cfg, 60);
    CHECK(r1.valid);
    CHECK_EQ(r1.target_id, r0.target_id);
    CHECK(!r1.continuity);
    CHECK_EQ(static_cast<int>(r1.reason), static_cast<int>(TargetSelection::Reason::kScore));

    // 对照组：空帧后出现在 150px 外（IoU=0、距离>40px）⇒ 新 track，id 变，continuity=false
    TargetSelector sel2;
    auto s0 = sel2.select({box(320, 240, 60, 120)}, cfg, 0);
    CHECK(s0.valid);
    for (int i = 1; i <= 5; ++i) {
        CHECK(!sel2.select({}, cfg, static_cast<uint32_t>(i * 10)).valid);
    }
    auto s1 = sel2.select({box(470, 240, 60, 120)}, cfg, 60);
    CHECK(s1.valid);
    CHECK(!s1.continuity);
    CHECK(s1.target_id != s0.target_id);
}

// 遥测累计量必须能被 reset() 清零（否则换局/换模型后数字只增不清，读不出这一局有没有在拦）
TEST(selector_holds_total_resets) {
    TargetSelector sel;
    auto cfg = make_cfg();
    for (int i = 0; i < 5; ++i) {
        sel.select({box(320.0f + 2.0f * static_cast<float>(i), 240, 60, 120)}, cfg,
                   static_cast<uint32_t>(i * 7));
    }
    sel.select({box(340, 240, 60, 120)}, cfg, 42);   // +12px 突跳 ⇒ 门控 hold
    CHECK_EQ(sel.selector_holds_total(), 1ull);
    sel.reset();
    CHECK_EQ(sel.selector_holds_total(), 0ull);
}

int main() {
    return ttbox_test::run_all();
}
