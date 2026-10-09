// ModelRegistry.hpp — 模型仓库（阶段 A-8）
//
// 目录结构（root = <project>/models，可用 ModelRegistryOptions 覆盖）：
//   models/
//   ├── registry/       # 仓库状态（active.json：当前激活模型）
//   ├── installed/      # 已验证、可运行模型（<model_id>/ 目录）
//   ├── staging/        # 用户上传/转换中的模型
//   ├── cache/          # 转换缓存（预留）
//   └── quarantine/     # 验证失败模型
//
// 每个 installed 模型：
//   installed/<model_id>/
//   ├── manifest.json   # 索引：version/sha256/signature/origin/converter_version/runtime_version/status
//   ├── metadata.json   # ModelAdapter.analyze 生成的 ModelMetadata（20 项）
//   ├── model.rknn      # 可运行模型文件
//   ├── source/         # 来源文件（预留：原始 onnx/转换产物）
//   └── validation/     # 验证结果（预留）
//
// 支持操作：list / import / validate / install / activate / deactivate / remove
// 禁止删除正在使用（active）的模型。
//
// 校验策略：validate 通过注入的 validator 回调完成（生产环境 = RKNNEngine +
// ModelAdapter.analyze；单测可注入模拟）。ModelRegistry 本身不直接依赖 RKNN，
// 保证 host（Windows/无 NPU）可编译与单测。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/Json.hpp"

namespace ttbox::core {

// 模型状态
enum class ModelStatus : int {
    kUnknown = 0,
    kDiscovered = 1,
    kValidating = 2,
    kReady = 3,
    kSelected = 4,
    kLoading = 5,
    kRunning = 6,
    kFailed = 7,
    kInvalid = 8,
    kStaging = 9,
    kInstalled = 10,
    kQuarantined = 11,
};

const char* model_status_name(ModelStatus s);

// 模型索引 manifest（预留云端字段：version/sha256/signature/origin/converter/runtime）
struct ModelManifest {
    std::string model_id;
    std::string label;
    std::string version = "1.0.0";
    std::string format = "rknn";
    std::string source_format;
    std::string task = "detect";
    std::string framework;
    std::string input_layout;
    std::string input_dtype;
    std::string quantization;
    std::string output_format;
    std::string model_family;
    std::string hardware = "rk3588";
    std::string sha256;
    std::string signature;
    std::string origin = "local";
    std::string converter_version;
    std::string runtime_version;
    ModelStatus status = ModelStatus::kStaging;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t output_count = 0;
    uint32_t class_count = 0;
    std::vector<std::string> class_names;
    uint32_t rknn_concurrency = 1;
    std::string worker_cores;
    JsonValue to_json() const;
    static ModelManifest from_json(const JsonValue& v);
};

struct ModelRecord {
    std::string model_id;
    std::string name;
    std::string version;
    std::string format;
    std::string path;
    ModelManifest manifest;
    std::string checksum;
    JsonValue metadata = JsonValue::object();
    ModelStatus status = ModelStatus::kUnknown;
    std::string failure_code;
    std::string failure_message;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    bool selected = false;
    bool running = false;

    JsonValue to_json() const;
};

// 仓库配置
struct ModelRegistryOptions {
    std::string root = "";   // models/ 根目录（空 = TTBOX_PROJECT_ROOT/models）
    bool create_dirs = true; // 初始化时创建子目录
};

class ModelRegistry {
public:
    explicit ModelRegistry(ModelRegistryOptions opts = {});

    // 初始化：确保目录结构存在。失败返回 false（可重试）。
    bool init(std::string* error = nullptr);

    // 校验回调：注入的 validator 应加载 rknn 并返回 metadata（Json）。
    // 返回 true = 验证通过。签名：
    //   bool(const std::string& rknn_path, JsonValue* metadata_out, std::string* error)
    void set_validator(std::function<bool(const std::string&, JsonValue*, std::string*)> v) {
        validator_ = std::move(v);
    }

    // ---- 仓库操作 ----
    // import：将模型文件复制到 staging/<model_id>/model.rknn，写 manifest.json
    bool import(const std::string& src_rknn, const std::string& model_id,
                const ModelManifest& manifest, std::string* error = nullptr);

    // validate：staging → 调用 validator；通过则写 validation/ok.json 并更新 manifest
    // （不移动目录，失败留 quarantine 由调用方处理 / quarantine() 移动）
    bool validate(const std::string& model_id, std::string* error = nullptr);

    // install：staging → installed（校验通过才允许）；生成 metadata.json 快照
    bool install(const std::string& model_id, std::string* error = nullptr);

    // 移动验证失败的模型到 quarantine
    bool quarantine(const std::string& model_id, const std::string& reason,
                    std::string* error = nullptr);

    // activate：设置当前激活模型（写入 registry/active.json）。
    // 激活前会做一次 validator 加载校验（保证"激活即可用"）；失败不改 active 并恢复旧值。
    bool activate(const std::string& model_id, std::string* error = nullptr);

    // deactivate：清空激活状态
    bool deactivate(std::string* error = nullptr);

    // 当前激活模型 id（无返回空串）
    std::string active_model() const;

    // remove：删除 installed 模型。正在使用（= active）时拒绝。
    bool remove(const std::string& model_id, std::string* error = nullptr);

    // list：兼容旧调用的 manifest 清单
    std::vector<ModelManifest> list() const;

    // records：Registry 唯一模型快照，包含校验状态、checksum、metadata 和选择状态。
    std::vector<ModelRecord> records() const;

    // refresh：重新扫描模型目录并刷新内存快照；不会由每次 API 请求触发。
    bool refresh(std::string* error = nullptr);

    // set_concurrency：更新已安装模型的 RKNN 并发（1~3）并原子写回 manifest。
    bool set_concurrency(const std::string& model_id, int count, std::string* error = nullptr);

    // ---- 路径辅助 ----
    std::string root_dir() const { return root_; }
    std::string staging_dir(const std::string& model_id) const;
    std::string installed_dir(const std::string& model_id) const;
    std::string rknn_path(const std::string& model_id) const;
    std::string metadata_path(const std::string& model_id) const;
    std::string manifest_path(const std::string& model_id) const;

private:
    bool ensure_dirs(std::string* error);
    bool write_active(const std::string& model_id, std::string* error);
    std::string read_active() const;
    bool exists(const std::string& path) const;
    bool copy_file(const std::string& src, const std::string& dst, std::string* error) const;
    bool scan_locked(std::vector<ModelRecord>* out, std::string* error) const;
    bool refresh_locked(std::string* error) const;
    bool build_record_locked(const std::string& model_dir, const std::string& model_id,
                             ModelRecord* out) const;
    std::string model_dir_locked(const std::string& model_id) const;

    std::string root_;
    ModelRegistryOptions opts_;
    std::function<bool(const std::string&, JsonValue*, std::string*)> validator_;
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const std::vector<ModelRecord>> snapshot_;
};

}  // namespace ttbox::core
