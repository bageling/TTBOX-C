// HidForwarder.cpp — A9 HID 透传转发器实现
/*
 * TTBOX 文件说明
 *
 * 文件：HidForwarder.cpp
 *
 * 作用：
 *   HID 报告转发器，将 AI 生成的鼠标指令转发到 USB 设备。
 *   使用无锁队列（SPSC Queue）实现高性能转发。
 *
 * 小白理解：
 *   这是从 AI 到 USB 鼠标的最后一公里。
 *   它用一个高效的队列把鼠标指令排队，然后一个一个地发出去。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "hid/HidForwarder.hpp"

#if defined(_WIN32)
namespace ttbox::core {
HidForwarder::~HidForwarder() = default;
bool HidForwarder::start(const Params&, std::string* error) {
    if (error) *error = "Windows 不支持 HID 透传（A9 目标 RK3588）";
    return false;
}
void HidForwarder::stop() {}
void HidForwarder::rx_loop() {}
void HidForwarder::tx_loop() {}
uint64_t HidForwarder::now_us() const { return 0; }
bool HidForwarder::set_affinity(std::thread&, int) { return false; }
}  // namespace ttbox::core
#else

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "common/Logger.hpp"

namespace ttbox::core {

using clock = std::chrono::steady_clock;

// 析构：确保 RX/TX 线程停止并回收（内部调用 stop）。
HidForwarder::~HidForwarder() {
    stop();
}

// 返回 steady_clock 的单调微秒时间戳（供 report 计算驻留时延）。
uint64_t HidForwarder::now_us() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch())
            .count());
}

// 把线程绑定到指定 CPU（cpu<0 表示不绑核，直接视为成功）。
bool HidForwarder::set_affinity(std::thread& t, int cpu) {
    if (cpu < 0) return true;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(static_cast<int>(cpu), &cs);
    return pthread_setaffinity_np(t.native_handle(), sizeof(cs), &cs) == 0;
}

// 启动转发器：校验路径、预打开设备、建队列并拉起 RX/TX 两个线程。
bool HidForwarder::start(const Params& params, std::string* error) {
    if (running_.load()) {
        if (error) *error = "HidForwarder 已在运行";
        return false;
    }
    if (params.hidraw_path.empty() || params.hidg_path.empty()) {
        if (error) *error = "需要 hidraw（输入）与 hidg（输出）路径";
        return false;
    }
    params_ = params;
    queue_ = std::make_unique<SpscQueue<HidReport, 1024>>();

    // 预校验可打开（避免线程启动后才发现）
    int in = ::open(params.hidraw_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (in < 0) {
        if (error) *error = "打开 hidraw 失败(" + params.hidraw_path + "): " + std::strerror(errno);
        return false;
    }
    ::close(in);
    int out = ::open(params.hidg_path.c_str(), O_WRONLY | O_NONBLOCK);
    if (out < 0) {
        if (error) *error = "打开 hidg 失败(" + params.hidg_path + "): " + std::strerror(errno);
        return false;
    }
    ::close(out);

    running_.store(true);
    rx_thread_ = std::thread(&HidForwarder::rx_loop, this);
    tx_thread_ = std::thread(&HidForwarder::tx_loop, this);
    if (params_.cpu >= 0) {
        set_affinity(rx_thread_, params_.cpu);
        set_affinity(tx_thread_, params_.cpu);
    }
    TTBOX_LOG_INFO("HidForwarder 启动: " + params_.hidraw_path + " -> " + params_.hidg_path +
                   " kind=" + std::to_string(static_cast<int>(params_.kind)) +
                   " cpu=" + std::to_string(params_.cpu));
    return true;
}

// 停止转发器：置停止标志并 join RX/TX 线程（允许重复调用）。
// ★ 不能按 running_ 早退：RX/TX 线程在 open 失败时会自行置 running_=false，
//   若此处 `exchange` 返回 false 就直接 return，则两个线程永不 join，
//   仍 joinable 的 std::thread 会在成员析构时触发 std::terminate()。
//   故无条件 join（对已结束但未 join 的线程调用 join 合法，立即返回）。
void HidForwarder::stop() {
    running_.store(false);
    if (rx_thread_.joinable()) rx_thread_.join();
    if (tx_thread_.joinable()) tx_thread_.join();
}

// RX 线程主体：poll + read hidraw，把报告推入 SPSC 队列。
void HidForwarder::rx_loop() {
    const int fd = ::open(params_.hidraw_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        stats_.rx_errors.fetch_add(1);
        TTBOX_LOG_ERROR("hidraw 打开失败: " + params_.hidraw_path);
        running_.store(false);
        return;
    }
    HidReport rep;
    struct pollfd pfd {fd, POLLIN, 0};
    while (running_.load()) {
        const int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            stats_.rx_errors.fetch_add(1);
            break;
        }
        if (pr == 0) continue;  // 超时（running 检查）
        if (!(pfd.revents & POLLIN)) continue;
        const ssize_t n = ::read(fd, rep.data.data(), rep.data.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            stats_.rx_errors.fetch_add(1);
            break;
        }
        if (n == 0) continue;
        rep.size = static_cast<uint8_t>(n);
        rep.kind = static_cast<uint8_t>(params_.kind);
        rep.timestamp_us = now_us();
        rep.seq = static_cast<uint32_t>(stats_.rx_reports.load(std::memory_order_relaxed));
        stats_.rx_reports.fetch_add(1);
        // 间隔（估算回报率）
        if (last_rx_us_ != 0) {
            stats_.rx_interval_us.add(rep.timestamp_us - last_rx_us_);
        }
        last_rx_us_ = rep.timestamp_us;
        if (!queue_->try_push(rep)) {
            stats_.push_drops.fetch_add(1);  // 队列满（理论不会，TX 足够快）
        }
        // 更新峰值深度
        const size_t qd = queue_->size();
        uint64_t cur = stats_.max_queue_depth.load();
        while (qd > cur && !stats_.max_queue_depth.compare_exchange_weak(cur, qd)) {
        }
    }
    ::close(fd);
}

// TX 线程主体：从队列取报告，按需重编码后写入 hidg。
void HidForwarder::tx_loop() {
    const int fd = ::open(params_.hidg_path.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        stats_.tx_errors.fetch_add(1);
        TTBOX_LOG_ERROR("hidg 打开失败: " + params_.hidg_path);
        running_.store(false);
        return;
    }
    HidReport rep;
    while (running_.load()) {
        if (!queue_->try_pop(&rep)) {
            std::this_thread::yield();  // 空队列让出（低占 CPU）
            continue;
        }
        uint8_t tx_data[16];
        size_t tx_size = rep.size;
        std::memcpy(tx_data, rep.data.data(), rep.size);
        if (!params_.raw_pass) {
            if (!reencode(rep, tx_data, &tx_size)) {
                continue;  // 非鼠标报告（如 consumer/system）：丢弃不转发
            }
        }
        // RAW 透传：原始 report 直接写入 gadget
        const ssize_t w = ::write(fd, tx_data, tx_size);
        if (w < 0) {
            // 主机未枚举/队列背压：丢弃并计数（不阻塞，不影响 RX）
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENODEV) {
                stats_.tx_backpressure.fetch_add(1);
            } else {
                stats_.tx_errors.fetch_add(1);
            }
            continue;
        }
        stats_.tx_reports.fetch_add(1);
        stats_.latency_us.add(now_us() - rep.timestamp_us);
    }
    ::close(fd);
}

// ---------------------------------------------------------------------------
// reencode：把真实键鼠报告适配为 gadget 期望的格式
//
// 目标（与 a9_setup_hid_gadget.sh 的 report_desc / report_length 对应）：
//   鼠标 hidg1: report protocol，report_length=9，descriptor 与罗技鼠标一致
//               → 罗技带 ReportID(0x02) 的 9 字节报告【完整透传】
//   键盘 hidg0: boot 键盘 8 字节报告
//
// 输入格式（真实设备，已实测 Logitech USB Receiver）：
//   罗技鼠标 hidraw1 = 9 字节：ReportID(0x02) + buttons(2B,16bit) + X(2B) + Y(2B)
//                              + wheel(1B) + AC Pan(1B)；
//   同 hidraw 还混有 consumer(ReportID 0x03)/system(0x04) 报告，必须过滤。
//   普通 HID 鼠标 hidraw = 3~4 字节：buttons + dx + dy [+ wheel]（映射为 9 字节）
//   普通 HID 键盘 hidraw = 8 字节 boot 格式：modifier + reserved + 6 keys
// 返回 false 表示该报告应丢弃（非鼠标报告 / 无法识别的长度）。
// ---------------------------------------------------------------------------
bool HidForwarder::reencode(const HidReport& rep, uint8_t* out, size_t* out_size) {
    const uint8_t* d = rep.data.data();
    const size_t n = rep.size;

    if (params_.kind == HidKind::kMouse) {
        // consumer / system control 报告：丢弃
        if (n >= 1 && (d[0] == 0x03 || d[0] == 0x04)) return false;
        // 罗技/带 ReportID(0x02) 的鼠标：完整透传原 9 字节
        if (n >= 1 && d[0] == 0x02) {
            if (n > 16) { ++stats_.bad_size; return false; }
            std::memcpy(out, d, n);
            *out_size = n;
            return true;
        }
        // 普通 3/4 字节鼠标（无 ReportID）：映射为 9 字节（与 gadget descriptor 一致）
        // 布局：ID(02) + buttons(2B LE) + X(2B LE) + Y(2B LE) + wheel(1B) + AC pan(1B)
        if (n >= 3) {
            out[0] = 0x02;
            out[1] = d[0]; out[2] = 0;                     // buttons
            out[3] = d[1]; out[4] = (d[1] & 0x80) ? 0xFF : 0x00;  // X (sign-extend)
            out[5] = d[2]; out[6] = (d[2] & 0x80) ? 0xFF : 0x00;  // Y (sign-extend)
            out[7] = (n >= 4) ? d[3] : 0;                  // wheel
            out[8] = 0;                                    // AC pan
            *out_size = 9;
            return true;
        }
        ++stats_.bad_size;
        return false;
    }

    if (params_.kind == HidKind::kKeyboard) {
        // 键盘：期望 8 字节 boot 格式。若带 ReportID（罗技键盘 byte0=0x01），
        // 跳过 ID 字节取后续 8 字节；否则原样转发。
        size_t off = 0;
        if (n >= 9 && d[0] != 0x00) off = 1;  // 带 ReportID：从 d[1] 起 8 字节
        if (n - off >= 8) {
            std::memcpy(out, d + off, 8);
            *out_size = 8;
            return true;
        }
        ++stats_.bad_size;
        return false;
    }

    ++stats_.bad_size;
    return false;
}

}  // namespace ttbox::core

#endif  // !_WIN32
