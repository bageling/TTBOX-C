// DmaBuf.hpp — DMA-BUF fd RAII 封装
//
// 生命周期要求（阶段 A-2）：
//   - 构造接管 fd，析构自动 close
//   - move-only（拷贝禁止，防止 double close）
//   - close() 幂等（已关闭再调用无害）
#pragma once

#include <cstdint>
#include <utility>

namespace ttbox::core {

// DmaBufFd：DMA-BUF 文件描述符的只移动（move-only）RAII 封装，析构自动 close。
class DmaBufFd {
public:
    // 默认构造：持有无效 fd（fd_ = -1）。
    DmaBufFd() = default;
    // 接管一个已打开的 DMA-BUF fd（所有权移交本对象，析构负责关闭）。
    DmaBufFd(int fd, uint32_t length) noexcept : fd_(fd), length_(length) {}
    // 析构：关闭持有的 fd（内部走幂等的 close()）。
    ~DmaBufFd() { close(); }

    DmaBufFd(const DmaBufFd&) = delete;
    DmaBufFd& operator=(const DmaBufFd&) = delete;

    // 移动构造 / 移动赋值：转移 fd 所有权，源对象置为无效，杜绝 double close。
    DmaBufFd(DmaBufFd&& other) noexcept { *this = std::move(other); }
    DmaBufFd& operator=(DmaBufFd&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            length_ = other.length_;
            other.fd_ = -1;
            other.length_ = 0;
        }
        return *this;
    }

    // 访问器：返回裸 fd / 字节长度 / 是否持有有效 fd。
    int fd() const noexcept { return fd_; }
    uint32_t length() const noexcept { return length_; }
    bool valid() const noexcept { return fd_ >= 0; }

    // 幂等关闭：fd 置 -1，防止 double close
    void close() noexcept;

private:
    // 持有的 fd（-1 = 无效）与对应的字节长度。
    int fd_ = -1;
    uint32_t length_ = 0;
};

}  // namespace ttbox::core
