// test_hotkey_guard.cpp — 热键保护（hotkey_guard）行为验证。
//
// 面板上的「热键保护」开关配一个切换键：按一次 = 全部挂起，再按一次 = 恢复。
// 挂起的实现是 AimThread 把物理按键位图在本控制周期内清零，于是瞄准 Gate 与压枪
// 一并失效（两者都读这个位图），而物理鼠标透传不受影响（那条路在 usbproxy 侧）。
//
// ★ 写这个测试踩过的两个坑，都记在这里免得下次再踩：
//   1) AimThread 只在**消费到新帧**的那个周期里采样按键。所以"改按键"必须同时推一个
//      新帧号；否则边沿检测压根不会被触发，看着像功能坏了，其实是代码没被叫到。
//   2) Pid1Controller 在误差恒定时输出会衰减（本例 err 恒定 -40/-120，pid 从 -3 掉到
//      -0.26，落到 deadzone 1.0 以下就成了 0 移动）。所以"有目标就持续出移动"是错的
//      假设。测"应该动"必须走**松开热键→再按下**的形态：松开会让 AimThread 执行
//      `if (!injection_allowed) pid.reset()`，按下的第一帧必然重新出力。这同时也是
//      真实使用形态。测"不该动"则相反，持续按住才有意义。
//
// 覆盖场景：
//   Case1 guard 关闭：按 toggle 键什么都不影响（瞄准照常出移动）
//   Case2 guard 开启 + 按一次 toggle：挂起 —— 瞄准热键按着也不出移动，status 如实报 true
//   Case3 再按一次 toggle：解除挂起，恢复移动；第三次按再次挂起（奇偶不串）
//   Case4 toggle 键一直按住：只翻转一次（按一下切换，不是按住连续翻转）
//   Case5 没配 toggle 键（0）：永不挂起
//   Case6 挂起后把 guard 关掉：立刻恢复，不留幽灵挂起
//   Case7 JSON 往返：hotkey_guard 能存能读
//   Case8 旧配置（无 hotkey_guard 键）：默认关闭 ⇒ 行为与本功能加入前一致
//   Case9 toggle 位掩码只取低 5 位：越界位被掩掉
//
// 说明：真实输出后端在 Windows 下是 stub，无法直接观察；这里用 RecordingHidOutput
// 记录 AimThread 实际 send 的每个 OutputAction，验的是「挂起后还有没有移动」这一行为。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "aim/AimThread.hpp"
#include "model/RuntimeProfile.hpp"
#include "output/IHidOutput.hpp"

using namespace ttbox::core::aim;

namespace {

constexpr uint16_t kAimKey = 0x02;      // 瞄准主热键 = 右键
constexpr uint16_t kToggleKey = 0x04;   // 热键保护切换键 = 中键
constexpr uint16_t kBothKeys = kAimKey | kToggleKey;

struct Action {
    int16_t move_x = 0;
    int16_t move_y = 0;
};

class RecordingHidOutput final : public ttbox::core::output::IHidOutput {
public:
    bool send(const ttbox::core::output::OutputAction& a) override {
        std::lock_guard<std::mutex> lk(mu_);
        actions_.push_back({a.move_x, a.move_y});
        return true;
    }
    std::vector<Action> snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return actions_;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        actions_.clear();
    }

private:
    std::mutex mu_;
    std::vector<Action> actions_;
};

// 目标框：画面中心偏右上；Gate 放行时应产生正向 X 与负向 Y 移动。
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
    std::shared_ptr<ttbox::core::RuntimeProfile> profile =
        std::make_shared<ttbox::core::RuntimeProfile>();
    ttbox::core::RuntimeConfig config;
    std::atomic<uint16_t> buttons{0};
    AimThread thread;
    uint64_t frame_ = 0;

    explicit TestCtx(bool guard_enabled, uint8_t toggle_key = 0x04) {
        profile->mouse.enabled = true;      // 总开关打开，本文件只测热键保护语义
        profile->mouse.aim_profiles[0].hotkey = 0x02;   // 右键
        profile->mouse.aim_profiles[0].hotkey2 = 0x00;
        profile->mouse.aim_profiles[0].hotkey_mode = 0; // any
        profile->mouse.kp_x = 1.0f;
        profile->mouse.kp_y = 1.0f;
        profile->mouse.lost_grace_ms = 78.0f;
        profile->mouse.aim_point.offset_x = 0.5f;
        profile->mouse.aim_point.offset_y = 0.5f;
        profile->mouse.hotkey_guard.enabled = guard_enabled;
        profile->mouse.hotkey_guard.toggle_hotkey = toggle_key;
        config.update(profile);
    }

    bool start() {
        return thread.start(&mailbox, output, 2000, &config, &buttons);
    }

    // 设定按键 + 推进一个控制周期（有目标）。两者必须成对，否则采样不到按键。
    void step(uint16_t bits, int settle_ms = 30) {
        buttons.store(bits);
        ++frame_;
        AimTargetTask t;
        t.frame_number = frame_;
        t.timestamp_us = frame_ * 8000;
        t.frame_width = 1280;
        t.frame_height = 720;
        t.has_target = true;
        t.target = make_box();
        t.aim_point = {600.0f, 240.0f};
        t.detections.push_back(make_box());
        mailbox.offer(0, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
    }

    // 持续按住 bits 推进 n 个周期，返回这段时间的全部输出（用于验"不该动"）。
    std::vector<Action> hold(int n, uint16_t bits, int settle_ms = 30) {
        output->clear();
        for (int i = 0; i < n; ++i) step(bits, settle_ms);
        return output->snapshot();
    }

    // 先松开瞄准热键让 PID 清空，再按下 bits 推进 n 个周期（用于验"应该动"）。
    std::vector<Action> press(int n, uint16_t bits, int settle_ms = 30) {
        step(static_cast<uint16_t>(bits & ~kAimKey), settle_ms);
        output->clear();
        for (int i = 0; i < n; ++i) step(bits, settle_ms);
        return output->snapshot();
    }
};

bool saw_movement(const std::vector<Action>& acts) {
    for (const auto& a : acts) {
        if (a.move_x != 0 || a.move_y != 0) return true;
    }
    return false;
}

bool all_zero(const std::vector<Action>& acts) {
    for (const auto& a : acts) {
        if (a.move_x != 0 || a.move_y != 0) return false;
    }
    return true;
}

// 走到"已挂起"状态（前置不满足时返回 false，由调用方记一条 FAIL）。
bool enter_suspended(TestCtx& ctx) {
    ctx.step(kAimKey);    // 仅按瞄准热键
    ctx.step(kBothKeys);  // 再按下 middle → 上升沿翻转
    ctx.step(kAimKey);    // 松开 middle，热键继续按着
    return ctx.thread.status().hotkeys_suspended;
}

}  // namespace

int main() {
    int fails = 0;
    auto check = [&fails](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++fails;
    };

    // Case1 guard 关闭：toggle 键按了也不影响瞄准
    {
        TestCtx ctx(false);
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        check(saw_movement(ctx.press(3, kAimKey)), "Case1 guard 关闭：仅热键 -> 有移动");
        ctx.step(kBothKeys);   // 按下 middle（guard 关，应当毫无影响）
        check(saw_movement(ctx.press(3, kBothKeys)), "Case1 guard 关闭：按 toggle 键不影响瞄准");
        check(!ctx.thread.status().hotkeys_suspended, "Case1 guard 关闭：不报挂起");
        ctx.thread.stop();
    }

    // Case2 按一次 toggle → 挂起；瞄准热键按着也不出移动
    {
        TestCtx ctx(true);
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        check(saw_movement(ctx.press(3, kAimKey)), "Case2 前置：挂起前确实在出移动");
        if (!enter_suspended(ctx)) {
            check(false, "Case2 前置：按一次 toggle 未挂起");
        } else {
            check(true, "Case2 按一次 toggle -> status 报挂起");
            const auto acts = ctx.hold(5, kAimKey);
            check(!acts.empty() && all_zero(acts), "Case2 挂起后：热键按着也恒零输出");
            check(ctx.thread.status().gated_frames > 0, "Case2 挂起计入 gated_frames");
        }
        ctx.thread.stop();
    }

    // Case3 再按一次 → 解除
    {
        TestCtx ctx(true);
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        if (!enter_suspended(ctx)) {
            check(false, "Case3 前置：未挂起");
        } else {
            ctx.step(kBothKeys);  // 再按一次 middle → 上升沿翻转回来
            ctx.step(kAimKey);    // 松开
            check(!ctx.thread.status().hotkeys_suspended, "Case3 再按一次 -> 解除挂起");
            check(saw_movement(ctx.press(3, kAimKey)), "Case3 解除后：恢复出移动");
            ctx.step(kBothKeys);  // 第三次按 → 应再次挂起（奇偶不串）
            ctx.step(kAimKey);
            check(ctx.thread.status().hotkeys_suspended, "Case3 第三次按 -> 再次挂起（奇偶不串）");
        }
        ctx.thread.stop();
    }

    // Case4 一直按住 toggle：只翻转一次，不会一闪一闪地恢复瞄准
    {
        TestCtx ctx(true);
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        ctx.step(kAimKey);
        ctx.step(kBothKeys);   // 按下 middle 并保持
        if (!ctx.thread.status().hotkeys_suspended) {
            check(false, "Case4 前置：按下未挂起");
        } else {
            // 保持按住（bits 始终 kBothKeys）推进 4 个周期：若实现是"按住就翻转"，
            // 这里会翻 4 次 → 期间必然有一半周期恢复出移动。
            const auto acts = ctx.hold(4, kBothKeys);
            check(ctx.thread.status().hotkeys_suspended, "Case4 按住 toggle：状态不来回翻");
            check(!acts.empty() && all_zero(acts), "Case4 按住 toggle：输出恒为零（不会间歇恢复）");
        }
        ctx.thread.stop();
    }

    // Case5 没配 toggle 键 → 永不挂起
    {
        TestCtx ctx(true, 0x00);
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        check(!ctx.thread.status().hotkeys_suspended, "Case5 toggle 键为 0 -> 不挂起");
        check(saw_movement(ctx.press(3, kBothKeys)), "Case5 toggle 键为 0 -> 瞄准照常出移动");
        ctx.thread.stop();
    }

    // Case6 挂起后把 guard 关掉 → 立刻恢复
    {
        TestCtx ctx(true);
        if (!ctx.start()) { std::printf("[FAIL] start\n"); return 1; }
        if (!enter_suspended(ctx)) {
            check(false, "Case6 前置：未挂起");
        } else {
            ctx.profile->mouse.hotkey_guard.enabled = false;  // 运行期关掉 guard
            ctx.config.update(ctx.profile);                   // 确保快照看到这次改动
            ctx.step(kAimKey);
            check(!ctx.thread.status().hotkeys_suspended, "Case6 关掉 guard -> 立刻取消挂起");
            check(saw_movement(ctx.press(3, kAimKey)), "Case6 关掉 guard -> 恢复出移动");
        }
        ctx.thread.stop();
    }

    // Case7 JSON 往返
    {
        ttbox::core::RuntimeProfile p;
        p.mouse.hotkey_guard.enabled = true;
        p.mouse.hotkey_guard.toggle_hotkey = 0x08;
        const std::string text = p.to_json().dump();
        auto res = ttbox::core::json_parse(text);
        check(res.ok, "Case7 hotkey_guard 序列化可解析");
        if (res.ok) {
            const auto q = ttbox::core::RuntimeProfile::from_json(res.value);
            check(q.mouse.hotkey_guard.enabled, "Case7 往返：enabled=true 保留");
            check(q.mouse.hotkey_guard.toggle_hotkey == 0x08, "Case7 往返：toggle_hotkey 保留");
        }
    }

    // Case8 旧配置没有 hotkey_guard 键 → 默认关闭，行为不变
    {
        auto res = ttbox::core::json_parse(R"({"mouse":{"enabled":true,"aim_hotkey":2}})");
        check(res.ok, "Case8 旧配置可解析");
        if (res.ok) {
            const auto q = ttbox::core::RuntimeProfile::from_json(res.value);
            check(!q.mouse.hotkey_guard.enabled, "Case8 旧配置 -> guard 默认关闭");
            check(q.mouse.hotkey_guard.toggle_hotkey == 0x04, "Case8 默认 toggle 键 = middle");
        }
    }

    // Case9 越界/负数的 toggle 键一律归 0（永不翻转）
    //
    // ★ 2026-09-25 改口径：原先是"掩低 5 位"（250 & 0x1F = 26）。那个写法对正数无害，
    //   但对**负数**会绕回成"任意键"：`-1 & 0x1F = 31` = 左|右|中|侧1|侧2 全中 ⇒
    //   按任何一个键都能翻转热键保护（fail-open）。统一改成「越界一律 0 = 永不命中」，
    //   与 aim_profiles 的 hotkey 同一口径（sanitize_hotkey_bits）。
    {
        auto res = ttbox::core::json_parse(
            R"({"mouse":{"hotkey_guard":{"enabled":true,"toggle_hotkey":250}}})");
        check(res.ok, "Case9 越界 toggle 可解析");
        if (res.ok) {
            const auto q = ttbox::core::RuntimeProfile::from_json(res.value);
            check(q.mouse.hotkey_guard.toggle_hotkey == 0,
                  "Case9 越界 toggle_hotkey 归 0（永不翻转）");
        }
        // ★ 真正要挡的是负数：掩低 5 位会变成 31 = 任意键都能翻转
        auto neg = ttbox::core::json_parse(
            R"({"mouse":{"hotkey_guard":{"enabled":true,"toggle_hotkey":-1}}})");
        check(neg.ok, "Case9b 负数 toggle 可解析");
        if (neg.ok) {
            const auto q = ttbox::core::RuntimeProfile::from_json(neg.value);
            check(q.mouse.hotkey_guard.toggle_hotkey == 0,
                  "Case9b 负数 toggle 必须为 0（掩低5位会变 31 = 任意键都翻转）");
        }
        // 合法单键不受影响
        auto ok1 = ttbox::core::json_parse(
            R"({"mouse":{"hotkey_guard":{"enabled":true,"toggle_hotkey":16}}})");
        if (ok1.ok) {
            const auto q = ttbox::core::RuntimeProfile::from_json(ok1.value);
            check(q.mouse.hotkey_guard.toggle_hotkey == 16,
                  "Case9c 合法键位（侧2=16）保持不变");
        }
    }

    if (fails == 0) std::printf("test_hotkey_guard: PASS\n");
    else std::printf("test_hotkey_guard: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
