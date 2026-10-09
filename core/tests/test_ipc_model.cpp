// test_ipc_model.cpp — v0.3 模型管理 IPC 验收：
// MODEL_LIST / MODEL_IMPORT / MODEL_VALIDATE / MODEL_INSTALL / MODEL_ACTIVATE / MODEL_REMOVE
// 附带：model_id 非法字符拒绝（path traversal 防护）、收件目录约束、active 跟随。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <direct.h>
#endif

#include "common/Json.hpp"
#include "ipc/IpcServer.hpp"
#include "model/ModelManagement.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace ttbox::core;

namespace {

// JSON 字符串转义（Windows 路径反斜杠必须转成 \\，否则 \U 等被当非法转义）
static std::string json_escape(std::string s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// IPC 传输失败时构造 status=-1 的伪响应（error 保留原因）
static JsonValue ipc_error_response(const std::string& err) {
    JsonValue j = JsonValue::object();
    j.set("status", JsonValue::number(-1));
    j.set("error", JsonValue::string(err));
    return j;
}

// 每实例唯一的临时目录标识：组合 PID、单调时钟和进程内序号。
// 仅 PID+序号仍会在 Windows 复用 PID、新进程序号重置时撞上旧残留目录。
static std::string unique_instance_id() {
    static std::atomic<int> seq{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(::getpid()) + "_" + std::to_string(tick) + "_" +
           std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
}

std::string tmp_socket() {
#if defined(_WIN32)
    return "tcp:0";  // 临时端口：OS 分配唯一端口（见 test_ipc.cpp 同注释）
#else
    return "/tmp/ttbox_ipc_model_" + std::to_string(static_cast<long>(::getpid())) + ".sock";
#endif
}

struct ModelFixture {
    std::string root;
    ModelManagement mm;
    IpcServer server;

    explicit ModelFixture(bool with_validator = true)
        : root((fs::temp_directory_path() / ("ttbox_ipc_model_" + unique_instance_id())).string()),
          mm(ModelRegistryOptions{root, true}) {
        std::string err;
        if (!mm.init(&err)) { std::fprintf(stderr, "init failed: %s\n", err.c_str()); std::abort(); }
        if (with_validator) {
            mm.set_validator([](const std::string& path, JsonValue* metadata, std::string* error) {
                if (!fs::is_regular_file(path)) {
                    if (error) *error = "MODEL_FILE_MISSING";
                    return false;
                }
                JsonValue value = JsonValue::object();
                value.set("input_width", JsonValue::number(320));
                value.set("input_height", JsonValue::number(320));
                value.set("output_count", JsonValue::number(2));
                value.set("class_count", JsonValue::number(1));
                value.set("decode_type", JsonValue::string("e2e"));
                if (metadata) *metadata = std::move(value);
                return true;
            });
        }
        wire();
    }

    ~ModelFixture() {
        server.stop();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void wire() {
        server.set_model_list_handler([this] {
            JsonValue data = JsonValue::object();
            JsonValue arr = JsonValue::array();
            for (const auto& m : mm.registry().list()) arr.push_back(m.to_json());
            data.set("models", std::move(arr));
            data.set("active", JsonValue::string(mm.registry().active_model()));
            return data;
        });
        server.set_model_import_handler(
            [this](const std::string& src, const std::string& id, const std::string& label,
                   const std::string& source_format, const std::string& sha256,
                   std::string* error) {
                (void)source_format;
                (void)sha256;
                if (src.rfind(mm.registry().root_dir() + "/_incoming", 0) != 0) {
                    if (error) *error = "模型文件必须先上传到收件目录";
                    return false;
                }
                ModelManifest mf;
                mf.label = label.empty() ? id : label;
                return mm.registry().import(src, id, mf, error);
            });
        server.set_model_validate_handler(
            [this](const std::string& id, std::string* error) { return mm.registry().validate(id, error); });
        server.set_model_install_handler(
            [this](const std::string& id, std::string* error) { return mm.registry().install(id, error); });
        server.set_model_activate_handler(
            [this](const std::string& id, std::string* error) { return mm.registry().activate(id, error); });
        server.set_model_remove_handler(
            [this](const std::string& id, std::string* error) { return mm.registry().remove(id, error); });
    }

    bool start() {
        std::string err;
        return server.start(tmp_socket(), &err);
    }

    // 写一个假模型到收件目录。返回正斜杠路径（与 Core root_dir() 拼接格式一致）。
    std::string make_incoming(const std::string& name) {
        const std::string dir = root + "/_incoming";
        fs::create_directories(dir);
        const std::string path = dir + "/" + name;
        std::ofstream f(path, std::ios::binary);
        f << std::string(4096, 'R');  // >1KB，通过文件级校验
        f.close();
        return path;
    }

    JsonValue ipc(const std::string& type, const std::string& params_json = "") {
        std::string req = "{\"type\":\"" + type + "\"";
        if (!params_json.empty()) req += ",\"params\":" + params_json;
        req += "}";
        std::string response;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        attempt_trace_.clear();
        // Windows 下连接刚 listen 的 socket 偶发 WSAECONNREFUSED：客户端重试 3 次
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto a0 = std::chrono::steady_clock::now();
            err.clear();
            if (ipc_request(server.socket_path(), req, response, 3000, &err)) {
                last_elapsed_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - t0).count();
                auto parsed = json_parse(response);
                return parsed.ok ? parsed.value : JsonValue::null();
            }
            const auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - a0).count();
            attempt_trace_ += "  [IPC-ATTEMPT] type=" + type + " n=" + std::to_string(attempt + 1) +
                              " cost=" + std::to_string(cost) + "ms err=" + err + "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        last_elapsed_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
        last_error_ = err;
        if (!attempt_trace_.empty()) std::fputs(attempt_trace_.c_str(), stderr);
        return ipc_error_response(err);
    }

    // 偶发失败诊断用：最近一次 ipc() 的耗时与传输错误（成功时 err 为空）。
    // 3×3s 超时合计 ≈9600ms，与「立刻被服务端关连接」（≈300ms）可据此区分。
    long long last_elapsed_ms_ = 0;
    std::string last_error_;
    std::string attempt_trace_;

};



}  // namespace

// 独立辅助：从 IPC 响应取 status（放在匿名空间外以便 TEST 宏展开后可见）
static int resp_status(const JsonValue& r) {
    const auto* v = r.find("status");
    return v ? static_cast<int>(v->as_int()) : -1;
}

// 偶发失败诊断（2026-09-22）：本用例曾在全量 ctest -j8 下 1/10 概率红，
// 但原断言只报 "-1 vs 0"，无法区分「传输超时/被拒」与「服务端 validate 真失败」。
// 失败时把 error 串与响应原文打到 stderr，便于定位（不改变判定）。
static void dump_ipc_detail(const char* tag, const JsonValue& r,
                            long long elapsed_ms, const std::string& transport_err) {
    const auto* e = r.find("error");
    std::fprintf(stderr, "  [IPC-DETAIL] %s status=%d elapsed=%lldms error=%s transport=%s\n", tag,
                 resp_status(r), elapsed_ms,
                 (e && e->is_string()) ? e->as_string().c_str() : "<none>",
                 transport_err.empty() ? "<none>" : transport_err.c_str());
}

TEST(model_ipc_full_lifecycle) {
    ModelFixture fx;
    CHECK(fx.start());

    // 1) 空列表
    auto r = fx.ipc("MODEL_LIST");
    CHECK_EQ(resp_status(r), 0);
    {
        const auto* data = r.find("data");
        const auto* models = data ? data->find("models") : nullptr;
        CHECK(models != nullptr && models->as_array().empty());
    }

    // 2) import（合法收件路径）
    const std::string src = fx.make_incoming("yolo_face.rknn");
    r = fx.ipc("MODEL_IMPORT",
               R"({"src_path":")" + json_escape(src) + R"(","model_id":"yolo-face-v1","label":"人脸检测"})");
    if (resp_status(r) != 0)
        dump_ipc_detail("MODEL_IMPORT", r, fx.last_elapsed_ms_, fx.last_error_);
    CHECK_EQ(resp_status(r), 0);

    // 3) import 二次同 id → 拒绝（staging 冲突不报错但 install 前提是 validate；重导 staging 会覆盖，
    //    这里 import 相同 id 应成功覆盖 staging——Core 语义如此，测成功即可）
    // 4) validate → install
    // Windows 文件系统偶发延迟（Defender 扫描锁）：validate 失败时重试 3 次
    r = fx.ipc("MODEL_VALIDATE", R"({"model_id":"yolo-face-v1"})");
    for (int attempt = 0; attempt < 3 && resp_status(r) != 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        r = fx.ipc("MODEL_VALIDATE", R"({"model_id":"yolo-face-v1"})");
    }
    if (resp_status(r) != 0)
        dump_ipc_detail("MODEL_VALIDATE", r, fx.last_elapsed_ms_, fx.last_error_);
    CHECK_EQ(resp_status(r), 0);
    r = fx.ipc("MODEL_INSTALL", R"({"model_id":"yolo-face-v1"})");
    for (int attempt = 0; attempt < 3 && resp_status(r) != 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        r = fx.ipc("MODEL_INSTALL", R"({"model_id":"yolo-face-v1"})");
    }
    if (resp_status(r) != 0)
        dump_ipc_detail("MODEL_INSTALL", r, fx.last_elapsed_ms_, fx.last_error_);
    CHECK_EQ(resp_status(r), 0);
    r = fx.ipc("MODEL_LIST");
    CHECK_EQ(resp_status(r), 0);
    {
        const auto* data = r.find("data");
        const auto* models = data ? data->find("models") : nullptr;
        CHECK(models != nullptr && models->as_array().size() == 1);
        const auto* active = data ? data->find("active") : nullptr;
        CHECK(active != nullptr && active->as_string().empty());
    }

    // 6) activate → active 跟随
    r = fx.ipc("MODEL_ACTIVATE", R"({"model_id":"yolo-face-v1"})");
    CHECK_EQ(resp_status(r), 0);
    r = fx.ipc("MODEL_LIST");
    {
        const auto* data = r.find("data");
        const auto* active = data ? data->find("active") : nullptr;
        CHECK(active != nullptr && active->as_string() == "yolo-face-v1");
    }

    // 7) remove 激活中的模型 → 拒绝
    r = fx.ipc("MODEL_REMOVE", R"({"model_id":"yolo-face-v1"})");
    CHECK_EQ(resp_status(r), 1);  // BAD_REQUEST

    // 8) deactivate → remove 成功
    // （Core 的 deactivate 走 ModelRegistry::deactivate；IPC 未单独暴露——用 activate 空值不可行，
    //   这里直接 remove 另一个模型路径验证。为完整性：remove 应在 deactivate 后成功。
    //   当前 Core remove 拒绝 active 模型 = 预期行为。）
}

TEST(model_ipc_import_rejects_outside_incoming) {
    ModelFixture fx;
    CHECK(fx.start());
    // 路径不在收件目录 → 拒绝（防任意文件读取）
    auto r = fx.ipc("MODEL_IMPORT",
                    R"({"src_path":"C:/Windows/system32/config","model_id":"evil"})");
    CHECK_EQ(resp_status(r), 1);
    r = fx.ipc("MODEL_IMPORT",
               R"({"src_path":"/etc/passwd","model_id":"evil"})");
    CHECK_EQ(resp_status(r), 1);
}

TEST(model_ipc_rejects_bad_model_id) {
    ModelFixture fx;
    CHECK(fx.start());
    // path traversal / 非法字符
    for (const char* bad : {"../evil", "a/b", "a b", "", "模型"}) {
        std::string params = std::string(R"({"model_id":")") + bad + R"("})";
        auto r = fx.ipc("MODEL_INSTALL", params);
        CHECK_EQ(resp_status(r), 1);
    }
}

TEST(model_ipc_validate_fails_without_validator) {
    // 无 validator（板端未注入 RKNN 校验器）→ validate 明确报错，不静默
    ModelFixture fx(false);
    CHECK(fx.start());
    const std::string src = fx.make_incoming("m.rknn");
    (void)fx.ipc("MODEL_IMPORT",
                 R"({"src_path":")" + json_escape(src) + R"(","model_id":"m1"})");
    auto r = fx.ipc("MODEL_VALIDATE", R"({"model_id":"m1"})");
    CHECK_EQ(resp_status(r), 1);
    const auto* err = r.find("error");
    CHECK(err != nullptr && (err->as_string().find("validator") != std::string::npos ||
                             err->as_string().find("VALIDATOR_NOT_CONFIGURED") != std::string::npos));
}

TEST(model_ipc_activate_requires_installed) {
    ModelFixture fx;
    CHECK(fx.start());
    // 未 install 就 activate → 拒绝
    auto r = fx.ipc("MODEL_ACTIVATE", R"({"model_id":"not-exist"})");
    CHECK_EQ(resp_status(r), 1);
}
