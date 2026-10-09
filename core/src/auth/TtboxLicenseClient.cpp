#include "auth/TtboxLicenseClient.hpp"
#include "auth/LicenseConstants.hpp"   // B-CONST-4：心跳 60/180 单点真源

#include "auth/TtboxCanonical.hpp"   // T1.07：canonical 纯函数（末尾无换行，M2.07 修复）
#include "common/Json.hpp"
#include "common/Logger.hpp"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <random>
#include <vector>

#if defined(_WIN32)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#if defined(__linux__) || defined(__APPLE__)
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "common/Json.hpp"

namespace ttbox::core::auth {

namespace {

// 取当前墙钟 Unix 毫秒。
int64_t now_unix_ms() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// 取当前墙钟 Unix 秒的十进制字符串（用于 HMAC 时间戳）。
std::string unix_timestamp_sec() {
    return std::to_string(
        static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()));
}

// 生成随机 nonce（random_device 播种，抗预测），len 默认 16。
std::string generate_nonce(int len = 16) {
    static const char chars[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    // ★ 2026-09-23：原来用 system_clock 播种 mt19937 ⇒ nonce **可预测**
    //   （攻击者只要知道大致时间就能枚举），HMAC 签名里的 nonce 等于没起作用。
    //   改用 random_device（Linux 下读 /dev/urandom）。
    //   注：本文件当前被 AUTH 门控、线上只装 OfflineCardClient，但 T2.03 一旦
    //   打开在线接入就会走到这里 ⇒ 现在改比那时改便宜。
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, sizeof(chars) - 2);
    std::string s;
    s.reserve(len);
    for (int i = 0; i < len; ++i) s += chars[dist(gen)];
    return s;
}

// ---- M2：签名卡内容解析（features / plan / uiBrand）----
//
// 字段命名沿用本协议既有的 camelCase 约定（clientToken / expireAt / heartbeatInterval…），
// 故品牌字段名为 `uiBrand`，而出参结构体字段为 `ui_brand`。
//
// ★ 服务端契约要求：card-login 与 heartbeat 的**成功响应都必须携带这三个字段**
//   （features 可为空数组，表示明确不授予任何功能位）。原因：
//   本函数只负责「把服务端说什么写进 out」，不持有历史；若服务端在成功响应里省略字段，
//   调用方将无法区分「服务端本轮没下发」与「服务端明确撤销」——
//   前者会误判成后者，表现为功能位每次心跳后静默清空（难排查的生产事故）。
//   失败路径的「回落历史值」由 apply_check_result 负责（见 LicenseStateMachine.hpp），
//   本函数**不**做任何回落或过滤：归一化与闭集过滤唯一发生在 to_snapshot。
//
// 缺失字段的处理：不写入 ⇒ out 对应项保持默认空 ⇒ 由状态机按失败/成功路径分别裁决。
// 从成功响应根对象提取 features / plan / uiBrand 写入 out（缺字段不写，由上层裁决）。
void parse_card_content(const JsonValue& root, LicenseStatus& out) {
    if (const JsonValue* f = root.find("features"); f != nullptr && f->is_array()) {
        std::vector<std::string> feats;
        feats.reserve(f->as_array().size());
        for (const JsonValue& v : f->as_array()) {
            if (v.is_string()) feats.push_back(v.as_string());
        }
        out.features = std::move(feats);
    }
    if (const JsonValue* p = root.find("plan"); p != nullptr && p->is_string()) {
        out.plan = p->as_string();
    }
    if (const JsonValue* b = root.find("uiBrand"); b != nullptr && b->is_string()) {
        out.ui_brand = b->as_string();
    }
}

// ---- 服务端地址解析（2026-09-26 第三轮审计修复）----
//
// ★ 旧 parse_url_host 把 base_url 的**路径段丢弃**（"https://cctv2.top:10086/ttbox"
//   只剩 host:port），请求行永远少一截前缀。现在四元组全保留：
//   scheme/http(s) + host + port + base_path（如 "/ttbox"）。
//   HMAC canonical 仍签**不含 base_path** 的 path（对齐 Python cloud_client：
//   它 sign(path_with_query)、请求 base_url + path —— 两端同一口径）。
struct ServerEndpoint {
    std::string scheme;    // "http" / "https"（小写）
    std::string host;
    int port = 0;
    std::string base_path; // "" 或 "/ttbox" 这类前缀（保证以 / 开头或为空）
};

// 解析 server_url 为 scheme/host/port/base_path 四元组；非法即写 err 返回 false。
bool parse_server_endpoint(const std::string& url, ServerEndpoint& ep,
                           std::string* err) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        if (err) *err = "server_url 缺 scheme: " + url;
        return false;
    }
    ep.scheme = url.substr(0, scheme_end);
    for (char& c : ep.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ep.scheme != "http" && ep.scheme != "https") {
        if (err) *err = "server_url scheme 仅支持 http/https: " + url;
        return false;
    }
    std::string rest = url.substr(scheme_end + 3);
    const size_t slash = rest.find('/');
    const std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    ep.base_path = (slash == std::string::npos) ? std::string() : rest.substr(slash);
    // 规整：去尾部 '/'（拼接时统一为 base + "/api/..."）
    while (!ep.base_path.empty() && ep.base_path.back() == '/') ep.base_path.pop_back();
    if (hostport.empty()) {
        if (err) *err = "server_url 缺 host: " + url;
        return false;
    }
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        ep.host = hostport.substr(0, colon);
        const std::string port_str = hostport.substr(colon + 1);
        // ★ std::stoi 对非数字端口抛未捕获异常（第三轮审计附带发现）⇒ 收敛为错误
        try {
            ep.port = std::stoi(port_str);
        } catch (...) {
            if (err) *err = "server_url 端口非法: " + port_str;
            return false;
        }
        if (ep.port <= 0 || ep.port > 65535) {
            if (err) *err = "server_url 端口越界: " + port_str;
            return false;
        }
    } else {
        ep.host = hostport;
        ep.port = (ep.scheme == "https") ? 443 : 80;
    }
    if (ep.host.empty()) {
        if (err) *err = "server_url 缺 host: " + url;
        return false;
    }
    return true;
}

// 拼装 HTTP/1.1 请求报文（含 Host/Content-Type/附加头/可选 body）。
std::string build_request(const std::string& method,
                          const std::string& full_path,
                          const std::string& host,
                          const std::string& body_json,
                          const std::vector<std::pair<std::string, std::string>>& extra_headers,
                          bool with_body) {
    std::ostringstream req;
    req << method << " " << full_path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "Content-Type: application/json\r\n";
    if (with_body) req << "Content-Length: " << body_json.size() << "\r\n";
    for (const auto& kv : extra_headers) req << kv.first << ": " << kv.second << "\r\n";
    req << "Connection: close\r\n";
    req << "\r\n";
    if (with_body) req << body_json;
    return req.str();
}

// 从原始应答解析 status 与 body（Connection: close ⇒ 读到对端关闭为止）
bool parse_http_reply(const std::string& total, int* out_status, std::string* out_body,
                      std::string* err) {
    const size_t line1 = total.find("\r\n");
    if (line1 == std::string::npos) {
        if (err) *err = "no HTTP status line";
        return false;
    }
    const std::string l1 = total.substr(0, line1);
    const auto sp1 = l1.find(' ');
    if (sp1 == std::string::npos) {
        if (err) *err = "bad HTTP status line";
        return false;
    }
    *out_status = std::atoi(l1.substr(sp1 + 1).c_str());
    const size_t hdr_end = total.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        if (err) *err = "no header/body separator";
        return false;
    }
    *out_body = total.substr(hdr_end + 4);
    return true;
}

// ---- 明文 TCP 传输（http://）----
// 明文 TCP 收发：解析连接 → 发送 → 读到对端关闭；仅用于 http://。
bool send_recv_plain(const std::string& host, int port, const std::string& request,
                     std::string& out_total, std::string* err) {
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (err) *err = "WSAStartup failed";
        return false;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        WSACleanup();
        if (err) *err = "DNS resolve failed";
        return false;
    }
    SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == INVALID_SOCKET) { freeaddrinfo(res); WSACleanup(); if (err) *err = "socket failed"; return false; }
    if (connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        closesocket(fd); freeaddrinfo(res); WSACleanup();
        if (err) *err = "connect failed";
        return false;
    }
    freeaddrinfo(res);
    send(fd, request.c_str(), static_cast<int>(request.size()), 0);
    char buf[4096];
    int n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) out_total.append(buf, n);
    closesocket(fd);
    WSACleanup();
    return true;
#elif defined(__linux__) || defined(__APPLE__)
    struct addrinfo hints, *res = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        if (err) *err = "DNS resolve failed";
        return false;
    }
    int fd = -1;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv;
        tv.tv_sec = 10; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (err) *err = "connect failed";
        return false;
    }
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(request.size())) {
        const ssize_t w = send(fd, request.data() + sent, request.size() - sent, 0);
        if (w <= 0) { close(fd); if (err) *err = "send failed"; return false; }
        sent += w;
    }
    char buf[4096];
    ssize_t r;
    while ((r = recv(fd, buf, sizeof(buf), 0)) > 0)
        out_total.append(buf, static_cast<size_t>(r));
    close(fd);
    return true;
#else
    (void)host; (void)port; (void)request; (void)out_total; (void)err;
    return false;
#endif
}

// ---- TLS 传输（https://，OpenSSL；Linux/macOS）----
// TLS 收发：建 ctx（系统 CA + VERIFY_PEER + SNI）→ 握手/校验证书 → 发送 → 读到关闭。
bool send_recv_tls(const std::string& host, int port, const std::string& request,
                   std::string& out_total, std::string* err) {
#if defined(__linux__) || defined(__APPLE__)
    // 复用 HttpClient.cpp 的初始化口径：系统 CA + VERIFY_PEER（不静默降级明文）。
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        if (err) *err = "SSL_CTX_new failed";
        return false;
    }
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);

    std::string conn_err;
    const int fd = []([[maybe_unused]] const std::string& h, int p, std::string* e) -> int {
        // 与 HttpClient::tcp_connect 相同的解析/连接逻辑（含 10s 超时）
        struct addrinfo hints, *res = nullptr;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        char port_str[16];
        std::snprintf(port_str, sizeof(port_str), "%d", p);
        if (getaddrinfo(h.c_str(), port_str, &hints, &res) != 0) {
            if (e) *e = "DNS resolve failed";
            return -1;
        }
        int f = -1;
        for (struct addrinfo* q = res; q; q = q->ai_next) {
            f = socket(q->ai_family, q->ai_socktype, q->ai_protocol);
            if (f < 0) continue;
            struct timeval tv;
            tv.tv_sec = 10; tv.tv_usec = 0;
            setsockopt(f, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(f, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (connect(f, q->ai_addr, q->ai_addrlen) == 0) break;
            close(f); f = -1;
        }
        freeaddrinfo(res);
        if (f < 0 && e) *e = "connect failed";
        return f;
    }(host, port, &conn_err);
    if (fd < 0) {
        SSL_CTX_free(ctx);
        if (err) *err = conn_err;
        return false;
    }
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        close(fd); SSL_CTX_free(ctx);
        if (err) *err = "SSL_new failed";
        return false;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
        if (err) *err = "SSL_connect failed（证书/网络）";
        SSL_free(ssl); close(fd); SSL_CTX_free(ctx);
        return false;
    }
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        if (err) *err = "TLS 证书校验失败";
        SSL_shutdown(ssl); SSL_free(ssl); close(fd); SSL_CTX_free(ctx);
        return false;
    }
    const char* p = request.data();
    size_t remain = request.size();
    while (remain > 0) {
        const int w = SSL_write(ssl, p, static_cast<int>(remain));
        if (w <= 0) {
            if (err) *err = "SSL_write failed";
            SSL_shutdown(ssl); SSL_free(ssl); close(fd); SSL_CTX_free(ctx);
            return false;
        }
        p += w;
        remain -= static_cast<size_t>(w);
    }
    char buf[4096];
    for (;;) {
        const int r = SSL_read(ssl, buf, sizeof(buf));
        if (r > 0) out_total.append(buf, static_cast<size_t>(r));
        else break;
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    SSL_CTX_free(ctx);
    return true;
#else
    (void)host; (void)port; (void)request; (void)out_total;
    if (err) *err = "https not implemented on this platform";
    return false;
#endif
}

}  // namespace

// ---- HMAC-SHA256 (platform-dependent) ----

// 计算请求的 HMAC-SHA256 签名（canonical 无末尾换行）；secret 未配置则返回空串拒签。
std::string TtboxLicenseClient::sign_request(const std::string& method,
                                              const std::string& path,
                                              const std::string& timestamp,
                                              const std::string& nonce,
                                              const std::string& body) {
    // ★ 契约 §2（已按云端 client_sign.go 的 clientSigningString 实测校准，M2.07）：
    //   canonical 末尾**无换行**；多拼换行 ⇒ 云端 HMAC 校验失败 401。
    const std::string canonical =
        build_canonical(method, path, timestamp, nonce, body);

    // ★ client_secret 为空 ⇒ 拒绝签名并报错（禁止"空 secret 照签"，见 §3.3）。
    if (client_secret_.empty()) {
        TTBOX_LOG_ERROR("client_secret 未配置，拒绝签名");
        return {};
    }
#if defined(__linux__) || defined(__APPLE__)
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    HMAC(EVP_sha256(), client_secret_.data(),
         static_cast<int>(client_secret_.size()),
         reinterpret_cast<const unsigned char*>(canonical.data()),
         canonical.size(), result, &result_len);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(result_len * 2);
    for (unsigned int i = 0; i < result_len; ++i) {
        out += hex[(result[i] >> 4) & 0x0F];
        out += hex[result[i] & 0x0F];
    }
    return out;
#elif defined(_WIN32)
    // Windows: use BCrypt for HMAC-SHA256
    // ★ 2026-09-26：BCryptOpenAlgorithmProvider 必须带 BCRYPT_ALG_HANDLE_HMAC_FLAG，
    //   否则 BCryptCreateHash 传 pbSecret 恒返回 STATUS_INVALID_PARAMETER
    //   ⇒ Windows 构建签名 100% 静默失败（第三轮审计）。
    BCryptAlgorithmHandle hAlg = nullptr;
    NTSTATUS nt = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                               NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (nt != STATUS_SUCCESS || !hAlg) return {};
    BCryptHashHandle hHash = nullptr;
    nt = BCryptCreateHash(hAlg, &hHash, NULL, 0,
                          reinterpret_cast<UCHAR*>(const_cast<char*>(client_secret_.data())),
                          static_cast<ULONG>(client_secret_.size()), 0);
    if (nt != STATUS_SUCCESS || !hHash) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    nt = BCryptHashData(hHash,
                        reinterpret_cast<UCHAR*>(const_cast<char*>(canonical.data())),
                        static_cast<ULONG>(canonical.size()), 0);
    if (nt != STATUS_SUCCESS) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    ULONG dwHashLen = 0;
    BCryptFinishHash(hHash, NULL, 0, &dwHashLen, 0);
    std::vector<UCHAR> hashBuf(dwHashLen);
    nt = BCryptFinishHash(hHash, hashBuf.data(), dwHashLen, &dwHashLen, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (nt != STATUS_SUCCESS) return {};
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(dwHashLen * 2);
    for (ULONG i = 0; i < dwHashLen; ++i) {
        out += hex[(hashBuf[i] >> 4) & 0x0F];
        out += hex[hashBuf[i] & 0x0F];
    }
    return out;
#else
    (void)method; (void)path; (void)timestamp; (void)nonce; (void)body;
    return {};
#endif
}

// ---- 统一签名传输 ----

// 统一签名传输：解析端点 → 签名 → 拼头 → 按 scheme 走 TLS/明文 → 解析 HTTP 响应。
bool TtboxLicenseClient::signed_exchange(
    const std::string& method,
    const std::string& path,
    const std::string& body_json,
    const std::vector<std::pair<std::string, std::string>>& extra_headers,
    HttpReply& out,
    std::string* err) {
    ServerEndpoint ep;
    std::string ep_err;
    if (!parse_server_endpoint(server_url_, ep, &ep_err)) {
        if (err) *err = ep_err;
        out.error = ep_err;
        return false;
    }
    const std::string ts = unix_timestamp_sec();
    const std::string nonce = generate_nonce();
    // ★ canonical 签**不含 base_path** 的 path（对齐 Python cloud_client 契约）
    const std::string sig = sign_request(method, path, ts, nonce, body_json);
    if (sig.empty()) {
        if (err) *err = "client_secret 未配置（拒签，不发请求）";
        out.error = "unsigned";
        return false;
    }
    std::vector<std::pair<std::string, std::string>> headers;
    headers.push_back({"X-App-Key", app_key_});
    headers.push_back({"X-Timestamp", ts});
    headers.push_back({"X-Nonce", nonce});
    headers.push_back({"X-Signature", sig});
    for (const auto& kv : extra_headers) headers.push_back(kv);

    const bool with_body = (method == "POST");
    const std::string full_path = ep.base_path + path;   // ★ base 路径保留
    const std::string reqs = build_request(method, full_path, ep.host, body_json,
                                            headers, with_body);
    std::string total;
    const bool ok = (ep.scheme == "https")
                        ? send_recv_tls(ep.host, ep.port, reqs, total, &out.error)
                        : send_recv_plain(ep.host, ep.port, reqs, total, &out.error);
    if (!ok) {
        if (err && out.error.empty()) *err = "transport failed";
        return false;
    }
    if (!parse_http_reply(total, &out.status, &out.body, &out.error)) {
        if (err) *err = out.error;
        return false;
    }
    return true;
}

// GET 请求；非 200 或传输失败返回空串。
std::string TtboxLicenseClient::api_get(const std::string& path) {
    HttpReply rep;
    std::string err;
    if (!signed_exchange("GET", path, "", {}, rep, &err)) {
        TTBOX_LOG_WARN("[LicenseClient] GET " + path + " 失败: " +
                       (rep.error.empty() ? err : rep.error));
        return {};
    }
    if (rep.status != 200) {
        TTBOX_LOG_WARN("[LicenseClient] GET " + path + " HTTP " + std::to_string(rep.status));
        return {};
    }
    return rep.body;
}

// POST 请求；非 200 或传输失败返回空串。
std::string TtboxLicenseClient::api_post(const std::string& path,
                                         const std::string& body_json) {
    HttpReply rep;
    std::string err;
    if (!signed_exchange("POST", path, body_json, {}, rep, &err)) {
        TTBOX_LOG_WARN("[LicenseClient] POST " + path + " 失败: " +
                       (rep.error.empty() ? err : rep.error));
        return {};
    }
    if (rep.status != 200) {
        TTBOX_LOG_WARN("[LicenseClient] POST " + path + " HTTP " + std::to_string(rep.status));
        return {};
    }
    return rep.body;
}

// 拉取 app-info，返回 (heartbeatInterval, heartbeatTimeout)；失败回落默认 60/180。
std::pair<int, int> TtboxLicenseClient::fetch_app_info() {
    int hb_interval = kHeartbeatIntervalSecDefault;
    int hb_timeout = kHeartbeatTimeoutSecDefault;
    // ★ 2026-09-26：查询参数是 **appKey**（camelCase，cloud_client.py:270 同源）。
    //   旧实现发 app_key= ⇒ 云端忽略 ⇒ 永远回落默认 60/180，且失败零日志。
    std::string body = api_get("/api/client/app-info?appKey=" + app_key_);
    if (body.empty()) {
        TTBOX_LOG_WARN("[LicenseClient] app-info 拉取失败，心跳参数回落默认 60/180");
        return {hb_interval, hb_timeout};
    }
    JsonParseResult pr = json_parse(body);
    if (!pr.ok) return {hb_interval, hb_timeout};
    const auto* data = pr.value.find("data");
    if (!data || !data->is_array() || data->as_array().empty())
        return {hb_interval, hb_timeout};
    const auto& first = data->as_array()[0];
    const auto* hi = first.find("heartbeatInterval");
    const auto* ht = first.find("heartbeatTimeout");
    if (hi) hb_interval = static_cast<int>(hi->as_number(kHeartbeatIntervalSecDefault));
    if (ht) hb_timeout = static_cast<int>(ht->as_number(kHeartbeatTimeoutSecDefault));
    return {hb_interval, hb_timeout};
}

// 卡密登录：POST card-login，HTTP 状态分层（403 到期/禁用⇒kExpired、401⇒kInvalidCard、
// 其余非 200⇒kNetworkError）+ 业务 ok=false⇒kInvalidCard；成功置 kValid。
bool TtboxLicenseClient::do_card_login(
    const std::string& card_key, const std::string& bind_device,
    LicenseStatus& out, std::string* err) {
    std::string body_json =
        "{\"appKey\":\"" + app_key_ +
        "\",\"cardKey\":\"" + card_key +
        "\",\"machineCode\":\"" + bind_device + "\"}";
    HttpReply rep;
    std::string xerr;
    if (!signed_exchange("POST", "/api/client/card-login", body_json, {}, rep, &xerr)) {
        if (err) *err = "card-login: " + (rep.error.empty() ? xerr : rep.error);
        out.state = LicenseState::kNetworkError;
        return false;
    }
    // ★ 2026-09-26：HTTP 状态分层（对齐 do_heartbeat / Python cloud_client 契约，第三轮审计）。
    //   旧实现非 200 一律归 kNetworkError ⇒ 云端权威否定（403=到期/禁用）被当成
    //   "断网" ⇒ 状态机走 kFallback 放行，授权执法被绕过。
    if (rep.status == 403) {
        // 权威否定：到期/禁用 ⇒ fail-closed 锁定（请求成功送达，但卡被拒）
        if (err) *err = "card-login: 403 到期/禁用（云端权威否定）";
        out.state = LicenseState::kExpired;
        out.last_error = "云端到期或已禁用";
        return true;
    }
    if (rep.status == 401) {
        // 未授权：卡无效/签名不被接受 ⇒ 权威否定，fail-closed
        if (err) *err = "card-login: 401 卡无效/未授权";
        out.state = LicenseState::kInvalidCard;
        out.last_error = "card unauthorized";
        return true;
    }
    if (rep.status != 200) {
        if (err) *err = "card-login: HTTP " + std::to_string(rep.status);
        out.state = LicenseState::kNetworkError;
        return false;
    }
    JsonParseResult pr = json_parse(rep.body);
    if (!pr.ok) {
        if (err) *err = std::string("card-login: bad json: ") + pr.error;
        return false;
    }
    const auto* ok = pr.value.find("ok");
    if (!ok || !ok->as_bool()) {
        const auto* msg = pr.value.find("message");
        std::string msg_str = msg ? msg->as_string() : "unknown error";
        if (err) *err = "card-login: " + msg_str;
        out.state = LicenseState::kInvalidCard;
        out.last_error = msg_str;
        return true;  // request succeeded but card invalid
    }
    const auto& data = pr.value;
    const auto* token = data.find("clientToken");
    if (token) out.cached_token = token->as_string();
    const auto* expire = data.find("expireAt");
    // ★ 2026-09-26 fail-closed：card-login 的**成功响应必须携带 expireAt**
    //   （契约同 Python cloud_client.card_login：expire_at / expire_unix_s 恒在）。
    //   旧实现"字段整个缺失/空串 ⇒ 保持 0=永久"，桥端回归/代理剥字段时
    //   订阅卡会被静默判成永久卡（断网后永不退出 fail-open）。
    //   另：云端语义 expire 必须 > 0（LicenseDaemon::activate_cloud 明确拒绝 ≤0），
    //   与离线卡的"0=永久"是两套语义，这里按云端契约 fail-closed。
    if (expire == nullptr) {
        if (err) *err = "card-login: 成功响应缺 expireAt";
        out.state = LicenseState::kInvalidCard;
        out.last_error = "expireAt missing";
        return true;
    }
    if (expire->is_number()) {
        out.expire_unix_ms = expire->as_int(0);
    } else {
        const std::string exp_str = expire->as_string("");
        if (exp_str.empty()) {
            if (err) *err = "card-login: expireAt 为空";
            out.state = LicenseState::kInvalidCard;
            out.last_error = "expireAt empty";
            return true;
        }
        int64_t v = 0;
        try {
            v = std::stoll(exp_str);
        } catch (...) {
            if (err) *err = "card-login: expireAt 不是合法整数: " + exp_str;
            out.state = LicenseState::kInvalidCard;
            out.last_error = "expireAt 无法解析为整数";
            return true;
        }
        out.expire_unix_ms = v;
    }
    if (out.expire_unix_ms <= 0) {
        if (err) *err = "card-login: expireAt 非法（云端契约要求正数到期点）";
        out.state = LicenseState::kInvalidCard;
        out.last_error = "expireAt non-positive";
        return true;
    }
    // M2：签名卡内容（features / plan / uiBrand）。此处不回落也不过滤 ——
    // 回落由 apply_check_result、闭集过滤由 to_snapshot 各自负责（单一职责）。
    parse_card_content(data, out);
    out.state = LicenseState::kValid;
    out.verified_at_ms = now_unix_ms();
    auto [hi, ht] = fetch_app_info();
    out.heartbeat_interval = hi;
    out.heartbeat_timeout = ht;
    out.next_check_ms = out.verified_at_ms + static_cast<int64_t>(hi) * 1000;
    return true;
}

// 心跳续期：POST heartbeat，HTTP 状态分层（403 到期/禁用、401 token 失效、其余网络错误）。
bool TtboxLicenseClient::do_heartbeat(
    const std::string& token, const std::string& bind_device,
    LicenseStatus& out, std::string* err) {
    std::string body_json =
        "{\"machineCode\":\"" + bind_device +
        "\",\"clientVersion\":\"1.0.0\"}";
    std::vector<std::pair<std::string, std::string>> extra;
    extra.push_back({"Authorization", "Bearer " + token});
    HttpReply rep;
    std::string xerr;
    if (!signed_exchange("POST", "/api/client/heartbeat", body_json, extra, rep, &xerr)) {
        // client_secret 未配置 / 传输失败 ⇒ 网络类失败（fail-open 由状态机裁决）
        if (err) *err = "heartbeat: " + (rep.error.empty() ? xerr : rep.error);
        out.state = LicenseState::kNetworkError;
        return false;
    }
    // ★ 2026-09-26：HTTP 状态分层（对齐 Python heartbeat_worker 契约，第三轮审计）。
    //   旧实现非 200 一律归 kNetworkError ⇒ 云端权威否定（403=到期/禁用）被当成
    //   "断网" ⇒ 状态机走 kFallback 永续放行，授权执法被绕过。
    if (rep.status == 403) {
        // 权威否定：到期/禁用 ⇒ fail-closed 锁定（不回落 card-login 也不宽限）
        if (err) *err = "heartbeat: 403 到期/禁用（云端权威否定）";
        out.state = LicenseState::kExpired;
        out.last_error = "云端到期或已禁用";
        return true;
    }
    if (rep.status == 401) {
        // token 失效 ⇒ 属"需要重新登录"，verify_once 会自然回落 card-login
        if (err) *err = "heartbeat: 401 token 失效";
        out.state = LicenseState::kInvalidCard;
        out.last_error = "token expired";
        return true;
    }
    if (rep.status != 200) {
        if (err) *err = "heartbeat: HTTP " + std::to_string(rep.status);
        out.state = LicenseState::kNetworkError;
        return false;
    }
    JsonParseResult pr = json_parse(rep.body);
    if (!pr.ok) {
        if (err) *err = std::string("heartbeat: bad json: ") + pr.error;
        out.state = LicenseState::kInvalidCard;
        return true;
    }
    const auto* ok = pr.value.find("ok");
    if (!ok || !ok->as_bool()) {
        const auto* msg = pr.value.find("message");
        std::string msg_str = msg ? msg->as_string() : "heartbeat failed";
        if (err) *err = "heartbeat: " + msg_str;
        out.state = LicenseState::kExpired;
        out.last_error = msg_str;
        return true;
    }
    // Heartbeat success: update token and expiry
    const auto* t = pr.value.find("clientToken");
    if (t) out.cached_token = t->as_string();
    const auto* exp = pr.value.find("expireAt");
    if (exp) {
        // ★ 契约上心跳成功响应**不携带** expireAt（cloud_client.heartbeat 只回
        //   server_time / heartbeat_interval / heartbeat_timeout）—— 靠 verify_once
        //   保留 card-login 写入的 expire（那里曾把整份状态清零 ⇒ 永久卡假象）。
        //   这里防御式保留：带了就更新，解析失败照旧按无效处理（不静默落 0）。
        const std::string exp_str = exp->as_string("");
        if (exp->is_number()) {
            out.expire_unix_ms = exp->as_int(0);
        } else if (!exp_str.empty()) {
            int64_t v = 0;
            try {
                v = std::stoll(exp_str);
            } catch (...) {
                if (err) *err = "heartbeat: expireAt 不是合法整数: " + exp_str;
                out.state = LicenseState::kInvalidCard;
                out.last_error = "expireAt 无法解析为整数";
                return true;
            }
            out.expire_unix_ms = v;
        }
    }
    const auto* hi = pr.value.find("heartbeatInterval");
    if (hi) out.heartbeat_interval = static_cast<int>(hi->as_number(60));
    // M2：续期响应同样携带签名卡内容（服务端契约要求，见 parse_card_content 说明）。
    parse_card_content(pr.value, out);
    out.state = LicenseState::kValid;
    out.verified_at_ms = now_unix_ms();
    out.next_check_ms = out.verified_at_ms +
                        static_cast<int64_t>(out.heartbeat_interval) * 1000;
    return true;
}

// ILicenseClient 实现：有缓存 token 先心跳，失败/无 token 回落 card-login；返回请求是否成功。
bool TtboxLicenseClient::verify_once(
    const std::string& card, const std::string& bind_device,
    LicenseStatus& out_status, std::string* err_message) {
    // Preserve previous cached token from caller
    std::string prev_token = out_status.cached_token;
    // ★ 2026-09-26：expire_unix_ms 必须跨调用保留 —— 心跳成功响应**不带** expireAt
    //   （契约），旧实现在这里整份清零后心跳不回填 ⇒ 每轮 verify 都把到期点抹成
    //   0=永久，订阅卡静默变永久卡（第三轮审计）。
    const int64_t prev_expire = out_status.expire_unix_ms;
    out_status = LicenseStatus{};
    out_status.expire_unix_ms = prev_expire;
    if (client_secret_.empty()) {   // §3.3：空 secret 拒签（网络类失败 ⇒ fail-open）
        out_status.state = LicenseState::kNetworkError;
        if (err_message) *err_message = "client_secret 未配置（拒绝签名）";
        return false;
    }
    if (card.empty()) {
        out_status.state = LicenseState::kInvalidCard;
        out_status.last_error = "card empty";
        if (err_message) *err_message = out_status.last_error;
        return false;
    }
    // If we have a valid cached token and card is a license key, try heartbeat first
    if (!prev_token.empty() && card.find("LS-") == 0) {
        auto [hi, ht] = fetch_app_info();
        out_status.heartbeat_interval = hi;
        out_status.heartbeat_timeout = ht;
        bool ok = do_heartbeat(prev_token, bind_device, out_status, err_message);
        if (ok && out_status.state == LicenseState::kValid) return true;
        // Heartbeat failed (token expired / network error) → fall back to card-login
    }
    // First login: card-login
    return do_card_login(card, bind_device, out_status, err_message);
}

// 覆盖服务端地址（空串忽略）。
void TtboxLicenseClient::override_server(const std::string& server_url) {
    if (!server_url.empty()) server_url_ = server_url;
}

// 覆盖 app_key / client_secret（空串忽略）。
void TtboxLicenseClient::override_credentials(const std::string& app_key,
                                              const std::string& client_secret) {
    if (!app_key.empty()) app_key_ = app_key;
    if (!client_secret.empty()) client_secret_ = client_secret;
}

}  // namespace ttbox::core::auth
