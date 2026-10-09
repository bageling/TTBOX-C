// FifoHidOutput.cpp — 保持现有移动帧协议：0x01 + dx(int16 LE) + dy(int16 LE)。
#include "output/FifoHidOutput.hpp"
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif
namespace ttbox::core::output {
// 惰性打开 FIFO 写端（O_WRONLY|O_NONBLOCK）；打开成功即复位门控标记。
bool FifoHidOutput::open_if_needed() {
#if defined(_WIN32)
    return false;
#else
    if (fd_ >= 0) return true;
    fd_ = ::open(path_.c_str(), O_WRONLY | O_NONBLOCK);
    control_sent_ = false;
    return fd_ >= 0;
#endif
}
bool FifoHidOutput::send_control() {
#if defined(_WIN32)
    return false;
#else
    // 旧 bridge 协议：0x02 + flags + hotkey；bit0=AI enabled，默认右键 0x02。
    const unsigned char frame[6] = {0x02, 0x01, 0x03, 0x00, 0x00, 0x00};
    const ssize_t n = ::write(fd_, frame, sizeof(frame));
    if (n == static_cast<ssize_t>(sizeof(frame))) { control_sent_ = true; return true; }
    if (errno == EPIPE || errno == ENXIO) close();
    return false;
#endif
}
// 关闭 FIFO：先下发关闭门控帧（禁用注入），再关 fd（幂等）。
void FifoHidOutput::close() {
#if !defined(_WIN32)
    if (fd_ >= 0 && control_sent_) {
        // 停止/析构前明确关闭 Bridge AI 门控，禁止残留状态继续注入。
        const unsigned char disable_frame[6] = {0x02, 0x00, 0x03, 0x00, 0x00, 0x00};
        const ssize_t ignored = ::write(fd_, disable_frame, sizeof(disable_frame));
        (void)ignored;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    control_sent_ = false;
#endif
}
// 发送移动帧：先补发门控帧（状态自描述），再写坐标取反后的 0x01 帧。
bool FifoHidOutput::send(const OutputAction& a) {
#if defined(_WIN32)
    (void)a; return false;
#else
    if (!open_if_needed()) return false;
    // Bridge 的门控状态可能在其重启后复位；每个动作前重发控制帧，保证状态自描述。
    if (!send_control()) return false;
    // 设备坐标系与 AimError 相反：目标在右/下时，hid bridge 需要负向报告。
    // 只在输出边界转换，不改变上层检测、误差和 PID 的数学符号约定。
    const int16_t device_x = static_cast<int16_t>(-a.move_x);
    const int16_t device_y = static_cast<int16_t>(-a.move_y);
    const unsigned char frame[5] = {0x01, static_cast<unsigned char>(device_x & 0xff),
        static_cast<unsigned char>((device_x >> 8) & 0xff), static_cast<unsigned char>(device_y & 0xff),
        static_cast<unsigned char>((device_y >> 8) & 0xff)};
    const ssize_t n = ::write(fd_, frame, sizeof(frame));
    if (n == static_cast<ssize_t>(sizeof(frame))) return true;
    if (errno == EPIPE || errno == ENXIO) close();
    return false;
#endif
}
}
