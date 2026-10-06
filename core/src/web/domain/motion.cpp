// motion.cpp — 运动训练/个人运动模型域（13 条路由，URL 一字未改）。
//
// 自 plugins/web/api/motion.py 逐行移植。领域逻辑在 infra/motion_store.*。
#include "web/domain/domain_routes.hpp"

#include <string>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/calibration_controller.hpp"  // config_write_lock()
#include "web/infra/ipc_client.hpp"
#include "web/infra/motion_store.hpp"

namespace ttbox::core::web {

namespace {

// 运行期共享单例（对齐入口模块的 MOTION_STORE）。
MotionProfileStore& store() {
    static MotionProfileStore s(motion_profiles_dir());
    return s;
}

// 对齐 api/motion.py::_motion_error：消息含 session/active/lease → 409，否则 422。
void motion_error(httplib::Response& res, const std::string& message) {
    const bool conflict = message.find("session") != std::string::npos ||
                          message.find("active") != std::string::npos ||
                          message.find("lease") != std::string::npos;
    send_json(res, conflict ? 409 : 422, false, JsonValue::object(), message, "");
}

// 把 TTBOX 个人模型启用状态写入 Core RuntimeProfile（读-改-写持配置锁）。
void apply_personal_motion_to_core(IpcClient& ipc, bool enabled, const std::string& profile_id,
                                   const JsonValue& mix_arg) {
    std::lock_guard<std::recursive_mutex> lk(config_write_lock());
    JsonValue prof;
    std::string err;
    if (!get_runtime_profile(ipc, &prof, &err)) {
        throw MotionError("读取 TTBOX Core RuntimeProfile 失败");
    }
    JsonValue mo = json_field(prof, "mouse").is_object() ? json_field(prof, "mouse")
                                                         : JsonValue::object();
    JsonValue personal = json_field(mo, "personal_motion").is_object()
                             ? json_field(mo, "personal_motion")
                             : JsonValue::object();
    personal.set("enabled", JsonValue::boolean(enabled));
    if (enabled) {
        const JsonValue profile = store().list_profile(profile_id);
        const JsonValue& model = json_field(profile, "model");
        if (!json_truthy(json_field(model, "ready"))) {
            throw MotionError("model is not ready");
        }
        const JsonValue values =
            (mix_arg.is_object() && !mix_arg.as_object().empty()) ? mix_arg : store().mix();
        personal.set("curve_blend", JsonValue::number(json_field(values, "curve").as_number(1.0)));
        personal.set("speed_blend", JsonValue::number(json_field(values, "speed").as_number(1.0)));
        personal.set("reaction_blend",
                     JsonValue::number(json_field(values, "reaction").as_number(0.7)));
        personal.set("max_reaction_delay_ms",
                     JsonValue::number(json_field(values, "max_reaction_delay_ms").as_number(250.0)));
        const JsonValue& knots = json_field(model, "knots");
        personal.set("knots", knots.is_array() ? knots : JsonValue::array());
    }
    mo.set("personal_motion", std::move(personal));
    prof.set("mouse", std::move(mo));
    JsonValue params = JsonValue::object();
    params.set("profile", prof);
    const JsonValue result = ipc.call("SET_CONFIG", params, kIpcTimeoutDefaultMs);
    if (ipc_status(result) != 0) {
        throw MotionError(json_field(result, "error").as_string("Core 配置更新失败"));
    }
}

}  // namespace

void register_motion_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/api/motion-profiles", [](const httplib::Request&, httplib::Response& res) {
        try {
            send_json(res, 200, true, store().list_profiles(), "", "");
        } catch (const MotionError& e) {
            motion_error(res, e.what());
        } catch (const std::exception& e) {
            send_json(res, 500, false, JsonValue::object(), e.what(), "");
        }
    });

    svr.Post("/api/motion-profiles", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, false, JsonValue::object(),
                  "only the internal default motion profile is supported", "");
    });

    svr.Patch(R"(/api/motion-profiles/([^/]+))",
              [](const httplib::Request& req, httplib::Response& res) {
                  const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                  const JsonValue body = parse_json_body(req);
                  try {
                      send_json(res, 200, true,
                                store().rename_profile(id, json_field(body, "name").as_string("")),
                                "", "");
                  } catch (const MotionError& e) {
                      motion_error(res, e.what());
                  } catch (const std::exception& e) {
                      send_json(res, 500, false, JsonValue::object(), e.what(), "");
                  }
              });

    // ★ 静态路由必须先于 <profile_id> 动态路由注册（httplib 按注册顺序首次匹配）。
    svr.Delete("/api/motion-profiles/active", [&ipc](const httplib::Request&,
                                                     httplib::Response& res) {
        try {
            const JsonValue result = store().deactivate();
            apply_personal_motion_to_core(ipc, false, "", JsonValue::null());
            send_json(res, 200, true, result, "", "");
        } catch (const MotionError& e) {
            motion_error(res, e.what());
        } catch (const std::exception& e) {
            send_json(res, 500, false, JsonValue::object(), e.what(), "");
        }
    });

    svr.Delete(R"(/api/motion-profiles/([^/]+))",
               [](const httplib::Request& req, httplib::Response& res) {
                   const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                   try {
                       send_json(res, 200, true, store().remove_profile(id), "", "");
                   } catch (const MotionError& e) {
                       motion_error(res, e.what());
                   } catch (const std::exception& e) {
                       send_json(res, 500, false, JsonValue::object(), e.what(), "");
                   }
               });

    svr.Get(R"(/api/motion-profiles/([^/]+)/export)",
            [](const httplib::Request& req, httplib::Response& res) {
                const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                try {
                    const JsonValue profile = store().list_profile(id);
                    const std::string export_path =
                        join_path(motion_profiles_dir(), ".motion-profile-" + id + ".json");
                    write_file(export_path, profile.dump());  // 对齐 Python 落盘副作用
                    res.status = 200;
                    res.set_content(profile.dump(), "application/json");
                    res.set_header("Content-Disposition",
                                   "attachment; filename=\"ttbox-motion-profile-" + id + ".json\"");
                } catch (const MotionError& e) {
                    motion_error(res, e.what());
                } catch (const std::exception& e) {
                    send_json(res, 500, false, JsonValue::object(), e.what(), "");
                }
            });

    svr.Post("/api/motion-training/sessions",
             [](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 try {
                     const JsonValue result = store().start_session(
                         json_field(body, "profile_id").as_string(""), now_seconds());
                     JsonValue data = JsonValue::object();
                     data.set("session_id", json_field(result, "id"));
                     data.set("profile_id", json_field(result, "profile_id"));
                     data.set("lease_expires_at", json_field(result, "lease_expires_at"));
                     send_json(res, 200, true, data, "", "");
                 } catch (const MotionError& e) {
                     motion_error(res, e.what());
                 } catch (const std::exception& e) {
                     send_json(res, 500, false, JsonValue::object(), e.what(), "");
                 }
             });

    svr.Put(R"(/api/motion-training/sessions/([^/]+)/heartbeat)",
            [](const httplib::Request& req, httplib::Response& res) {
                const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                try {
                    send_json(res, 200, true, store().heartbeat(id, now_seconds()), "", "");
                } catch (const MotionError& e) {
                    motion_error(res, e.what());
                } catch (const std::exception& e) {
                    send_json(res, 500, false, JsonValue::object(), e.what(), "");
                }
            });

    svr.Post(R"(/api/motion-training/sessions/([^/]+)/samples)",
             [](const httplib::Request& req, httplib::Response& res) {
                 const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                 const JsonValue body = parse_json_body(req);
                 try {
                     send_json(res, 200, true,
                               store().append_sample(id, body, now_seconds()), "", "");
                 } catch (const MotionError& e) {
                     motion_error(res, e.what());
                 } catch (const std::exception& e) {
                     send_json(res, 500, false, JsonValue::object(), e.what(), "");
                 }
             });

    svr.Delete(R"(/api/motion-training/sessions/([^/]+))",
               [](const httplib::Request& req, httplib::Response& res) {
                   const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                   try {
                       send_json(res, 200, true, store().stop_session(id, now_seconds()), "", "");
                   } catch (const MotionError& e) {
                       motion_error(res, e.what());
                   } catch (const std::exception& e) {
                       send_json(res, 500, false, JsonValue::object(), e.what(), "");
                   }
               });

    svr.Post(R"(/api/motion-profiles/([^/]+)/train)",
             [](const httplib::Request& req, httplib::Response& res) {
                 const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                 try {
                     send_json(res, 200, true, store().train(id), "", "");
                 } catch (const MotionError& e) {
                     motion_error(res, e.what());
                 } catch (const std::exception& e) {
                     send_json(res, 500, false, JsonValue::object(), e.what(), "");
                 }
             });

    svr.Post(R"(/api/motion-profiles/([^/]+)/activate)",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                 const JsonValue body = parse_json_body(req);
                 try {
                     const JsonValue result = store().activate(id, body);
                     apply_personal_motion_to_core(ipc, true, id, json_field(result, "mix"));
                     send_json(res, 200, true, result, "", "");
                 } catch (const MotionError& e) {
                     motion_error(res, e.what());
                 } catch (const std::exception& e) {
                     send_json(res, 500, false, JsonValue::object(), e.what(), "");
                 }
             });

    svr.Delete(R"(/api/motion-profiles/([^/]+)/samples)",
               [](const httplib::Request& req, httplib::Response& res) {
                   const std::string id = req.matches.size() > 1 ? req.matches[1].str() : "";
                   try {
                       send_json(res, 200, true, store().clear_samples(id), "", "");
                   } catch (const MotionError& e) {
                       motion_error(res, e.what());
                   } catch (const std::exception& e) {
                       send_json(res, 500, false, JsonValue::object(), e.what(), "");
                   }
               });
}

}  // namespace ttbox::core::web
