// test_license_card.cpp — M2.01/M2.02 单元测试：离线卡（解析/canonical/归一化 + 全链验签）
//
// 覆盖：
//   · canonical 串黄金值（与 Python 发卡工具逐字节锁：tools/license/ttbox_license_gen.py
//     canonical_license() —— 两侧任何单方改动 ⇒ 签名失效 ⇒ 本测试红，跨语言契约锁）；
//   · 解析：字段投影 / features 闭集归一化（乱序+闭集外名字 ⇒ 闭集序子集） /
//     ui_brand sanitize 回落 / fail-closed 表（坏 plan / 坏字符集 / 缺字段 / 坏 base64）；
//   · OfflineCardClient 全链（**生产钥签名**的固定向量，见文首生成说明）：
//     好卡⇒kValid+字段投影 · 篡改签名⇒kInvalidCard · 他板卡⇒kBoundElsewhere ·
//     过期卡⇒kExpired · 未知 key_id⇒kInvalidCard · req_ok 恒 true（离线无网络类失败）。
// 框架：ttbox_test（TEST/CHECK，禁裸 assert）。AUTH 无关（全部无 OpenSSL）。
//
// ★ 向量生成：tools/license/ttbox_license_gen.py issue-card，私钥 =
//   tools/license/.testkeys/ttbox-license-2026a.priv.pem（不入库），公钥内嵌
//   LicenseKeys.hpp。**换钥时须同步重生成本文件向量**（key_id/签名都会变——
//   这是有意的：换钥不重跑向量 ⇒ 测试红，逼着对账）。
#include "test_util.hpp"

#include "auth/LicenseCard.hpp"
#include "auth/LicenseDaemon.hpp"          // LicenseState / LicenseStatus
#include "auth/LicenseStateMachine.hpp"    // known_features
#include "auth/OfflineCardClient.hpp"

#include <string>
#include <vector>

using ttbox::core::auth::LicenseCard;
using ttbox::core::auth::LicenseState;
using ttbox::core::auth::OfflineCardClient;
using ttbox::core::auth::license_base64_decode;
using ttbox::core::auth::license_canonical;
using ttbox::core::auth::parse_license_card;

namespace {

// ---- 生产钥签名的固定向量（2026-09-17 生成，字段全固定）----

// 有效卡：device=TESTSERIAL0001 / plan=subscription / is_pro=true /
//         features={capture,aim} / ui_brand=acme / issued=1750000000 / 永久
const char* kValidCard =
    "{\"key_id\":\"ttbox-license-2026a\",\"license\":{\"device\":\"TESTSERIAL0001\","
    "\"expires_at\":0,\"features\":[\"capture\",\"aim\"],\"is_pro\":true,"
    "\"issued_at\":1750000000,\"license_id\":\"ttbox-lic-test-0001\","
    "\"plan\":\"subscription\",\"ui_brand\":\"acme\"},"
    "\"signature\":\"uyp6U/uWpOCYJQsEMVisB4UcYviFvqkIJf6/inQhHy1SrfmDMb496M3gDLeGTK/r9YamU4dg2vd9750XR4weBw==\"}";

// 过期卡：expires_at=1750000001（测试注入 now=1750000005）
const char* kExpiredCard =
    "{\"key_id\":\"ttbox-license-2026a\",\"license\":{\"device\":\"TESTSERIAL0001\","
    "\"expires_at\":1750000001,\"features\":[\"inference\"],\"is_pro\":false,"
    "\"issued_at\":1750000000,\"license_id\":\"ttbox-lic-test-0002\","
    "\"plan\":\"trial\",\"ui_brand\":\"ttbox\"},"
    "\"signature\":\"ZGVL7kL1yyXol0FuJLsqnt8j1mABIuN4T+Ash1zBZC1P63TdQpMzA//TQxsOyHG2zAi9LKReMgrgO03/ZrRWAQ==\"}";

// 他板卡：device=OTHERDEVICE009
const char* kOtherDeviceCard =
    "{\"key_id\":\"ttbox-license-2026a\",\"license\":{\"device\":\"OTHERDEVICE009\","
    "\"expires_at\":0,\"features\":[\"capture\",\"inference\",\"aim\",\"ota\"],"
    "\"is_pro\":true,\"issued_at\":1750000000,\"license_id\":\"ttbox-lic-test-0003\","
    "\"plan\":\"permanent\",\"ui_brand\":\"ttbox\"},"
    "\"signature\":\"BF6FREZ8eJcapdib3lBL8fma3PZs94Dxcg5FpGWsdt3NjVUecHhYZ157Hdr80XBrs3h3E0trbSXrFQawtR2aAw==\"}";

// 有效卡的 canonical 黄金串（与 Python canonical_license() 输出逐字节一致；
// '\\n' 在源码里写作 "\\n" 转义，实际是 LF）。
const char* kGoldenCanonical =
    "device\nTESTSERIAL0001\nexpires_at\n0\nfeatures\ncapture,aim\nis_pro\n1\n"
    "issued_at\n1750000000\nlicense_id\nttbox-lic-test-0001\nplan\nsubscription\n"
    "ui_brand\nacme\n";

}  // namespace

// ---- canonical 黄金值（跨语言契约锁）----
TEST(license_card_canonical_golden) {
    LicenseCard c;
    std::string err;
    CHECK(parse_license_card(kValidCard, &c, &err));
    CHECK(license_canonical(c) == kGoldenCanonical);
}

// ---- 解析：字段投影 + features 闭集归一化（乱序 + 闭集外名字）----
TEST(license_card_parse_normalizes_features) {
    // 取有效卡原文，把 features 数组改成乱序 + 加闭集外名字 "zzz"：
    // 签名按归一化后的闭集序计算 ⇒ 原签名仍然有效（闭集外名字不进 canonical）。
    const std::string raw = std::string(kValidCard)
        .replace(std::string(kValidCard).find("[\"capture\",\"aim\"]"), 17,
                 "[\"zzz\",\"aim\",\"capture\",\"aim\"]");
    LicenseCard c;
    std::string err;
    CHECK(parse_license_card(raw, &c, &err));
    // 归一化：闭集过滤 + 去重 + 闭集序
    CHECK(c.features.size() == 2);
    CHECK(c.features[0] == "capture");
    CHECK(c.features[1] == "aim");
    CHECK(c.device == "TESTSERIAL0001");
    CHECK(c.plan == "subscription");
    CHECK(c.is_pro);
    CHECK(c.ui_brand == "acme");
    CHECK(c.issued_at == 1750000000);
    CHECK(c.expires_at == 0);
    CHECK(c.key_id == "ttbox-license-2026a");
    // 加了闭集外名字 ⇒ canonical 不变 ⇒ 签名仍验过（用 OfflineCardClient 证明）
    OfflineCardClient cl;
    ttbox::core::auth::LicenseStatus st;
    CHECK(cl.verify_once(raw, "TESTSERIAL0001", st, nullptr));
    CHECK(st.state == LicenseState::kValid);
}

// ---- 解析：ui_brand sanitize 回落（非法品牌 ⇒ 默认 ttbox；签名按原文验 ⇒ 该卡
//      由 Python 侧签的是合法 acme，这里只测 sanitize 分支的纯解析行为）----
TEST(license_card_parse_sanitize_ui_brand) {
    // 把 ui_brand 改成非法值（不重签 ⇒ 签名必失效——但 parse 只管结构；
    // 这里断言 parse 不拒绝、且 sanitize 落回默认品牌）。
    std::string raw(kValidCard);
    const size_t pos = raw.find("\"acme\"");
    CHECK(pos != std::string::npos);
    raw.replace(pos, 6, "\"bad brand!\"");  // 含空格/叹号 ⇒ 非法
    LicenseCard c;
    std::string err;
    CHECK(parse_license_card(raw, &c, &err));
    CHECK(c.ui_brand == "ttbox");  // sanitize 回落默认
}

// ---- 解析 fail-closed 表：任一字段违例 ⇒ 整卡拒绝 ----
TEST(license_card_parse_fail_closed_table) {
    LicenseCard c;
    std::string err;
    // 坏 plan
    {
        std::string raw(kValidCard);
        raw.replace(raw.find("\"subscription\""), 14, "\"lifetime\"");
        CHECK(!parse_license_card(raw, &c, &err));
    }
    // 坏字符集（license_id 带空格）
    {
        std::string raw(kValidCard);
        raw.replace(raw.find("ttbox-lic-test-0001"), 19, "ttbox lic test 0001");
        CHECK(!parse_license_card(raw, &c, &err));
    }
    // 缺 signature
    {
        std::string raw(kValidCard);
        const size_t p = raw.find(",\"signature\":");
        CHECK(p != std::string::npos);
        raw.erase(p, std::string(kValidCard).size() - p - 1);
        CHECK(!parse_license_card(raw, &c, &err));
    }
    // 签名 base64 长度非 64B（"AAAA" = 3B）
    {
        std::string raw(kValidCard);
        const size_t p = raw.find("\"signature\":\"") + 13;
        const size_t q = raw.find('"', p);
        raw.replace(p, q - p, "AAAA");
        CHECK(!parse_license_card(raw, &c, &err));
    }
    // 非 JSON
    CHECK(!parse_license_card("not json at all", &c, &err));
    // 未知 key_id（结构合法、族不匹配——OfflineCardClient 层拒绝）
    {
        std::string raw(kValidCard);
        raw.replace(raw.find("ttbox-license-2026a"), 19, "ttbox-license-2030z");
        CHECK(parse_license_card(raw, &c, &err));  // parse 不管 key_id 族
    }
}

// ---- base64 解码 ----
TEST(license_card_base64_decode) {
    std::vector<uint8_t> out;
    CHECK(license_base64_decode("AAAA", &out) && out.size() == 3);
    CHECK(license_base64_decode("AA==", &out) && out.size() == 1);
    CHECK(license_base64_decode("AA", &out) == false);   // 非 4 倍长
    CHECK(license_base64_decode("A!==", &out) == false); // 非法字符
}

// ---- OfflineCardClient 全链：好卡 ⇒ kValid + 签名卡内容投影 ----
TEST(offline_client_valid_card_projects_fields) {
    OfflineCardClient cl;
    cl.set_now_unix_s(1750000005);  // 固定时间（issued 之后、永久卡）
    ttbox::core::auth::LicenseStatus st;
    std::string err;
    CHECK(cl.verify_once(kValidCard, "TESTSERIAL0001", st, &err));
    CHECK(st.state == LicenseState::kValid);
    CHECK(st.is_pro);
    CHECK(st.plan == "subscription");
    CHECK(st.ui_brand == "acme");
    CHECK(st.features.size() == 2);
    CHECK(st.features[0] == "capture" && st.features[1] == "aim");
    CHECK(st.expire_unix_ms == 0);  // 永久
    CHECK(st.bind_device == "TESTSERIAL0001");
    CHECK(st.card == "ttbox-li******");  // 前 8 位脱敏
    CHECK(err.empty());
}

// ---- 全链负控：篡改签名（换一个 base64 字符）⇒ kInvalidCard ----
TEST(offline_client_rejects_tampered_signature) {
    std::string raw(kValidCard);
    const size_t p = raw.find("\"signature\":\"") + 24;  // 签名串中部
    CHECK(p < raw.size());
    raw[p] = (raw[p] == 'A') ? 'B' : 'A';  // 翻转一个字符
    OfflineCardClient cl;
    ttbox::core::auth::LicenseStatus st;
    std::string err;
    CHECK(cl.verify_once(raw, "TESTSERIAL0001", st, &err));
    CHECK(st.state == LicenseState::kInvalidCard);
    CHECK(!err.empty());
}

// ---- 全链负控：篡改卡内容（device 改一个字符，签名失效）⇒ kInvalidCard ----
TEST(offline_client_rejects_tampered_payload) {
    std::string raw(kValidCard);
    const size_t p = raw.find("TESTSERIAL0001");
    CHECK(p != std::string::npos);
    raw.replace(p, 14, "TESTSERIAL0002");
    OfflineCardClient cl;
    ttbox::core::auth::LicenseStatus st;
    CHECK(cl.verify_once(raw, "TESTSERIAL0002", st, nullptr));
    CHECK(st.state == LicenseState::kInvalidCard);  // 内容改动 ⇒ 签名对不上
}

// ---- 全链：他板卡 ⇒ kBoundElsewhere（签名有效但绑定不符）----
TEST(offline_client_rejects_other_device_card) {
    OfflineCardClient cl;
    ttbox::core::auth::LicenseStatus st;
    std::string err;
    CHECK(cl.verify_once(kOtherDeviceCard, "TESTSERIAL0001", st, &err));
    CHECK(st.state == LicenseState::kBoundElsewhere);
}

// ---- 全链：过期卡 ⇒ kExpired（固定注入时间）----
TEST(offline_client_rejects_expired_card) {
    OfflineCardClient cl;
    cl.set_now_unix_s(1750000005);  // > expires_at(1750000001)
    ttbox::core::auth::LicenseStatus st;
    CHECK(cl.verify_once(kExpiredCard, "TESTSERIAL0001", st, nullptr));
    CHECK(st.state == LicenseState::kExpired);
}

// ---- 全链：过期边界的未过期侧（now < expires_at ⇒ kValid；防 off-by-one）----
TEST(offline_client_accepts_before_expiry) {
    OfflineCardClient cl;
    cl.set_now_unix_s(1750000000);  // == issued_at，< expires_at
    ttbox::core::auth::LicenseStatus st;
    CHECK(cl.verify_once(kExpiredCard, "TESTSERIAL0001", st, nullptr));
    CHECK(st.state == LicenseState::kValid);
    CHECK(st.expire_unix_ms == 1750000001000);
}

// ---- 全链：未知 key_id ⇒ kInvalidCard（换钥即旧卡全废的负控）----
TEST(offline_client_rejects_unknown_key_id) {
    std::string raw(kValidCard);
    raw.replace(raw.find("ttbox-license-2026a"), 19, "ttbox-license-2030z");
    // 前置：结构解析仍成功（key_id 族是 OfflineCardClient 层的拒绝点）
    LicenseCard parsed;
    std::string perr;
    CHECK(parse_license_card(raw, &parsed, &perr));
    CHECK(parsed.key_id == "ttbox-license-2030z");
    OfflineCardClient cl;
    ttbox::core::auth::LicenseStatus st;
    std::string err;
    CHECK(cl.verify_once(raw, "TESTSERIAL0001", st, &err));
    CHECK(st.state == LicenseState::kInvalidCard);
    CHECK(err.find("unknown key_id") != std::string::npos);  // 拒因明确
}

// ---- 全链：空卡 ⇒ kInvalidCard（防御；daemon 层先行拦截）----
TEST(offline_client_rejects_empty_card) {
    OfflineCardClient cl;
    ttbox::core::auth::LicenseStatus st;
    CHECK(cl.verify_once("", "TESTSERIAL0001", st, nullptr));
    CHECK(st.state == LicenseState::kInvalidCard);
}
