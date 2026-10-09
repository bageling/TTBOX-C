// RKNNEngine.hpp — RKNN C API 推理引擎（阶段 A-4，单 Worker）
//
// 目标：
//   - 全部 C++（librknnrt.so + rknn_api.h），Python 不进入高速 AI 链路
//   - 输入零拷贝优先：set_input 直接引用调用方 buffer（RGA 输出 mmap va），
//     pass_through=1 时绕过 runtime 内部复制/格式转换
//   - 模型/配置外置：model_path / core_mask / 输入尺寸全部由调用方从 config 提供
//   - 单 Worker 耗时统计：set_input / run / output / total（min/avg/p50/p95/p99/max）
//
// 接口边界（后续 A-5 Worker / A-6 Decode 依赖）：
//   RKNNEngine::infer(input_buf) -> 输出张量（float32，want_float=1）
/*
 * TTBOX 文件说明
 *
 * 文件：RKNNEngine.hpp
 *
 * 作用：
 *   NPU 推理引擎的定义。
 *
 * 小白理解：
 *   这是 RKNNEngine.cpp 的头文件，定义了 NPU 推理的接口。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/Stats.hpp"
// ★ 契约值类型（RknnOutputInfo / RknnModelInfo / RknnStageStats）已下沉到
//   common/RknnContract.hpp（2026-10-06 架构整理）。它们描述"模型输入输出长什么样"，
//   是数据不是行为，放在这里会让 model 层为了读几个字段而 include 整个引擎头，
//   从而和 rknn 形成模块级依赖环。下沉后 model 只依赖 common 叶子。
#include "common/RknnContract.hpp"
#include "rknn/InputQuant.hpp"

namespace ttbox::core {

class RKNNEngine {
public:
    struct Impl;  // pimpl（A-6 outputs_get_impl 辅助函数需要访问）
    // 引擎初始化参数（模型路径、core_mask、pass_through、缓存刷新、测试 hook）。
    struct Params {
        std::string model_path;       // .rknn 路径（由 config/ModelStore 提供，不硬编码）
        int core_mask = 0;            // 0=RKNN_NPU_CORE_AUTO（来自 config/model 配置，不写死）
        bool pass_through = true;     // true=INT8/NHWC 启用输入零拷贝（WorkerPool 做 XOR 量化映射）；
                                      // 其它模型自动回退兼容 I/O
        bool disable_cache_flush = false;  // true=跳过 CPU↔NPU 缓存同步（降低推理延迟）
                                          // 可通过 config rknn_disable_cache_flush 开启
        // 测试专用注入点；生产调用不设置，仍走真实 librknnrt。
        std::function<bool(RknnModelInfo*, std::string*)> test_init_hook;
        std::function<bool(std::string*)> test_run_hook;
    };

    RKNNEngine();
    ~RKNNEngine();
    RKNNEngine(const RKNNEngine&) = delete;
    RKNNEngine& operator=(const RKNNEngine&) = delete;

    // 加载模型 + 初始化 runtime + 设置 core_mask。成功返回 true。
    bool init(const Params& params, std::string* error = nullptr);
    void destroy();

    bool initialized() const { return inited_; }
    const RknnModelInfo& info() const { return info_; }
    const RknnStageStats& stats() const { return stats_; }
    void reset_stats();  // 清空分阶段统计（预热后调用）
    // 预热：加载后空跑 rounds 次（全零输入），把 NPU 上下文初始化提前做掉；
    // 完成后自动 reset_stats()，预热样本不计入 infer_ms/e2e_ms。rounds<=0 = 不预热。
    bool warmup(int rounds = 3, std::string* error = nullptr);
    double load_ms() const { return load_ms_; }          // 模型加载 + init 耗时（ms）

    // 输入搬运模式。写入点共三处，全部列举（禁再声称"唯一写入点"）：
    //   ① 本成员缺省初始化 = kCompatible（声明处，永不为未定义值）；
    //   ② init_zero_copy() 内 `pass_mode_ = mode`（RKNNEngine.cpp:308）——**唯一**写入
    //      classify_input_pass() 分类结论之处，与 zero_copy_ready_=true 同处赋值；
    //      被量化守卫拒绝时该函数提前 return，不写（保持 ① 的 kCompatible）。
    //   ③ destroy() 复位 = kCompatible（RKNNEngine.cpp:417），保证 destroy→init 重建一致。
    // WorkerPool 只消费本结论，禁止自行判定。
    InputPassMode input_pass_mode() const { return pass_mode_; }

    // ★ 语义收窄为"XOR 快路径已激活"，并**结构上蕴含 zero_copy_ready_**：
    //   即 pass_through_active()==true ⇒ zero_copy_ready_==true。
    //   删除了旧实现里三处对独立标志位的直接赋值（design §G2.3.1 陷阱二）。
    bool pass_through_active() const {
        return zero_copy_ready_ && pass_mode_ == InputPassMode::kXorShift128;
    }

    // RK3588 高性能路径：初始化一次性绑定的 RKNN tensor memory。
    // 失败时调用方继续使用 set_input/run/get_raw_outputs 回退路径。
    bool init_zero_copy(std::string* error = nullptr);
    // 零拷贝输入/输出 mem 的虚拟地址与大小访问器（未就绪时返回空/0）。
    void* input_memory() const;
    size_t input_memory_size() const;
    void* output_memory(uint32_t index) const;
    size_t output_memory_size(uint32_t index) const;
    bool zero_copy_ready() const { return zero_copy_ready_; }
    // ★ 2026-09-23：零拷贝**半绑致命**标志。init_zero_copy() 一旦把输入
    //   rknn_set_io_mem 绑上、之后某步失败，ctx 就没有解绑 API，兼容 I/O 会与之冲突。
    //   此时返回 true ⇒ 调用方必须判定本引擎不可用（重建 worker），
    //   **不能**当成"零拷贝不可用"继续用同一个 ctx（那样该 worker 会永久静默失败）。
    bool zero_copy_fatal() const { return zero_copy_fatal_; }
    bool run_zero_copy(std::string* error = nullptr);

    // external DMA-BUF 直绑是否**可能**成立（= 输入是 UINT8 原生）。
    // 判定在 init_zero_copy() 一次性做出（mode == kUint8Native），运行时只读。
    //   · true  → 每帧可尝试 bind_external_input_fd()
    //   · false → 模型是 INT8（kXorShift128），直绑会绕过 XOR，恒不成立 ⇒
    //             调用方应**根本不要每帧尝试**，否则每帧一次 rknn_query + 一条 WARN
    //             （144 fps 下 = 每秒 144 条日志刷爆 journal，2026-09-23 板端实测）。
    bool external_dma_supported() const { return external_dma_supported_; }

    // 绑定外部 DMA-BUF 为 RKNN 输入，避免每帧复制到 runtime 自有内存。
    // 注意：不支持时（external_dma_supported()==false）本函数只**首次**打一条 WARN，
    // 之后静默返回 false —— 调用方不得依赖它做每帧回退判定。
    bool bind_external_input_fd(int fd, void* virt_addr, size_t size,
                                std::string* error = nullptr);

    // 设置输入（零拷贝：直接引用 buf，不复制）。size 为 buf 有效字节。
    bool set_input(const void* buf, size_t size, std::string* error = nullptr);

    // 执行 NPU 推理
    bool run(std::string* error = nullptr);

    // 获取输出（want_float=1，float32；调用方预分配 out_bufs[i] 至少 info_.output_sizes[i]*4 字节）
    bool get_outputs(void** out_bufs, size_t* out_sizes, std::string* error = nullptr);

    // 获取原生输出（want_float=0，零转换；调用方按 info_.outputs[i].size 预分配 buffer）。
    // A-6 Decode 直供路径：禁止无意义的 float 转换。
    bool get_raw_outputs(void** out_bufs, size_t* out_sizes, std::string* error = nullptr);

    // 便捷：一帧完整推理（set_input + run + outputs），输出为 float32 向量
    bool infer(const void* input_buf, size_t input_size,
               std::vector<std::vector<float>>& outputs,
               std::string* error = nullptr);

private:
    std::unique_ptr<Impl> impl_;
    Params params_;
    RknnModelInfo info_;
    RknnStageStats stats_;
    double load_ms_ = 0.0;
    bool inited_ = false;
    // 输入搬运模式（写入点①缺省=kCompatible；②init_zero_copy 写分类结论；③destroy 复位；
    // 详见 input_pass_mode() 上方注释）。默认兼容 I/O —— 永远正确、永远可用。
    InputPassMode pass_mode_ = InputPassMode::kCompatible;
    bool zero_copy_ready_ = false;
    // ★ 零拷贝半绑致命标志（见 zero_copy_fatal() 注释；destroy 时一并复位）。
    bool zero_copy_fatal_ = false;
    // external DMA-BUF 直绑可行性（init_zero_copy 一次性判定；destroy 同 pass_mode_ 复位）。
    bool external_dma_supported_ = false;
    // "直绑被拒"只报一次的标志（防每帧刷屏）。
    bool dma_bind_reject_logged_ = false;
};

}  // namespace ttbox::core
