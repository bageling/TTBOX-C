// FifoHidOutput.hpp — 兼容现有 ttbox-hid-bridge 的 FIFO 输出后端。
#pragma once
#include "output/IHidOutput.hpp"
#include <string>
namespace ttbox::core::output {
// FifoHidOutput：向 ttbox-hid-bridge FIFO 输出的旧后端（0x01 移动帧协议）。
class FifoHidOutput final : public IHidOutput {
public:
    // 构造：记录 FIFO 路径。
    explicit FifoHidOutput(std::string path) : path_(std::move(path)) {}
    // 析构：close()。
    ~FifoHidOutput() override { close(); }
    // 持有裸 fd + 析构里 close() ⇒ 不可拷贝，否则复制即 double-close（fd 会被内核复用）。
    FifoHidOutput(const FifoHidOutput&) = delete;
    FifoHidOutput& operator=(const FifoHidOutput&) = delete;
    // 发送移动帧（先补发门控帧）。
    bool send(const OutputAction& action) override;
    // 关闭 FIFO（关闭前下发关闭门控帧，幂等）。
    void close();
private:
    // 惰性打开 FIFO 写端。
    bool open_if_needed();
    // 发送 0x02 门控帧（开启 Bridge 的 AI 注入）。
    bool send_control();
    // FIFO 路径。
    std::string path_;
    // FIFO fd（-1 = 未打开）。
    int fd_ = -1;
    // 本会话是否已下发过门控帧（close 时决定是否补发关闭帧）。
    bool control_sent_ = false;
};
}
