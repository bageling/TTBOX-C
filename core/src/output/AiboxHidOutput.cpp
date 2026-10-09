// AiboxHidOutput.cpp — AIBOX 兼容鼠标报告写入 /dev/hidg0
#include "output/AiboxHidOutput.hpp"
#include "output/OutputGate.hpp"
#include "model/RuntimeProfile.hpp"
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif
namespace ttbox::core::output {
// 惰性打开 hidg（O_WRONLY|O_NONBLOCK）；Windows 恒 false。
bool AiboxHidOutput::open_if_needed() {
#if defined(_WIN32)
    return false;
#else
    if (fd_ >= 0) return true;
    fd_ = ::open(path_.c_str(), O_WRONLY | O_NONBLOCK);
    return fd_ >= 0;
#endif
}
// 关闭 fd（幂等）。
void AiboxHidOutput::close() {
#if !defined(_WIN32)
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
}
// 组装并写入 9 字节鼠标报告（先过 Gate；EPIPE/ENXIO 视为掉线并关闭 fd）。
bool AiboxHidOutput::send(const OutputAction& a) {
#if defined(_WIN32)
    (void)a; return false;
#else
    // 静态总闸 + 配置实时保险门：判据已抽到 OutputGate.hpp 的 output_gate_allows()，
    // 与 IOutputBackend::gate_allows **共用同一份**（此前两处各写一遍然后漂移：
    // 本侧缺「标定模式」豁免、且按键源没绑时直接放行 = fail-open）。
    // 改配置/热键后无需重启即时生效，因为判据每次发送都重读快照。
    if (!output_gate_allows(OutputGateInputs{enabled_, config_source_, button_source_})) {
        return false;
    }
    if (!open_if_needed()) return false;
    // 当前 gadget 鼠标描述符要求 9 字节：ReportID=2 + buttons(16bit LE)
    // + X(int16 LE) + Y(int16 LE) + wheel + pan。
    // 只注入鼠标移动，不改按钮状态。
    const unsigned char report[9] = {
        0x02, 0x00, 0x00,
        static_cast<unsigned char>(a.move_x & 0xff), static_cast<unsigned char>((a.move_x >> 8) & 0xff),
        static_cast<unsigned char>(a.move_y & 0xff), static_cast<unsigned char>((a.move_y >> 8) & 0xff),
        0x00, 0x00};
    const ssize_t n = ::write(fd_, report, sizeof(report));
    if (n == static_cast<ssize_t>(sizeof(report))) return true;
    if (errno == EPIPE || errno == ENXIO) close();
    return false;
#endif
}
}  // namespace ttbox::core::output
