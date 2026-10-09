// test_preprocess_metric.cpp — 面板「预处理路径 / 推理并发」后端真源回归
//
// 为什么需要这个测试（真实的翻车经过）：
//   plugins/web/templates/index.html 的「预处理路径」那一格读 `raw_preprocess_backend`，
//   但 core 侧**从来没有产出过这个键** ⇒ 前端恒显示 "-"，业主看到的是"没有预处理数据"。
//   「存在性断言 ≠ 生效」的又一次：格子存在 ≠ 数据存在。
//   本测试钉死"序列化产物里必须有这些键"，防止再次出现前端读了、后端没供的情况。
//
// 另一个被本测试覆盖的口径：`preprocess_to_track_ms` 曾在 web 侧拿 e2e_ms 顶替，
//   那是端到端不是"预处理→跟踪"。真值应由 resize+infer+decode 合成（web 侧已改）。
#include <cstdio>
#include <string>

#include "common/Metrics.hpp"
#include "ipc/IpcServer.hpp"

using namespace ttbox::core;

int main() {
    int fails = 0;
    auto check = [&fails](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++fails;
    };

    SystemStatus st{};
    st.running = true;
    st.metrics.preprocess_backend = "rga";
    st.metrics.preprocess_error = "";
    st.metrics.inference_capacity_fps = 492.0;
    st.metrics.model_workers_total = 3;
    st.metrics.resize_ms = 0.21;
    st.metrics.infer_run_ms = 5.75;
    st.metrics.decode_ms = 0.07;

    // system_status_to_json 返回的就是 data 本体（IPC 层再包 {"data":...,"status":...}）
    const JsonValue root = system_status_to_json(st);
    check(root.find("running") != nullptr, "status JSON 是 data 本体（含 running）");
    const JsonValue* m = root.find("metrics");
    check(m != nullptr, "status JSON 含 metrics");
    if (m == nullptr) return 1 + fails;

    // ① 预处理后端键必须存在且取到真值（此前后端从不产出 ⇒ 面板恒 "-")
    const JsonValue* backend = m->find("raw_preprocess_backend");
    check(backend != nullptr, "metrics 含 raw_preprocess_backend 键");
    check(backend != nullptr && backend->as_string("") == "rga",
          "raw_preprocess_backend 取到真值 rga");

    // ② 失败原因键也要存在（空串合法：正常运行就是没错误）
    const JsonValue* perr = m->find("raw_preprocess_error");
    check(perr != nullptr, "metrics 含 raw_preprocess_error 键");
    check(perr != nullptr && perr->as_string("x") == "", "raw_preprocess_error 默认空串");

    // ③ 并发吞吐：3 路 ÷ e2e 6.10ms ≈ 492 fps
    const JsonValue* cap = m->find("inference_capacity_fps");
    check(cap != nullptr, "metrics 含 inference_capacity_fps 键");
    const double cap_v = cap ? cap->as_number(0.0) : 0.0;
    check(cap_v > 400.0 && cap_v < 600.0, "inference_capacity_fps 落在合理区间（约 492）");

    // ④ 前端合成 preprocess_to_track_ms 的三个分量必须都存在，否则合成值会退化成 0
    check(m->find("resize_ms") != nullptr, "metrics 含 resize_ms（合成延迟的分量）");
    check(m->find("infer_run_ms") != nullptr, "metrics 含 infer_run_ms（合成延迟的分量）");
    check(m->find("decode_ms") != nullptr, "metrics 含 decode_ms（合成延迟的分量）");

    std::printf("\n%s (%d 失败)\n", fails == 0 ? "ALL PASS" : "HAS FAILURE", fails);
    return fails == 0 ? 0 : 1;
}
