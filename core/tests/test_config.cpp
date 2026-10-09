// test_config.cpp — ConfigManager：真实 default.json / 缺失文件 / 坏 JSON
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <process.h>  // _getpid：夹具唯一化需真实 PID（此前 Windows 硬编码 12345）
#else
#include <unistd.h>
#endif

#include "common/Json.hpp"
#include "config/ConfigManager.hpp"
#include "test_util.hpp"

// TTBOX_PROJECT_ROOT 由 CMake 注入（<root>/ttbox/core）
#ifndef TTBOX_PROJECT_ROOT
#error "TTBOX_PROJECT_ROOT must be injected by CMake (-DTTBOX_PROJECT_ROOT); refuse silent fallback to '.'."
#endif

namespace {

long getpid_like() {
#if defined(_WIN32)
    // 夹具可重入化：此前硬编码 12345 ⇒ 并发实例共用同一临时文件（判据数字被污染）。
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

std::string real_config_path() {
    return std::string(TTBOX_PROJECT_ROOT) + "/config/default.json";
}

std::string tmp_bad_json_path() {
    return std::string("/tmp/ttbox_core_test_bad_") + std::to_string(getpid_like()) + ".json";
}

// 跨平台临时目录（Windows 的 /tmp 不存在，必须用 temp_directory_path）
std::string unique_temp_dir(const char* prefix) {
    const std::string tag = std::to_string(getpid_like()) + "_" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count() %
                                           1000000);
    return (std::filesystem::temp_directory_path() /
            (std::string(prefix) + tag))
        .string();
}

std::string read_file_text(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST(config_loads_existing_default_json) {
    ttbox::core::ConfigManager cfg;
    std::string error;
    bool ok = cfg.load(real_config_path(), &error);
    CHECK(ok);
    if (ok) {
        CHECK(cfg.loaded());
        // conf 是用户可配置项（A-7），default.json 提供运行时默认值；断言能读到有效正数
        CHECK(cfg.get_double("conf", -1.0) > 0.0);
        // 输入尺寸随当前激活模型自适应（256/320/416/640…），不能硬编码断言具体值；
        // 只要求是有效正数（build_runtime_params 会用 Registry 真实模型尺寸覆盖它们）
        CHECK(cfg.get_int("model_input_width", 0) > 0);
        CHECK(cfg.get_int("model_input_height", 0) > 0);
        CHECK(cfg.get_string("aim_keys_text", "") == "KEY_LEFTSHIFT,KEY_RIGHTSHIFT");
        CHECK(!cfg.flatten().empty());
    } else {
        std::printf("  [info] 配置加载失败: %s\n", error.c_str());
    }
}

TEST(config_missing_file_errors_explicitly) {
    ttbox::core::ConfigManager cfg;
    std::string error;
    bool ok = cfg.load("/tmp/definitely_not_exists_ttbox.json", &error);
    CHECK(!ok);
    CHECK(!cfg.loaded());
    CHECK(!error.empty());  // 明确错误，不允许 silent fallback
}

TEST(config_bad_json_errors_explicitly) {
    const std::string path = tmp_bad_json_path();
    {
        std::ofstream f(path, std::ios::trunc);
        f << "{ \"conf\": 0.25, \"broken\" ";  // 语法错误（未闭合）
    }
    ttbox::core::ConfigManager cfg;
    std::string error;
    bool ok = cfg.load(path, &error);
    CHECK(!ok);
    CHECK(!cfg.loaded());
    CHECK(!error.empty());
    CHECK(error.find("JSON") != std::string::npos);  // 明确提示 JSON 错误
    std::remove(path.c_str());
}

TEST(config_non_object_root_errors) {
    const std::string path = tmp_bad_json_path();
    {
        std::ofstream f(path, std::ios::trunc);
        f << "[1,2,3]";
    }
    ttbox::core::ConfigManager cfg;
    std::string error;
    bool ok = cfg.load(path, &error);
    CHECK(!ok);
    CHECK(error.find("对象") != std::string::npos);
    std::remove(path.c_str());
}

// QA-B1 / R3 回归（Windows 可跑）——QA 审查指出的缺口：分层写回路径此前零测试，
// 导致"写回目标是目录而不是 10-device.json"这种阻断级 bug 混进 26/26 全绿。
//
// 断言覆盖：
//   B1：persist 写进 <dir>/10-device.json（文件），tmp 也建在同目录且事后不残留，
//       目录本身不被当文件写（旧 bug 会把 tmp 建到 <dir>.tmp 并 rename 到目录）
//   R3：设备层只写与 00-factory 基线的差异——与基线相同的键（conf、mouse）
//       被剔除，不把工厂层"快照冻结"进设备层；工厂文件原样不动
//   语义：重载后深合并视图 = 预期完整配置
TEST(config_layered_persist_writes_diff_to_device_file) {
    namespace fs = std::filesystem;
    const std::string dir_s = unique_temp_dir("ttbox_layered_");
    const fs::path dir(dir_s);
    const fs::path factory = dir / "00-factory.json";
    const fs::path device = dir / "10-device.json";
    std::error_code fec;
    fs::create_directories(dir, fec);
    CHECK(!fec);
    {
        std::ofstream f(factory, std::ios::binary | std::ios::trunc);
        f << "{\"conf\":0.25,\"crop_width\":640,"
             "\"mouse\":{\"enabled\":false,\"scale\":1.0}}";
        std::ofstream d(device, std::ios::binary | std::ios::trunc);
        d << "{\"crop_width\":512,\"device_note\":\"keep\"}";
    }
    const std::string factory_before = read_file_text(factory);

    ttbox::core::ConfigManager cfg;
    std::string error;
    CHECK(cfg.load(dir_s, &error));
    CHECK(cfg.loaded());
    CHECK(cfg.is_layered());
    // B1 核心：写回目标是设备层"文件"，不是 --config 传入的目录
    CHECK(cfg.path() == device.string());

    // 读视图 = 深合并（工厂 ← 设备）
    CHECK(cfg.get_int("crop_width", 0) == 512);    // 设备层覆盖工厂
    CHECK(cfg.get_double("conf", -1.0) == 0.25);   // 工厂层提供
    CHECK(cfg.get_string("device_note", "") == "keep");
    const ttbox::core::JsonValue* mouse = cfg.root().find("mouse");
    CHECK(mouse != nullptr && mouse->find("enabled") != nullptr &&
          mouse->find("enabled")->as_bool(true) == false);

    // 模拟 Application::persist_runtime_profile 的组装：全量视图 + 键更新
    ttbox::core::JsonValue view = cfg.root();
    view.set("crop_width", ttbox::core::JsonValue::number(600));
    ttbox::core::JsonValue rp = ttbox::core::JsonValue::object();
    rp.set("model_id", ttbox::core::JsonValue::string("m1"));
    view.set("runtime_profile", std::move(rp));
    CHECK(cfg.persist(view, &error));

    // B1 断言：写的是 10-device.json 文件，tmp 不残留，目录未被当文件写
    CHECK(fs::is_regular_file(device));
    CHECK(!fs::exists(dir / "10-device.json.tmp"));
    CHECK(!fs::exists(fs::path(dir_s + ".tmp")));

    // R3 断言：设备层 = 与基线的差异（不含工厂键 conf / mouse）
    ttbox::core::JsonParseResult parsed =
        ttbox::core::json_parse(read_file_text(device));
    CHECK(parsed.ok);
    if (parsed.ok) {
        CHECK(parsed.value.find("conf") == nullptr);   // 工厂键不快照进设备层
        CHECK(parsed.value.find("mouse") == nullptr);  // 与基线相同 → 整键剔除
        const ttbox::core::JsonValue* cw = parsed.value.find("crop_width");
        CHECK(cw != nullptr && cw->as_int(0) == 600);
        const ttbox::core::JsonValue* note = parsed.value.find("device_note");
        CHECK(note != nullptr && note->as_string("") == "keep");
        const ttbox::core::JsonValue* rpv = parsed.value.find("runtime_profile");
        CHECK(rpv != nullptr && rpv->find("model_id") != nullptr);
    }

    // 工厂层原样未动
    CHECK(read_file_text(factory) == factory_before);

    // 重载：合并视图恢复完整有效配置
    ttbox::core::ConfigManager cfg2;
    CHECK(cfg2.load(dir_s, &error));
    CHECK(cfg2.get_int("crop_width", 0) == 600);
    CHECK(cfg2.get_double("conf", -1.0) == 0.25);
    CHECK(cfg2.get_string("device_note", "") == "keep");
    const ttbox::core::JsonValue* mouse2 = cfg2.root().find("mouse");
    CHECK(mouse2 != nullptr && mouse2->find("enabled") != nullptr &&
          mouse2->find("enabled")->as_bool(true) == false);

    fs::remove_all(dir, fec);
}

// 单文件模式 persist 回归：全量写回（历史行为不变）+ 原子替换生效 + tmp 不残留。
// persist 的原子写代码整体下沉到 ConfigManager（B1 重构），必须确认老路径无回归。
TEST(config_single_file_persist_roundtrip) {
    namespace fs = std::filesystem;
    const std::string dir_s = unique_temp_dir("ttbox_single_");
    const fs::path file = fs::path(dir_s) / "single.json";
    std::error_code fec;
    fs::create_directories(dir_s, fec);
    CHECK(!fec);
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << "{\"a\":1,\"b\":\"x\"}";
    }

    ttbox::core::ConfigManager cfg;
    std::string error;
    CHECK(cfg.load(file.string(), &error));
    CHECK(!cfg.is_layered());
    CHECK(cfg.get_int("a", 0) == 1);

    ttbox::core::JsonValue view = cfg.root();
    view.set("b", ttbox::core::JsonValue::string("y"));
    CHECK(cfg.persist(view, &error));
    CHECK(!fs::exists(fs::path(file.string() + ".tmp")));  // 原子替换后 tmp 不残留

    ttbox::core::ConfigManager cfg2;
    CHECK(cfg2.load(file.string(), &error));
    CHECK(cfg2.get_int("a", 0) == 1);          // 未动的键全量保留（非 diff 模式）
    CHECK(cfg2.get_string("b", "") == "y");    // 更新生效

    fs::remove_all(dir_s, fec);
}
