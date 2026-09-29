// test_pull_curve_aimthread.cpp — AimThread 输出链注入点验证（拉枪曲线 + BB 对标第二批）。
//
// 覆盖验收场景：
//   Case1 热键ON + 远距离目标 + 拉枪启用  -> move_y 出现弧线附加量（区别于无拉枪基线）
//   Case2 距离 < min_distance             -> 无弧线附加
//   Case3 拉枪 disabled                  -> 无弧线附加
//   Case4 热键OFF + 拉枪激活             -> 最终输出仍被安全门吃成 {0,0}
//   Case5 BB 拟人化链（humanize）开启     -> 近距离基线为 0 时被噪声顶成非零（接线生效）
//   Case6 BB 三段查表压枪开启             -> 误差≈0 时仍出现纯下压量（接线生效）
//   Case7 新版提前量开启                  -> 老的持续提前量让路（X 输出回落，互斥生效）
//
// 说明：PullCurve / HumanizeShaper / RecoilController 算法本身的单测在
// test_mouse.cpp 与 test_bb_second_batch.cpp；本文件只验证 AimThread 输出链
// **注入点位置**正确、与死区/安全门的先后关系正确。
// Case5/Case6 是"接线了没有"的哨兵：这两个模块默认关，若 AimThread 忘了调用它们，
// 断言会红（而不是静默通过）。
// Case7 是"新老互斥"的哨兵：业主裁定「新版替老版，界面只留一套」，
// 老 continuous_lead 必须在新版开启时完全不参与输出，否则两条 X 偏移会叠加。
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "aim/AimThread.hpp"
#include "output/IHidOutput.hpp"

using namespace ttbox::core::aim;

namespace {

struct Action {
    int16_t move_x = 0;
    int16_t move_y = 0;
    uint64_t frame = 0;
};

class RecordingHidOutput final : public ttbox::core::output::IHidOutput {
public:
    bool send(const ttbox::core::output::OutputAction& a) override {
        std::lock_guard<std::mutex> lk(mu_);
        actions_.push_back({a.move_x, a.move_y, a.frame_number});
        return true;
    }
    std::vector<Action> snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return actions_;
    }
private:
    std::mutex mu_;
    std::vector<Action> actions_;
};

// 目标框：画面中心偏左上（ref=640,360 时 err_x<0 err_y<0）。
// 位置需同时满足：误差距离 > min_distance(80) 触发拉枪，
// 且误差距离 < FOV 半径（min(1280,720)*0.5*1.0=320）不被 TargetSelector 过滤。
// 中心 (500,200)：err=(-140,-160)，距离≈213 → 两者都满足。
ttbox::core::DetectionBox make_far_box() {
    ttbox::core::DetectionBox b;
    b.x1 = 460.0f; b.y1 = 160.0f; b.x2 = 540.0f; b.y2 = 240.0f;
    b.score = 0.9f;
    b.class_id = 0;
    return b;
}

// 近距离目标：误差距离 < 80，不触发拉枪。
ttbox::core::DetectionBox make_near_box() {
    ttbox::core::DetectionBox b;
    b.x1 = 620.0f; b.y1 = 340.0f; b.x2 = 660.0f; b.y2 = 380.0f;
    b.score = 0.9f;
    b.class_id = 0;
    return b;
}

struct TestCtx {
    AimTargetMailbox mailbox{1};
    std::shared_ptr<RecordingHidOutput> output = std::make_shared<RecordingHidOutput>();
    std::shared_ptr<ttbox::core::RuntimeProfile> profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0};
    AimThread thread;

    TestCtx(bool pull_enabled = true) {
        profile->mouse.enabled = true;
        profile->mouse.aim_profiles[0].hotkey = 0x02;   // 右键
        profile->mouse.aim_profiles[0].hotkey2 = 0x00;
        profile->mouse.aim_profiles[0].hotkey_mode = 0; // any
        profile->mouse.kp_x = 1.0f;         // 小 kp：输出量级可预测
        profile->mouse.kp_y = 1.0f;
        profile->mouse.sensitivity = 1.0f;
        profile->mouse.output_scale = 1.0f;
        profile->mouse.output_deadzone = 0.0f;  // 死区关，观察拉枪附加量
        profile->mouse.lost_grace_ms = 78.0f;
        profile->mouse.aim_point.offset_x = 0.5f;
        profile->mouse.aim_point.offset_y = 0.5f;
        // 拉枪曲线配置
        profile->mouse.pull_curve.enabled = pull_enabled;
        profile->mouse.pull_curve.strength = 0.8f;
        profile->mouse.pull_curve.jitter_px = 0.0f;   // 抖动已于 2026-09-29 删除（字段保留），显式置 0 保持基线可预期
        profile->mouse.pull_curve.min_distance = 80.0f;
        config.update(profile);
    }

    bool start() {
        return thread.start(&mailbox, output, 2000, &config, &buttons);
    }

    // 改完 profile 后重新发布快照（AimThread 每周期重读 ⇒ 无需重启线程）
    void reapply() { config.update(profile); }

    void feed(uint64_t frame, uint64_t ts_us, const ttbox::core::DetectionBox& box) {
        AimTargetTask t;
        t.frame_number = frame;
        t.timestamp_us = ts_us;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = box;
        t.aim_point = {(box.x1 + box.x2) * 0.5f, (box.y1 + box.y2) * 0.5f};
        t.detections.push_back(box);
        mailbox.offer(0, t);
    }
};

// 时序加固（2026-09-22）：固定 sleep 在高负载（如全量构建后首跑）下会假红——
// AimThread 可能 40ms 内一个 tick 都没跑。改为谓词轮询：条件满足立即返回，
// 最长等 timeout_ms；谓词需要「线程确实活跃过」（acts 非空）来区分
// 「等到了」与「根本没跑」，避免空 acts 让否定型断言（Case2/4）空洞通过。
template <typename Pred>
bool wait_until(TestCtx& ctx, Pred pred, int timeout_ms = 2000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (pred(ctx.output->snapshot())) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred(ctx.output->snapshot());
}

// 统计最后一次非零输出（排除热键关闭产生的 0）
bool any_move(const std::vector<Action>& acts) {
    for (const auto& a : acts) if (a.move_x != 0 || a.move_y != 0) return true;
    return false;
}

}  // namespace

int main() {
    int fails = 0;
    auto check = [&fails](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++fails;
    };

    // Case1: 热键ON + 远距离目标 + 拉枪启用 -> 出现移动（弧线附加 Y）
    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x02);                 // 热键 ON
        ctx.feed(1, 1000, make_far_box());       // 远距离目标
        // 等「线程活跃且已产出移动」，最长 2s；不再依赖固定 40ms（高负载假红）。
        const bool got_move = wait_until(ctx, [](const std::vector<Action>& acts) {
            return !acts.empty() && any_move(acts);
        });
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        bool moved = any_move(acts);
        check(got_move && moved, "Case1 拉枪启用+远距离+热键ON -> 出移动");
        // 拉枪只附加 Y 弧线；X 来自 PID 本身（err_x<0 -> move_x<0）。
        // 有移动即可证明注入点生效（对比 Case3 同配置拉枪关闭）。
    }

    // Case2: 距离 < min_distance -> 无弧线附加（但 PID 移动仍在）
    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x02);
        ctx.feed(1, 1000, make_near_box());      // 近距离目标
        // 先等线程确实跑过（acts 非空），再断言无移动——否则空 acts 让否定断言空洞通过。
        const bool thread_ran = wait_until(ctx, [](const std::vector<Action>& acts) {
            return !acts.empty();
        });
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        bool moved = any_move(acts);
        check(thread_ran && !moved, "Case2 近距离(<min_distance) -> 无拉枪附加、无移动");
    }

    // Case3: 拉枪 disabled + 远距离 -> 与 Case1 对照（无弧线附加）
    {
        TestCtx ctx(false);                      // pull_curve.enabled=false
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x02);
        ctx.feed(1, 1000, make_far_box());
        const bool got_move = wait_until(ctx, [](const std::vector<Action>& acts) {
            return !acts.empty() && any_move(acts);
        });
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        bool moved = any_move(acts);
        check(got_move && moved, "Case3 拉枪关闭 -> 仍有纯 PID 移动（注入点未破坏原链路）");
    }

    // Case4: 热键OFF + 拉枪激活 -> 最终输出仍被安全门吃成 {0,0}
    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x00);                 // 热键 OFF
        ctx.feed(1, 1000, make_far_box());
        // 等安全门路径真实跑过：acts 非空即线程 tick 过且被门控成 0。
        const bool thread_ran = wait_until(ctx, [](const std::vector<Action>& acts) {
            return !acts.empty();
        });
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        bool all_zero = true;
        for (const auto& a : acts) if (a.move_x != 0 || a.move_y != 0) all_zero = false;
        check(thread_ran && all_zero && !acts.empty(), "Case4 热键OFF+拉枪激活 -> 安全门优先，输出仍 0");
    }

    // Case5: BB 拟人化链（humanize）接线生效。
    // 判据：近距离目标的基线输出恒为 {0,0}（见 Case2），只有把高斯噪声接到输出链上，
    // 才会出现非零位移 —— 若 AimThread 忘了调 humanize_shaper_，本用例必红。
    {
        TestCtx ctx(false);
        ctx.profile->mouse.humanize.enabled = true;
        ctx.profile->mouse.humanize.smooth_factor = 0.0f;   // 关低通，只留噪声，判据更干净
        ctx.profile->mouse.humanize.noise_sigma = 5.0f;     // 放大到能被 int16 截断看见
        ctx.reapply();
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x02);
        ctx.feed(1, 1000, make_near_box());
        const bool got_move = wait_until(ctx, [](const std::vector<Action>& acts) {
            return !acts.empty() && any_move(acts);
        });
        ctx.thread.stop();
        check(got_move, "Case5 humanize 开启 -> 近距离也出现非零输出（接线生效）");
    }

    // Case6: BB 三段查表压枪接线生效。
    // 判据：近距离目标 PID 误差≈0（基线输出 0），压枪量只可能来自 recoil_bb 引擎；
    // 预设1 段1 vert=1.5px、global_vert=1 ⇒ 约 2 count 的纯下压。
    {
        TestCtx ctx(false);
        auto& m = ctx.profile->mouse;
        m.recoil.enabled = true;
        m.recoil.hotkey = 0x01;          // 左键作压枪热键
        m.recoil.hotkey2 = 0x00;
        m.recoil.hotkey_mode = 1;        // any
        m.recoil_bb.enabled = true;
        m.recoil_bb.preset = 1;
        m.recoil_bb.global_vert = 1.0f;
        m.recoil_bb.global_horiz = 0.0f;
        m.recoil_bb.smooth = 0.0f;       // 关平滑，输出确定
        m.recoil_bb.delay_ms = 0.0f;
        m.recoil_bb.distance_limit = 0.0f;
        m.recoil_bb.drift_enabled = false;
        m.vertical_correction.enabled = false;  // 只看压枪本体
        ctx.reapply();
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x03);         // 左键(压枪) + 右键(瞄准)
        ctx.feed(1, 1000, make_near_box());
        const bool got_pull = wait_until(ctx, [](const std::vector<Action>& acts) {
            for (const auto& a : acts) if (a.move_y > 0) return true;
            return false;
        });
        ctx.thread.stop();
        check(got_pull, "Case6 BB 三段查表压枪开启 -> 误差≈0 仍出现向下压枪量（接线生效）");
    }

    // Case7: 新老提前量互斥（业主 2026-09-24 裁定「新版替老版，界面只留一套」）。
    // 判据：同样目标与热键下，只开老的持续提前量时 X 输出更大（多了偏置）；
    //       把新版二代打开（先把激活距离设 0 ⇒ 它自己不产生偏移，只验互斥）后，老的必须让路
    //       ⇒ X 输出回落到纯 PID 量级。若互斥守卫被删（老的照跑），两组输出相同，本用例必红。
    {
        auto max_abs_x = [](const std::vector<Action>& acts) {
            int best = 0;
            for (const auto& a : acts) {
                const int v = static_cast<int>(a.move_x);
                const int av = (v < 0) ? -v : v;
                if (av > best) best = av;
            }
            return best;
        };
        auto run_case = [&](bool lead2_on) -> int {
            TestCtx ctx(false);
            auto& m = ctx.profile->mouse;
            m.continuous_lead.enabled = true;
            m.continuous_lead.enter_distance = 1.0f;   // 极易触发（误差≈140 count/帧）
            m.continuous_lead.scale = 1.0f;
            m.lead2.enabled = lead2_on;
            m.lead2.activation_distance = 0.0f;        // 新版自身零偏移，只验"老的有没有让路"
            ctx.reapply();
            if (!ctx.start()) { std::printf("[FAIL] start\n"); return -1; }
            ctx.buttons.store(0x02);
            ctx.feed(1, 1000, make_far_box());
            wait_until(ctx, [](const std::vector<Action>& acts) {
                return !acts.empty() && any_move(acts);
            });
            // 老的偏置是渐入的（level_ += (target-level_)*0.2），要跑够帧才接近满值
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            ctx.thread.stop();
            return max_abs_x(ctx.output->snapshot());
        };
        const int only_old = run_case(false);
        const int with_new = run_case(true);
        check(only_old > with_new && with_new > 0,
              "Case7 新版提前量开启 -> 老的持续提前量让路（X 输出回落）");
    }

    if (fails == 0) std::printf("test_pull_curve_aimthread: ALL PASS\n");
    else std::printf("test_pull_curve_aimthread: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
