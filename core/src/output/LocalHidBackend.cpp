// LocalHidBackend.cpp — 本机 HID 后端（迁移自 AiboxHidOutput）
#include "output/LocalHidBackend.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <chrono>

namespace ttbox::core::output {

// 惰性打开 hidg（O_WRONLY|O_NONBLOCK）；Windows 恒 false。
bool LocalHidBackend::open_if_needed() {
#if defined(_WIN32)
    return false;
#else
    if (fd_ >= 0) return true;
    fd_ = ::open(path_.c_str(), O_WRONLY | O_NONBLOCK);
    return fd_ >= 0;
#endif
}

// 连接：置连接态并打开 hidg（失败置错误态并给错误信息）。
bool LocalHidBackend::connect(std::string* error) {
#if defined(_WIN32)
    if (error) *error = "Windows 无 V4L2/HID 硬件";
    health_.state = BackendState::kError;
    health_.detail = "unsupported platform";
    return false;
#else
    health_.state = BackendState::kConnecting;
    health_.detail = "opening " + path_;
    if (open_if_needed()) {
        health_.state = BackendState::kConnected;
        health_.detail = "connected";
        return true;
    }
    health_.state = BackendState::kError;
    health_.detail = std::string(std::strerror(errno));
    if (error) *error = "open " + path_ + " failed: " + std::string(std::strerror(errno));
    return false;
#endif
}

// 断开：关闭 fd 并置断开态。
void LocalHidBackend::disconnect() {
#if !defined(_WIN32)
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    health_.state = BackendState::kDisconnected;
    health_.detail = "disconnected";
}

// 重连：先断开并计数，再连接。
bool LocalHidBackend::reconnect(std::string* error) {
    disconnect();
    ++health_.reconnect_count;
    return connect(error);
}

// 健康快照（拷贝）。
BackendHealth LocalHidBackend::health() const { return health_; }

// 写 9 字节报告；短写 / EPIPE / ENXIO 视为失败（掉线则断开）。
bool LocalHidBackend::write_report(const unsigned char report[9]) {
#if defined(_WIN32)
    (void)report;
    return false;
#else
    const ssize_t n = ::write(fd_, report, 9);
    if (n == static_cast<ssize_t>(9)) return true;
    if (errno == EPIPE || errno == ENXIO) {
        disconnect();  // 设备掉线，下次发送前重开
    }
    return false;
#endif
}

// 发送移动报告：过 Gate → 惰性打开 → 写带当前按键状态的 9 字节报告，并记账。
bool LocalHidBackend::mouse_move(int32_t dx, int32_t dy, int32_t wheel) {
    if (!gate_allows()) return false;
    if (!open_if_needed()) return false;
    // 与 AiboxHidOutput 相同的 9 字节报告：
    // ReportID=2 + buttons(16bit LE) + X(int16 LE) + Y(int16 LE) + wheel + pan
    // ★ buttons 必须带上**当前按键状态**：写死 0 的话，按下左键后鼠标一动报告就说"已松开"。
    const unsigned char report[9] = {
        0x02,
        static_cast<unsigned char>(button_state_ & 0xff),
        static_cast<unsigned char>((button_state_ >> 8) & 0xff),
        static_cast<unsigned char>(dx & 0xff), static_cast<unsigned char>((dx >> 8) & 0xff),
        static_cast<unsigned char>(dy & 0xff), static_cast<unsigned char>((dy >> 8) & 0xff),
        static_cast<unsigned char>(wheel & 0xff), 0x00};
    const bool ok = write_report(report);
    if (ok) {
        ++health_.send_ok;
        health_.last_send_ok_us = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    } else {
        ++health_.send_fail;
    }
    return ok;
}

// 按键注入：button 是**协议编号**（1=左 … 5=前），映射成报告里的位掩码。
// 只发"状态变化"的那一份报告（按下 / 松开各一份），不 sleep ——
// 按压时长由调用方（AimThread 扳机）用 down→up 两条命令控制，后端不阻塞控制线程。
bool LocalHidBackend::mouse_button(uint8_t button, uint8_t action) {
    if (button < 1 || button > 16) return false;
    const uint16_t bit = static_cast<uint16_t>(1u << (button - 1));
    if (action == kActDown) {
        button_state_ |= bit;
    } else if (action == kActUp) {
        button_state_ &= static_cast<uint16_t>(~bit);
    } else if (action == kActClick) {
        // 一次完整点击：按下 → 抬起，两份报告连发（本机 gadget 路径非生产主链路，
        // 生产走 usb_proxy，按压时长由调用方拆成 down/up 两条命令）。
        button_state_ |= bit;
        (void)emit_button_report();
        button_state_ &= static_cast<uint16_t>(~bit);
    } else {
        return false;
    }
    return emit_button_report();
}

// 发送仅含按键状态的报告（过 Gate → 惰性打开 → 写报告；与 mouse_move 同款记账）。
bool LocalHidBackend::emit_button_report() {
    if (!gate_allows()) return false;
    if (!open_if_needed()) return false;
    const unsigned char report[9] = {
        0x02,
        static_cast<unsigned char>(button_state_ & 0xff),
        static_cast<unsigned char>((button_state_ >> 8) & 0xff),
        0, 0, 0, 0, 0, 0};
    if (!write_report(report)) {
        ++health_.send_fail;
        return false;
    }
    ++health_.send_ok;
    return true;
}

// click = button(action=click)。
bool LocalHidBackend::mouse_click(uint8_t button) {
    return mouse_button(button, kActClick);
}

}  // namespace ttbox::core::output