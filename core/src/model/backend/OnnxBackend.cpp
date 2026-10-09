// OnnxBackend.cpp — ONNX Runtime CPU 推理后端（TTBOX_CORE_HAS_ONNX 时编译）
#include "model/backend/OnnxBackend.hpp"

#if defined(TTBOX_CORE_HAS_ONNX) && TTBOX_CORE_HAS_ONNX

#include <cstring>
#include <onnxruntime_cxx_api.h>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace ttbox::core {

namespace {
// RKNN tensor_type：FLOAT32=0（与 DecodeNMS::read_elem 的 kTypeFloat32 一致）
// ONNX 元素类型 → RKNN tensor_type 数值（供 DecodeNMS 统一读取）
constexpr int kOrtTypeToRknn(ONNXTensorElementDataType t) {
    switch (t) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return 0;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return 1;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: return 2;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return 3;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16: return 4;
        default: return 0;
    }
}
}  // namespace

// ONNX Runtime 会话及其输入/输出名称与指针缓存（PImpl，隔离 ORT 头）
struct OnnxBackend::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "ttbox_onnx"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::vector<const char*> input_ptrs;
    std::vector<const char*> output_ptrs;
};

// 构造：配置会话选项（单线程、全量图优化）
OnnxBackend::OnnxBackend() : impl_(std::make_unique<Impl>()) {
    impl_->options.SetIntraOpNumThreads(1);
    impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
}

// 析构：Impl 由 unique_ptr 自动释放
OnnxBackend::~OnnxBackend() = default;

// 加载模型、读取输入/输出形状与类型并缓存张量名称
bool OnnxBackend::init(const std::string& model_path, std::string* error) {
    destroy();
    try {
        // Windows 下 ONNX Runtime 路径参数是宽字符（ORTCHAR_T = wchar_t），
        // 需要做 UTF-8 → 宽字符转换（含中文路径兼容）
#if defined(_WIN32)
        int wlen = MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) {
            if (error) *error = "ONNX model path UTF-8 conversion failed";
            return false;
        }
        std::wstring wpath(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, &wpath[0], wlen);
        impl_->session = std::make_unique<Ort::Session>(impl_->env, wpath.c_str(), impl_->options);
#else
        impl_->session = std::make_unique<Ort::Session>(impl_->env, model_path.c_str(), impl_->options);
#endif
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("ONNX session create failed: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        if (error) *error = std::string("ONNX session create failed: ") + e.what();
        return false;
    }

    Ort::AllocatorWithDefaultOptions alloc;
    const size_t n_in = impl_->session->GetInputCount();
    const size_t n_out = impl_->session->GetOutputCount();

    auto read_shape = [&](size_t i, bool input) -> std::vector<uint32_t> {
        Ort::TypeInfo ti = input ? impl_->session->GetInputTypeInfo(i)
                                 : impl_->session->GetOutputTypeInfo(i);
        auto tsi = ti.GetTensorTypeAndShapeInfo();
        const auto shape = tsi.GetShape();
        std::vector<uint32_t> dims;
        for (int64_t d : shape) {
            if (d < 0) d = 1;  // 动态维按 1 处理，推理时以实际 tensor 为准
            dims.push_back(static_cast<uint32_t>(d));
        }
        return dims;
    };
    auto read_elem_type = [&](size_t i, bool input) -> ONNXTensorElementDataType {
        Ort::TypeInfo ti = input ? impl_->session->GetInputTypeInfo(i)
                                 : impl_->session->GetOutputTypeInfo(i);
        return ti.GetTensorTypeAndShapeInfo().GetElementType();
    };

    // 输入 0（当前单输入模型）
    if (n_in >= 1) {
        const auto dims = read_shape(0, true);
        info_.input_dims = dims;
        info_.input_width = dims.size() >= 3 ? dims[dims.size() - 1] : 1;
        info_.input_height = dims.size() >= 2 ? dims[dims.size() - 2] : 1;
        info_.input_type = kOrtTypeToRknn(read_elem_type(0, true));
        info_.input_fmt = 0;  // NCHW
        uint64_t elems = 1;
        for (uint32_t d : dims) elems *= d;
        info_.input_size = static_cast<uint32_t>(elems * sizeof(float));
        impl_->input_names.push_back(
            std::string(impl_->session->GetInputNameAllocated(0, alloc).get()));
    }

    // 输出（全部）
    info_.n_outputs = static_cast<uint32_t>(n_out);
    info_.outputs.reserve(n_out);
    info_.output_n_elems.reserve(n_out);
    info_.output_sizes.reserve(n_out);
    for (size_t i = 0; i < n_out; ++i) {
        const auto dims = read_shape(i, false);
        RknnOutputInfo oi;
        oi.dims = dims;
        uint64_t elems = 1;
        for (uint32_t d : dims) elems *= d;
        oi.n_elems = static_cast<uint32_t>(elems);
        oi.size = static_cast<uint32_t>(elems * sizeof(float));
        oi.type = kOrtTypeToRknn(read_elem_type(i, false));
        oi.fmt = 0;  // NCHW
        oi.scale = 0.0f;
        oi.zp = 0;
        info_.outputs.push_back(oi);
        info_.output_n_elems.push_back(oi.n_elems);
        info_.output_sizes.push_back(oi.size);
        impl_->output_names.push_back(
            std::string(impl_->session->GetOutputNameAllocated(i, alloc).get()));
    }

    impl_->input_ptrs.reserve(impl_->input_names.size());
    impl_->output_ptrs.reserve(impl_->output_names.size());
    for (const auto& n : impl_->input_names) impl_->input_ptrs.push_back(n.c_str());
    for (const auto& n : impl_->output_names) impl_->output_ptrs.push_back(n.c_str());

    inited_ = true;
    return true;
}

// 释放会话与缓存，复位模型信息
void OnnxBackend::destroy() {
    if (!impl_) return;
    impl_->session.reset();
    impl_->input_names.clear();
    impl_->output_names.clear();
    impl_->input_ptrs.clear();
    impl_->output_ptrs.clear();
    info_ = RknnModelInfo{};
    inited_ = false;
}

// 用输入张量运行会话，并把各输出拷贝为 float 向量
bool OnnxBackend::infer(const void* input, size_t input_size,
                        std::vector<std::vector<float>>& outputs,
                        std::string* error) {
    if (!inited_ || !impl_->session) {
        if (error) *error = "ONNX backend not initialized";
        return false;
    }
    if (impl_->input_names.empty() || impl_->output_names.empty()) {
        if (error) *error = "ONNX model has no input/output";
        return false;
    }
    try {
        const auto& udims = info_.input_dims;
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> dims(udims.begin(), udims.end());
        Ort::Value in = Ort::Value::CreateTensor<float>(
            mem, const_cast<float*>(static_cast<const float*>(input)),
            input_size / sizeof(float), dims.data(), dims.size());

        auto results = impl_->session->Run(
            Ort::RunOptions{nullptr}, impl_->input_ptrs.data(), &in, 1,
            impl_->output_ptrs.data(), impl_->output_names.size());

        outputs.clear();
        outputs.reserve(results.size());
        for (const auto& res : results) {
            const float* data = res.GetTensorData<float>();
            const size_t count = res.GetTensorTypeAndShapeInfo().GetElementCount();
            outputs.emplace_back(data, data + count);
        }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("ONNX run failed: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        if (error) *error = std::string("ONNX run failed: ") + e.what();
        return false;
    }
}

}  // namespace ttbox::core
#endif  // TTBOX_CORE_HAS_ONNX


