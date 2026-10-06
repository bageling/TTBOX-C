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
#include "ota/OtaCrypto.hpp"
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

// ============ 7. SHA-256 标准测试向量（自实现必须钉死）============
TEST(ota_sha256_vectors) {
    using ttbox::core::ota::sha256_hex;
    EXPECT(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "空串 SHA-256 不对");
    EXPECT(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "\"abc\" SHA-256 不对");
    EXPECT(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "448bit SHA-256 不对");
    // 一百万个 'a'：走分块路径（1MB 缓冲的边界）
    const std::string big(1000000, 'a');
    EXPECT(sha256_hex(big) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
           "1M 'a' SHA-256 不对");
}

// ============ 8. 真实历史签名往返（★ 最强的一步）============
// 拿 V1.0.55 的**真实旁车签名**跑一遍完整链路：读 sign.json → 取 SIGNED_FIELDS →
// canonical → base64 解签名 → PEM 解公钥 → Ed25519 验签。
// ★ 同一份数据已用 Python cryptography 验过（结果：通过，见 fixture 的 _python_crosscheck）。
//   本用例的意义就是钉住「C++ 侧与 Python 侧对同一签名结论一致」。
TEST(ota_real_signature_roundtrip) {
    using namespace ttbox::core::ota;
    std::ifstream fh(TTBOX_OTA_REAL_SIG, std::ios::binary);
    EXPECT(fh.good(), std::string("打不开真实签名 fixture: ") + TTBOX_OTA_REAL_SIG);
    if (!fh.good()) {
        TEST_SKIP_REQUIRED(std::string("真实签名 fixture 不可用: ") + TTBOX_OTA_REAL_SIG);
    }
    std::ostringstream ss;
    ss << fh.rdbuf();
    const JsonParseResult pr = json_parse(ss.str());
    EXPECT(pr.ok && pr.value.is_object(), "真实签名 fixture 解析失败");
    if (!pr.ok || !pr.value.is_object()) return;
    const JsonValue* root_v = &pr.value;

    // 1) 公钥 PEM → raw 32B
    uint8_t pk[32] = {0};
    const std::string pem = jstr(*root_v, "public_key_pem");
    EXPECT(load_ed25519_pem(pem, pk), "PEM 公钥解析失败");
    // 已知答案：公钥 hex 应为 8a25d73f00f0e07b1d1eee59d5d0dd8d0e4b4a4e9ee3f0...（长度 32 字节）
    // —— 只断言"解出了非全零的 32 字节"，具体值由下面的真实验签兜住
    bool all_zero = true;
    for (int i = 0; i < 32; ++i) {
        if (pk[i] != 0) all_zero = false;
    }
    EXPECT(!all_zero, "PEM 解出的公钥不应全零");

    // 2) sign.json 的 SIGNED_FIELDS → canonical
    const JsonValue* sj = root_v->find("sign_json");
    EXPECT(sj != nullptr && sj->is_object(), "fixture 缺 sign_json");
    if (sj == nullptr || !sj->is_object()) return;
    const std::vector<std::string> kSigned = {"sha256", "version", "built_at", "key_id"};
    const std::string canon = canonical_signed_fields(*sj, kSigned);
    EXPECT(canon.size() == 146, "canonical 长度应为 146，实际 " + std::to_string(canon.size()));
    EXPECT(canon.find("\"version\":\"V1.0.55\"") != std::string::npos, "canonical 内容异常");

    // 3) base64 解签名（64 字节）
    std::vector<uint8_t> sig;
    EXPECT(base64_decode(jstr(*sj, "signature"), &sig), "签名 base64 解码失败");
    EXPECT(sig.size() == 64, "签名长度应为 64 字节，实际 " + std::to_string(sig.size()));
    if (sig.size() != 64) return;

    // 4) 验签 —— 真实签名应当通过
    EXPECT(verify_ed25519(sig.data(), canon, pk), "★ 真实签名验签失败（C++ 与 Python 结论不一致）");

    // 5) 负例：改一个字节必须失败（验签不是摆设）
    sig[0] = static_cast<uint8_t>(sig[0] ^ 0x01);
    EXPECT(!verify_ed25519(sig.data(), canon, pk), "篡改签名后仍验签通过 —— 严重缺陷");
    sig[0] = static_cast<uint8_t>(sig[0] ^ 0x01);

    // 6) 负例：改消息一个字节必须失败
    std::string bad_canon = canon;
    const size_t p = bad_canon.find("V1.0.55");
    if (p != std::string::npos) bad_canon[p + 1] = '9';
    EXPECT(!verify_ed25519(sig.data(), bad_canon, pk), "篡改消息后仍验签通过 —— 严重缺陷");
}
