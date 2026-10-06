// WebServer.cpp — WebServer 实现（见 .hpp 头注释）。
#include "web/WebServer.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "common/Paths.hpp"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_routes.hpp"
#include "web/translate/profile_translate.hpp"

namespace ttbox::core::web {

namespace {

// 解析 IPv4 点分十进制为 32 位整数；格式非法返回 false。
bool parse_ipv4(const std::string& text, uint32_t* out) {
    uint32_t value = 0;
    uint32_t current = 0;
    int octets = 0;
    bool has_digit = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + static_cast<uint32_t>(c - '0');
            if (current > 255) return false;
            has_digit = true;
        } else if (c == '.') {
            if (!has_digit) return false;
            value = (value << 8) | current;
            ++octets;
            current = 0;
            has_digit = false;
        } else {
            return false;
        }
    }
    if (!has_digit) return false;
    value = (value << 8) | current;
    ++octets;
    if (octets != 4) return false;
    *out = value;
    return true;
}

// 统一 JSON 信封 / 路径拼接 / 文件读取已收敛到 domain_internal.hpp（inline 单点），
// 此处不再重复定义，避免同名重载歧义（WebServer.cpp 现 include 了 domain_internal.hpp）。

}  // namespace

WebServer::WebServer(Options options)
    : options_(options),
      ipc_(options.ipc_socket.empty() ? std::string(ttbox::core::paths::kIpcSocketDefault)
                                      : options.ipc_socket),
      static_dir_(join_path(options.web_root, "static")),
      template_dir_(join_path(options.web_root, "templates")) {}

void WebServer::register_routes() {
    svr_.Get("/api/health", [this](const httplib::Request& req, httplib::Response& res) {
        handle_health(req, res);
    });

    // ---- D1 来源校验落地（方案 §5.4）：登记特权端点，非可信来源 → 403 forbidden_source ----
    add_privileged_path("POST", "/api/ota/install");
    add_privileged_path("POST", "/api/update/install");
    add_privileged_path("POST", "/api/system/reboot");
    add_privileged_path("POST", "/api/system/poweroff");
    add_privileged_path("POST", "/api/system/storage/expand");
    add_privileged_path("POST", "/api/system/reactivate");
    add_privileged_path("POST", "/api/control/calibration/start");

    // ---- runtime_profile_getter 接线：经 IPC 读 GET_CONFIG.runtime_profile，
    //      让 web_body_to_profile 的 FOV prev_profile 深合并语义真正生效 ----
    set_runtime_profile_getter([this] { return ipc_runtime_profile_or_null(ipc_); });

    // ---- 业务域路由（T03 核心域）----
    register_control_routes(svr_, ipc_);
    register_state_routes(svr_, ipc_);
    register_models_routes(svr_, ipc_);
    register_models_meta_routes(svr_, ipc_);
    register_presets_routes(svr_, ipc_);
    register_hardware_routes(svr_, ipc_);
    register_license_routes(svr_, ipc_);
    register_ota_routes(svr_, ipc_);

    // ---- T04 辅助域 + 静态 ----
    register_system_routes(svr_, ipc_);
    register_diagnostics_routes(svr_, ipc_);
    register_brand_routes(svr_, ipc_);
    register_pages_routes(svr_, ipc_);
    register_aim_routes(svr_, ipc_);
    register_preview_routes(svr_, ipc_);

    // ---- T05 标定/运动 ----
    register_calib_routes(svr_, ipc_);
    register_motion_routes(svr_, ipc_);
}

bool WebServer::serve_static() {
    // 前端 JS/CSS/HTML 不动：只做静态托管（static/ 与 templates/）。
    // set_mount_point 在目录不存在时返回 false —— 汇总上报，供 start() 决策。
    const bool static_ok = svr_.set_mount_point("/static", static_dir_);
    const bool templates_ok = svr_.set_mount_point("/templates", template_dir_);
    return static_ok && templates_ok;
}

bool WebServer::start(std::string* error) {
    // 线程模型对齐旧 waitress serve(threads=64)。
    // new_task_queue 每次 new ThreadPool 不 delete：TaskQueue 所有权归 Server，
    // 其析构会回收（httplib 既有约定，非泄漏）。
    const int threads = options_.threads;
    svr_.new_task_queue = [threads] {
        return new httplib::ThreadPool(static_cast<size_t>(threads));
    };
    // 请求体上限对齐 Python MAX_CONTENT_LENGTH（256 MB，模型上传）。
    svr_.set_payload_max_length(256u * 1024u * 1024u);

    // ★ V1.0.47 上板故障根因：升级瞬间旧 Flask(waitress) 退出后，8000 端口存在 TIME_WAIT
    //   连接，而 httplib 默认在 Linux 下只设 SO_REUSEPORT（不解决 TIME_WAIT 端口复用）
    //   ⇒ 新进程 bind 8000 报 listen 失败 → 健康检查失败 → 升级回滚。
    //   这里用 set_socket_options 覆盖为「SO_REUSEADDR（解决 TIME_WAIT）+ SO_REUSEPORT（保留
    //   原多进程语义）」，让升级重启时端口能被立即复用。Windows 侧只设 SO_REUSEADDR。
    svr_.set_socket_options([](socket_t sock) {
        int yes = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const void*>(&yes), sizeof(yes));
#ifdef SO_REUSEPORT
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
                   reinterpret_cast<const void*>(&yes), sizeof(yes));
#endif
#endif
    });

    svr_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        return enforce_gate(req, res);
    });

    register_routes();
    if (!serve_static()) {
        // 静态目录缺失 → 面板静态资源不可用，但 IPC/API 路由不受影响，故只告警不阻断。
        std::cerr << "ttbox_web: 警告 静态目录挂载失败 static=" << static_dir_
                  << " templates=" << template_dir_ << std::endl;
    }

    if (!svr_.listen(options_.host, options_.port)) {
        if (error != nullptr) {
            *error = "listen 失败 " + options_.host + ":" + std::to_string(options_.port);
        }
        return false;
    }
    return true;
}

bool WebServer::is_trusted_source(const std::string& remote_addr) {
    if (remote_addr == "::1") return true;  // IPv6 loopback

    std::string addr = remote_addr;
    // IPv4-mapped IPv6（如 "::ffff:192.168.1.5"）还原为 IPv4 再判。
    constexpr const char kIpv4MappedPrefix[] = "::ffff:";
    constexpr size_t kIpv4MappedPrefixLen = sizeof(kIpv4MappedPrefix) - 1;
    if (addr.compare(0, kIpv4MappedPrefixLen, kIpv4MappedPrefix) == 0) {
        addr = addr.substr(kIpv4MappedPrefixLen);
    }

    uint32_t ip = 0;
    if (!parse_ipv4(addr, &ip)) return false;  // 其余 IPv6 / 非法地址 → 非可信
    if ((ip & 0xFF000000u) == 0x7F000000u) return true;  // 127.0.0.0/8 loopback
    if ((ip & 0xFF000000u) == 0x0A000000u) return true;  // 10.0.0.0/8
    if ((ip & 0xFFF00000u) == 0xAC100000u) return true;  // 172.16.0.0/12
    if ((ip & 0xFFFF0000u) == 0xC0A80000u) return true;  // 192.168.0.0/16
    return false;
}

void WebServer::add_privileged_path(const std::string& method, const std::string& path) {
    privileged_paths_.insert(method + " " + path);
}

httplib::Server::HandlerResponse WebServer::enforce_gate(const httplib::Request& req,
                                                         httplib::Response& res) {
    // T01 骨架：特权端点集合为空 ⇒ 当前对所有来源放行（与迁移前 Python 免密行为一致）。
    // T03 填入 §5.4 清单后，此分支对「非可信来源访问特权端点」返回 403 forbidden_source。
    if (is_privileged_path(req.method, req.path) && !is_trusted_source(req.remote_addr)) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"forbidden_source"})", "application/json");
        return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
}

bool WebServer::is_privileged_path(const std::string& method, const std::string& path) const {
    return privileged_paths_.count(method + " " + path) > 0;
}

void WebServer::handle_health(const httplib::Request&, httplib::Response& res) {
    std::string ping_error;
    const bool reachable = ipc_.ping(&ping_error);

    // 再走一次 call() 全链路（GET_STATUS），验证 NDJSON 请求/响应往返。
    const JsonValue status_resp = ipc_.call("GET_STATUS", JsonValue::object(),
                                            kIpcTimeoutDefaultMs);
    const JsonValue* status_v = status_resp.find("status");
    const int core_status = status_v != nullptr ? static_cast<int>(status_v->as_int()) : -1;

    JsonValue data = JsonValue::object();
    data.set("backend", JsonValue::string("cpp"));
    data.set("core_reachable", JsonValue::boolean(reachable));
    data.set("core_status", JsonValue::number(static_cast<double>(core_status)));

    send_json(res, 200, true, data, "", "");
}

}  // namespace ttbox::core::web
