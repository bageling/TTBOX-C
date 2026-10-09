// main.cpp — ttbox_web 入口（替代 plugins/web/bin/ttbox-web.py；T01 最小可运行骨架）
//
// 运行期配置来自环境变量（systemd 单元已注入）：
//   TTBOX_WEB_PORT                    — 监听端口（默认 8000）
//   TTBOX_ROOT                      — 插件根（含 static/ 与 templates/；默认 CWD）
//   TTBOX_IPC_SOCKET                — Core IPC socket（留空走 core Paths.hpp 单点真源）
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "web/WebServer.hpp"

namespace {

// 读环境变量；未设置或为空则返回 fallback。
std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string(value) : fallback;
}

}  // namespace

// 进程入口：从环境变量装配 Options 并启动 WebServer，启动失败返回 1。
int main() {
    using ttbox::core::web::WebServer;

    WebServer::Options options;
    // 监听地址为编译期单点（WebServer.hpp::kDefaultHost）；V-20 已删除 TTBOX_WEB_HOST，不得复活。
    options.host = ttbox::core::web::kDefaultHost;
    options.port = std::atoi(env_or("TTBOX_WEB_PORT", "8000").c_str());
    // 端口越界（非 1~65535）时回退编译期默认值。
    if (options.port <= 0 || options.port > 65535) {
        options.port = ttbox::core::web::kDefaultPort;
    }
    options.threads = ttbox::core::web::kDefaultThreads;
    options.web_root = env_or("TTBOX_ROOT", ".");
    options.ipc_socket = env_or("TTBOX_IPC_SOCKET", "");  // 空 → WebServer 取 Paths.hpp 默认

    WebServer server(std::move(options));
    std::string error;
    if (!server.start(&error)) {
        std::cerr << "ttbox_web: " << error << std::endl;
        return 1;
    }
    return 0;
}
