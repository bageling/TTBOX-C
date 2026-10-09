// MouseControlClient.cpp — 官方 usb-proxy MOVE 包编码与发送
#include "output/MouseControlClient.hpp"

#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#endif

namespace ttbox::core::output {
namespace {
// 小端写入 u16 / u32 / i32 到字节缓冲。
void put_u16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); }
void put_u32(std::vector<uint8_t>& out, uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (i * 8))); }
void put_i32(std::vector<uint8_t>& out, int32_t v) { put_u32(out, static_cast<uint32_t>(v)); }
}

// 析构：断开连接。
MouseControlClient::~MouseControlClient() { disconnect(); }

// 编码 MOVE 包（共 20 字节）。
std::vector<uint8_t> MouseControlClient::encode_move(uint32_t request_id, int32_t dx, int32_t dy, int32_t wheel) {
    std::vector<uint8_t> out;
    out.reserve(20);
    put_u16(out, 0x4F50);
    out.push_back(1);
    out.push_back(4);
    put_u32(out, request_id);
    put_i32(out, dx);
    put_i32(out, dy);
    put_i32(out, wheel);
    return out;
}

// 编码 BUTTON_CMD 包（type=5，共 10 字节）。
std::vector<uint8_t> MouseControlClient::encode_button(uint32_t request_id, uint8_t button,
                                                       uint8_t action) {
    std::vector<uint8_t> out;
    out.reserve(10);
    put_u16(out, 0x4F50);
    out.push_back(1);          // version
    out.push_back(5);          // type = BUTTON_CMD（与 usb-proxy mouse_control.hpp 对齐）
    put_u32(out, request_id);
    out.push_back(button);     // 按钮**编号** 1..8（1=左键）
    out.push_back(action);     // 1=down 2=up 3=click
    return out;
}

// 连接 usb-proxy 的 cmd.sock（AF_UNIX / SOCK_SEQPACKET）。
bool MouseControlClient::connect(std::string* error) {
#if defined(_WIN32)
    if (error) *error = "Windows 不支持 Unix SOCK_SEQPACKET";
    return false;
#else
    if (fd_ >= 0) return true;
    fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd_ < 0) { if (error) *error = std::strerror(errno); return false; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        if (error) *error = "socket 路径过长";
        disconnect();
        return false;
    }
    std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
    if (::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error) *error = std::strerror(errno);
        disconnect();
        return false;
    }
    return true;
#endif
}

// 汇总各原子字段为遥测快照（跨线程读点显式原子读）。
MouseControlTelemetry MouseControlClient::telemetry() const {
    MouseControlTelemetry t;
    // 跨线程读点（控制线程会改 fd_：断线重连时 close + 重开）⇒ 显式原子读。
    t.connected = fd_.load(std::memory_order_relaxed) >= 0;
    t.socket_write_ok = socket_write_ok_.load(std::memory_order_relaxed);
    t.socket_write_fail = socket_write_fail_.load(std::memory_order_relaxed);
    t.send_count = send_count_.load(std::memory_order_relaxed);
    t.button_count = button_count_.load(std::memory_order_relaxed);
    t.last_dx = last_dx_.load(std::memory_order_relaxed);
    t.last_dy = last_dy_.load(std::memory_order_relaxed);
    t.last_wheel = last_wheel_.load(std::memory_order_relaxed);
    t.last_timestamp_us = last_timestamp_us_.load(std::memory_order_relaxed);
    return t;
}

// 关闭 socket（幂等；Windows 为空实现）。
void MouseControlClient::disconnect() {
#if !defined(_WIN32)
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
}

// 发送 MOVE 包；失败断开重连后重发一次（应对 usb-proxy 刚重启）。
bool MouseControlClient::send_move(int32_t dx, int32_t dy, int32_t wheel, std::string* error) {
#if defined(_WIN32)
    (void)dx; (void)dy; (void)wheel; if (error) *error = "Windows 不支持 Unix socket"; return false;
#else
    if (fd_ < 0 && !connect(error)) return false;
    const auto packet = encode_move(next_request_id_, dx, dy, wheel);
    auto record_success = [&]() {
        ++next_request_id_;
        if (next_request_id_ == 0) next_request_id_ = 1;
        socket_write_ok_.fetch_add(1, std::memory_order_relaxed);
        last_dx_.store(dx, std::memory_order_relaxed);
        last_dy_.store(dy, std::memory_order_relaxed);
        last_wheel_.store(wheel, std::memory_order_relaxed);
        last_timestamp_us_.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()), std::memory_order_relaxed);
        return true;
    };
    auto do_send = [&]() -> ssize_t {
        return ::send(fd_, packet.data(), packet.size(), MSG_NOSIGNAL);
    };
    send_count_.fetch_add(1, std::memory_order_relaxed);
    ssize_t n = do_send();
    if (n != static_cast<ssize_t>(packet.size())) {
        socket_write_fail_.fetch_add(1, std::memory_order_relaxed);
        if (error) *error = std::strerror(errno);
        disconnect();
        // 首帧失败大概率是 usb-proxy 刚重启/换线：断开后立即重连重发，
        // 避免热键第一帧注入丢失（等下一帧再重连至少要空跑一个轮询周期）。
        if (connect(error)) {
            n = do_send();
            if (n == static_cast<ssize_t>(packet.size())) return record_success();
            socket_write_fail_.fetch_add(1, std::memory_order_relaxed);
            if (error) *error = std::strerror(errno);
            disconnect();
        }
        return false;
    }
    return record_success();
#endif
}

// 发送 BUTTON_CMD 包（button 1..8）；失败断开重连后重发一次。
bool MouseControlClient::send_button(uint8_t button, uint8_t action, std::string* error) {
    if (button < 1 || button > 8) {
        if (error) *error = "按钮编号超出 1..8";
        return false;
    }
#if defined(_WIN32)
    (void)action; if (error) *error = "Windows 不支持 Unix socket"; return false;
#else
    if (fd_ < 0 && !connect(error)) return false;
    const auto packet = encode_button(next_request_id_, button, action);
    auto finish = [&]() {
        ++next_request_id_;
        if (next_request_id_ == 0) next_request_id_ = 1;
        socket_write_ok_.fetch_add(1, std::memory_order_relaxed);
        button_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    auto do_send = [&]() -> ssize_t {
        return ::send(fd_, packet.data(), packet.size(), MSG_NOSIGNAL);
    };
    ssize_t n = do_send();
    if (n != static_cast<ssize_t>(packet.size())) {
        socket_write_fail_.fetch_add(1, std::memory_order_relaxed);
        if (error) *error = std::strerror(errno);
        disconnect();
        // 与 send_move 同款：首帧失败多为 usb-proxy 刚重启 ⇒ 断线重连后立刻重发一次。
        if (connect(error)) {
            n = do_send();
            if (n == static_cast<ssize_t>(packet.size())) return finish();
            socket_write_fail_.fetch_add(1, std::memory_order_relaxed);
            if (error) *error = std::strerror(errno);
            disconnect();
        }
        return false;
    }
    return finish();
#endif
}

}  // namespace ttbox::core::output
