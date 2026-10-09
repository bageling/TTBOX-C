// LocalHidBackend.hpp — 本机 HID 输出后端（迁移自 AiboxHidOutput，行为不变）
#pragma once

#include "output/OutputBackend.hpp"
#include <string>

namespace ttbox::core::output {

// 本机 /dev/hidg0 直接输出。报告格式与 AiboxHidOutput 完全一致：
//   buttons(16bit LE) + X(int16 LE) + Y(int16 LE) + wheel(8) + pan(8)，共 9 字节。
// Hotkey Gate / mouse.enabled 实时判定在基类 gate_allows() 中（与 AiboxHidOutput 相同）。
class LocalHidBackend final : public IOutputBackend {
public:
    // 构造：记录 hidg 设备路径（不打开）。
    explicit LocalHidBackend(std::string hidg_path = "/dev/hidg1")
        : path_(std::move(hidg_path)) {}
    // 析构：断开连接（关 fd）。
    ~LocalHidBackend() override { disconnect(); }

    // 持有裸 fd + 析构里 disconnect() ⇒ 不可拷贝，否则复制即 double-close（fd 会被内核复用）。
    LocalHidBackend(const LocalHidBackend&) = delete;
    LocalHidBackend& operator=(const LocalHidBackend&) = delete;

    // 打开 hidg 建立连接。
    bool connect(std::string* error = nullptr) override;
    // 关闭 fd 并置断开态。
    void disconnect() override;
    // 断开后重连（计数）。
    bool reconnect(std::string* error = nullptr) override;
    // 健康快照。
    BackendHealth health() const override;

    // 发送相对移动报告（含当前按键状态）。
    bool mouse_move(int32_t dx, int32_t dy, int32_t wheel = 0) override;
    // 发送按键状态变化报告（down/up/click）。
    bool mouse_button(uint8_t button, uint8_t action) override;
    // click = button(action=click)。
    bool mouse_click(uint8_t button) override;

    // 后端名。
    const char* name() const override { return "local_hid"; }

private:
    // 惰性打开 hidg（已打开则直接返回 true）。
    bool open_if_needed();
    // 写 9 字节报告；EPIPE/ENXIO 视为掉线并断开。
    bool write_report(const unsigned char report[9]);
    bool emit_button_report();  // 只带按键状态变化的报告（dx=dy=wheel=0）

    // hidg 设备路径。
    std::string path_;
    // 设备 fd（-1 = 未打开）。
    int fd_ = -1;
    // 健康/遥测快照。
    mutable BackendHealth health_;
    // 当前按下的按钮掩码。为什么后端自己要存：**每份报告都要带上按键状态** ——
    // 此前 mouse_move 硬编码 buttons=0 ⇒ 按下左键后只要鼠标一动，报告就把按键写成"全松开"，
    // 表现为"点了没反应 / 一移动就断"。按键与位移在同一份报告里，必须一起维护。
    uint16_t button_state_ = 0;
};

}  // namespace ttbox::core::output
