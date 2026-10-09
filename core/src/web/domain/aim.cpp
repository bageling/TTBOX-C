// aim.cpp — 瞄准轨迹域（1 条路由，URL 一字未改）。
//
// 自 plugins/web/api/aim.py 逐行为移植。板端调试与压枪自整定取证用：起后台线程
// 采样 GET_STATUS.metrics 的瞄准误差/移动量，结束落 run/aim_trace.json。
#include "web/domain/domain_routes.hpp"

#include <cmath>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/domain/sysinfo_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// 共享轨迹态（对齐入口模块的 _aim_trace dict）。
struct AimTraceState {
    std::mutex mu;
    bool running = false;
    std::vector<JsonValue> samples;
    double started_at = 0.0;
    double stop_at = 0.0;
};
// 取全局单例轨迹态（函数局部静态，跨请求共享）。
AimTraceState& aim_trace() {
    static AimTraceState s;
    return s;
}

// 保留 3 位小数（轨迹采样数值统一精度）。
double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

// 本地时间戳 %Y%m%d_%H%M%S（给轨迹文件名用）。
std::string timestamp_name() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// 采样线程（对齐 aim.py::_collect）：循环 GET_STATUS.metrics，结束落盘。
void aim_collect(IpcClient* ipc, int duration_sec, double started_at, double stop_at) {
    AimTraceState& st = aim_trace();
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(st.mu);
            if (!st.running) break;
        }
        const double now = now_seconds();
        if (now >= stop_at) break;
        try {
            const JsonValue status = get_status_data(*ipc);
            const JsonValue m = json_field(status, "metrics");
            JsonValue sample = JsonValue::object();
            sample.set("t", JsonValue::number(round3(now - started_at)));
            sample.set("err_x", JsonValue::number(round3(mnum(m, "aim_error_x"))));
            sample.set("err_y", JsonValue::number(round3(mnum(m, "aim_error_y"))));
            sample.set("move_x", JsonValue::number(mnum(m, "mouse_dx")));
            sample.set("move_y", JsonValue::number(mnum(m, "mouse_dy")));
            sample.set("target", JsonValue::boolean(mint(m, "target_frames") > 0 ||
                                                   mbool(m, "aim_active")));
            std::lock_guard<std::mutex> lk(st.mu);
            st.samples.push_back(std::move(sample));
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 落盘（对齐 aim.py：写 run/aim_trace.json，返回名另取 .jsonl）。
    const std::string run_dir = join_path(ttbox_prefix(), "run");
    std::error_code ec;
    std::filesystem::create_directories(run_dir, ec);
    JsonValue doc = JsonValue::object();
    {
        std::lock_guard<std::mutex> lk(st.mu);
        JsonValue arr = JsonValue::array();
        for (const JsonValue& s : st.samples) arr.push_back(s);
        doc.set("samples", std::move(arr));
        doc.set("duration_sec", JsonValue::number(static_cast<double>(duration_sec)));
    }
    write_file(join_path(run_dir, "aim_trace.json"), doc.dump());
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.running = false;
    }
}

}  // namespace

// 注册瞄准轨迹路由：POST /api/diagnostics/aim-trace（起后台采样线程）。
void register_aim_routes(httplib::Server& svr, IpcClient& ipc) {
    // 开始一段限时轨迹记录：校验时长/是否已在录/推理是否在跑，然后起采样线程。
    svr.Post("/api/diagnostics/aim-trace",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 const int duration_sec =
                     static_cast<int>(json_field(body, "duration_sec").as_int(10));
                 if (duration_sec <= 0 || duration_sec > 120) {
                     send_json(res, 400, false, JsonValue::object(),
                               "duration_sec 必须在 1~120 之间", "");
                     return;
                 }
                 AimTraceState& st = aim_trace();
                 {
                     std::lock_guard<std::mutex> lk(st.mu);
                     if (st.running) {
                         send_json(res, 400, false, JsonValue::object(), "已有轨迹记录进行中", "");
                         return;
                     }
                 }
                 // Core 未运行时拒绝开始（避免记录全 0 假轨迹）。
                 const JsonValue status = get_status_data(ipc);
                 if (!json_truthy(json_field(status, "runtime_running"))) {
                     send_json(res, 400, false, JsonValue::object(),
                               "推理服务未运行，无法记录瞄准轨迹", "");
                     return;
                 }
                 const double started_at = now_seconds();
                 {
                     std::lock_guard<std::mutex> lk(st.mu);
                     st.running = true;
                     st.samples.clear();
                     st.started_at = started_at;
                     st.stop_at = started_at + duration_sec;
                 }
                 std::thread(aim_collect, &ipc, duration_sec, started_at,
                             started_at + duration_sec)
                     .detach();
                 const std::string fname = "aim_trace_" + timestamp_name() + ".jsonl";
                 JsonValue data = JsonValue::object();
                 data.set("duration_sec", JsonValue::number(static_cast<double>(duration_sec)));
                 data.set("filename", JsonValue::string(fname));
                 data.set("path", JsonValue::string(
                     join_path(join_path(ttbox_prefix(), "run"), fname)));
                 data.set("recording", JsonValue::boolean(true));
                 send_json(res, 200, true, data, "", "");
             });
}

}  // namespace ttbox::core::web
