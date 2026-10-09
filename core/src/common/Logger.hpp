// Logger.hpp — 轻量日志（分级 + 分流 + 文件 sink）
//
// 口径真源：《代码书写规矩·技术版》§5（本次改造据此落地，批次 1.4）。
//   §5.1 分级 FATAL > ERROR > WARN > INFO > DEBUG（kOff 是关闭哨兵，不是可打印级别）。
//   §5.2 分流：FATAL/ERROR 直写文件并 fsync；客户操作写 operation.log 同步；
//              WARN/INFO 进内存环形缓冲、每 5 分钟批量落盘；DEBUG 仅内存。
//   §5.3 格式：<ISO8601 本地时间> <LEVEL> <module> <message> k1=v1 k2=v2
//   §5.4 禁止用 std::cout / print 当日志；允许同时转发 journald，但**必须**有文件 sink。
//
// 【三个落盘文件的分工】§5.2 要求「12 小时清理一次，只留重要操作与报错」。这里
// 靠文件分工天然成立，**不需要重写/过滤已有文件内容**（重写日志文件是自伤风险）：
//   · ttbox.log        全量流水（WARN/INFO 批量落盘 + ERROR/FATAL 直写），12h 轮转掉旧的
//   · ttbox-error.log  只收 FATAL/ERROR，直写 + fsync，内容永不过滤 ⇒「报错」永远留存
//   · operation.log    客户操作（启动/停止/换模型/改参数/导出/恢复出厂），同步 ⇒「重要操作」永远留存
//
// 【ConsoleSink 为何保留】systemd 把 stdout 收进 journald，`journalctl -u ttbox-core`
// 是板端现场第一手入口；但 journald 会滚动丢失（§5.4）⇒ ConsoleSink 是附加、FileSink 是必需。
//
// 【module 从哪来】§5.3 要求 module 用「文件或逻辑模块短名」。这里由 __FILE__ 派生：
// 去目录、去扩展名 ⇒ V4L2Capture.cpp → `V4L2Capture`，即逻辑模块短名，且**不必改任何
// 存量调用点**（宏签名保持不变）。文件行号作为尾部 kv `at=<file>:<line>` 保留，供定位。
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ttbox::core {

// §5.1 分级。数值序即严重度序，比较大小即比严重度。
enum class LogLevel : int {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3,
    kFatal = 4,
    kOff = 5,  // 哨兵：关闭全部输出；不是可打印级别。
};

// §5.3 键值对：便于自动解析。禁止把变量拼进 message。
using LogKv = std::vector<std::pair<std::string, std::string>>;

// 日志落地点抽象。
class LogSink {
public:
    virtual ~LogSink() = default;
    // 落一条已格式化好的日志行（sink 自行决定级别相关行为）。
    virtual void write(LogLevel level, const std::string& line) = 0;
    // 客户操作（§5.2 六类）。默认丢弃；FileSink 覆盖为「同步写 operation.log」。
    virtual void operation(const std::string& /*line*/) {}
    // 把内存缓冲批量落盘。无缓冲的 sink 空实现即可。
    virtual void flush() {}
};

// 默认控制台 sink：INFO/DEBUG -> stdout，WARN 及以上 -> stderr。
class ConsoleSink : public LogSink {
public:
    void write(LogLevel level, const std::string& line) override;
};

// §5.2 文件 sink。构造约定：
//   · 目录不存在或不可写 ⇒ usable()==false，此后全部 write 静默丢弃
//     —— **日志坏掉不许拖垮启动**（这是硬要求：日志是观测设施，不是业务依赖）。
//   · 跨平台（宿主 ctest 也要跑）：一律 std::FILE* + fflush + 平台 fsync，
//     不用 POSIX open/write，免得 Windows 宿主编不过。
class FileSink : public LogSink {
public:
    // dir：日志目录（板端 /var/log/ttbox）。max_bytes/keep 对应 §5.2「单文件 8MB、
    // 保留 3 个历史文件」。
    explicit FileSink(std::string dir, std::size_t max_bytes = 8u << 20, int keep = 3);
    ~FileSink() override;

    void write(LogLevel level, const std::string& line) override;
    void operation(const std::string& line) override;
    void flush() override;

    bool usable() const { return usable_; }

    // §5.2 滚动节流：距上次 ≥12 小时才真做（供 Application 周期 tick 调用）。
    // 做的是「轮转 + 历史裁剪」，不动内容；「只留重要操作与报错」由文件分工保证。
    void housekeeping();

    // 测试可注入：把「5 分钟批量落盘」的间隔调小（默认 300s）。
    void set_flush_interval_for_test(std::chrono::seconds s) { flush_interval_ = s; }

private:
    // 把 path（**含 .log 的完整文件路径**）轮转到 <path>.1，历史后移，超过 keep_ 份的删除。
    void rotate_locked(std::FILE*& fp, const std::string& path, std::size_t& written);
    void open_main_locked();
    void flush_buffer_locked();
    bool write_raw_locked(std::FILE* fp, const std::string& line);

    mutable std::mutex file_mutex_;
    std::string dir_;
    std::size_t max_bytes_;
    int keep_;
    bool usable_ = false;

    std::FILE* fp_ = nullptr;     // ttbox.log（全量流水）
    std::FILE* err_fp_ = nullptr; // ttbox-error.log（只 FATAL/ERROR，永不过滤）
    std::FILE* op_fp_ = nullptr;  // operation.log（客户操作，同步）
    std::size_t fp_bytes_ = 0;
    std::size_t err_bytes_ = 0;
    std::size_t op_bytes_ = 0;

    std::deque<std::string> buf_;  // WARN/INFO 环形缓冲
    std::size_t buf_limit_ = 4096; // 上限行数；超出丢最旧（内存有界，§5.2「少写卡」）
    std::chrono::seconds flush_interval_{300};
    std::chrono::steady_clock::time_point last_flush_{};
    std::chrono::steady_clock::time_point last_housekeep_{};
};

// 全局日志器单例：分级过滤 + 多 Sink 分发（控制台 / 文件）。
class Logger {
public:
    static Logger& instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 设置全局最低输出级别（低于该级别的日志被丢弃）。
    void set_level(LogLevel level);
    // ★ 无锁读（2026-09-23 审查复核 #36）：原为「持锁写 / 无锁读」的不对称访问。当前唯一写
    //   调用点是启动期命令行解析（`--log-level`），发生在 IPC 注册与线程拉起之前 ⇒ 眼下无
    //   实际竞争；但接口是开放的，一旦有人接上 IPC/setter 路径就是 data race。改原子消除。
    LogLevel level() const { return level_.load(std::memory_order_relaxed); }

    // 追加一个输出 sink（空指针忽略）。
    void add_sink(std::shared_ptr<LogSink> sink);
    void clear_sinks();  // 重置 sink 列表（测试/重配置用）

    // 线程安全写入；file/line 由日志宏填充。kv 为空时输出与旧格式等价（只是少了方括号）。
    void log(LogLevel level, const std::string& msg, const char* file, int line);
    void log(LogLevel level, const std::string& msg, const char* file, int line,
             const LogKv& kv);

    // §5.2 客户操作专用通道：写 operation.log 并同步落盘（不受级别过滤 —— 客户点了
    // 什么必须留痕，哪怕当前级别是 ERROR）。
    void operation(const std::string& what);

    // 把各 sink 的内存缓冲批量落盘（退出前、以及周期 tick 调用）。
    void flush();

private:
    Logger() = default;

    mutable std::mutex mutex_;
    // 原子：与 set_level / level() / log() 的读写路径匹配，不再靠 mutex_ 兜。
    std::atomic<LogLevel> level_{LogLevel::kInfo};
    std::vector<std::shared_ptr<LogSink>> sinks_;
};

// 级别名（§5.3 大写、不填充）：DEBUG/INFO/WARN/ERROR/FATAL。
const char* log_level_name(LogLevel level);

// §5.3 ISO8601 本地时间，带时区偏移，例：2026-10-01T12:03:44+08:00。
std::string log_timestamp_iso8601();

// 由 __FILE__ 派生 module 短名（去目录、去扩展名）；同时输出基名供 at= 用。
std::string log_module_name(const char* file);

// 日志宏（自动携带源文件与行号）。module 由 file 派生，签名与改造前一致 ⇒ 存量调用点零改动。
#define TTBOX_LOG_DEBUG(msg) \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kDebug, (msg), __FILE__, __LINE__)
#define TTBOX_LOG_INFO(msg) \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kInfo, (msg), __FILE__, __LINE__)
#define TTBOX_LOG_WARN(msg) \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kWarn, (msg), __FILE__, __LINE__)
#define TTBOX_LOG_ERROR(msg) \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kError, (msg), __FILE__, __LINE__)
#define TTBOX_LOG_FATAL(msg) \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kFatal, (msg), __FILE__, __LINE__)

// 带键值对的版本（§5.3 推荐给新代码用：`TTBOX_LOG_ERROR_KV("ioctl failed", {{"fd","7"}})`
// 而不是把 fd 拼进 message）。旧代码可继续用上面的简版。
#define TTBOX_LOG_ERROR_KV(msg, kv)                                                       \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kError, (msg), __FILE__, __LINE__, (kv))
#define TTBOX_LOG_WARN_KV(msg, kv)                                                        \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kWarn, (msg), __FILE__, __LINE__, (kv))
#define TTBOX_LOG_INFO_KV(msg, kv)                                                        \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kInfo, (msg), __FILE__, __LINE__, (kv))
#define TTBOX_LOG_FATAL_KV(msg, kv)                                                       \
    ::ttbox::core::Logger::instance().log(::ttbox::core::LogLevel::kFatal, (msg), __FILE__, __LINE__, (kv))

}  // namespace ttbox::core
