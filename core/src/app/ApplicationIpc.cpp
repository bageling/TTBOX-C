// ApplicationIpc.cpp — 应用 IPC 命令处理（11 个 handle_*）
//
// ★ 由 Application.cpp 按职责拆分而来（2026-10-04 core 结构治理 S1）。
//   拆分方式 = **只搬定义，不改任何逻辑**：成员函数仍是 Application 的同一个类的成员，
//   只是定义落在别的编译单元（类外定义可跨 TU 分散，这是 C++ 标准允许的）。
//   ⇒ 外部行为、符号名、ABI 全部不变；唯一变化是 .o 的组织方式。
//
// 本文件负责：应用 IPC 命令处理（11 个 handle_*）
// ★ 这 11 个函数是 IPC 命令表的实现：改一条命令只需要动这一个文件。
//   它们只**向下**依赖 runtime 组（persist_runtime_profile /
//   switch_active_model_runtime / persist_runtime_intent /
//   try_resume_from_degraded），不反向依赖 —— 依赖是单向的，无环。
#include "app/Application.hpp"
#include "app/ApplicationInternal.hpp"   // now_ms / incoming_dir_of（inline 共享）
#include "auth/LicenseGate.hpp"           // T1.10：唯一授权执法快照（会话边界 / IPC 投影）
#include "common/Json.hpp"                // JsonValue / json_parse_file / dump
#include "common/Logger.hpp"
#include "model/ModelManagement.hpp"
#include "model/ModelRegistry.hpp"

// V1.0.50 硬件写入处理器所需（fork/exec 脚本 + 原子写配置）
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using ttbox::core::app_internal::incoming_dir_of;

namespace ttbox::core {
using ttbox::core::app_internal::now_ms;

// SET_CONFIG 原子更新：
//   JSON 解析 → RuntimeProfile::validate → RuntimeConfig.update（内存原子替换）→ 持久化。
// 任何一步失败都直接返回 false；内存与磁盘均保证不被污染。
bool Application::handle_config_update(const JsonValue& profile_json, std::string* error,
                                       bool* persisted) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (persisted) *persisted = false;
    if (!profile_json.is_object()) {
        if (error) *error = "profile 必须是 JSON 对象";
        return false;
    }

    // 1) API 原始输入严格校验：from_json() 对“磁盘历史坏配置”会执行自愈，
    // 但 SET_CONFIG 是新写入请求，不能把非法 1×1 偷偷改成 0×0 后报告成功。
    // 必须在解析前检查原始 capture；0=全帧，非零下限与 RuntimeProfile 一致。
    if (const JsonValue* capture = profile_json.find("capture");
        capture && capture->is_object()) {
        // 常量单一权威源：ttbox::core::kMinCaptureRoiPx / kMaxCaptureRoiPx
        // （定义在 RuntimeProfile.hpp，DUP-10），与 RuntimeProfile::validate 共用。
        for (const char* key : {"width", "height"}) {
            if (const JsonValue* value = capture->find(key); value && value->is_number()) {
                const int64_t pixels = value->as_int();
                if (pixels != 0 &&
                    (pixels < kMinCaptureRoiPx || pixels > kMaxCaptureRoiPx)) {
                    if (error) {
                        *error = std::string("profile 校验失败: capture.") + key +
                                 " 非法（0=全帧，或需在 " + std::to_string(kMinCaptureRoiPx) +
                                 "~" + std::to_string(kMaxCaptureRoiPx) + " 之间）";
                    }
                    return false;
                }
            }
        }
    }

    // 2) 解析 canonical profile，并执行完整语义校验
    RuntimeProfile profile = RuntimeProfile::from_json(profile_json);
    std::string validate_error;
    if (!profile.validate(&validate_error)) {
        if (error) *error = validate_error.empty() ? "profile 校验失败" : ("profile 校验失败: " + validate_error);
        return false;
    }

    // model_id 代表真正完成 RKNN 加载与首帧验证的当前模型，只允许 MODEL_ACTIVATE
    // 事务修改。普通 SET_CONFIG 改它会绕过 ModelRegistry、切换和回滚门槛。
    if (auto current = runtime_config_.snapshot(); current && profile.model_id != current->model_id) {
        if (error) *error = "model_id 只能通过模型激活接口修改";
        return false;
    }

    // 3) 唯一提交入口在同一把锁内完成：canonical 落盘、ConfigManager 根节点刷新、
    // RuntimeConfig 内存发布。并发请求因此不会产生“磁盘最后是 B、内存最后是 A”。
    std::string persist_error;
    if (!persist_runtime_profile(profile, &persist_error)) {
        if (error) *error = "写入配置文件失败，现配置保持不变: " + persist_error;
        return false;
    }
    if (persisted) *persisted = true;
    return true;
}
// M2.02：ACTIVATE_LICENSE 的 Application 侧——激活 + Gate publish + wire 投影。
// 失败语义 = fail-closed：拒绝时（坏卡/他板卡/过期卡）**不 publish 投影数据**，
// 但 Gate 快照仍刷新（拒绝态也是真状态；AI 路径由 ai_allowed() 决定，恒不放开）。
bool Application::handle_license_activate(const std::string& card_envelope,
                                         JsonValue* data, std::string* error) {
    if (!license_daemon_) {
        if (error) *error = "license daemon 未初始化";
        return false;
    }
    std::string aerr;
    const bool ok = license_daemon_->activate(card_envelope, &aerr);
    // 无论成败：把最新授权态 publish 到 LicenseGate（唯一执法快照），
    // 使主循环自动拉起逻辑（run loop 的 ai_allowed 重查）立即看到新状态。
    auth::LicenseGate::instance().publish(auth::to_snapshot(
        license_daemon_->status_snapshot(), license_daemon_->store_load_result(),
        static_cast<int64_t>(now_ms())));
    if (!ok) {
        if (error) *error = aerr.empty() ? "激活被拒绝" : aerr;
        return false;
    }
    // wire 投影（§3.2 口径：state 小写名 + activated 派生 + 签名卡内容字段）
    if (data) {
        const auth::LicenseSnapshot ls = auth::LicenseGate::instance().snapshot();
        *data = JsonValue::object();
        data->set("state", JsonValue::string(auth::wire_state_name(ls)));
        data->set("activated", JsonValue::boolean(auth::wire_activated(ls)));
        data->set("plan", JsonValue::string(ls.plan));
        data->set("is_pro", JsonValue::boolean(ls.is_pro));
        data->set("ui_brand", JsonValue::string(ls.ui_brand));
        data->set("expire_unix_ms",
                  JsonValue::number(static_cast<double>(ls.expire_unix_ms)));
        JsonValue feats = JsonValue::array();
        for (const std::string& f : ls.features) {
            feats.push_back(JsonValue::string(f));
        }
        data->set("features", std::move(feats));
    }
    return true;
}
// ---------------------------------------------------------------------------
// M2.07：ACTIVATE_CLOUD 的 Application 侧（契约 §3.2）
// ---------------------------------------------------------------------------
// params: {
//   "expire_unix_ms": <ms, 必填, >now 否则 400>,
//   "features": ["capture","inference","aim","ota"],   // web 下发（D5 全闭集），
//                                                      // core normalize 收窄
//   "plan": "subscription",        // 可选，缺省 'none'
//   "card_mask": "LS-****-XXXX",   // 可选
//   "max_devices": 3,              // 可选，仅投影
//   "source": "cloud",             // 固定
//   "deactivate": false            // true ⇒ 立即锁定（快路径），不落盘
// }
// 成功 data 与 ACTIVATE_LICENSE 同形（state/activated/plan/is_pro/ui_brand/
// expire_unix_ms/features）。授权执法点（LicenseGate/FeatureGates）不变。
bool Application::handle_license_activate_cloud(const JsonValue& params,
                                                JsonValue* data, std::string* error) {
    if (!license_daemon_) {
        if (error) *error = "license daemon 未初始化";
        return false;
    }

    // ---- deactivate:true ⇒ 快路径锁定（D4；web 收到云端 403 时调用）----
    const JsonValue* deact_v = params.find("deactivate");
    if (deact_v != nullptr && deact_v->as_bool(false)) {
        std::string reason;
        if (const JsonValue* r = params.find("reason");
            r != nullptr && r->is_string()) {
            reason = r->as_string();
        }
        license_daemon_->deactivate_cloud(reason);
        // publish：锁定态立即进入唯一执法快照 → pipeline_allowed()=false → AI 停
        auth::LicenseGate::instance().publish(auth::to_snapshot(
            license_daemon_->status_snapshot(), license_daemon_->store_load_result(),
            static_cast<int64_t>(now_ms())));
        if (data) {
            const auth::LicenseSnapshot ls = auth::LicenseGate::instance().snapshot();
            *data = JsonValue::object();
            data->set("state", JsonValue::string(auth::wire_state_name(ls)));
            data->set("activated", JsonValue::boolean(auth::wire_activated(ls)));
            data->set("plan", JsonValue::string(ls.plan));
            data->set("is_pro", JsonValue::boolean(ls.is_pro));
            data->set("ui_brand", JsonValue::string(ls.ui_brand));
            data->set("expire_unix_ms",
                      JsonValue::number(static_cast<double>(ls.expire_unix_ms)));
            JsonValue feats = JsonValue::array();
            for (const std::string& f : ls.features) {
                feats.push_back(JsonValue::string(f));
            }
            data->set("features", std::move(feats));
        }
        return true;
    }

    // ---- 正常云激活 ----
    const JsonValue* expire_v = params.find("expire_unix_ms");
    if (expire_v == nullptr || !expire_v->is_number()) {
        if (error) *error = "缺少必需字段 'expire_unix_ms'（数字，Unix 毫秒）";
        return false;
    }
    const int64_t expire_unix_ms = expire_v->as_int(0);

    std::vector<std::string> features;
    if (const JsonValue* f = params.find("features");
        f != nullptr && f->is_array()) {
        for (const JsonValue& e : f->as_array()) {
            if (e.is_string()) features.push_back(e.as_string());
        }
    }
    std::string plan;
    if (const JsonValue* p = params.find("plan"); p != nullptr && p->is_string()) {
        plan = p->as_string();
    }
    std::string card_mask;
    if (const JsonValue* m = params.find("card_mask"); m != nullptr && m->is_string()) {
        card_mask = m->as_string();
    }

    std::string aerr;
    const bool ok = license_daemon_->activate_cloud(expire_unix_ms, features, plan,
                                                    card_mask, &aerr);
    // 无论成败：把最新授权态 publish 到 LicenseGate（唯一执法快照）。
    auth::LicenseGate::instance().publish(auth::to_snapshot(
        license_daemon_->status_snapshot(), license_daemon_->store_load_result(),
        static_cast<int64_t>(now_ms())));
    if (!ok) {
        if (error) *error = aerr.empty() ? "云端激活被拒绝" : aerr;
        return false;
    }
    // wire 投影（与 handle_license_activate 同形；max_devices 仅回显）
    if (data) {
        const auth::LicenseSnapshot ls = auth::LicenseGate::instance().snapshot();
        *data = JsonValue::object();
        data->set("state", JsonValue::string(auth::wire_state_name(ls)));
        data->set("activated", JsonValue::boolean(auth::wire_activated(ls)));
        data->set("plan", JsonValue::string(ls.plan));
        data->set("is_pro", JsonValue::boolean(ls.is_pro));
        data->set("ui_brand", JsonValue::string(ls.ui_brand));
        data->set("expire_unix_ms",
                  JsonValue::number(static_cast<double>(ls.expire_unix_ms)));
        JsonValue feats = JsonValue::array();
        for (const std::string& f : ls.features) {
            feats.push_back(JsonValue::string(f));
        }
        data->set("features", std::move(feats));
        if (const JsonValue* md = params.find("max_devices");
            md != nullptr && md->is_number()) {
            data->set("max_devices", JsonValue::number(md->as_number()));
        }
    }
    return true;
}
bool Application::handle_model_activate(const std::string& model_id, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    // 热切换语义：activate 只改注册表选中态；真正"切过去跑"由 switch_active_model_runtime
    // 完成（stop → 以新模型重建 Worker 参数 → start → 首帧真实推理通过才提交 running）。
    // 旧模型路径：runtime 切换失败时回滚 active.json 并恢复旧模型运行，当前推理不中断窗口由
    // stop→start 间隙承担（~1s，与进程级重启相比无 IPC/端口中断）。
    const std::string previous_active = model_management_->registry().active_model();
    if (previous_active == model_id && core_runtime_ && core_runtime_->running() &&
        running_model_id_ == model_id) {
        std::string sync_error;
        if (!sync_model_id_to_profile(model_id, &sync_error)) {
            if (error) *error = "模型已运行，但配置同步失败: " + sync_error;
            return false;
        }
        return true;  // 幂等：运行态与配置均已确认一致
    }
    // ★ 无画面预检：见 has_capture_signal 注释。HDMI 没接/信号源没开时，下面的
    //   switch_active_model_runtime 必然在首帧门槛超时（5s），而回滚路径要走同一条门槛，
    //   实测整个请求 ~10.7s 才返回一个看不出真因的失败。这里提前 200ms 判定并直接拒绝；
    //   此刻**尚未改动 registry 与 runtime**，调用方无需回滚，状态零副作用。
    if (!has_capture_signal()) {
        if (error) {
            *error = "未检测到画面输入（HDMI 无信号或信号源未输出）：采集持续 0 帧，"
                     "无法验证新模型，本次切换未执行。请先点亮信号源再试。";
        }
        return false;
    }
    if (!model_management_->registry().activate(model_id, error)) {
        return false;  // 激活校验失败：active 未变，无需回滚
    }
    if (!core_runtime_ || !core_runtime_->running()) {
        // runtime 未在跑（如 HDMI 断流后）：同样立即用新模型拉起，
        // 不留"选中未生效"的中间态（用户视角=切换必须真实生效）
        TTBOX_LOG_INFO("模型已激活，runtime 未运行 → 直接以新模型拉起: " + model_id);
        if (!switch_active_model_runtime(model_id, error)) {
            std::string switch_error = error && !error->empty() ? *error : "拉起失败";
            // 拉起失败：回滚 active 到旧模型（主循环会自动重试恢复）
            std::string rb_error;
            if (!previous_active.empty() && previous_active != model_id) {
                model_management_->registry().activate(previous_active, &rb_error);
            } else if (previous_active.empty()) {
                model_management_->registry().deactivate(&rb_error);
            }
            if (error) *error = "模型切换失败（runtime 未运行，拉起新模型失败已回滚）: " + switch_error;
            return false;
        }
        TTBOX_LOG_INFO("模型热切换完成（runtime 原本未运行）: running_model_id=" + running_model_id_);
        std::string sync_error;
        if (!sync_model_id_to_profile(model_id, &sync_error)) {
            std::string rb_error;
            if (!previous_active.empty() && previous_active != model_id) {
                model_management_->registry().activate(previous_active, &rb_error);
                if (switch_active_model_runtime(previous_active, &rb_error)) {
                    sync_model_id_to_profile(previous_active, nullptr);
                }
            } else if (previous_active.empty()) {
                model_management_->registry().deactivate(&rb_error);
                core_runtime_->stop();
                runtime_started_ = false;
                running_model_id_.clear();
                want_runtime_running_.store(false);
            }
            if (error) *error = "新模型已启动但配置提交失败，已执行回滚: " + sync_error;
            return false;
        }
        return true;
    }
    if (!switch_active_model_runtime(model_id, error)) {
        // 切换失败：回滚 active 到旧模型，恢复旧模型运行
        std::string switch_error = error && !error->empty() ? *error : "切换失败";
        std::string rb_error;
        bool rb_capture_missing = false;
        if (!previous_active.empty()) {
            model_management_->registry().activate(previous_active, &rb_error);
        } else {
            model_management_->registry().deactivate(&rb_error);
        }
        if (!previous_active.empty() &&
            switch_active_model_runtime(previous_active, &rb_error, &rb_capture_missing)) {
            if (error) *error = "模型切换失败已回滚到 " + previous_active + ": " + switch_error;
        } else if (previous_active.empty()) {
            core_runtime_->stop();
            runtime_started_ = false;
            running_model_id_.clear();
            want_runtime_running_.store(false);
            if (error) *error = "模型切换失败，已恢复为未选择模型状态: " + switch_error;
        } else if (rb_capture_missing) {
            // ★ 旧模型也只是"没画面可验证"，而注册表与配置都已回到旧模型 ⇒ 这不是回滚失败。
            //   报"回滚失败"会让操作者以为设备状态坏了，实际只需点亮信号源。
            if (error) {
                *error = "模型切换失败（无画面输入，无法验证新模型）；已回滚到 " +
                         previous_active + "，接入信号源后会自动恢复运行。原因: " + switch_error;
            }
        } else {
            // 旧模型也起不来：让主循环 2s 自动重试拉起（want 仍为 true）
            if (error) {
                *error = "模型切换失败且回滚失败: " + switch_error;
            }
        }
        return false;
    }
    TTBOX_LOG_INFO("模型热切换完成: running_model_id=" + running_model_id_);
    std::string sync_error;
    if (!sync_model_id_to_profile(model_id, &sync_error)) {
        std::string rb_error;
        if (!previous_active.empty()) {
            model_management_->registry().activate(previous_active, &rb_error);
        } else {
            model_management_->registry().deactivate(&rb_error);
        }
        if (!previous_active.empty() && switch_active_model_runtime(previous_active, &rb_error)) {
            std::string restore_sync_error;
            if (sync_model_id_to_profile(previous_active, &restore_sync_error)) {
                if (error) *error = "新模型配置提交失败，已回滚到 " + previous_active + ": " + sync_error;
            } else if (error) {
                *error = "新模型配置提交失败，旧模型已恢复运行但配置回写失败: " + restore_sync_error;
            }
        } else if (previous_active.empty()) {
            core_runtime_->stop();
            runtime_started_ = false;
            running_model_id_.clear();
            want_runtime_running_.store(false);
            if (error) *error = "新模型配置提交失败，已恢复为未选择模型状态: " + sync_error;
        } else if (error) {
            *error = "新模型配置提交失败且回滚失败: " + sync_error + "; " + rb_error;
        }
        return false;
    }
    return true;
}
bool Application::handle_model_import(const std::string& src_path, const std::string& model_id,
                                      const std::string& label, const std::string& source_format,
                                      const std::string& sha256, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用（初始化失败）";
        return false;
    }
    ModelRegistry& reg = model_management_->registry();
    // 路径安全：src_path 必须位于收件目录内（防 ../ 任意文件读取）。
    // Windows 下 fs 返回反斜杠路径，统一归一化成 '/' 再比较。
    auto normalize = [](std::string s) {
        for (char& c : s) if (c == '\\') c = '/';
        return s;
    };
    std::error_code path_ec;
    const std::filesystem::path incoming_path =
        std::filesystem::weakly_canonical(incoming_dir_of(reg), path_ec);
    const std::filesystem::path source_path =
        std::filesystem::weakly_canonical(src_path, path_ec);
    const std::string incoming = normalize(incoming_path.string());
    const std::string src_norm = normalize(source_path.string());
    const std::string incoming_prefix = incoming.empty() || incoming.back() == '/'
                                            ? incoming : incoming + "/";
    // canonical 后再做“目录边界”判断：拒绝 ../、符号链接逃逸和
    // /_incoming_fake 这类仅字符串前缀相同的相邻目录。
    if (path_ec || src_norm.rfind(incoming_prefix, 0) != 0) {
        if (error) *error = "模型文件必须先上传到收件目录（" + incoming + "）";
        return false;
    }
    ModelManifest manifest;
    manifest.label = label.empty() ? model_id : label;
    manifest.origin = "local";
    // 来源格式：直接上传=rknn（默认）；ONNX 转换产物=onnx。落 manifest 供前端/排查。
    manifest.source_format = (source_format == "onnx") ? "onnx" : "rknn";
    if (!sha256.empty()) manifest.sha256 = sha256;  // Web 层 hashlib 计算（标准实现），Core 只存不算
    if (!reg.import(src_path, model_id, manifest, error)) return false;
    TTBOX_LOG_INFO("模型已导入 staging: " + model_id);
    return true;
}
bool Application::handle_model_install(const std::string& model_id, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    return model_management_->registry().install(model_id, error);
}
JsonValue Application::handle_model_list() {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    JsonValue data = JsonValue::object();
    if (!model_management_) {
        data.set("models", JsonValue::array());
        data.set("selected_model_id", JsonValue::string(""));
        data.set("running_model_id", JsonValue::string(""));
        data.set("state", JsonValue::string("failed"));
        data.set("available", JsonValue::boolean(false));
        return data;
    }
    const ModelRegistry& reg = model_management_->registry();
    const std::string selected = reg.active_model();
    const std::string running = running_model_id_;
    JsonValue arr = JsonValue::array();
    for (const auto& record : reg.records()) {
        ModelRecord current = record;
        current.selected = (current.model_id == selected);
        current.running = (!running.empty() && current.model_id == running);
        if (current.running) current.status = ModelStatus::kRunning;
        else if (current.selected && current.status == ModelStatus::kReady) current.status = ModelStatus::kSelected;
        arr.push_back(current.to_json());
    }
    data.set("models", std::move(arr));
    data.set("selected_model_id", JsonValue::string(selected));
    data.set("running_model_id", JsonValue::string(running));
    data.set("state", JsonValue::string(selected.empty() ? "stopped" :
                                        (selected == running ? "running" : "switching")));
    data.set("failure_code", JsonValue::string(model_failure_code_));
    data.set("failure_message", JsonValue::string(model_failure_message_));
    data.set("available", JsonValue::boolean(true));
    return data;
}
bool Application::handle_model_remove(const std::string& model_id, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    return model_management_->registry().remove(model_id, error);
}
bool Application::handle_model_set_concurrency(const std::string& model_id, int count,
                                               std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    if (!model_management_->registry().set_concurrency(model_id, count, error)) {
        return false;
    }
    // 当前运行的是该模型时，立即重建 worker 池使并发生效；否则下次启动该模型时生效。
    if (core_runtime_ && core_runtime_->running() && running_model_id_ == model_id) {
        std::string switch_error;
        bool rebuild_capture_missing = false;
        if (!switch_active_model_runtime(model_id, &switch_error, &rebuild_capture_missing)) {
            if (rebuild_capture_missing) {
                // 并发数已落盘；只是当前没画面、无法验证重建后的首帧 ⇒ 有信号后主循环会自动拉起。
                if (error) {
                    *error = "并发数已保存；当前无画面输入，运行时重建将在信号恢复后自动生效。原因: " +
                             switch_error;
                }
                return false;
            }
            if (error) *error = "并发已保存，但运行时重建失败: " + switch_error;
            return false;
        }
    }
    return true;
}
bool Application::handle_model_validate(const std::string& model_id, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    return model_management_->registry().validate(model_id, error);
}
// RUNTIME_CONTROL：start / stop / restart。
// 直接复用 CoreRuntime start/stop（幂等），不触碰平台 RuntimeController 状态机。
bool Application::handle_runtime_control(const std::string& action, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!core_runtime_) {
        if (error) *error = "core_runtime 未初始化";
        return false;
    }
    if (action == "start") {
        // 用户显式点启动 ⇒ 取消更新冒烟自检的"稍后自动停"（用户意愿优先）。
        post_update_smoke_.store(false);
        if (core_runtime_->running()) return true;  // 幂等
        // T1.10① / M2.03：授权未通过不启动 AI 路径（透传/Web 管理不受影响）。
        if (!auth::LicenseGate::instance().pipeline_allowed()) {
            if (error) *error = "授权未通过：缺少 feature 'capture'，AI 功能路径不启动";
            return false;
        }
        want_runtime_running_.store(true);
        persist_runtime_intent(true);
        running_model_id_.clear();
        // R4：降级态（core_runtime_ 已分配但未 initialize）先整链重建再 start；
        // 重建失败返回真实等待原因，而不是"内部对象未初始化"这类误导性内部错误。
        if (runtime_waiting_config_ && !try_resume_from_degraded(error)) {
            if (error && error->empty()) *error = "等待模型配置（WAITING_FOR_MODEL）";
            model_failure_code_ = "WAITING_FOR_MODEL";
            model_failure_message_ = error ? *error : "";
            // want 保持 true：主循环 2s 自动重试，配置补全即自动拉起
            return false;
        }
        if (!core_runtime_->start(error)) {
            if (error && error->empty()) *error = "CoreRuntime 启动失败";
            // 启动失败仍保留 want=true：主循环每 2s 自动重试，直到 HDMI/模型就绪
            return false;
        }
        runtime_started_ = true;
        return true;
    }
    if (action == "stop") {
        // 用户显式点停止 ⇒ 同时取消更新冒烟自检（用户意愿优先）。
        post_update_smoke_.store(false);
        want_runtime_running_.store(false);
        persist_runtime_intent(false);
        core_runtime_->stop();
        runtime_started_ = false;
        running_model_id_.clear();
        model_failure_code_.clear();
        model_failure_message_.clear();
        return true;
    }
    if (action == "restart") {
        // 用户显式重启 ⇒ 取消更新冒烟自检（视为"保持运行"，不再自动停）。
        post_update_smoke_.store(false);
        // T1.10① / M2.03：授权未通过不重启 AI 路径。
        if (!auth::LicenseGate::instance().pipeline_allowed()) {
            if (error) *error = "授权未通过：缺少 feature 'capture'，AI 功能路径不启动";
            return false;
        }
        want_runtime_running_.store(true);
        persist_runtime_intent(true);
        // 新运行世代在首帧 inference+decode 通过前没有 current model。
        // 禁止 restart 返回后继续展示旧世代 running_model_id。
        running_model_id_.clear();
        model_failure_code_.clear();
        model_failure_message_.clear();
        core_runtime_->stop();
        // R4：同 start——降级态先整链重建，失败返回真实等待原因
        if (runtime_waiting_config_ && !try_resume_from_degraded(error)) {
            if (error && error->empty()) *error = "等待模型配置（WAITING_FOR_MODEL）";
            model_failure_code_ = "WAITING_FOR_MODEL";
            model_failure_message_ = error ? *error : "";
            runtime_started_ = false;
            return false;
        }
        if (!core_runtime_->start(error)) {
            if (error && error->empty()) *error = "CoreRuntime 重启失败";
            runtime_started_ = false;
            return false;
        }
        runtime_started_ = true;
        return true;
    }
    if (error) *error = "未知 action: " + action;
    return false;
}

// ============ 硬件写入（V1.0.50）============
// 为什么在 core 侧做：edid_apply.sh 要写 /sys/class/hdmirx/**、
// /sys/kernel/debug/hdmirx/ 与固件目录；ttbox_usb_mode.sh 要改 systemd 单元并
// daemon-reload。core 跑 User=root，是唯一有权限的层；web（ttbox，无 sudo）
// 直写 sysfs 只会得到「看起来实现了、上板就崩」的代码。
namespace {

// 运行根前缀（对齐 Python ttbox_paths / web 侧 ttbox_prefix()）。
std::string hw_root() {
    const char* v = std::getenv("TTBOX_PREFIX");
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string("/opt/ttbox");
}

// 跑一个脚本，捕获合并输出（stdout+stderr），返回 exit code。
// 不用 std::system：它走 /bin/sh 且不回传 stdout，错误文案无法回传给 web。
//
// ★ 命令拼装踩坑（V1.0.50 板端实测 exit 2）：初版写成 `{ <cmd> ; } 2>&1`，
//   在 dash 下 `{` 之后紧跟环境变量前置赋值（TTBOX_EDID_REHANDSHAKE=1 ...）会被
//   当成独立词，报 `sh: 1: Syntax error: "}" unexpected`，脚本根本没跑起来。
//   正确做法：用 `env VAR=val <cmd>` 传环境变量（POSIX 通用），shell 一重定向即可，
//   不需要 brace group。
int run_script(const std::string& cmd, std::string* output, int timeout_sec) {
    const std::string wrapped = "timeout " + std::to_string(timeout_sec) + " " + cmd + " 2>&1";
    FILE* p = ::popen(wrapped.c_str(), "r");
    if (p == nullptr) {
        if (output) *output = "popen 失败";
        return -1;
    }
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) out += buf;
    const int rc = ::pclose(p);
    if (output) *output = out;
    // pclose 返回的是 wait 状态；取退出码（-1 表示异常终止）
    return (rc == -1) ? -1 : (rc >> 8);
}

// 原子写：先写 .tmp 再 rename（对齐 ApplicationRuntime.cpp 的 tmp+rename 范式）。
// 配置文件是 EDID 注入的真源，半写状态会让下次开机注入坏 EDID。
bool write_json_atomic(const std::string& path, const JsonValue& value) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << value.dump();
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }
    return true;
}

}  // namespace

// APPLY_EDID：应用 EDID（对齐 Python api/hardware.py::update_display_hardware 的 apply 分支）。
//   1) 写 config/hardware_display.json（合并白名单键，core 有权限）
//   2) 调 scripts/edid/edid_apply.sh（TTBOX_EDID_REHANDSHAKE=1）
bool Application::handle_hardware_action(const std::string& action, const JsonValue& params,
                                         JsonValue* data, std::string* error) {
    if (action == "apply_edid") {
        const std::string root = hw_root();
        const std::string cfg_path = root + "/config/hardware_display.json";
        const std::string script = root + "/scripts/edid/edid_apply.sh";

        // 1) 合并白名单键写配置（防注入：只接受这几个键）
        JsonValue cur = JsonValue::object();
        const JsonParseResult old_cfg = json_parse_file(cfg_path);
        if (old_cfg.ok && old_cfg.value.is_object()) cur = old_cfg.value;

        const JsonValue* cfg_in = params.find("config");
        if (cfg_in != nullptr && cfg_in->is_object()) {
            static const char* kAllowed[] = {
                "device", "name", "vendor", "product_id", "serial", "native_mode",
                "native_only", "profile", "loopout_enabled", "loopout_overlay_enabled",
                "loopout_pixel_format", "loopout_overlay_thickness", "loopout_overlay_color"};
            for (const char* k : kAllowed) {
                const JsonValue* v = cfg_in->find(k);
                if (v != nullptr) cur.set(k, *v);
            }
        }
        // device 归一：auto/空 → /dev/video0；拒绝 DRM 输出节点（与 Python 同口径）
        const std::string dev = cur.find("device") ? cur.find("device")->as_string("auto") : "auto";
        if (dev.empty() || dev == "auto") {
            cur.set("device", JsonValue::string("/dev/video0"));
        } else if (dev.rfind("/dev/dri/", 0) == 0 || dev.find("card") != std::string::npos ||
                   dev.find("renderD") != std::string::npos) {
            if (error) {
                *error = "HDMI-RX 输入必须使用 /dev/video0，/dev/dri/card0 仅用于 loopout";
            }
            return false;
        }

        if (!write_json_atomic(cfg_path, cur)) {
            if (error) *error = "写配置失败：" + cfg_path;
            return false;
        }

        // 2) 调 EDID 应用脚本（root 身份，HPD 重协商）
        std::string out;
        const int rc = run_script("env TTBOX_EDID_REHANDSHAKE=1 bash " + script + " " +
                                      cur.find("device")->as_string("/dev/video0"),
                                  &out, 60);
        JsonValue res = JsonValue::object();
        res.set("exit", JsonValue::number(rc));
        res.set("output", JsonValue::string(out.size() > 500 ? out.substr(out.size() - 500) : out));
        res.set("applied", JsonValue::boolean(rc == 0));
        if (data != nullptr) data->set("result", std::move(res));
        if (rc != 0 && error != nullptr) {
            *error = "EDID 应用失败（exit " + std::to_string(rc) + "）";
            if (!out.empty()) *error += "：" + out.substr(out.size() > 200 ? out.size() - 200 : 0);
        }
        return rc == 0;
    }

    if (action == "set_usb_mode") {
        const std::string mode = params.find("mode") ? params.find("mode")->as_string("") : "";
        if (mode.empty()) {
            if (error) *error = "缺少 params.mode";
            return false;
        }
        // 透传模式真源 = systemd 单元的 USB_PROXY_MODE（Python 版同口径，见
        // web/infra/mouse 的说明）。core 是 root ⇒ 可以改单元 + daemon-reload。
        const std::string root = hw_root();
        const std::string unit = root + "/deploy/systemd/ttbox-usbproxy.service";
        const std::string script = root + "/scripts/ttbox_usb_mode.sh";

        // 优先用官方脚本（它负责改单元 + reload + 重启 + 健康检查）
        std::string out;
        int rc = run_script("bash " + script + " " + mode, &out, 30);
        if (rc != 0) {
            // 脚本缺失/失败 ⇒ 退化为直接改单元（core 是 root，可写）
            out += "\n[fallback] 直接改 systemd 单元";
            rc = run_script(
                "sed -i 's/^Environment=USB_PROXY_MODE=.*/Environment=USB_PROXY_MODE=" + mode +
                    "/' " + unit + " && systemctl daemon-reload && systemctl restart ttbox-usbproxy",
                &out, 30);
        }
        JsonValue res = JsonValue::object();
        res.set("mode", JsonValue::string(mode));
        res.set("exit", JsonValue::number(rc));
        res.set("output", JsonValue::string(out.size() > 500 ? out.substr(out.size() - 500) : out));
        res.set("applied", JsonValue::boolean(rc == 0));
        if (data != nullptr) data->set("result", std::move(res));
        if (rc != 0 && error != nullptr) {
            *error = "USB 透传模式切换失败（exit " + std::to_string(rc) + "）";
            if (!out.empty()) *error += "：" + out.substr(out.size() > 200 ? out.size() - 200 : 0);
        }
        return rc == 0;
    }

    if (error) *error = "未知 hardware action: " + action;
    return false;
}

}  // namespace ttbox::core
