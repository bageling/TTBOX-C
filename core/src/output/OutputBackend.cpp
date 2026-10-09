// OutputBackend.cpp — 设备选择器 + IHidOutput 兼容层
/*
 * TTBOX 文件说明
 *
 * 文件：OutputBackend.cpp
 *
 * 作用：
 *   将瞄准指令转换成真实的鼠标/外设输出。
 *
 * 小白理解：
 *   AimThread 算出了"应该往右移动 10 个像素"，
 *   OutputBackend 负责把这个指令发给 HID 设备，
 *   HID 设备再通过 USB 线告诉电脑："鼠标向右动 10 个像素"。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "output/OutputBackend.hpp"
#include "output/OutputGate.hpp"

#include <utility>
#include <chrono>
#include <mutex>

#include "common/Logger.hpp"
#include "model/RuntimeProfile.hpp"
#include "output/LocalHidBackend.hpp"
#include "output/MouseControlClient.hpp"

namespace {
// UsbProxyBackend：经 usb-proxy 的 mouse-control cmd.sock 输出（生产主链路）。
class UsbProxyBackend final : public ttbox::core::output::IOutputBackend {
public:
    // 构造：绑定 cmd.sock 路径。
    explicit UsbProxyBackend(std::string path) : client_(std::move(path)) {}
    // 建立到 usb-proxy 的连接。
    bool connect(std::string* e = nullptr) override { return client_.connect(e); }
    // 断开连接。
    void disconnect() override { client_.disconnect(); }
    // 重连：断开后计数并重新连接。
    bool reconnect(std::string* e = nullptr) override {
        disconnect();
        { std::lock_guard<std::mutex> lk(health_mtx_); ++health_.reconnect_count; }
        return connect(e);
    }
    // 本函数由 IPC / status 线程调用，而 mouse_move / mouse_button 在**控制线程**写同一份
    //   health_ —— 两边并发读写同一个结构（多字段非原子）即为数据竞争。
    //   故所有对 health_ 的访问都走 health_mtx_。临界区只有几个字段赋值，
    //   且 send_move/send_button 都刻意放在临界区**之外**，不把发送延迟与轮询耦合。
    ttbox::core::output::BackendHealth health() const override {
        const auto t = client_.telemetry();
        std::lock_guard<std::mutex> lk(health_mtx_);
        health_.state = t.connected ? ttbox::core::output::BackendState::kConnected : ttbox::core::output::BackendState::kDisconnected;
        health_.socket_write_ok = t.socket_write_ok;
        health_.socket_write_fail = t.socket_write_fail;
        health_.send_count = t.send_count;
        health_.last_dx = t.last_dx;
        health_.last_dy = t.last_dy;
        health_.last_wheel = t.last_wheel;
        health_.last_timestamp_us = t.last_timestamp_us;
        return health_;
    }
    // 发送相对移动（过 Gate）；记账写 health_。
    bool mouse_move(int32_t x, int32_t y, int32_t w = 0) override {
        if (!gate_allows()) return false;
        std::string error;
        const bool sent = client_.send_move(x, y, w, &error);
        std::lock_guard<std::mutex> lk(health_mtx_);
        if (!sent) { health_.detail = error; ++health_.send_fail; return false; }
        ++health_.send_ok;
        health_.last_send_ok_us = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        return true;
    }
    // 按键注入（自动扳机）：此前只做 gate 检查、一个字节都没发出去 ⇒ 面板上的
    // 「自动扳机」开了也点不动（TriggerController 决策全被丢在后端门口）。
    bool mouse_button(uint8_t button, uint8_t action) override {
        if (!gate_allows()) return false;
        std::string error;
        const bool sent = client_.send_button(button, action, &error);
        std::lock_guard<std::mutex> lk(health_mtx_);
        if (!sent) {
            health_.detail = error;
            ++health_.send_fail;
            return false;
        }
        ++health_.send_ok;
        return true;
    }
    // click = button(action=click)。
    bool mouse_click(uint8_t b) override { return mouse_button(b, ttbox::core::output::kActClick); }
    // 后端名。
    const char* name() const override { return "usb_proxy_mouse_control"; }
private:
    ttbox::core::output::MouseControlClient client_;
    mutable std::mutex health_mtx_;   // 保护 health_（控制线程写记账，IPC 线程读快照）
    mutable ttbox::core::output::BackendHealth health_;
};
}


namespace ttbox::core::output {

// 发送前 Gate：判据已抽到 OutputGate.hpp（**单一权威源**），与 AiboxHidOutput::send 共用同一份。
// 此前两处各写一遍然后漂移（aibox 侧缺标定豁免、两侧都在"按键源没绑"时 fail-open），
// 靠注释互相保证"口径一致"是不可靠的，改为共用函数。
bool IOutputBackend::gate_allows() const {
    return output_gate_allows(OutputGateInputs{enabled_, config_source_, button_source_});
}

// 析构：unique_ptr 自动释放后端。
OutputBackend::~OutputBackend() = default;

// 按 kind 创建具体后端、绑定 Gate 源并（在总闸开启时）预连接。
bool OutputBackend::configure(const Params& p, std::string* error) {
    params_ = p;
    backend_.reset();

    std::unique_ptr<IOutputBackend> backend;
    if (p.kind == "usb_proxy") {
        backend = std::make_unique<UsbProxyBackend>(p.proxy_socket_path);
    } else if (p.kind == "local_hid" || p.kind.empty()) {
        backend = std::make_unique<LocalHidBackend>(p.hidg_path);
    } else {
        if (error) *error = "未知输出后端: " + p.kind;
        return false;
    }
    backend->set_enabled(p.enabled);
    backend->set_button_source(p.button_source);
    backend->set_config_source(p.runtime_config);
    backend_ = std::move(backend);

    // 预连接：链路建立与注入门控解耦（connect ≠ 注入）。
    // 注入放行仍由 gate_allows() + 热键决定，这里只把管道先通上，
    // 让 health/遥测能反映真实链路，并消除首次按热键的建连延迟。
    if (p.enabled) {
        std::string connect_error;
        if (!backend_->connect(&connect_error)) {
            TTBOX_LOG_WARN("输出后端预连接失败（保持惰性重连）: " + connect_error);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// IHidOutput 兼容：AimThread 仍调用 send(OutputAction)，零改动。
// 热路径：无分配、无锁、无日志；Gate 判定在后端内部（与 AiboxHidOutput 相同）。
// ---------------------------------------------------------------------------
// 掩码 → 协议编号后转发；空掩码 fail-closed（不猜成左键）。
bool OutputBackend::mouse_button(uint8_t button, uint8_t action) {
    if (!backend_) return false;
    const uint8_t index = button_index_from_mask(button);
    if (index == 0) return false;   // 空掩码 = 不点任何键（fail-closed，绝不猜成"点左键"）
    return backend_->mouse_button(index, action);
}

// click = button(action=click)。
bool OutputBackend::mouse_click(uint8_t button) {
    return mouse_button(button, ttbox::core::output::kActClick);
}

// IHidOutput::send：转发为鼠标移动。
bool OutputBackend::send(const OutputAction& action) {
    if (!backend_) return false;
    // 行为与原 AiboxHidOutput 一致：整帧写入（含零移动帧=复位帧）；
    // 按键状态本机后端不注入（按钮接口保留给网络/串口后端）。
    return backend_->mouse_move(action.move_x, action.move_y, 0);
}

// 后端健康快照（无后端返回默认值）。
BackendHealth OutputBackend::health() const {
    return backend_ ? backend_->health() : BackendHealth{};
}

// 后端名字（无后端返回 "none"）。
const char* OutputBackend::backend_name() const {
    return backend_ ? backend_->name() : "none";
}

// 转发总闸到后端。
void OutputBackend::set_enabled(bool enabled) {
    if (backend_) backend_->set_enabled(enabled);
}

// 转发按键源到后端。
void OutputBackend::set_button_source(std::atomic<uint16_t>* source) {
    if (backend_) backend_->set_button_source(source);
}

// 转发配置源到后端。
void OutputBackend::set_config_source(RuntimeConfig* config) {
    if (backend_) backend_->set_config_source(config);
}

}  // namespace ttbox::core::output
