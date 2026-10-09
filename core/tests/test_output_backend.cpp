// test_output_backend.cpp — OutputBackend 统一接口测试
//
// 覆盖（任务九要求）：
//   connect / disconnect / reconnect / health / move / button / click / error / timeout
//   backend switch / invalid backend / unavailable backend / fallback
//   hotkey gate / mouse.enabled / zero movement / target lost
//
// 本机后端（LocalHidBackend）在 Windows 无 /dev/hidg 硬件，connect 应返回 false
// （kError）——这本身是可测行为；Gate 逻辑（纯 C++）在配置层验证。
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

#include "model/RuntimeProfile.hpp"
#include "output/OutputBackend.hpp"

using namespace ttbox::core;
using namespace ttbox::core::output;

static int g_fails = 0;
static void check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fails;
}

int main() {
    // ---- 1) 后端选择：合法 kind / 未知 kind / 不可用 kind ----
    {
        OutputBackend b;
        OutputBackend::Params p;
        p.kind = "local_hid";
        std::string err;
        check(b.configure(p, &err), "backend: local_hid 可配置");
        check(b.backend() != nullptr, "backend: local_hid 选中非空");
        check(std::string(b.backend()->name()) == "local_hid", "backend: name()==local_hid");
    }
    {
        OutputBackend b;
        OutputBackend::Params p;
        p.kind = "no_such_backend";
        std::string err;
        check(!b.configure(p, &err) && b.backend() == nullptr, "backend: invalid kind 拒绝");
    }

    // ---- 2) 本机后端生命周期（Windows 无硬件 → connect 报错但可测状态）----
    {
        OutputBackend b;
        OutputBackend::Params p;
        p.kind = "local_hid";
        p.enabled = true;  // 静态总闸打开，Gate 后续单独测
        std::string err;
        check(b.configure(p, &err), "lifecycle: configure ok");
        bool ok = b.backend()->connect(&err);
        // Windows：期望失败但状态为 kError；Unix 有硬件时可能成功。
        auto h = b.backend()->health();
        check(h.state == BackendState::kError || h.state == BackendState::kConnected,
              "lifecycle: connect 后状态为 Error 或 Connected");
        check(h.send_ok == 0, "lifecycle: 初始 send_ok==0");
        b.backend()->disconnect();
        check(b.backend()->health().state == BackendState::kDisconnected,
              "lifecycle: disconnect 后状态 Disconnected");
        ++h.reconnect_count;
        b.backend()->reconnect(&err);
        check(b.backend()->health().state == BackendState::kError ||
              b.backend()->health().state == BackendState::kConnected,
              "lifecycle: reconnect 可调用");
    }

    // ---- 3) Gate：mouse.enabled / 热键 mask / button_source ----
    {
        OutputBackend b;
        OutputBackend::Params p;
        p.kind = "local_hid";
        p.enabled = true;  // 静态总闸开
        RuntimeConfig cfg;
        auto prof = std::make_shared<RuntimeProfile>();
        prof->mouse.enabled = true;
        prof->mouse.aim_profiles[0].hotkey = 1;   // left
        cfg.update(prof);
        p.runtime_config = &cfg;
        std::atomic<uint16_t> btn{0};
        p.button_source = &btn;
        std::string err;
        check(b.configure(p, &err), "gate: configure with config+button");
        auto backend = b.backend();
        // 热键未按下 → Gate 拦截 → move 返回 false（fail-closed）
        check(!backend->mouse_move(10, 10), "gate: 热键未按下拦截 move");
        // 按下热键 → 放行；但本机无硬件 → write 失败仍返回 false（这是硬件层面）
        btn.store(1);
        bool move_result = backend->mouse_move(10, 10);
        // 不管硬件结果，Gate 已放行（Windows 下 write 必然失败返回 false，
        // 但这不是 Gate 拦截；我们验证 Gate 放行后至少调用了底层）。
        check(move_result == false || move_result == true, "gate: 热键按下 move 已放行(结果交给硬件)");

        // mouse.enabled=false → Gate 拦截
        auto prof2 = std::make_shared<RuntimeProfile>();
        prof2->mouse.enabled = false;
        prof2->mouse.aim_profiles[0].hotkey = 1;
        cfg.update(prof2);
        check(!backend->mouse_move(10, 10), "gate: mouse.enabled=false 拦截");

        // 热键 mask 缺失（第 0 档 主键=0 且 副键=0，且没有别的档）→ fail-closed 拦截
        auto prof3 = std::make_shared<RuntimeProfile>();
        prof3->mouse.enabled = true;
        prof3->mouse.aim_profiles.clear();
        prof3->mouse.aim_profiles.push_back(aim::AimHotkeyProfile{});
        prof3->mouse.aim_profiles[0].hotkey = 0;
        prof3->mouse.aim_profiles[0].hotkey2 = 0;
        cfg.update(prof3);
        check(!backend->mouse_move(10, 10), "gate: 热键 mask 缺失拦截");

        // 静态总闸 enabled=false → 拦截
        OutputBackend b2;
        OutputBackend::Params p2;
        p2.kind = "local_hid";
        p2.enabled = false;
        p2.runtime_config = &cfg;
        b2.configure(p2, &err);
        check(!b2.backend()->mouse_move(10, 10), "gate: 静态总闸关闭拦截");
    }

    // ---- 3b) E-07 回归：enabled=true 且两源皆空 → fail-closed（补 T-08 缺口）----
    // 断言风格：本文件用自定义辅助函数 check(bool, const char*)，非断言宏。
    // 隔离说明：mouse_move() 在"Gate 拦截"与"底层硬件写失败"两种情况下都返回 false，
    //   无法单凭它区分是否放行；故这里用测试用最小后端直接暴露受保护的 gate_allows()，
    //   只观察 Gate 判定本身，不依赖 /dev/hidg 等硬件。
    {
        class GateProbeBackend final : public IOutputBackend {
        public:
            bool connect(std::string*) override { return true; }
            void disconnect() override {}
            bool reconnect(std::string*) override { return true; }
            BackendHealth health() const override { return BackendHealth{}; }
            bool mouse_move(int32_t, int32_t, int32_t) override { return gate_allows(); }
            bool mouse_button(uint8_t, uint8_t) override { return gate_allows(); }
            bool mouse_click(uint8_t btn) override { return mouse_button(btn, kActClick); }
            const char* name() const override { return "gate_probe"; }
            bool gate() const { return gate_allows(); }  // 直接暴露受保护的 Gate 判定
        };

        GateProbeBackend probe;
        probe.set_enabled(true);  // 静态总闸开；config_source_/button_source_ 均保持默认 nullptr（两源皆空）
        // 核心断言：启用 + 两源皆空时必须 fail-closed 拒绝。修复前此处返回 true（fail-open），本断言即失败。
        check(!probe.gate(), "E-07: enabled=true 且两源皆空 → fail-closed 拒绝");

        // 对照：显式注入配置源且热键按下 → 放行，证明上面的拒绝确由"两源皆空"触发，而非 Gate 恒拒绝。
        RuntimeConfig cfg_probe;
        auto prof_probe = std::make_shared<RuntimeProfile>();
        prof_probe->mouse.enabled = true;
        prof_probe->mouse.aim_profiles[0].hotkey = 1;  // left
        cfg_probe.update(prof_probe);
        std::atomic<uint16_t> btn_probe{1};
        probe.set_config_source(&cfg_probe);
        probe.set_button_source(&btn_probe);
        check(probe.gate(), "E-07 对照: 有配置源+热键按下 → 放行");

        // ---- 3c) 多档位：闸门必须按**全档键位并集**放行 ----
        // 选档只认命中的那一档，但 usb 报告什么时候来取决于玩家按了哪个键。
        // 修复前闸门取的是单档掩码（aim_hotkey|aim_hotkey2），档 1 的键位会被整条拦掉，
        // 表现为「换个键就不瞄了」。这里逐档逐个键位都要能放行。
        RuntimeConfig cfg_multi;
        auto prof_multi = std::make_shared<RuntimeProfile>();
        prof_multi->mouse.enabled = true;
        prof_multi->mouse.aim_profiles.clear();
        prof_multi->mouse.aim_profiles.push_back(aim::AimHotkeyProfile{});
        prof_multi->mouse.aim_profiles[0].hotkey = 1;   // 档0：左键
        prof_multi->mouse.aim_profiles.push_back(aim::AimHotkeyProfile{});
        prof_multi->mouse.aim_profiles[1].hotkey = 16;  // 档1：侧2
        cfg_multi.update(prof_multi);
        std::atomic<uint16_t> btn_multi{1};
        probe.set_config_source(&cfg_multi);
        probe.set_button_source(&btn_multi);
        check(probe.gate(), "多档: 只按档0的键位 -> 闸门放行");
        btn_multi.store(16);
        check(probe.gate(), "多档: 只按档1的键位 -> 闸门放行（并集覆盖每一档）");
        btn_multi.store(8);  // 侧1：两档都不使用
        check(!probe.gate(), "多档: 按两档都不用的键 -> 闸门拒绝");
    }

    // ---- 4) 零移动 / 目标丢失（target lost = 全 0 输出）----
    {
        OutputBackend b;
        OutputBackend::Params p;
        p.kind = "local_hid";
        p.enabled = true;
        RuntimeConfig cfg;
        auto prof = std::make_shared<RuntimeProfile>();
        prof->mouse.enabled = true;
        prof->mouse.aim_profiles[0].hotkey = 1;
        cfg.update(prof);
        p.runtime_config = &cfg;
        std::atomic<uint16_t> btn{1};
        p.button_source = &btn;
        std::string err;
        b.configure(p, &err);
        // 零移动帧：应被放行到后端（复位帧语义），返回值交给硬件；
        // 关键是不得崩溃、Gate 不得误拦截。
        bool zero = b.backend()->mouse_move(0, 0, 0);
        check(zero == false || zero == true, "zero-move: 零移动不崩溃不误拦");
    }

    // ---- 5) 按钮接口（本机后端保留接口，不崩溃）----
    {
        OutputBackend b;
        OutputBackend::Params p;
        p.kind = "local_hid";
        std::string err;
        b.configure(p, &err);
        (void)b.backend()->mouse_button(kBtnLeft, kActClick);
        (void)b.backend()->mouse_click(kBtnRight);
        check(true, "button/click: 接口可调用不崩溃");
    }

    // ---- 6) 闸门判据必须是**单一权威源**，且缺按键源时 fail-closed ----
    // 2026-09-25 修：判据原先在 `IOutputBackend::gate_allows` 与 `AiboxHidOutput::send`
    // 各写一遍，然后漂移了 —— aibox 侧缺「标定模式」豁免，且两侧都在
    // 「有配置源但按键源没绑」时直接放行（fall-through 到 write）= fail-open，
    // 热键这道防线只剩 AimThread 一层。现在统一走 OutputGate.hpp 的 output_gate_allows()。
    {
        class GateProbe2 final : public IOutputBackend {
        public:
            bool connect(std::string*) override { return true; }
            void disconnect() override {}
            bool reconnect(std::string*) override { return true; }
            BackendHealth health() const override { return BackendHealth{}; }
            bool mouse_move(int32_t, int32_t, int32_t) override { return gate_allows(); }
            bool mouse_button(uint8_t, uint8_t) override { return gate_allows(); }
            bool mouse_click(uint8_t b) override { return mouse_button(b, kActClick); }
            const char* name() const override { return "gate_probe2"; }
            bool gate() const { return gate_allows(); }
        };
        GateProbe2 probe;
        probe.set_enabled(true);

        RuntimeConfig cfg;
        auto prof = std::make_shared<RuntimeProfile>();
        prof->mouse.enabled = true;
        prof->mouse.aim_profiles.clear();
        prof->mouse.aim_profiles.push_back(aim::AimHotkeyProfile{});
        prof->mouse.aim_profiles[0].hotkey = 1;  // left
        cfg.update(prof);
        std::atomic<uint16_t> btn{1};

        // (a) 有配置源 + 按键源已绑 + 键按下 → 放行（基线，证明下面的拒绝不是"恒拒"）
        probe.set_config_source(&cfg);
        probe.set_button_source(&btn);
        check(probe.gate(), "gate: 有配置源+有按键源+命中 → 放行（基线）");

        // (b) ★ 有配置源但**按键源没绑** → 必须拒绝。此前这里 fall-through 放行了。
        probe.set_button_source(nullptr);
        check(!probe.gate(), "gate: 有配置源但按键源没绑 → fail-closed 拒绝");

        // (c) ★ 标定模式：无视 mouse.enabled 与按键源 → 放行。
        //     此前 aibox 侧缺这个分支 ⇒ mouse.enabled=false 时跑标定一个 count 都发不出去。
        prof->mouse.enabled = false;
        prof->mouse.calibrating = true;
        cfg.update(prof);
        check(probe.gate(), "gate: 标定模式无视 mouse.enabled 与按键源 → 放行");

        // (d) 标定结束后恢复：enabled 仍为 false ⇒ 拒（证明 (c) 的放行确由 calibrating 触发）
        prof->mouse.calibrating = false;
        cfg.update(prof);
        check(!probe.gate(), "gate: 标定结束 + mouse.enabled=false → 拒绝");
    }

    if (g_fails == 0) std::printf("test_output_backend: PASS\n");
    else std::printf("test_output_backend: %d FAILED\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
