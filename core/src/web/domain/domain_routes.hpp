// domain_routes.hpp — 7 个域层的路由注册声明（T03）。
//
// 各域 .cpp 实现 register_*；WebServer::register_routes() 统一调用。
// collect_web_state / models_view / ota_current_version / preset_names 供跨域复用。
#pragma once

#include "common/Json.hpp"
#include "httplib.h"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

void register_control_routes(httplib::Server& svr, IpcClient& ipc);
void register_state_routes(httplib::Server& svr, IpcClient& ipc);
void register_models_routes(httplib::Server& svr, IpcClient& ipc);
void register_models_meta_routes(httplib::Server& svr, IpcClient& ipc);
void register_presets_routes(httplib::Server& svr, IpcClient& ipc);
void register_hardware_routes(httplib::Server& svr, IpcClient& ipc);
void register_license_routes(httplib::Server& svr, IpcClient& ipc);
void register_ota_routes(httplib::Server& svr, IpcClient& ipc);
void register_system_routes(httplib::Server& svr, IpcClient& ipc);
void register_diagnostics_routes(httplib::Server& svr, IpcClient& ipc);
void register_brand_routes(httplib::Server& svr, IpcClient& ipc);
void register_pages_routes(httplib::Server& svr, IpcClient& ipc);
void register_aim_routes(httplib::Server& svr, IpcClient& ipc);
void register_preview_routes(httplib::Server& svr, IpcClient& ipc);
void register_calib_routes(httplib::Server& svr, IpcClient& ipc);
void register_motion_routes(httplib::Server& svr, IpcClient& ipc);

// /api/state 与 /api/control/* 共用的状态快照；core 离线返回 JsonValue::null() 哨兵。
JsonValue collect_web_state(IpcClient& ipc);

// MODEL_LIST data → 面板模型卡片数组（/api/state 与 /api/models/select 同源）。
// 需要 ipc：rknn_concurrency 在 worker_cores 缺失时回退 GET_CONFIG 全局默认。
JsonValue models_view(IpcClient& ipc, const JsonValue& ml_data);

// 系统部署版本（current 软链名；对齐 Python _ota_current_version）。
std::string ota_current_version(IpcClient& ipc);

// 预设名数组（排除 `_` 保留名，对齐 Python _preset_names）。
JsonValue preset_names();

}  // namespace ttbox::core::web
