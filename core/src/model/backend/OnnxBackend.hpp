#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "model/backend/ModelBackend.hpp"
// ★ 2026-10-06 架构整理：只用 RknnModelInfo / RknnOutputInfo 契约，已下沉到
//   common/RknnContract.hpp；本后端不跑 RKNN，无需依赖引擎头。
#include "common/RknnContract.hpp"

namespace ttbox::core {

// ONNX Runtime CPU 后端（Windows/宿主机；TTBOX_CORE_HAS_ONNX 时可用）。
// 说明：
//   - 与 RKNN 后端共用 IModelBackend 接口，输入/输出统一以 float32 承载
//     （ONNX 原生 float；输出 RknnOutputInfo.type=0 使 DecodeNMS 直接按 float 读）。
//   - 输入 buffer 需符合模型输入张量的布局（NCHW float），由上层预处理保证。
#if defined(TTBOX_CORE_HAS_ONNX) && TTBOX_CORE_HAS_ONNX
class OnnxBackend final : public IModelBackend {
public:
    // 构造：创建 ORT 会话选项（单线程、全量图优化）
    OnnxBackend();
    // 析构：释放 ORT 会话
    ~OnnxBackend() override;
    OnnxBackend(const OnnxBackend&) = delete;
    OnnxBackend& operator=(const OnnxBackend&) = delete;

    ModelBackendKind kind() const override { return ModelBackendKind::kOnnx; }
    // 加载 ONNX 模型并采集输入/输出张量结构
    bool init(const std::string& model_path, std::string* error = nullptr) override;
    // 释放会话与名称缓存，复位模型信息
    void destroy() override;
    bool initialized() const override { return inited_; }
    const RknnModelInfo& info() const override { return info_; }
    // 执行一次 CPU 推理：输入 NCHW float，输出各张量展平
    bool infer(const void* input, size_t input_size,
               std::vector<std::vector<float>>& outputs,
               std::string* error = nullptr) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RknnModelInfo info_;
    bool inited_ = false;
};
#endif

}  // namespace ttbox::core
