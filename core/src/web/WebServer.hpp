// WebServer.hpp — httplib 包装：路由注册 + 静态托管 + 来源校验中间件（T01 骨架）
//
// 分层（方案 §3.1）：
//   HTTP 层（本类）→ 业务层（domain/，T02–T05 填）→ 基础设施层（infra/，复用 core）。
// T01 只落地 HTTP 壳的最小可运行骨架：/api/health 探针 + 静态托管 + 来源校验框架。
#pragma once

#include <set>
#include <string>

#include "httplib.h"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

// 面板监听默认值（0.0.0.0:8000，threads=64）。★ V1.0.52：原与 plugins/web/lib/settings.py
// 同值，该文件已随 Python 后端移除 ⇒ 此处成为唯一真源（env TTBOX_WEB_PORT 可覆盖）。
inline constexpr const char* kDefaultHost = "0.0.0.0";
inline constexpr int kDefaultPort = 8000;
inline constexpr int kDefaultThreads = 64;  // 对齐旧 waitress serve(threads=64)

class WebServer {
public:
    struct Options {
        std::string host = kDefaultHost;
        int port = kDefaultPort;
        int threads = kDefaultThreads;
        std::string web_root = ".";  // 含 static/ 与 templates/ 的插件根
        std::string ipc_socket;      // 留空 → core/src/common/Paths.hpp 默认值
    };

    explicit WebServer(Options options);

    // 注册全部路由。T01 仅最小骨架：/api/health + 页面占位；86 条域路由由 T02–T05 增补。
    void register_routes();

    // 挂载静态资源目录（static/ 与 templates/；前端不动，C++ 只托管）。
    // 返回 false 表示任一目录缺失（set_mount_point 失败），start() 据此告警但不阻断。
    bool serve_static();

    // 阻塞式监听。失败（如端口占用）返回 false 并写入 error。
    bool start(std::string* error = nullptr);

    // 来源校验（D1 §5.4）：放行 loopback（127.0.0.0/8、::1）与 RFC1918 私有网段，
    // 其余判为非可信。
    static bool is_trusted_source(const std::string& remote_addr);

    // 特权端点登记（T03 填 §5.4 清单：/api/ota/install 等）。T01 集合为空 ⇒ 全放行。
    void add_privileged_path(const std::string& method, const std::string& path);

private:
    // pre-routing 中间件：特权端点 + 非可信来源 → 403 forbidden_source。
    httplib::Server::HandlerResponse enforce_gate(const httplib::Request& req,
                                                  httplib::Response& res);

    bool is_privileged_path(const std::string& method, const std::string& path) const;

    // 健康探针：验证 HTTP → IPC（ipc_ping + call）全链路。
    void handle_health(const httplib::Request& req, httplib::Response& res);

    Options options_;
    IpcClient ipc_;
    httplib::Server svr_;
    std::set<std::string> privileged_paths_;  // 元素形如 "POST /api/ota/install"
    std::string static_dir_;
    std::string template_dir_;
};

}  // namespace ttbox::core::web
