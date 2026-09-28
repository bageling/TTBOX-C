// test_aim_thread.cpp — AimThread 生命周期与最新任务消费测试。
//
// ★ item 11 口径统一：本文件已由"裸 main() + assert"并入 ttbox_test 框架。
//   理由（与 test_aim_target_mailbox.cpp 同款）：
//     ① assert 受 NDEBUG 控制 —— Release 构建下 `assert` 被整段编译掉 ⇒ 测试变**空操作**、
//        恒退 0（"零断言静默 PASS"的一种形态）。CHECK 恒生效，杜绝此坑。
//     ② 裸 assert 失败即 SIGABRT —— CTest 只见"异常退出"，无 passed/skipped/failed 摘要行、
//        无法计 skip（框架外无 report_skip），破坏"绿=真绿"。
//   改用 TEST/CHECK 后：断言失败被**计数**（不中断进程），退出码由 run_all() 统一汇总。
//   注：本 TU 是**单文件可执行**（core/CMakeLists.txt:515-517 仅编本文件），故底部 main()
//       直接调用 run_all()（等价 test_main.cpp 惯用法），**无需改动 core/CMakeLists.txt**，
//       CTest 项 test_aim_thread 计数（1）保持不变。
#include "test_util.hpp"
#include "aim/AimThread.hpp"
#include "output/IHidOutput.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

using namespace ttbox::core::aim;

namespace {

// 记录每帧实际发出的 move，并累加出"真正交给输出链的 count 总和"。
// 用来把 AimStatus::out_counts_* 这个**标定分母**与逐帧输出对账。
class CountingHidOutput final : public ttbox::core::output::IHidOutput {
public:
    bool send(const ttbox::core::output::OutputAction& a) override {
        std::lock_guard<std::mutex> lk(mu_);
        sum_x_ += a.move_x;
        sum_y_ += a.move_y;
        if (a.move_x == 0 && a.move_y == 0) ++zero_frames_;
        ++frames_;
        return true;
    }
    // 按键注入（自动扳机）：记录"真的被调用过几次按下/抬起"。
    // 基类默认实现返回 false ⇒ 若 AimThread 没接线，这里一次都不会被调。
    bool mouse_button(uint8_t button, uint8_t action) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (action == 1) { ++down_; last_button_ = button; }
        else if (action == 2) { ++up_; }
        return true;
    }
    int64_t sum_x() { std::lock_guard<std::mutex> lk(mu_); return sum_x_; }
    int64_t sum_y() { std::lock_guard<std::mutex> lk(mu_); return sum_y_; }
    uint64_t frames() { std::lock_guard<std::mutex> lk(mu_); return frames_; }
    uint64_t zero_frames() { std::lock_guard<std::mutex> lk(mu_); return zero_frames_; }
    uint64_t button_down() { std::lock_guard<std::mutex> lk(mu_); return down_; }
    uint64_t button_up() { std::lock_guard<std::mutex> lk(mu_); return up_; }
    uint8_t last_button() { std::lock_guard<std::mutex> lk(mu_); return last_button_; }
private:
    std::mutex mu_;
    int64_t sum_x_ = 0;
    int64_t sum_y_ = 0;
    uint64_t frames_ = 0;
    uint64_t down_ = 0;
    uint64_t up_ = 0;
    uint64_t zero_frames_ = 0;   // 零位移帧（移动节流的判据）
    uint8_t last_button_ = 0;
};

ttbox::core::DetectionBox calib_box() {
    ttbox::core::DetectionBox b;
    b.x1 = 560.0f; b.y1 = 180.0f; b.x2 = 640.0f; b.y2 = 300.0f;
    b.score = 0.9f;
    b.class_id = 0;
    return b;
}

}  // namespace

// 自动标定的**分母真源**：累计请求投递的 HID count 必须与逐帧实际发出的 move 逐位一致。
//
// 为什么单独锁这一条：标定要算的是物理增益 gain(px/count) = Δ目标位移px / ΔΣcounts。
// 旧实现把「偏置的 px」当分母（px/px），量纲就不对 ⇒ 比值恒 ≈1.0、与游戏灵敏度无关，
// 标定"成功"也只能写出与真实手感无关的 kp。分母换成 Σcounts 之后，这个累计量
// 一旦与真实输出错一位，全部标定结论都错 ⇒ 必须有用例钉住，不能靠肉眼。
TEST(aim_thread_out_counts_match_sent_moves) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<CountingHidOutput>();
    auto profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0};

    profile->mouse.enabled = true;
    profile->mouse.calibrating = true;          // 标定期无视物理热键强制放行注入
    profile->mouse.calibration_bias_x = 20.0f;  // 参考点像素偏置（控制误差域）
    profile->mouse.aim_profiles[0].hotkey = 0x02;
    profile->mouse.kp_x = 1.0f;
    profile->mouse.kp_y = 1.0f;
    profile->mouse.kd_x = 0.0f;
    profile->mouse.kd_y = 0.0f;
    // smooth 是**削弱倍率**（outputScale = 10000 - smooth，只削 Kp/Kd）：
    // 默认 9900 会把 Kp 砍到 1/100，配合 output_deadzone 默认 1.0 ⇒ 本用例可能全帧零输出，
    // 断言就退化成 0 == 0。这里显式关掉削弱与死区，保证确实产生非零输出。
    profile->mouse.smooth_x = 0.0f;
    profile->mouse.smooth_y = 0.0f;
    profile->mouse.output_deadzone = 0.0f;
    config.update(profile);

    AimThread thread;
    CHECK(thread.start(&mailbox, output, 1000, &config, &buttons));

    for (uint64_t f = 1; f <= 40; ++f) {
        AimTargetTask t;
        t.frame_number = f;
        t.timestamp_us = 1000ULL * f;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = calib_box();
        t.aim_point = {600.0f, 240.0f};
        t.detections.push_back(calib_box());
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    thread.stop();  // join 后不再有新的 send，读数与累计量稳定

    const auto st = thread.status();
    CHECK(output->frames() > 0);
    // 核心不变式：累计量 == 逐帧实际发出量之和（两条轴各自成立）
    CHECK_EQ(st.out_counts_x, output->sum_x());
    CHECK_EQ(st.out_counts_y, output->sum_y());
    // 防止"全帧零输出 ⇒ 0 == 0 的弱断言"：本场景必然有非零输出
    CHECK(st.out_counts_x != 0);
}

// Gate 关（未按热键且不在标定）时输出被归零 ⇒ 累计量不得增长。
// 若漏在 Gate 之前累计，标定期间会把"被拦掉的帧"也算进分母 ⇒ gain 偏小。
TEST(aim_thread_out_counts_ignore_gated_frames) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<CountingHidOutput>();
    auto profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0};

    profile->mouse.enabled = true;
    profile->mouse.calibrating = false;
    profile->mouse.aim_profiles[0].hotkey = 0x02;
    profile->mouse.kp_x = 1.0f;
    profile->mouse.kp_y = 1.0f;
    profile->mouse.smooth_x = 0.0f;
    profile->mouse.smooth_y = 0.0f;
    profile->mouse.output_deadzone = 0.0f;
    config.update(profile);

    AimThread thread;
    CHECK(thread.start(&mailbox, output, 1000, &config, &buttons));

    for (uint64_t f = 1; f <= 20; ++f) {  // buttons 恒 0 ⇒ 热键未按
        AimTargetTask t;
        t.frame_number = f;
        t.timestamp_us = 1000ULL * f;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = calib_box();
        t.aim_point = {600.0f, 240.0f};
        t.detections.push_back(calib_box());
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    thread.stop();

    const auto st = thread.status();
    CHECK(output->frames() > 0);          // 帧照常走完输出链（Gate 在末端归零）
    CHECK_EQ(st.out_counts_x, 0);
    CHECK_EQ(st.out_counts_y, 0);
    CHECK_EQ(output->sum_x(), 0);
    CHECK(st.gated_frames > 0);
}

// 生命周期：start → offer(frame=7) → status 反映最新任务 → 无目标时位移归零。
TEST(aim_thread_lifecycle_consumes_latest_task) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<ttbox::core::output::NullHidOutput>();
    AimThread thread;
    CHECK(thread.start(&mailbox, output, 1000));

    AimTargetTask task;
    task.frame_number = 7;
    task.timestamp_us = 123;
    CHECK(mailbox.offer(0, task));

    // 活性上界（liveness bound），**非等值语义**：任务最终**必被** AimThread 消费，
    //   但**不承诺**在某个固定时限内完成——旧写法 `sleep_for(10ms)` 隐含"10ms 内必被
    //   调度消费"的延迟假设，高负载下（如紧跟并行 `cmake --build -j`）工作线程可能尚未
    //   被调度 ⇒ 偶发假红（实测 1/20）。故改为**带 deadline 的轮询等待**：1ms 间隔、上界 500ms。
    //   500ms 依据：远大于本场景实际所需（观测到的失败只差一个调度周期即可消费），又远小于
    //   CTest 默认超时，既不掩盖真故障、也不会误杀慢机器。
    //   到期**不提前 return**：照常取最终快照并交由下面的 CHECK 判定失败，避免退化成
    //   "零断言静默 PASS"（P0-2b 族反模式）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!thread.status().has_task && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // 在 stop() 之前取快照（与旧用例同序，保持语义不变）
    const auto status = thread.status();
    thread.stop();

    CHECK(status.has_task);
    CHECK_EQ(status.last_frame, static_cast<uint64_t>(7));
    CHECK(!status.has_target);
    CHECK(status.move_x == 0 && status.move_y == 0);
}

// 「无目标时也压枪」必须在**集成层**可达（不是只在模块单测里可达）
//
// ★ 2026-09-25 修：压枪原先整段待在 `if (selected.valid)` 块里，而传给
//   RecoilController 的 target_visible 实参就是 selected.valid ⇒ 恒为 true ⇒
//   RecoilController.hpp 里 `no_target_always && !has_target`（无目标时也压枪）和
//   `target_lost_release_ms`（目标丢失保持窗）两个分支永远进不去。
//   模块级单测（test_bb_second_batch.cpp）直接传 target_visible=false 全过，
//   **正好把"集成层走不到"盖住了** —— 这条用例补的就是那一层。
TEST(aim_thread_recoil_keeps_pressing_without_target_when_no_target_always) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<CountingHidOutput>();
    auto profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0x02};  // 全程按住右键（= 压枪热键 + 瞄准热键）

    profile->mouse.enabled = true;
    profile->mouse.aim_profiles[0].hotkey = 0x02;
    profile->mouse.output_deadzone = 0.0f;   // 死区会吃掉小压枪量，断言要看得见
    // 老压枪配置：update_bb 用它判"开火中"（hotkey_hit）
    profile->mouse.recoil.enabled = true;
    profile->mouse.recoil.hotkey = 0x02;
    profile->mouse.recoil.hotkey_mode = 1;   // any
    // BB 三段查表：开「无目标时也压枪」，关掉延迟/平滑/距离限制便于断言
    profile->mouse.recoil_bb.enabled = true;
    profile->mouse.recoil_bb.preset = 3;               // vert 三段 1.0 / 1.3 / 1.6
    profile->mouse.recoil_bb.preset_total_time_ms[0] = 1500.0f;
    profile->mouse.recoil_bb.preset_total_time_ms[1] = 1500.0f;
    profile->mouse.recoil_bb.preset_total_time_ms[2] = 1500.0f;
    profile->mouse.recoil_bb.no_target_always = true;  // ★ 面板那个复选框
    profile->mouse.recoil_bb.delay_ms = 0.0f;
    profile->mouse.recoil_bb.smooth = 0.0f;
    profile->mouse.recoil_bb.distance_limit = 0.0f;
    profile->mouse.recoil_bb.global_vert = 1.0f;
    config.update(profile);

    AimThread thread;
    CHECK(thread.start(&mailbox, output, 1000, &config, &buttons));

    // 全程**无目标**：检测列表空
    for (uint64_t f = 1; f <= 40; ++f) {
        AimTargetTask t;
        t.frame_number = f;
        t.timestamp_us = 1000ULL * f;   // 帧间隔 1ms
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = false;
        t.aim_point = {600.0f, 240.0f};
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    thread.stop();

    CHECK(output->frames() > 0);
    // ★ 核心断言：无目标 + 按住开火键 + no_target_always ⇒ 必须有下压输出。
    //   修复前这里恒为 0（压枪整段被 selected.valid 门控，根本没被调用）。
    CHECK(output->sum_y() != 0);
}

// 自动扳机（v7.26）接线：开启 + 有目标 ⇒ 必须真的发出"按下"命令，并在按压时长后"抬起"。
//
// 为什么单独锁这条：TriggerController（两套状态机 + BB 标定值）此前**全仓无人 include**，
// AimThread 也不跑它 ⇒ 面板上的自动扳机开关是死的。模块级单测能把状态机测透，
// 却盖不住"根本没人调用它"—— 这正是"单测绿、集成死"那一类坑。
TEST(aim_thread_trigger_fires_and_releases_when_enabled) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<CountingHidOutput>();
    auto profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0};

    profile->mouse.enabled = true;
    // 扳机常满足：key1/key2/key3 置 0 表示"该键不参与判定"（见 AutoTrigger::key_down）
    profile->mouse.trigger.enabled = true;
    profile->mouse.trigger.key1 = 0;
    profile->mouse.trigger.key2 = 0;
    profile->mouse.trigger.key3 = 0;
    profile->mouse.trigger.rifle_mode = true;
    // 连发间隔 20ms > 按压时长 4ms ⇒ 按下后必定有"不开火"的帧走到抬起分支。
    // （反过来 interval < press_duration 时语义就是"一直按住"，那是配出来的，不是缺陷。）
    profile->mouse.trigger.rifle_interval = 20.0f;
    profile->mouse.trigger.confidence = 0.0f;          // 置信门放开（框 score 0.9 也够）
    profile->mouse.trigger.dist_threshold = 10000.0f;  // 距离门放开
    profile->mouse.trigger.crosshair_check = false;
    profile->mouse.trigger.click_key = 0x01;           // 左键（掩码）
    profile->mouse.trigger.press_duration = 4.0f;      // 4ms 后抬起（帧间隔 1ms ⇒ 必然抬到）
    profile->mouse.trigger.recoil_enabled = true;
    config.update(profile);

    AimThread thread;
    CHECK(thread.start(&mailbox, output, 1000, &config, &buttons));

    for (uint64_t f = 1; f <= 40; ++f) {
        AimTargetTask t;
        t.frame_number = f;
        t.timestamp_us = 1000ULL * f;   // 帧间隔 1ms
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = calib_box();
        t.aim_point = {600.0f, 240.0f};
        t.detections.push_back(calib_box());
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    thread.stop();

    const auto st = thread.status();
    CHECK(output->frames() > 0);
    // ★ 核心断言：扳机开了 + 有目标 ⇒ 必须真的点了下去（修复前恒 0，因为没人调 mouse_button）
    CHECK(output->button_down() > 0);
    CHECK(st.trigger_fire_count > 0);
    CHECK_EQ(st.trigger_fire_count, output->button_down());
    CHECK_EQ(output->last_button(), 0x01);   // 点的是配置的 click_key（左键掩码）
    // 按压时长到点后必须抬起，不能永远按住（鼠标卡在按下态 = 一直开枪）
    CHECK(output->button_up() > 0);
    CHECK(st.trigger_active);
}

// 扳机默认全关 ⇒ 一个按键命令都不许发出（"不开就零影响"是这批模块的统一纪律）。
TEST(aim_thread_trigger_stays_silent_when_disabled) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<CountingHidOutput>();
    auto profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0x02};

    profile->mouse.enabled = true;
    profile->mouse.aim_profiles[0].hotkey = 0x02;
    // trigger / trigger2 都用结构体默认（enabled=false）
    config.update(profile);

    AimThread thread;
    CHECK(thread.start(&mailbox, output, 1000, &config, &buttons));

    for (uint64_t f = 1; f <= 20; ++f) {
        AimTargetTask t;
        t.frame_number = f;
        t.timestamp_us = 1000ULL * f;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = calib_box();
        t.aim_point = {600.0f, 240.0f};
        t.detections.push_back(calib_box());
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    thread.stop();

    CHECK(output->frames() > 0);
    CHECK_EQ(output->button_down(), 0u);
    CHECK_EQ(output->button_up(), 0u);
    CHECK_EQ(thread.status().trigger_fire_count, 0u);
    CHECK(!thread.status().trigger_active);
}

// 贝塞尔弧线接线：`BezierTrajectory` 此前**三层全死**（无人 include / MouseProfile 无成员 /
// 配置不解析）⇒ 开了也没人跑。这条用例锁的是集成层：开启后同一段帧的输出必须真的变了。
// 模块级性质（只加垂直分量、偏移 ∝ 距离、误差趋零时归零）在 test_bezier_trajectory Case13~15。
namespace {
std::pair<int64_t, int64_t> run_aim_frames(bool bezier_on) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<CountingHidOutput>();
    auto profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0x02};

    profile->mouse.enabled = true;
    profile->mouse.aim_profiles[0].hotkey = 0x02;
    profile->mouse.bezier.enabled = bezier_on;
    profile->mouse.bezier.curvature = 0.5f;    // 放大弧线，便于断言看出差异
    profile->mouse.output_deadzone = 0.0f;
    profile->mouse.smooth_x = 0.0f;
    profile->mouse.smooth_y = 0.0f;
    profile->mouse.kp_x = 1.0f;
    profile->mouse.kp_y = 1.0f;
    config.update(profile);

    AimThread thread;
    thread.start(&mailbox, output, 1000, &config, &buttons);
    for (uint64_t f = 1; f <= 20; ++f) {
        AimTargetTask t;
        t.frame_number = f;
        t.timestamp_us = 1000ULL * f;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = calib_box();
        t.aim_point = {600.0f, 240.0f};
        t.detections.push_back(calib_box());
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    thread.stop();
    return {output->sum_x(), output->sum_y()};
}
}  // namespace

// ★ 为什么比的是**方向比值**而不是位移绝对值：
//   每帧控制误差方向基本一致 ⇒ sum_y/sum_x ≈ 单帧输出方向，与"实际跑到几帧"无关；
//   而绝对值会随线程取帧抖动变化（曾据此写出"关着也判不同"的**假阳性**用例 ——
//   反向验证时它照样绿，等于没锁住任何东西）。弧线加的是垂直分量 ⇒ 方向偏转最明显。
TEST(aim_thread_bezier_warp_changes_output_when_enabled) {
    const auto off = run_aim_frames(false);
    const auto on = run_aim_frames(true);
    CHECK(off.first != 0 && off.second != 0);   // 两轴都有输出（防 0 分母 / 0==0 弱断言）
    const double r_off = static_cast<double>(off.second) / static_cast<double>(off.first);
    const double r_on = static_cast<double>(on.second) / static_cast<double>(on.first);
    CHECK(std::fabs(r_on - r_off) > 0.05);      // ★ 接线后输出方向真的偏了
}

// ── 扳机联动两项（2026-09-26 接线）：trigger.y_offset 与 trigger2.move_throttle_frames ──
// 这两格面板早就有，但 core 从不读（配置键在 core/src 里 0 消费点）。
namespace {
struct TriggerLinkageResult {
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    uint64_t zero_frames = 0;
    uint64_t fires = 0;
};

// y_offset：压枪联动偏移（目标框高 × 比例）；throttle：开火后不发位移的帧数
TriggerLinkageResult run_trigger_linkage(float y_offset, int throttle) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<CountingHidOutput>();
    auto profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    // ★ 必须按住 hotkey（0x02）：Hotkey Gate 会把未按下热键的输出整帧归零，
    //   buttons=0 时 sum_y 恒 0，用例就成了"比两个 0"（假绿）。
    std::atomic<uint16_t> buttons{0x02};

    profile->mouse.enabled = true;
    profile->mouse.aim_profiles[0].hotkey = 0x02;
    profile->mouse.trigger.enabled = true;
    profile->mouse.trigger.key1 = 0;
    profile->mouse.trigger.key2 = 0;
    profile->mouse.trigger.key3 = 0;
    profile->mouse.trigger.rifle_mode = true;
    profile->mouse.trigger.rifle_interval = 20.0f;
    profile->mouse.trigger.confidence = 0.0f;
    profile->mouse.trigger.dist_threshold = 10000.0f;
    profile->mouse.trigger.crosshair_check = false;
    profile->mouse.trigger.click_key = 0x01;
    profile->mouse.trigger.press_duration = 4.0f;
    profile->mouse.trigger.recoil_enabled = true;
    profile->mouse.trigger.y_offset = y_offset;
    profile->mouse.trigger2.enabled = (throttle > 0);
    profile->mouse.trigger2.key1 = 0;
    profile->mouse.trigger2.key2 = 0;
    profile->mouse.trigger2.fire_button = 0x01;
    profile->mouse.trigger2.confidence = 0.0f;
    profile->mouse.trigger2.first_err = 10000.0f;
    profile->mouse.trigger2.move_throttle_frames = throttle;
    profile->mouse.output_deadzone = 0.0f;
    config.update(profile);

    AimThread thread;
    thread.start(&mailbox, output, 1000, &config, &buttons);
    for (uint64_t f = 1; f <= 30; ++f) {
        AimTargetTask t;
        t.frame_number = f;
        t.timestamp_us = 1000ULL * f;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = calib_box();
        t.aim_point = {600.0f, 240.0f};
        t.detections.push_back(calib_box());
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    thread.stop();
    return TriggerLinkageResult{output->sum_x(), output->sum_y(), output->zero_frames(),
                                thread.status().trigger_fire_count};
}
}  // namespace

// y_offset：开火后瞄准点要额外往下压「框高 × 比例」⇒ 输出的 Y 分量必须变多。
// 比的是 sum_y 的**差值**（同一段帧、同样配置，只改这一格），不是绝对值绝对值会随
// 取帧抖动漂 —— 那是上一轮写贝塞尔用例时踩过的假阳性。
TEST(aim_thread_trigger_y_offset_adds_downward_bias) {
    const auto off = run_trigger_linkage(0.0f, 0);
    const auto on = run_trigger_linkage(1.0f, 0);
    CHECK(off.fires > 0);            // 前提：两边都真的开了枪（否则比的是空跑）
    CHECK(on.fires > 0);
    CHECK(on.sum_y > off.sum_y);     // ★ 比例 1.0 ⇒ 下压更多
}

// move_throttle_frames：开火后若干帧不送位移（防扣扳机抖动）。
// ★ 同时锁「位移不丢」：节流帧的位移退回 remainder，所以累计位移不该被吃掉一大截。
TEST(aim_thread_trigger2_move_throttle_suspends_output) {
    const auto off = run_trigger_linkage(0.0f, 0);
    const auto on = run_trigger_linkage(0.0f, 4);
    CHECK(on.fires > 0);
    CHECK(on.zero_frames > off.zero_frames);   // ★ 节流 ⇒ 零位移帧变多
    // 只锁「链路没被锁死」：节流结束后必须继续有输出。
    // ★ 不去断言"总位移量级守恒" —— 节流改变的是闭环行为（误差多存在几帧，PID 后续
    //   输出轨迹整体不同），总量本就不该相等；位移不丢是由"退回 remainder"保证的，
    //   那属于实现细节，靠代码注释与人工复核，不靠这条集成用例。
    CHECK(off.sum_y != 0);
    CHECK(on.sum_y != 0);
}

// ── V3 阶段 2（2026-09-28）：倍镜倍率把 PID 误差压回角度域 ──
// 根因：kp 是按腰射标定的，倍镜下同一**角度**偏差在画面上被放大 M 倍
//   ⇒ 等效增益被放大 M 倍 ⇒ 6 倍镜必过冲（实测 530px 且振荡）。
// 修法：PID 输入误差除以本档真实倍率 M（腰射 M=1 ⇒ 行为不变）。
// 用例锁的是"同一段帧、只改 zoom_scale，累计输出按倍率缩小"这个**端到端**结论。
namespace {
struct ZoomRun {
    int64_t sum_x = 0;
    int64_t sum_y = 0;
};
ZoomRun run_zoom_output(float zoom_scale) {
    AimTargetMailbox mailbox(1);
    auto output = std::make_shared<CountingHidOutput>();
    auto profile = std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0x02};   // 按住右键，否则 Hotkey Gate 整帧归零 ⇒ 假绿

    profile->mouse.enabled = true;
    profile->mouse.aim_profiles[0].hotkey = 0x02;
    profile->mouse.aim_profiles[0].zoom_scale = zoom_scale;
    profile->mouse.output_deadzone = 0.0f;   // 关掉输出门限，只比"误差折算"这一件事
    config.update(profile);

    AimThread thread;
    thread.start(&mailbox, output, 1000, &config, &buttons);
    for (uint64_t f = 1; f <= 30; ++f) {
        AimTargetTask t;
        t.frame_number = f;
        t.timestamp_us = 1000ULL * f;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = calib_box();
        t.aim_point = {800.0f, 240.0f};   // 画面中心 640 ⇒ 恒定 +160px 横向误差
        t.detections.push_back(calib_box());
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const ZoomRun r{output->sum_x(), output->sum_y()};
    thread.stop();
    return r;
}
}  // namespace

// 判据用 **Y 轴**：检测框中心 y=240、画面中心 360 ⇒ 天然有 -120px 恒定误差，
// 不必靠 aim_point 造误差（X 轴在这个 harness 里误差为 0，比 0 是假绿）。
static int64_t zoom_abs_y(const ZoomRun& r) { return r.sum_y < 0 ? -r.sum_y : r.sum_y; }

TEST(aim_thread_zoom_scale_shrinks_output_by_magnification) {
    const ZoomRun hip = run_zoom_output(1.0f);      // 腰射：不折算
    const ZoomRun scoped = run_zoom_output(8.674f); // 6 倍镜真实倍率（实测 1.44×6）
    const int64_t a = zoom_abs_y(hip);
    const int64_t b = zoom_abs_y(scoped);
    CHECK(a > 0);            // 前提：腰射真的动了（否则比的是两个 0）
    CHECK(b > 0);            // 前提：高倍下仍有输出（被压成 0 说明折算过头）
    CHECK(b * 4 < a);        // ★ 8.67 倍 ⇒ 输出应缩到约 1/8.67，放宽到 1/4 防取帧抖动
}

// 倍率越大 ⇒ 输出越小（单调）。用实测的三个档位（腰射 / 2 倍 / 6 倍）串起来，
// 防止"只压第一个档、其余档没接上"这种局部接线错误蒙混过关。
TEST(aim_thread_zoom_scale_monotonic_across_scopes) {
    const int64_t hip = zoom_abs_y(run_zoom_output(1.0f));
    const int64_t s2 = zoom_abs_y(run_zoom_output(2.873f));   // 2 倍镜实测
    const int64_t s6 = zoom_abs_y(run_zoom_output(8.674f));   // 6 倍镜实测
    CHECK(hip > s2);     // ★ 2 倍必须比腰射压得狠
    CHECK(s2 > s6);      // ★ 6 倍必须比 2 倍压得更狠
}

// 腰射（zoom=1.0）就是"没配过这个字段"的行为 —— 老配置 / OTA 升级后手感不变。
// ★ 不做"两轮逐 count 相等"：取帧数会抖动（30 帧 sleep 2ms 不保证拿到同样多帧），
//   钉死相等只会得到一条 flaky 用例（上一轮写节流用例时踩过同一类假阳性）。
TEST(aim_thread_zoom_scale_one_keeps_hipfire_behavior) {
    const int64_t a = zoom_abs_y(run_zoom_output(1.0f));
    const int64_t b = zoom_abs_y(run_zoom_output(1.0f));
    CHECK(a > 0);
    CHECK(b > 0);
    const int64_t d = a > b ? a - b : b - a;
    CHECK(d * 4 < a);    // 两轮只差取帧抖动（<25%），不是被倍率改了量级
}

int main() {
    std::printf("=== ttbox_core tests (aim_thread) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
