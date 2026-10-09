#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// ★ 2026-10-06 架构整理：本头只用 RknnModelInfo（info() 的返回类型），不需要整个
//   RKNNEngine 引擎头 —— 契约类型已下沉到 common/RknnContract.hpp。
//   需要真正跑推理的 ModelBackend.cpp 仍然 include "rknn/RKNNEngine.hpp"（它持有
//   RKNNEngine 成员），这是"后端用引擎"的合理依赖方向，保留。
#include "common/RknnContract.hpp"

namespace ttbox::core {

// 推理后端种类：kAuto 按平台/后缀推断，kRknn = NPU，kOnnx = CPU
enum class ModelBackendKind {
    kAuto,
    kRknn,
    kOnnx,
};

// 推理后端统一接口（RKNN 与 ONNX 共用；输入输出统一以 float32 承载）
class IModelBackend {
public:
    virtual ~IModelBackend() = default;
    // 后端种类标识
    virtual ModelBackendKind kind() const = 0;
    // 加载模型并完成初始化；失败填 error 返回 false
    virtual bool init(const std::string& model_path, std::string* error = nullptr) = 0;
    // 释放模型与会话资源
    virtual void destroy() = 0;
    // 是否已成功初始化
    virtual bool initialized() const = 0;
    // 模型输入/输出结构信息（init 成功后有效）
    virtual const RknnModelInfo& info() const = 0;
    // 执行一次推理：输入原始 buffer，输出按张量展平的 float 列表
    virtual bool infer(const void* input, size_t input_size,
                       std::vector<std::vector<float>>& outputs,
                       std::string* error = nullptr) = 0;
};

// 后端工厂：创建后端实例，或按文件后缀探测后端种类
class ModelBackendFactory {
public:
    // 创建指定种类的后端（不可用时返回 UnsupportedBackend 占位并填 error）
    static std::unique_ptr<IModelBackend> create(ModelBackendKind kind,
                                                  std::string* error = nullptr);
    // 按文件扩展名探测后端种类（.rknn/.onnx，其它返回 kAuto）
    static ModelBackendKind detect_kind(const std::string& model_path);
};

}  // namespace ttbox::core
