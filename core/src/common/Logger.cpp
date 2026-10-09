// Logger.cpp — 轻量日志实现（分级 / 格式 / 分发）
//
// 口径真源：《代码书写规矩·技术版》§5。文件落盘（三分流 / 轮转 / 缓冲）在
// LoggerFileSink.cpp —— 拆开是为了守住 §4.2 的行数规矩，也把「格式与分发」和
// 「落地与滚动」两件事分开读。
/*
 * TTBOX 文件说明
 *
 * 文件：Logger.cpp
 *
 * 作用：
 *   TTBOX 的日志系统（前半段）。
 *   记录程序运行过程中的信息、警告和错误。
 *
 * 小白理解：
 *   就像飞机的黑匣子一样，日志记录了程序运行中的所有重要事件。
 *   出问题时，先看日志找原因。
 *   这一段负责"把话说清楚"（时间、级别、哪个模块、发生了什么）；
 *   至于写进哪个文件、什么时候翻页，交给 LoggerFileSink.cpp。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "Logger.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ttbox::core {

namespace {

// 取基名（去掉目录分隔符）。
const char* base_name(const char* file) {
    if (file == nullptr) return "";
    const char* base = file;
    if (const char* slash = std::strrchr(file, '/'); slash != nullptr) {
        base = slash + 1;
    } else if (const char* bs = std::strrchr(file, '\\'); bs != nullptr) {
        base = bs + 1;
    }
    return base;
}

}  // namespace

// 级别 → 大写名（DEBUG/INFO/WARN/ERROR/FATAL/OFF）。
const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kInfo: return "INFO";
        case LogLevel::kWarn: return "WARN";
        case LogLevel::kError: return "ERROR";
        case LogLevel::kFatal: return "FATAL";
        case LogLevel::kOff: return "OFF";
    }
    return "UNKNOWN";
}

// 生成带时区偏移的 ISO8601 本地时间串（§5.3 格式前缀）。
std::string log_timestamp_iso8601() {
    using clock = std::chrono::system_clock;
    const std::time_t t = clock::to_time_t(clock::now());

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    // 时区偏移（秒）。glibc 直接给 tm_gmtoff；Windows 没有该字段，用「本地时间字符串
    // 反算回 time_t」求差（DST 切换当天可能差 1 小时，日志可接受）。
    long off_sec = 0;
#if defined(_WIN32)
    std::tm copy = tm_buf;
    off_sec = static_cast<long>(std::difftime(t, std::mktime(&copy)));
#else
    off_sec = static_cast<long>(tm_buf.tm_gmtoff);
#endif
    const char sign = (off_sec < 0) ? '-' : '+';
    const long abs_off = (off_sec < 0) ? -off_sec : off_sec;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  sign, abs_off / 3600, (abs_off % 3600) / 60);
    return buf;
}

// 由 __FILE__ 派生 module 短名（去目录、去扩展名）。
std::string log_module_name(const char* file) {
    std::string base = base_name(file);
    const std::size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) base.resize(dot);
    return base;
}

// WARN 及以上走 stderr、其余走 stdout，并立即刷新。
void ConsoleSink::write(LogLevel level, const std::string& line) {
    if (level >= LogLevel::kWarn) {
        std::fprintf(stderr, "%s\n", line.c_str());
    } else {
        std::fprintf(stdout, "%s\n", line.c_str());
    }
    std::fflush(nullptr);
}

// 进程内单例（函数局部静态，初始化线程安全）。
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

// 设置全局最低输出级别（原子写，读写路径对称）。
void Logger::set_level(LogLevel level) {
    // ★ 不再持 mutex_：级别改原子后这条路径与 level()/log() 的无锁读自洽（2026-09-23 #36）。
    //   mutex_ 仍保留给 sinks_ 用，不能删。
    level_.store(level, std::memory_order_relaxed);
}

// 追加一个 sink（持锁，空指针忽略）。
void Logger::add_sink(std::shared_ptr<LogSink> sink) {
    if (!sink) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

// 清空 sink 列表（测试 / 重配置用）。
void Logger::clear_sinks() {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.clear();
}

// 无 kv 的重载：委托给带 kv 版本，传空键值表。
void Logger::log(LogLevel level, const std::string& msg, const char* file, int line) {
    static const LogKv kEmpty;
    log(level, msg, file, line, kEmpty);
}

// 核心写日志：级别过滤 → 组行（时间/级别/module/消息/kv/at=）→ 分发各 sink。
void Logger::log(LogLevel level, const std::string& msg, const char* file, int line,
                 const LogKv& kv) {
    if (static_cast<int>(level) <
        static_cast<int>(level_.load(std::memory_order_relaxed))) {
        return;
    }

    // §5.3 格式：<ISO8601 本地时间> <LEVEL> <module> <message> k1=v1 k2=v2
    std::string text;
    text.reserve(msg.size() + 96);
    text += log_timestamp_iso8601();
    text += ' ';
    text += log_level_name(level);
    text += ' ';
    text += log_module_name(file);
    text += ' ';
    text += msg;
    for (const auto& item : kv) {
        if (item.first.empty()) continue;
        text += ' ';
        text += item.first;
        text += '=';
        text += item.second;
    }
    // 行号放尾部 kv（规矩 §5.3 未列 at=，但「键值对便于自动解析」允许附加；定位离不开它）。
    text += " at=";
    text += base_name(file);
    text += ':';
    text += std::to_string(line);

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        if (sink) sink->write(level, text);
    }
}

// 客户操作通道：组 OPERATION 行并分发给各 sink 的 operation（不受级别过滤）。
void Logger::operation(const std::string& what) {
    std::string text = log_timestamp_iso8601();
    text += " OPERATION ";
    text += what;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        if (sink) sink->operation(text);
    }
}

// 让各 sink 把内存缓冲批量落盘（退出前 / 周期 tick 调用）。
void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        if (sink) sink->flush();
    }
}

}  // namespace ttbox::core
