// test_ota_golden.cpp — OTA C++ 移植的逐值对拍（框架：core/tests/test_util.hpp）。
//
// 基准 = core/tests/fixtures/ota_golden.json —— 由 tools/ota_golden_gen.py 在删 Python
// **之前**冻结的现役实现输出（114 例）。OTA 是唯一升级通道，本文件是等价性的唯一判据。
//
// ★ 最高优先级是 A 段 canonical：Ed25519 签名是对这些**字节**签的，
//   键序/空格/转义/非 ASCII 任一处不同 ⇒ 验签必然失败 ⇒ 板子升不动。
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "ota/OtaCanonical.hpp"
#include "ota/OtaVersion.hpp"
#include "test_util.hpp"

using ttbox::core::JsonParseResult;
using ttbox::core::JsonValue;
using ttbox::core::json_parse;

namespace {

#define EXPECT(cond, msg)                                                      \
    do {                                                                       \
        if (!(cond)) ::ttbox_test::report_failure(__FILE__, __LINE__, (msg));  \
    } while (0)

JsonValue g_root;
bool g_loaded = false;
bool g_load_failed = false;

const JsonValue* golden() {
    if (!g_loaded) {
        g_loaded = true;
        std::ifstream fh(TTBOX_OTA_GOLDEN, std::ios::binary);
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

#define REQUIRE_GOLDEN(root)                                                    \
    const JsonValue* root = golden();                                           \
    if ((root) == nullptr) {                                                    \
        TEST_SKIP_REQUIRED(std::string("OTA 黄金样本不可用: ") + TTBOX_OTA_GOLDEN); \
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

}  // namespace

// ============ 1. canonical（验签序列化，最高优先级）============
TEST(ota_golden_canonical) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("A_canonical");
    EXPECT(arr != nullptr && arr->is_array(), "A_canonical 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    int n = 0;
    for (const JsonValue& row : arr->as_array()) {
        const JsonValue* in = row.find("in");
        const std::string want = jstr(row, "out");
        if (in == nullptr) continue;
        const std::string got = ttbox::core::ota::canonical_dump(*in);
        EXPECT(got == want, "canonical 不一致:\n     got  = '" + got + "'\n     want = '" + want + "'");
        n++;
    }
    EXPECT(n > 0, "A 段为空");
}

// ============ 2. 版本排序键 ============
TEST(ota_golden_version_key) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("B_version_key");
    EXPECT(arr != nullptr && arr->is_array(), "B_version_key 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    for (const JsonValue& row : arr->as_array()) {
        const std::string in = jstr(row, "in");
        const JsonValue* want = row.find("out");
        if (want == nullptr || !want->is_array()) continue;
        const std::vector<ttbox::core::ota::VersionSeg> got =
            ttbox::core::ota::version_key(in);
        EXPECT(got.size() == want->as_array().size(),
               "version_key('" + in + "') 段数 " + std::to_string(got.size()) + " != " +
                   std::to_string(want->as_array().size()));
        const size_t n = got.size() < want->as_array().size() ? got.size()
                                                              : want->as_array().size();
        for (size_t i = 0; i < n; ++i) {
            const auto& cols = want->as_array()[i].as_array();
            if (cols.size() < 3) continue;
            EXPECT(got[i].cls == cols[0].as_int(-1) && got[i].num == cols[1].as_int(-1) &&
                       got[i].text == cols[2].as_string(""),
                   "version_key('" + in + "') 第 " + std::to_string(i) + " 段不一致");
        }
    }
}

// ============ 3. 降级判定（含 9<10 数值比较）============
TEST(ota_golden_is_downgrade) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("C_is_downgrade");
    EXPECT(arr != nullptr && arr->is_array(), "C_is_downgrade 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    int n_true = 0, n_false = 0;
    for (const JsonValue& row : arr->as_array()) {
        const std::string nv = jstr(row, "new");
        const std::string cv = jstr(row, "cur");
        const bool want = jbool(row, "out", false);
        const bool got = ttbox::core::ota::is_downgrade(nv, cv);
        EXPECT(got == want,
               "is_downgrade('" + nv + "', '" + cv + "') = " + (got ? "true" : "false") +
                   " 期望 " + (want ? "true" : "false"));
        if (want) n_true++; else n_false++;
    }
    // 两类都必须跑到（否则"全绿"是空断言）
    EXPECT(n_true > 0, "C 段无 true 例");
    EXPECT(n_false > 0, "C 段无 false 例");
}

// ============ 4. 标识符消毒 ============
TEST(ota_golden_check_safe_id) {
    REQUIRE_GOLDEN(root);
    const JsonValue* arr = root->find("D_check_safe_id");
    EXPECT(arr != nullptr && arr->is_array(), "D_check_safe_id 缺失");
    if (arr == nullptr || !arr->is_array()) return;
    int n_ok = 0, n_bad = 0;
    for (const JsonValue& row : arr->as_array()) {
        const std::string in = jstr(row, "in");
        const bool want_ok = jbool(row, "ok", false);
        std::string got_out;
        const bool got_ok = ttbox::core::ota::check_safe_id(in, &got_out);
        EXPECT(got_ok == want_ok, "check_safe_id('" + in + "') ok=" +
                                      (got_ok ? "true" : "false") + " 期望 " +
                                      (want_ok ? "true" : "false"));
        if (want_ok && got_ok) {
            EXPECT(got_out == jstr(row, "out"), "check_safe_id('" + in + "') 输出不一致");
        }
        if (want_ok) n_ok++; else n_bad++;
    }
    EXPECT(n_ok > 0, "D 段无合法例");
    EXPECT(n_bad > 0, "D 段无非法例（应含 ../../etc/passwd 等）");
}

// ============ 5. 常量表 ============
TEST(ota_golden_meta) {
    REQUIRE_GOLDEN(root);
    const JsonValue* meta = root->find("E_meta");
    EXPECT(meta != nullptr, "E_meta 缺失");
    if (meta == nullptr) return;
    const JsonValue* sf = meta->find("SIGNED_FIELDS");
    EXPECT(sf != nullptr && sf->is_array() && sf->as_array().size() == 4, "SIGNED_FIELDS 应为 4 项");
    if (sf != nullptr && sf->is_array()) {
        static const char* kWant[4] = {"sha256", "version", "built_at", "key_id"};
        for (size_t i = 0; i < sf->as_array().size() && i < 4; ++i) {
            EXPECT(sf->as_array()[i].as_string("") == kWant[i],
                   "SIGNED_FIELDS[" + std::to_string(i) + "] 不一致");
        }
    }
    EXPECT(jstr(*meta, "DEFAULT_KEY_ID") == "ttbox-ota-2026b", "DEFAULT_KEY_ID 不一致");
    EXPECT(jstr(*meta, "SAFE_ID_RE") == "^[A-Za-z0-9][A-Za-z0-9._-]*$", "SAFE_ID_RE 不一致");
}

// ============ 6. canonical 的"键序无关"自证（不依赖样本）============
TEST(ota_canonical_key_order_invariant) {
    JsonValue a = JsonValue::object();
    a.set("z", JsonValue::number(1));
    a.set("a", JsonValue::number(2));
    JsonValue b = JsonValue::object();
    b.set("a", JsonValue::number(2));
    b.set("z", JsonValue::number(1));
    EXPECT(ttbox::core::ota::canonical_dump(a) == ttbox::core::ota::canonical_dump(b),
           "键序不同的同内容对象 canonical 必须相同");
    EXPECT(ttbox::core::ota::canonical_dump(a) == "{\"a\":2,\"z\":1}", "canonical 排序/格式不对");
    // 中文原样（ensure_ascii=False）
    JsonValue c = JsonValue::object();
    c.set("k", JsonValue::string("中"));
    EXPECT(ttbox::core::ota::canonical_dump(c) == "{\"k\":\"中\"}", "非 ASCII 应原样输出");
}
