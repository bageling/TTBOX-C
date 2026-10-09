// test_logger.cpp — Logger 级别过滤 / §5.3 格式 / §5.2 文件分流验证
//
// 口径真源：《代码书写规矩·技术版》§5（本文件随批次 1.4 一起落地）。
// 覆盖点：
//   ① §5.1 分级序 FATAL > ERROR > WARN > INFO > DEBUG（kOff 是哨兵不是级别）
//   ② §5.3 行格式 <ISO8601 本地时间> <LEVEL> <module> <message> k1=v1 ... at=<file>:<line>
//   ③ §5.2 三文件分流：ttbox.log 全量 / ttbox-error.log 只报错 / operation.log 只客户操作
//   ④ §5.2 DEBUG 仅内存（不得落盘）
//   ⑤ 日志设施降级安全：目录不可用时静默丢弃，不崩、不抛、不拖垮启动
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "common/Logger.hpp"
#include "test_util.hpp"

namespace {

// 捕获型 sink：记录收到的行
class CaptureSink : public ttbox::core::LogSink {
public:
    void write(ttbox::core::LogLevel level, const std::string& line) override {
        lines.push_back(line);
        last_level = level;
        count.fetch_add(1);
    }
    std::vector<std::string> lines;
    ttbox::core::LogLevel last_level = ttbox::core::LogLevel::kDebug;
    std::atomic<int> count{0};
};

// 每个用例一个独立临时目录：互不干扰，也绝不去碰板端的 /var/log/ttbox。
std::string make_temp_dir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path p = std::filesystem::temp_directory_path() /
                                    ("ttbox-logtest-" + std::string(tag) + "-" +
                                     std::to_string(stamp));
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void remove_dir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace

// ───────────────────────── ① 分级与级别过滤 ─────────────────────────

TEST(logger_level_order_fatal_is_top) {
    using L = ttbox::core::LogLevel;
    CHECK(static_cast<int>(L::kFatal) > static_cast<int>(L::kError));
    CHECK(static_cast<int>(L::kError) > static_cast<int>(L::kWarn));
    CHECK(static_cast<int>(L::kWarn) > static_cast<int>(L::kInfo));
    CHECK(static_cast<int>(L::kInfo) > static_cast<int>(L::kDebug));
    // kOff 是「关掉一切」的哨兵，必须大于所有可打印级别
    CHECK(static_cast<int>(L::kOff) > static_cast<int>(L::kFatal));
}

TEST(logger_writes_with_level_tag) {
    auto& logger = ttbox::core::Logger::instance();
    logger.clear_sinks();
    auto sink = std::make_shared<CaptureSink>();
    logger.add_sink(sink);
    logger.set_level(ttbox::core::LogLevel::kDebug);

    logger.log(ttbox::core::LogLevel::kInfo, "hello-info", __FILE__, __LINE__);
    CHECK(sink->count.load() == 1);
    CHECK(!sink->lines.empty());
    if (!sink->lines.empty()) {
        const std::string& line = sink->lines[0];
        CHECK(line.find(" INFO ") != std::string::npos);            // §5.3 级别大写、两侧分立
        CHECK(line.find("hello-info") != std::string::npos);
        CHECK(line.find("test_logger") != std::string::npos);        // module = 文件短名（去扩展名）
        CHECK(line.find("at=test_logger.cpp:") != std::string::npos);  // 行号 kv 供定位
    }
}

TEST(logger_level_filter_blocks_debug_and_info) {
    auto& logger = ttbox::core::Logger::instance();
    logger.clear_sinks();
    auto sink = std::make_shared<CaptureSink>();
    logger.add_sink(sink);
    logger.set_level(ttbox::core::LogLevel::kWarn);  // 过滤 Debug/Info

    logger.log(ttbox::core::LogLevel::kDebug, "d", __FILE__, __LINE__);
    logger.log(ttbox::core::LogLevel::kInfo, "i", __FILE__, __LINE__);
    CHECK(sink->count.load() == 0);

    logger.log(ttbox::core::LogLevel::kWarn, "w", __FILE__, __LINE__);
    logger.log(ttbox::core::LogLevel::kError, "e", __FILE__, __LINE__);
    logger.log(ttbox::core::LogLevel::kFatal, "f", __FILE__, __LINE__);
    CHECK(sink->count.load() == 3);  // FATAL 不得被级别过滤掉
}

TEST(logger_macro_basic) {
    auto& logger = ttbox::core::Logger::instance();
    logger.clear_sinks();
    auto sink = std::make_shared<CaptureSink>();
    logger.add_sink(sink);
    logger.set_level(ttbox::core::LogLevel::kInfo);

    TTBOX_LOG_INFO("macro-info");
    TTBOX_LOG_WARN("macro-warn");
    TTBOX_LOG_FATAL("macro-fatal");
    CHECK(sink->count.load() == 3);
    CHECK(!sink->lines.empty());
    if (!sink->lines.empty()) {
        // 宏自带的 module 必须正确派生（__FILE__ 注入链没断）
        CHECK(sink->lines[0].find("test_logger") != std::string::npos);
    }
}

// ───────────────────────── ② 格式（§5.3） ─────────────────────────

TEST(logger_timestamp_iso8601_shape) {
    const std::string ts = ttbox::core::log_timestamp_iso8601();
    // 规矩给的字面格式：2026-10-01T12:03:44+08:00 —— 带时区偏移、不带毫秒
    const std::regex re(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}$)");
    CHECK(std::regex_match(ts, re));
    CHECK(ts.find('.') == std::string::npos);
    CHECK(ts.find('T') != std::string::npos);
}

TEST(logger_module_name_derivation) {
    using ttbox::core::log_module_name;
    CHECK(log_module_name("/a/b/V4L2Capture.cpp") == "V4L2Capture");
    CHECK(log_module_name("C:\\src\\aim\\AimThread.cpp") == "AimThread");
    CHECK(log_module_name("Logger.hpp") == "Logger");
    CHECK(log_module_name("noext") == "noext");
}

TEST(logger_level_name_is_uppercase_unpadded) {
    using ttbox::core::log_level_name;
    CHECK(std::string(log_level_name(ttbox::core::LogLevel::kDebug)) == "DEBUG");
    CHECK(std::string(log_level_name(ttbox::core::LogLevel::kInfo)) == "INFO");
    CHECK(std::string(log_level_name(ttbox::core::LogLevel::kWarn)) == "WARN");
    CHECK(std::string(log_level_name(ttbox::core::LogLevel::kError)) == "ERROR");
    CHECK(std::string(log_level_name(ttbox::core::LogLevel::kFatal)) == "FATAL");
}

TEST(logger_kv_appended_after_message) {
    auto& logger = ttbox::core::Logger::instance();
    logger.clear_sinks();
    auto sink = std::make_shared<CaptureSink>();
    logger.add_sink(sink);
    logger.set_level(ttbox::core::LogLevel::kError);

    logger.log(ttbox::core::LogLevel::kError, "v4l2 ioctl failed", __FILE__, __LINE__,
               {{"fd", "7"}, {"errno", "25"}});
    CHECK(sink->count.load() == 1);
    if (!sink->lines.empty()) {
        const std::string& line = sink->lines[0];
        CHECK(line.find("fd=7") != std::string::npos);
        CHECK(line.find("errno=25") != std::string::npos);
        const std::size_t p_msg = line.find("v4l2 ioctl failed");
        const std::size_t p_fd = line.find("fd=7");
        const std::size_t p_at = line.find("at=");
        CHECK(p_msg != std::string::npos && p_fd != std::string::npos && p_at != std::string::npos);
        CHECK(p_msg < p_fd);  // kv 在 message 之后
        CHECK(p_fd < p_at);   // 行号 kv 在最后
    }
}

// ───────────────────────── ③ 文件分流（§5.2） ─────────────────────────

TEST(file_sink_splits_three_files) {
    const std::string dir = make_temp_dir("split");
    auto sink = std::make_shared<ttbox::core::FileSink>(dir);
    CHECK(sink->usable());
    // WARN/INFO 默认要攒 5 分钟才落盘；测试里把间隔归零，当场验证分流
    sink->set_flush_interval_for_test(std::chrono::seconds(0));

    sink->write(ttbox::core::LogLevel::kInfo, "line-info");
    sink->write(ttbox::core::LogLevel::kWarn, "line-warn");
    sink->write(ttbox::core::LogLevel::kError, "line-error");
    sink->write(ttbox::core::LogLevel::kFatal, "line-fatal");
    sink->write(ttbox::core::LogLevel::kDebug, "line-debug");
    sink->operation("stop");
    sink->flush();

    const std::string main_log = read_file(dir + "/ttbox.log");
    const std::string err_log = read_file(dir + "/ttbox-error.log");
    const std::string op_log = read_file(dir + "/operation.log");

    // ttbox.log = 全量流水（WARN/INFO 批量 + ERROR/FATAL 直写）
    CHECK(main_log.find("line-info") != std::string::npos);
    CHECK(main_log.find("line-warn") != std::string::npos);
    CHECK(main_log.find("line-error") != std::string::npos);
    CHECK(main_log.find("line-fatal") != std::string::npos);
    // §5.2 DEBUG 仅内存 ⇒ 任何一个文件都不该出现
    CHECK(main_log.find("line-debug") == std::string::npos);

    // ttbox-error.log 只收 FATAL/ERROR（内容永不过滤 ⇒ §5.2「只留…报错」永在）
    CHECK(err_log.find("line-error") != std::string::npos);
    CHECK(err_log.find("line-fatal") != std::string::npos);
    CHECK(err_log.find("line-info") == std::string::npos);
    CHECK(err_log.find("line-warn") == std::string::npos);
    CHECK(err_log.find("line-debug") == std::string::npos);

    // operation.log 只收客户操作
    CHECK(op_log.find("stop") != std::string::npos);
    CHECK(op_log.find("line-info") == std::string::npos);
    CHECK(op_log.find("line-error") == std::string::npos);

    // Windows 上文件被占用时删不掉 ⇒ 先释放 sink
    sink.reset();
    remove_dir(dir);
}

TEST(file_sink_rotates_and_keeps_bounded_history) {
    const std::string dir = make_temp_dir("rotate");
    // 给一个很小的上限，便于在测试里触发轮转（生产是 8MB / 保留 3 份，§5.2）
    auto sink = std::make_shared<ttbox::core::FileSink>(dir, 256, 3);
    CHECK(sink->usable());
    sink->set_flush_interval_for_test(std::chrono::seconds(0));

    for (int i = 0; i < 40; ++i) {
        sink->write(ttbox::core::LogLevel::kError,
                    "padding-" + std::to_string(i) + std::string(48, 'x'));
    }
    sink->flush();

    const bool rotated = std::filesystem::exists(dir + "/ttbox.log.1");
    const bool overflow = std::filesystem::exists(dir + "/ttbox.log.4");  // keep=3 ⇒ 不该有 .4
    sink.reset();

    CHECK(rotated);
    CHECK(!overflow);
    remove_dir(dir);
}

TEST(file_sink_unusable_dir_is_safe_and_silent) {
    // 父目录不存在 ⇒ fopen 必失败。日志设施必须降级而不是炸掉进程。
    const std::string bogus =
        (std::filesystem::temp_directory_path() / "ttbox-logtest-missing" / "deep").string();
    auto sink = std::make_shared<ttbox::core::FileSink>(bogus);
    CHECK(!sink->usable());

    sink->write(ttbox::core::LogLevel::kError, "boom");
    sink->write(ttbox::core::LogLevel::kInfo, "noise");
    sink->operation("start");
    sink->flush();
    sink->housekeeping();
    CHECK(!std::filesystem::exists(bogus + "/ttbox.log"));
}

// ───────────────────────── ④ 客户操作通道（§5.2） ─────────────────────────

TEST(logger_operation_ignores_level_filter) {
    auto& logger = ttbox::core::Logger::instance();
    logger.clear_sinks();
    const std::string dir = make_temp_dir("op");
    auto fs = std::make_shared<ttbox::core::FileSink>(dir);
    logger.add_sink(fs);
    logger.set_level(ttbox::core::LogLevel::kOff);  // 级别关到最死

    logger.log(ttbox::core::LogLevel::kInfo, "should-not-appear", __FILE__, __LINE__);
    // 客户点了什么必须留痕，不受当前级别影响（§5.2 六类操作）
    logger.operation("start");
    logger.flush();

    const std::string op_log = read_file(dir + "/operation.log");
    const std::string main_log = read_file(dir + "/ttbox.log");

    logger.clear_sinks();
    fs.reset();

    CHECK(op_log.find("OPERATION") != std::string::npos);
    CHECK(op_log.find("start") != std::string::npos);
    CHECK(main_log.find("should-not-appear") == std::string::npos);
    remove_dir(dir);
}
