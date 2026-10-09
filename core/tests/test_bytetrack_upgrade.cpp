// test_bytetrack_upgrade.cpp — TargetSelector 轨迹生命周期单元测试
// 验证能力：
//   Case1: 轨迹生命周期（修"只增不删"隐患）—— 目标反复进出，轨迹数不无限增长
//   Case2: 轨迹上限 max_tracks —— 大量目标同时出现时轨迹数封顶
// ★ 2026-10-07 清理：原 Case3/4/5（卡尔曼速度学习 / 预测中心 / 开关）已随卡尔曼死链一并删除。
//   关联参考点恒用裸框心 cx/cy，无速度预测。
#include <cmath>
#include <cstdio>
#include <vector>

#include "mouse/TargetSelector.hpp"
#include "common/Types.hpp"
#include "test_util.hpp"

using namespace ttbox::core::aim;
using ttbox::core::DetectionBox;

namespace {
TargetSelectorConfig make_cfg() {
    TargetSelectorConfig c;
    c.fov_range = 1.0f;
    c.confidence = 0.0f;   // 不过滤，便于压轨迹数
    c.roi_w = 640;
    c.roi_h = 480;
    c.lost_grace_ms = 30.0f;
    return c;
}
DetectionBox box(float cx, float cy, float w = 40, float h = 80, float conf = 0.9f) {
    DetectionBox b;
    b.x1 = cx - w * 0.5f; b.y1 = cy - h * 0.5f;
    b.x2 = cx + w * 0.5f; b.y2 = cy + h * 0.5f;
    b.score = conf; b.class_id = 0;
    return b;
}
}  // namespace

// Case1: 轨迹生命周期——目标反复进出，轨迹数在 track_buffer 内收敛（不无限增长）
TEST(bytetrack_track_lifecycle_bounded) {
    TargetSelector sel;
    auto cfg = make_cfg();
    cfg.confidence = 0.25f;
    // 反复出现(3帧)→消失(远超 buffer 50帧) 循环多轮
    for (int round = 0; round < 20; ++round) {
        float cx = 320.0f;
        for (int i = 0; i < 3; ++i) {
            sel.select({box(cx, 240)}, cfg, static_cast<uint32_t>(1000 + round * 60 + i * 7));
        }
        // 消失足够久（track_buffer=30 帧）→ 轨迹应被删除
        for (int i = 0; i < 50; ++i) {
            sel.select({}, cfg, static_cast<uint32_t>(1000 + round * 60 + 30 + i * 7));
        }
        // 轨迹数不应随轮次累积（每次清空/收敛）
        CHECK(sel.tracks().size() <= 2u);
    }
}

// Case2: 轨迹上限——84 个目标同时出现，tracks 不得超 max_tracks(64)
TEST(bytetrack_max_tracks_cap) {
    TargetSelector sel;
    auto cfg = make_cfg();
    // 84 个目标平铺
    std::vector<DetectionBox> dets;
    int n = 0;
    for (int r = 0; r < 7 && n < 84; ++r)
        for (int c = 0; c < 12 && n < 84; ++c) {
            dets.push_back(box(50 + c * 50, 50 + r * 50, 30, 60));
            ++n;
        }
    auto selr = sel.select(dets, cfg, 1000);
    CHECK(selr.valid);
    // 轨迹数封顶
    CHECK(sel.tracks().size() <= cfg.max_tracks);
}

int main() {
    return ttbox_test::run_all();
}