// test_license_shortcode.cpp — M2.05：卡号短码/校验位 + M2.03 门控/能力投影（单元测试）
//
// 覆盖（规格 §4.1 用例表）：
//   · shortcode_cross_language_vector          跨语言向量（与 Python 测试**同一字面串**）
//   · shortcode_single_char_substitution_rejected  单字符替换**全枚举**（8 位 × 31 其它字符）
//   · shortcode_adjacent_transposition          相邻换位（显式登记 {0,31} 盲区，不假装全覆盖）
//   · shortcode_normalization                   大小写/分隔符/前缀/ Crockford 归一 变体
//   · shortcode_not_security_boundary           verify 只查校验位、**不查签名**（准入仍归 OfflineCardClient）
//   · gate_pipeline_allowed_requires_capture    pipeline_allowed() == feature_enabled("capture")
//   · features_capabilities_projection          capabilities 由 features 派生（四项布尔）
//
// 框架：ttbox_test（TEST/CHECK，禁裸 assert）。本 TU 并入 ttbox_core_tests（test_main.cpp 提供 main）。
// 依赖：全部为纯逻辑（无 OpenSSL / 无网络 / 无文件 I-O），host 与板端同跑一份断言。
#include "test_util.hpp"

#include "auth/LicenseGate.hpp"
#include "auth/LicenseKeys.hpp"
#include "auth/LicenseShortCode.hpp"
#include "auth/LicenseStateMachine.hpp"
#include "auth/LicenseStore.hpp"
#include "auth/OfflineCardClient.hpp"
#include "common/Metrics.hpp"

#include <cstdint>
#include <string>
#include <vector>

using ttbox::core::auth::feature_name::kAim;
using ttbox::core::auth::feature_name::kCapture;
using ttbox::core::auth::feature_name::kInference;
using ttbox::core::auth::feature_name::kOta;
using ttbox::core::auth::LicenseSnapshot;
using ttbox::core::auth::LicenseState;
using ttbox::core::auth::LicenseStatus;
using ttbox::core::auth::StoreLoadResult;
using ttbox::core::auth::to_snapshot;

namespace {

// ★ 跨语言向量（唯一权威字面串）：
//   license_id = "ttbox-lic-20260917-3842ff"
//   ⇒ FNV-1a-32 = 7307651 (0x006F83C3) ⇒ body7 = "006Z0C3" ⇒ check = '3'
//   ⇒ short_code = "TTB-006Z-0C33"
//   同一个字面串在 tools/license/test_license_gen.py 中**各钉一份**：
//   任一侧单方改动 ⇒ 两侧测试不同步 ⇒ 红灯逼对账（特性，非缺陷）。
constexpr const char* kVectorLicenseId = "ttbox-lic-20260917-3842ff";
constexpr const char* kVectorShortCode = "TTB-006Z-0C33";

char alphabet_at(int i) {
    static const std::string a(ttbox::core::auth::kShortCodeAlphabet);
    return a[static_cast<std::size_t>(i)];
}

int alphabet_index(char c) {
    const std::string a(ttbox::core::auth::kShortCodeAlphabet);
    for (int i = 0; i < ttbox::core::auth::kShortCodeRadix; ++i) {
        if (a[static_cast<std::size_t>(i)] == c) return i;
    }
    return -1;
}

// 把 8 位 body 装回人读格式 "TTB-XXXX-XXXX"（与 derive 同形）。
std::string format_code(const std::string& body8) {
    return std::string("TTB-") + body8.substr(0, 4) + "-" + body8.substr(4, 4);
}

// 取短码的 8 位 body（去前缀/分隔符）。
std::string body_of(const std::string& code) {
    std::string out;
    for (char c : code) {
        if (c == '-') continue;
        out.push_back(c);
    }
    if (out.size() >= 3 && out.compare(0, 3, "TTB") == 0) out.erase(0, 3);
    return out;
}

LicenseStatus make_status(LicenseState st) {
    LicenseStatus s;
    s.state = st;
    s.verified_at_ms = 1'000;
    s.expire_unix_ms = 10'000'000;
    return s;
}

}  // namespace

// ---- ① 跨语言向量：C++ 与 Python 必须产同一字面串 ----
TEST(shortcode_cross_language_vector) {
    const std::string got = ttbox::core::auth::license_short_code_derive(kVectorLicenseId);
    CHECK(got == std::string(kVectorShortCode));
    // 中间量也锁死（便于两侧定位分歧发生在哪一步）。
    CHECK_EQ(ttbox::core::auth::short_code_fnv1a32(kVectorLicenseId), 7307651u);
    CHECK(ttbox::core::auth::short_code_body7(7307651u) == std::string("006Z0C3"));
    CHECK(ttbox::core::auth::short_code_luhn_check(std::string("006Z0C3")) == '3');
    // 形状：TTB + 8 位 body（含两段分隔符）+ 校验位 ⇒ 13 字符（见头文件"长度口径"说明）。
    CHECK(got.size() == 13);
    CHECK(got.compare(0, 4, "TTB-") == 0);
    CHECK(got[8] == '-');
    CHECK(ttbox::core::auth::license_short_code_verify(got));
}

// ---- ② 单字符替换：8 位 body 任一位置换成任一其它 alphabet 字符 ⇒ 全拒 ----
TEST(shortcode_single_char_substitution_rejected) {
    const std::string body = body_of(kVectorShortCode);
    CHECK(body.size() == 8);
    int checked = 0;
    for (int pos = 0; pos < 8; ++pos) {
        for (int ci = 0; ci < ttbox::core::auth::kShortCodeRadix; ++ci) {
            const char c = alphabet_at(ci);
            if (c == body[static_cast<std::size_t>(pos)]) continue;  // 未改变 ⇒ 非"替换"
            std::string cand = body;
            cand[static_cast<std::size_t>(pos)] = c;
            const bool ok = ttbox::core::auth::license_short_code_verify(format_code(cand));
            if (ok) {
                CHECK(false);  // 任一漏检即失败（含位次/字符，便于定位）
            }
            ++checked;
        }
    }
    CHECK(checked == 8 * (ttbox::core::auth::kShortCodeRadix - 1));  // 248（实测 0 漏检）
}

// ---- ③ 相邻换位：除登记盲区外必须检测 ----
//   盲区 = 两字符**相同**（交换后串不变，非错误）或 alphabet 下标差 == 31（{0,31}，Luhn mod N
//   的已知边界）。这两类**显式登记**，不假装全覆盖（规格 §2.4 同理）。
TEST(shortcode_adjacent_transposition) {
    const std::string body = body_of(kVectorShortCode);
    int detected = 0;
    int registered_blind = 0;
    for (int i = 0; i + 1 < 8; ++i) {
        const char a = body[static_cast<std::size_t>(i)];
        const char b = body[static_cast<std::size_t>(i + 1)];
        std::string cand = body;
        cand[static_cast<std::size_t>(i)] = b;
        cand[static_cast<std::size_t>(i + 1)] = a;
        const bool ok = ttbox::core::auth::license_short_code_verify(format_code(cand));
        if (a == b) {
            CHECK(ok);  // 交换后与原文相同 ⇒ 仍合法（此非"检出的错误"）
            ++registered_blind;
            continue;
        }
        const int ia = alphabet_index(a);
        const int ib = alphabet_index(b);
        const bool luhn_blind = (ia >= 0 && ib >= 0) &&
                                ((ia > ib ? ia - ib : ib - ia) == ttbox::core::auth::kShortCodeRadix - 1);
        if (luhn_blind) {
            CHECK(ok);  // ★ 登记：{0,31}（'0'↔'Z'）换位为 Luhn mod 32 的已知漏检边界
            ++registered_blind;
            continue;
        }
        CHECK(!ok);  // 其余相邻换位必须被拒
        ++detected;
    }
    CHECK(detected >= 1);                 // 本向量确有被检出的换位（非全盲）
    CHECK(detected + registered_blind == 7);
}

// ---- ④ 归一：大小写/分隔符/前缀/Crockford 归一 变体都通过 ----
TEST(shortcode_normalization) {
    const std::string code(kVectorShortCode);
    CHECK(ttbox::core::auth::license_short_code_verify(code));                       // 规范形
    CHECK(ttbox::core::auth::license_short_code_verify("ttb-006z-0c33"));            // 全小写 + 小写前缀
    CHECK(ttbox::core::auth::license_short_code_verify("TTB006Z0C33"));              // 无分隔符（长度 11）
    CHECK(ttbox::core::auth::license_short_code_verify("006Z0C33"));                 // 无前缀
    CHECK(ttbox::core::auth::license_short_code_verify("OO6Z-OC33"));                // O→0 归一（两处）
    CHECK(ttbox::core::auth::license_short_code_verify("  TTB-006Z-0C33  "));        // 前后空白

    // 校验位错 ⇒ 拒；空/过短/闭集外 ⇒ 拒。
    CHECK(!ttbox::core::auth::license_short_code_verify("TTB-006Z-0C34"));
    CHECK(!ttbox::core::auth::license_short_code_verify(std::string()));
    CHECK(!ttbox::core::auth::license_short_code_verify("TTBX"));
    CHECK(!ttbox::core::auth::license_short_code_verify("TTB-006Z-0CU3"));  // 'U' 闭集外（Crockford 排除）
}

// ---- ⑤ 非安全边界：verify 只查校验位，不查签名（准入仍归 OfflineCardClient）----
//   构造一张"短码对、签名坏"的卡：短码校验通过，而 OfflineCardClient 必拒。
TEST(shortcode_not_security_boundary) {
    // (a) 短码对：由卡内 license_id 派生并自校验通过。
    CHECK(ttbox::core::auth::license_short_code_verify(
        ttbox::core::auth::license_short_code_derive(kVectorLicenseId)));

    // (b) 同一 license_id 的卡，签名是 64 个 0 字节（base64）⇒ 验签必败。
    //     卡其余字段合法（device 与本板一致）⇒ 只有签名坏这一处差异。
    const std::string sig64 = std::string(84, 'A') + "AA==";  // 64B 零 ⇒ 88 字符
    const std::string envelope =
        std::string("{\"license\":{") +
        "\"license_id\":\"" + kVectorLicenseId + "\"," +
        "\"device\":\"TESTSERIAL0001\"," +
        "\"plan\":\"subscription\"," +
        "\"is_pro\":true," +
        "\"features\":[\"capture\"]," +
        "\"ui_brand\":\"ttbox\"," +
        "\"issued_at\":1750000000," +
        "\"expires_at\":0}," +
        "\"key_id\":\"" + std::string(ttbox::core::auth::kLicenseKeyId) + "\"," +
        "\"signature\":\"" + sig64 + "\"}";

    ttbox::core::auth::OfflineCardClient client;
    LicenseStatus out;
    std::string err;
    (void)client.verify_once(envelope, "TESTSERIAL0001", out, &err);
    // 准入被拒（签名不符）——这正是安全边界所在。
    CHECK(out.state == LicenseState::kInvalidCard);
    CHECK(out.last_error.find("signature mismatch") != std::string::npos);
    // 而短码仍通过校验：证明短码**不是**准入判据（非安全边界，规格 §0.6）。
    CHECK(ttbox::core::auth::license_short_code_verify(kVectorShortCode));
}

// ---- ⑥ 派生入参合法性（fail-closed：非法 id 不臆造短码）----
TEST(shortcode_derive_rejects_invalid_id) {
    CHECK(ttbox::core::auth::license_short_code_derive("").empty());
    CHECK(ttbox::core::auth::license_short_code_derive("bad id").empty());       // 空格
    CHECK(ttbox::core::auth::license_short_code_derive("../etc/passwd").empty()); // 路径分隔
    CHECK(ttbox::core::auth::license_short_code_derive(std::string(65, 'a')).empty()); // >64
    CHECK(!ttbox::core::auth::license_short_code_derive("ok_id-1").empty());
}

// ---- ⑦ M2.03 会话级 gate：pipeline_allowed() == feature_enabled("capture") ----
TEST(gate_pipeline_allowed_requires_capture) {
    LicenseStatus s = make_status(LicenseState::kValid);

    s.features = {"inference", "aim"};
    LicenseSnapshot only_ai = to_snapshot(s, StoreLoadResult{}, 2000);
    CHECK(!only_ai.pipeline_allowed());              // 无 capture ⇒ 整链不起（无帧源）
    CHECK(only_ai.feature_enabled(kInference));      // 但特性位仍在（能力投影用）

    s.features = {"capture"};
    CHECK(to_snapshot(s, StoreLoadResult{}, 2000).pipeline_allowed());

    s.features = {"capture", "inference", "aim", "ota"};
    CHECK(to_snapshot(s, StoreLoadResult{}, 2000).pipeline_allowed());

    // 不可信态：features 清空 ⇒ pipeline_allowed() 恒 false（隐含 state 门）。
    LicenseStatus unknown = make_status(LicenseState::kUnknown);
    unknown.features = {"capture"};
    CHECK(!to_snapshot(unknown, StoreLoadResult{}, 2000).pipeline_allowed());
}

// ---- ⑧ M2.03 capabilities 投影：由 features 派生（四项布尔，core 唯一裁决）----
TEST(features_capabilities_projection) {
    LicenseStatus s = make_status(LicenseState::kValid);
    s.features = {"capture", "inference"};
    const LicenseSnapshot snap = to_snapshot(s, StoreLoadResult{}, 2000);

    CHECK(snap.capability(kCapture));
    CHECK(snap.capability(kInference));
    CHECK(!snap.capability(kAim));
    CHECK(!snap.capability(kOta));

    // 结构体默认值契约：Capabilities 构造即全 false（缺省=无能力，与"诚实未激活"一致）。
    const ttbox::core::LicenseStatusBlock block;
    CHECK(!block.capabilities.capture);
    CHECK(!block.capabilities.inference);
    CHECK(!block.capabilities.aim);
    CHECK(!block.capabilities.ota);
    CHECK(block.short_code.empty());   // M2.05：默认无短码

    // 按 Application 的映射口径（capability(feature_name::kX)）复现一版，逐项相等。
    ttbox::core::LicenseStatusBlock mapped;
    mapped.capabilities.capture = snap.capability(kCapture);
    mapped.capabilities.inference = snap.capability(kInference);
    mapped.capabilities.aim = snap.capability(kAim);
    mapped.capabilities.ota = snap.capability(kOta);
    CHECK(mapped.capabilities.capture && mapped.capabilities.inference);
    CHECK(!mapped.capabilities.aim && !mapped.capabilities.ota);
}
