// ota.cpp — OTA 与更新域路由（薄编排，实现见 ota_impl.cpp）。
//
// 自 plugins/web/api/ota.py 与 lib/ota.py 逐行为移植。URL 一字未改。
// POST /api/ota/install 与 /api/update/install 是特权端点（D1 来源校验），
// 写任务文件交 root 更新器（web 无 sudoers，不直接 systemd-run）。
#include "web/domain/domain_routes.hpp"
#include "web/domain/ota_internal.hpp"

namespace ttbox::core::web {

// 注册 OTA 路由（薄编排）：安装 / 状态 / 检查。
void register_ota_routes(httplib::Server& svr, IpcClient& ipc) {
    // POST /api/ota/install：特权端点，写任务文件交 root 更新器。
    svr.Post("/api/ota/install",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 ota_install_impl(ipc, req, res);
             });
    // POST /api/update/install：同上（旧版别名端点）。
    svr.Post("/api/update/install",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 ota_install_impl(ipc, req, res);
             });

    // GET /api/update/status：读更新器写的 ota_status.json 并投影状态。
    svr.Get("/api/update/status",
            [](const httplib::Request& req, httplib::Response& res) {
                (void)req;
                ota_status_impl(res);
            });

    // POST /api/update/check：向 OTA 服务器查询是否有新版本。
    svr.Post("/api/update/check",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 (void)req;
                 ota_check_impl(ipc, res);
             });
}

}  // namespace ttbox::core::web
