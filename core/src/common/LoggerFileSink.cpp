// LoggerFileSink.cpp — Logger 的文件落盘实现（《代码书写规矩·技术版》§5.2 三分流）
//
// 为什么单独一个文件（而不是并进 Logger.cpp）：§4.2 规定「存量文件被修改，行数增幅 ≤ 20%」。
// Logger.cpp 本是 117 行的轻量实现，把轮转 / 环形缓冲 / 同步落盘 / 三文件分流全塞进去会翻三倍
// ⇒ 拆开。顺带也把职责分清了：Logger.cpp 管「格式与分发」，本文件管「落地与滚动」。
//
// 三个落盘文件的分工（§5.2「12 小时清理一次，只留重要操作与报错」靠它天然成立）：
//   · ttbox.log        全量流水（WARN/INFO 批量 + ERROR/FATAL 直写），12h 轮转丢旧流水
//   · ttbox-error.log  只收 FATAL/ERROR，直写 + fsync，内容永不过滤 ⇒ 报错永在
//   · operation.log    客户操作，同步 ⇒ 重要操作永在

#include "Logger.hpp"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ttbox::core {

namespace {

// 一次性把 FILE* 刷到磁盘。Windows 用 _commit，POSIX 用 fsync（§5.2「同步落盘」）。
bool sync_file(std::FILE* fp) {
    if (fp == nullptr) return false;
    if (std::fflush(fp) != 0) return false;
#if defined(_WIN32)
    return ::_commit(::_fileno(fp)) == 0;
#else
    return ::fsync(::fileno(fp)) == 0;
#endif
}

}  // namespace

// 构造：打开三个落盘文件并计入既有字节数；主文件打不开则整体降级为不可用。
FileSink::FileSink(std::string dir, std::size_t max_bytes, int keep)
    : dir_(std::move(dir)), max_bytes_(max_bytes > 0 ? max_bytes : (8u << 20)),
      keep_(keep > 0 ? keep : 1) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    open_main_locked();
    if (fp_ == nullptr) {
        // 目录不存在 / 无写权限：整体降级为不可用，调用方自行决定是否告警。
        // ★ 故意不抛、不 abort：日志设施挂了不许拖垮启动。
        usable_ = false;
        return;
    }
    err_fp_ = std::fopen((dir_ + "/ttbox-error.log").c_str(), "a");
    op_fp_ = std::fopen((dir_ + "/operation.log").c_str(), "a");
    usable_ = true;
    last_flush_ = std::chrono::steady_clock::now();
    last_housekeep_ = last_flush_;
}

// 析构：把缓冲余量刷进全量流水，再关闭三个文件句柄。
FileSink::~FileSink() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    for (const auto& line : buf_) {
        if (write_raw_locked(fp_, line)) fp_bytes_ += line.size() + 1;
    }
    buf_.clear();
    if (fp_ != nullptr) { std::fflush(fp_); std::fclose(fp_); fp_ = nullptr; }
    if (err_fp_ != nullptr) { std::fflush(err_fp_); std::fclose(err_fp_); err_fp_ = nullptr; }
    if (op_fp_ != nullptr) { std::fflush(op_fp_); std::fclose(op_fp_); op_fp_ = nullptr; }
}

// 打开 ttbox.log（追加模式），并把已有文件字节数计入轮转配额。
void FileSink::open_main_locked() {
    const std::string path = dir_ + "/ttbox.log";
    fp_ = std::fopen(path.c_str(), "a");
    if (fp_ == nullptr) return;
    // 已有内容要计入字节数，否则重启后 8MB 轮转判据从 0 重来。
    if (std::fseek(fp_, 0, SEEK_END) == 0) {
        const long n = std::ftell(fp_);
        fp_bytes_ = (n > 0) ? static_cast<std::size_t>(n) : 0;
    }
}

// 向文件写一行 + 换行；返回是否整行写入成功。
bool FileSink::write_raw_locked(std::FILE* fp, const std::string& line) {
    if (fp == nullptr) return false;
    const std::size_t n = std::fwrite(line.data(), 1, line.size(), fp);
    std::fputc('\n', fp);
    return n == line.size();
}

// 轮转：关闭当前文件、把历史文件 .1…<keep> 逐级移位、重开路径。
void FileSink::rotate_locked(std::FILE*& fp, const std::string& path, std::size_t& written) {
    if (fp != nullptr) {
        std::fflush(fp);
        std::fclose(fp);
        fp = nullptr;
    }
    // path 是**含 .log 的完整文件路径**；历史文件为 <path>.1 … <path>.<keep_>
    // （即 ttbox.log → ttbox.log.1 → ttbox.log.2 …）。先删最旧，再依次后移，
    // 最后把当前文件落到 .1。
    std::remove((path + "." + std::to_string(keep_)).c_str());
    for (int i = keep_ - 1; i >= 1; --i) {
        std::rename((path + "." + std::to_string(i)).c_str(),
                    (path + "." + std::to_string(i + 1)).c_str());
    }
    std::rename(path.c_str(), (path + ".1").c_str());
    written = 0;
    fp = std::fopen(path.c_str(), "a");
}

// 距上次落盘满一个间隔时，把环形缓冲整体写入并触发字节轮转（「少写卡」批量语义）。
void FileSink::flush_buffer_locked() {
    const auto now = std::chrono::steady_clock::now();
    if (buf_.empty()) {
        last_flush_ = now;
        return;
    }
    if (now - last_flush_ < flush_interval_) return;

    for (const auto& line : buf_) {
        if (write_raw_locked(fp_, line)) fp_bytes_ += line.size() + 1;
    }
    // 一次 fflush 顶 N 行 —— 「少写卡」（#64）就靠这个批量语义。
    std::fflush(fp_);
    buf_.clear();
    last_flush_ = now;
    if (fp_bytes_ > max_bytes_) rotate_locked(fp_, dir_ + "/ttbox.log", fp_bytes_);
}

// 按级别分流：FATAL/ERROR 直写报错流水并 fsync；WARN/INFO 进缓冲。
void FileSink::write(LogLevel level, const std::string& line) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (!usable_) return;

    // §5.2 DEBUG 仅内存：连缓冲都不进，免得 DEBUG 刷屏把 WARN/INFO 挤出环形缓冲。
    if (level == LogLevel::kDebug) return;

    const bool severe = (level == LogLevel::kError || level == LogLevel::kFatal);

    if (severe) {
        // 报错：两个文件都直写 + fsync。ttbox-error.log 内容永不过滤 ⇒「报错」永在。
        if (err_fp_ != nullptr) {
            if (write_raw_locked(err_fp_, line)) err_bytes_ += line.size() + 1;
            sync_file(err_fp_);
            if (err_bytes_ > max_bytes_) rotate_locked(err_fp_, dir_ + "/ttbox-error.log", err_bytes_);
        }
        if (fp_ != nullptr) {
            if (write_raw_locked(fp_, line)) fp_bytes_ += line.size() + 1;
            sync_file(fp_);
            if (fp_bytes_ > max_bytes_) rotate_locked(fp_, dir_ + "/ttbox.log", fp_bytes_);
        }
        return;
    }

    // WARN/INFO：进环形缓冲，到点批量落盘。
    buf_.push_back(line);
    while (buf_.size() > buf_limit_) buf_.pop_front();
    flush_buffer_locked();
}

// 客户操作：同步写 operation.log（写完立刻 fsync）。
void FileSink::operation(const std::string& line) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (!usable_ || op_fp_ == nullptr) return;
    if (write_raw_locked(op_fp_, line)) op_bytes_ += line.size() + 1;
    sync_file(op_fp_);  // §5.2「同步」：客户点了什么必须立刻落盘
    if (op_bytes_ > max_bytes_) rotate_locked(op_fp_, dir_ + "/operation.log", op_bytes_);
}

// 强制把缓冲余量全部落盘（不受 flush_interval_ 节流）。
void FileSink::flush() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (!usable_) return;
    for (const auto& line : buf_) {
        if (write_raw_locked(fp_, line)) fp_bytes_ += line.size() + 1;
    }
    buf_.clear();
    if (fp_ != nullptr) std::fflush(fp_);
    last_flush_ = std::chrono::steady_clock::now();
    if (fp_bytes_ > max_bytes_) rotate_locked(fp_, dir_ + "/ttbox.log", fp_bytes_);
}

// 12 小时周期维护：轮转全量流水（报错/操作文件不在清理范围）。
void FileSink::housekeeping() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (!usable_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_housekeep_ < std::chrono::hours(12)) return;
    last_housekeep_ = now;

    // §5.2「12 小时清理一次，只留重要操作与报错」：只轮转全量流水（丢掉旧 WARN/INFO），
    // 报错与客户操作分别在各自文件里，不在清理范围 —— 不需要读写重写任何文件内容。
    flush_buffer_locked();
    rotate_locked(fp_, dir_ + "/ttbox.log", fp_bytes_);
}

}  // namespace ttbox::core
