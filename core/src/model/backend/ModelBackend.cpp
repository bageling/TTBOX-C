#include "model/backend/ModelBackend.hpp"
#if defined(TTBOX_CORE_HAS_ONNX) && TTBOX_CORE_HAS_ONNX
#include "model/backend/OnnxBackend.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>

#if defined(TTBOX_CORE_HAS_RKNN) && TTBOX_CORE_HAS_RKNN
#include "rknn/RKNNEngine.hpp"
#endif

namespace ttbox::core {
namespace {

// 占位后端：该平台/构建未启用对应后端，任何操作都返回失败
class UnsupportedBackend final : public IModelBackend {
public:
    explicit UnsupportedBackend(ModelBackendKind kind) : kind_(kind) {}
    ModelBackendKind kind() const override { return kind_; }
    bool init(const std::string&, std::string* error) override {
        if (error) *error = kind_ == ModelBackendKind::kOnnx
            ? "ONNX backend is not enabled in this build"
            : "RKNN backend is not available on this platform";
        return false;
    }
    void destroy() override {}
    bool initialized() const override { return false; }
    const RknnModelInfo& info() const override { return info_; }
    bool infer(const void*, size_t, std::vector<std::vector<float>>&, std::string* error) override {
        if (error) *error = "backend is unavailable";
        return false;
    }
private:
    ModelBackendKind kind_;
    RknnModelInfo info_;
};

#if defined(TTBOX_CORE_HAS_RKNN) && TTBOX_CORE_HAS_RKNN
// RKNN 后端：包装 RKNNEngine（板端 NPU）
class RknnBackend final : public IModelBackend {
public:
    ModelBackendKind kind() const override { return ModelBackendKind::kRknn; }
    bool init(const std::string& path, std::string* error) override {
        RKNNEngine::Params params;
        params.model_path = path;
        return engine_.init(params, error);
    }
    void destroy() override { engine_.destroy(); }
    bool initialized() const override { return engine_.initialized(); }
    const RknnModelInfo& info() const override { return engine_.info(); }
    bool infer(const void* input, size_t input_size,
               std::vector<std::vector<float>>& outputs,
               std::string* error) override {
        return engine_.infer(input, input_size, outputs, error);
    }
private:
    RKNNEngine engine_;
};
#endif

}  // namespace

// 按扩展名（大小写不敏感）判定后端种类
ModelBackendKind ModelBackendFactory::detect_kind(const std::string& model_path) {
    std::string ext = std::filesystem::path(model_path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".rknn") return ModelBackendKind::kRknn;
    if (ext == ".onnx") return ModelBackendKind::kOnnx;
    return ModelBackendKind::kAuto;
}

// 创建后端实例：kAuto 按平台选择，编译开关缺失时回退 UnsupportedBackend
std::unique_ptr<IModelBackend> ModelBackendFactory::create(ModelBackendKind kind,
                                                             std::string* error) {
    if (kind == ModelBackendKind::kAuto) {
#if defined(_WIN32)
        kind = ModelBackendKind::kOnnx;
#else
        kind = ModelBackendKind::kRknn;
#endif
    }
#if defined(TTBOX_CORE_HAS_RKNN) && TTBOX_CORE_HAS_RKNN
    if (kind == ModelBackendKind::kRknn) return std::make_unique<RknnBackend>();
#endif
    if (error) *error = kind == ModelBackendKind::kOnnx
        ? "ONNX backend is not enabled in this build"
        : "RKNN backend is not available in this build";
    return std::make_unique<UnsupportedBackend>(kind);
}

}  // namespace ttbox::core

