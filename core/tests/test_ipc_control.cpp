// test_ipc_control.cpp — Phase 1 新增 IPC 消息验收：SET_CONFIG / RUNTIME_CONTROL。
//
// SET_CONFIG 事务序：params.profile 校验 → 持久化成功 → 发布运行配置。
//   - 合法 profile → status=0, applied=true, persisted=true
//   - 非法 profile（confidence 越界）→ status=1 + 明确 error，运行配置不被污染
//   - 缺 params.profile → status=1
//   - 未注册 handler → status=3
//   - 落盘失败 → status=1，内存与磁盘均保持旧值
//   - GET_CONFIG 兼容性：SET 后读回的是新配置
// RUNTIME_CONTROL：
//   - start/stop/restart → status=0 且 handler 收到正确 action
//   - 非法 action → status=1
//   - 未注册 handler → status=3
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "common/Json.hpp"
#include "ipc/IpcServer.hpp"
#include "model/RuntimeProfile.hpp"
// T1.15：线格式契约用例用 input_pass_mode_name()/tensor_*_name() 取名字（不硬编码），
// 使"名字函数"与"IPC 线格式"共用一份真值；该头文件零 rknn_api.h 依赖，host 可编。
#include "rknn/InputQuant.hpp"
#include "test_util.hpp"

namespace {

std::string tmp_socket_path2() {
#if defined(_WIN32)
    return "tcp:0";  // 临时端口：OS 分配唯一端口（见 test_ipc.cpp 同注释）
#else
    return "/tmp/ttbox_core_test_ctrl_" + std::to_string(static_cast<long>(::getpid())) + ".sock";
#endif
}

// Windows TIME_WAIT 下固定端口偶发 bind 失败：带重试的启动（总等待 ~1s）
static bool start_with_retry(ttbox::core::IpcServer& server, std::string* error = nullptr) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (server.start(tmp_socket_path2(), error)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

// 记录 handler 调用的轻量夹具：镜像 Application::handle_config_update 的真实原子序
// （from_json → validate → 收/拒），profile 只在完全通过后才写入 last（可见污染语义）。
struct ConfigFixture {
    
ttbox::core::JsonValue last = ttbox::core::JsonValue::object();
    bool reject = false;
    std::string reject_reason;
    bool persist_ok = true;
    bool called = false;
    bool validate_enabled = true;  // 打开后走真实 RuntimeProfile 校验（与产品一致）

    bool handle(const ttbox::core::JsonValue& profile, std::string* error, bool* persisted) {
        called = true;
        if (validate_enabled) {
            ttbox::core::RuntimeProfile rp = ttbox::core::RuntimeProfile::from_json(profile);
            std::string verr;
            if (!rp.validate(&verr)) {
                if (error) *error = verr.empty() ? "profile 校验失败" : ("profile 校验失败: " + verr);
                if (persisted) *persisted = false;
                return false;  // 校验失败：last 不被写入（不污染）
            }
        }
        if (reject) {
            if (error) *error = reject_reason;
            if (persisted) *persisted = false;
            return false;
        }
        if (!persist_ok) {
            if (error) *error = "模拟持久化失败";
            if (persisted) *persisted = false;
            return false;
        }
        last = profile;
        if (persisted) *persisted = true;
        return true;
    }
};

struct RuntimeFixture {
    std::vector<std::string> actions;
    bool ok = true;
    std::string fail_on;
    std::string fail_reason = "simulated start failure";

    bool handle(const std::string& action, std::string* error) {
        actions.push_back(action);
        if (!ok && action == fail_on) {
            if (error) *error = fail_reason;
            return false;
        }
        return true;
    }
};

ttbox::core::JsonValue make_profile(double confidence, bool enabled) {
    ttbox::core::JsonValue p = ttbox::core::JsonValue::object();
    p.set("model_id", ttbox::core::JsonValue::string(""));
    ttbox::core::JsonValue inf = ttbox::core::JsonValue::object();
    inf.set("confidence", ttbox::core::JsonValue::number(confidence));
    inf.set("iou", ttbox::core::JsonValue::number(0.45));
    inf.set("class_filter", ttbox::core::JsonValue::array());
    inf.set("max_detections", ttbox::core::JsonValue::number(20));
    p.set("inference", inf);
    ttbox::core::JsonValue m = ttbox::core::JsonValue::object();
    m.set("enabled", ttbox::core::JsonValue::boolean(enabled));
    m.set("aim_hotkey", ttbox::core::JsonValue::number(2));
    m.set("aim_hotkey2", ttbox::core::JsonValue::number(0));
    m.set("aim_hotkey_mode", ttbox::core::JsonValue::string("any"));
    p.set("mouse", m);
    ttbox::core::JsonValue fov = ttbox::core::JsonValue::object();
    fov.set("enabled", ttbox::core::JsonValue::boolean(false));
    fov.set("shape", ttbox::core::JsonValue::number(0));
    fov.set("radius", ttbox::core::JsonValue::number(0.5));
    fov.set("center_x", ttbox::core::JsonValue::number(0.5));
    fov.set("center_y", ttbox::core::JsonValue::number(0.5));
    p.set("fov", fov);
    return p;
}

std::string set_config_request(const std::string& profile_json) {
    return R"({"type":"SET_CONFIG","params":{"profile":)" + profile_json + R"(}})";
}

}  // namespace

TEST(ipc_set_config_ok) {
    ttbox::core::IpcServer server;
    ConfigFixture fx;
    server.set_config_update_handler(
        [&fx](const ttbox::core::JsonValue& p, std::string* e, bool* persisted) {
            return fx.handle(p, e, persisted);
        });
    std::string error;
    CHECK(start_with_retry(server, &error));

    // 合法 profile（confidence=0.3, enabled=true）
    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(),
          set_config_request(make_profile(0.3, true).dump()), response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 0);
        const auto* data_v = parsed.value.find("data");
        CHECK(data_v != nullptr);
        if (data_v) {
            const auto* applied = data_v->find("applied");
            const auto* persisted = data_v->find("persisted");
            CHECK(applied != nullptr && applied->as_bool() == true);
            CHECK(persisted != nullptr && persisted->as_bool() == true);
        }
    }
    CHECK(fx.called);
    // handler 收到的 profile 内容与请求一致
    const auto* conf = fx.last.find("inference");
    CHECK(conf != nullptr);
    if (conf) {
        const auto* c = conf->find("confidence");
        CHECK(c != nullptr && c->as_number() == 0.3);
    }
    server.stop();
}

TEST(ipc_set_config_invalid_rejected_without_pollution) {
    ttbox::core::IpcServer server;
    ConfigFixture fx;
    server.set_config_update_handler(
        [&fx](const ttbox::core::JsonValue& p, std::string* e, bool* persisted) {
            return fx.handle(p, e, persisted);
        });
    std::string error;
    CHECK(start_with_retry(server, &error));

    // 非法 profile：confidence = 1.5 越界（RuntimeProfile::validate 会拒绝）
    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(),
          set_config_request(make_profile(1.5, true).dump()), response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 1);  // BAD_REQUEST
        const auto* err_v = parsed.value.find("error");
        CHECK(err_v != nullptr && err_v->as_string().find("confidence") != std::string::npos);
    }
    CHECK(fx.called);                        // handler 被触达并拒绝
    CHECK(!fx.last.is_object() || fx.last.as_object().empty());  // 运行配置未被污染
    server.stop();
}

TEST(ipc_set_config_missing_profile_bad_request) {
    ttbox::core::IpcServer server;
    ConfigFixture fx;
    server.set_config_update_handler(
        [&fx](const ttbox::core::JsonValue& p, std::string* e, bool* persisted) {
            return fx.handle(p, e, persisted);
        });
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(), R"({"type":"SET_CONFIG"})",
          response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 1);
    }
    CHECK(!fx.called);  // 缺参时不应触达 handler
    server.stop();
}

TEST(ipc_set_config_no_handler_internal) {
    ttbox::core::IpcServer server;  // 不注册 handler
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(),
          set_config_request(make_profile(0.3, true).dump()), response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 3);  // INTERNAL
    }
    server.stop();
}

TEST(ipc_set_config_persist_failure_rejected_without_apply) {
    ttbox::core::IpcServer server;
    ConfigFixture fx;
    fx.persist_ok = false;  // 模拟落盘失败
    server.set_config_update_handler(
        [&fx](const ttbox::core::JsonValue& p, std::string* e, bool* persisted) {
            return fx.handle(p, e, persisted);
        });
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(),
          set_config_request(make_profile(0.3, true).dump()), response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 1);
        const auto* err_v = parsed.value.find("error");
        CHECK(err_v != nullptr && err_v->as_string().find("持久化失败") != std::string::npos);
    }
    CHECK(!fx.last.is_object() || fx.last.as_object().empty());
    server.stop();
}

TEST(ipc_set_config_get_config_roundtrip) {
    // 兼容性：SET_CONFIG 之后 GET_CONFIG 读回的是新配置（同一份 handler 存储）。
    ttbox::core::IpcServer server;
    ConfigFixture fx;
    server.set_config_update_handler(
        [&fx](const ttbox::core::JsonValue& p, std::string* e, bool* persisted) {
            return fx.handle(p, e, persisted);
        });
    server.set_config_provider([&fx] { return fx.last; });
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(),
          set_config_request(make_profile(0.77, false).dump()), response, 2000, &error));
    // GET_CONFIG 读回
    CHECK(ttbox::core::ipc_request(server.socket_path(), R"({"type":"GET_CONFIG"})",
          response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* data_v = parsed.value.find("data");
        CHECK(data_v != nullptr);
        if (data_v) {
            const auto* inf = data_v->find("inference");
            CHECK(inf != nullptr);
            if (inf) {
                const auto* c = inf->find("confidence");
                CHECK(c != nullptr && c->as_number() == 0.77);
            }
        }
    }
    server.stop();
}

TEST(ipc_runtime_control_ok) {
    ttbox::core::IpcServer server;
    RuntimeFixture fx;
    server.set_runtime_control_handler(
        [&fx](const std::string& action, std::string* e) { return fx.handle(action, e); });
    std::string error;
    CHECK(start_with_retry(server, &error));

    for (const char* action : {"start", "stop", "restart"}) {
        std::string req = std::string(R"({"type":"RUNTIME_CONTROL","params":{"action":")") +
                          action + R"("}})";
        std::string response;
        CHECK(ttbox::core::ipc_request(server.socket_path(), req, response, 2000, &error));
        auto parsed = ttbox::core::json_parse(response);
        CHECK(parsed.ok);
        if (parsed.ok) {
            const auto* status_v = parsed.value.find("status");
            CHECK(status_v != nullptr && status_v->as_int() == 0);
            const auto* data_v = parsed.value.find("data");
            if (data_v) {
                const auto* a = data_v->find("action");
                CHECK(a != nullptr && a->as_string() == action);
            }
        }
    }
    CHECK_EQ(fx.actions.size(), static_cast<size_t>(3));
    server.stop();
}

TEST(ipc_runtime_control_invalid_action) {
    ttbox::core::IpcServer server;
    RuntimeFixture fx;
    server.set_runtime_control_handler(
        [&fx](const std::string& action, std::string* e) { return fx.handle(action, e); });
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(),
          R"({"type":"RUNTIME_CONTROL","params":{"action":"explode"}})",
          response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 1);  // BAD_REQUEST
    }
    CHECK(fx.actions.empty());
    server.stop();
}

TEST(ipc_runtime_control_no_handler_internal) {
    ttbox::core::IpcServer server;  // 不注册 handler
    std::string error;
    CHECK(start_with_retry(server, &error));

    std::string response;
    CHECK(ttbox::core::ipc_request(server.socket_path(),
          R"({"type":"RUNTIME_CONTROL","params":{"action":"start"}})",
          response, 2000, &error));
    auto parsed = ttbox::core::json_parse(response);
    CHECK(parsed.ok);
    if (parsed.ok) {
        const auto* status_v = parsed.value.find("status");
        CHECK(status_v != nullptr && status_v->as_int() == 3);
    }
    server.stop();
}

// ===========================================================================
// T1.15：GET_STATUS.metrics 的「模型输入通路」字段 —— 线格式契约锁（host 可跑）
//
// 为什么直接调 system_status_to_json 而不起 server：
//   本用例要锁的是**键名与类型**，不是 socket 行为（socket 已由上方用例覆盖）。
//   直接构造 SystemStatus ⇒ 无端口/时序依赖，失败信息直接指向"哪个键没了/类型变了"。
//
// 与 Web 侧的闭环：plugins/web/tests/test_web_model_input.py 的
//   core_metric_key_names_are_locked 消费的就是这里断言的键名；两处合起来 =
//   "core 序列化 ⇄ Web 消费"的双向锁。任一侧改名，两侧之一必红。
//
// 为什么断言默认值（而不是只断言我塞进去的值）：
//   面板在 Core 未运行时会读到默认值。若默认从 "unknown" 变成 "compatible"，
//   面板会把"没数据"谎报成"已判定为慢路径" —— 这正是本特性要消灭的盲区。
// ===========================================================================
TEST(ipc_get_status_metrics_model_input_wire_contract) {
    using namespace ttbox::core;

    SystemStatus st;
    auto& pm = st.metrics;
    // 名字一律走纯函数取（而不是硬编码字符串）⇒ 名字函数一改，本用例随契约一起红
    pm.model_input_pass_mode = input_pass_mode_name(InputPassMode::kXorShift128);
    pm.model_input_type = kTensorTypeInt8;
    pm.model_input_type_name = tensor_type_name(kTensorTypeInt8);
    pm.model_input_fmt = kTensorFmtNhwc;
    pm.model_input_fmt_name = tensor_fmt_name(kTensorFmtNhwc);
    pm.model_input_qnt_type = kQntAffineAsym;
    pm.model_input_qnt_name = tensor_qnt_type_name(kQntAffineAsym);
    pm.model_input_zp = -128;
    pm.model_input_scale = 1.0 / 255.0;
    pm.model_input_width = 640;
    pm.model_input_height = 640;
    pm.model_zero_copy_ready = true;
    pm.model_fast_path_active = true;
    pm.model_external_dma_requested = false;
    pm.model_external_dma_bound = false;
    pm.model_workers_total = 3;
    pm.model_workers_zero_copy = 3;
    pm.model_workers_fast_path = 2;
    pm.model_input_note = "回落说明（中文必须原样过线）";

    const JsonValue data = system_status_to_json(st);
    const JsonValue* m = data.find("metrics");
    CHECK(m != nullptr);
    if (m != nullptr) {
        const JsonValue* v = m->find("model_input_pass_mode");
        CHECK(v != nullptr && v->as_string() == "xor_shift128");
        const JsonValue* ty = m->find("model_input_type");
        CHECK(ty != nullptr && ty->as_number() == 2.0);
        const JsonValue* tyn = m->find("model_input_type_name");
        CHECK(tyn != nullptr && tyn->as_string() == "int8");
        const JsonValue* fm = m->find("model_input_fmt");
        CHECK(fm != nullptr && fm->as_number() == 1.0);
        const JsonValue* fmn = m->find("model_input_fmt_name");
        CHECK(fmn != nullptr && fmn->as_string() == "nhwc");
        const JsonValue* qt = m->find("model_input_qnt_type");
        CHECK(qt != nullptr && qt->as_number() == 2.0);
        const JsonValue* qtn = m->find("model_input_qnt_name");
        CHECK(qtn != nullptr && qtn->as_string() == "affine_asymmetric");
        const JsonValue* zp = m->find("model_input_zp");
        CHECK(zp != nullptr && zp->as_number() == -128.0);
        const JsonValue* sc = m->find("model_input_scale");
        CHECK(sc != nullptr && sc->as_number() == 1.0 / 255.0);
        const JsonValue* mw = m->find("model_input_width");
        CHECK(mw != nullptr && mw->as_number() == 640.0);
        const JsonValue* mh = m->find("model_input_height");
        CHECK(mh != nullptr && mh->as_number() == 640.0);
        const JsonValue* zc = m->find("model_zero_copy_ready");
        CHECK(zc != nullptr && zc->as_bool() == true);
        const JsonValue* fp = m->find("model_fast_path_active");
        CHECK(fp != nullptr && fp->as_bool() == true);
        const JsonValue* dr = m->find("model_external_dma_requested");
        CHECK(dr != nullptr && dr->as_bool() == false);
        const JsonValue* db = m->find("model_external_dma_bound");
        CHECK(db != nullptr && db->as_bool() == false);
        const JsonValue* wt = m->find("model_workers_total");
        CHECK(wt != nullptr && wt->as_number() == 3.0);
        const JsonValue* wz = m->find("model_workers_zero_copy");
        CHECK(wz != nullptr && wz->as_number() == 3.0);
        const JsonValue* wf = m->find("model_workers_fast_path");
        CHECK(wf != nullptr && wf->as_number() == 2.0);
        const JsonValue* nt = m->find("model_input_note");
        CHECK(nt != nullptr && nt->as_string() == "回落说明（中文必须原样过线）");
    }

    // 过线往返：dump() → parse 后中文说明必须逐字回来（防序列化把非 ASCII 写坏）
    const JsonValue data2 = system_status_to_json(st);
    const auto reparsed = json_parse(data2.dump());
    CHECK(reparsed.ok);
    if (reparsed.ok) {
        const JsonValue* m2 = reparsed.value.find("metrics");
        CHECK(m2 != nullptr);
        if (m2 != nullptr) {
            const JsonValue* nt2 = m2->find("model_input_note");
            CHECK(nt2 != nullptr && nt2->as_string() == "回落说明（中文必须原样过线）");
            const JsonValue* p2 = m2->find("model_input_pass_mode");
            CHECK(p2 != nullptr && p2->as_string() == "xor_shift128");
        }
    }

    // 默认值（Core 未运行 / 未取到）：必须 unknown / -1 / 0 / false，**绝不**默认 compatible
    const SystemStatus blank;
    const JsonValue blank_data = system_status_to_json(blank);
    const JsonValue* bm = blank_data.find("metrics");
    CHECK(bm != nullptr);
    if (bm != nullptr) {
        const JsonValue* bp = bm->find("model_input_pass_mode");
        CHECK(bp != nullptr && bp->as_string() == "unknown");
        const JsonValue* bt = bm->find("model_input_type");
        CHECK(bt != nullptr && bt->as_number() == -1.0);
        const JsonValue* bn = bm->find("model_input_type_name");
        CHECK(bn != nullptr && bn->as_string() == "unknown");
        const JsonValue* bf = bm->find("model_fast_path_active");
        CHECK(bf != nullptr && bf->as_bool() == false);
        const JsonValue* bw = bm->find("model_workers_total");
        CHECK(bw != nullptr && bw->as_number() == 0.0);
        const JsonValue* bnote = bm->find("model_input_note");
        CHECK(bnote != nullptr && bnote->as_string().empty());
    }
}
