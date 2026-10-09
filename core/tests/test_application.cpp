// test_application.cpp — Application：startup / run / clean shutdown
//
// ★ P0-1 源级绊线（Tripwire，非"静默跳过"）：
//   本源无条件引用 ttbox::core::Application，而该符号仅在
//   `TTBOX_CORE_HAS_RKNN` 或 `WIN32` 下编入 libttbox_core（core/CMakeLists.txt:165）。
//   注册条件见 core/CMakeLists.txt:416（UNIX ∧ NOT APPLE ∧ TTBOX_CORE_HAS_RKNN）。
//   条件不满足时【必须编译失败】—— 绝不允许退化成空 TU / 桩：那会静默吞掉覆盖
//   且 CTest 仍报绿（add_test 只判退出码）。
#if !(defined(TTBOX_CORE_HAS_RKNN) && TTBOX_CORE_HAS_RKNN) && !defined(_WIN32)
#error "test_application.cpp 需要 TTBOX_CORE_HAS_RKNN；请检查 core/CMakeLists.txt:416 的注册条件，勿无条件包含本源"
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "app/Application.hpp"
#include "common/Json.hpp"
#include "common/Logger.hpp"
#include "ipc/IpcServer.hpp"
#include "test_util.hpp"

// TTBOX_PROJECT_ROOT 由 CMake 注入（<root>/ttbox/core）
#ifndef TTBOX_PROJECT_ROOT
#error "TTBOX_PROJECT_ROOT must be injected by CMake (-DTTBOX_PROJECT_ROOT); refuse silent fallback to '.'."
#endif

namespace {

std::string real_config_path() {
    return std::string(TTBOX_PROJECT_ROOT) + "/config/default.json";
}

std::string tmp_socket_path() {
#if defined(_WIN32)
    return "tcp:127.0.0.1:39127";
#else
    return "/tmp/ttbox_core_app_test_" + std::to_string(static_cast<long>(::getpid())) + ".sock";
#endif
}

}  // namespace

TEST(application_startup_run_clean_shutdown) {
    // 复位 Logger sink（单例可能已被前序测试添加），避免重复输出
    ttbox::core::Logger::instance().clear_sinks();

    // 复位全局 shutdown 标志（Application 内部）
    ttbox::core::Application app;

    std::string cfg = "--config";
    std::string cfg_path = real_config_path();
    std::string ipc = "--ipc";
    std::string ipc_path = tmp_socket_path();
    char* argv[] = {const_cast<char*>("ttbox_core"), cfg.data(), cfg_path.data(),
                    ipc.data(), ipc_path.data(), nullptr};
    int rc = app.initialize(5, argv);
    CHECK_EQ(rc, 0);
    if (rc != 0) {
        return;
    }

    // run() 在独立线程运行，主线程请求退出
    std::thread runner([&app] { app.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(app.running());
    ttbox::core::Application::request_shutdown();
    runner.join();
    app.shutdown();
    CHECK(!app.running());

    // IPC 也应已关闭（socket 文件已删除）
    std::string response;
    std::string error;
    bool ok = ttbox::core::ipc_request(ipc_path, R"({"type":"PING"})", response, 500, &error);
    CHECK(!ok);
}

TEST(application_initialize_missing_config_fails) {
    ttbox::core::Logger::instance().clear_sinks();
    ttbox::core::Application app;
    std::string cfg = "--config";
    std::string bad_path = "/tmp/not_exist_ttbox_config.json";
    char* argv[] = {const_cast<char*>("ttbox_core"), cfg.data(), bad_path.data(), nullptr};
    int rc = app.initialize(3, argv);
    CHECK_NE(rc, 0);  // 配置缺失必须明确失败，不允许 silent fallback
}

// T02 启动死锁修复 — 行为验收（Linux；本文件在 Windows 不参与编译）：
//
// 修复前：空模型库 → build_runtime_params 返回 MODEL_NOT_SELECTED →
//         initialize() return 1 进程退出，而 IPC 从未启动 → Web 无法连接 →
//         无法选模型 → systemd Restart=always 无限重启（现场实测 82 次/31 分钟）。
// 修复后：initialize() 返回 0，进程进入"待配置"降级态：
//   a) run() 存活（进程不退出）
//   b) IPC PING 可达（Web/工具能连上，可以下发 MODEL_ACTIVATE）
//   c) GET_STATUS 的 runtime_running=false（诚实上报"等待模型"，不假装运行）
TEST(application_model_not_selected_stays_alive_ipc_ping) {
    ttbox::core::Logger::instance().clear_sinks();
    ttbox::core::Application app;

    // 独立空模型库：保证 registry 无 active 模型，强制走 MODEL_NOT_SELECTED 分支
    // （不能复用仓库自带 models 目录——那里可能有 active 模型，测不到降级路径）。
    const std::string tag = std::to_string(static_cast<long>(::getpid())) + "_" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count() %
                                           1000000);
    const std::string base = "/tmp/ttbox_app_degraded_" + tag;
    const std::string models_root = base + "/models";
    std::error_code fec;
    std::filesystem::create_directories(models_root, fec);
    CHECK(!fec);
    std::string config_path = base + "/config.json";  // 非 const：argv 需要 char*
    {
        std::ofstream f(config_path);
        f << "{\n"
             "  \"model_registry_root\": \""
          << models_root
          << "\",\n"
             "  \"hid_package_root\": \""
          << base
          << "/hid\",\n"
             "  \"license_server_secret\": \"\"\n"
             "}\n";
    }

    std::string cfg = "--config";
    std::string ipc = "--ipc";
    std::string ipc_path = tmp_socket_path();
    char* argv[] = {const_cast<char*>("ttbox_core"), cfg.data(), config_path.data(),
                    ipc.data(), ipc_path.data(), nullptr};
    int rc = app.initialize(5, argv);
    CHECK_EQ(rc, 0);  // 修复前这里是 1（MODEL_NOT_SELECTED 直接退出）
    if (rc == 0) {
        // a) + b)：run() 存活 + IPC PING 应答 pong=true
        std::thread runner([&app] { app.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(app.running());  // 降级态：进程存活，等待模型激活

        std::string response;
        std::string error;
        bool ok = ttbox::core::ipc_request(ipc_path, R"({"type":"PING"})", response,
                                           2000, &error);
        CHECK(ok);
        if (ok) {
            ttbox::core::JsonParseResult pong = ttbox::core::json_parse(response);
            CHECK(pong.ok);
            if (pong.ok) {
                const ttbox::core::JsonValue* pong_data = pong.value.find("data");
                CHECK(pong_data != nullptr && pong_data->find("pong") &&
                      pong_data->find("pong")->is_bool() &&
                      pong_data->find("pong")->as_bool());
            }
        }

        // c)：GET_STATUS 诚实上报 runtime_running=false（等待模型，非假装运行）
        std::string status_resp;
        bool ok2 = ttbox::core::ipc_request(ipc_path, R"({"type":"GET_STATUS"})",
                                            status_resp, 2000, &error);
        CHECK(ok2);
        if (ok2) {
            ttbox::core::JsonParseResult st = ttbox::core::json_parse(status_resp);
            CHECK(st.ok);
            if (st.ok) {
                const ttbox::core::JsonValue* st_data = st.value.find("data");
                CHECK(st_data != nullptr);
                if (st_data) {
                    const ttbox::core::JsonValue* rr = st_data->find("runtime_running");
                    CHECK(rr != nullptr && rr->is_bool() && !rr->as_bool());
                }
            }
        }

        ttbox::core::Application::request_shutdown();
        runner.join();
        app.shutdown();
        CHECK(!app.running());
    }

    // 清理临时目录（保留失败现场意义不大：registry 内容都是本测试生成的）
    std::filesystem::remove_all(base, fec);
}
