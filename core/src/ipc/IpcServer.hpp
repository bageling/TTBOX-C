// IpcServer.hpp — AF_UNIX JSON 行协议服务端（阶段 A-1：最小 IPC）
//
// 协议（详见 docs/ipc-protocol.md）：
//   - 传输：AF_UNIX SOCK_STREAM（Unix）/ TCP loopback（Windows，路径 "tcp:PORT"）
//   - 帧格式：一行 JSON（'\n' 分隔）
//   - 请求：{"id":<可选>,"type":"PING|GET_STATUS|GET_CONFIG|SET_CONFIG|RUNTIME_CONTROL|ACTIVATE_LICENSE|ACTIVATE_CLOUD","params":<可选>}
//   - 响应：{"id":...,"type":...,"status":<错误码>,"data":{...},"error":<可选>}
//   - 错误码：0 OK / 1 BAD_REQUEST / 2 NOT_FOUND / 3 INTERNAL / 4 UNSUPPORTED
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

#include "common/Json.hpp"
#include "common/Metrics.hpp"

namespace ttbox::core {

// IPC 错误码（与 docs/ipc-protocol.md 保持一致）
enum class IpcError : int {
    kOk = 0,
    kBadRequest = 1,
    kNotFound = 2,
    kInternal = 3,
    kUnsupported = 4,
};

struct IpcResponse {
    IpcError status = IpcError::kInternal;
    std::string id;
    std::string type;
    std::string error;
    JsonValue data = JsonValue::object();

    // 序列化为一行 JSON（含换行）
    std::string to_json() const;
};

class IpcServer {
public:
    // 析构：停服并排空在途连接（见 stop）
    ~IpcServer();

    // 启动监听。socket_path: Unix 下为文件路径（默认 common/Paths.hpp::kIpcSocketDefault，
    // 可用 --ipc 参数或 TTBOX_IPC_SOCKET 环境变量覆盖）；Windows 下为 "tcp:<port>"。
    bool start(const std::string& socket_path, std::string* error = nullptr);
    // 停止监听并排空在途连接（阻塞至所有连接线程退出，可重复调用）
    void stop();
    bool running() const { return running_.load(); }
    const std::string& socket_path() const { return socket_path_; }

    // 数据提供回调（由 Application 注入）
    using StatusProvider = std::function<SystemStatus()>;
    using ConfigProvider = std::function<JsonValue()>;
    // 注入状态快照提供器（GET_STATUS）
    void set_status_provider(StatusProvider p) { status_provider_ = std::move(p); }
    // 注入配置读取器（GET_CONFIG）
    void set_config_provider(ConfigProvider p) { config_provider_ = std::move(p); }

    // Phase2：预览快照回调（返回 JPEG 字节；无帧返回 false）
    using PreviewProvider = std::function<bool(std::vector<uint8_t>*, uint64_t*)>;  // 出参2: 帧序号（seq，供 web 去重）
    void set_preview_provider(PreviewProvider p) { preview_provider_ = std::move(p); }

    // 配置写入回调（SET_CONFIG）：由 Application 注入。
    // 原子序（在回调内实现）：JSON 解析 → RuntimeProfile::validate → RuntimeConfig.update → 持久化。
    // 返回 false 时 resp.error 带失败原因，当前运行配置保证不被污染。
    // persisted=true 表示已写入配置文件；data 返回 {"applied":true,"persisted":bool}。
    using ConfigUpdateHandler = std::function<bool(const JsonValue& profile_json,
                                                   std::string* error, bool* persisted)>;
    void set_config_update_handler(ConfigUpdateHandler h) { config_update_ = std::move(h); }

    // Runtime 启停回调（RUNTIME_CONTROL）：action = "start"|"stop"|"restart"。
    using RuntimeControlHandler = std::function<bool(const std::string& action, std::string* error)>;
    void set_runtime_control_handler(RuntimeControlHandler h) { runtime_control_ = std::move(h); }

    // ---- 硬件写入回调（V1.0.50；core 跑 User=root，是唯一有权写 sysfs 的层）----
    // Web 后端（ttbox 身份，无 sudo）做不到这两件事：EDID 注入要写
    // /sys/class/hdmirx/** 与 /sys/kernel/debug/hdmirx/，USB 透传切换要改 systemd
    // 单元的 USB_PROXY_MODE 并重载服务。web 只能把请求转给 core。
    // 语义：params 传 JSON，handler 返回执行结果（data 供回显，error 供报错文案）。
    //   APPLY_EDID  —— 应用 EDID（写 config + 调 scripts/edid/edid_apply.sh）
    //   SET_USB_MODE —— 切换 USB 透传模式（改单元 + daemon-reload + restart usbproxy）
    using HardwareActionHandler = std::function<bool(const std::string& action,
                                                     const JsonValue& params,
                                                     JsonValue* data,
                                                     std::string* error)>;
    void set_hardware_action_handler(HardwareActionHandler h) { hardware_action_ = std::move(h); }

    // ---- 模型管理回调（v0.3）：桥接 ModelRegistry，由 Application 注入 ----
    // 注意：文件上传不走 IPC（IPC 帧不适合大二进制）。上传 = 调用方先把 .rknn
    // 写入 Core 可访问的 staging 收件目录（默认 models/_incoming/<文件名>），
    // 再发 MODEL_IMPORT 引用该路径。Gateway 的 HTTP 上传端点负责落盘到该目录。
    // list：返回 installed 模型清单 + 当前激活 id
    using ModelListHandler = std::function<JsonValue()>;
    // import：src_path（已在收件目录）→ model_id, label
    using ModelImportHandler = std::function<bool(const std::string& src_path,
                                                  const std::string& model_id,
                                                  const std::string& label,
                                                  const std::string& source_format,
                                                  const std::string& sha256,
                                                  std::string* error)>;
    // validate / install / activate / remove：按 model_id
    using ModelActionHandler = std::function<bool(const std::string& model_id, std::string* error)>;
    void set_model_list_handler(ModelListHandler h) { model_list_ = std::move(h); }
    void set_model_import_handler(ModelImportHandler h) { model_import_ = std::move(h); }
    void set_model_validate_handler(ModelActionHandler h) { model_validate_ = std::move(h); }
    void set_model_install_handler(ModelActionHandler h) { model_install_ = std::move(h); }
    void set_model_activate_handler(ModelActionHandler h) { model_activate_ = std::move(h); }
    void set_model_remove_handler(ModelActionHandler h) { model_remove_ = std::move(h); }

    // 模型并发设置回调（MODEL_SET_CONCURRENCY）：按 model_id 写 manifest worker_cores。
    using ModelConcurrencyHandler = std::function<bool(const std::string& model_id, int count, std::string* error)>;
    void set_model_concurrency_handler(ModelConcurrencyHandler h) { model_concurrency_ = std::move(h); }

    // ---- M2.02：离线卡激活回调（ACTIVATE_LICENSE）----
    // 请求：{"type":"ACTIVATE_LICENSE","params":{"card":"<离线卡 JSON 信封原文>"}}
    // 回调内原子序：set_card → 立即验签（Ed25519 + 设备绑定 + 过期）→ kValid 才
    // 落盘 LicenseStore（license.json）→ publish 到 LicenseGate。
    // 返回 true 时 *data 填授权 wire 投影（state/activated/plan/is_pro/features/ui_brand）；
    // 返回 false 时 resp.error 带拒绝原因（坏卡/他板卡/过期卡 ⇒ 400 语义）。
    using LicenseActivateHandler = std::function<bool(const std::string& card,
                                                     JsonValue* data,
                                                     std::string* error)>;
    void set_license_activate_handler(LicenseActivateHandler h) { license_activate_ = std::move(h); }

    // ---- M2.07：云端卡密激活回调（ACTIVATE_CLOUD；web card-login 成功后交 core 落盘/执法）----
    // 请求：{"type":"ACTIVATE_CLOUD","params":{
    //   "expire_unix_ms":<ms,必填,>now>, "features":[...], "plan":"subscription",
    //   "card_mask":"LS-****-XXXX", "max_devices":3, "source":"cloud",
    //   "deactivate":false }}     // true ⇒ 立即锁定（kExpired），不落盘
    // 返回 true 时 *data 填授权 wire 投影（与 ACTIVATE_LICENSE 同形）；
    // 返回 false 时 resp.error 带拒绝原因（expire 非法 / daemon 未初始化 / 落盘失败）。
    using LicenseCloudActivateHandler = std::function<bool(const JsonValue& params,
                                                           JsonValue* data,
                                                           std::string* error)>;
    void set_license_cloud_activate_handler(LicenseCloudActivateHandler h) {
        license_cloud_activate_ = std::move(h);
    }

private:
    // 接受连接主循环（单线程）；连接数超限则立即关闭新连接
    void accept_loop();
    // 单连接处理：读一行请求、分发、回写一行响应
    void handle_connection(int fd);
    // 按 type 分发请求到对应回调并组装响应
    IpcResponse handle_request(const JsonValue& request);

    std::string socket_path_;   // 生效的监听路径（Windows 临时端口会回写实际端口）
    int listen_fd_ = -1;        // 监听 socket fd（-1 = 未监听）
    // Preview 是较大的 base64 响应，浏览器刷新、状态轮询和配置读取会短时重叠。
    // 8 个连接会把正常 Web 并发误判为过载并直接断开；32 仍是明确硬上限，
    // 同时覆盖实际产品并发峰值。
    static constexpr int kMaxConnections = 32;
    std::atomic<int> active_connections_{0};
    // 在途连接 fd 跟踪：stop() 需要 shutdown 这些 fd 唤醒卡在 read_line 的
    // detached 线程，并排空 active_connections_ 后再析构，否则连接线程在对象
    // 销毁后仍访问 this（handler/计数器）→ use-after-free 崩溃（竞态，位置随机）。
    std::mutex conn_fds_mutex_;
    std::condition_variable conn_fds_cv_;
    std::vector<int> conn_fds_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    StatusProvider status_provider_;
    PreviewProvider preview_provider_;
    ConfigProvider config_provider_;
    ConfigUpdateHandler config_update_;
    RuntimeControlHandler runtime_control_;
    HardwareActionHandler hardware_action_;  // V1.0.50：APPLY_EDID / SET_USB_MODE
    ModelListHandler model_list_;
    ModelImportHandler model_import_;
    ModelActionHandler model_validate_;
    ModelActionHandler model_install_;
    ModelActionHandler model_activate_;
    ModelActionHandler model_remove_;
    ModelConcurrencyHandler model_concurrency_;
    LicenseActivateHandler license_activate_;
    // ★ M2.07：云端卡密激活回调（ACTIVATE_CLOUD）。缺此成员 ⇒ set_*_handler 引用未声明
    //   标识符（编译期 TTBOX_CORE 构建失败），且 handle_request 的 ACTIVATE_CLOUD 分支无法编译。
    LicenseCloudActivateHandler license_cloud_activate_;
};

// 同步请求客户端：发送一行 JSON 请求，读取一行 JSON 响应。
// 成功返回 true，response 为响应 JSON 文本；失败返回 false（error 说明）。
bool ipc_request(const std::string& socket_path, const std::string& request_json,
                 std::string& response, int timeout_ms = 2000, std::string* error = nullptr);

// 便捷 PING：成功返回 true（可同时拿到 data.pong）
bool ipc_ping(const std::string& socket_path, std::string* error = nullptr);

// SystemStatus -> JSON（供 GET_STATUS 输出）
JsonValue system_status_to_json(const SystemStatus& status);

}  // namespace ttbox::core
