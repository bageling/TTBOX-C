// test_input_path_summary.cpp — T1.15 输入通路诊断：快照 / 聚合 / 文案 / 契约名单测
//
// 依据：design §G1.2.1（谓词裁决）+ 本轮可观测性需求（换模型后面板直读"走了哪条快路径"）。
//
// 为什么这些用例能在 host（Windows 标量）跑：
//   rknn/InputPathSummary.hpp 是 header-only 且**零 rknn_api.h 依赖** ⇒ 判定、聚合、文案
//   全部可在本机验证。板端只剩"取值 → 塞进 PipelineMetrics"的机械搬运（CoreRuntime.cpp），
//   该文件不在 host 构建里，故本文件是这两层逻辑的唯一可验证出口。
//
// ★ 本文件最大价值在最后一条：input_path_summary_matches_predicate_matrix ——
//   它把"文案/聚合"与"谓词裁决"两个纯函数在同一次遍历里互相锁定。若将来有人在
//   classify_input_pass 里加了新判据却忘了同步 describe_input_path，该用例立刻变红。

#include <cstdint>
#include <string>
#include <vector>

#include "rknn/InputPathSummary.hpp"
#include "rknn/InputQuant.hpp"
#include "test_util.hpp"

// CHECK_EQ 内部用 std::to_string() 组装失败信息，std::string 无法 to_string ⇒ 字符串
// 断言必须换成本宏（同样打印两侧实际值，避免"只报不等、不报值"的排查地狱）。
#define CHECK_STR(a, b) \
    do { \
        const std::string _cs_a = (a); \
        const std::string _cs_b = (b); \
        if (_cs_a != _cs_b) { \
            ::ttbox_test::report_failure(__FILE__, __LINE__, \
                std::string(#a " == " #b "  [\"") + _cs_a + "\" vs \"" + _cs_b + "\"]"); \
        } \
    } while (0)

using namespace ttbox::core;

namespace {

ModelInputPathSnapshot make_snap(int type, int fmt, int qnt, int zp, float scale,
                                 uint32_t w, uint32_t h,
                                 bool zero_copy, bool fast_active,
                                 bool dma_requested = false, bool dma_bound = false) {
    ModelInputPathSnapshot s;
    s.pass_mode = static_cast<int>(classify_input_pass(type, fmt, qnt, scale, zp));
    s.input_type = type;
    s.input_fmt = fmt;
    s.input_qnt_type = qnt;
    s.input_zp = zp;
    s.input_scale = scale;
    s.input_w = w;
    s.input_h = h;
    s.zero_copy_ready = zero_copy;
    s.fast_path_active = fast_active;
    s.external_dma_requested = dma_requested;
    s.external_dma_bound = dma_bound;
    return s;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---- 契约锁①：三个通路名是 IPC/Web 的稳定契约，**改字即破坏面板解析** ----
TEST(input_path_names_are_stable_contract) {
    CHECK_STR(std::string(input_pass_mode_name(InputPassMode::kCompatible)), std::string("compatible"));
    CHECK_STR(std::string(input_pass_mode_name(InputPassMode::kXorShift128)), std::string("xor_shift128"));
    CHECK_STR(std::string(input_pass_mode_name(InputPassMode::kUint8Native)), std::string("uint8_native"));
    // 越界值必须回落 "unknown"，**不得**落成 "compatible"（那会把"不可知"谎报成"正常"）
    CHECK_STR(std::string(input_pass_mode_name(static_cast<InputPassMode>(99))), std::string("unknown"));

    CHECK_STR(std::string(tensor_type_name(kTensorTypeFp32)), std::string("fp32"));
    CHECK_STR(std::string(tensor_type_name(kTensorTypeFp16)), std::string("fp16"));
    CHECK_STR(std::string(tensor_type_name(kTensorTypeInt8)), std::string("int8"));
    CHECK_STR(std::string(tensor_type_name(kTensorTypeUint8)), std::string("uint8"));
    CHECK_STR(std::string(tensor_type_name(kTensorTypeInt16)), std::string("int16"));
    CHECK_STR(std::string(tensor_type_name(-1)), std::string("unknown"));

    CHECK_STR(std::string(tensor_fmt_name(kTensorFmtNchw)), std::string("nchw"));
    CHECK_STR(std::string(tensor_fmt_name(kTensorFmtNhwc)), std::string("nhwc"));
    CHECK_STR(std::string(tensor_fmt_name(kTensorFmtNc1hwc2)), std::string("nc1hwc2"));
    CHECK_STR(std::string(tensor_fmt_name(-1)), std::string("unknown"));

    CHECK_STR(std::string(tensor_qnt_type_name(kQntNone)), std::string("none"));
    CHECK_STR(std::string(tensor_qnt_type_name(kQntDfp)), std::string("dfp"));
    CHECK_STR(std::string(tensor_qnt_type_name(kQntAffineAsym)), std::string("affine_asymmetric"));
    CHECK_STR(std::string(tensor_qnt_type_name(-1)), std::string("unknown"));
}

// ---- 无 worker（runtime 未启动）⇒ 全 unknown/0/false，绝不臆造 compatible ----
TEST(input_path_summary_empty_is_unavailable) {
    const ModelInputPathSummary by_null = summarize_input_path(nullptr, 0);
    CHECK_STR(by_null.pass_mode_name, std::string("unknown"));
    CHECK_EQ(by_null.input_type, -1);
    CHECK_STR(by_null.input_type_name, std::string("unknown"));
    CHECK_EQ(by_null.input_fmt, -1);
    CHECK_STR(by_null.input_qnt_name, std::string("unknown"));
    CHECK_EQ(by_null.input_zp, 0);
    CHECK_EQ(by_null.input_scale, 0.0);
    CHECK_EQ(by_null.input_w, 0u);
    CHECK_EQ(by_null.input_h, 0u);
    CHECK_EQ(by_null.workers_total, 0u);
    CHECK_EQ(by_null.workers_fast_path, 0u);
    CHECK(!by_null.zero_copy_ready);
    CHECK(!by_null.fast_path_active);
    CHECK(!by_null.external_dma_bound);
    CHECK(by_null.note.empty());

    // 非 nullptr 但 n==0 必须同语义（防"数组已建、count 忘传"这一路静默）
    const ModelInputPathSnapshot one[1] = {};
    const ModelInputPathSummary by_zero = summarize_input_path(one, 0);
    CHECK_STR(by_zero.pass_mode_name, std::string("unknown"));
    CHECK_EQ(by_zero.workers_total, 0u);
}

// ---- INT8 零均值 + 零拷贝就绪 + 快路径激活 ⇒ 正常态：无文案 ----
TEST(input_path_summary_int8_fast_path_healthy) {
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, -128, 1.0f / 255.0f,
                  640, 640, /*zero_copy=*/true, /*fast_active=*/true)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("xor_shift128"));
    CHECK_STR(r.input_type_name, std::string("int8"));
    CHECK_STR(r.input_fmt_name, std::string("nhwc"));
    CHECK_STR(r.input_qnt_name, std::string("affine_asymmetric"));
    CHECK_EQ(r.input_zp, -128);
    CHECK_EQ(r.input_w, 640u);
    CHECK_EQ(r.input_h, 640u);
    CHECK_EQ(r.workers_total, 1u);
    CHECK_EQ(r.workers_fast_path, 1u);
    CHECK_EQ(r.workers_zero_copy, 1u);
    CHECK(r.zero_copy_ready);
    CHECK(r.fast_path_active);
    CHECK(r.note.empty());  // 健康态不产出说明文字（面板不显示该行）
}

// ---- UINT8 原生 = 最优路径：同样无文案 ----
TEST(input_path_summary_uint8_native_no_note) {
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeUint8, kTensorFmtNhwc, kQntNone, 0, 1.0f, 320, 320,
                  /*zero_copy=*/true, /*fast_active=*/false)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("uint8_native"));
    CHECK(r.zero_copy_ready);
    CHECK(!r.fast_path_active);  // UINT8 的搬运 = memcpy，不属 XOR 快路径
    CHECK(r.note.empty());
}

// ---- 回落四因：文案必须点出"卡在哪一条判据"（人话，面板直读） ----
TEST(input_path_summary_fp16_fallback_reason) {
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeFp16, kTensorFmtNhwc, kQntNone, 0, 1.0f, 640, 640, false, false)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("compatible"));
    CHECK(contains(r.note, "FP16"));
    CHECK(contains(r.note, "12.6"));  // 文案必须带代价量级（用户据此判断值不值得换模型）
}

TEST(input_path_summary_int8_nonzero_zp_reason) {
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, -4, 0.0171f, 640, 640, true, false)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("compatible"));
    CHECK(contains(r.note, "zp=-4"));
    CHECK(contains(r.note, "-128"));
}

TEST(input_path_summary_int8_dfp_reason) {
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntDfp, -128, 0.0171f, 640, 640, true, false)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("compatible"));
    CHECK(contains(r.note, "dfp"));
}

TEST(input_path_summary_nchw_reason) {
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeInt8, kTensorFmtNchw, kQntAffineAsym, -128, 0.0171f, 640, 640, true, false)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("compatible"));
    CHECK(contains(r.note, "nchw"));
}

TEST(input_path_summary_uint8_nchw_reason) {
    // UINT8 但非 NHWC：既不满足 kUint8Native 也不满足 XOR ⇒ 走到兜底文案（须带类型/布局）
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeUint8, kTensorFmtNchw, kQntNone, 0, 1.0f, 640, 640, true, false)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("compatible"));
    CHECK(contains(r.note, "uint8"));
    CHECK(contains(r.note, "nchw"));
}

// ---- XOR 模式但零拷贝没建起来（init_zero_copy 失败）⇒ 必须说出来，不能沉默 ----
TEST(input_path_summary_xor_mode_without_zero_copy) {
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, -128, 0.0039f, 640, 640,
                  /*zero_copy=*/false, /*fast_active=*/false)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("xor_shift128"));  // 分类结论仍在（谓词确实通过）
    CHECK(!r.zero_copy_ready);
    CHECK(!r.fast_path_active);
    CHECK(!r.note.empty());
    CHECK(contains(r.note, "零拷贝"));
}

// ---- ★ 核心不变式句："分类为 xor 且零拷贝就绪" 却 "未激活" ⇒ 内部状态异常，必须报缺陷 ----
TEST(input_path_summary_xor_ready_but_inactive_is_defect) {
    const ModelInputPathSnapshot s[1] = {
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, -128, 0.0039f, 640, 640,
                  /*zero_copy=*/true, /*fast_active=*/false)};
    const ModelInputPathSummary r = summarize_input_path(s, 1);
    CHECK_STR(r.pass_mode_name, std::string("xor_shift128"));
    CHECK(r.zero_copy_ready);
    CHECK(!r.fast_path_active);
    CHECK(contains(r.note, "异常"));
}

// ---- 分类字段取"首个 worker"为准（模型全 worker 共享，结构上必然同值） ----
TEST(input_path_summary_first_worker_is_authoritative) {
    const ModelInputPathSnapshot s[3] = {
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, -128, 0.0039f, 640, 640, true, true),
        make_snap(kTensorTypeFp16, kTensorFmtNhwc, kQntNone, 0, 1.0f, 320, 320, false, false),
        make_snap(kTensorTypeFp16, kTensorFmtNhwc, kQntNone, 0, 1.0f, 320, 320, false, false),
    };
    const ModelInputPathSummary r = summarize_input_path(s, 3);
    CHECK_STR(r.pass_mode_name, std::string("xor_shift128"));  // 首个为准，不做"多数票"
    CHECK_STR(r.input_type_name, std::string("int8"));
    CHECK_EQ(r.input_w, 640u);
    CHECK_EQ(r.workers_total, 3u);
    CHECK_EQ(r.workers_fast_path, 1u);  // 偏斜被如实计数（不是 0/1 的二值化）
    CHECK_EQ(r.workers_zero_copy, 1u);
}

// ---- worker 间偏斜：3 个 worker 只有 2 个吃到快路径 ⇒ 计数必须暴露，不能平均掉 ----
TEST(input_path_summary_worker_skew_counted) {
    const ModelInputPathSnapshot s[3] = {
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, -128, 0.0039f, 640, 640, true, true),
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, -128, 0.0039f, 640, 640, true, true),
        make_snap(kTensorTypeInt8, kTensorFmtNhwc, kQntAffineAsym, -128, 0.0039f, 640, 640, false, false),
    };
    const ModelInputPathSummary r = summarize_input_path(s, 3);
    CHECK_EQ(r.workers_total, 3u);
    CHECK_EQ(r.workers_fast_path, 2u);
    CHECK_EQ(r.workers_zero_copy, 2u);
    CHECK(r.fast_path_active);  // 至少一个 ⇒ true（语义是"有"而不是"全部"）
    CHECK(r.note.empty());      // 首 worker 健康 ⇒ 无说明；偏斜由计数表达，不靠文案
}

// ---- external DMA-BUF：开关 vs 实际绑定是两件事，必须分开报 ----
TEST(input_path_summary_external_dma_flag_vs_bound) {
    const ModelInputPathSnapshot s[2] = {
        make_snap(kTensorTypeUint8, kTensorFmtNhwc, kQntNone, 0, 1.0f, 640, 640, true, false,
                  /*dma_requested=*/true, /*dma_bound=*/true),
        make_snap(kTensorTypeUint8, kTensorFmtNhwc, kQntNone, 0, 1.0f, 640, 640, true, false,
                  /*dma_requested=*/true, /*dma_bound=*/false),
    };
    const ModelInputPathSummary r = summarize_input_path(s, 2);
    CHECK(r.external_dma_requested);  // 开关开了
    CHECK(r.external_dma_bound);      // 且至少一个 worker 真绑上了
    CHECK_EQ(r.workers_total, 2u);

    // 开关开但一个都没绑上 ⇒ requested=true / bound=false（这正是要能在面板上看出来的差异）
    const ModelInputPathSnapshot t[1] = {
        make_snap(kTensorTypeUint8, kTensorFmtNhwc, kQntNone, 0, 1.0f, 640, 640, true, false,
                  /*dma_requested=*/true, /*dma_bound=*/false)};
    const ModelInputPathSummary r2 = summarize_input_path(t, 1);
    CHECK(r2.external_dma_requested);
    CHECK(!r2.external_dma_bound);
}

// ---- 原子量 → 快照：scale 走 IEEE-754 位模式，必须**逐位无损**（定点会丢精度） ----
TEST(input_path_stats_snapshot_roundtrip_and_reset) {
    InputPathStats st;
    // 缺省值 = 兼容 I/O（与 RKNNEngine::pass_mode_ 的缺省语义一致：永不为未定义值）
    const ModelInputPathSnapshot fresh = snapshot_input_path(st);
    CHECK_EQ(fresh.pass_mode, static_cast<int>(InputPassMode::kCompatible));
    CHECK_EQ(fresh.input_type, -1);
    CHECK_EQ(fresh.input_zp, 0);
    CHECK(!fresh.zero_copy_ready);
    CHECK(!fresh.fast_path_active);

    const float kScale = 1.0f / 255.0f;  // 二进制上不循环，定点必然截断；位模式必须原样回来
    st.pass_mode.store(static_cast<int32_t>(InputPassMode::kXorShift128));
    st.input_type.store(kTensorTypeInt8);
    st.input_fmt.store(kTensorFmtNhwc);
    st.input_qnt_type.store(kQntAffineAsym);
    st.input_zp.store(-128);
    store_float_bits(st.input_scale_bits, kScale);
    st.input_w.store(640);
    st.input_h.store(384);
    st.zero_copy_ready.store(true);
    st.fast_path_active.store(true);
    st.external_dma_requested.store(true);
    st.external_dma_bound.store(true);

    const ModelInputPathSnapshot snap = snapshot_input_path(st);
    CHECK_EQ(snap.pass_mode, static_cast<int>(InputPassMode::kXorShift128));
    CHECK_EQ(snap.input_type, kTensorTypeInt8);
    CHECK_EQ(snap.input_fmt, kTensorFmtNhwc);
    CHECK_EQ(snap.input_qnt_type, kQntAffineAsym);
    CHECK_EQ(snap.input_zp, -128);
    CHECK_EQ(snap.input_w, 640u);
    CHECK_EQ(snap.input_h, 384u);
    CHECK(snap.zero_copy_ready);
    CHECK(snap.fast_path_active);
    CHECK(snap.external_dma_requested);
    CHECK(snap.external_dma_bound);
    // 位级相等（不用浮点容差：本路径不允许有任何精度损失）
    CHECK(snap.input_scale == kScale);
    {
        uint32_t want = 0;
        std::memcpy(&want, &kScale, sizeof(want));
        float back = load_float_bits(st.input_scale_bits);
        uint32_t got = 0;
        std::memcpy(&got, &back, sizeof(got));
        CHECK_EQ(want, got);
    }

    // stop() 会调 reset()：停后不得残留"上一轮运行时的快路径已激活"
    st.reset();
    const ModelInputPathSnapshot cleared = snapshot_input_path(st);
    CHECK_EQ(cleared.pass_mode, static_cast<int>(InputPassMode::kCompatible));
    CHECK_EQ(cleared.input_type, -1);
    CHECK_EQ(cleared.input_scale, 0.0f);
    CHECK_EQ(cleared.input_w, 0u);
    CHECK(!cleared.zero_copy_ready);
    CHECK(!cleared.fast_path_active);
    CHECK(!cleared.external_dma_bound);
}

// ===========================================================================
// ★ 交叉锁：谓词裁决（classify_input_pass）与 文案/聚合（summarize_input_path）
//   在同一遍历里互相锁定 —— 任何人给谓词加新判据却漏改文案，本用例立刻变红。
// ===========================================================================
TEST(input_path_summary_matches_predicate_matrix) {
    const int types[] = {kTensorTypeFp32, kTensorTypeFp16, kTensorTypeInt8,
                         kTensorTypeUint8, kTensorTypeInt16};
    const int fmts[] = {kTensorFmtNchw, kTensorFmtNhwc, kTensorFmtNc1hwc2, kTensorFmtUndefined};
    const int qnts[] = {kQntNone, kQntDfp, kQntAffineAsym};
    const int zps[] = {-128, -4, 0};
    const float scales[] = {1.0f, 1.0f / 255.0f, 0.0171f};

    int compatible_seen = 0;
    int fast_seen = 0;
    for (int t : types) {
        for (int f : fmts) {
            for (int q : qnts) {
                for (int zp : zps) {
                    for (float sc : scales) {
                        const InputPassMode mode = classify_input_pass(t, f, q, sc, zp);
                        if (mode == InputPassMode::kXorShift128) {
                            // XOR 分类：判定只依赖 type/fmt/qnt/zp，**与 scale 无关**
                            // （本循环对同一组 type/fmt/qnt/zp 取三个 scale，全部应落同一结论）
                            ++fast_seen;
                            CHECK(t == kTensorTypeInt8);
                            CHECK(f == kTensorFmtNhwc);
                            CHECK(q == kQntAffineAsym);
                            CHECK(zp == -128);

                            // 就绪态 ⇒ 无文案
                            const ModelInputPathSnapshot ok[1] = {make_snap(t, f, q, zp, sc, 640, 640, true, true)};
                            const ModelInputPathSummary rok = summarize_input_path(ok, 1);
                            CHECK_STR(rok.pass_mode_name, std::string("xor_shift128"));
                            CHECK(rok.note.empty());

                            // 未就绪态 ⇒ 必须有文案（不许沉默）
                            const ModelInputPathSnapshot no[1] = {make_snap(t, f, q, zp, sc, 640, 640, false, false)};
                            const ModelInputPathSummary rno = summarize_input_path(no, 1);
                            CHECK(!rno.note.empty());
                        } else if (mode == InputPassMode::kCompatible) {
                            ++compatible_seen;
                            // ★ 核心交叉锁：凡是判定为兼容 I/O 的组合，文案**必须**给出理由。
                            //   空文案 = 用户看到"兼容 I/O"却不知道为什么 —— 正是本次要消灭的盲区。
                            const ModelInputPathSnapshot s[1] = {make_snap(t, f, q, zp, sc, 640, 640, true, false)};
                            const ModelInputPathSummary r = summarize_input_path(s, 1);
                            CHECK_STR(r.pass_mode_name, std::string("compatible"));
                            CHECK(!r.note.empty());
                            CHECK_EQ(r.workers_fast_path, 0u);
                        } else {  // kUint8Native
                            CHECK(t == kTensorTypeUint8);
                            CHECK(f == kTensorFmtNhwc);
                            const ModelInputPathSnapshot s[1] = {make_snap(t, f, q, zp, sc, 640, 640, true, false)};
                            const ModelInputPathSummary r = summarize_input_path(s, 1);
                            CHECK_STR(r.pass_mode_name, std::string("uint8_native"));
                            CHECK(r.note.empty());  // 最优路径无需解释
                        }
                    }
                }
            }
        }
    }
    // 矩阵必须真的覆盖到三类结论（防"循环写错导致一个分支都没进"却全绿）
    CHECK(compatible_seen > 0);
    CHECK(fast_seen > 0);
}
