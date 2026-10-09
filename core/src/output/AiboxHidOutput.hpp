// AiboxHidOutput.hpp — AIBOX 兼容的 /dev/hidg0 直接输出后端
// 报告格式: buttons(16bit LE) + X(int16 LE) + Y(int16 LE) + wheel(8) + pan(8)
#pragma once
#include "output/IHidOutput.hpp"
#include <string>
#include <atomic>
namespace ttbox::core { class RuntimeConfig; }
namespace ttbox::core::output {
// AiboxHidOutput：直接写 /dev/hidg 的 AIBOX 兼容输出后端。
class AiboxHidOutput final : public IHidOutput {
public:
    // 构造：记录 hidg 设备路径（不打开）。
    explicit AiboxHidOutput(std::string hidg_path = "/dev/hidg1") : path_(std::move(hidg_path)) {}
    // 析构：close()。
    ~AiboxHidOutput() override { close(); }
    // 持有裸 fd + 析构里 close() ⇒ 必须不可拷贝：复制一份就等于两个对象关同一个 fd
    // （而 fd 会被内核复用，结果是关错对象）。语义上也无需拷贝（都指向同一路 hidg）。
    AiboxHidOutput(const AiboxHidOutput&) = delete;
    AiboxHidOutput& operator=(const AiboxHidOutput&) = delete;
    // 写入 9 字节鼠标报告（过 Gate 判定）。
    bool send(const OutputAction& action) override;
    // 静态总闸（output_enabled / 外部 kill 开关）；mouse.enabled 由配置实时判定。
    void set_enabled(bool enabled) { enabled_ = enabled; }
    // 保险门按钮源；放行掩码不再由调用方传入（禁止写死），每次发送时从 config_source_ 实时读取。
    void set_button_source(std::atomic<uint16_t>* source) { button_source_ = source; }
    // 绑定运行时配置：热键 mask 与 mouse.enabled 每次发送时取实时快照，改配置即时生效。
    void set_config_source(ttbox::core::RuntimeConfig* config) { config_source_ = config; }
    // 关闭持有的 fd（幂等）。
    void close();
private:
    // 惰性打开 hidg（已打开则直接返回 true）。
    bool open_if_needed();
    // hidg 设备路径。
    std::string path_;
    // 设备 fd（-1 = 未打开）。
    int fd_ = -1;
    // 静态总闸（output_enabled / kill switch）。
    bool enabled_ = false;
    // 物理按键位图源（nullptr = 未绑）。
    std::atomic<uint16_t>* button_source_ = nullptr;
    // 运行期配置源（读 mouse.enabled / 热键）。
    ttbox::core::RuntimeConfig* config_source_ = nullptr;
};
}  // namespace ttbox::core::output
