// ipc_client.cpp — IpcClient 实现（见 .hpp 头注释）。
#include "web/infra/ipc_client.hpp"

#include <string>

#include "ipc/IpcServer.hpp"

namespace ttbox::core::web {

namespace {

// IPC 错误码与 core/src/ipc/IpcServer.hpp::IpcError 同值；此处仅取合成错误所需的 kInternal。
constexpr double kIpcInternal = 3.0;

// 构造合成错误响应（IPC 不可达 / 响应解析失败时的兜底），供 HTTP 层统一映射。
JsonValue make_error_response(const std::string& type, const std::string& error) {
    JsonValue resp = JsonValue::object();
    resp.set("type", JsonValue::string(type));
    resp.set("status", JsonValue::number(kIpcInternal));
    resp.set("error", JsonValue::string(error));
    resp.set("data", JsonValue::object());
    return resp;
}

}  // namespace

// 发送一条 IPC 请求：编 NDJSON → 收响应 → 解析；不可达/解析失败返回合成错误对象。
JsonValue IpcClient::call(const std::string& type, const JsonValue& params,
                          int timeout_ms) const {
    JsonValue request = JsonValue::object();
    request.set("type", JsonValue::string(type));
    if (params.is_object()) {
        request.set("params", params);
    }

    std::string response_text;
    std::string error;
    // ipc_request 内部自动补齐行尾 '\n'（NDJSON 一行协议），此处传紧凑 JSON 即可。
    if (!ttbox::core::ipc_request(socket_path_, request.dump(), response_text,
                                  timeout_ms, &error)) {
        return make_error_response(type, "ipc_unavailable: " + error);
    }

    JsonParseResult parsed = ttbox::core::json_parse(response_text);
    if (!parsed.ok || !parsed.value.is_object()) {
        return make_error_response(type, "ipc_bad_response: " + parsed.error);
    }
    return parsed.value;
}

// 探活：内部走 core 的 ipc_ping。
bool IpcClient::ping(std::string* error) const {
    return ttbox::core::ipc_ping(socket_path_, error);
}

}  // namespace ttbox::core::web
