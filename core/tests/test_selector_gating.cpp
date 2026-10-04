// test_selector_gating.cpp — V1.0.11：选靶「量测门控 / continuity / 切靶确认窗」单元测试。
//
// 定障依据（板端 2026-09-29 完整录制 341,859 帧，见
// .workbuddy/artifacts/yu-选靶与落点抖动方案-2026-09-30.md）：
//   · 稳态（已对准）误差每秒过零 3.67 次、帧间只动 0.13px ⇒ 幅度小、频率高，就是"瞄上后还晃"；
//   · corr(框高极差, 落点极差)=0.744，落点对 y1 敏感度 0.69（对 y2 只 0.31）⇒ 是框顶在摆；
//   · 落点尖峰（>20px，占 1.26% 帧）里 88.6% 是**突跳**（跳变量 ÷ 邻帧位移中位 ≥2）
//     ⇒ 连续性判据能拦，且不误杀跟枪（离线回放 p90 抖动 −38.2%、互相关滞后 0 帧）。
//
// 本文件锁死六件事，任何一条红了都说明门控被改坏：
//   (1) 突跳被 hold：输出沿用上一帧框（holding_previous）＋ 遥测计数增长；
//   (2) 连续快移不误杀：局部速度基准被真实速度接管后阈值自动放宽，全程放行；
//   (3) 连续坏量测认输：hold 满 kMeasMaxHold=10 帧必须放行（防把锁定锁死）；
//   (4) 框高比硬上限 kSizeRatioCap=1.35：配置写 2.0 也只能更严、不能放宽；
//   (5) continuity：id 变了但框重合 ⇒ 标记延续（上游据此不重置平滑器/PID）；
//   (6) 切靶确认窗：丢锁后新目标要在同一处连续待够帧数才切，一闪而过的框切不过去。
#include <cmath>
#include <vector>

#include "mouse/TargetSelector.hpp"
#include "test_util.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

namespace {

TargetSelectorConfig make_cfg() {
    TargetSelectorConfig c;
    c.fov_range = 1.0f;      // 搜索半径 = min(640,480) = 480px（全帧短边），中心 (320,240)
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
float height(const DetectionBox& b) { return b.y2 - b.y1; }

constexpr float kEps = 0.05f;

}  // namespace

// (1) 突跳被 hold + (3) hold 满上限认输
// 前 5 帧走 2px/帧 正常移动把局部速度基准建起来（d_hist=[2,2,2,2] ⇒ 阈值 max(8, 3×2)=8px），
// 第 6 帧同一尺寸的框突然跳 +72px ⇒ 判坏、沿用上一帧框；连续喂同一个坏量测到第 11 帧才放行。
TEST(selector_measure_jump_held_then_gives_in) {
    TargetSelector sel;
    auto cfg = make_cfg();
    for (int i = 0; i < 5; ++i) {
        const float x = 320.0f + 2.0f * static_cast<float>(i);
        auto r = sel.select({box(x, 240, 60, 120)}, cfg, static_cast<uint32_t>(i * 7));
        CHECK(r.valid);
        CHECK(!r.held);   // 正常小位移一律放行
    }
    // 第 6 帧：突跳（72px ≫ 阈值 8px）
    auto r = sel.select({box(400, 240, 60, 120)}, cfg, 42);
    CHECK(r.valid);
    CHECK(r.held);                                     // 量测被判坏
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(TargetSelection::kTrackLock));
    CHECK(std::abs(center_x(r.box) - 328.0f) < kEps);  // 输出 = 上一帧框（未更新）
    CHECK_EQ(sel.selector_holds_total(), 1ull);
    // 继续喂同一个坏量测：hold 上限 kMeasMaxHold=10 ⇒ 第 2~10 次一律沿用旧框
    for (int i = 0; i < 9; ++i) {
        auto rh = sel.select({box(400, 240, 60, 120)}, cfg, static_cast<uint32_t>(49 + i * 7));
        CHECK(rh.valid);
        CHECK(rh.held);
        CHECK(std::abs(center_x(rh.box) - 328.0f) < kEps);
    }
    CHECK_EQ(sel.selector_holds_total(), 10ull);
    // 第 11 帧：认输放行 —— 门控会自愈，不会把锁定锁死
    auto r2 = sel.select({box(400, 240, 60, 120)}, cfg, 130);
    CHECK(r2.valid);
    CHECK(!r2.held);
    CHECK(std::abs(center_x(r2.box) - 400.0f) < kEps);
}

// (2) 连续快移不误杀
// 12px/帧 ≫ 绝对下限 8px —— 起步因基准为零会被 hold 几帧（这是设计：宁可晚几帧也不放坏量测），
// 认输后真实速度进基准，阈值抬到 36px，之后必须全程放行，否则就是"跟枪被门控杀掉"。
TEST(selector_continuous_fast_move_not_held) {
    TargetSelector sel;
    auto cfg = make_cfg();
    int held_tail = 0, valid_tail = 0;
    for (int i = 0; i < 28; ++i) {
        const float x = 200.0f + 12.0f * static_cast<float>(i);
        auto r = sel.select({box(x, 240, 60, 120)}, cfg, static_cast<uint32_t>(i * 7));
        CHECK(r.valid);
        if (i >= 18) {
            if (r.held) ++held_tail; else ++valid_tail;
            CHECK(!r.held);
            CHECK(std::abs(center_x(r.box) - x) < kEps);   // 跟得上，没有滞后
        }
    }
    CHECK_EQ(held_tail, 0);
    CHECK_EQ(valid_tail, 10);
}

// (4) 框高比硬上限：配置只能调得更严、不能放宽
TEST(selector_size_ratio_cap_cannot_be_loosened) {
    // 板端存量 config.d 里写的是 2.0 —— 生效值仍须被钳到 1.35，
    // 否则"同一目标头顶边界抖动"（实测 p50=1.43 / p90=1.86）会 92% 被放行。
    auto run = [](float cand_h, float ratio_cfg) -> float {
        TargetSelector sel;
        auto cfg = make_cfg();
        cfg.track_size_ratio = ratio_cfg;
        CHECK(sel.select({box(320, 240, 60, 100)}, cfg, 0).valid);   // 锁定 h=100 的目标
        // 第 2 帧：近处候选（h=cand_h）+ 稍远处候选（h=110，比 1.10 一定合规）
        auto r = sel.select({box(321, 241, 60, cand_h), box(326, 240, 60, 110)}, cfg, 7);
        CHECK(r.valid);
        CHECK(!r.held);    // 位移只有 ~1.4px，门控不该插手 —— 本条只验尺寸门
        return height(r.box);
    };
    // 比 1.40 > 1.35 ⇒ 近处那个必须被挡，只能选 h=110
    CHECK(std::abs(run(140.0f, 2.0f) - 110.0f) < kEps);
    // 比 1.30 ≤ 1.35 ⇒ 近处那个仍能竞争（门没被焊死）
    CHECK(std::abs(run(130.0f, 2.0f) - 130.0f) < kEps);
    // 关掉尺寸门（0）⇒ 近处 h=140 的候选又回来了（反证上面确实是"门"在挡）
    CHECK(std::abs(run(140.0f, 0.0f) - 140.0f) < kEps);
}

// (5) continuity：id 变了（或重建）但框与上一帧重合 ⇒ 标记为"同一目标的延续"
// 上游 AimThread 据此**不重置**平滑器/PID（避免落点跳 + 重新起步）。
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
    // 同一位置重新出现：第 2 层 rect_lock 复用旧 track，框与上帧重合 ⇒ continuity
    auto r1 = sel.select({box(322, 242, 60, 120)}, cfg, 60);
    CHECK(r1.valid);
    CHECK_EQ(r1.target_id, r0.target_id);
    CHECK(r1.continuity);
    CHECK_EQ(static_cast<int>(r1.reason), static_cast<int>(TargetSelection::kRectLock));

    // 对照组：空帧后出现在 150px 外（框完全不重合）⇒ 新 track，continuity=false
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

// (6) 切靶确认窗：丢锁后新目标要在同一处连续待够帧数才切
// 布局：中心 (320,240)，锁定 A(320,240)；A 消失后场上剩「候选 + 480/230px 处的陪跑框」。
// 候选都放在 A 的 track_lock 匹配半径（对角 134 + 8 = 142px）之外、FOV 半径 480 之内，
// 且比陪跑框更近中心 ⇒ 每帧都会被选中 ⇒ 真正走到确认窗。
TEST(selector_switch_confirm_window) {
    auto cfg = make_cfg();   // switch_confirm_frames 默认 4
    const DetectionBox fixed = box(320, 470, 60, 120);   // 离中心 230px 的陪跑框，永不被选

    // ---- (a) 一闪而过：候选每帧换个地方（相距 360px > 确认半径 40px）⇒ 永远确认不了 ----
    {
        TargetSelector sel;
        CHECK(sel.select({box(320, 240, 60, 120), box(520, 240, 60, 120)}, cfg, 0).valid);
        const float spots[2] = {140.0f, 500.0f};
        for (int i = 0; i < 8; ++i) {
            auto r = sel.select({box(spots[i % 2], 240, 60, 120), fixed}, cfg,
                                static_cast<uint32_t>(40 + i * 7));
            CHECK(!r.valid);   // 确认窗一直没放行（这就是"挡住一闪而过的错框"）
        }
    }

    // ---- (b) 稳定出现：同一位置连续 4 帧 ⇒ 第 4 帧真切过去（延迟 3 帧 ≈21ms，可接受）----
    {
        TargetSelector sel;
        CHECK(sel.select({box(320, 240, 60, 120), box(520, 240, 60, 120)}, cfg, 0).valid);
        for (int i = 0; i < 3; ++i) {
            auto r = sel.select({box(140.0f, 240, 60, 120), fixed}, cfg,
                                static_cast<uint32_t>(40 + i * 7));
            CHECK(!r.valid);
        }
        auto r = sel.select({box(140.0f, 240, 60, 120), fixed}, cfg, 61);
        CHECK(r.valid);
        CHECK(std::abs(center_x(r.box) - 140.0f) < kEps);
    }

    // ---- (c) 首次锁定不受确认窗延迟（没有"上一帧输出框"就不该等）----
    {
        TargetSelector sel;
        auto r = sel.select({box(320, 240, 60, 120), box(520, 240, 60, 120)}, cfg, 0);
        CHECK(r.valid);
        CHECK(std::abs(center_x(r.box) - 320.0f) < kEps);
    }
}

// 遥测累计量必须能被 reset() 清零（否则换局/换模型后数字只增不清，读不出这一局有没有在拦）
TEST(selector_holds_total_resets) {
    TargetSelector sel;
    auto cfg = make_cfg();
    for (int i = 0; i < 5; ++i) {
        sel.select({box(320.0f + 2.0f * static_cast<float>(i), 240, 60, 120)}, cfg,
                   static_cast<uint32_t>(i * 7));
    }
    sel.select({box(400, 240, 60, 120)}, cfg, 42);
    CHECK_EQ(sel.selector_holds_total(), 1ull);
    sel.reset();
    CHECK_EQ(sel.selector_holds_total(), 0ull);
}

int main() {
    return ttbox_test::run_all();
}
