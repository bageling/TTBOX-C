// calib.cpp — 自动标定域（5 条路由，URL 一字未改）。
//
// 自 plugins/web/api/calib.py 逐行移植。算法/线程体在 infra/calibration_controller.*。
#include "web/domain/domain_routes.hpp"

#include <string>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/calibration_controller.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// 运行期共享单例（对齐入口模块的 _cal / _cal_lock 同源对象）。
CalibrationController& controller(IpcClient& ipc) {
    static CalibrationController c(ipc);
    return c;
}

// 三字段信封（ok + data + detail），对齐 calib.py 的返回形状。
void send_calib(httplib::Response& res, int status, bool ok, const JsonValue& data,
                const std::string& detail) {
    JsonValue body = JsonValue::object();
    body.set("ok", JsonValue::boolean(ok));
    body.set("data", data);
    body.set("detail", JsonValue::string(detail));
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

// float(body[a] or body[b] or 0)；非法返回 false（对齐 calib.py 的 400 语义）。
bool parse_calib_num(const JsonValue& body, const char* a, const char* b, double* out) {
    const JsonValue& va = json_field(body, a);
    const JsonValue& vb = json_field(body, b);
    const JsonValue* chosen = nullptr;
    if (json_truthy(va)) chosen = &va;
    else if (json_truthy(vb)) chosen = &vb;
    else {
        *out = 0.0;
        return true;
    }
    if (chosen->is_number()) {
        *out = chosen->as_number();
        return true;
    }
    if (chosen->is_bool()) {
        *out = chosen->as_bool() ? 1.0 : 0.0;
        return true;
    }
    if (chosen->is_string()) {
        try {
            *out = std::stod(chosen->as_string());
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

}  // namespace

// 注册自动标定域路由：查询 / 手动写参 / 启动 / 取消 / 清除。
void register_calib_routes(httplib::Server& svr, IpcClient& ipc) {
    // GET /api/control/calibration：返回标定控制器完整 payload。
    svr.Get("/api/control/calibration",
            [&ipc](const httplib::Request&, httplib::Response& res) {
                send_calib(res, 200, true, controller(ipc).payload(), "");
            });

    // PUT /api/control/calibration：手动写增益/延迟（校验 > 0）并联动推导 PID。
    svr.Put("/api/control/calibration",
            [&ipc](const httplib::Request& req, httplib::Response& res) {
                const JsonValue body = parse_json_body(req);
                double gain_x = 0.0;
                double gain_y = 0.0;
                double delay = 0.0;
                if (!parse_calib_num(body, "gain_x_px_per_count", "mouse_gain_x_px_per_count",
                                     &gain_x) ||
                    !parse_calib_num(body, "gain_y_px_per_count", "mouse_gain_y_px_per_count",
                                     &gain_y) ||
                    !parse_calib_num(body, "response_delay_ms", "mouse_response_delay_ms",
                                     &delay)) {
                    send_json(res, 400, false, JsonValue::object(), "参数格式错误", "");
                    return;
                }
                if (gain_x <= 0 || gain_y <= 0) {
                    send_json(res, 400, false, JsonValue::object(), "增益必须 > 0", "");
                    return;
                }
                bool ok = false;
                std::string detail;
                const JsonValue data = controller(ipc).manual_update(gain_x, gain_y, delay, &ok,
                                                                      &detail);
                send_calib(res, ok ? 200 : 500, ok, data, detail);
            });

    // POST /api/control/calibration/start：前置校验后起标定线程（409 已在跑 / 400 前置不满足）。
    svr.Post("/api/control/calibration/start",
             [&ipc](const httplib::Request&, httplib::Response& res) {
                 std::string error;
                 const int status = controller(ipc).start(&error);
                 if (status == 409 || status == 400) {
                     send_json(res, status, false, JsonValue::object(), error, "");
                     return;
                 }
                 send_calib(res, 200, true, controller(ipc).payload(), "标定已启动");
             });

    // POST /api/control/calibration/cancel：请求取消当前标定。
    svr.Post("/api/control/calibration/cancel",
             [&ipc](const httplib::Request&, httplib::Response& res) {
                 controller(ipc).cancel();
                 send_calib(res, 200, true, controller(ipc).payload(), "标定已取消");
             });

    // DELETE /api/control/calibration：删除标定留档文件并回报最新 payload。
    svr.Delete("/api/control/calibration",
               [&ipc](const httplib::Request&, httplib::Response& res) {
                   controller(ipc).clear();
                   send_calib(res, 200, true, controller(ipc).payload(), "标定已清除");
               });
}

}  // namespace ttbox::core::web
