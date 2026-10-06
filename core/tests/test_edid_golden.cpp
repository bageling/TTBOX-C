// test_edid_golden.cpp — EDID C++ 移植的**逐字节对拍**（框架：core/tests/test_util.hpp）。
//
// 基准 = core/tests/fixtures/edid_golden.json —— 由 tools/edid_golden_gen.py 在删 Python
// **之前**冻结的现役 Python 实现输出（185 例）。本文件是"移植是否等价"的唯一判据。
//
// ★ 口径：失败例只断言「同样失败」，**不断言错误文案**——文案不是接口。
// ★ 覆盖：meta 常量表 / mode_info / CVT-RB 像素时钟 / lookup_timing / build→256B / PnP。
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "edid/EdidBuilder.hpp"
#include "edid/EdidTiming.hpp"
#include "edid/EdidValidator.hpp"
#include "test_util.hpp"

using ttbox::core::JsonParseResult;
using ttbox::core::JsonValue;
using ttbox::core::json_parse;

namespace {

// 断言宏：把消息一并报出（CHECK 只能报表达式文本，对中文长串不便）
#define EXPECT(cond, msg)                                        \
    do {                                                         \
        if (!(cond)) ::ttbox_test::report_failure(__FILE__, __LINE__, (msg)); \
    } while (0)

// ---- 黄金样本（懒加载，进程内只读一次）----
JsonValue g_root;
bool g_loaded = false;
bool g_load_failed = false;

const JsonValue* golden() {
    if (!g_loaded) {
        g_loaded = true;
        std::ifstream fh(TTBOX_EDID_GOLDEN, std::ios::binary);
        if (!fh.good()) {
            g_load_failed = true;
            return nullptr;
        }
        std::ostringstream ss;
        ss << fh.rdbuf();
        const JsonParseResult r = json_parse(ss.str());
        if (!r.ok || !r.value.is_object()) {
            g_load_failed = true;
            return nullptr;
        }
        g_root = r.value;
    }
    if (g_load_failed) return nullptr;
    return &g_root;
}

// 每条用例开头：`const JsonValue* root = golden();` 之后若为 nullptr，用
// TEST_SKIP_REQUIRED 跳过（该宏含裸 `return;`，只能出现在返回 void 的 TEST 体内）。
#define REQUIRE_GOLDEN(root)                                                          \
    const JsonValue* root = golden();                                                 \
    if ((root) == nullptr) {                                                          \
        TEST_SKIP_REQUIRED(std::string("EDID 黄金样本不可用: ") + TTBOX_EDID_GOLDEN);  \
    }

// ---- 小工具 ----
std::string hexof(const std::vector<uint8_t>& b) {
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) {
        s += k[(c >> 4) & 0xF];
        s += k[c & 0xF];
    }
    return s;
}

std::string jstr(const JsonValue& o, const char* k) {
    const JsonValue* v = o.find(k);
    return v == nullptr ? std::string() : v->as_string("");
}

bool jbool(const JsonValue& o, const char* k, bool def) {
    const JsonValue* v = o.find(k);
    if (v == nullptr || !v->is_bool()) return def;
    return v->as_bool(def);
}

int64_t jint(const JsonValue& o, const char* k, int64_t def) {
    const JsonValue* v = o.find(k);
    return v == nullptr ? def : v->as_int(def);
}

double jnum(const JsonValue& o, const char* k, double def) {
    const JsonValue* v = o.find(k);
    return v == nullptr ? def : v->as_number(def);
}

}  // namespace

// ============ 1. meta：常量表（表内容漂移即 FAIL）============
TEST(edid_golden_meta) {
    REQUIRE_GOLDEN(root);
    const JsonValue* meta = root->find("meta");
    EXPECT(meta != nullptr, "meta 段缺失");
    if (meta == nullptr) return;

    const JsonValue* tm = meta->find("timing_map");
    EXPECT(tm != nullptr && tm->is_object(), "timing_map 缺失");
    if (tm != nullptr && tm->is_object()) {
        EXPECT(tm->as_object().size() == ttbox::core::edid::timing_map().size(),
               "timing_map 条目数不一致");
        for (const auto& kv : tm->as_object()) {
            ttbox::core::edid::DisplayTiming t;
            const bool found = ttbox::core::edid::lookup_timing(kv.first, &t);
            EXPECT(found, "timing_map 缺 token: " + kv.first);
            if (!found) continue;
            const JsonValue& e = kv.second;
            EXPECT(t.width == jint(e, "w", -1) && t.height == jint(e, "h", -1),
                   kv.first + " 宽高不一致");
            EXPECT(t.pixel_clock == jnum(e, "pc", -1.0), kv.first + " 像素时钟不一致");
            EXPECT(t.h_front_porch == jint(e, "hfp", -1) && t.h_sync == jint(e, "hs", -1) &&
                       t.h_back_porch == jint(e, "hbp", -1),
                   kv.first + " 水平空白不一致");
            EXPECT(t.v_front_porch == jint(e, "vfp", -1) && t.v_sync == jint(e, "vs", -1) &&
                       t.v_back_porch == jint(e, "vbp", -1),
                   kv.first + " 垂直空白不一致");
            EXPECT(t.h_pol == jbool(e, "h_pol", true) && t.v_pol == jbool(e, "v_pol", true),
                   kv.first + " 同步极性不一致");
        }
    }

    const JsonValue* sm = meta->find("safe_modes");
    EXPECT(sm != nullptr && sm->is_array(), "safe_modes 缺失");
    if (sm != nullptr && sm->is_array()) {
        EXPECT(sm->as_array().size() == ttbox::core::edid::safe_modes().size(),
               "safe_modes 条目数不一致");
        size_t i = 0;
        for (const JsonValue& row : sm->as_array()) {
            if (i >= ttbox::core::edid::safe_modes().size()) break;
            const auto& ref = ttbox::core::edid::safe_modes()[i];
            const auto& cols = row.as_array();
            if (cols.size() >= 5) {
                EXPECT(ref.token == cols[0].as_string("") && ref.width == cols[1].as_int(0) &&
                           ref.height == cols[2].as_int(0) && ref.refresh == cols[3].as_int(0) &&
                           ref.pixel_clock_khz == cols[4].as_int(0),
                       "safe_modes[" + std::to_string(i) + "] 不一致");
            }
            i++;
        }
    }

    const JsonValue* pset = meta->find("profiles_set");
    if (pset != nullptr && pset->is_array()) {
        // profiles_set 内容由 EdidConfig 落地后在此续断言（当前仅记数）
        EXPECT(pset->as_array().size() > 0, "profiles_set 为空");
    }
}

// ============ 2. mode_info（含非法输入）============
TEST(edid_golden_mode_info) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("A_mode_info");
    EXPECT(arr != nullptr && arr->is_array(), "A_mode_info 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    int n = 0;
    for (const JsonValue& row : arr->as_array()) {
        const std::string token = jstr(row, "token");
        const JsonValue* res = row.find("result");
        const bool py_has = (res != nullptr && !res->is_null());
        ttbox::core::edid::ModeInfo mi;
        const bool cpp_has = ttbox::core::edid::mode_info(token, &mi);
        EXPECT(py_has == cpp_has, "mode_info('" + token + "') 有无判定不一致");
        if (py_has && cpp_has && res->is_array() && res->as_array().size() >= 5) {
            const auto& c = res->as_array();
            EXPECT(mi.token == c[0].as_string("") && mi.width == c[1].as_int(0) &&
                       mi.height == c[2].as_int(0) && mi.refresh == c[3].as_int(0) &&
                       mi.pixel_clock_khz == c[4].as_int(0),
                   "mode_info('" + token + "') 值不一致");
        }
        n++;
    }
    EXPECT(n > 0, "A 段用例为空");
}

// ============ 3. CVT-RB 像素时钟（单一真源）============
TEST(edid_golden_pixel_clock) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("B_pixel_clock");
    EXPECT(arr != nullptr && arr->is_array(), "B_pixel_clock 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    for (const JsonValue& row : arr->as_array()) {
        const int w = static_cast<int>(jint(row, "w", 0));
        const int h = static_cast<int>(jint(row, "h", 0));
        const int r = static_cast<int>(jint(row, "r", 0));
        const int64_t want = jint(row, "khz", -1);
        const int64_t got = ttbox::core::edid::reduced_blanking_pixel_clock_khz(w, h, r);
        EXPECT(got == want, "pc(" + std::to_string(w) + "x" + std::to_string(h) + "@" +
                                std::to_string(r) + ")=" + std::to_string(got) + " 期望 " +
                                std::to_string(want));
    }
}

// ============ 4. lookup_timing + verify() ============
TEST(edid_golden_lookup_timing) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("C_lookup_timing");
    EXPECT(arr != nullptr && arr->is_array(), "C_lookup_timing 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    for (const JsonValue& row : arr->as_array()) {
        const std::string token = jstr(row, "token");
        const bool py_ok = jbool(row, "ok", false);
        ttbox::core::edid::DisplayTiming t;
        const bool cpp_ok = ttbox::core::edid::lookup_timing(token, &t);
        EXPECT(py_ok == cpp_ok, "lookup_timing('" + token + "') 有无判定不一致");
        if (!py_ok || !cpp_ok) continue;
        const JsonValue* want = row.find("timing");
        if (want == nullptr || !want->is_object()) continue;
        EXPECT(t.width == jint(*want, "w", -1) && t.height == jint(*want, "h", -1),
               token + " 宽高不一致");
        EXPECT(t.h_front_porch == jint(*want, "hfp", -1) && t.h_sync == jint(*want, "hs", -1) &&
                   t.h_back_porch == jint(*want, "hbp", -1) &&
                   t.v_front_porch == jint(*want, "vfp", -1) &&
                   t.v_sync == jint(*want, "vs", -1) &&
                   t.v_back_porch == jint(*want, "vbp", -1),
               token + " 空白参数不一致");
        EXPECT(t.pixel_clock_10khz() == jint(*want, "pc_10khz", -1), token + " pc_10khz 不一致");
        EXPECT(t.h_blank() == jint(*want, "h_blank", -1) &&
                   t.v_blank() == jint(*want, "v_blank", -1) &&
                   t.h_total() == jint(*want, "h_total", -1) &&
                   t.v_total() == jint(*want, "v_total", -1),
               token + " 派生量不一致");
        const std::string py_verr = jstr(*want, "verify_error");
        EXPECT(t.verify() == py_verr,
               token + " verify()='" + t.verify() + "' 期望 '" + py_verr + "'");
    }
}

// ============ 5. build_from_config → 256 字节（核心）============
TEST(edid_golden_build) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("D_build");
    EXPECT(arr != nullptr && arr->is_array(), "D_build 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    int n_ok = 0;
    int n_fail = 0;
    for (const JsonValue& row : arr->as_array()) {
        const JsonValue* cfg = row.find("config");
        if (cfg == nullptr) continue;
        const bool py_ok = jbool(row, "ok", false);
        const std::string tag = jstr(*cfg, "native_mode") + "/only=" +
                                (jbool(*cfg, "native_only", false) ? "T" : "F") + "/pid=" +
                                jstr(*cfg, "product_id");
        const ttbox::core::edid::BuildResult got = ttbox::core::edid::build_from_config(*cfg);
        EXPECT(py_ok == got.ok, "build " + tag + " 成功判定不一致（cpp=" +
                                    (got.ok ? std::string("ok")
                                            : std::string("fail:") + got.error) +
                                    "）");
        if (!py_ok) {
            n_fail++;
            continue;
        }
        if (!got.ok) continue;
        n_ok++;
        EXPECT(got.edid.size() == 256, "build " + tag + " 长度=" + std::to_string(got.edid.size()));
        EXPECT(hexof(got.edid) == jstr(row, "hex"), "build " + tag + " 256 字节不一致");
        std::vector<std::string> errs;
        const bool vok = ttbox::core::edid::verify_edid(got.edid, &errs);
        EXPECT(vok == jbool(row, "verify_ok", false), "build " + tag + " verify 结论不一致");
    }
    // 基准里既有成功例也有失败例，两类都必须被跑到（否则"全绿"是空断言）
    EXPECT(n_ok > 0, "D 段成功例为 0");
    EXPECT(n_fail > 0, "D 段失败例为 0（基准应含 1440p165 超限与非法 product_id）");
}

// ============ 6. PnP 编解码 ============
TEST(edid_golden_pnp) {
    REQUIRE_GOLDEN(root);
    const JsonValue* h = root->find("F_helpers");
    EXPECT(h != nullptr, "F_helpers 缺失");
    if (h == nullptr) return;
    const JsonValue* arr = h->find("pnp");
    EXPECT(arr != nullptr && arr->is_array(), "pnp 段缺失");
    if (arr == nullptr || !arr->is_array()) return;
    for (const JsonValue& row : arr->as_array()) {
        const std::string vendor = jstr(row, "vendor");
        std::vector<uint8_t> enc;
        const bool ok = ttbox::core::edid::pnp_encode(vendor, &enc);
        EXPECT(ok, "pnp_encode('" + vendor + "') 应成功");
        if (!ok) continue;
        EXPECT(hexof(enc) == jstr(row, "bytes"), "pnp_encode('" + vendor + "') 字节不一致");
        EXPECT(ttbox::core::edid::pnp_decode(enc) == jstr(row, "roundtrip"),
               "pnp_decode roundtrip('" + vendor + "') 不一致");
    }
}
