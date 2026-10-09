// win_core_main.cpp — TTBOX Windows 本地 Core 宿主（第二阶段：Web 模型库真实后端）
//
// 职责（与板端 Application 的差异 = 硬件层缺失，业务层完全复用生产代码）：
//   - ConfigManager   读真实 default.json
//   - ModelManagement + ModelRegistry（生产代码，唯一模型真相）
//   - IpcServer（Windows TCP loopback，协议与板端 Unix socket 完全一致）
//   - 模型 validator：
//       .rknn → 文件级校验（Windows 无 RKNN runtime）
//       .onnx → OnnxBackend 真实加载 + 元数据探测（输入尺寸/输出结构/class 数）
//   - STATUS provider（最小真实状态）
//
// 用法: win_core_main.exe --config <default.json> --ipc tcp:8090 --models <models_root>
//
// Web 侧配合：TTBOX_IPC_SOCKET=tcp:127.0.0.1:8090（ipc_request TCP 分支）
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "common/Logger.hpp"
#include "common/Metrics.hpp"
#include "config/ConfigManager.hpp"
#include "ipc/IpcServer.hpp"
#include "model/ModelManagement.hpp"
#include "model/RuntimeProfile.hpp"

#if defined(TTBOX_CORE_HAS_ONNX) && TTBOX_CORE_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "model/backend/OnnxBackend.hpp"
#endif

namespace fs = std::filesystem;
using namespace ttbox::core;

static std::atomic<bool> g_stop{false};

#if defined(_WIN32)
#include <windows.h>
static BOOL WINAPI win_signal(DWORD) {
    g_stop.store(true);
    return TRUE;
}
#endif

namespace {

// ---------- .onnx 真实元数据校验器（生产 OnnxBackend）----------
#if defined(TTBOX_CORE_HAS_ONNX) && TTBOX_CORE_HAS_ONNX
bool onnx_validator(const std::string& path, JsonValue* meta_out, std::string* error) {
    OnnxBackend backend;
    if (!backend.init(path, error)) return false;
    const RknnModelInfo& info = backend.info();
    if (info.n_outputs == 0 || info.input_width == 0) {
        if (error) *error = "ONNX 元数据不完整（无输出或无输入尺寸）";
        return false;
    }
    // 真实推理探针：全零输入跑一次，验证 runtime 真能执行
    std::vector<float> in(info.input_size / sizeof(float), 0.0f);
    std::vector<std::vector<float>> outputs;
    if (!backend.infer(in.data(), in.size() * sizeof(float), outputs, error)) return false;
    if (outputs.size() != info.n_outputs) {
        if (error) *error = "ONNX 推理输出数与元数据不符";
        return false;
    }
    // 元数据：与板端 metadata.json 字段语义一致（decode_type/class_count 为硬门槛）
    JsonValue meta = JsonValue::object();
    meta.set("input_width", JsonValue::number(info.input_width));
    meta.set("input_height", JsonValue::number(info.input_height));
    meta.set("input_dtype", JsonValue::string("float32"));
    meta.set("output_count", JsonValue::number(info.n_outputs));
    if (info.n_outputs == 1 && info.outputs[0].dims.size() == 3) {
        // e2e 单输出 [1,N,6]: x1,y1,x2,y2,score,class_id → 类别数在推理时由 class_id 值域决定，
        // COCO 系模型固定 80。这里不猜：e2e 用 80 作为上限登记，Decode 端以实际 id 为准。
        meta.set("decode_type", JsonValue::string("e2e"));
        meta.set("class_count", JsonValue::number(80));
    } else if (info.n_outputs % 3 == 0 && info.outputs.size() >= 2 &&
               info.outputs[1].dims.size() == 4) {
        // DFL-dist：reg/cls/aux 三元组 → cls 通道数 = outputs[1].dims[1]
        meta.set("decode_type", JsonValue::string("dfl_dist"));
        meta.set("class_count", JsonValue::number(info.outputs[1].dims[1]));
    } else {
        if (error) *error = "ONNX 输出布局无法识别（非 e2e/DFL-dist）";
        return false;
    }
    meta.set("quantization", JsonValue::string("fp32"));
    if (meta_out) *meta_out = std::move(meta);
    return true;
}
#endif

// ---------- .rknn 文件级校验（Windows 无 RKNN runtime；与 file_level_validator 同语义）----------
bool file_level_validator(const std::string& path, JsonValue* meta_out, std::string* error) {
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::file_size(path, ec) < 1024) {
        if (error) *error = "文件缺失或过小（<1KB）";
        return false;
    }
    if (meta_out) {
        JsonValue meta = JsonValue::object();
        meta.set("input_width", JsonValue::number(0));
        meta.set("input_height", JsonValue::number(0));
        meta.set("output_count", JsonValue::number(0));
        meta.set("class_count", JsonValue::number(0));
        meta.set("output_format", JsonValue::string("unknown"));
        meta.set("quantization", JsonValue::string(""));
        meta.set("note", JsonValue::string("Windows 文件级校验（无 RKNN runtime）"));
        *meta_out = std::move(meta);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/default.json";
    std::string ipc_path = "tcp:8090";
    std::string models_root;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!std::strcmp(argv[i], "--ipc") && i + 1 < argc) ipc_path = argv[++i];
        else if (!std::strcmp(argv[i], "--models") && i + 1 < argc) models_root = argv[++i];
    }

    TTBOX_LOG_INFO("TTBOX Windows Core 宿主启动");

#if defined(_WIN32)
    SetConsoleCtrlHandler(win_signal, TRUE);
#endif

    // ---- 1. 配置 ----
    ConfigManager config;
    std::string err;
    if (!config.load(config_path, &err)) {
        TTBOX_LOG_WARN("配置加载失败（用内置默认值继续）: " + err);
    }

    // ---- 2. 模型仓库（生产 ModelRegistry）----
    if (models_root.empty()) {
        models_root = config.get_string("model_registry_root", "");
    }
    if (models_root.empty()) models_root = "models";
    // 绝对化：import 的收件目录前缀校验要求 src 与 root 同一形态（绝对路径）
    {
        std::error_code ec;
        fs::path ap = fs::absolute(models_root, ec);
        if (!ec) models_root = ap.string();
    }
    // ---- 2b. 运行时配置（RuntimeConfig 内存热更新，与板端 Application 同构）----
    // 启动时从配置文件 runtime_profile 段初始化内存快照，保证 GET_CONFIG 一开始就有真实值。
    RuntimeConfig runtime_config;
    if (config.loaded()) {
        const JsonValue* p = config.root().find("runtime_profile");
        if (p != nullptr && p->is_object()) {
            RuntimeProfile prof = RuntimeProfile::from_json(*p);
            std::string verr;
            if (prof.validate(&verr)) {
                runtime_config.update(prof);
            } else {
                TTBOX_LOG_WARN("配置文件 runtime_profile 校验未通过（用默认值继续）: " + verr);
            }
        }
    }

    ModelManagement mm{ModelRegistryOptions{models_root, true}};
    if (!mm.init(&err)) {
        TTBOX_LOG_ERROR("ModelRegistry 初始化失败: " + err);
        return 1;
    }
    // validator 按扩展名分发
    mm.set_validator([](const std::string& path, JsonValue* meta, std::string* error) {
        // 内容嗅探：import 会把模型统一命名为 model.rknn（ModelRegistry 固定布局），
        // Windows 下不能依赖扩展名。先尝试真实 ONNX 加载（可执行推理才算过），
        // 失败再回退文件级校验（.rknn 在 Windows 无 runtime）。
#if defined(TTBOX_CORE_HAS_ONNX) && TTBOX_CORE_HAS_ONNX
        std::string onnx_err;
        if (onnx_validator(path, meta, &onnx_err)) return true;
        // 非 ONNX（或加载失败）→ 文件级。把 ONNX 错误附在失败信息里供排查。
        std::string ferr;
        if (!file_level_validator(path, meta, &ferr)) {
            if (error) *error = ferr + "（ONNX 试加载: " + onnx_err + "）";
            return false;
        }
        return true;
#else
        return file_level_validator(path, meta, error);
#endif
    });
    if (!mm.registry().refresh(&err)) {
        TTBOX_LOG_WARN("模型仓库刷新失败: " + err);
    }
    TTBOX_LOG_INFO("ModelRegistry 就绪: " + mm.registry().root_dir());

    // ---- 3. IPC（Windows TCP，协议与板端一致）----
    IpcServer ipc;
    ipc.set_status_provider([&] {
        SystemStatus st;
        st.running = true;
        st.runtime_running = false;  // Windows 宿主无流水线
        st.app_name = "ttbox-win-core";
        st.version = "1.0.0-win";
        st.ipc_socket = ipc.socket_path();
        st.config_file = config_path;
        return st;
    });
    // GET_CONFIG：唯一真源 = RuntimeConfig 内存快照（SET_CONFIG 热更新后的最新值），
    // 回退配置文件 runtime_profile 段（与板端 Application::config_provider 同构）。
    ipc.set_config_provider([&] {
        JsonValue data = JsonValue::object();
        JsonValue prof = JsonValue::object();
        if (auto snap = runtime_config.snapshot()) {
            prof = snap->to_json();
        } else if (config.loaded()) {
            const JsonValue* p = config.root().find("runtime_profile");
            if (p != nullptr && p->is_object()) prof = *p;
        }
        data.set("runtime_profile", std::move(prof));
        data.set("config_file", JsonValue::string(config_path));
        return data;
    });
    ipc.set_model_list_handler([&] {
        JsonValue data = JsonValue::object();
        const ModelRegistry& reg = mm.registry();
        const std::string selected = reg.active_model();
        JsonValue arr = JsonValue::array();
        for (const auto& record : reg.records()) {
            ModelRecord cur = record;
            cur.selected = (cur.model_id == selected);
            cur.running = cur.selected;  // Windows 无流水线：激活即视为运行
            if (cur.running) cur.status = ModelStatus::kRunning;
            else if (cur.selected && cur.status == ModelStatus::kReady)
                cur.status = ModelStatus::kSelected;
            arr.push_back(cur.to_json());
        }
        data.set("models", std::move(arr));
        data.set("selected_model_id", JsonValue::string(selected));
        data.set("running_model_id", JsonValue::string(selected));
        data.set("state", JsonValue::string(selected.empty() ? "stopped" : "running"));
        data.set("failure_code", JsonValue::string(""));
        data.set("failure_message", JsonValue::string(""));
        data.set("available", JsonValue::boolean(true));
        return data;
    });
    ipc.set_model_import_handler([&](const std::string& src, const std::string& model_id,
                                     const std::string& label, const std::string& source_format,
                                     const std::string& sha256, std::string* error) {
        // 路径安全：与 Application 相同的收件目录约束
        auto normalize = [](std::string s) {
            for (char& c : s) if (c == '\\') c = '/';
            return s;
        };
        const std::string incoming =
            normalize(mm.registry().root_dir() + "/_incoming");
        if (normalize(src).rfind(incoming, 0) != 0) {
            if (error) *error = "模型文件必须先上传到收件目录（" + incoming + "）";
            return false;
        }
        ModelManifest manifest;
        manifest.label = label.empty() ? model_id : label;
        manifest.origin = "local";
        manifest.source_format = (source_format == "onnx") ? "onnx" : "rknn";
        manifest.sha256 = sha256;
        return mm.registry().import(src, model_id, manifest, error);
    });
    ipc.set_model_validate_handler(
        [&](const std::string& id, std::string* error) { return mm.registry().validate(id, error); });
    ipc.set_model_install_handler(
        [&](const std::string& id, std::string* error) { return mm.registry().install(id, error); });
    // Windows 无流水线：activate = 注册表选中（真实生效于 registry/active.json）
    ipc.set_model_activate_handler(
        [&](const std::string& id, std::string* error) { return mm.registry().activate(id, error); });
    ipc.set_model_remove_handler(
        [&](const std::string& id, std::string* error) { return mm.registry().remove(id, error); });
    // 配置热更新：完整复刻板端 Application::handle_config_update 语义 ——
    // RuntimeProfile::from_json → validate → runtime_config.update(内存原子替换) → 落盘。
    // 任一失败不污染内存与磁盘；persisted=false 表示内存已生效但落盘失败。
    ipc.set_config_update_handler([&](const JsonValue& profile_json, std::string* error,
                                      bool* persisted) {
        if (persisted) *persisted = false;
        if (!profile_json.is_object()) {
            if (error) *error = "profile 必须是 JSON 对象";
            return false;
        }
        // 1) 解析（严格：非法字段/类型错误 → 失败）
        RuntimeProfile profile = RuntimeProfile::from_json(profile_json);
        std::string verr;
        if (!profile.validate(&verr)) {
            if (error) *error = verr.empty() ? "profile 校验失败" : ("profile 校验失败: " + verr);
            return false;
        }
        // 2) 内存热更新（原子替换 shared_ptr；运行中模块下个周期读到新值）
        runtime_config.update(profile);
        // 3) 落盘：读回 config 根，仅替换 runtime_profile 键，其余键原样保留
        bool saved = false;
        if (!config_path.empty() && config.loaded()) {
            JsonValue merged = config.root();  // 深拷贝宿主 JSON
            merged.set("runtime_profile", profile_json);
            const std::string text = merged.dump();
            const std::string tmp = config_path + ".tmp";
            std::ofstream out(tmp);
            bool ok = false;
            if (out) {
                out << text;
                out.close();
                std::error_code ec;
                fs::rename(tmp, config_path, ec);
                ok = !ec;
            }
            if (!ok && error) *error = "内存配置已生效，但写入配置文件失败: " + config_path;
            saved = ok;
        }
        if (persisted) *persisted = saved;
        return true;
    });

    if (!ipc.start(ipc_path, &err)) {
        TTBOX_LOG_ERROR("IPC 启动失败: " + err);
        return 1;
    }
    TTBOX_LOG_INFO("IPC 已监听: " + ipc.socket_path());
    TTBOX_LOG_INFO("模型目录: " + mm.registry().root_dir());

    // ---- 4. 事件循环（信号退出）----
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    TTBOX_LOG_INFO("退出中...");
    ipc.stop();
    return 0;
}
