// HidRuntime.hpp — A9-P2 HID Runtime 实现（IHidRuntime）
//
// 职责：
//   - 管理 hidraw 读取 → HidForwarder（SPSC 队列 + TX 到 hidg）
//   - 管理 gadget 生命周期（configfs，通过脚本/直接 sysfs 写入）
//   - 提供 IHidRuntime 接口给 AI Runtime
// 边界：本模块持有 /dev/hidraw*、/dev/hidg*、configfs 的全部访问；
//       AI Runtime 只通过 IHidRuntime 访问。
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hid/HidForwarder.hpp"
#include "hid/HidPackageConfig.hpp"
#include "hid/HidTypes.hpp"
#include "hid/IHidRuntime.hpp"

namespace ttbox::core {

class HidRuntime : public IHidRuntime {
public:
    HidRuntime() = default;
    // ★ 本类**不要派生**：析构里调虚函数 stop()（cppcheck virtualCallInConstructor 告警）。
    //   C++ 规定：派生类析构期间动态派发已停止到基类 ⇒ 派生类的 stop() **不会**被执行，
    //   派生类自己打开的句柄会在析构里泄漏。
    //   ⇒ 真要派生时，改成"基类析构只清基类资源，派生资源由派生类析构自己清"。
    //   同类问题另见 capture/V4L2Capture.hpp（stop/close）。
    //   （对比：output/LocalHidBackend 标了 final ⇒ 天然免疫。）
    ~HidRuntime() override { stop(); }

    // 指定 HID 包根目录（默认 TTBOX_PROJECT_ROOT/hid）
    void set_root(std::string root) { root_ = std::move(root); }

    // IHidRuntime
    bool start(std::string* error = nullptr) override;
    void stop() override;
    HidRuntimeStatus status() const override { return status_; }
    MouseState get_mouse_state() const override;
    KeyboardState get_keyboard_state() const override;
    HidRuntimeMetrics get_metrics() const override;

    // 注入配置（否则从 <root>/config/hid_config.json 加载）
    void set_config(HidPackageConfig cfg) { cfg_ = std::move(cfg); }

private:
    // 确保 USB gadget 已建立（已绑定则跳过，否则走启动脚本）
    bool setup_gadget_if_needed(std::string* error);
    // 定位键盘/鼠标 hidraw：优先配置，其次按 input 名称/HID_PHYS 关键词枚举
    bool find_hidraw(const std::string& configured, const char* suffix,
                     std::string* out) const;
    // 从 forwarder 统计刷新最近鼠标/键盘状态（当前为占位空实现）
    void parse_state_from_forwarders();

    std::string root_;
    HidPackageConfig cfg_;
    std::atomic<HidRuntimeStatus> status_{HidRuntimeStatus::kStopped};
    std::vector<std::unique_ptr<HidForwarder>> forwarders_;  // 键盘/鼠标各一路
    mutable std::mutex state_mtx_;                            // 保护 last_mouse_/last_keyboard_
    MouseState last_mouse_;
    KeyboardState last_keyboard_;
    HidRuntimeMetrics metrics_;
};

}  // namespace ttbox::core
