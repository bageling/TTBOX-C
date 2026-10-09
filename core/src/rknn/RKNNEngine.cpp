// RKNNEngine.cpp — RKNN C API 推理实现（librknnrt）
/*
 * TTBOX 文件说明
 *
 * 文件：RKNNEngine.cpp
 *
 * 作用：
 *   NPU（神经网络处理器）推理引擎。
 *   负责把图片数据送入 NPU 执行 AI 计算，并取回结果。
 *
 * 小白理解：
 *   NPU 是专门用来跑 AI 的芯片。这个模块负责：
 *   1. 把图片数据送进 NPU
 *   2. 让 NPU 执行 AI 模型计算
 *   3. 把计算结果取出来供后续处理
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "rknn/RKNNEngine.hpp"

#if defined(_WIN32)
// Windows 占位：无 NPU 硬件，CMake 仅在 Unix 编译本文件。
namespace ttbox::core {
}
#else

#include <chrono>
#include <cstring>
#include <cstdio>

#include "rknn_api.h"
#include "common/Logger.hpp"

namespace ttbox::core {

namespace {

using clock = std::chrono::steady_clock;

// 计算 [a,b] 区间的微秒数（分阶段耗时统计用）。
uint32_t elapsed_us(const clock::time_point& a, const clock::time_point& b) {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
}

// 把 RKNN 张量类型枚举转成可读字符串（仅日志用）。
const char* type_name(int t) {
    switch (t) {
        case RKNN_TENSOR_FLOAT32: return "FLOAT32";
        case RKNN_TENSOR_FLOAT16: return "FLOAT16";
        case RKNN_TENSOR_INT8: return "INT8";
        case RKNN_TENSOR_UINT8: return "UINT8";
        case RKNN_TENSOR_INT16: return "INT16";
        default: return "OTHER";
    }
}

// 把 RKNN 张量布局枚举转成可读字符串（仅日志用）。
const char* fmt_name(int f) {
    switch (f) {
        case RKNN_TENSOR_NCHW: return "NCHW";
        case RKNN_TENSOR_NHWC: return "NHWC";
        case RKNN_TENSOR_UNDEFINED: return "UNDEFINED";
        default: return "OTHER";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

// 引擎 pimpl：持有 runtime 上下文与零拷贝 tensor 内存句柄。
struct RKNNEngine::Impl {
    rknn_context ctx = 0;  // 0 = 未初始化
    rknn_tensor_mem* input_mem = nullptr;
    std::vector<rknn_tensor_mem*> output_mems;
    uint32_t input_mem_size = 0;
    std::vector<uint32_t> output_mem_sizes;
};

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

// 构造：只分配 Impl，不加载模型（真正初始化在 init()）。
RKNNEngine::RKNNEngine() : impl_(std::make_unique<Impl>()) {}

// 析构：释放 runtime 与全部 tensor 内存。
RKNNEngine::~RKNNEngine() {
    destroy();
}

// 初始化：加载 .rknn 模型、查询输入输出属性、按需设置 NPU core_mask。
bool RKNNEngine::init(const Params& params, std::string* error) {
    if (inited_) {
        if (error) *error = "RKNNEngine 已初始化";
        return false;
    }
    if (params.model_path.empty()) {
        if (error) *error = "model_path 为空";
        return false;
    }
    params_ = params;

    const auto t0 = clock::now();

    if (params_.test_init_hook) {
        if (!params_.test_init_hook(&info_, error)) return false;
        inited_ = true;
        load_ms_ = static_cast<double>(elapsed_us(t0, clock::now())) / 1000.0;
        return true;
    }

    // ---- 1. rknn_init：加载模型 + 初始化 runtime ----
    uint32_t init_flag = 0;
    if (params_.disable_cache_flush) {
        init_flag |= RKNN_FLAG_DISABLE_FLUSH_INPUT_MEM_CACHE |
                     RKNN_FLAG_DISABLE_FLUSH_OUTPUT_MEM_CACHE;
    }
    int rc = rknn_init(&impl_->ctx, const_cast<char*>(params_.model_path.c_str()), 0,
                       init_flag, nullptr);
    if (rc != RKNN_SUCC) {
        if (error) *error = "rknn_init 失败（rc=" + std::to_string(rc) + "）: " + params_.model_path;
        return false;
    }
    TTBOX_LOG_INFO("rknn_init OK: " + params_.model_path);

    // ---- 2. 查询输入/输出数量 ----
    rknn_input_output_num io_num{};
    rc = rknn_query(impl_->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (rc != RKNN_SUCC) {
        if (error) *error = "rknn_query(IN_OUT_NUM) 失败 rc=" + std::to_string(rc);
        destroy();
        return false;
    }
    info_.n_inputs = io_num.n_input;
    info_.n_outputs = io_num.n_output;
    if (info_.n_inputs < 1) {
        if (error) *error = "模型无输入";
        destroy();
        return false;
    }

    // ---- 3. 查询输入 0 属性 ----
    rknn_tensor_attr input_attr{};
    input_attr.index = 0;
    rc = rknn_query(impl_->ctx, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));
    if (rc != RKNN_SUCC) {
        if (error) *error = "rknn_query(INPUT_ATTR) 失败 rc=" + std::to_string(rc);
        destroy();
        return false;
    }
    info_.input_type = input_attr.type;
    info_.input_fmt = input_attr.fmt;
    info_.input_dims.assign(input_attr.dims, input_attr.dims + input_attr.n_dims);
    // dims 顺序：NCHW 时 [1,C,H,W]；NHWC 时 [1,H,W,C]
    if (input_attr.fmt == RKNN_TENSOR_NHWC && input_attr.n_dims >= 4) {
        info_.input_height = input_attr.dims[1];
        info_.input_width = input_attr.dims[2];
    } else if (input_attr.n_dims >= 4) {
        info_.input_height = input_attr.dims[2];
        info_.input_width = input_attr.dims[3];
    }
    info_.input_size = input_attr.size;  // 对齐后字节数
    // T1.14：输入量化参数（rknn_api.h:293-296，字段恒存在，无"查询失败"分支）。
    // fl 不存——DFP 输入当前不存在于任何已知模型，避免死字段。
    info_.input_qnt_type = input_attr.qnt_type;
    info_.input_scale = input_attr.scale;
    info_.input_zp = input_attr.zp;

    // ---- 4. 查询输出属性 ----
    info_.output_n_elems.clear();
    info_.output_sizes.clear();
    info_.outputs.clear();
    for (uint32_t i = 0; i < info_.n_outputs; ++i) {
        rknn_tensor_attr out_attr{};
        out_attr.index = i;
        rc = rknn_query(impl_->ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
        if (rc != RKNN_SUCC) {
            if (error) *error = "rknn_query(OUTPUT_ATTR[" + std::to_string(i) + "]) 失败";
            destroy();
            return false;
        }
        info_.output_n_elems.push_back(out_attr.n_elems);
        info_.output_sizes.push_back(out_attr.size);
        RknnOutputInfo oi;
        oi.n_elems = out_attr.n_elems;
        oi.size = out_attr.size;
        oi.type = out_attr.type;
        oi.fmt = out_attr.fmt;
        oi.scale = out_attr.scale;
        oi.zp = out_attr.zp;
        oi.dims.assign(out_attr.dims, out_attr.dims + out_attr.n_dims);
        info_.outputs.push_back(std::move(oi));
    }

    // ---- 5. core_mask（来自 config/model 配置，不写死）----
    if (params_.core_mask != 0) {
        rc = rknn_set_core_mask(impl_->ctx, static_cast<rknn_core_mask>(params_.core_mask));
        if (rc != RKNN_SUCC) {
            TTBOX_LOG_WARN("rknn_set_core_mask(" + std::to_string(params_.core_mask) +
                           ") 失败 rc=" + std::to_string(rc) + "（继续用默认）");
        } else {
            TTBOX_LOG_INFO("core_mask 已设置: " + std::to_string(params_.core_mask));
        }
    }

    load_ms_ = static_cast<double>(elapsed_us(t0, clock::now())) / 1000.0;
    inited_ = true;

    {
        std::string log = "模型信息: 输入 " + std::to_string(info_.n_inputs) +
                          " 输出 " + std::to_string(info_.n_outputs) +
                          " | 输入 " + std::to_string(info_.input_width) + "x" +
                          std::to_string(info_.input_height) + " " +
                          type_name(info_.input_type) + " " + fmt_name(info_.input_fmt) +
                          " size=" + std::to_string(info_.input_size) + "B" +
                          // T1.14（G5 板端钉死项）：打印输入量化实测值，用于确认
                          // zp==-128 / qnt==AFFINE 是否成立、并钉死 w_stride 单位语义。
                          " qnt=" + std::string(get_qnt_type_string(
                              static_cast<rknn_tensor_qnt_type>(info_.input_qnt_type))) +
                          " scale=" + std::to_string(info_.input_scale) +
                          " zp=" + std::to_string(info_.input_zp) +
                          " | 加载 " + std::to_string(load_ms_) + "ms";
        TTBOX_LOG_INFO(log);
    }
    return true;
}

bool RKNNEngine::init_zero_copy(std::string* error) {
    if (!inited_) {
        if (error) *error = "引擎未初始化";
        return false;
    }
    if (zero_copy_ready_) return true;
    // 输入量化分类：唯一判定入口（design §G2.3.1）。谓词 =
    //   INT8 ∧ NHWC ∧ qnt_type==AFFINE_ASYMMETRIC ∧ zp==-128  → kXorShift128
    //   UINT8 ∧ NHWC                                            → kUint8Native（方案 D 预留）
    //   其余（非零均值 zp!=-128 / DFP / NONE / FP16 / NCHW）    → kCompatible
    const InputPassMode mode = classify_input_pass(
        info_.input_type, info_.input_fmt,
        info_.input_qnt_type, info_.input_scale, info_.input_zp);
    // 【陷阱一修复】kCompatible（含非零均值 INT8 / DFP / NONE / FP16 / NCHW）或
    //   pass_through=0 时，必须在此 return false ⇒ zero_copy_ready_ 保持 false
    //   ⇒ WorkerPool 走 set_input()/run() 兼容 I/O（runtime 内部量化）。
    //   只拒 bind_external_input_fd 不够：那样 zero_copy_ready_ 仍为 true，
    //   WorkerPool 会照常 XOR → 非零均值模型静默算错（design §G2.3.1）。
    if (!params_.pass_through || mode == InputPassMode::kCompatible) {
        if (error) {
            *error = "输入量化不满足 XOR 快路径（非 INT8/NHWC/AFFINE 或 zp != -128），回退兼容 I/O";
        }
        TTBOX_LOG_WARN("RKNN 输入量化守卫：拒绝零拷贝快路径（qnt=" +
            std::string(get_qnt_type_string(
                static_cast<rknn_tensor_qnt_type>(info_.input_qnt_type))) +
            " scale=" + std::to_string(info_.input_scale) +
            " zp=" + std::to_string(info_.input_zp) + "），回退兼容 I/O");
        return false;
    }

    // =====================================================================
    // ★ 2026-09-23 重构：拆成「只查询」→「才分配绑定」两个阶段。
    //   原因：rknn_set_io_mem 把输入绑成"用户内存"后，**没有解绑 API**。
    //   原来在绑定输入之后才去查询输出属性 / 创建输出 mem，任一步失败就直接
    //   return false —— 已创建的 mem 不销毁（NPU 内存泄漏），ctx 停在半绑态，
    //   而兼容路径的 rknn_inputs_set 与半绑态冲突 ⇒ 该 worker **永久** 100%
    //   推理失败，上层却只当"零拷贝不可用"照常跑（静默坏掉）。
    //   查询是廉价的、也是最可能失败的环节 ⇒ 全部前置，绑定阶段就几乎不会失败。
    // =====================================================================

    // ---- 阶段一：只查询 ----
    rknn_tensor_attr input_attr{};
    input_attr.index = 0;
    int rc = rknn_query(impl_->ctx, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));
    if (rc != RKNN_SUCC) {
        if (error) *error = "查询零拷贝输入属性失败 rc=" + std::to_string(rc);
        return false;
    }
    rknn_input_output_num io_num{};
    rc = rknn_query(impl_->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (rc != RKNN_SUCC) {
        if (error) *error = "查询零拷贝 I/O 数量失败";
        return false;
    }
    std::vector<rknn_tensor_attr> output_attrs;
    output_attrs.reserve(io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        rknn_tensor_attr output_attr{};
        output_attr.index = i;
        rc = rknn_query(impl_->ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attr, sizeof(output_attr));
        if (rc != RKNN_SUCC) {
            if (error) *error = "查询零拷贝输出属性失败 index=" + std::to_string(i);
            return false;
        }
        output_attrs.push_back(output_attr);
    }

    // ---- 阶段二：分配 + 绑定（任何失败都整体回滚）----
    // 回滚销毁**已创建的全部** mem，不是只销毁失败那一个。
    auto rollback_zero_copy = [this]() {
        if (impl_->input_mem) {
            rknn_destroy_mem(impl_->ctx, impl_->input_mem);
            impl_->input_mem = nullptr;
        }
        impl_->input_mem_size = 0;
        for (auto* mem : impl_->output_mems) {
            if (mem) rknn_destroy_mem(impl_->ctx, mem);
        }
        impl_->output_mems.clear();
        impl_->output_mem_sizes.clear();
    };

    rknn_tensor_attr input_binding = input_attr;
    // 此处 mode 必为 kXorShift128 / kUint8Native（kCompatible 已在上面 return false）。
    input_binding.type = (mode == InputPassMode::kUint8Native) ? RKNN_TENSOR_UINT8
                                                              : RKNN_TENSOR_INT8;
    input_binding.fmt = RKNN_TENSOR_NHWC;
    input_binding.pass_through = 1;
    impl_->input_mem = rknn_create_mem(impl_->ctx, input_binding.size_with_stride);
    if (!impl_->input_mem) {
        if (error) *error = "rknn_create_mem 输入失败";
        return false;  // 还没绑定 ⇒ ctx 干净，可直接走兼容 I/O
    }
    rc = rknn_set_io_mem(impl_->ctx, impl_->input_mem, &input_binding);
    if (rc != RKNN_SUCC) {
        // 绑定未成功 ⇒ ctx 仍干净，回滚后走兼容 I/O（与修复前行为一致）。
        // 只有"绑定已成功"之后的失败才置 zero_copy_fatal_。
        rollback_zero_copy();
        if (error) *error = "rknn_set_io_mem 输入失败 rc=" + std::to_string(rc);
        return false;
    }
    impl_->input_mem_size = input_binding.size_with_stride;

    for (uint32_t i = 0; i < output_attrs.size(); ++i) {
        rknn_tensor_attr output_attr = output_attrs[i];
        auto* mem = rknn_create_mem(impl_->ctx, output_attr.size_with_stride);
        if (!mem) {
            rollback_zero_copy();
            // ★ 输入已绑定且无解绑 API ⇒ 这个 ctx 不能再拿去跑兼容 I/O，
            //   标致命让上层直接判定 worker 失败，别留一个永久静默坏的 worker。
            zero_copy_fatal_ = true;
            if (error) {
                *error = "创建零拷贝输出内存失败 index=" + std::to_string(i) +
                         "（输入已绑定且无法解绑，本引擎不可用）";
            }
            return false;
        }
        if (rknn_set_io_mem(impl_->ctx, mem, &output_attr) != RKNN_SUCC) {
            rknn_destroy_mem(impl_->ctx, mem);
            rollback_zero_copy();
            zero_copy_fatal_ = true;
            if (error) {
                *error = "绑定零拷贝输出失败 index=" + std::to_string(i) +
                         "（输入已绑定且无法解绑，本引擎不可用）";
            }
            return false;
        }
        impl_->output_mems.push_back(mem);
        impl_->output_mem_sizes.push_back(output_attr.size_with_stride);
    }
    zero_copy_ready_ = true;
    pass_mode_ = mode;  // ← 写入点②：唯一写入 classify_input_pass() 结论之处，与 zero_copy_ready_ 同处（design §G2.3.1 结构保证 1）；另两处（缺省 kCompatible、destroy 复位）不携带分类结论
    // external DMA-BUF 直绑只对 UINT8 原生成立；INT8（kXorShift128）直绑会绕过 XOR。
    // 这里一次性定死，WorkerPool 据此决定"要不要每帧尝试直绑"——避免每帧一次
    // rknn_query + 一条 WARN（144 fps 下每秒 144 条，2026-09-23 板端实测刷爆 journal）。
    external_dma_supported_ = (mode == InputPassMode::kUint8Native);
    TTBOX_LOG_INFO("RKNN 零拷贝 I/O 已绑定: input=" +
                   std::to_string(impl_->input_mem_size) + " bytes, outputs=" +
                   std::to_string(impl_->output_mems.size()) +
                   ", external_dma=" + (external_dma_supported_ ? "可用" : "不可用（非 UINT8 原生输入，直绑恒被拒）"));
    return true;
}

// 返回零拷贝输入 mem 的虚拟地址（未建立时为空）。
void* RKNNEngine::input_memory() const {
    return (impl_ && impl_->input_mem) ? impl_->input_mem->virt_addr : nullptr;
}

// 返回零拷贝输入 mem 的字节数。
size_t RKNNEngine::input_memory_size() const {
    return impl_ ? impl_->input_mem_size : 0;
}

// 返回第 index 个零拷贝输出 mem 的虚拟地址。
void* RKNNEngine::output_memory(uint32_t index) const {
    if (!impl_ || index >= impl_->output_mems.size() || !impl_->output_mems[index]) return nullptr;
    return impl_->output_mems[index]->virt_addr;
}

// 返回第 index 个零拷贝输出 mem 的字节数。
size_t RKNNEngine::output_memory_size(uint32_t index) const {
    return impl_ && index < impl_->output_mem_sizes.size() ? impl_->output_mem_sizes[index] : 0;
}

// 在已绑定的零拷贝 I/O 上执行一次 NPU 推理。
bool RKNNEngine::run_zero_copy(std::string* error) {
    if (!inited_ || !zero_copy_ready_) {
        if (error) *error = "零拷贝 I/O 未初始化";
        return false;
    }
    const auto t0 = clock::now();
    const int rc = rknn_run(impl_->ctx, nullptr);
    stats_.run.add(elapsed_us(t0, clock::now()));
    if (rc != RKNN_SUCC) {
        if (error) *error = "rknn_run 零拷贝失败 rc=" + std::to_string(rc);
        return false;
    }
    return true;
}

// 把外部 DMA-BUF 绑定为 RKNN 输入（仅 UINT8 原生模式放行）。
bool RKNNEngine::bind_external_input_fd(int fd, void* virt_addr, size_t size,
                                        std::string* error) {
    if (!inited_ || !zero_copy_ready_ || !impl_) {
        if (error) *error = "RKNN 零拷贝输入尚未初始化";
        return false;
    }
    if (fd < 0 || virt_addr == nullptr || size == 0) {
        if (error) *error = "外部 DMA-BUF 输入参数无效";
        return false;
    }
    rknn_tensor_attr attr{};
    attr.index = 0;
    if (rknn_query(impl_->ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr)) != RKNN_SUCC) {
        if (error) *error = "查询 RKNN 输入属性失败";
        return false;
    }
    // 外部 DMA-BUF 直传不做 XOR，仅 UINT8 原生（方案 D）正确；
    // INT8（kXorShift128）直传会绕过 XOR → 必错，故一律拒绝（design §G2.3.1）。
    // 拒绝发生在 rknn_create_mem_from_fd 之前 ⇒ 无副作用，impl_->input_mem 不被破坏。
    //
    // ★ 日志只报一次：本函数在逐帧路径上被调用（WorkerPool 每帧尝试直绑），
    //   INT8 模型下恒定被拒 ⇒ 144 fps 时每秒 144 条 WARN 会把 journal 刷爆
    //   （2026-09-23 板端实测）。后续调用静默返回 false，理由由
    //   external_dma_supported()/init 阶段那条 INFO 承载。
    if (input_pass_mode() != InputPassMode::kUint8Native) {
        if (error) {
            *error = "external DMA-BUF 直传仅支持 UINT8 原生输入；当前模式非 kUint8Native，拒绝（回退拷贝+XOR）";
        }
        if (!dma_bind_reject_logged_) {
            dma_bind_reject_logged_ = true;
            TTBOX_LOG_WARN("拒绝 external DMA-BUF 直绑：input_pass_mode != kUint8Native（避免绕过 XOR）；"
                           "本模型为 INT8，后续每帧走拷贝+XOR，此提示不再重复");
        }
        return false;
    }
    attr.type = RKNN_TENSOR_UINT8;
    attr.fmt = RKNN_TENSOR_NHWC;
    attr.pass_through = 1;
    if (size < attr.size_with_stride) {
        if (error) *error = "外部 DMA-BUF 小于 RKNN 输入 stride";
        return false;
    }
    auto* external = rknn_create_mem_from_fd(impl_->ctx, fd, virt_addr,
                                               static_cast<uint32_t>(size), 0);
    if (!external) {
        if (error) *error = "rknn_create_mem_from_fd 失败";
        return false;
    }
    if (rknn_set_io_mem(impl_->ctx, external, &attr) != RKNN_SUCC) {
        rknn_destroy_mem(impl_->ctx, external);
        if (error) *error = "外部 DMA-BUF 输入绑定失败";
        return false;
    }
    if (impl_->input_mem && impl_->input_mem != external) {
        rknn_destroy_mem(impl_->ctx, impl_->input_mem);
    }
    impl_->input_mem = external;
    impl_->input_mem_size = size;
    // 注：不设置任何"快路径已激活"标志——external DMA-BUF 直传只可能是 UINT8 原生
    // （见上方守卫），其搬运语义由 input_pass_mode()==kUint8Native 表达，无需额外位。
    return true;
}

// 销毁 runtime 与全部 tensor 内存，并复位所有快路径状态标志。
void RKNNEngine::destroy() {
    if (impl_ && impl_->ctx != 0) {
        if (impl_->input_mem) {
            rknn_destroy_mem(impl_->ctx, impl_->input_mem);
            impl_->input_mem = nullptr;
        }
        for (auto* mem : impl_->output_mems) {
            if (mem) rknn_destroy_mem(impl_->ctx, mem);
        }
        impl_->output_mems.clear();
        impl_->output_mem_sizes.clear();
        rknn_destroy(impl_->ctx);
        impl_->ctx = 0;
    }
    inited_ = false;
    zero_copy_ready_ = false;
    zero_copy_fatal_ = false;             // ← 与 zero_copy_ready_ 同时复位（新 ctx 是干净的）
    pass_mode_ = InputPassMode::kCompatible;  // ← 写入点③：复位，保证 destroy→init 重建一致（不携带分类结论）
    external_dma_supported_ = false;          // ← 与 pass_mode_ 同时复位（换模型后重新判定）
    dma_bind_reject_logged_ = false;          // ← 复位后允许新模型再报一次被拒提示
}

// 清空分阶段耗时统计（预热后调用）。
void RKNNEngine::reset_stats() {
    stats_.set_input.clear();
    stats_.run.clear();
    stats_.output.clear();
    stats_.total.clear();
}

// ---- 预热：加载后先空跑几帧，把 NPU 上下文/权重常驻这一步提前做掉 ----
// 为什么必要：首帧推理要初始化 NPU 上下文，实测比稳态慢一个量级，用户感知就是
// 「点开始后要等一会儿才出结果」。预热用全零输入跑 rounds 次，然后 reset_stats()
// 把这些样本清掉，避免污染后续的 infer_ms / e2e_ms 统计（存在性 ≠ 生效：只跑不算，
// 得确认统计里看不到预热的影响）。
bool RKNNEngine::warmup(int rounds, std::string* error) {
    if (!inited_) {
        if (error) *error = "引擎未初始化";
        return false;
    }
    if (rounds <= 0) {
        return true;
    }
    const size_t bytes = info_.input_size > 0 ? static_cast<size_t>(info_.input_size)
                                              : input_memory_size();
    if (bytes == 0) {
        if (error) *error = "预热失败：无法确定输入字节数";
        return false;
    }
    std::vector<uint8_t> zeros(bytes, 0);
    const auto t0 = clock::now();
    for (int i = 0; i < rounds; ++i) {
        std::vector<std::vector<float>> outs;
        std::string e;
        if (!infer(zeros.data(), bytes, outs, &e)) {
            if (error) *error = "预热第 " + std::to_string(i + 1) + " 次失败: " + e;
            return false;
        }
    }
    reset_stats();
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    TTBOX_LOG_INFO("RKNNEngine 预热完成: " + std::to_string(rounds) + " 次 / " +
                   std::to_string(static_cast<int>(ms)) + " ms（已清空统计，不计入 infer_ms）");
    return true;
}

// ---------------------------------------------------------------------------
// 推理
// ---------------------------------------------------------------------------

// 设置输入：零拷贝模式拷入 input mem，否则走兼容 rknn_inputs_set 路径。
bool RKNNEngine::set_input(const void* buf, size_t size, std::string* error) {
    if (!inited_) {
        if (error) *error = "引擎未初始化";
        return false;
    }
    if (buf == nullptr || size == 0) {
        if (error) *error = "输入 buffer 无效";
        return false;
    }
    if (zero_copy_ready_) {
        if (size > impl_->input_mem_size || impl_->input_mem == nullptr) {
            if (error) *error = "零拷贝输入尺寸超限";
            return false;
        }
        std::memcpy(impl_->input_mem->virt_addr, buf, size);
        return true;
    }
    rknn_input in{};
    in.index = 0;
    // A-6 实测（黄瓦 INT8 模型 + OIP 对齐）：INT8 模型若以 in.type=INT8 喂 0-255 原始像素，
    // runtime 会把像素当"已量化 INT8 值"，128-255 溢出为负 → 输出错乱（1 检测 vs Python 0）。
    // rknnlite 喂 UINT8 原始像素由 runtime 量化；此处 INT8 模型同样改喂 UINT8 像素，与 rknnlite 对齐。
    // 2026-09-03 真实链路排查（yolo261n-rk3588, COCO 640 FP16）：本模型若按声明类型 FP16 喂
    // 0-1 归一化 half（Preprocess /255），输出类别通道全 ~0（sigmoid≈0.502，模型"失明"）；
    // 改喂 UINT8 原始像素 0-255（与 INT8 模型同路径，runtime 自行量化）→ class 0/2 等正确激活。
    // 故 FP16 模型同样以 UINT8 原始像素喂入；此改动与 rknnlite 参考实现完全一致。
    in.type = static_cast<rknn_tensor_type>(info_.input_type);
    if (info_.input_type == 2 || info_.input_type == 1) {  // INT8 / FP16：喂原始像素（0-255），让 runtime 量化
        in.type = RKNN_TENSOR_UINT8;
    }
    in.size = static_cast<uint32_t>(size);
    in.fmt = static_cast<rknn_tensor_format>(info_.input_fmt);
    in.buf = const_cast<void*>(buf);
    // 兼容 I/O 路径始终喂 UINT8 原始像素，pass_through=0 交给 runtime
    // 做类型转换/量化；与 Python(rknnlite) 行为一致，禁止在此处直通。
    in.pass_through = 0;

    const auto t0 = clock::now();
    const int rc = rknn_inputs_set(impl_->ctx, 1, &in);
    stats_.set_input.add(elapsed_us(t0, clock::now()));
    if (rc != RKNN_SUCC) {
        if (error) *error = "rknn_inputs_set 失败 rc=" + std::to_string(rc);
        return false;
    }
    // 【陷阱二修复】旧实现在此有 `pass_through_active_ = params_.pass_through;`，
    // 使回退路径也能把"快路径已激活"置真，与 zero_copy_ready_ 结构上解耦。
    // 该赋值已删除：pass_through_active() 现由 (zero_copy_ready_ && mode==kXorShift128)
    // 派生，回退路径永不使其为真（design §G2.3.1 结构保证 3）。
    return true;
}

// 执行一次 NPU 推理（兼容 I/O 路径）。
bool RKNNEngine::run(std::string* error) {
    if (!inited_) {
        if (error) *error = "引擎未初始化";
        return false;
    }
    if (params_.test_run_hook) return params_.test_run_hook(error);
    const auto t0 = clock::now();
    const int rc = rknn_run(impl_->ctx, nullptr);
    stats_.run.add(elapsed_us(t0, clock::now()));
    if (rc != RKNN_SUCC) {
        if (error) *error = "rknn_run 失败 rc=" + std::to_string(rc);
        return false;
    }
    return true;
}

namespace {

// 通用输出获取：want_float 控制是否转 float32（1=转换，0=原生零转换）
bool outputs_get_impl(RKNNEngine::Impl* impl, rknn_context ctx,
                      uint32_t n_outputs, int want_float,
                      void** out_bufs, size_t* out_sizes,
                      StatsCollector* output_stats, std::string* error) {
    std::vector<rknn_output> outs(n_outputs);
    for (uint32_t i = 0; i < n_outputs; ++i) {
        outs[i].index = i;
        outs[i].want_float = want_float;
        outs[i].is_prealloc = 1;
        outs[i].buf = out_bufs[i];
        outs[i].size = static_cast<uint32_t>(out_sizes[i]);  // 字节数
    }
    const auto t0 = clock::now();
    const int rc = rknn_outputs_get(ctx, n_outputs, outs.data(), nullptr);
    output_stats->add(elapsed_us(t0, clock::now()));
    if (rc != RKNN_SUCC) {
        if (error) *error = std::string("rknn_outputs_get 失败 rc=") + std::to_string(rc);
        return false;
    }
    // 预分配模式下需主动 release（释放 runtime 内部引用）
    rknn_outputs_release(ctx, n_outputs, outs.data());
    (void)impl;
    return true;
}

}  // namespace

// 取输出并转为 float32（want_float=1）。
bool RKNNEngine::get_outputs(void** out_bufs, size_t* out_sizes, std::string* error) {
    if (!inited_) {
        if (error) *error = "引擎未初始化";
        return false;
    }
    if (out_bufs == nullptr || out_sizes == nullptr) {
        if (error) *error = "输出参数无效";
        return false;
    }
    return outputs_get_impl(impl_.get(), impl_->ctx, info_.n_outputs, 1,
                            out_bufs, out_sizes, &stats_.output, error);
}

// 取原生输出、不做 float 转换（want_float=0，供 Decode 直供）。
bool RKNNEngine::get_raw_outputs(void** out_bufs, size_t* out_sizes, std::string* error) {
    if (!inited_) {
        if (error) *error = "引擎未初始化";
        return false;
    }
    if (out_bufs == nullptr || out_sizes == nullptr) {
        if (error) *error = "输出参数无效";
        return false;
    }
    return outputs_get_impl(impl_.get(), impl_->ctx, info_.n_outputs, 0,
                            out_bufs, out_sizes, &stats_.output, error);
}

// 便捷整帧推理：set_input + run + 取 float32 输出。
bool RKNNEngine::infer(const void* input_buf, size_t input_size,
                       std::vector<std::vector<float>>& outputs,
                       std::string* error) {
    if (!inited_) {
        if (error) *error = "引擎未初始化";
        return false;
    }
    const auto t0 = clock::now();
    if (!set_input(input_buf, input_size, error)) {
        return false;
    }
    if (!run(error)) {
        return false;
    }
    // 预分配输出 buffer（float32）
    outputs.resize(info_.n_outputs);
    std::vector<void*> bufs(info_.n_outputs);
    std::vector<size_t> sizes(info_.n_outputs);
    for (uint32_t i = 0; i < info_.n_outputs; ++i) {
        const size_t n_float = info_.output_n_elems[i];
        outputs[i].resize(n_float);
        bufs[i] = outputs[i].data();
        sizes[i] = n_float * sizeof(float);
    }
    if (!get_outputs(bufs.data(), sizes.data(), error)) {
        return false;
    }
    stats_.total.add(elapsed_us(t0, clock::now()));
    return true;
}

}  // namespace ttbox::core

#endif  // !_WIN32
