// ota_internal.hpp — OTA 域内部共享声明（ota.cpp 路由层 ↔ ota_impl.cpp 实现层）。
#pragma once

#include "httplib.h"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

// OTA 默认 key_id（对齐 lib/ota.py::_OTA_DEFAULT_KEY_ID；install 与 check 共用）。
inline const char* ota_default_key_id() { return "ttbox-ota-2026b"; }

// /api/ota/install 与 /api/update/install 共用的安装实现（D1 特权端点）。
void ota_install_impl(IpcClient& ipc, const httplib::Request& req, httplib::Response& res);

// GET /api/update/status 的投影实现。
void ota_status_impl(httplib::Response& res);

// POST /api/update/check 的检查实现。
void ota_check_impl(IpcClient& ipc, httplib::Response& res);

}  // namespace ttbox::core::web
