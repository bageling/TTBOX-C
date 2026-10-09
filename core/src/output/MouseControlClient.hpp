// MouseControlClient.hpp — usb-proxy 官方 mouse-control 薄客户端
// 只负责 MOVE 包编码与 cmd.sock 写入，不直接访问 HID/raw-gadget。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "common/Paths.hpp"   // A-PATH-5：mouse cmd.sock 默认单点真源

namespace ttbox::core::output {

// usb-proxy 客户端遥测快照（供 Web / 排障展示）。
struct MouseControlTelemetry {
    bool connected = false;
    uint64_t socket_write_ok = 0;
    uint64_t socket_write_fail = 0;
    uint64_t send_count = 0;
    uint64_t button_count = 0;      // 按键命令成功投递数（诊断：扳机到底点没点出去）
    int32_t last_dx = 0;
    int32_t last_dy = 0;
    int32_t last_wheel = 0;
    uint64_t last_timestamp_us = 0;
};

// MouseControlClient：usb-proxy mouse-control 的薄客户端（MOVE/BUTTON 包编码 + cmd.sock 写入）。
class MouseControlClient {
public:
    // 构造：记录 cmd.sock 路径。
    explicit MouseControlClient(std::string socket_path = paths::kMouseCmdSocketDefault)
        : socket_path_(std::move(socket_path)) {}
    // 析构：disconnect()。
    ~MouseControlClient();

    MouseControlClient(const MouseControlClient&) = delete;
    MouseControlClient& operator=(const MouseControlClient&) = delete;

    // 编码 MOVE 包（magic 0x4F50 + ver + type 4 + request_id + dx/dy/wheel）。
    static std::vector<uint8_t> encode_move(uint32_t request_id, int32_t dx, int32_t dy, int32_t wheel = 0);
    // BUTTON_CMD（type=5）：payload = <B button编号(1..8), B action(down/up/click)>
    static std::vector<uint8_t> encode_button(uint32_t request_id, uint8_t button, uint8_t action);
    // 连接 cmd.sock（SOCK_SEQPACKET）。
    bool connect(std::string* error = nullptr);
    // 关闭 socket（幂等）。
    void disconnect();
    // 发送 MOVE 包；首帧失败断开重连重发一次。
    bool send_move(int32_t dx, int32_t dy, int32_t wheel = 0, std::string* error = nullptr);
    // 发送 BUTTON_CMD 包；首帧失败断开重连重发一次。
    bool send_button(uint8_t button, uint8_t action, std::string* error = nullptr);
    // 取遥测快照（原子读，线程安全）。
    MouseControlTelemetry telemetry() const;
    // 是否已连接。
    bool connected() const { return fd_.load(std::memory_order_relaxed) >= 0; }
    // 下一个请求序号。
    uint32_t next_request_id() const { return next_request_id_.load(std::memory_order_relaxed); }

private:
    // cmd.sock 路径。
    std::string socket_path_;
    // fd_ 与 next_request_id_ 由**控制线程**写（connect / disconnect / send_*），
    // 却被**IPC / status 线程**经 telemetry() / connected() / next_request_id() 读
    // （路径：IPC GET_STATUS → IOutputBackend::health() → client_.telemetry()）。
    // 非原子 int ⇒ 数据竞争（UB），撕裂的 fd 还可能被 ::close 到复用后的别的连接上。
    // 改成 std::atomic 后，线程内原有的 `fd_ >= 0` / `++next_request_id_` 写法靠
    // atomic 的隐式转换仍然成立（仍是原子操作）；跨线程读点一律显式 .load()，避免
    // 读者误以为那是普通读。
    std::atomic<int> fd_{-1};
    std::atomic<uint32_t> next_request_id_{1};
    std::atomic<uint64_t> socket_write_ok_{0};
    std::atomic<uint64_t> socket_write_fail_{0};
    std::atomic<uint64_t> send_count_{0};
    std::atomic<uint64_t> button_count_{0};
    std::atomic<int32_t> last_dx_{0};
    std::atomic<int32_t> last_dy_{0};
    std::atomic<int32_t> last_wheel_{0};
    std::atomic<uint64_t> last_timestamp_us_{0};
};

}  // namespace ttbox::core::output
