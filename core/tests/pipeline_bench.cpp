// pipeline_bench.cpp — 第13阶段：Pipeline 模块分项耗时基准
// 测量 TargetSelector / CoordinateTransform / Controller 各模块耗时（微秒级，host 与板端均可跑）
// 输出：每模块 avg/min/max（us），以及"Pipeline 总耗时 = selector+coordinate+controller"（不含 Detector）
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "test_pid_controller.hpp"
#include "mouse/AimPointProfile.hpp"
#include "mouse/CoordinateTransform.hpp"
#include "mouse/TargetSelector.hpp"
#include "pipeline/Target.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

static DetectionBox box(float x1, float y1, float x2, float y2, float score, int cls) {
    return DetectionBox{x1, y1, x2, y2, score, cls};
}

static double us_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - t0).count();
}

int main() {
    constexpr int kFrames = 5000;
    TargetSelectorConfig cfg;
    cfg.roi_w = 2560;
    cfg.roi_h = 1440;
    cfg.confidence = 0.25f;
    AimPointProfile ap;
    ap.offset_x = 0.5f;
    ap.offset_y = 0.15f;
    TestPidController controller;
    TestPidParams pp;
    pp.reference_x = 1280.0f;
    pp.reference_y = 720.0f;
    controller.configure(pp);

    // 模拟 5 个检测（真实场景数量）
    std::vector<DetectionBox> dets = {
        box(1196.0f, 706.0f, 1280.0f, 783.0f, 0.88f, 5),
        box(1305.0f, 746.0f, 1383.0f, 801.0f, 0.90f, 2),
        box(1357.0f, 742.0f, 1411.0f, 777.0f, 0.86f, 2),
        box(1443.0f, 735.0f, 1519.0f, 806.0f, 0.80f, 2),
        box(1502.0f, 706.0f, 1529.0f, 969.0f, 0.83f, 0),
    };

    double t_sel = 0, t_coord = 0, t_ctrl = 0;
    double sel_min = 1e9, coord_min = 1e9, ctrl_min = 1e9;
    double sel_max = 0, coord_max = 0, ctrl_max = 0;

    TargetSelector selector;
    for (int i = 0; i < kFrames; ++i) {
        // Selector
        auto t0 = std::chrono::steady_clock::now();
        const auto sel = selector.select(dets, cfg, static_cast<uint32_t>(i));
        const double ds = us_since(t0);
        t_sel += ds; if (ds < sel_min) sel_min = ds; if (ds > sel_max) sel_max = ds;

        // Coordinate（瞄准点 + 参考点 + 误差）
        float tx = 0, ty = 0, rx = 0, ry = 0, ex = 0, ey = 0;
        auto t1 = std::chrono::steady_clock::now();
        if (sel.valid) {
            aim_point_at(sel.box, sel.box.class_id, ap, &tx, &ty);
            CoordinateTransform::reference_point(2560.0f, 1440.0f, ap, &rx, &ry);
            ex = tx - rx;
            ey = ty - ry;
        }
        const double dc = us_since(t1);
        t_coord += dc; if (dc < coord_min) coord_min = dc; if (dc > coord_max) coord_max = dc;

        // Controller
        TargetPoint tp;
        tp.valid = sel.valid;
        tp.x = tx;
        tp.y = ty;
        auto t2 = std::chrono::steady_clock::now();
        const auto cmd = controller.update(tp);
        const double dctrl = us_since(t2);
        t_ctrl += dctrl; if (dctrl < ctrl_min) ctrl_min = dctrl; if (dctrl > ctrl_max) ctrl_max = dctrl;
        (void)cmd;
    }

    std::printf("pipeline_bench: %d 帧, 5 detections, ROI 2560x1440\n", kFrames);
    std::printf("  TargetSelector : avg=%.3f us  min=%.3f  max=%.3f\n",
                t_sel / kFrames, sel_min, sel_max);
    std::printf("  Coordinate     : avg=%.3f us  min=%.3f  max=%.3f\n",
                t_coord / kFrames, coord_min, coord_max);
    std::printf("  Controller     : avg=%.3f us  min=%.3f  max=%.3f\n",
                t_ctrl / kFrames, ctrl_min, ctrl_max);
    std::printf("  Pipeline 总耗时(不含Detector) : avg=%.3f us\n",
                (t_sel + t_coord + t_ctrl) / kFrames);
    std::printf("  相对 infer(≈70000us) 占比 : %.4f%%\n",
                (t_sel + t_coord + t_ctrl) / kFrames / 70000.0 * 100.0);
    return 0;
}
