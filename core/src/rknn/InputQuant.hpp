// InputQuant.hpp — 输入量化分类纯函数（header-only）
//
// 设计依据：design.md §G1.2.1（谓词裁决）/ §G2.1（单一判定入口）/ §G2.4（单测矩阵）。
//
// 背景（design.md §G1）：pass_through=1 时 RKNN runtime 把输入 buffer 字节直接当作
// 模型输入节点的 int8 张量值（不做任何转换）。RGA 产出的是 uint8 像素 u，快路径在
// 搬运时做 XOR 0x80 得到带符号解释 q_s8 = u − 128。模型按非对称仿射反量化
//   v = (q_s8 − zp) · scale
// 代入 zp == −128：v = (u − 128 + 128) · scale = u · scale。要 v 等于训练归一化
// f(u) = u / std（零均值），只需 zp == −128 —— 而 scale 恰是该模型自身量化出的 1/std，
// 模型用自己的 scale 反量化即为恒等。⇒ XOR 正确性只依赖 zp == −128，与 scale 取值无关。
//
// ★ 因此谓词 = (INT8 ∧ NHWC ∧ qnt_type==AFFINE_ASYMMETRIC ∧ zp==−128)；
//   scale 不参与判定（保留入参仅供调用方打 WARN 与 scale-无关性回归测试，防后人把
//   scale 子句加回——加的后果只会"误拒"合法模型，绝不提升正确性）。
//
// 零 rknn_api.h 依赖 ⇒ Windows 默认构建可直接单测；NEON 仅在 __ARM_NEON 下启用，
// 故同一份断言在 Windows（标量）与板端 aarch64（NEON）各跑一遍。
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace ttbox::core {

// 与 rknn_api.h 的枚举数值镜像（本头文件不 include rknn_api.h，保持零依赖；
// 数值须与 core/third_party/rknn/rknn_api.h 一致，回归用例 input_quant_enum_mirror 锁死）：
//   rknn_tensor_type:      FLOAT32=0 FLOAT16=1 INT8=2 UINT8=3 INT16=4
//   rknn_tensor_format:    NCHW=0 NHWC=1 NC1HWC2=2 UNDEFINED=3
//   rknn_tensor_qnt_type:  NONE=0 DFP=1 AFFINE_ASYMMETRIC=2
inline constexpr int kTensorTypeFp32 = 0;
inline constexpr int kTensorTypeFp16 = 1;
inline constexpr int kTensorTypeInt8 = 2;
inline constexpr int kTensorTypeUint8 = 3;
inline constexpr int kTensorTypeInt16 = 4;
inline constexpr int kTensorFmtNchw = 0;
inline constexpr int kTensorFmtNhwc = 1;
inline constexpr int kTensorFmtNc1hwc2 = 2;
inline constexpr int kTensorFmtUndefined = 3;
inline constexpr int kQntNone = 0;
inline constexpr int kQntDfp = 1;
inline constexpr int kQntAffineAsym = 2;

// 输入侧搬运模式（引擎唯一判定结论；WorkerPool 只消费，不自行判定）
enum class InputPassMode : int {
    kCompatible = 0,   // 回落兼容 I/O（set_input 喂 UINT8，runtime 内部量化）——永远正确、永远可用
    kXorShift128 = 1,  // INT8 + 零均值量化（zp==-128）：快路径可用，搬运时 XOR 0x80
    kUint8Native = 2,  // UINT8 原生输入（方案 D 预留）：直传，零变换
};

// 输入侧唯一判定入口（design §G1.2.1 裁决）。
// ★ scale 入参不参与判定：XOR 正确性只依赖 zp==-128；保留 scale 仅供调用方打 WARN
//   与 scale-无关性回归测试。禁止在本函数内任何分支引用 scale（防回归见 design §G2.4）。
inline InputPassMode classify_input_pass(int input_type, int input_fmt,
                                         int qnt_type, float /*scale*/, int32_t zp) {
    // 方案 D 预留：UINT8 原生输入 → 直传零变换
    if (input_type == kTensorTypeUint8 && input_fmt == kTensorFmtNhwc) {
        return InputPassMode::kUint8Native;
    }
    // XOR 快路径：仅 INT8 + NHWC + 非对称仿射 + zp==-128
    if (input_type == kTensorTypeInt8 && input_fmt == kTensorFmtNhwc &&
        qnt_type == kQntAffineAsym && zp == -128) {
        return InputPassMode::kXorShift128;
    }
    // 其余（非零均值 zp!=-128 / DFP / NONE / FP16 / NCHW）：兼容 I/O
    return InputPassMode::kCompatible;
}

// ---------------------------------------------------------------------------
// 字符串名（IPC/Web 契约；**稳定不可改** —— 改即破坏面板与外部诊断工具解析）
//
// 放在本头文件的原因：这几个枚举的"真值来源"就是上面的镜像常量（以及下面的
// InputPassMode），名字与判据必须同居一处。若挪到 IPC/Web 各写一份映射表，
// 判据一变（例如将来新增 kUint8Native 之外的第四种模式）就会漏改一处而静默错显示。
//
// 未知值一律回落 "unknown"（**绝不**回落成某个合法名）：调用方据此可区分
// "确实是 compatible" 与 "读到了本端不认识的值"，避免把不可知误报成正常。
// ---------------------------------------------------------------------------
inline const char* tensor_type_name(int t) {
    switch (t) {
        case kTensorTypeFp32:   return "fp32";
        case kTensorTypeFp16:   return "fp16";
        case kTensorTypeInt8:   return "int8";
        case kTensorTypeUint8:  return "uint8";
        case kTensorTypeInt16:  return "int16";
        default:                return "unknown";
    }
}

inline const char* tensor_fmt_name(int f) {
    switch (f) {
        case kTensorFmtNchw:    return "nchw";
        case kTensorFmtNhwc:    return "nhwc";
        case kTensorFmtNc1hwc2: return "nc1hwc2";
        case kTensorFmtUndefined: return "undefined";
        default:                return "unknown";
    }
}

inline const char* tensor_qnt_type_name(int q) {
    switch (q) {
        case kQntNone:       return "none";
        case kQntDfp:        return "dfp";
        case kQntAffineAsym: return "affine_asymmetric";
        default:             return "unknown";
    }
}

// 注意：本函数名即"哪条快路径"的对外答案，板端换模型后在面板直接显示其返回值。
inline const char* input_pass_mode_name(InputPassMode m) {
    switch (m) {
        case InputPassMode::kUint8Native: return "uint8_native";   // 直传，零变换
        case InputPassMode::kXorShift128: return "xor_shift128";   // CPU 拷贝 + XOR 0x80
        case InputPassMode::kCompatible:  return "compatible";     // 回落兼容 I/O
        default:                          return "unknown";
    }
}

// XOR 0x80 搬运（自 WorkerPool.cpp 原 copy_uint8_to_int8_shift128 整体迁入）：
// uint8 像素 → int8（等价于减 128，即字节 XOR 0x80）。RKNN pass_through=1 时输入 mem
// 直接作为模型原生 int8 张量使用，必须在此完成量化映射；否则 NPU 会把 128..255
// 当作 -128..-1，产生错误推理。header-only + inline ⇒ 单一定义，两平台共用。
inline void xor_shift128_copy(uint8_t* dst, const uint8_t* src, size_t n) {
#if defined(__ARM_NEON)
    const uint8x16_t bias = vdupq_n_u8(0x80);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        vst1q_u8(dst + i + 0, veorq_u8(vld1q_u8(src + i + 0), bias));
        vst1q_u8(dst + i + 16, veorq_u8(vld1q_u8(src + i + 16), bias));
        vst1q_u8(dst + i + 32, veorq_u8(vld1q_u8(src + i + 32), bias));
        vst1q_u8(dst + i + 48, veorq_u8(vld1q_u8(src + i + 48), bias));
    }
    for (; i + 16 <= n; i += 16) {
        vst1q_u8(dst + i, veorq_u8(vld1q_u8(src + i), bias));
    }
    for (; i < n; ++i) dst[i] = static_cast<uint8_t>(src[i] ^ 0x80);
#else
    for (size_t i = 0; i < n; ++i) dst[i] = static_cast<uint8_t>(src[i] ^ 0x80);
#endif
}

}  // namespace ttbox::core
