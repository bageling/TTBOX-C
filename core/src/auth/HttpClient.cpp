#include "auth/HttpClient.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <cstring>
#include <sstream>
#include <string>

namespace ttbox::core::auth {

namespace {

#if defined(__linux__) || defined(__APPLE__)
// 建立到 host:port 的 TCP 连接（含 10s 收发超时）；成功返回 fd，失败返回 -1 并写 err。
int tcp_connect(const std::string& host, int port, std::string* err) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), port_str, &hints, &res);
    if (rc != 0) {
        if (err) *err = std::string("getaddrinfo: ") + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        // 10s connect timeout
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && err) {
        *err = std::string("connect failed (host=") + host + ":" + port_str + ")";
    }
    return fd;
}

// 创建 TLS 客户端上下文：系统默认 CA + VERIFY_PEER + AUTO_RETRY；失败返回 nullptr。
SSL_CTX* make_ssl_ctx() {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    const SSL_METHOD* m = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(m);
    if (!ctx) return nullptr;
    // 使用系统默认 CA bundle（Debian /usr/lib/ssl/certs/ca-certificates.crt）
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    return ctx;
}

// 在已连的 fd 上完成 TLS 握手、发送 POST 表单、读全响应；返回 body，写 out_status/out_err。
std::string send_https_post(int fd,
                             SSL_CTX* ctx,
                             const std::string& host,
                             const std::string& path,
                             const std::string& body,
                             int* out_status,
                             std::string* out_err) {
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        if (out_err) *out_err = "SSL_new failed";
        return {};
    }
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        if (out_err) *out_err = "SSL_connect failed";
        SSL_free(ssl);
        return {};
    }
    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "Content-Type: application/x-www-form-urlencoded\r\n";
    req << "Content-Length: " << body.size() << "\r\n";
    req << "Connection: close\r\n";
    req << "User-Agent: TTBox/" << "1.0" << "\r\n";
    req << "\r\n";
    req << body;
    std::string reqs = req.str();
    const char* p = reqs.data();
    size_t remain = reqs.size();
    while (remain > 0) {
        int w = SSL_write(ssl, p, static_cast<int>(remain));
        if (w <= 0) {
            if (out_err) *out_err = "SSL_write failed";
            SSL_shutdown(ssl);
            SSL_free(ssl);
            return {};
        }
        p += w;
        remain -= static_cast<size_t>(w);
    }
    std::string total;
    char buf[4096];
    for (;;) {
        int r = SSL_read(ssl, buf, sizeof(buf));
        if (r > 0) {
            total.append(buf, static_cast<size_t>(r));
        } else {
            break;
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    // 解析 HTTP 响应：
    // HTTP/1.1 200 OK\r\n...\r\n\r\n<body>
    *out_status = 0;
    size_t line1 = total.find("\r\n");
    if (line1 == std::string::npos) {
        if (out_err) *out_err = "no HTTP status line";
        return total;
    }
    std::string l1 = total.substr(0, line1);
    auto sp1 = l1.find(' ');
    if (sp1 != std::string::npos) {
        *out_status = std::atoi(l1.substr(sp1 + 1).c_str());
    }
    size_t hdr_end = total.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        if (out_err) *out_err = "no header/body separator";
        return total;
    }
    return total.substr(hdr_end + 4);
}
#endif

}  // namespace

// 证书公钥 pin 注入占位接口：当前为空实现（阶段 5 再补）。
void https_set_pubkey_pins(const char** /*sha256_hex_pins*/, int /*count*/) {
    // 预留：阶段 5 再实现 pinning
}

// HTTPS 表单 POST 的对外入口：解析连接 → 握手发送 → 填充 status/body/error 返回。
HttpsPostResult https_post_form(const std::string& host,
                                 int port,
                                 const std::string& path,
                                 const std::string& form_body) {
    HttpsPostResult r;
#if defined(__linux__) || defined(__APPLE__)
    static SSL_CTX* s_ctx = make_ssl_ctx();
    if (!s_ctx) {
        r.error = "SSL_CTX init failed";
        return r;
    }
    std::string err;
    int fd = tcp_connect(host, port, &err);
    if (fd < 0) {
        r.error = err;
        return r;
    }
    int status = 0;
    r.body = send_https_post(fd, s_ctx, host, path, form_body, &status, &err);
    ::close(fd);
    r.status_code = status;
    if (r.status_code == 0) r.error = err.empty() ? std::string("no response") : err;
#else
    r.error = "https_post_form not implemented for this platform";
#endif
    return r;
}

}  // namespace ttbox::core::auth
