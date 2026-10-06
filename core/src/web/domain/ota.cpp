// ota.cpp — OTA 与更新域路由（薄编排，实现见 ota_impl.cpp）。
//
// 自 plugins/web/api/ota.py 与 lib/ota.py 逐行为移植。URL 一字未改。
// POST /api/ota/install 与 /api/update/install 是特权端点（D1 来源校验），
// 写任务文件交 root 更新器（web 无 sudoers，不直接 systemd-run）。
#include "web/domain/domain_routes.hpp"
#include "web/domain/ota_internal.hpp"

namespace ttbox::core::web {

void register_ota_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Post("/api/ota/install",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 ota_install_impl(ipc, req, res);
             });
    svr.Post("/api/update/install",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 ota_install_impl(ipc, req, res);
             });

    svr.Get("/api/update/status",
            [](const httplib::Request& req, httplib::Response& res) {
                (void)req;
                ota_status_impl(res);
            });

    svr.Post("/api/update/check",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 (void)req;
                 ota_check_impl(ipc, res);
             });
}

}  // namespace ttbox::core::web
