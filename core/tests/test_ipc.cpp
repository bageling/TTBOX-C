// test_ipc.cpp — IPC 服务端：PING / GET_STATUS / GET_CONFIG / 错误处理
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "common/Json.hpp"
#include "common/Metrics.hpp"
#include "ipc/IpcServer.hpp"
#include "test_util.hpp"

namespace {

std::string tmp_socket_path() {
#if defined(_WIN32)
    return "tcp:0";  // 临时端口：OS 分配唯一端口，start() 后从 socket_path() 读回，
                     // 彻底消除多用例共用固定端口的 TIME_WAIT 竞态
#else
    return "/tmp/ttbox_core_test_" + std::to_string(static_cast<long>(::getpid())) + ".sock";
#endif
}

// Windows TIME_WAIT 下固定端口偶发 bind 失败：带重试的启动（总等待 ~1s）
static bool start_with_retry(ttbox::core::IpcServer& server, std::string* error = nullptr) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (server.start(tmp_socket_path(), error)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

ttbox::core::SystemStatus test_status() {
    ttbox::core::SystemStatus st;
    st.running = true;
    st.app_name = "test";
    st.version = "0.0.0";
    st.uptime_ms = 12.5;
    st.ipc_socket = "test.sock";
    st.config_file = "test.json";
    st.metrics.last_frame = 42;
    st.metrics.last_timestamp_us = 1789000000123456ULL;
    return st;
}

ttbox::core::JsonValue test_config_json() {
    ttbox::core::JsonValue cfg = ttbox::core::JsonValue::object();
    cfg.set("conf", ttbox::core::JsonValue::string("0.25"));
    cfg.set("nms", ttbox::core::JsonValue::string("0.45"));
    return cfg;
}

}  // namespace

TEST(ipc_ping_roundtrip) {
    ttbox::core::IpcServer server;
    std::string error;
    CHECK(start_with_retry(server, &error));
    if (!server.running()) {
        std::printf("  [info] server start failed: %s\n", error.c_str());
        return;
    }

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(), R"({"type":"PING"})", response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 0);
        const auto* data_v = parsed.value.find("data");
        CHECK(data_v != nullptr);
        if (data_v != nullptr) {
            const auto* pong_v = data_v->find("pong");
            CHECK(pong_v != nullptr && pong_v->as_bool() == true);
        }
    }
    server.stop();
}

TEST(ipc_get_status) {
    ttbox::core::IpcServer server;
    server.set_status_provider(test_status);
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(), R"({"type":"GET_STATUS"})", response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 0);
        const auto* data_v = parsed.value.find("data");
        CHECK(data_v != nullptr);
        if (data_v != nullptr) {
            const auto* running_v = data_v->find("running");
            CHECK(running_v != nullptr && running_v->as_bool() == true);
            const auto* version_v = data_v->find("version");
            CHECK(version_v != nullptr && version_v->as_string() == "0.0.0");
            const auto* metrics_v = data_v->find("metrics");
            CHECK(metrics_v != nullptr);
            if (metrics_v != nullptr) {
                const auto* last_frame_v = metrics_v->find("last_frame");
                CHECK(last_frame_v != nullptr && last_frame_v->as_int() == 42);
                const auto* last_ts_v = metrics_v->find("last_timestamp_us");
                CHECK(last_ts_v != nullptr && last_ts_v->as_int() == 1789000000123456LL);
            }
        }
    }
    server.stop();
}

TEST(ipc_get_config) {
    ttbox::core::IpcServer server;
    server.set_config_provider(test_config_json);
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(), R"({"type":"GET_CONFIG"})", response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 0);
        const auto* data_v = parsed.value.find("data");
        CHECK(data_v != nullptr && data_v->is_object());
        if (data_v != nullptr && data_v->is_object()) {
            const auto* conf_v = data_v->find("conf");
            CHECK(conf_v != nullptr && conf_v->as_string() == "0.25");
        }
    }
    server.stop();
}

TEST(ipc_unsupported_type_error) {
    ttbox::core::IpcServer server;
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(), R"({"type":"DO_NOTHING"})", response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        // 错误码 4 = UNSUPPORTED
        CHECK(status_v != nullptr && status_v->as_int() == 4);
    }
    server.stop();
}

TEST(ipc_bad_json_error) {
    ttbox::core::IpcServer server;
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(), "{ not json", response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        // 错误码 1 = BAD_REQUEST
        CHECK(status_v != nullptr && status_v->as_int() == 1);
    }
    server.stop();
}

TEST(ipc_stop_drains_inflight_connections) {
    // 回归：旧实现使用 detached 连接线程，但 stop() 只 join accept 线程。
    // 对象析构后连接线程继续访问 handler/active_connections_，会随机 UAF 崩溃。
    for (int round = 0; round < 30; ++round) {
        ttbox::core::IpcServer server;
        std::atomic<bool> handler_entered{false};
        server.set_status_provider([&handler_entered] {
            handler_entered.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            return test_status();
        });
        std::string error;
        CHECK(start_with_retry(server, &error));
        if (!server.running()) return;

        const std::string path = server.socket_path();
        std::thread client([path] {
            std::string response;
            std::string error;
            ttbox::core::ipc_request(path, R"({"type":"GET_STATUS"})", response, 1000, &error);
        });
        for (int spin = 0; spin < 100 && !handler_entered.load(std::memory_order_acquire); ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(handler_entered.load(std::memory_order_acquire));
        server.stop();  // 必须等连接线程彻底退出后才返回
        client.join();
        CHECK(!server.running());
    }
}

TEST(ipc_client_connection_refused) {
    std::string response;
    std::string error;
    bool ok = ttbox::core::ipc_request("/tmp/nonexistent_ttbox_core.sock",
                                       R"({"type":"PING"})", response, 500, &error);
    CHECK(!ok);  // 连接被拒 → 明确失败
}

// SO_RCVTIMEO 单位回归（2026-09-23 定位）：
// 客户端给 2000ms 超时，服务端故意慢 300ms —— 必须拿到响应。
// Windows 上 SO_RCVTIMEO 取 DWORD 毫秒；旧实现传 struct timeval，内核只读走前 4 字节
// （= tv_sec），2000ms 被当成 2ms ⇒ 本用例在修好前稳定失败。
// 这条断言把「超时参数必须是毫秒、且真的生效」钉死，防止再退回按秒解释。
TEST(ipc_recv_timeout_honors_milliseconds) {
    ttbox::core::IpcServer server;
    server.set_status_provider([]() -> ttbox::core::SystemStatus {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return test_status();
    });
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = ttbox::core::ipc_request(server.socket_path(), R"({"type":"GET_STATUS"})",
                                             response, 2000, &error);
    const auto cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    if (!ok) {
        std::printf("  [info] 慢响应请求失败: %s (cost=%lldms)\n", error.c_str(),
                    static_cast<long long>(cost_ms));
    }
    CHECK(ok);
    if (ok) {
        auto parsed = ttbox::core::json_parse(response);
        CHECK(parsed.ok);
        if (parsed.ok) {
            const auto* status_v = parsed.value.find("status");
            CHECK(status_v != nullptr && status_v->as_int() == 0);
        }
    }
    // 超时设置真的按毫秒生效：300ms 的响应不该被 2ms（旧 bug 的等效值）判死。
    CHECK(cost_ms >= 250);
}

// ---------------------------------------------------------------------------
// ★ 服务端必须给 accept 出来的 fd 设接收超时（2026-09-23 全仓审查复核 #16）
//
// 修复前：对端连上却不发一个字节时，连接线程永久卡在 read_line 的 recv 上、槽位不归还；
// 累计 kMaxConnections(32) 个半开连接后，所有新连接被立刻关闭，IPC 等同不可用
// （客户端表现为「连接成功却读不到响应」）。
//
// 反向验证：把 IpcServer::handle_connection 开头那段 set_recv_timeout_ms 去掉后，
// 本用例必须变红（客户端等满 12s 也拿不到 EOF）—— 否则这个断言是空的。
// ---------------------------------------------------------------------------

namespace {

// 只建立连接、不发任何字节。成功返回 fd，失败 -1。
int silent_connect(const std::string& socket_path) {
#if defined(_WIN32)
    std::string port_str = socket_path;
    const std::string prefix = "tcp:";
    if (port_str.rfind(prefix, 0) == 0) port_str = port_str.substr(prefix.size());
    const int port = std::atoi(port_str.c_str());
    if (port <= 0) return -1;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s);
        return -1;
    }
    return static_cast<int>(s);
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
#endif
}

void fd_close(int fd) {
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

// 给客户端 fd 设接收超时（毫秒）。★ 两平台参数类型不同，与 IpcServer.cpp 的
// set_recv_timeout_ms 同款写法 —— Windows 收 DWORD 毫秒，Unix 才收 struct timeval。
void fd_set_recv_timeout(int fd, int timeout_ms) {
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(timeout_ms);
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

long long fd_recv_one(int fd, char* b) {
#if defined(_WIN32)
    return static_cast<long long>(::recv(static_cast<SOCKET>(fd), b, 1, 0));
#else
    return static_cast<long long>(::recv(fd, b, 1, 0));
#endif
}

}  // namespace

TEST(ipc_server_drops_silent_connection) {
    ttbox::core::IpcServer server;
    std::string error;
    CHECK(start_with_retry(server, &error));

    const int fd = silent_connect(server.socket_path());
    CHECK(fd >= 0);
    if (fd < 0) return;

    // 服务端超时是 5s（IpcServer.cpp kServerRecvTimeoutMs）。客户端给 12s，
    // 修复前这里会一直等到客户端超时 ⇒ n != 0 ⇒ 用例红。
    fd_set_recv_timeout(fd, 12000);
    char b = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const long long n = fd_recv_one(fd, &b);
    const auto cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();

    if (n != 0) {
        std::printf("  [info] 静默连接未被服务端关闭: n=%lld cost=%lldms\n", n,
                    static_cast<long long>(cost_ms));
    }
    // 0 = 收到 EOF，即服务端主动关了这条没发数据的连接。RST 之类的负数不在此列，
    // 用严格的 0 判定，避免把「客户端自己超时」也算通过。
    CHECK(n == 0);
    fd_close(fd);
}
