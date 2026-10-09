// preview.cpp — MJPEG 预览流（1 条路由，URL 一字未改）。
//
// 自 plugins/web/api/preview.py 逐行为移植。**V1.0.52：唯一数据源 = core GET_PREVIEW IPC**
// （base64 JPEG 解码后组 multipart 帧）。
//
// ★ 为何删掉 8001 回退（业主令「去掉所有 Python 代码」）：
//   原实现是「IPC 可达走直出，IPC 不可达则 socket 级透传 127.0.0.1:8001 的 Python
//   preview 服务」。但那个上游**自己也要走同一个 core IPC 取帧**
//   （ttbox-preview.py:58 `sock.sendall(b'{"type":"GET_PREVIEW"}\n')`）⇒ core 真挂时
//   上游同样拿不到帧，只能吐它缓存里的**最后一帧**。
//   ⇒ 该回退不但无效，还把「core 已死」这个事实**伪装成一张冻结的画面**，掩盖真实状态。
//   删除后：IPC 不可达 ⇒ 保持连接重试，画面不更新（前端按 preview.fps 归零即可判掉线）。
#include "web/domain/domain_routes.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "auth/LicenseCard.hpp"
#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

// 注册 MJPEG 预览路由：GET /api/preview.mjpg（分块推流，唯一数据源 = core GET_PREVIEW）。
void register_preview_routes(httplib::Server& svr, IpcClient& ipc) {
    // 持续拉取 core 的 JPEG 帧，按 multipart/x-mixed-replace 逐帧推出；IPC 不可达则放慢重试。
    svr.Get("/api/preview.mjpg", [&ipc](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store, no-cache");
        res.set_header("X-Accel-Buffering", "no");
        auto last_seq = std::make_shared<int64_t>(-1);
        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=ttboxframe",
            [&ipc, last_seq](size_t, httplib::DataSink& sink) -> bool {
                // ★ V1.0.49 起动态判断（不再启动时冻结一次 ipc_mode）：
                //   IPC 可达 ⇒ 走直出；即便此刻暂无帧（预览未开）也只 sleep 重试，
                //   这样面板开着时「开启预览」下一 tick 即出图，无需刷新页面。
                // ★ V1.0.52：IPC 完全不可达时**不再回退 8001**（理由见文件头），改为放慢重试。
                const JsonValue r = ipc.call("GET_PREVIEW", JsonValue::object(), 2000);
                if (ipc_status(r) != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    return sink.is_writable();
                }
                const JsonValue* d = r.find("data");
                if (d != nullptr && d->is_object()) {
                    const std::string b64 = json_field(*d, "jpeg_base64").as_string("");
                    const int64_t seq = json_field(*d, "seq").as_int(0);
                    if (!b64.empty() && seq != *last_seq) {
                        std::vector<uint8_t> px;
                        if (ttbox::core::auth::license_base64_decode(b64, &px) && !px.empty()) {
                            *last_seq = seq;
                            std::string frame =
                                "--ttboxframe\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                                std::to_string(px.size()) + "\r\n\r\n";
                            frame.append(reinterpret_cast<const char*>(px.data()), px.size());
                            frame += "\r\n";
                            if (!sink.write(frame.data(), frame.size())) return false;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return sink.is_writable();
            });
    });
}

}  // namespace ttbox::core::web
