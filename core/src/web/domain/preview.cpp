// preview.cpp — MJPEG 预览流（1 条路由，URL 一字未改）。
//
// 自 plugins/web/api/preview.py 逐行为移植。优先 core GET_PREVIEW IPC（base64 JPEG
// 解码后组 multipart 帧）；IPC 不可达时回退到 8001 的 socket 级透传代理（剥上游
// HTTP 头只转发 multipart body，等价 Python proxy_stream）。
#include "web/domain/domain_routes.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "auth/LicenseCard.hpp"
#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// 上游预览地址（TTBOX_PREVIEW_URL，默认 127.0.0.1:8001）。
std::pair<std::string, int> preview_upstream() {
    const char* v = std::getenv("TTBOX_PREVIEW_URL");
    std::string url = (v != nullptr && *v != '\0') ? std::string(v) : std::string("");
    std::string host = "127.0.0.1";
    int port = 8001;
    if (!url.empty()) {
        const size_t scheme = url.find("://");
        std::string rest = (scheme == std::string::npos) ? url : url.substr(scheme + 3);
        while (!rest.empty() && rest.back() == '/') rest.pop_back();
        const size_t slash = rest.find('/');
        if (slash != std::string::npos) rest = rest.substr(0, slash);
        const size_t colon = rest.rfind(':');
        if (colon != std::string::npos) {
            host = rest.substr(0, colon);
            port = std::atoi(rest.substr(colon + 1).c_str());
            if (port <= 0 || port > 65535) port = 8001;
        } else {
            host = rest;
        }
    }
    return {host, port};
}

// socket 透传代理状态（跨 provider 调用复用同一条上游连接）。
struct PreviewProxy {
    int sock = -1;
    bool headers_done = false;
    std::string buf;
    std::string host;
    int port = 8001;
};

// 上游代理帧源：连接 8001，剥 HTTP 头，透传 multipart body。
bool preview_proxy_frame(std::shared_ptr<PreviewProxy> px, httplib::DataSink& sink) {
    if (px->sock < 0) {
        px->sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (px->sock < 0) return false;
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        ::setsockopt(px->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(px->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        const std::string portstr = std::to_string(px->port);
        if (::getaddrinfo(px->host.c_str(), portstr.c_str(), &hints, &res) != 0 || res == nullptr) {
            ::close(px->sock);
            px->sock = -1;
            return false;
        }
        const int rc = ::connect(px->sock, res->ai_addr, res->ai_addrlen);
        ::freeaddrinfo(res);
        if (rc != 0) {
            ::close(px->sock);
            px->sock = -1;
            return false;
        }
        const std::string req = "GET /api/preview.mjpg HTTP/1.1\r\nHost: " + px->host + ":" +
                                std::to_string(px->port) + "\r\n\r\n";
        ::send(px->sock, req.data(), req.size(), 0);
    }
    char buf[65536];
    const ssize_t n = ::recv(px->sock, buf, sizeof(buf), 0);
    if (n <= 0) {
        ::close(px->sock);
        px->sock = -1;
        return false;
    }
    if (!px->headers_done) {
        px->buf += std::string(buf, static_cast<size_t>(n));
        const size_t hdr = px->buf.find("\r\n\r\n");
        if (hdr == std::string::npos) return sink.is_writable();
        const std::string body = px->buf.substr(hdr + 4);
        px->buf.clear();
        px->headers_done = true;
        if (!body.empty()) {
            if (!sink.write(body.data(), body.size())) return false;
        }
        return sink.is_writable();
    }
    if (!sink.write(buf, static_cast<size_t>(n))) return false;
    return sink.is_writable();
}

}  // namespace

void register_preview_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/api/preview.mjpg", [&ipc](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store, no-cache");
        res.set_header("X-Accel-Buffering", "no");
        auto last_seq = std::make_shared<int64_t>(-1);
        auto [phost, pport] = preview_upstream();
        auto proxy = std::make_shared<PreviewProxy>();
        proxy->host = phost;
        proxy->port = pport;
        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=ttboxframe",
            [&ipc, last_seq, proxy](size_t, httplib::DataSink& sink) -> bool {
                // ★ V1.0.49：动态判断（不再启动时冻结一次 ipc_mode）。
                //   IPC 可达 ⇒ 走直出；即便此刻暂无帧（预览未开）也只 sleep 重试、不回退
                //   proxy —— 这样面板开着时「开启预览」下一 tick 即出图，无需刷新页面
                //   （V1.0.48 实测：启动探测冻结成 proxy 后，开启预览要刷新一次才出图）。
                //   IPC 完全不可达（core 未运行）才回退 8001 proxy。
                const JsonValue r = ipc.call("GET_PREVIEW", JsonValue::object(), 2000);
                if (ipc_status(r) != 0) {
                    return preview_proxy_frame(proxy, sink);
                }
                const JsonValue* d = r.find("data");
                if (d != nullptr && d->is_object()) {
                    const std::string b64 = json_field(*d, "jpeg_base64").as_string("");
                    const int64_t seq = json_field(*d, "seq").as_int(0);
                    if (!b64.empty() && seq != *last_seq) {
                        std::vector<uint8_t> px;
                        if (ttbox::core::auth::license_base64_decode(b64, &px) && !px.empty()) {
                            *last_seq = seq;
                            std::string frame =
                                "--ttboxframe\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                                std::to_string(px.size()) + "\r\n\r\n";
                            frame.append(reinterpret_cast<const char*>(px.data()), px.size());
                            frame += "\r\n";
                            if (!sink.write(frame.data(), frame.size())) return false;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return sink.is_writable();
            });
    });
}

}  // namespace ttbox::core::web
