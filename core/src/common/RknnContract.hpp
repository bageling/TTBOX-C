// RknnContract.hpp — NPU 运行时查询出来的**契约值类型**（叶子头）
//
// ★ 为什么独立成一个头（2026-10-06 架构整理）：
//   下面三个 struct 描述的是"模型输入输出长什么样"和"分阶段耗时"——它们是**数据**，
//   不是 RKNN 推理引擎的行为。它们此前住在 rknn/RKNNEngine.hpp（与 RKNNEngine 类同住），
//   于是产生了这样的传递依赖：
//     model/Decoder.hpp          为拿 RknnModelInfo  → include rknn/RKNNEngine.hpp
//     model/ModelAdapter.hpp     同上
//     model/backend/*.hpp        同上
//   而 rknn/DecodeNMS.hpp 又为了拿 InferenceProfile/FovProfile 反过来 include
//   model/RuntimeProfile.hpp ⇒ **model ↔ rknn 形成模块级依赖环**。
//
//   把契约值类型下沉到 common/（干净叶子层）后：
//     - model 只依赖 common（叶子），不再看见 rknn；
//     - rknn 也只依赖 common；
//     - 两模块可各自独立编译，改契约不会触发对侧全量重编译。
//
// 命名说明：类型仍在 namespace ttbox::core，**所有使用点无需改动**。
// 使用约定：
//   - 只需要"模型输入输出形状 / 耗时统计"的值类型 → include 本头；
//   - 需要真正跑推理（RKNNEngine 类）→ include rknn/RKNNEngine.hpp（它 include 本头）。
#pragma once

#include <cstdint>
#include <vector>

#include "common/Stats.hpp"

namespace ttbox::core {

// 单个输出张量的属性（init 时由 rknn_query 获取，全部来自 runtime，不猜测）
struct RknnOutputInfo {
    uint32_t n_elems = 0;   // 元素数（张量元素个数）
    uint32_t size = 0;      // 原生字节数（attr.size，含对齐）
    int type = 0;           // rknn_tensor_type（FLOAT32/FLOAT16/INT8/...）
    int fmt = 0;            // rknn_tensor_format（NCHW/NHWC）
    std::vector<uint32_t> dims;  // 原始 dims（如 {1,84,8400}）
    float scale = 0.0f;     // 反量化 scale（INT8 等需要）
    int zp = 0;             // 反量化 zero point
};

// 模型输入输出信息（init 时由 rknn_query 获取，全部来自 runtime，不猜测）
struct RknnModelInfo {
    uint32_t n_inputs = 0;
    uint32_t n_outputs = 0;

    // 输入 0（当前单输入模型）
    std::vector<uint32_t> input_dims;   // 例如 {1, 640, 640, 3}
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t input_size = 0;            // 输入 tensor 所需字节（含对齐）
    int input_type = 0;                 // rknn_tensor_type（来自 query）
    int input_fmt = 0;                  // rknn_tensor_format
    // 输入量化参数（T1.14：与 RknnOutputInfo.scale/zp 同名同义，init 时由 rknn_query 获取，不猜测）
    int input_qnt_type = 0;             // rknn_tensor_qnt_type（AFFINE_ASYMMETRIC / DFP / NONE）
    float input_scale = 0.0f;           // 输入反量化 scale
    int32_t input_zp = 0;               // 输入反量化 zero point（XOR 快路径要求 == -128）

    // 输出（全部）
    std::vector<uint32_t> output_n_elems;  // 各输出元素数
    std::vector<uint32_t> output_sizes;    // 各输出字节（native type）
    std::vector<RknnOutputInfo> outputs;   // 各输出完整属性（A-6 Decode 反量化用）
};

// 单 Worker 分阶段耗时统计（面板/日志展示用）
struct RknnStageStats {
    StatsCollector set_input;
    StatsCollector run;
    StatsCollector output;
    StatsCollector total;
};

}  // namespace ttbox::core
