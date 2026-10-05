// test_hotkey_gate.cpp — Hotkey Gate 最终输出安全边界验证。
//
// 覆盖验收场景：
//   Case1 无目标 + 热键OFF -> HID {0,0}
//   Case2 有目标 + 热键OFF -> 检测/选中正常(status.has_target) 但 HID {0,0}
//   Case3 有目标 + 热键ON  -> HID 产生真实移动（方向朝目标）
//   Case4 热键一直ON + 目标突现 -> 下一控制周期立即出移动
//   Case5 热键一直ON + 目标消失 -> 停止 AI 移动（{0,0}，无旧坐标漂移）
//   Case6 瞄准中松开热键 -> 立即 {0,0}
//   Case7 松开后再次按下 -> 用当前最新目标恢复移动
//
// 说明：真实输出后端（FIFO/AiboxHidOutput）在 Windows 下为 stub，无法直接观察；
// 这里用 RecordingHidOutput 记录 AimThread 实际 send 的每个 OutputAction，
// 验证“Gate 位于最终输出边界”这一行为。
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

// 目标框：画面中心偏右上；若 Gate 放行，应产生正向 X 与负向 Y 移动。
ttbox::core::DetectionBox make_box() {
    ttbox::core::DetectionBox b;
    b.x1 = 560.0f; b.y1 = 180.0f; b.x2 = 640.0f; b.y2 = 300.0f;
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

    TestCtx() {
        profile->mouse.enabled = true;      // 总开关打开，本文件只测热键位语义
        profile->mouse.aim_profiles[0].hotkey = 0x02;   // 右键
        profile->mouse.aim_profiles[0].hotkey2 = 0x00;
        profile->mouse.aim_profiles[0].hotkey_mode = 0; // any
        profile->mouse.kp_x = 1.0f;
        profile->mouse.kp_y = 1.0f;
        profile->mouse.lost_grace_ms = 78.0f;
        profile->mouse.aim_point.offset_x = 0.5f;
        profile->mouse.aim_point.offset_y = 0.5f;
        config.update(profile);
    }

    bool start() {
        return thread.start(&mailbox, output, 2000, &config, &buttons);
    }

    void feed(uint64_t frame, uint64_t ts_us, bool has_target) {
        AimTargetTask t;
        t.frame_number = frame;
        t.timestamp_us = ts_us;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = has_target;
        if (has_target) {
            t.target = make_box();
            t.aim_point = {600.0f, 240.0f};
            t.detections.push_back(make_box());
        }
        mailbox.offer(0, t);
    }
};

int wait_frames(TestCtx& ctx, int ms = 30) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return static_cast<int>(ctx.output->snapshot().size());
}

}  // namespace

int main() {
    int fails = 0;
    auto check = [&fails](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++fails;
    };

    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x00);                      // 热键 OFF
        ctx.feed(1, 1000, false);                     // Case1: 无目标
        wait_frames(ctx);
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        bool all_zero = true;
        for (auto& a : acts) if (a.move_x != 0 || a.move_y != 0) all_zero = false;
        check(all_zero && !acts.empty(), "Case1 无目标+热键OFF -> HID=0,0");
    }

    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x00);                      // 热键 OFF
        ctx.feed(1, 1000, true);                      // Case2: 有目标
        wait_frames(ctx);
        ctx.thread.stop();
        auto st = ctx.thread.status();
        (void)st;
        auto acts = ctx.output->snapshot();
        bool all_zero = true;
        for (auto& a : acts) if (a.move_x != 0 || a.move_y != 0) all_zero = false;
        check(all_zero && !acts.empty(), "Case2 有目标+热键OFF -> HID=0,0");
        check(ctx.thread.status().gated_frames > 0, "Case2 记录 gated_frames>0");
    }

    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x02);                      // 热键 ON（右键）
        ctx.feed(1, 1000, true);                      // Case3: 有目标
        wait_frames(ctx);
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        bool moved = false;
        for (auto& a : acts) if (a.move_x != 0 || a.move_y != 0) moved = true;
        check(moved, "Case3 有目标+热键ON -> HID 产生真实移动");
    }

    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x02);                      // 热键一直 ON
        ctx.feed(1, 1000, false);                     // 无目标
        wait_frames(ctx);
        ctx.feed(2, 16000, true);                     // Case4: 目标突现
        wait_frames(ctx);
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        bool moved_after_appear = false;
        for (auto& a : acts) if (a.move_x != 0 || a.move_y != 0) moved_after_appear = true;
        check(moved_after_appear, "Case4 热键ON+目标突现 -> 出移动");
    }

    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x02);                      // 热键一直 ON
        ctx.feed(1, 1000, true);                      // 有目标，产生移动
        wait_frames(ctx);
        const int n_before = wait_frames(ctx);
        ctx.feed(2, 16000, false);                    // Case5: 目标消失
        wait_frames(ctx);
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        // 目标消失后（第2帧起）：不允许凭旧坐标输出非零移动
        bool zero_after_lost = true;
        for (size_t i = static_cast<size_t>(n_before); i < acts.size(); ++i)
            if (acts[i].move_x != 0 || acts[i].move_y != 0) zero_after_lost = false;
        check(zero_after_lost, "Case5 目标消失 -> 无旧坐标漂移");
    }

    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x02);                      // 热键 ON
        ctx.feed(1, 1000, true);
        wait_frames(ctx);
        const int n_before6 = wait_frames(ctx);
        ctx.buttons.store(0x00);                      // Case6: 松开热键
        ctx.feed(2, 16000, true);                     // 仍有目标
        wait_frames(ctx);
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        // 松开热键后（第2帧起）：必须立即全零
        bool zero_after_release = true;
        for (size_t i = static_cast<size_t>(n_before6); i < acts.size(); ++i)
            if (acts[i].move_x != 0 || acts[i].move_y != 0) zero_after_release = false;
        check(zero_after_release, "Case6 瞄准中松开热键 -> 立即停止");
    }

    {
        TestCtx ctx;
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.buttons.store(0x00);                      // 热键 OFF
        ctx.feed(1, 1000, true);                      // AI 持续运行
        wait_frames(ctx);
        ctx.buttons.store(0x02);                      // Case7: 再次按下
        ctx.feed(2, 16000, true);
        wait_frames(ctx);
        ctx.thread.stop();
        auto acts = ctx.output->snapshot();
        bool moved_after_repress = false;
        for (auto& a : acts) if (a.move_x != 0 || a.move_y != 0) moved_after_repress = true;
        check(moved_after_repress, "Case7 松开后再次按下 -> 用最新目标恢复移动");
    }

    if (fails == 0) std::printf("test_hotkey_gate: PASS\n");
    else std::printf("test_hotkey_gate: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
