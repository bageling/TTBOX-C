// IpcServer.cpp — IPC 服务端/客户端实现（Unix AF_UNIX，Windows TCP loopback）
#include "ipc/IpcServer.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

#include "common/Logger.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ttbox::core {

namespace {

// IPC 链路诊断开关（默认关闭）。设 TTBOX_IPC_DEBUG=1 后，服务端把「监听/接受/拒绝/
// 读到多少字节/是否回了包」逐条打到 stderr。用于定位「客户端连接成功却读不到响应」
// 这类跨进程偶发问题（ctest 并发下曾复现）。生产默认关闭，零开销。
bool ipc_debug_enabled() {
    static const bool on = []() {
        const char* v = std::getenv("TTBOX_IPC_DEBUG");
        return v != nullptr && *v != '\0' && std::strcmp(v, "0") != 0;
    }();
    return on;
}

#define IPCDBG(...)                              \
    do {                                         \
        if (ipc_debug_enabled()) {               \
            std::fprintf(stderr, __VA_ARGS__);   \
            std::fflush(stderr);                 \
        }                                        \
    } while (0)

// 最近一次 socket 调用的错误码：Windows = WSAGetLastError()，Unix = errno。
int sock_last_error() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

// 设置接收超时（毫秒）。**平台语义不同，这里踩过坑（2026-09-23 实测确认）**：
//   - Windows/Winsock：SO_RCVTIMEO 取 `DWORD`（毫秒）。若按 Unix 那样传 `struct timeval`，
//     内核会把前 4 字节当 DWORD 读 —— 而 timeval 的前 4 字节正是 `tv_sec`，
//     于是 timeout_ms=3000 实际只生效 3ms（实测：请求 2000ms 时 13ms 即报超时）。
//     附带效应：不足 1s 的超时 tv_sec=0，而 Winsock 里 0 表示无限等待。
//     症状是客户端「连接成功、请求发出、却在几毫秒内报读取响应失败」。
//   - Unix：SO_RCVTIMEO 取 `struct timeval`。
// 返回 false 表示设置失败（旧代码丢弃返回值，导致上面这个错静默存在）。
bool set_recv_timeout_ms(int fd, int timeout_ms) {
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(timeout_ms);
    return ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
#else
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

// 设置发送超时（毫秒）。★ 2026-09-26（第四轮审计）：此前只有 SO_RCVTIMEO ——
//   客户端连上、发一条请求后不再 read，大响应（GET_PREVIEW 数百 KB base64）会
//   填满 TCP 发送缓冲，sock_send 永久阻塞 ⇒ 连接线程永不退出、槽位耗尽、
//   stop() 排空无限期挂起（关机挂死）。SO_SNDTIMEO 平台语义与 SO_RCVTIMEO 相同。
bool set_send_timeout_ms(int fd, int timeout_ms) {
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(timeout_ms);
    return ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
#else
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

#if defined(_WIN32)
// MSVC 无 ssize_t：Windows 分支统一用 long long 语义的别名。
using ssize_t = long long;
// Windows：路径 "tcp:<port>"。返回监听 fd（SOCKET 转 int），失败 -1。
// bound_port 出参：port==0（临时端口）时回写 OS 实际分配的端口
int listen_tcp(const std::string& path, std::string* error, int* bound_port = nullptr) {
    static bool ws_inited = []() {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)ws_inited;

    std::string port_str = path;
    const std::string prefix = "tcp:";
    if (port_str.rfind(prefix, 0) == 0) port_str = port_str.substr(prefix.size());
    int port = 0;
    try {
        port = std::stoi(port_str);
    } catch (...) {
        if (error) *error = "非法 TCP 端口: " + path;
        return -1;
    }

    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        if (error) *error = "socket() 失败";
        return -1;
    }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "bind() 失败 (端口 " + port_str + " 可能被占用)";
        ::closesocket(fd);
        return -1;
    }
    if (port == 0 && bound_port != nullptr) {
        // 临时端口：读回 OS 实际分配值（测试专用路径，生产用固定端口不受影响）
        sockaddr_in actual{};
        int len = static_cast<int>(sizeof(actual));
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) != 0) {
            if (error) *error = "getsockname() 失败";
            ::closesocket(fd);
            return -1;
        }
        *bound_port = static_cast<int>(ntohs(actual.sin_port));
    }
    if (::listen(fd, 8) != 0) {
        if (error) *error = "listen() 失败";
        ::closesocket(fd);
        return -1;
    }
    return static_cast<int>(fd);
}

int connect_tcp(const std::string& path, std::string* error) {
    std::string port_str = path;
    const std::string prefix = "tcp:";
    if (port_str.rfind(prefix, 0) == 0) port_str = port_str.substr(prefix.size());
    int port = 0;
    try {
        port = std::stoi(port_str);
    } catch (...) {
        if (error) *error = "非法 TCP 端口: " + path;
        return -1;
    }
    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        if (error) *error = "socket() 失败";
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int wsa_error = ::WSAGetLastError();
        if (error) {
            *error = "connect() 失败 (port=" + port_str + ", WSA=" +
                     std::to_string(wsa_error) + ")";
        }
        ::closesocket(fd);
        return -1;
    }
    return static_cast<int>(fd);
}

ssize_t sock_send(int fd, const void* buf, size_t len) {
    return static_cast<ssize_t>(::send(static_cast<SOCKET>(fd),
                                       reinterpret_cast<const char*>(buf),
                                       static_cast<int>(len), 0));
}

ssize_t sock_recv(int fd, void* buf, size_t len) {
    return static_cast<ssize_t>(::recv(static_cast<SOCKET>(fd),
                                       reinterpret_cast<char*>(buf),
                                       static_cast<int>(len), 0));
}

void sock_close(int fd) { ::closesocket(static_cast<SOCKET>(fd)); }

#else  // !_WIN32 (Unix)

int listen_unix(const std::string& path, std::string* error) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = "socket() 失败: " + std::string(std::strerror(errno));
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        if (error) *error = "socket 路径过长: " + path;
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    // 清理残留 socket 文件（若存在且可写）
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "bind() 失败 (" + path + "): " + std::string(std::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 8) != 0) {
        if (error) *error = "listen() 失败: " + std::string(std::strerror(errno));
        ::close(fd);
        ::unlink(path.c_str());
        return -1;
    }
    // 权限收紧（T02 方案 A）：socket 位于 /run/ttbox（RuntimeDirectory，属 ttbox:ttbox），
    // 0660 = 属主与同组（ttbox）可读写。此前的 0666 是配合 /tmp 全局可写的旧设计：
    // /tmp 为 1777，任意本地用户可抢先创建同名 socket 劫持控制通道（安全缺陷）。
    // Web / 预览进程以同组 ttbox 运行（systemd User=ttbox Group=ttbox），连接不受影响。
    ::chmod(path.c_str(), 0660);
    return fd;
}

int connect_unix(const std::string& path, std::string* error) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = "socket() 失败";
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        if (error) *error = "socket 路径过长: " + path;
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "connect() 失败 (" + path + "): " + std::string(std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

ssize_t sock_send(int fd, const void* buf, size_t len) {
    // 客户端在 Preview 大响应中途关闭连接时，普通 send 会触发 SIGPIPE，
    // Linux 默认行为是杀死整个 Core。MSG_NOSIGNAL 把它变成可处理的 EPIPE。
    return ::send(fd, buf, len, MSG_NOSIGNAL);
}

ssize_t sock_recv(int fd, void* buf, size_t len) {
    return ::recv(fd, buf, len, 0);
}

void sock_close(int fd) { ::close(fd); }

#endif  // _WIN32

std::string read_line(int fd, bool* ok, size_t max_line = 65536) {
    std::string buf;
    char tmp[4096];
    *ok = true;
    while (true) {
        ssize_t n = sock_recv(fd, tmp, sizeof(tmp));
        if (n <= 0) {
            IPCDBG("[IPC-LINK] fd=%d recv n=%lld sockerr=%d (已读 %zu 字节)\n", fd,
                   static_cast<long long>(n), sock_last_error(), buf.size());
            *ok = false;
            break;
        }
        buf.append(tmp, static_cast<size_t>(n));
        auto pos = buf.find('\n');
        if (pos != std::string::npos) {
            buf.resize(pos);
            break;
        }
        if (buf.size() > max_line) {
            *ok = false;  // 超长行，防滥用
            break;
        }
    }
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// IpcResponse
// ---------------------------------------------------------------------------

std::string IpcResponse::to_json() const {
    JsonValue resp = JsonValue::object();
    resp.set("id", JsonValue::string(id));
    resp.set("type", JsonValue::string(type));
    resp.set("status", JsonValue::number(static_cast<double>(static_cast<int>(status))));
    resp.set("data", data);
    if (!error.empty()) {
        resp.set("error", JsonValue::string(error));
    }
    return resp.dump() + "\n";
}

// ---------------------------------------------------------------------------
// IpcServer
// ---------------------------------------------------------------------------

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start(const std::string& socket_path, std::string* error) {
    if (running_.load()) {
        if (error) *error = "IpcServer 已在运行";
        return false;
    }
    int fd = -1;
    std::string effective_path = socket_path;
#if defined(_WIN32)
    int bound_port = 0;
    fd = listen_tcp(socket_path, error, &bound_port);
    // 临时端口（"tcp:0"）：回写 OS 实际分配的端口，后续自举握手与客户端
    // 都通过 socket_path() 拿到唯一端口——彻底消除多测试用例共用固定端口
    // 导致的 TIME_WAIT/bind 竞态。
    if (fd >= 0 && bound_port > 0) {
        effective_path = "tcp:" + std::to_string(bound_port);
    }
#else
    fd = listen_unix(socket_path, error);
#endif
    if (fd < 0) return false;

    socket_path_ = effective_path;
    listen_fd_ = fd;
    running_.store(true);
    IPCDBG("[IPC-SRV] LISTEN path=%s fd=%d\n", socket_path_.c_str(), listen_fd_);
    accept_thread_ = std::thread(&IpcServer::accept_loop, this);

    // 自举握手：Windows 下连接刚 listen 的 socket 偶发 WSAECONNREFUSED（accept 尚未就绪），
    // 这里内部自连一次 PING，确保 start() 返回后任何客户端首次连接都能成功。
    // 失败重试 5 次 × 40ms；全部失败则回滚启动（fail-fast，不带病运行）。
    bool ready = false;
    std::string probe_error;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::string pong;
        if (ipc_request(socket_path_, R"({"type":"PING"})", pong, 500, &probe_error)) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    if (!ready) {
        // 统一走 stop()：它会先关闭监听唤醒 accept，再排空所有在途连接。
        // 旧代码先 join 阻塞在 accept 的线程、后关闭监听，在自举失败路径会死锁。
        stop();
        if (error) *error = "IPC 自举握手失败: " + probe_error;
        return false;
    }

    TTBOX_LOG_INFO("IPC 服务已启动: " + socket_path);
    return true;
}

void IpcServer::stop() {
    if (!running_.exchange(false)) return;

#if defined(_WIN32)
    if (listen_fd_ >= 0) {
        // Windows 下 shutdown() 不保证唤醒阻塞 accept；先关闭监听 socket，
        // 让 accept 返回 INVALID_SOCKET，再回收 accept 线程。
        ::shutdown(static_cast<SOCKET>(listen_fd_), SD_BOTH);
        ::closesocket(static_cast<SOCKET>(listen_fd_));
        listen_fd_ = -1;
    }
#else
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);  // 使阻塞的 accept 立即返回
    }
#endif
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (listen_fd_ >= 0) {
        sock_close(listen_fd_);
        listen_fd_ = -1;
    }

    // ---- 排空在途连接线程（防 use-after-free，见 hpp conn_fds_ 注释）----
    // 持锁 shutdown 所有 fd，唤醒卡在 read_line/sock_send 的线程；随后通过
    // 条件变量等到列表真正清空。没有"超时后继续析构"的逃生口（连接线程仍会
    // 访问 this）—— 但 SO_SNDTIMEO/SO_RCVTIMEO 保证卡住的线程最迟 5s 自行退出，
    // 所以这里的等待有界。仍保留周期性日志：万一真挂住，至少看得见卡在排空。
    {
        std::unique_lock<std::mutex> lk(conn_fds_mutex_);
        for (int fd : conn_fds_) {
#if defined(_WIN32)
            ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
            ::shutdown(fd, SHUT_RDWR);
#endif
        }
        int waited_s = 0;
        while (!conn_fds_.empty()) {
            if (conn_fds_cv_.wait_for(lk, std::chrono::seconds(10)) ==
                std::cv_status::timeout) {
                waited_s += 10;
                TTBOX_LOG_WARN("IPC 排空等待中: 仍有 " +
                               std::to_string(conn_fds_.size()) + " 个连接未退出 (" +
                               std::to_string(waited_s) + "s)");
                for (int fd : conn_fds_) {
#if defined(_WIN32)
                    ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
                    ::shutdown(fd, SHUT_RDWR);
#endif
                }
            }
        }
    }
    active_connections_.store(0, std::memory_order_release);

#if !defined(_WIN32)
    ::unlink(socket_path_.c_str());
#endif
    TTBOX_LOG_INFO("IPC 服务已停止: " + socket_path_);
}

void IpcServer::accept_loop() {
    while (running_.load()) {
        // 连接数满了就拒绝
        if (active_connections_.load(std::memory_order_acquire) >= kMaxConnections) {
            int reject_fd = -1;
#if defined(_WIN32)
            SOCKET rc = ::accept(static_cast<SOCKET>(listen_fd_), nullptr, nullptr);
            reject_fd = rc == INVALID_SOCKET ? -1 : static_cast<int>(rc);
#else
            reject_fd = ::accept(listen_fd_, nullptr, nullptr);
#endif
            if (reject_fd >= 0) sock_close(reject_fd);
            IPCDBG("[IPC-SRV] REJECT 连接数已满 active=%d\n",
                   active_connections_.load(std::memory_order_acquire));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        int client_fd = -1;
#if defined(_WIN32)
        SOCKET c = ::accept(static_cast<SOCKET>(listen_fd_), nullptr, nullptr);
        client_fd = c == INVALID_SOCKET ? -1 : static_cast<int>(c);
#else
        client_fd = ::accept(listen_fd_, nullptr, nullptr);
#endif
        if (client_fd < 0) {
            if (running_.load()) {
                TTBOX_LOG_WARN("accept() 失败（服务停止中则忽略）");
                IPCDBG("[IPC-SRV] ACCEPT-FAIL listen_fd=%d\n", listen_fd_);
            }
            continue;
        }
        IPCDBG("[IPC-SRV] ACCEPT fd=%d active=%d listen=%d\n", client_fd,
               active_connections_.load(std::memory_order_acquire) + 1, listen_fd_);
        active_connections_.fetch_add(1, std::memory_order_acq_rel);
        {
            // 登记在途 fd：stop() 靠它 shutdown 唤醒卡住的连接线程（见 stop 注释）
            std::lock_guard<std::mutex> lk(conn_fds_mutex_);
            conn_fds_.push_back(client_fd);
        }
        std::thread([this, client_fd] {
            // 线程函数必须吞掉异常：handler 抛出的异常若逃出线程体，会直接
            // std::terminate 整个 Core；而且下面「注销 fd + 递减计数」也不会执行，
            // 该连接槽位永久泄漏（累计到 kMaxConnections 后所有新连接被立即关闭，
            // 表现为客户端「连接成功却读不到响应」）。
            try {
                handle_connection(client_fd);
            } catch (const std::exception& e) {
                TTBOX_LOG_ERROR(std::string("IPC 连接处理抛异常: ") + e.what());
            } catch (...) {
                TTBOX_LOG_ERROR("IPC 连接处理抛未知异常");
            }
            {
                // 锁内完成所有对 IpcServer 成员的最后访问，再把 fd 从列表移除。
                // stop() 以“列表为空”为线程已不再访问 this 的完成条件。
                std::lock_guard<std::mutex> lk(conn_fds_mutex_);
                IPCDBG("[IPC-SRV] fd=%d 关闭(收尾)\n", client_fd);
                sock_close(client_fd);
                active_connections_.fetch_sub(1, std::memory_order_acq_rel);
                conn_fds_.erase(std::remove(conn_fds_.begin(), conn_fds_.end(), client_fd),
                                conn_fds_.end());
                conn_fds_cv_.notify_all();
            }
        }).detach();
    }
}

// 服务端接收超时（毫秒）。用途只有一个：防「连上却不发数据」的半开连接占住连接槽。
// 正常请求毫秒级到达，5s 对合法客户端足够宽松。
// 超时后 recv 返回 <= 0 ⇒ read_line 置 ok=false ⇒ 连接线程走「无响应」分支收尾、
// 关 fd 并释放槽位（槽位归还见 accept_loop 里线程收尾段）。
// ★ 客户端侧的超时在 ipc_request 里设；两侧共用 set_recv_timeout_ms（平台语义差异见其注释）。
constexpr int kServerRecvTimeoutMs = 5000;
// ★ 发送超时（第四轮审计）：防"客户端收响应"卡死连接线程（见 set_send_timeout_ms 注释）。
constexpr int kServerSendTimeoutMs = 5000;

void IpcServer::handle_connection(int fd) {
    // ★ 2026-09-23 复核发现：accept 出来的 fd 此前没有任何超时，对端连上不发数据就会永久
    //   卡在 read_line 的 recv 上、槽位不释放 —— 累计 kMaxConnections(32) 个半开连接后，
    //   所有新连接被立刻关闭，表现为客户端「连接成功却读不到响应」，IPC 等同不可用。
    if (!set_recv_timeout_ms(fd, kServerRecvTimeoutMs)) {
        IPCDBG("[IPC-SRV] fd=%d 设置接收超时失败 sockerr=%d\n", fd, sock_last_error());
    }
    if (!set_send_timeout_ms(fd, kServerSendTimeoutMs)) {
        IPCDBG("[IPC-SRV] fd=%d 设置发送超时失败 sockerr=%d\n", fd, sock_last_error());
    }
    bool ok = false;
    std::string request_text = read_line(fd, &ok);
    std::string response_text;
    IPCDBG("[IPC-SRV] fd=%d read ok=%d bytes=%zu text=%.80s\n", fd, ok ? 1 : 0,
           request_text.size(), request_text.c_str());
    if (ok && !request_text.empty()) {
        JsonParseResult parsed = json_parse(request_text);
        IpcResponse resp;
        if (!parsed.ok) {
            resp.status = IpcError::kBadRequest;
            resp.type = "";
            resp.error = "invalid JSON request: " + parsed.error;
        } else if (!parsed.value.is_object()) {
            resp.status = IpcError::kBadRequest;
            resp.error = "请求必须是 JSON 对象";
        } else {
            resp = handle_request(parsed.value);
        }
        response_text = resp.to_json();
    } else if (!ok && !request_text.empty() && request_text.size() > 65536) {
        // ★ 超长请求也必须回一个错误包：旧实现静默断开，客户端只能干等超时
        //   且错误与真实原因（请求超长）不符。
        IpcResponse resp;
        resp.status = IpcError::kBadRequest;
        resp.error = "请求超过 64KB 上限";
        response_text = resp.to_json();
    } else {
        // 空/异常连接：无需响应。这条是「客户端连接成功却读不到响应」的关键分支：
        // ok=0 表示对端在读请求前就关了连接（或 recv 出错），empty=1 表示只收到空行。
        IPCDBG("[IPC-SRV] fd=%d NO-RESPONSE ok=%d empty=%d\n", fd, ok ? 1 : 0,
               request_text.empty() ? 1 : 0);
    }
    if (!response_text.empty()) {
        // send() 允许短写，尤其是 Preview 的大 base64 JSON。必须循环到完整发送
        // 或明确遇到断连；禁止只发半截 JSON 后静默当成功。
        size_t sent = 0;
        while (sent < response_text.size()) {
            const ssize_t n = sock_send(fd, response_text.data() + sent,
                                        response_text.size() - sent);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        IPCDBG("[IPC-SRV] fd=%d sent %zu/%zu bytes\n", fd, sent, response_text.size());
    }
    // fd 由 accept_loop 的连接线程 lambda 在锁内统一注销并关闭（防止 stop()
    // 排空阶段 shutdown 到已关闭的 fd 号）——这里不再 close。
}

IpcResponse IpcServer::handle_request(const JsonValue& request) {
    IpcResponse resp;
    const JsonValue* id_v = request.find("id");
    resp.id = id_v ? id_v->as_string() : "";

    const JsonValue* type_v = request.find("type");
    if (type_v == nullptr || !type_v->is_string()) {
        resp.status = IpcError::kBadRequest;
        resp.error = "缺少必需字段 'type'（必须是字符串）";
        return resp;
    }
    const std::string type = type_v->as_string();
    resp.type = type;

    if (type == "PING") {
        JsonValue data = JsonValue::object();
        data.set("pong", JsonValue::boolean(true));
        data.set("server", JsonValue::string("ttbox_core"));
        resp.status = IpcError::kOk;
        resp.data = std::move(data);
        return resp;
    }

    if (type == "GET_STATUS") {
        if (!status_provider_) {
            resp.status = IpcError::kInternal;
            resp.error = "状态提供器未注册";
            return resp;
        }
        resp.status = IpcError::kOk;
        resp.data = system_status_to_json(status_provider_());
        return resp;
    }

        if (type == "GET_PREVIEW") {
        if (!preview_provider_) {
            resp.status = IpcError::kInternal;
            resp.error = "preview provider not registered";
            return resp;
        }
        std::vector<uint8_t> jpeg;
        uint64_t pseq = 0;
        if (!preview_provider_(&jpeg, &pseq) || jpeg.empty()) {
            resp.status = IpcError::kNotFound;
            resp.error = "暂无预览帧";
            return resp;
        }
        // base64 编码进 JSON
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        b64.reserve(((jpeg.size() + 2) / 3) * 4);
        for (size_t i = 0; i < jpeg.size(); i += 3) {
            const uint32_t v = (static_cast<uint32_t>(jpeg[i]) << 16) |
                               (i + 1 < jpeg.size() ? static_cast<uint32_t>(jpeg[i+1]) << 8 : 0) |
                               (i + 2 < jpeg.size() ? static_cast<uint32_t>(jpeg[i+2]) : 0);
            b64 += table[(v >> 18) & 63];
            b64 += table[(v >> 12) & 63];
            b64 += (i + 1 < jpeg.size()) ? table[(v >> 6) & 63] : '=';
            b64 += (i + 2 < jpeg.size()) ? table[v & 63] : '=';
        }
        JsonValue pd = JsonValue::object();
        pd.set("jpeg_base64", JsonValue::string(b64));
        pd.set("bytes", JsonValue::number(static_cast<double>(jpeg.size())));
        pd.set("seq", JsonValue::number(static_cast<double>(pseq)));
        resp.status = IpcError::kOk;
        resp.data = std::move(pd);
        return resp;
    }

if (type == "GET_CONFIG") {
        if (!config_provider_) {
            resp.status = IpcError::kInternal;
            resp.error = "config provider not registered";
            return resp;
        }
        resp.status = IpcError::kOk;
        resp.data = config_provider_();
        return resp;
    }

    // ---- SET_CONFIG：原子更新运行时配置（解析→校验→update→落盘）----
    // 请求：{"type":"SET_CONFIG","params":{"profile":{...RuntimeProfile JSON...}}}
    // 任意一步失败都直接返回错误，当前运行配置与配置文件不被污染。
    if (type == "SET_CONFIG") {
        if (!config_update_) {
            resp.status = IpcError::kInternal;
            resp.error = "配置更新处理器未注册";
            return resp;
        }
        const JsonValue* params = request.find("params");
        const JsonValue* profile_v = params ? params->find("profile") : nullptr;
        if (!profile_v || !profile_v->is_object()) {
            resp.status = IpcError::kBadRequest;
            resp.error = "missing object field params.profile";
            return resp;
        }
        std::string handler_error;
        bool persisted = false;
        if (!config_update_(*profile_v, &handler_error, &persisted)) {
            resp.status = IpcError::kBadRequest;
            resp.error = handler_error.empty() ? "config update rejected" : handler_error;
            return resp;
        }
        JsonValue data = JsonValue::object();
        data.set("applied", JsonValue::boolean(true));
        data.set("persisted", JsonValue::boolean(persisted));
        resp.status = IpcError::kOk;
        resp.data = std::move(data);
        return resp;
    }

    // ---- RUNTIME_CONTROL：启动/停止/重启 AI 流水线 ----
    // 请求：{"type":"RUNTIME_CONTROL","params":{"action":"start|stop|restart"}}
    if (type == "RUNTIME_CONTROL") {
        if (!runtime_control_) {
            resp.status = IpcError::kInternal;
            resp.error = "运行时控制处理器未注册";
            return resp;
        }
        const JsonValue* params = request.find("params");
        const JsonValue* action_v = params ? params->find("action") : nullptr;
        const std::string action = action_v ? action_v->as_string() : "";
        if (action != "start" && action != "stop" && action != "restart") {
            resp.status = IpcError::kBadRequest;
            resp.error = "params.action must be start|stop|restart";
            return resp;
        }
        std::string handler_error;
        if (!runtime_control_(action, &handler_error)) {
            resp.status = IpcError::kInternal;
            resp.error = handler_error.empty() ? ("runtime " + action + " failed") : handler_error;
            return resp;
        }
        JsonValue data = JsonValue::object();
        data.set("action", JsonValue::string(action));
        resp.status = IpcError::kOk;
        resp.data = std::move(data);
        return resp;
    }

    // ---- 模型管理（v0.3）：LIST / IMPORT / VALIDATE / INSTALL / ACTIVATE / REMOVE ----
    // 通用参数校验辅助：取 params.<field> 字符串
    auto param_str = [&request](const char* field, std::string* out) -> bool {
        const JsonValue* params = request.find("params");
        const JsonValue* v = params ? params->find(field) : nullptr;
        if (!v || !v->is_string() || v->as_string().empty()) return false;
        *out = v->as_string();
        return true;
    };
    // 防 path traversal：model_id 只允许 [A-Za-z0-9_-]
    auto valid_model_id = [](const std::string& id) -> bool {
        if (id.size() < 1 || id.size() > 64) return false;
        for (char c : id) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        }
        return true;
    };

    if (type == "MODEL_LIST") {
        if (!model_list_) {
            resp.status = IpcError::kInternal;
            resp.error = "model list handler not registered";
            return resp;
        }
        resp.status = IpcError::kOk;
        resp.data = model_list_();
        return resp;
    }

    if (type == "MODEL_IMPORT") {
        if (!model_import_) {
            resp.status = IpcError::kInternal;
            resp.error = "model import handler not registered";
            return resp;
        }
        std::string src_path, model_id, label, source_format, sha256;
        if (!param_str("src_path", &src_path) || !param_str("model_id", &model_id)) {
            resp.status = IpcError::kBadRequest;
            resp.error = "params.src_path 与 params.model_id 必填（字符串）";
            return resp;
        }
        (void)param_str("label", &label);  // 可选
        (void)param_str("source_format", &source_format);  // 可选：rknn(默认)/onnx
        (void)param_str("sha256", &sha256);  // 可选：Web 层 hashlib 计算；空=跳过
        if (!valid_model_id(model_id)) {
            resp.status = IpcError::kBadRequest;
            resp.error = "model_id 只允许字母/数字/下划线/连字符（1~64 字符）";
            return resp;
        }
        std::string handler_error;
        if (!model_import_(src_path, model_id, label, source_format, sha256, &handler_error)) {
            resp.status = IpcError::kBadRequest;
            resp.error = handler_error.empty() ? "模型导入失败" : handler_error;
            return resp;
        }
        JsonValue data = JsonValue::object();
        data.set("model_id", JsonValue::string(model_id));
        resp.status = IpcError::kOk;
        resp.data = std::move(data);
        return resp;
    }

    // MODEL_VALIDATE / MODEL_INSTALL / MODEL_ACTIVATE / MODEL_REMOVE 共用形态
    auto handle_model_action = [&](const char* action_name,
                                   const ModelActionHandler& handler) -> IpcResponse {
        IpcResponse r;
        r.id = resp.id;
        r.type = type;
        if (!handler) {
            r.status = IpcError::kInternal;
            r.error = std::string(action_name) + " handler not registered";
            return r;
        }
        std::string model_id;
        if (!param_str("model_id", &model_id)) {
            r.status = IpcError::kBadRequest;
            r.error = "params.model_id 必填（字符串）";
            return r;
        }
        if (!valid_model_id(model_id)) {
            r.status = IpcError::kBadRequest;
            r.error = "model_id 只允许字母/数字/下划线/连字符";
            return r;
        }
        std::string handler_error;
        if (!handler(model_id, &handler_error)) {
            r.status = IpcError::kBadRequest;
            r.error = handler_error.empty() ? (std::string(action_name) + " 失败") : handler_error;
            return r;
        }
        JsonValue data = JsonValue::object();
        data.set("model_id", JsonValue::string(model_id));
        data.set("action", JsonValue::string(action_name));
        r.status = IpcError::kOk;
        r.data = std::move(data);
        return r;
    };

    if (type == "MODEL_VALIDATE") {
        return handle_model_action("validate", model_validate_);
    }
    if (type == "MODEL_INSTALL") {
        return handle_model_action("install", model_install_);
    }
    if (type == "MODEL_ACTIVATE") {
        return handle_model_action("activate", model_activate_);
    }
    if (type == "MODEL_REMOVE") {
        return handle_model_action("remove", model_remove_);
    }

    if (type == "MODEL_SET_CONCURRENCY") {
        if (!model_concurrency_) {
            resp.status = IpcError::kInternal;
            resp.error = "model concurrency handler not registered";
            return resp;
        }
        std::string model_id;
        if (!param_str("model_id", &model_id)) {
            resp.status = IpcError::kBadRequest;
            resp.error = "params.model_id 必填（字符串）";
            return resp;
        }
        if (!valid_model_id(model_id)) {
            resp.status = IpcError::kBadRequest;
            resp.error = "model_id 只允许字母/数字/下划线/连字符";
            return resp;
        }
        const JsonValue* params = request.find("params");
        const JsonValue* count_v = params ? params->find("count") : nullptr;
        if (!count_v || (!count_v->is_number() && !count_v->is_string())) {
            resp.status = IpcError::kBadRequest;
            resp.error = "params.count 必填（1~3 整数）";
            return resp;
        }
        int count = 0;
        if (count_v->is_number()) {
            count = static_cast<int>(count_v->as_int(0));
        } else {
            try {
                count = std::stoi(count_v->as_string());
            } catch (...) {
                count = 0;
            }
        }
        if (count < 1 || count > 3) {
            resp.status = IpcError::kBadRequest;
            resp.error = "params.count 必须在 1~3 之间";
            return resp;
        }
        std::string handler_error;
        if (!model_concurrency_(model_id, count, &handler_error)) {
            resp.status = IpcError::kBadRequest;
            resp.error = handler_error.empty() ? "设置并发失败" : handler_error;
            return resp;
        }
        JsonValue data = JsonValue::object();
        data.set("model_id", JsonValue::string(model_id));
        data.set("count", JsonValue::number(static_cast<double>(count)));
        resp.status = IpcError::kOk;
        resp.data = std::move(data);
        return resp;
    }

    // ---- M2.02：ACTIVATE_LICENSE（离线卡激活；Web → core 的唯一激活入口）----
    // 请求：{"type":"ACTIVATE_LICENSE","params":{"card":"<离线卡 JSON 信封>"}}
    // 响应：data = 授权 wire 投影（state/activated/plan/is_pro/features/ui_brand）；
    //       坏卡/他板卡/过期卡 ⇒ kBadRequest + 具体原因（fail-closed，不落盘）。
    if (type == "ACTIVATE_LICENSE") {
        if (!license_activate_) {
            resp.status = IpcError::kInternal;
            resp.error = "license activate handler not registered";
            return resp;
        }
        const JsonValue* params_v = request.find("params");
        if (params_v == nullptr || !params_v->is_object()) {
            resp.status = IpcError::kBadRequest;
            resp.error = "缺少必需字段 'params'（必须是对象）";
            return resp;
        }
        const JsonValue* card_v = params_v->find("card");
        if (card_v == nullptr || !card_v->is_string() ||
            card_v->as_string().empty()) {
            resp.status = IpcError::kBadRequest;
            resp.error = "params.card 必须为非空字符串（离线卡 JSON 信封）";
            return resp;
        }
        JsonValue data = JsonValue::object();
        std::string handler_error;
        if (!license_activate_(card_v->as_string(), &data, &handler_error)) {
            resp.status = IpcError::kBadRequest;
            resp.error = handler_error.empty() ? "激活被拒绝" : handler_error;
            return resp;
        }
        resp.status = IpcError::kOk;
        resp.data = std::move(data);
        return resp;
    }

    // ---- M2.07：ACTIVATE_CLOUD（云端卡密激活；web card-login 成功后交 core）----
    // 契约见 IpcServer.hpp（§3.2）：expire 校验/features 收窄/落盘/执法全在 core；
    // deactivate:true ⇒ 立即锁定（kExpired + last_error=reason），不落盘（D4 快路径）。
    if (type == "ACTIVATE_CLOUD") {
        if (!license_cloud_activate_) {
            resp.status = IpcError::kInternal;
            resp.error = "license cloud activate handler not registered";
            return resp;
        }
        const JsonValue* params_v = request.find("params");
        if (params_v == nullptr || !params_v->is_object()) {
            resp.status = IpcError::kBadRequest;
            resp.error = "缺少必需字段 'params'（必须是对象）";
            return resp;
        }
        JsonValue data = JsonValue::object();
        std::string handler_error;
        if (!license_cloud_activate_(*params_v, &data, &handler_error)) {
            resp.status = IpcError::kBadRequest;
            resp.error = handler_error.empty() ? "云端激活被拒绝" : handler_error;
            return resp;
        }
        resp.status = IpcError::kOk;
        resp.data = std::move(data);
        return resp;
    }

    resp.status = IpcError::kUnsupported;
    resp.error = "unsupported request type: " + type;
    return resp;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

bool ipc_request(const std::string& socket_path, const std::string& request_json,
                 std::string& response, int timeout_ms, std::string* error) {
    int fd = -1;
#if defined(_WIN32)
    fd = connect_tcp(socket_path, error);
#else
    fd = connect_unix(socket_path, error);
#endif
    if (fd < 0) return false;
    IPCDBG("[IPC-CLI] 已连接 fd=%d path=%s\n", fd, socket_path.c_str());

    if (!set_recv_timeout_ms(fd, timeout_ms)) {
        // 显式失败：旧实现无条件丢弃 setsockopt 返回值，把「超时设错」变成静默行为。
        if (error) *error = "设置接收超时失败 (sockerr=" + std::to_string(sock_last_error()) + ")";
        sock_close(fd);
        return false;
    }

    std::string payload = request_json;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');

    // ★ 2026-09-26（第四轮审计）：SOCK_STREAM 允许任意短写，大 payload（大卡信封 /
    //   超大 SET_CONFIG）只 send 一次会把半截 JSON 当成功 —— 与服务端同一口径：
    //   循环发到完整或明确断连。
    ssize_t sent_total = 0;
    while (sent_total < static_cast<ssize_t>(payload.size())) {
        const ssize_t n = sock_send(fd, payload.data() + sent_total,
                                    payload.size() - static_cast<size_t>(sent_total));
        if (n <= 0) break;
        sent_total += n;
    }
    IPCDBG("[IPC-CLI] fd=%d send n=%lld/%zu sockerr=%d\n", fd, static_cast<long long>(sent_total),
           payload.size(), sock_last_error());
    if (sent_total <= 0) {
        if (error) *error = "发送请求失败";
        sock_close(fd);
        return false;
    }
    if (sent_total < static_cast<ssize_t>(payload.size())) {
        if (error) *error = "发送请求不完整（" + std::to_string(sent_total) + "/" +
                            std::to_string(payload.size()) + "）";
        sock_close(fd);
        return false;
    }

    bool ok = false;
    // ★ 响应读取上限 8MB：GET_PREVIEW 的大 base64 响应轻松超过 64KB，
    //   旧实现复用请求的 64KB 上限读响应 ⇒ 高画质预览必然"读取响应失败"。
    response = read_line(fd, &ok, 8u * 1024 * 1024);
    IPCDBG("[IPC-CLI] fd=%d read ok=%d bytes=%zu\n", fd, ok ? 1 : 0, response.size());
    sock_close(fd);
    if (!ok) {
        if (error) *error = "读取响应失败（超时或连接关闭）";
        return false;
    }
    return true;
}

bool ipc_ping(const std::string& socket_path, std::string* error) {
    std::string resp_text;
    if (!ipc_request(socket_path, R"({"type":"PING"})", resp_text, 2000, error)) {
        return false;
    }
    JsonParseResult parsed = json_parse(resp_text);
    if (!parsed.ok || !parsed.value.is_object()) {
        if (error) *error = "PING 响应解析失败";
        return false;
    }
    const JsonValue* status_v = parsed.value.find("status");
    return status_v != nullptr && status_v->as_int() == static_cast<int64_t>(IpcError::kOk);
}

// ---------------------------------------------------------------------------
// SystemStatus -> JSON
// ---------------------------------------------------------------------------

JsonValue system_status_to_json(const SystemStatus& status) {
    JsonValue data = JsonValue::object();
    data.set("running", JsonValue::boolean(status.running));
    data.set("runtime_running", JsonValue::boolean(status.runtime_running));
    data.set("app_name", JsonValue::string(status.app_name));
    data.set("version", JsonValue::string(status.version));
    data.set("uptime_ms", JsonValue::number(status.uptime_ms));
    data.set("ipc_socket", JsonValue::string(status.ipc_socket));
    data.set("config_file", JsonValue::string(status.config_file));
    data.set("current_model_id", JsonValue::string(status.current_model_id));

    // ---- T1.10② 授权投影（§3.1 契约：字段名写死，Web/工具据此消费）----
    // 唯一来源 = LicenseGate 快照（Application 心跳写入 SystemStatus.license）；
    // 本层只做序列化，**不得**在此推导授权语义。既有字段语义不变（只增不删）。
    {
        const LicenseStatusBlock& lic = status.license;
        JsonValue l = JsonValue::object();
        l.set("activated", JsonValue::boolean(lic.activated));
        l.set("state", JsonValue::string(lic.state));
        l.set("plan", JsonValue::string(lic.plan));
        l.set("is_pro", JsonValue::boolean(lic.is_pro));
        JsonValue feats = JsonValue::array();
        for (const auto& f : lic.features) {
            feats.push_back(JsonValue::string(f));
        }
        l.set("features", std::move(feats));
        l.set("ui_brand", JsonValue::string(lic.ui_brand));
        // ---- M2.05 / M2.03 新增投影（**只增不删**；既有 12 字段一字不动）----
        // 计算点在 core（to_snapshot + Application 映射），本层只序列化、零推导。
        l.set("short_code", JsonValue::string(lic.short_code));
        JsonValue caps = JsonValue::object();
        caps.set("capture", JsonValue::boolean(lic.capabilities.capture));
        caps.set("inference", JsonValue::boolean(lic.capabilities.inference));
        caps.set("aim", JsonValue::boolean(lic.capabilities.aim));
        caps.set("ota", JsonValue::boolean(lic.capabilities.ota));
        l.set("capabilities", std::move(caps));
        l.set("expires_at", JsonValue::number(static_cast<double>(lic.expires_at)));
        l.set("grace_until", JsonValue::number(static_cast<double>(lic.grace_until)));
        l.set("last_error", JsonValue::string(lic.last_error));
        l.set("heartbeat_interval_s", JsonValue::number(static_cast<double>(lic.heartbeat_interval_s)));
        data.set("license", std::move(l));
    }

    JsonValue m = JsonValue::object();
    m.set("fps", JsonValue::number(status.metrics.fps));
    m.set("capture_fps", JsonValue::number(status.metrics.capture_fps));
    m.set("infer_total", JsonValue::number(static_cast<double>(status.metrics.infer_total)));
    m.set("mouse_dx", JsonValue::number(static_cast<double>(status.metrics.mouse_dx)));
    m.set("mouse_dy", JsonValue::number(static_cast<double>(status.metrics.mouse_dy)));
    m.set("gated_frames", JsonValue::number(static_cast<double>(status.metrics.gated_frames)));
    m.set("last_frame", JsonValue::number(static_cast<double>(status.metrics.last_frame)));
    m.set("last_timestamp_us", JsonValue::number(static_cast<double>(status.metrics.last_timestamp_us)));
    m.set("target_frames", JsonValue::number(static_cast<double>(status.metrics.target_frames)));
    m.set("no_target_frames", JsonValue::number(static_cast<double>(status.metrics.no_target_frames)));
    m.set("aim_active", JsonValue::boolean(status.metrics.aim_active));
    m.set("injection_allowed", JsonValue::boolean(status.metrics.injection_allowed));
    m.set("aim_hotkeys_suspended", JsonValue::boolean(status.metrics.aim_hotkeys_suspended));
    m.set("aim_active_profile", JsonValue::number(static_cast<double>(status.metrics.aim_active_profile)));
    m.set("mouse_control_connected", JsonValue::boolean(status.metrics.mouse_control_connected));
    m.set("output_backend_enabled", JsonValue::boolean(status.metrics.output_backend_enabled));
    m.set("mouse_control_socket_write_ok", JsonValue::number(static_cast<double>(status.metrics.mouse_control_socket_write_ok)));
    m.set("mouse_control_socket_write_fail", JsonValue::number(static_cast<double>(status.metrics.mouse_control_socket_write_fail)));
    m.set("mouse_control_send_count", JsonValue::number(static_cast<double>(status.metrics.mouse_control_send_count)));
    m.set("last_mouse_control_dx", JsonValue::number(static_cast<double>(status.metrics.last_mouse_control_dx)));
    m.set("last_mouse_control_dy", JsonValue::number(static_cast<double>(status.metrics.last_mouse_control_dy)));
    m.set("last_mouse_control_wheel", JsonValue::number(static_cast<double>(status.metrics.last_mouse_control_wheel)));
    m.set("last_mouse_control_timestamp_us", JsonValue::number(static_cast<double>(status.metrics.last_mouse_control_timestamp_us)));
    m.set("aim_error_x", JsonValue::number(status.metrics.aim_error_x));
    m.set("aim_error_y", JsonValue::number(status.metrics.aim_error_y));
    m.set("target_point_x", JsonValue::number(status.metrics.target_point_x));
    m.set("target_point_y", JsonValue::number(status.metrics.target_point_y));
    m.set("reference_x", JsonValue::number(status.metrics.reference_x));
    m.set("reference_y", JsonValue::number(status.metrics.reference_y));
    m.set("pid_output_x", JsonValue::number(status.metrics.pid_output_x));
    m.set("pid_output_y", JsonValue::number(status.metrics.pid_output_y));
    m.set("scheduler_input_x", JsonValue::number(status.metrics.scheduler_input_x));
    m.set("scheduler_input_y", JsonValue::number(status.metrics.scheduler_input_y));
    // 压枪 v1 闭环遥测（面板 control_trace.recoil_cl 消费）
    m.set("recoil_cl_add_y", JsonValue::number(status.metrics.recoil_cl_add_y));
    m.set("recoil_cl_integral", JsonValue::number(status.metrics.recoil_cl_integral));
    m.set("recoil_cl_state", JsonValue::number(static_cast<double>(status.metrics.recoil_cl_state)));
    m.set("recoil_cl_obs_frames", JsonValue::number(static_cast<double>(status.metrics.recoil_cl_obs_frames)));
    m.set("recoil_cl_p_term", JsonValue::number(status.metrics.recoil_cl_p_term));
    m.set("recoil_cl_i_term", JsonValue::number(status.metrics.recoil_cl_i_term));
    // 目标中心（标定状态机的真实目标位移数据源）
    m.set("aim_pos_x", JsonValue::number(status.metrics.aim_pos_x));
    m.set("aim_pos_y", JsonValue::number(status.metrics.aim_pos_y));
    // 累计请求投递 count（自动标定的分母真源）：标定侧取两次读数之差算 gain = Δpx/Δcounts。
    m.set("aim_out_counts_x", JsonValue::number(static_cast<double>(status.metrics.aim_out_counts_x)));
    m.set("aim_out_counts_y", JsonValue::number(static_cast<double>(status.metrics.aim_out_counts_y)));
    m.set("aim_has_target", JsonValue::boolean(status.metrics.aim_has_target));
    m.set("aim_target_id", JsonValue::number(static_cast<double>(status.metrics.aim_target_id)));
    m.set("aim_target_class_id", JsonValue::number(static_cast<double>(status.metrics.aim_target_class_id)));
    m.set("aim_target_width", JsonValue::number(status.metrics.aim_target_width));
    m.set("aim_target_height", JsonValue::number(status.metrics.aim_target_height));
    m.set("aim_target_x1", JsonValue::number(status.metrics.aim_target_x1));
    m.set("aim_target_y1", JsonValue::number(status.metrics.aim_target_y1));
    m.set("aim_target_x2", JsonValue::number(status.metrics.aim_target_x2));
    m.set("aim_target_y2", JsonValue::number(status.metrics.aim_target_y2));
    JsonValue boxes = JsonValue::array();
    for (const auto& box : status.metrics.detection_boxes) {
        JsonValue item = JsonValue::object();
        item.set("x1", JsonValue::number(box.x1));
        item.set("y1", JsonValue::number(box.y1));
        item.set("x2", JsonValue::number(box.x2));
        item.set("y2", JsonValue::number(box.y2));
        item.set("score", JsonValue::number(box.score));
        item.set("class_id", JsonValue::number(static_cast<double>(box.class_id)));
        boxes.push_back(std::move(item));
    }
    m.set("detection_boxes", std::move(boxes));
    m.set("preview_fps", JsonValue::number(status.metrics.preview_fps));
    m.set("preview_encode_ms", JsonValue::number(status.metrics.preview_encode_ms));
    m.set("preview_width", JsonValue::number(static_cast<double>(status.metrics.preview_width)));
    m.set("preview_height", JsonValue::number(static_cast<double>(status.metrics.preview_height)));
    m.set("preview_bytes", JsonValue::number(static_cast<double>(status.metrics.preview_bytes)));
    m.set("preview_frames", JsonValue::number(static_cast<double>(status.metrics.preview_frames)));
    m.set("preview_dropped", JsonValue::number(static_cast<double>(status.metrics.preview_dropped)));
    // ★ M2.03：预览水印降级投影（受限卡 true；全功能 false）—— 供 /api/state.preview.watermark 观测。
    m.set("preview_watermark", JsonValue::boolean(status.metrics.preview_watermark));
    m.set("buffer_age_ms", JsonValue::number(status.metrics.buffer_age_ms));
    m.set("last_dequeued_count", JsonValue::number(static_cast<double>(status.metrics.last_dequeued_count)));
    m.set("buffer_count", JsonValue::number(static_cast<double>(status.metrics.buffer_count)));
    m.set("input_width", JsonValue::number(static_cast<double>(status.metrics.input_width)));
    m.set("input_height", JsonValue::number(static_cast<double>(status.metrics.input_height)));
    m.set("capture_ms", JsonValue::number(status.metrics.capture_ms));
    m.set("resize_ms", JsonValue::number(status.metrics.resize_ms));
    m.set("infer_ms", JsonValue::number(status.metrics.infer_ms));
    m.set("infer_set_input_ms", JsonValue::number(status.metrics.infer_set_input_ms));
    m.set("infer_run_ms", JsonValue::number(status.metrics.infer_run_ms));
    m.set("infer_output_ms", JsonValue::number(status.metrics.infer_output_ms));
    m.set("decode_ms", JsonValue::number(status.metrics.decode_ms));
    m.set("aim_ms", JsonValue::number(status.metrics.aim_ms));
    m.set("e2e_ms", JsonValue::number(status.metrics.e2e_ms));
    // 分位数（真实样本，ms）
    m.set("e2e_p50_ms", JsonValue::number(status.metrics.e2e_p50_ms));
    m.set("e2e_p95_ms", JsonValue::number(status.metrics.e2e_p95_ms));
    m.set("e2e_p99_ms", JsonValue::number(status.metrics.e2e_p99_ms));
    m.set("e2e_max_ms", JsonValue::number(status.metrics.e2e_max_ms));
    m.set("infer_p50_ms", JsonValue::number(status.metrics.infer_p50_ms));
    m.set("infer_p95_ms", JsonValue::number(status.metrics.infer_p95_ms));
    m.set("infer_p99_ms", JsonValue::number(status.metrics.infer_p99_ms));
    m.set("decode_p50_ms", JsonValue::number(status.metrics.decode_p50_ms));
    m.set("decode_p95_ms", JsonValue::number(status.metrics.decode_p95_ms));
    m.set("decode_p99_ms", JsonValue::number(status.metrics.decode_p99_ms));
    // ---- T1.15：模型输入通路诊断（契约只增不删；名字稳定，Web 按名消费）----
    // 用途：换 INT8 模型后无需查板端 UART 日志，面板即可确认"是否真的吃到快路径"。
    m.set("model_input_pass_mode", JsonValue::string(status.metrics.model_input_pass_mode));
    m.set("model_input_type", JsonValue::number(static_cast<double>(status.metrics.model_input_type)));
    m.set("model_input_type_name", JsonValue::string(status.metrics.model_input_type_name));
    m.set("model_input_fmt", JsonValue::number(static_cast<double>(status.metrics.model_input_fmt)));
    m.set("model_input_fmt_name", JsonValue::string(status.metrics.model_input_fmt_name));
    m.set("model_input_qnt_type", JsonValue::number(static_cast<double>(status.metrics.model_input_qnt_type)));
    m.set("model_input_qnt_name", JsonValue::string(status.metrics.model_input_qnt_name));
    m.set("model_input_zp", JsonValue::number(static_cast<double>(status.metrics.model_input_zp)));
    m.set("model_input_scale", JsonValue::number(status.metrics.model_input_scale));
    m.set("model_input_width", JsonValue::number(static_cast<double>(status.metrics.model_input_width)));
    m.set("model_input_height", JsonValue::number(static_cast<double>(status.metrics.model_input_height)));
    m.set("model_zero_copy_ready", JsonValue::boolean(status.metrics.model_zero_copy_ready));
    m.set("model_fast_path_active", JsonValue::boolean(status.metrics.model_fast_path_active));
    m.set("model_external_dma_requested", JsonValue::boolean(status.metrics.model_external_dma_requested));
    m.set("model_external_dma_bound", JsonValue::boolean(status.metrics.model_external_dma_bound));
    m.set("model_workers_total", JsonValue::number(static_cast<double>(status.metrics.model_workers_total)));
    m.set("model_workers_zero_copy", JsonValue::number(static_cast<double>(status.metrics.model_workers_zero_copy)));
    m.set("model_workers_fast_path", JsonValue::number(static_cast<double>(status.metrics.model_workers_fast_path)));
    m.set("model_input_note", JsonValue::string(status.metrics.model_input_note));
    m.set("detect_count", JsonValue::number(static_cast<double>(status.metrics.detect_count)));
    m.set("tracks", JsonValue::number(static_cast<double>(status.metrics.tracks)));
    m.set("frames_superseded", JsonValue::number(static_cast<double>(status.metrics.frames_superseded)));
    m.set("inference_capacity_fps", JsonValue::number(status.metrics.inference_capacity_fps));
    m.set("raw_preprocess_backend", JsonValue::string(status.metrics.preprocess_backend));
    m.set("raw_preprocess_error", JsonValue::string(status.metrics.preprocess_error));
    m.set("frames_total", JsonValue::number(static_cast<double>(status.metrics.frames_total)));
    data.set("metrics", std::move(m));
    return data;
}

}  // namespace ttbox::core
