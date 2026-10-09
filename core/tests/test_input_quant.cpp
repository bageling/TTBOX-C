// test_input_quant.cpp — T1.14 输入量化守卫 + XOR 搬运单元测试
//
// 依据 design.md §G2.4（单测矩阵）/ §G5（验收判据）。
//
// 分两层：
//   ① 纯函数层（rknn/InputQuant.hpp）：默认构建（Windows 标量）与板端 aarch64（NEON）
//      均编译并运行 —— 同一份断言两平台各跑一遍。
//   ② 引擎接线层（rknn/RKNNEngine）：仅在 TTBOX_CORE_HAS_RKNN（板端）编译与运行。
//      守卫在 init_zero_copy() 内、rknn_query 之前返回，故负例无需真实 NPU / 模型即可断言。
//
// ★ 两层纪律（勿混）：
//   ① 纯函数层（rknn/InputQuant.hpp 亦不含 rknn_api.h）—— 默认构建（含 Windows 标量）
//      必须能编译 ⇒ **本文件顶层不得 include rknn_api.h**；
//   ② 引擎接线层 —— 仅在 TTBOX_CORE_HAS_RKNN 下启用；此时经 RKNNEngine.hpp **间接**、
//      以及下方 `#include "rknn_api.h"` 的**直接**引用拿到真实枚举，供编译期 static_assert 真对锁。
//      该直接引用被 `#ifdef TTBOX_CORE_HAS_RKNN` 包裹 ⇒ 不违反第 ① 层（旧注释只说"不得包含
//      rknn_api.h"，与第 ② 层实际引用矛盾，故此处按两层精确表述订正）。

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "rknn/InputQuant.hpp"

// ===========================================================================
// D1 锁必需件：host 枚举"真对锁"依赖 CMake 注入的真实 rknn_api.h 绝对路径宏。
// ★ 这是**编译期硬约束**，不是注释里的约定：宏一旦从 CMake 掉线，本 TU 立即
//   #error 编译失败 —— 绝不让真对锁"无声消失、套件照样全绿"（否则重蹈 D4 批评的
//   静默跳过反模式）。CMake 侧注入见 core/CMakeLists.txt 对 ttbox_core_tests 的
//   target_compile_definitions(TTBOX_RKNN_API_HEADER=...)，并配有 configure 期
//   文件存在性断言；此处 #ifndef 是最后一道防线。
// ===========================================================================
#ifndef TTBOX_RKNN_API_HEADER
#error "锁失效：TTBOX_RKNN_API_HEADER 未定义 —— host 枚举真对锁会静默消失。请在 core/CMakeLists.txt 为 ttbox_core_tests 注入该宏。"
#endif

#ifdef TTBOX_CORE_HAS_RKNN
#include "rknn/RKNNEngine.hpp"
// 板端构建：rknn_api.h 可见（ttbox_core 以 PUBLIC 方式把 third_party/rknn 暴露给
// ttbox_core_tests）。仅用于下面的枚举镜像"真对锁"。
#include "rknn_api.h"

// ===========================================================================
// 枚举镜像"真对锁"（board-only，编译期 static_assert）
//
// 背景（T1.14 D1）：host 侧 input_quant_enum_mirror 只断言镜像常量等于硬编码
// 字面量（自指），对 core/third_party/rknn/rknn_api.h **零约束**，因此拦不住
// 镜像漂移（kTensorFmtUndefined 曾写 2，而真实 RKNN_TENSOR_UNDEFINED==3）。
// 下面把每个镜像常量与**真实枚举**逐一对锁；只在能包含 rknn_api.h 的板端构建
// 生效。若镜像与 rknn_api.h 不一致，这里会**编译失败**（早于任何运行）。
// ===========================================================================
// 注：本块位于 `using namespace ttbox::core;` 之前，故镜像常量须写全限定名。
static_assert(ttbox::core::kTensorTypeFp32 == RKNN_TENSOR_FLOAT32, "mirror drift: kTensorTypeFp32");
static_assert(ttbox::core::kTensorTypeFp16 == RKNN_TENSOR_FLOAT16, "mirror drift: kTensorTypeFp16");
static_assert(ttbox::core::kTensorTypeInt8 == RKNN_TENSOR_INT8, "mirror drift: kTensorTypeInt8");
static_assert(ttbox::core::kTensorTypeUint8 == RKNN_TENSOR_UINT8, "mirror drift: kTensorTypeUint8");
static_assert(ttbox::core::kTensorTypeInt16 == RKNN_TENSOR_INT16, "mirror drift: kTensorTypeInt16");
static_assert(ttbox::core::kTensorFmtNchw == RKNN_TENSOR_NCHW, "mirror drift: kTensorFmtNchw");
static_assert(ttbox::core::kTensorFmtNhwc == RKNN_TENSOR_NHWC, "mirror drift: kTensorFmtNhwc");
static_assert(ttbox::core::kTensorFmtUndefined == RKNN_TENSOR_UNDEFINED,
              "mirror drift: kTensorFmtUndefined");  // ← D1：曾错写 2，真值 3（NC1HWC2 占 2）
static_assert(ttbox::core::kTensorFmtNc1hwc2 == RKNN_TENSOR_NC1HWC2,
              "mirror drift: kTensorFmtNc1hwc2");  // ← D1 补齐：NCHW0/NHWC1/NC1HWC2 2/UNDEFINED 3
static_assert(ttbox::core::kQntNone == RKNN_TENSOR_QNT_NONE, "mirror drift: kQntNone");
static_assert(ttbox::core::kQntDfp == RKNN_TENSOR_QNT_DFP, "mirror drift: kQntDfp");
static_assert(ttbox::core::kQntAffineAsym == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC,
              "mirror drift: kQntAffineAsym");
// rknn_tensor_format 的 4 个取值（NCHW0/NHWC1/NC1HWC2 2/UNDEFINED 3）现已**全部**对锁到
// 镜像常量（上面 kTensorFmtNchw/Nhwc/Nc1hwc2/Undefined 四条），无遗漏项。
// host 侧另有一条运行时"真对锁"（input_quant_enum_mirror_against_real_header），
// 使 Windows 构建也能因 rknn_api.h 变化而失败（见下）。
#endif

#include "test_util.hpp"

using namespace ttbox::core;

namespace {

// 朴素 XOR 0x80 参考实现（逐字节），用于与向量化实现逐字节比对。
std::vector<uint8_t> naive_xor(const std::vector<uint8_t>& src) {
    std::vector<uint8_t> out(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        out[i] = static_cast<uint8_t>(src[i] ^ 0x80);
    }
    return out;
}

// 断言 xor_shift128_copy 与朴素实现逐字节相等（失败只报一次，避免刷屏）。
void check_xor_matches_naive(const std::vector<uint8_t>& src) {
    if (src.empty()) return;
    std::vector<uint8_t> dst(src.size(), 0xAA);
    xor_shift128_copy(dst.data(), src.data(), src.size());
    const std::vector<uint8_t> ref = naive_xor(src);
    for (size_t i = 0; i < src.size(); ++i) {
        if (dst[i] != ref[i]) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "xor mismatch at %zu: got 0x%02X want 0x%02X", i, dst[i], ref[i]);
            ::ttbox_test::report_failure(__FILE__, __LINE__, msg);
            return;
        }
    }
}

// 生成确定性伪随机字节（不使用 <random>，保证跨平台逐位一致）。
void fill_deterministic(std::vector<uint8_t>& buf, uint32_t seed) {
    uint32_t s = seed;
    for (auto& b : buf) {
        s = s * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(s >> 24);
    }
}

}  // namespace

// ---- D1 host 真对锁辅助（读/解析真实 rknn_api.h；宏缺失时上方 #error 已拦截编译）----
namespace {

// 读取整份文本文件；读取失败返回空串（调用方据此 FAIL，绝不静默跳过）。
std::string slurp_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 从 C 源码文本里解析 `typedef enum <tag> { ... }` 的"枚举名 → 数值"映射。
// 支持显式赋值（NAME = N）与隐式自增（NAME ⇒ 前一值 +1）；自动剥离 /* */ 与 // 注释。
// 找不到该 enum 时返回空 map（调用方据此 FAIL）。刻意保持零依赖、纯文本，供 host 运行。
std::map<std::string, int> parse_enum_values(const std::string& text, const std::string& tag) {
    std::map<std::string, int> out;
    const std::string marker = "typedef enum " + tag;
    const size_t mp = text.find(marker);
    if (mp == std::string::npos) return out;
    const size_t ob = text.find('{', mp);
    if (ob == std::string::npos) return out;
    const size_t cb = text.find('}', ob);
    if (cb == std::string::npos) return out;
    std::string block = text.substr(ob + 1, cb - ob - 1);
    // 去块注释 /* ... */
    for (;;) {
        const size_t s = block.find("/*");
        if (s == std::string::npos) break;
        const size_t e = block.find("*/", s + 2);
        if (e == std::string::npos) { block.erase(s); break; }
        block.erase(s, e - s + 2);
    }
    // 去行注释 // ...
    for (;;) {
        const size_t s = block.find("//");
        if (s == std::string::npos) break;
        const size_t e = block.find('\n', s + 2);
        if (e == std::string::npos) { block.erase(s); break; }
        block.erase(s, e - s);
    }
    int running = 0;
    size_t i = 0;
    while (i < block.size()) {
        const size_t comma = block.find(',', i);
        std::string stmt = block.substr(i, (comma == std::string::npos ? block.size() : comma) - i);
        i = (comma == std::string::npos) ? block.size() : comma + 1;
        const size_t a = stmt.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        const size_t b = stmt.find_last_not_of(" \t\r\n");
        stmt = stmt.substr(a, b - a + 1);
        size_t k = 0;
        while (k < stmt.size() &&
               (std::isalnum(static_cast<unsigned char>(stmt[k])) || stmt[k] == '_')) {
            ++k;
        }
        if (k == 0) continue;
        const std::string name = stmt.substr(0, k);
        int value = running;
        const size_t eq = stmt.find('=', k);
        if (eq != std::string::npos) value = std::atoi(stmt.c_str() + eq + 1);
        out[name] = value;
        running = value + 1;
    }
    return out;
}

// 比对镜像常量与真实头文件解析值；不一致即记失败（含具体数值便于定位）。
void expect_mirror(const std::map<std::string, int>& m, const char* name, int mirror) {
    const auto it = m.find(name);
    if (it == m.end()) {
        ::ttbox_test::report_failure(__FILE__, __LINE__,
                                     std::string("enum member missing in real header: ") + name);
        return;
    }
    if (it->second != mirror) {
        char msg[192];
        std::snprintf(msg, sizeof(msg), "%s: mirror=%d real_header=%d", name, mirror, it->second);
        ::ttbox_test::report_failure(__FILE__, __LINE__, msg);
    }
}

}  // namespace

// ---- 边界四点：0x00->0x80、0x7F->0xFF、0x80->0x00、0xFF->0x7F ----
TEST(input_quant_xor_boundary_points) {
    const uint8_t in[4] = {0x00, 0x7F, 0x80, 0xFF};
    uint8_t out[4] = {0, 0, 0, 0};
    xor_shift128_copy(out, in, 4);
    CHECK_EQ(static_cast<int>(out[0]), 0x80);
    CHECK_EQ(static_cast<int>(out[1]), 0xFF);
    CHECK_EQ(static_cast<int>(out[2]), 0x00);
    CHECK_EQ(static_cast<int>(out[3]), 0x7F);

    // 带符号解释自检：q_s8 = u - 128（0->-128、128->0、255->127）
    CHECK_EQ(static_cast<int>(static_cast<int8_t>(out[0])), -128);
    CHECK_EQ(static_cast<int>(static_cast<int8_t>(out[2])), 0);
    CHECK_EQ(static_cast<int>(static_cast<int8_t>(out[3])), 127);
}

// ---- 随机全量比对：640x640x3 vs 朴素逐字节实现 ----
TEST(input_quant_xor_random_vs_naive) {
    std::vector<uint8_t> src(640u * 640u * 3u);
    fill_deterministic(src, 0x12345678u);
    check_xor_matches_naive(src);
}

// ---- NEON/标量批量边界：64 字节主循环 / 16 字节尾循环 / 标量尾三处边界 ----
TEST(input_quant_xor_batch_boundaries) {
    const size_t sizes[] = {1,   15,  16,  17,  63,  64,
                            65,  127, 128, 129, 320, 640};
    for (size_t n : sizes) {
        std::vector<uint8_t> src(n);
        fill_deterministic(src, 0x9E3779B9u ^ static_cast<uint32_t>(n));
        check_xor_matches_naive(src);
    }
}

// ---- 谓词正例：(INT8, NHWC, AFFINE, zp=-128) → kXorShift128 ----
TEST(input_quant_predicate_positive) {
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 1.0f, -128) ==
          InputPassMode::kXorShift128);
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 1.0f / 255.0f, -128) ==
          InputPassMode::kXorShift128);
}

// ---- ★ scale-无关性（回归守卫，design §G1.2.1 / §G2.4）----
// 同 (INT8, NHWC, AFFINE, zp=-128)，任意 scale → 结论恒为 kXorShift128。
// 若有人把 scale 子句加回判定，本用例必 FAIL（destructive counter-proof）。
TEST(input_quant_scale_independence) {
    const float scales[] = {1.0f,        1.0f / 255.0f, 1.0f / 127.0f,
                            0.5f,        2.0f,         0.0f,
                            -1.0f,       0.0171f,      1e-6f};
    for (float sc : scales) {
        CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, sc, -128) ==
              InputPassMode::kXorShift128);
    }
}

// ---- 谓词反例①（ImageNet 非零均值，守卫有效性核心）----
TEST(input_quant_predicate_negative_imagenet) {
    // ImageNet mean≈123.675 / std≈58.395 → zp≈-4（推断值，板端 G5 钉死）
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 0.0171f, -4) ==
          InputPassMode::kCompatible);
    // 只认精确 zp==-128：±1 都不得放行
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 0.0171f, -127) ==
          InputPassMode::kCompatible);
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 0.0171f, -129) ==
          InputPassMode::kCompatible);
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 0.0171f, 0) ==
          InputPassMode::kCompatible);
}

// ---- 谓词反例②：DFP / NONE（qnt_type 必查，DFP zp 语义不同）----
TEST(input_quant_predicate_negative_qnt_type) {
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntDfp, 0.0171f, -128) ==
          InputPassMode::kCompatible);
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNhwc, kQntNone, 0.0171f, -128) ==
          InputPassMode::kCompatible);
}

// ---- 谓词反例③：NCHW / FP16 / FP32 ----
TEST(input_quant_predicate_negative_type_or_fmt) {
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtNchw, kQntAffineAsym, 1.0f, -128) ==
          InputPassMode::kCompatible);
    CHECK(classify_input_pass(kTensorTypeInt8, kTensorFmtUndefined, kQntAffineAsym, 1.0f, -128) ==
          InputPassMode::kCompatible);
    CHECK(classify_input_pass(kTensorTypeFp16, kTensorFmtNhwc, kQntAffineAsym, 1.0f, -128) ==
          InputPassMode::kCompatible);
    CHECK(classify_input_pass(kTensorTypeFp32, kTensorFmtNhwc, kQntNone, 1.0f, 0) ==
          InputPassMode::kCompatible);
}

// ---- 谓词：UINT8 原生（方案 D 预留）→ kUint8Native ----
TEST(input_quant_predicate_uint8_native) {
    CHECK(classify_input_pass(kTensorTypeUint8, kTensorFmtNhwc, kQntNone, 1.0f, 0) ==
          InputPassMode::kUint8Native);
    CHECK(classify_input_pass(kTensorTypeUint8, kTensorFmtNhwc, kQntAffineAsym, 1.0f, -128) ==
          InputPassMode::kUint8Native);
    // UINT8 但非 NHWC → 兼容 I/O
    CHECK(classify_input_pass(kTensorTypeUint8, kTensorFmtNchw, kQntNone, 1.0f, 0) ==
          InputPassMode::kCompatible);
}

// ---- 枚举镜像字面量自检（**弱**：仅防手滑，非漂移锁）----
// 说明：以下 CHECK_EQ 的期望值是硬编码字面量，属"常量对自己"的自指断言，对
// core/third_party/rknn/rknn_api.h **零约束**（改头文件它照样 PASS）。真正的漂移锁是：
//   ① 板端编译期 static_assert（文件顶部，TTBOX_CORE_HAS_RKNN）；
//   ② host 运行期对真实头文件解析（下方 input_quant_enum_mirror_against_real_header）。
// 本用例只保证镜像常量本身的数值不被无意改动。
TEST(input_quant_enum_mirror) {
    CHECK_EQ(kTensorTypeFp32, 0);
    CHECK_EQ(kTensorTypeFp16, 1);
    CHECK_EQ(kTensorTypeInt8, 2);
    CHECK_EQ(kTensorTypeUint8, 3);
    CHECK_EQ(kTensorTypeInt16, 4);
    CHECK_EQ(kTensorFmtNchw, 0);
    CHECK_EQ(kTensorFmtNhwc, 1);
    CHECK_EQ(kTensorFmtNc1hwc2, 2);  // ← D1 补齐的常量
    CHECK_EQ(kTensorFmtUndefined, 3);  // ← D1：曾错写 2
    CHECK_EQ(kQntNone, 0);
    CHECK_EQ(kQntDfp, 1);
    CHECK_EQ(kQntAffineAsym, 2);
    CHECK_EQ(static_cast<int>(InputPassMode::kCompatible), 0);
    CHECK_EQ(static_cast<int>(InputPassMode::kXorShift128), 1);
    CHECK_EQ(static_cast<int>(InputPassMode::kUint8Native), 2);
}

// ---- ★ 枚举镜像"真对锁"（host 可跑）：解析真实 rknn_api.h 文本再比对 ----
// 这是"测试有牙齿"的判据：其期望值来自**真实头文件**（而非对本文件常量取字面量），
// 故 rknn_api.h 一旦增删/重排枚举，本用例必 FAIL。头文件绝对路径由 CMake 注入
// （core/CMakeLists.txt 对 ttbox_core_tests 加的 TTBOX_RKNN_API_HEADER）；
// 该宏由 CMake 无条件注入，且文件顶部 `#ifndef TTBOX_RKNN_API_HEADER → #error` 兜底，
// 故本块**无条件编译并运行**（不再有可静默消失的 #ifdef 外壳）。
TEST(input_quant_enum_mirror_against_real_header) {
    const std::string text = slurp_file(TTBOX_RKNN_API_HEADER);
    CHECK(!text.empty());  // 头文件不可读 ⇒ 锁失效，直接 FAIL（绝不静默通过）
    if (text.empty()) return;

    const std::map<std::string, int> t = parse_enum_values(text, "_rknn_tensor_type");
    const std::map<std::string, int> f = parse_enum_values(text, "_rknn_tensor_format");
    const std::map<std::string, int> q = parse_enum_values(text, "_rknn_tensor_qnt_type");
    CHECK(!t.empty());  // 三个 enum 都必须解析到
    CHECK(!f.empty());
    CHECK(!q.empty());

    // rknn_tensor_type
    expect_mirror(t, "RKNN_TENSOR_FLOAT32", kTensorTypeFp32);
    expect_mirror(t, "RKNN_TENSOR_FLOAT16", kTensorTypeFp16);
    expect_mirror(t, "RKNN_TENSOR_INT8", kTensorTypeInt8);
    expect_mirror(t, "RKNN_TENSOR_UINT8", kTensorTypeUint8);
    expect_mirror(t, "RKNN_TENSOR_INT16", kTensorTypeInt16);
    // rknn_tensor_format（D1 核心：NC1HWC2=2 / UNDEFINED=3 必须对锁）
    expect_mirror(f, "RKNN_TENSOR_NCHW", kTensorFmtNchw);
    expect_mirror(f, "RKNN_TENSOR_NHWC", kTensorFmtNhwc);
    expect_mirror(f, "RKNN_TENSOR_NC1HWC2", kTensorFmtNc1hwc2);
    expect_mirror(f, "RKNN_TENSOR_UNDEFINED", kTensorFmtUndefined);
    // rknn_tensor_qnt_type
    expect_mirror(q, "RKNN_TENSOR_QNT_NONE", kQntNone);
    expect_mirror(q, "RKNN_TENSOR_QNT_DFP", kQntDfp);
    expect_mirror(q, "RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC", kQntAffineAsym);
}

// ===========================================================================
// 引擎接线层（板端 TTBOX_CORE_HAS_RKNN）
// ===========================================================================
#ifdef TTBOX_CORE_HAS_RKNN
namespace {

RknnModelInfo make_info(int type, int fmt, int qnt, float scale, int32_t zp) {
    RknnModelInfo info;
    info.n_inputs = 1;
    info.n_outputs = 1;
    info.input_width = 640;
    info.input_height = 640;
    info.input_type = type;
    info.input_fmt = fmt;
    info.input_qnt_type = qnt;
    info.input_scale = scale;
    info.input_zp = zp;
    return info;
}

}  // namespace

// 守卫负例（design §G5-5）：非零均值 INT8 模型 → init_zero_copy 必须拒绝、回落兼容 I/O。
// 守卫在 rknn_query 之前返回 ⇒ 本用例无需真实 NPU/模型即可断言（test_init_hook 注入）。
TEST(input_quant_guard_rejects_nonzero_mean) {
    RKNNEngine engine;
    RKNNEngine::Params p;
    p.model_path = "TEST_MODEL";
    p.pass_through = true;
    p.test_init_hook = [](RknnModelInfo* info, std::string*) {
        *info = make_info(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 0.0171f, -4);
        return true;
    };
    std::string err;
    CHECK(engine.init(p, &err));
    CHECK(engine.initialized());
    CHECK(engine.input_pass_mode() == InputPassMode::kCompatible);  // 初始默认即兼容

    // 核心断言：拒绝零拷贝快路径 ⇒ zero_copy_ready_ 保持 false（陷阱一修复生效）
    CHECK(!engine.init_zero_copy(&err));
    CHECK(!engine.zero_copy_ready());
    CHECK(!engine.pass_through_active());  // 陷阱二：绝不因回退而漏为真
    CHECK(engine.input_pass_mode() == InputPassMode::kCompatible);
    CHECK(err.find("回退兼容 I/O") != std::string::npos);  // WARN 文案含"回退兼容 I/O"
}

// 结构不变式：pass_through_active() 蕴含 zero_copy_ready_（永不结构解耦）。
TEST(input_quant_pass_through_implies_zero_copy) {
    RKNNEngine engine;
    RKNNEngine::Params p;
    p.model_path = "TEST_MODEL";
    p.pass_through = true;
    p.test_init_hook = [](RknnModelInfo* info, std::string*) {
        *info = make_info(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 1.0f / 255.0f, -128);
        return true;
    };
    std::string err;
    CHECK(engine.init(p, &err));
    // 未建立零拷贝 I/O 前，快路径必为未激活（pass_through_active ⇒ zero_copy_ready）
    CHECK(!engine.pass_through_active());
    CHECK(!engine.zero_copy_ready());
}

// ===========================================================================
// ★ 陷阱二回归锁（板端可跑；无需真实 NPU —— 仅用 hook 注入 + 早返回路径）
//
// 陷阱二历史形态：set_input() / init_zero_copy() / bind_external_input_fd() 曾各自
// 直接写一个独立标志 pass_through_active_（= params_.pass_through / = true），使
// "快路径已激活"与 zero_copy_ready_ 结构解耦 ⇒ 非零均值模型回退后仍可能被 WorkerPool
// 当快路径 XOR 搬运，静默算错。现该成员与三处直接赋值均已删除；pass_through_active()
// 改为派生 (zero_copy_ready_ && pass_mode_==kXorShift128)（design §G2.3.1 结构保证 3）。
//
// ★ 可达性（诚实记录，避免"空断言"）：历史赋值位于 rknn_inputs_set()/rknn_query()
//   **成功之后**的尾段；hook 注入下 impl_->ctx==0，调用 librknnrt 属未定义行为，
//   故**尾段在本用例中不可达**。本用例因此只断言"所有回退入口绝不改写模式/激活态"，
//   并把真链路的强断言下沉到板端 T1.13 验收（见用例末尾的补充断言清单）。
// ===========================================================================
TEST(input_quant_trap_two_fallback_paths_never_activate) {
    RKNNEngine engine;
    RKNNEngine::Params p;
    p.model_path = "TEST_MODEL";
    p.pass_through = true;
    // 与快路径**完全匹配**的输入（INT8/NHWC/AFFINE/zp=-128）：一旦有人误设激活态即为真，
    // 故这是最敏感的检测参数。
    p.test_init_hook = [](RknnModelInfo* info, std::string*) {
        *info = make_info(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, 1.0f / 255.0f, -128);
        return true;
    };
    std::string err;
    CHECK(engine.init(p, &err));
    CHECK(engine.initialized());

    // 起点：init(hook) 不建立零拷贝 ⇒ 模式仍为默认兼容、激活态必假。
    CHECK(!engine.zero_copy_ready());
    CHECK(!engine.pass_through_active());
    CHECK(engine.input_pass_mode() == InputPassMode::kCompatible);

    // 回退入口①：set_input 参数非法 ⇒ 早返回（不触及 librknnrt），且绝不改写模式/激活态。
    CHECK(!engine.set_input(nullptr, 0, &err));
    CHECK(!engine.pass_through_active());
    CHECK(engine.input_pass_mode() == InputPassMode::kCompatible);

    // 回退入口②：未建立零拷贝时的 external DMA-BUF 直绑 ⇒ 早返回（拒绝），同样不得激活。
    CHECK(!engine.bind_external_input_fd(-1, nullptr, 0, &err));
    CHECK(!engine.pass_through_active());
    CHECK(engine.input_pass_mode() == InputPassMode::kCompatible);

    // 陷阱二核心结构不变式：激活态蕴含零拷贝就绪 —— 回退路径下必须恒为假。
    CHECK(!engine.zero_copy_ready());
    CHECK(!engine.pass_through_active() || engine.zero_copy_ready());

    // ---- 下沉到板端 T1.13（真实模型 + 真实 rknn_context）的补充断言建议 ----
    //   1) 真 INT8/NHWC/AFFINE/zp==-128 模型：init()→init_zero_copy() 成功后
    //      CHECK(input_pass_mode()==kXorShift128); CHECK(zero_copy_ready());
    //      CHECK(pass_through_active());
    //   2) 继以 set_input()/run()（零拷贝路径）后，仍 CHECK(pass_through_active());
    //   3) 非零均值 INT8 或 FP16 模型：init_zero_copy() 成功后 CHECK(!zero_copy_ready());
    //      再 set_input()/run() 后 CHECK(!pass_through_active()) 且
    //      CHECK(input_pass_mode()==kCompatible);
    //   4) 任意路径不变式：CHECK(!pass_through_active() || zero_copy_ready());
}
#else  // !TTBOX_CORE_HAS_RKNN
// ===========================================================================
// ★ item 4 · 可听见信号（"不能静默丢覆盖"）：
//   本 TU 的【引擎接线层】共 3 条用例（见上）仅在板端 TTBOX_CORE_HAS_RKNN 下编译；
//   host 构建下它们**静默消失**，套件仍全绿 —— 正是"覆盖悄悄减少、而灯还亮着"。
//   故在此显式发声，使每一次 host 编译都在构建日志里留下痕迹。
//   选型理由（#pragma message  vs  configure 期"跳过 N 条"日志）：
//     选 **#pragma message** —— 它与被守卫的区域**同文件共置**：将来这块 guard 若被
//     移动/删除，信号跟着走，**不可能与代码漂移**；且在本 TU 每次编译时都触发。
//     反之 configure 期日志只在"重新 configure"时出现、且需在 CMakeLists 另处维护，
//     容易与本源脱节。二者皆"可听见"，此处取共置性更强的一种。
#pragma message("test_input_quant: SKIP 3 board-only cases (no TTBOX_CORE_HAS_RKNN): guard_rejects_nonzero_mean / pass_through_implies_zero_copy / trap_two_fallback_paths_never_activate")
#endif  // TTBOX_CORE_HAS_RKNN
