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
#include "edid/EdidApply.hpp"
#include "edid/EdidBuilder.hpp"
#include "edid/EdidConfig.hpp"
#include "edid/EdidTiming.hpp"
#include "edid/EdidValidator.hpp"
#include "test_util.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

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

// ============ 7. E 段：native_mode 保护（edid_apply.sh::PYEOF 逻辑）============
TEST(edid_golden_native_mode_guard) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("E_native_mode_guard");
    EXPECT(arr != nullptr && arr->is_array(), "E_native_mode_guard 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    for (const JsonValue& row : arr->as_array()) {
        const std::string profile = jstr(row, "profile");
        const std::string nm_in = jstr(row, "native_mode_in");
        const std::string want = jstr(row, "resolved");
        const std::string got = ttbox::core::edid::resolve_native_mode(profile, nm_in);
        EXPECT(got == want, "resolve_native_mode(profile='" + profile + "', nm='" + nm_in +
                                "') = '" + got + "' 期望 '" + want + "'");
    }
}

// ============ 8. F 段：字段归一工具（safe_ascii / hex_text / bool_value）============
TEST(edid_golden_helpers) {
    REQUIRE_GOLDEN(root);
    const JsonValue* h = root->find("F_helpers");
    EXPECT(h != nullptr, "F_helpers 缺失");
    if (h == nullptr) return;
    const JsonValue* arr = h->find("helpers");
    EXPECT(arr != nullptr && arr->is_array(), "helpers 段缺失");
    if (arr == nullptr || !arr->is_array()) return;
    for (const JsonValue& row : arr->as_array()) {
        const std::string fn = jstr(row, "fn");
        const JsonValue* in = row.find("in");
        std::string got;
        // ★ 期望值口径：字符串类 helper 的 out 是 JSON 字符串；_bool_value 的 out 是
        //   JSON **布尔**（Python bool）⇒ 归一成 "True"/"False" 再比，否则读成空串假失败。
        const JsonValue* outv = row.find("out");
        const std::string want = (outv != nullptr && outv->is_bool())
                                     ? (outv->as_bool(false) ? std::string("True")
                                                            : std::string("False"))
                                     : jstr(row, "out");
        if (fn == "_safe_ascii") {
            const std::string s = (in == nullptr) ? std::string() : in->as_string("");
            got = ttbox::core::edid::safe_ascii(s, static_cast<size_t>(jint(row, "limit", 0)),
                                                jstr(row, "fallback"));
        } else if (fn == "_hex_text") {
            const std::string s = (in == nullptr) ? std::string() : in->as_string("");
            got = ttbox::core::edid::hex_text(s, static_cast<int>(jint(row, "width", 0)),
                                              jstr(row, "fallback"));
        } else if (fn == "_bool_value") {
            JsonValue placeholder = JsonValue::null();
            const JsonValue& v = (in == nullptr) ? placeholder : *in;
            got = ttbox::core::edid::bool_value(v, jbool(row, "fallback", false)) ? "True" : "False";
        } else {
            EXPECT(false, "未知 helper: " + fn);
            continue;
        }
        EXPECT(got == want, fn + "(in=" + (in == nullptr ? std::string("null") : in->is_string()
                                                                  ? "\"" + in->as_string("") + "\""
                                                                  : std::string("scalar")) +
                              ") = '" + got + "' 期望 '" + want + "'");
    }
}

// ============ 9. 面板模式列表（自 _probe_edid_modes 移植）============
// 旧实现是"调 hdmirx_edid --list 再解析文本"，而那个命令打印的就是 TIMING_MAP
// ⇒ 这里用「与时序表逐条一致」作为等价判据（无需另存黄金样本）。
TEST(edid_panel_modes_json) {
    const JsonValue all = ttbox::core::edid::advertised_modes_json();
    EXPECT(all.is_array(), "advertised_modes_json 不是数组");
    if (!all.is_array()) return;
    const auto& arr = all.as_array();
    EXPECT(arr.size() == ttbox::core::edid::timing_map().size(), "条数与时序表不一致");

    size_t i = 0;
    for (const JsonValue& e : arr) {
        if (i >= ttbox::core::edid::timing_map().size()) break;
        const auto& kv = ttbox::core::edid::timing_map()[i];
        const ttbox::core::edid::DisplayTiming& t = kv.second;
        EXPECT(jstr(e, "token") == kv.first, "第 " + std::to_string(i) + " 条 token 不一致");
        char label[64];
        std::snprintf(label, sizeof(label), "%dx%d@%d", t.width, t.height,
                      static_cast<int>(t.refresh));
        EXPECT(jstr(e, "label") == label,
               "第 " + std::to_string(i) + " 条 label='" + jstr(e, "label") + "' 期望 '" + label +
                   "'");
        EXPECT(jint(e, "width", -1) == t.width && jint(e, "height", -1) == t.height,
               "第 " + std::to_string(i) + " 条宽高不一致");
        EXPECT(jint(e, "refresh", -1) == static_cast<int>(t.refresh),
               "第 " + std::to_string(i) + " 条刷新率不一致");
        EXPECT(jint(e, "pixel_clock_khz", -1) ==
                   static_cast<int64_t>(ttbox::core::edid::py_round(t.pixel_clock * 1000.0)),
               "第 " + std::to_string(i) + " 条像素时钟不一致");
        i++;
    }
    // 面板用截断版：<=16 时内容必须与全量一致
    const JsonValue cut = ttbox::core::edid::advertised_modes_json_truncated(16);
    EXPECT(cut.is_array() && cut.as_array().size() == arr.size(), "截断 16 不应改变条数");
}

// ============ 10. PnP 编解码 ============
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

// ============ 11. 应用流程的纯函数（判锁 / JSON 输出 / 设备白名单）============
namespace {
// 断言 JSON 里存在 "k":v（容忍 ": " 与 ":" 两种分隔）
bool json_has_kv(const std::string& j, const std::string& k, const std::string& v) {
    return j.find("\"" + k + "\":" + v) != std::string::npos ||
           j.find("\"" + k + "\": " + v) != std::string::npos;
}
}  // namespace

TEST(edid_apply_pure_helpers) {
    using ttbox::core::edid::v4l2_timing_locked;
    using ttbox::core::edid::debugfs_locked;
    using ttbox::core::edid::make_failure_json;
    using ttbox::core::edid::make_success_json;

    // debugfs 判锁：Clk + Ch0/1/2 四段全 Lock 才算锁上（脚本用 grep -qE 同款）
    EXPECT(debugfs_locked("Clk-Ch:Lock Ch0:Lock Ch1:Lock Ch2:Lock\n"), "四通道全锁应判锁上");
    EXPECT(!debugfs_locked("Clk-Ch:Unlock Ch0:Lock Ch1:Lock Ch2:Lock\n"), "Clk 未锁应判未锁");
    EXPECT(!debugfs_locked("Clk-Ch:Lock Ch0:Lock Ch1:Lock Ch2:Unlock\n"), "Ch2 未锁应判未锁");
    EXPECT(!debugfs_locked("Clk-Ch:Lock Ch0:Lock Ch1:Lock\n"), "缺 Ch2 应判未锁");
    EXPECT(!debugfs_locked(""), "空文本应判未锁");
    EXPECT(!debugfs_locked("其它文本\n"), "无 Clk-Ch 行应判未锁");
    // 多行时只看含 Clk-Ch 的那一行
    EXPECT(!debugfs_locked("Mode: HDMI\nClk-Ch:Unlock Ch0:Lock Ch1:Lock Ch2:Lock\n"),
           "应取 Clk-Ch 所在行判定");

    // v4l2 判锁：无 failed / No locks 即锁上
    EXPECT(v4l2_timing_locked("Active width: 2560\n"), "正常输出应判锁上");
    EXPECT(!v4l2_timing_locked("failed to query\n"), "含 failed 应判未锁");
    EXPECT(!v4l2_timing_locked("No locks\n"), "含 No locks 应判未锁");

    // JSON 输出结构（供 core 原样转发）
    const std::string ok = make_success_json(true, "/opt/ttbox/runtime/edid/current.bin", "TTBox-COMPAT");
    EXPECT(json_has_kv(ok, "ok", "true"), "成功 JSON 应含 ok:true");
    EXPECT(json_has_kv(ok, "hpd", "\"rehandshake\""), "重协商模式 hpd 应为 rehandshake");
    EXPECT(json_has_kv(ok, "method", "\"v4l2_ctl\""), "成功 JSON 应含 method");
    EXPECT(ok.find("TTBox-COMPAT") != std::string::npos, "成功 JSON 应含模式名");
    const std::string ok2 = make_success_json(false, "f", "m");
    EXPECT(json_has_kv(ok2, "hpd", "\"unchanged\""), "纯注入模式 hpd 应为 unchanged");

    const std::string bad = make_failure_json(true, false, "EDID 已写入但未锁定");
    EXPECT(json_has_kv(bad, "ok", "false"), "失败 JSON 应含 ok:false");
    EXPECT(json_has_kv(bad, "edid_applied", "true"), "失败 JSON 应含 edid_applied");
    EXPECT(json_has_kv(bad, "locked", "false"), "失败 JSON 应含 locked");

    // 设备白名单：非 /dev/video0 必须拒绝（脚本同款硬拦），且不碰任何硬件
    ttbox::core::edid::ApplyOptions bad_opt;
    bad_opt.video_dev = "/dev/dri/card0";
    const ttbox::core::edid::ApplyResult br = ttbox::core::edid::apply(bad_opt);
    EXPECT(!br.ok, "非 /dev/video0 应拒绝");
    EXPECT(br.error.find("必须使用 /dev/video0") != std::string::npos, "应说明设备错误");
}

// ============ 12. dry-run 走通「读配置 → 保护 → 构建 → 落盘」全链 ============
TEST(edid_apply_dry_run) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // 唯一目录名用时间戳而非 getpid：getpid 在 MSYS/Linux 头文件位置不同，跨平台易踩坑
    const long long uniq =
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tmp = fs::temp_directory_path(ec) / ("ttbox_edid_test_" + std::to_string(uniq));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "config", ec);

    // 出厂兼容身份（deploy/config/hardware_display.json 原样）
    const std::string cfg =
        R"({"device":"auto","name":"TTBox-COMPAT","vendor":"AIB","product_id":"0x2400",)"
        R"("serial":"0xA1B00001","native_mode":"1080p240","native_only":true,"profile":"boot-safe-full"})";
    {
        std::ofstream f(tmp / "config" / "hardware_display.json", std::ios::binary);
        f << cfg;
    }

    ttbox::core::edid::ApplyOptions opt;
    opt.prefix = tmp.string();
    opt.dry_run = true;
    const ttbox::core::edid::ApplyResult r = ttbox::core::edid::apply(opt);
    EXPECT(r.ok, "dry-run 应成功：" + r.error);
    EXPECT(r.edid_applied, "dry-run 应标记 EDID 已生成");

    const fs::path out = tmp / "runtime" / "edid" / "current.bin";
    EXPECT(fs::exists(out), "current.bin 应已生成（含自动建目录）");
    if (fs::exists(out)) {
        std::ifstream f(out, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string got = ss.str();
        EXPECT(got.size() == 256, "current.bin 应为 256 字节，实际 " + std::to_string(got.size()));

        // 与"直接构建同一配置"逐字节一致 —— 证明 apply 没在中间改坏配置
        const JsonParseResult pr = json_parse(cfg);
        EXPECT(pr.ok, "测试配置应可解析");
        if (pr.ok) {
            const ttbox::core::edid::BuildResult direct =
                ttbox::core::edid::build_from_config(pr.value);
            EXPECT(direct.ok, "直接构建应成功");
            if (direct.ok) {
                const std::string want(reinterpret_cast<const char*>(direct.edid.data()),
                                       direct.edid.size());
                EXPECT(got == want, "current.bin 应与直接构建逐字节一致");
            }
        }
    }
    fs::remove_all(tmp, ec);
}
