// ipc_client.hpp — Web → Core IPC 客户端薄封装（T01 基础设施）
//
// 复用 core 既有 ttbox::core::ipc_request / ipc_ping（core/src/ipc/IpcServer.hpp），
// 不新写 socket。职责：把「type + params」编成 NDJSON 一行请求，解析一行响应，
// 向上返回 {id,type,status,data,error} 完整响应对象；IPC 不可达时不抛异常，
// 而是返回 status=3（kInternal）的合成错误对象，HTTP 层据此映射 503 core_offline。
#pragma once

#include <string>
#include <utility>

#include "common/Json.hpp"

namespace ttbox::core::web {

// 超时分级常量（与方案附 D / api_v1.py TIMEOUTS 同值；Core 无超时，客户端兜底）。
inline constexpr int kIpcTimeoutDefaultMs = 5000;              // 其余命令
inline constexpr int kIpcTimeoutRuntimeControlMs = 30000;      // RUNTIME_CONTROL
inline constexpr int kIpcTimeoutModelImportMs = 60000;         // MODEL_IMPORT / INSTALL
inline constexpr int kIpcTimeoutModelActivateMs = 120000;      // MODEL_ACTIVATE / VALIDATE
inline constexpr int kIpcTimeoutPreviewMs = 3000;              // GET_PREVIEW

class IpcClient {
public:
    explicit IpcClient(std::string socket_path) : socket_path_(std::move(socket_path)) {}

    // 发送一条 IPC 请求。timeout_ms 为客户端兜底超时（毫秒）。
    // 返回完整响应对象；不可达 / 响应解析失败时返回合成错误对象（status=3）。
    JsonValue call(const std::string& type, const JsonValue& params, int timeout_ms) const;

    // PING 探活（内部走 ttbox::core::ipc_ping）。
    bool ping(std::string* error = nullptr) const;

    const std::string& socket_path() const { return socket_path_; }

private:
    std::string socket_path_;
};

}  // namespace ttbox::core::web
