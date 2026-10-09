// brand.cpp — 网页背景（branding background）域（5 条路由，URL 一字未改）。
//
// 自 plugins/web/api/brand.py 逐行为移植。读/写/改/删全返回同一拒绝响应，
// 不碰 IPC、不碰磁盘、不读配置（网页背景仅对 TTBOX 授权系统开放）。
#include "web/domain/domain_routes.hpp"

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// 统一拒绝响应：网页背景仅对 TTBOX 授权系统开放。
void branding_rejected(httplib::Response& res) {
    send_json(res, 200, false, JsonValue::object(), "网页背景仅对 TTBOX 授权系统开放", "");
}

}  // namespace

// 注册网页背景域路由：4 个读写方法统一拒绝，image 返回空对象。
void register_brand_routes(httplib::Server& svr, IpcClient&) {
    svr.Get("/api/branding/background",
            [](const httplib::Request&, httplib::Response& res) { branding_rejected(res); });
    svr.Post("/api/branding/background",
             [](const httplib::Request&, httplib::Response& res) { branding_rejected(res); });
    svr.Patch("/api/branding/background",
              [](const httplib::Request&, httplib::Response& res) { branding_rejected(res); });
    svr.Delete("/api/branding/background",
               [](const httplib::Request&, httplib::Response& res) { branding_rejected(res); });

    svr.Get("/api/branding/background/image",
            [](const httplib::Request&, httplib::Response& res) {
                send_json(res, 200, true, JsonValue::object(), "", "");
            });
}

}  // namespace ttbox::core::web
