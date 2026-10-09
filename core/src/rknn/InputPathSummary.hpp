// InputPathSummary.hpp — 模型输入通路诊断：单 worker 快照 + 跨 worker 聚合（header-only）
//
// 目的（T1.15 可观测性）：换 INT8 模型后，"到底走了哪条输入快路径"原先只写在串口日志里
//   （RKNNEngine.cpp:249-253 会打印实测 qnt/scale/zp），面板上看不到。结果是：模型换了，
//   到底吃到零拷贝还是静默回落兼容 I/O，只能靠人盯着 UART 猜。
//   本头文件把该结论变成**纯数据**，经
//     WorkerStats.input_path（本文件）
//       → CoreRuntime::collect_metrics（薄接线）
//         → PipelineMetrics.model_*（Metrics.hpp）
//           → IPC GET_STATUS.metrics（IpcServer.cpp）
//             → Web state.model_input（ttbox-web.py）→ 面板
//   一路投影到面板。
//
// ★ 为什么聚合逻辑放在 header-only 纯函数里（而不是直接写在 CoreRuntime.cpp 里）：
//   CoreRuntime.cpp / WorkerPool.cpp 都**不在 Windows 默认构建**里（见 core/CMakeLists.txt
//   的「条件源闭包（P0-1）」——它们无条件引用 WorkerPool/RKNNEngine，只在板端编）。
//   若把聚合散写进去，本机（无 aarch64 交叉工具链）就连一行都验不了。搬进 header 后：
//     ① 判定/聚合逻辑在 host（Windows 标量）与板端各跑一遍同一份断言；
//     ② CoreRuntime 侧只剩"取值 → 塞进 metrics"的机械搬运，出错面被压到最小。
//   这是"把不可验证的板端代码换成可验证的纯函数"的固定套路，与 InputQuant.hpp 同源。
//
// 线程模型（重要，勿想当然）：
//   · 每个字段各自是原子量 ⇒ 单字段读写无数据竞争；
//   · 但字段之间**没有**跨字段原子性 —— IPC 轮询可能读到"pass_mode 已更新、scale 还是上一版"
//     （热重载换模型的瞬间）。对诊断显示无害（下一次轮询即自洽），故不引入整体锁：
//     加锁会让板端每帧路径上的写侧与 IPC 读侧互相等待，代价远大于收益。
//   · 除 external_dma_bound 外（首帧绑定成功后才置 true，运行期可变），其余字段均在
//     worker start() 内一次性写入、此后只读。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "rknn/InputQuant.hpp"

namespace ttbox::core {

// ---------------------------------------------------------------------------
// float 的原子表示：IEEE-754 位模式
// C++17 没有 std::atomic<float>（那是 C++20 才加的具名特化）。定点（×1e6 取整）会损精度，
// 位模式（memcpy 到 uint32_t）是**唯一无损**的原子表示。两个 helper 就是它的全部实现。
// ---------------------------------------------------------------------------
inline void store_float_bits(std::atomic<uint32_t>& slot, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    slot.store(bits, std::memory_order_relaxed);
}

inline float load_float_bits(const std::atomic<uint32_t>& slot) {
    const uint32_t bits = slot.load(std::memory_order_relaxed);
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// 单 worker 输入通路诊断（原子量；写入 = worker start() 内 engine init 之后）
// ---------------------------------------------------------------------------
struct InputPathStats {
    // InputPassMode 的 int 值：0=kCompatible 1=kXorShift128 2=kUint8Native
    // （缺省 0 = 兼容 I/O，与 RKNNEngine::pass_mode_ 的缺省语义一致：永不为未定义值）
    std::atomic<int32_t> pass_mode{0};
    std::atomic<int32_t> input_type{-1};      // rknn_tensor_type（-1 = 未取到）
    std::atomic<int32_t> input_fmt{-1};       // rknn_tensor_format
    std::atomic<int32_t> input_qnt_type{-1};  // rknn_tensor_qnt_type
    std::atomic<int32_t> input_zp{0};         // 反量化 zero point（快路径要求 == -128）
    std::atomic<uint32_t> input_scale_bits{0};// 反量化 scale 的 IEEE-754 位模式（见上方说明）
    std::atomic<uint32_t> input_w{0};         // 模型输入宽（与采集分辨率比对，可发现配置漂移）
    std::atomic<uint32_t> input_h{0};
    std::atomic<bool> zero_copy_ready{false};        // 引擎零拷贝输入 mem 已就绪
    std::atomic<bool> fast_path_active{false};       // XOR 快路径已激活（== pass_through_active）
    std::atomic<bool> external_dma_requested{false}; // config 开关（rknn_external_dma_input）
    std::atomic<bool> external_dma_bound{false};     // 实际绑定成功（运行期可变）

    // 将所有诊断字段复位为缺省值（缺省 pass_mode=0=compatible）。
    void reset() {
        pass_mode.store(0, std::memory_order_relaxed);
        input_type.store(-1, std::memory_order_relaxed);
        input_fmt.store(-1, std::memory_order_relaxed);
        input_qnt_type.store(-1, std::memory_order_relaxed);
        input_zp.store(0, std::memory_order_relaxed);
        input_scale_bits.store(0, std::memory_order_relaxed);
        input_w.store(0, std::memory_order_relaxed);
        input_h.store(0, std::memory_order_relaxed);
        zero_copy_ready.store(false, std::memory_order_relaxed);
        fast_path_active.store(false, std::memory_order_relaxed);
        external_dma_requested.store(false, std::memory_order_relaxed);
        external_dma_bound.store(false, std::memory_order_relaxed);
    }
};

// 上述原子量的非原子投影（供纯函数聚合；避免纯函数里出现原子类型）
struct ModelInputPathSnapshot {
    int pass_mode = 0;
    int input_type = -1;
    int input_fmt = -1;
    int input_qnt_type = -1;
    int input_zp = 0;
    float input_scale = 0.0f;
    uint32_t input_w = 0;
    uint32_t input_h = 0;
    bool zero_copy_ready = false;
    bool fast_path_active = false;
    bool external_dma_requested = false;
    bool external_dma_bound = false;
};

// 原子 → POD（每个字段各取一次 relaxed 读；不保证字段间同版，理由见文件头）
inline ModelInputPathSnapshot snapshot_input_path(const InputPathStats& s) {
    ModelInputPathSnapshot o;
    o.pass_mode = static_cast<int>(s.pass_mode.load(std::memory_order_relaxed));
    o.input_type = static_cast<int>(s.input_type.load(std::memory_order_relaxed));
    o.input_fmt = static_cast<int>(s.input_fmt.load(std::memory_order_relaxed));
    o.input_qnt_type = static_cast<int>(s.input_qnt_type.load(std::memory_order_relaxed));
    o.input_zp = static_cast<int>(s.input_zp.load(std::memory_order_relaxed));
    o.input_scale = load_float_bits(s.input_scale_bits);
    o.input_w = s.input_w.load(std::memory_order_relaxed);
    o.input_h = s.input_h.load(std::memory_order_relaxed);
    o.zero_copy_ready = s.zero_copy_ready.load(std::memory_order_relaxed);
    o.fast_path_active = s.fast_path_active.load(std::memory_order_relaxed);
    o.external_dma_requested = s.external_dma_requested.load(std::memory_order_relaxed);
    o.external_dma_bound = s.external_dma_bound.load(std::memory_order_relaxed);
    return o;
}

// ---------------------------------------------------------------------------
// 人话说明：输入通路**为什么**是现在这条（空串 = 无异常，面板不显示该行）
//
// 为什么要做成纯函数而不是散在 Web 里：这些判据（FP16 不支持 / zp≠-128 / DFP / NCHW）
// 与 classify_input_pass 的谓词是同一套知识。放在本头文件 ⇒ 判据一变，
// 说明文字与判定结论同处修改；放 Web 就会变成第二份漂移的谓词实现。
//
// 文案口径（勿写成"错误"）：回落兼容 I/O **不是故障** —— FP16 模型在 TTBOX 上完全能跑，
// 只是每帧 set_input 要 12.6ms（INT8 只要 0.07ms）。所以文案是"回落 / 代价"，不是"报错"。
// ---------------------------------------------------------------------------
inline std::string describe_input_path(int pass_mode, int input_type, int input_fmt,
                                       int input_qnt_type, int input_zp,
                                       bool zero_copy_ready, bool fast_path_active) {
    const bool is_xor_mode = (pass_mode == static_cast<int>(InputPassMode::kXorShift128));
    const bool is_uint8_mode = (pass_mode == static_cast<int>(InputPassMode::kUint8Native));

    if (is_xor_mode && fast_path_active && zero_copy_ready) {
        return std::string();  // 快路径正常：无需解释
    }
    if (is_xor_mode && zero_copy_ready && !fast_path_active) {
        // 理论上 kXorShift128 ∧ zero_copy_ready ⇒ pass_through_active，此分支是防御性写法：
        // 一旦出现说明 pass_mode_ 与 zero_copy_ready_ 的赋值被拆散（design §G2.3.1 陷阱二）。
        return "内部状态异常：分类为 XOR 快路径且零拷贝已就绪，但快路径未激活（请报缺陷）";
    }
    if (is_xor_mode && !zero_copy_ready) {
        return "分类为 XOR 快路径，但零拷贝输入未建立 ⇒ 每帧回落 set_input（见启动日志 WARN）";
    }
    if (is_uint8_mode) {
        return std::string();  // UINT8 原生 = 最优路径：无需解释
    }

    // ---- 以下 = kCompatible（兼容 I/O）。逐条给出"卡在哪一条判据" ----
    if (input_type == kTensorTypeFp16) {
        return "输入为 FP16：快路径只支持 INT8(零均值) 或 UINT8 原生，故回落兼容 I/O"
               "（代价：每帧 set_input 约 12.6ms，INT8 约 0.07ms）";
    }
    if (input_type == kTensorTypeFp32) {
        return "输入为 FP32：快路径只支持 INT8(零均值) 或 UINT8 原生，故回落兼容 I/O";
    }
    if (input_type == kTensorTypeInt8) {
        if (input_fmt != kTensorFmtNhwc) {
            return std::string("INT8 但布局为 ") + tensor_fmt_name(input_fmt) +
                   "（快路径要求 NHWC）⇒ 回落兼容 I/O";
        }
        if (input_qnt_type != kQntAffineAsym) {
            return std::string("INT8 但量化类型为 ") + tensor_qnt_type_name(input_qnt_type) +
                   "（快路径要求 AFFINE_ASYMMETRIC）⇒ 回落兼容 I/O";
        }
        if (input_zp != -128) {
            return std::string("INT8 但 zp=") + std::to_string(input_zp) +
                   " ≠ -128（非零均值）⇒ XOR 后不等于 u·scale，快路径不可用，回落兼容 I/O";
        }
    }
    // 其余组合（UINT8 但 NCHW、INT16 等）：无专门文案，如实报出类型/布局即可
    return std::string("输入 ") + tensor_type_name(input_type) + "/" + tensor_fmt_name(input_fmt) +
           " 不在快路径白名单内 ⇒ 回落兼容 I/O";
}

// ---------------------------------------------------------------------------
// 跨 worker 聚合结果（可直接塞进 PipelineMetrics，字段一一对应）
// ---------------------------------------------------------------------------
struct ModelInputPathSummary {
    // 分类字段（模型全 worker 共享 ⇒ 取首个 worker 为准）
    std::string pass_mode_name = "unknown";  // 三种模式名之一；无 worker ⇒ "unknown"
    int input_type = -1;
    std::string input_type_name = "unknown";
    int input_fmt = -1;
    std::string input_fmt_name = "unknown";
    int input_qnt_type = -1;
    std::string input_qnt_name = "unknown";
    int input_zp = 0;
    double input_scale = 0.0;
    uint32_t input_w = 0;
    uint32_t input_h = 0;
    // 汇总字段
    uint32_t workers_total = 0;
    uint32_t workers_zero_copy = 0;
    uint32_t workers_fast_path = 0;
    bool zero_copy_ready = false;       // 至少一个 worker 就绪
    bool fast_path_active = false;      // 至少一个 worker 走在 XOR 快路径
    bool external_dma_requested = false;// 开关（取首 worker 的 config 值）
    bool external_dma_bound = false;    // 至少一个 worker 真的绑上了
    // 人话说明：为什么是现在这条通路（空串 = 无异常/无 runtime ⇒ 面板不显示该行）。
    // 文案的唯一来源是下面的 describe_input_path()。
    std::string note;
};

// 聚合规则（逐条都有理由，勿随手改）：
//   · 分类字段取 **首个** 而不是"多数票"：模型是全 worker 共享的同一个 .rknn ⇒ 分类结论
//     结构上必然一致（InputPassMode 在 init_zero_copy 里由同一份 rknn_query 结果判定）。
//     若真出现不一致，那是"某 worker 初始化失败后回落"的异常，用下面的计数暴露，
//     而不是把异常平均成一个好看的多数值。
//   · 布尔/计数类用"至少一个 / 计数"：板端 3 worker 里只有 2 个走快路径时，
//     workers_fast_path=2/workers_total=3 直接说明问题（若用"全部"语义则退化成 0，
//     反而看不出"大部分是好的"）。
//   · n == 0（runtime 未起 / 无 worker）⇒ 全字段保持 unknown/0/未就绪，
//     语义与 PipelineMetrics 注释里的 "unavailable" 一致；绝不臆造 compatible。
inline ModelInputPathSummary summarize_input_path(const ModelInputPathSnapshot* items, size_t n) {
    ModelInputPathSummary out;
    if (items == nullptr || n == 0) return out;  // ⇒ 全 unknown / 0 / false

    out.workers_total = static_cast<uint32_t>(n);
    const ModelInputPathSnapshot& first = items[0];

    const InputPassMode mode = static_cast<InputPassMode>(first.pass_mode);
    // 只有取值落在闭集内才采信名字；越界（理论上不可达，防御未来加模式后旧 Web 误读）
    // ⇒ 回落 "unknown" 而非硬转成 "compatible"。
    const char* name = input_pass_mode_name(mode);
    out.pass_mode_name = name;
    out.input_type = first.input_type;
    out.input_type_name = tensor_type_name(first.input_type);
    out.input_fmt = first.input_fmt;
    out.input_fmt_name = tensor_fmt_name(first.input_fmt);
    out.input_qnt_type = first.input_qnt_type;
    out.input_qnt_name = tensor_qnt_type_name(first.input_qnt_type);
    out.input_zp = first.input_zp;
    out.input_scale = static_cast<double>(first.input_scale);
    out.input_w = first.input_w;
    out.input_h = first.input_h;
    out.external_dma_requested = first.external_dma_requested;

    for (size_t i = 0; i < n; ++i) {
        if (items[i].zero_copy_ready) ++out.workers_zero_copy;
        if (items[i].fast_path_active) ++out.workers_fast_path;
        if (items[i].external_dma_bound) out.external_dma_bound = true;
    }
    out.zero_copy_ready = out.workers_zero_copy > 0;
    out.fast_path_active = out.workers_fast_path > 0;

    out.note = describe_input_path(first.pass_mode, first.input_type, first.input_fmt,
                                   first.input_qnt_type, first.input_zp,
                                   out.zero_copy_ready, out.fast_path_active);
    return out;
}

}  // namespace ttbox::core
