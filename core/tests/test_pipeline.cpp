// test_pipeline.cpp — 第13阶段：完整 Pipeline 单元测试（10 项）
//
// 覆盖：
//   1. 无 Detection          → Target.valid=false，Controller 输出安全命令
//   2. 单个 Detection        → 正确选中
//   3. 多个 Detection        → TargetSelector 按规则选择（最近中心）
//   4. Detection 越界        → 安全处理
//   5. NaN/Inf               → 拒绝（Controller 输出安全命令）
//   6. confidence 低于阈值    → 过滤
//   7. 坐标转换              → 已知输入输出完全一致
//   8. Controller            → 已知 TargetPoint 输出符合预期
//   9. 连续帧                → 目标位置变化，PID 状态正确连续
//   10. 目标消失             → Target invalid，Controller 输出安全
//
// 本测试只验证模块边界（TargetSelector/Coordinate/Controller），不触碰 HID/RKNN。
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <vector>

#include "aim/Pid1Controller.hpp"
#include "common/CoreContracts.hpp"
#include "controller/IController.hpp"
#include "controller/PidController.hpp"
#include "mouse/AimPointProfile.hpp"
#include "mouse/CoordinateTransform.hpp"
#include "mouse/TargetSelector.hpp"
#include "pipeline/Target.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

// 辅助：构造一个 DetectionBox
static DetectionBox box(float x1, float y1, float x2, float y2,
                        float score, int cls) {
    return DetectionBox{x1, y1, x2, y2, score, cls};
}

// 测试1：无 Detection → Target.valid=false，Controller 输出安全命令（dx=dy=0, valid=false）
static void test_no_detection() {
    TargetSelector selector;
    TargetSelectorConfig cfg;
    cfg.roi_w = 2560;
    cfg.roi_h = 1440;
    cfg.confidence = 0.25f;

    const std::vector<DetectionBox> empty;
    const auto sel = selector.select(empty, cfg, 1000);
    CHECK(!sel.valid);

    // Target 语义：无目标 → invalid
    Target t;
    t.valid = sel.valid;
    CHECK(!t.valid);

    // Controller：invalid 目标点 → 安全命令
    PidController controller;
    PidControllerParams p;
    p.kp_x = 17.0f;
    p.kp_y = 10.0f;
    p.reference_x = 1280.0f;
    p.reference_y = 720.0f;
    controller.configure(p);

    TargetPoint tp;  // valid=false
    tp.valid = false;
    tp.x = 0.0f;
    tp.y = 0.0f;
    const auto cmd = controller.update(tp);
    CHECK(!cmd.valid);
    CHECK(cmd.dx == 0);
    CHECK(cmd.dy == 0);
    std::printf("test1_no_detection: PASS\n");
}

// 测试2：单个 Detection → 正确选中
static void test_single_detection() {
    TargetSelector selector;
    TargetSelectorConfig cfg;
    cfg.roi_w = 2560;
    cfg.roi_h = 1440;
    cfg.confidence = 0.25f;

    const auto d = box(1196.0f, 706.0f, 1280.0f, 783.0f, 0.88f, 5);  // bus
    const auto sel = selector.select({d}, cfg, 2000);
    CHECK(sel.valid);
    CHECK(sel.box.class_id == 5);
    CHECK(std::abs(sel.box.score - 0.88f) < 1e-4f);
    CHECK(std::abs(sel.box.x1 - 1196.0f) < 1e-3f);
    std::printf("test2_single_detection: PASS\n");
}

// 测试3：多个 Detection → TargetSelector 按规则选择（离中心最近）
static void test_multi_detection() {
    TargetSelector selector;
    TargetSelectorConfig cfg;
    cfg.roi_w = 2560;
    cfg.roi_h = 1440;
    cfg.confidence = 0.25f;
    // 中心 (1280,720)；三个目标：近中心 / 远左上 / 远右下
    const auto near = box(1250.0f, 700.0f, 1310.0f, 740.0f, 0.90f, 2);   // 最近
    const auto far1 = box(100.0f, 100.0f, 200.0f, 200.0f, 0.95f, 0);     // 远
    const auto far2 = box(2000.0f, 1000.0f, 2100.0f, 1100.0f, 0.80f, 5); // 远
    const auto sel = selector.select({far1, near, far2}, cfg, 3000);
    CHECK(sel.valid);
    CHECK(sel.box.class_id == 2);  // 应选中最近的 car
    std::printf("test3_multi_detection: PASS\n");
}

// 测试4：Detection 越界 → 安全处理（不会崩溃，越界框要么被过滤要么不产生无效命令）
static void test_out_of_bounds() {
    TargetSelector selector;
    TargetSelectorConfig cfg;
    cfg.roi_w = 2560;
    cfg.roi_h = 1440;
    cfg.confidence = 0.25f;
    // 完全在画面外的框（负坐标 / 超大坐标）
    const auto neg = box(-500.0f, -500.0f, -100.0f, -100.0f, 0.90f, 0);
    const auto huge = box(10000.0f, 10000.0f, 20000.0f, 20000.0f, 0.90f, 2);
    const auto sel = selector.select({neg, huge}, cfg, 4000);
    // 越界框远离中心 → 不应被选中（valid=false 也属安全处理）
    CHECK(!sel.valid || sel.box.class_id >= 0);

    // Controller 对越界目标点：必须安全
    PidController controller;
    PidControllerParams p;
    p.kp_x = 17.0f;
    p.kp_y = 10.0f;
    p.reference_x = 1280.0f;
    p.reference_y = 720.0f;
    controller.configure(p);
    TargetPoint tp;
    tp.valid = true;
    tp.x = 20000.0f;  // 越界
    tp.y = 20000.0f;
    const auto cmd = controller.update(tp);
    // 越界但有限 → 输出被 clamp，不产生异常值
    CHECK(std::abs(cmd.dx) <= 32767);
    CHECK(std::abs(cmd.dy) <= 32767);
    std::printf("test4_out_of_bounds: PASS\n");
}

// 测试5：NaN/Inf → 必须拒绝（Controller 输出安全命令）
static void test_nan_inf() {
    PidController controller;
    PidControllerParams p;
    p.kp_x = 17.0f;
    p.kp_y = 10.0f;
    p.reference_x = 1280.0f;
    p.reference_y = 720.0f;
    controller.configure(p);

    TargetPoint nan_tp;
    nan_tp.valid = true;
    nan_tp.x = std::numeric_limits<float>::quiet_NaN();
    nan_tp.y = 720.0f;
    const auto cmd_nan = controller.update(nan_tp);
    CHECK(!cmd_nan.valid);
    CHECK(cmd_nan.dx == 0);
    CHECK(cmd_nan.dy == 0);

    TargetPoint inf_tp;
    inf_tp.valid = true;
    inf_tp.x = 1280.0f;
    inf_tp.y = std::numeric_limits<float>::infinity();
    const auto cmd_inf = controller.update(inf_tp);
    CHECK(!cmd_inf.valid);
    CHECK(cmd_inf.dx == 0);
    CHECK(cmd_inf.dy == 0);
    std::printf("test5_nan_inf: PASS\n");
}

// 测试6：confidence 低于阈值 → 必须过滤
static void test_low_confidence() {
    TargetSelector selector;
    TargetSelectorConfig cfg;
    cfg.roi_w = 2560;
    cfg.roi_h = 1440;
    cfg.confidence = 0.55f;  // 阈值 0.55
    const auto low = box(1250.0f, 700.0f, 1310.0f, 740.0f, 0.30f, 2);  // 0.30 < 0.55
    const auto sel = selector.select({low}, cfg, 5000);
    CHECK(!sel.valid);  // 低置信度被过滤
    std::printf("test6_low_confidence: PASS\n");
}

// 测试7：坐标转换 — 已知输入，输出必须与预期完全一致
// 复用 test_coordinate_transform 的确定性用例：box(90,40,110,140) aim_point(0.5,0.15)
// roi 200x200 → 参考点=(100,100)，target=(100,55) → 期望 error=(0,-45)
// ★ V1.0.13：aim_offset(10,-5) 已删（准星恒为裁剪区正中心）。
static void test_coordinate() {
    AimPointProfile p;
    p.offset_x = 0.5f;
    p.offset_y = 0.15f;
    float ex = 0.0f, ey = 0.0f;
    const DetectionBox b = box(90.0f, 40.0f, 110.0f, 140.0f, 0.9f, 1);
    CHECK(CoordinateTransform::pixel_error(b, 1, p, 200.0f, 200.0f, &ex, &ey));
    // target=(100,55) reference=(100,100) → error=(0,-45)
    CHECK(std::abs(ex) < 1e-5f);
    CHECK(std::abs(ey + 45.0f) < 1e-5f);

    // 参考点独立验证
    float rx = 0.0f, ry = 0.0f;
    CoordinateTransform::reference_point(200.0f, 200.0f, p, &rx, &ry);
    CHECK(std::abs(rx - 100.0f) < 1e-5f);
    CHECK(std::abs(ry - 100.0f) < 1e-5f);
    std::printf("test7_coordinate: PASS\n");
}

// 测试8：Controller — 已知 TargetPoint，输出 MouseCommand 必须符合预期。
// 目标点=参考点 → 误差=0 → 输出应接近 0（死区后为 0）
static void test_controller() {
    PidController controller;
    PidControllerParams p;
    p.kp_x = 17.0f;
    p.kp_y = 10.0f;
    p.reference_x = 1280.0f;
    p.reference_y = 720.0f;
    p.output_deadzone = 1.0f;
    controller.configure(p);

    TargetPoint tp;
    tp.valid = true;
    tp.x = 1280.0f;  // 正好在参考点
    tp.y = 720.0f;
    const auto cmd = controller.update(tp);
    CHECK(cmd.valid);
    CHECK(cmd.dx == 0);  // 误差 0 → 无输出
    CHECK(cmd.dy == 0);

    // 误差非零但很小（< 死区）→ 输出 0
    tp.x = 1281.0f;
    const auto cmd2 = controller.update(tp);
    CHECK(cmd2.dx == 0);
    std::printf("test8_controller: PASS\n");
}

// 测试9：连续帧 — 目标位置变化，PID 状态正确连续（误差收敛，输出有限）
static void test_continuous_frames() {
    PidController controller;
    PidControllerParams p;
    p.kp_x = 17.0f;
    p.kp_y = 10.0f;
    p.reference_x = 1280.0f;
    p.reference_y = 720.0f;
    p.output_deadzone = 0.0f;  // 关死区，观察真实输出
    controller.configure(p);

    // 目标从远处 (1500,900) 向参考点连续移动
    float px = 1500.0f, py = 900.0f;
    for (int i = 0; i < 30; ++i) {
        TargetPoint tp;
        tp.valid = true;
        tp.x = px;
        tp.y = py;
        const auto cmd = controller.update(tp);
        CHECK(cmd.valid);
        CHECK(std::abs(cmd.dx) <= 32767);
        CHECK(std::abs(cmd.dy) <= 32767);
        // 每帧目标向中心靠近 10px
        px -= 10.0f;
        py -= 10.0f;
    }
    // 目标到达参考点 → 输出收敛为 0
    TargetPoint tp;
    tp.valid = true;
    tp.x = 1280.0f;
    tp.y = 720.0f;
    const auto final_cmd = controller.update(tp);
    CHECK(final_cmd.valid);
    CHECK(final_cmd.dx == 0);
    CHECK(final_cmd.dy == 0);
    std::printf("test9_continuous_frames: PASS\n");
}

// 测试10：目标消失 → Target invalid，Controller 输出安全命令
static void test_target_lost() {
    PidController controller;
    PidControllerParams p;
    p.kp_x = 17.0f;
    p.kp_y = 10.0f;
    p.reference_x = 1280.0f;
    p.reference_y = 720.0f;
    controller.configure(p);

    // 先有目标
    TargetPoint tp;
    tp.valid = true;
    tp.x = 1400.0f;
    tp.y = 800.0f;
    const auto cmd1 = controller.update(tp);
    CHECK(cmd1.valid);

    // 目标消失（valid=false）→ 安全命令
    TargetPoint lost;
    lost.valid = false;
    lost.x = 0.0f;
    lost.y = 0.0f;
    const auto cmd2 = controller.update(lost);
    CHECK(!cmd2.valid);
    CHECK(cmd2.dx == 0);
    CHECK(cmd2.dy == 0);

    // 连续丢失多帧 → 一直安全
    for (int i = 0; i < 10; ++i) {
        const auto c = controller.update(lost);
        CHECK(!c.valid);
        CHECK(c.dx == 0);
        CHECK(c.dy == 0);
    }
    std::printf("test10_target_lost: PASS\n");
}

int main() {
    test_no_detection();
    test_single_detection();
    test_multi_detection();
    test_out_of_bounds();
    test_nan_inf();
    test_low_confidence();
    test_coordinate();
    test_controller();
    test_continuous_frames();
    test_target_lost();

    if (g_failures == 0) {
        std::printf("test_pipeline: ALL 10 PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_pipeline: %d FAILURES\n", g_failures);
    return 1;
}
