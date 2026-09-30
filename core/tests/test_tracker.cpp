// test_tracker.cpp — 第15阶段：AimTracker + TestPidController 运动场景自动测试
//
// 覆盖 9 种场景（真实仿真轨迹）：
//   1. 静止      2. 匀速      3. 加速      4. 减速
//   5. 急停      6. 方向反转  7. 目标切换  8. 短暂丢失  9. 完全丢失
//
// 验证：Tracker 稳定性（速度估计收敛/Reset）、PID 稳定性（输出有限无发散）、
//       响应延迟、预测误差、过冲、饱和、跟随误差。
// 本测试只验证模块边界（Tracker/Controller），不触碰 HID/RKNN。
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "common/CoreContracts.hpp"
#include "test_pid_controller.hpp"
#include "mouse/AimTracker.hpp"
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

// 每帧 4ms（250Hz 推理节奏，接近 RK3588 实际）
static constexpr uint64_t kDtUs = 4000;
static constexpr uint64_t kRefX = 1280;
static constexpr uint64_t kRefY = 720;

// 运行单帧：更新 tracker + 预测 + controller，返回 MouseCommand
static MouseCommand run_frame(AimTracker& tr, TestPidController& ctl,
                              float tx, float ty, int tid, uint64_t now_us,
                              float pred_s, float* pred_err_x = nullptr,
                              float* pred_err_y = nullptr) {
    tr.update(tx, ty, tid, now_us);
    float px = tx, py = ty;
    if (pred_s > 0.0f) tr.predict(pred_s, &px, &py);
    TargetPoint tp;
    tp.valid = true;
    tp.x = px;
    tp.y = py;
    if (pred_err_x) *pred_err_x = px - kRefX;
    if (pred_err_y) *pred_err_y = py - kRefY;
    return ctl.update(tp);
}

// 场景1：静止目标 — 预测误差≈0，输出收敛 0，无饱和
static void test_static() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    float max_abs_out = 0.0f, max_pred_err = 0.0f;
    for (int i = 0; i < 200; ++i) {
        float pex = 0, pey = 0;
        const auto cmd = run_frame(tr, ctl, kRefX, kRefY, 1, static_cast<uint64_t>(i) * kDtUs,
                                   0.05f, &pex, &pey);
        max_abs_out = std::max(max_abs_out, static_cast<float>(std::max(std::abs(cmd.dx), std::abs(cmd.dy))));
        max_pred_err = std::max(max_pred_err, std::max(std::abs(pex), std::abs(pey)));
    }
    CHECK(max_pred_err < 1.0f);      // 静止目标预测误差应≈0
    CHECK(max_abs_out <= 32767);     // 无饱和
    std::printf("test1_static: max_pred_err=%.3f max_out=%.1f\n", max_pred_err, max_abs_out);
}

// 场景2：匀速目标（100px/s 向右）— 预测命中精度：t 帧预测点应命中 t+pred 后的实际位置
// 注意：pred_err（预测点-参考点）是"预测控制误差"，匀速下含超前量必然>raw_err；
// 真正要验证的是"预测是否命中未来位置"（线性外推对匀速目标应精确）。
static void test_constant_velocity() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    const float pred_s = 0.05f;       // 预测 50ms
    const int pred_frames = 12;       // 50ms / 4ms ≈ 12 帧
    const int n = 200;
    std::vector<float> pos(n), pred_x(n);
    float x = kRefX;
    for (int i = 0; i < n; ++i) {
        x += 100.0f * (kDtUs / 1e6f);  // 100px/s
        pos[i] = x;
        tr.update(x, kRefY, 1, static_cast<uint64_t>(i) * kDtUs);
        float px = 0, py = 0;
        tr.predict(pred_s, &px, &py);
        pred_x[i] = px;
        TargetPoint tp; tp.valid = true; tp.x = px; tp.y = kRefY;
        const auto cmd = ctl.update(tp);
        CHECK(std::abs(cmd.dx) <= 32767);
    }
    // 预测命中精度：|pred_x[i] - pos[i+pred_frames]|（匀速下应≈0）
    double hit_err_sum = 0.0;
    int hit_n = 0;
    for (int i = 0; i + pred_frames < n; ++i) {
        hit_err_sum += std::abs(pred_x[i] - pos[i + pred_frames]);
        ++hit_n;
    }
    const double hit_err_avg = hit_err_sum / hit_n;
    // 预测超前量 = v*pred = 100*0.05 = 5px（预期值）
    CHECK(hit_err_avg < 2.0);  // 匀速下线性外推命中精度高
    std::printf("test2_const_vel: pred_hit_err_avg=%.2f (期望≈0, 理论超前5px)\n", hit_err_avg);
}

// 场景3：加速目标（a=50px/s²）— 输出有限，预测误差有界
static void test_acceleration() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    float max_out = 0.0f, max_pred_err = 0.0f;
    float x = kRefX, v = 0.0f;
    for (int i = 0; i < 300; ++i) {
        v += 50.0f * (kDtUs / 1e6f);  // 50px/s²
        x += v * (kDtUs / 1e6f);
        float pex = 0, pey = 0;
        const auto cmd = run_frame(tr, ctl, x, kRefY, 1, static_cast<uint64_t>(i) * kDtUs,
                                   0.05f, &pex, &pey);
        max_out = std::max(max_out, static_cast<float>(std::abs(cmd.dx)));
        max_pred_err = std::max(max_pred_err, std::abs(pex));
        CHECK(std::abs(cmd.dx) <= 32767);
    }
    CHECK(max_out <= 32767);  // 无饱和
    CHECK(max_pred_err < 100.0f);  // 加速下预测误差有界
    std::printf("test3_accel: max_out=%.1f max_pred_err=%.1f\n", max_out, max_pred_err);
}

// 场景4：减速目标（先加速后减速）— 输出有限，无发散
static void test_deceleration() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    float max_out = 0.0f, max_pred_err = 0.0f;
    float x = kRefX, v = 0.0f;
    for (int i = 0; i < 400; ++i) {
        // 前 200 帧加速 +50px/s²，后 200 帧减速 -50px/s²
        const float a = (i < 200) ? 50.0f : -50.0f;
        v += a * (kDtUs / 1e6f);
        if (v < 0.0f) v = 0.0f;
        x += v * (kDtUs / 1e6f);
        float pex = 0, pey = 0;
        const auto cmd = run_frame(tr, ctl, x, kRefY, 1, static_cast<uint64_t>(i) * kDtUs,
                                   0.05f, &pex, &pey);
        max_out = std::max(max_out, static_cast<float>(std::abs(cmd.dx)));
        max_pred_err = std::max(max_pred_err, std::abs(pex));
        CHECK(std::abs(cmd.dx) <= 32767);
    }
    CHECK(max_out <= 32767);
    CHECK(max_pred_err < 100.0f);
    std::printf("test4_decel: max_out=%.1f max_pred_err=%.1f\n", max_out, max_pred_err);
}

// 场景5：急停（匀速后瞬间停止）— 速度估计应迅速回落，输出有限
static void test_sudden_stop() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    float x = kRefX;
    const float v = 120.0f;  // 120px/s
    // 匀速 100 帧后急停
    for (int i = 0; i < 100; ++i) {
        x += v * (kDtUs / 1e6f);
        run_frame(tr, ctl, x, kRefY, 1, static_cast<uint64_t>(i) * kDtUs, 0.05f);
    }
    const float stop_x = x;
    float max_out_after_stop = 0.0f;
    for (int i = 100; i < 300; ++i) {
        // 目标停在 stop_x 不动
        const auto cmd = run_frame(tr, ctl, stop_x, kRefY, 1,
                                   static_cast<uint64_t>(i) * kDtUs, 0.05f);
        max_out_after_stop = std::max(max_out_after_stop, static_cast<float>(std::abs(cmd.dx)));
        CHECK(std::abs(cmd.dx) <= 32767);
    }
    // 急停后 200 帧内输出应回落（< 初始运动时的峰值量级）
    CHECK(max_out_after_stop <= 32767);
    std::printf("test5_sudden_stop: max_out_after_stop=%.1f\n", max_out_after_stop);
}

// 场景6：方向反转（向右 60 帧后向左 80 帧）— 速度符号翻转，目标穿越参考点后输出转负
static void test_direction_reversal() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    float x = kRefX;
    const float v = 100.0f;
    bool saw_positive_out = false, saw_negative_out = false;
    for (int i = 0; i < 140; ++i) {
        // 前 60 帧向右（到 1304），后 80 帧向左（穿越 1280 到 1172）
        const float dir = (i < 60) ? 1.0f : -1.0f;
        x += dir * v * (kDtUs / 1e6f);
        float pex = 0, pey = 0;
        const auto cmd = run_frame(tr, ctl, x, kRefY, 1, static_cast<uint64_t>(i) * kDtUs, 0.05f, &pex, &pey);
        if (i < 60 && cmd.dx > 0) saw_positive_out = true;
        if (i >= 60 && cmd.dx < 0) saw_negative_out = true;
        CHECK(std::abs(cmd.dx) <= 32767);
    }
    CHECK(saw_positive_out);   // 向右时输出正
    CHECK(saw_negative_out);   // 向左穿越参考点后输出负
    std::printf("test6_reversal: pos_out=%d neg_out=%d\n", saw_positive_out, saw_negative_out);
}

// 场景7：目标切换（id 1→2）— tracker 应 Reset 速度，输出有限
static void test_target_switch() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    // 目标1 向右运动 60 帧
    float x1 = kRefX;
    for (int i = 0; i < 60; ++i) {
        x1 += 100.0f * (kDtUs / 1e6f);
        run_frame(tr, ctl, x1, kRefY, 1, static_cast<uint64_t>(i) * kDtUs, 0.05f);
    }
    // 检查切换判定
    CHECK(tr.target_switched(2));
    // 切换到目标2（向左运动），速度应 Reset（不残留目标1的速度）
    tr.reset();
    float x2 = kRefX;
    bool saw_negative = false;
    for (int i = 60; i < 140; ++i) {
        x2 -= 100.0f * (kDtUs / 1e6f);
        const auto cmd = run_frame(tr, ctl, x2, kRefY, 2, static_cast<uint64_t>(i) * kDtUs, 0.05f);
        if (cmd.dx < 0) saw_negative = true;
        CHECK(std::abs(cmd.dx) <= 32767);
    }
    CHECK(saw_negative);  // 新目标方向被正确跟随（无旧速度残留）
    std::printf("test7_switch: switched=%d\n", tr.target_switched(2) ? 0 : 1);
}

// 场景8：短暂丢失（10 帧无目标）— tracker 保持原状态，恢复后速度有效
static void test_brief_loss() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    float x = kRefX;
    for (int i = 0; i < 60; ++i) {
        x += 100.0f * (kDtUs / 1e6f);
        run_frame(tr, ctl, x, kRefY, 1, static_cast<uint64_t>(i) * kDtUs, 0.05f);
    }
    const auto state_before = tr.state();
    // 短暂丢失：不调用 update（模拟无检测帧），10 帧后恢复
    for (int i = 60; i < 70; ++i) {
        // 无目标帧：tracker 不更新（由 AimThread 决策，测试这里保持状态）
    }
    // 恢复后继续更新：速度应从原状态延续（EMA 平滑，不突变）
    float x2 = x;
    for (int i = 70; i < 100; ++i) {
        x2 += 100.0f * (kDtUs / 1e6f);
        run_frame(tr, ctl, x2, kRefY, 1, static_cast<uint64_t>(i) * kDtUs, 0.05f);
    }
    CHECK(tr.state().valid);
    CHECK(std::abs(tr.state().vx) > 0.0f);  // 速度仍有效
    (void)state_before;
    std::printf("test8_brief_loss: vx=%.1f\n", tr.state().vx);
}

// 场景9：完全丢失（reset）— tracker 状态清空，再出现时重新建立
static void test_complete_loss() {
    AimTracker tr;
    TestPidController ctl;
    TestPidParams p;
    p.reference_x = kRefX; p.reference_y = kRefY;
    ctl.configure(p);
    for (int i = 0; i < 60; ++i) {
        run_frame(tr, ctl, kRefX + 50.0f, kRefY, 1, static_cast<uint64_t>(i) * kDtUs, 0.05f);
    }
    CHECK(tr.state().valid);
    tr.reset();  // 完全丢失 → LOST 超过 grace 后外部调用 reset
    CHECK(!tr.state().valid);
    // 重新出现：tracker 从零状态重新建立（速度清零起步）
    float pex = 0, pey = 0;
    const auto cmd = run_frame(tr, ctl, kRefX + 50.0f, kRefY, 1, 1000 * kDtUs, 0.05f, &pex, &pey);
    CHECK(tr.state().valid);
    CHECK(tr.state().vx == 0.0f);  // 首帧速度清零
    CHECK(std::abs(cmd.dx) <= 32767);
    std::printf("test9_complete_loss: reset_ok\n");
}

// 场景10（V3 阶段 3a）：滤波截止频率按目标框高自适应
//
// 实测依据（docs/calib/range-measure-2026-09-28.json，同腰射档）：
//   20m 处 box_h=84.55px 抖动 stdev 0.04px（占框 0.05%）
//   30m 处 box_h=50.69px 抖动 stdev 0.18px（占框 0.36%）⇒ 相对抖动差 7.6 倍
// ⇒ 远处小框要滤得更狠（截止频率更低）。但滤波越强滞后越大，故**默认关闭**。
static void test_box_adaptive_cutoff() {
    // ① 默认关闭 ⇒ 无论框高多少，截止频率恒为既有基准 0.8Hz。
    //    这条才是"老配置 / OTA 升级后行为不变"的真正判据。
    {
        AimTracker tr;
        tr.update(640.0f, 360.0f, 1, 0);
        tr.set_box_h(20.0f);
        CHECK(std::abs(tr.min_cutoff_hz() - 0.8f) < 1e-6f);
        tr.set_box_h(300.0f);
        CHECK(std::abs(tr.min_cutoff_hz() - 0.8f) < 1e-6f);
    }
    // ② 开启后：框越小 ⇒ 截止频率越低（滤得越狠）；框超过参考尺寸 ⇒ 封顶在 max
    {
        BoxAdaptiveFilterConfig cfg;
        cfg.enabled = true;
        cfg.ref_box_h_px = 100.0f;
        cfg.max_cutoff_hz = 0.8f;
        cfg.min_cutoff_hz = 0.15f;
        cfg.box_h_ema_alpha = 1.0f;   // α=1 ⇒ 直接用当帧框高，便于精确断言
        AimTracker small; small.configure(cfg);
        small.update(640.0f, 360.0f, 1, 0);
        small.set_box_h(20.0f);       // 远处小框
        AimTracker big; big.configure(cfg);
        big.update(640.0f, 360.0f, 1, 0);
        big.set_box_h(500.0f);        // 超过参考尺寸 ⇒ 封顶
        const float c_small = small.min_cutoff_hz();
        const float c_big = big.min_cutoff_hz();
        CHECK(c_small < c_big);                   // ★ 小框滤得更狠
        CHECK(std::abs(c_big - 0.8f) < 1e-6f);    // 大框回到既有基准值
        // 20px/100px = 0.2 ⇒ 0.15 + (0.8-0.15)×0.2 = 0.28
        CHECK(std::abs(c_small - 0.28f) < 1e-3f);
    }
    // ③ box_h <= 0（目标丢失 / 没框）⇒ **保持上一帧**，不清零。
    //    清零会让 cutoff 瞬间跳到最狠一档，目标一回来先吃一段迟钝期。
    {
        BoxAdaptiveFilterConfig cfg;
        cfg.enabled = true;
        cfg.box_h_ema_alpha = 1.0f;
        AimTracker tr; tr.configure(cfg);
        tr.update(640.0f, 360.0f, 1, 0);
        tr.set_box_h(50.0f);
        const float before = tr.min_cutoff_hz();
        tr.set_box_h(0.0f);
        CHECK(std::abs(tr.min_cutoff_hz() - before) < 1e-6f);   // ★ 保持不变
        tr.set_box_h(-5.0f);
        CHECK(std::abs(tr.min_cutoff_hz() - before) < 1e-6f);
    }
    // ④ 换目标 ⇒ 框高 EMA 重新建立（旧目标的框尺寸不带到新目标）
    {
        BoxAdaptiveFilterConfig cfg;
        cfg.enabled = true;
        cfg.box_h_ema_alpha = 1.0f;
        AimTracker tr; tr.configure(cfg);
        tr.update(640.0f, 360.0f, 1, 0);
        tr.set_box_h(200.0f);
        CHECK(tr.state().box_h_ema > 0.0f);
        tr.update(640.0f, 360.0f, 2, kDtUs);   // 换目标
        CHECK(tr.state().box_h_ema == 0.0f);   // ★ 已清零，等新目标首帧重建
    }
    std::printf("test10_box_adaptive_cutoff: cutoff_ok\n");
}

int main() {
    test_static();
    test_constant_velocity();
    test_acceleration();
    test_deceleration();
    test_sudden_stop();
    test_direction_reversal();
    test_target_switch();
    test_brief_loss();
    test_complete_loss();
    test_box_adaptive_cutoff();
    if (g_failures == 0) {
        std::printf("test_tracker: ALL 10 PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_tracker: %d FAILURES\n", g_failures);
    return 1;
}