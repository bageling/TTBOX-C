// core/tests/test_ttbox_canonical.cpp — canonical 拼接逐字节单测（T1.07 / M2.07 尾换行修复）
// 无 OpenSSL 依赖 ⇒ 在默认构建（TTBOX_CORE_BUILD_AUTH=OFF）下可执行。
// 契约基准（云端 client_sign.go 的 clientSigningString = "%s\n%s\n%s\n%s\n%s"）：
//   签名原文 = METHOD\nPATH_WITH_QUERY\nTIMESTAMP\nNONCE\nBODY，**末尾无换行**。
// 实测：末尾多一个 "\n" ⇒ 云端 HMAC 校验失败 ⇒ 401；不带换行 ⇒ 200/业务码。
// 注：本工程 test_util.hpp 的 CHECK_EQ 走 std::to_string ⇒ 只可用于数值/字符；
//      字符串相等一律用 CHECK（比较表达式）或 CHECK_NE（不相等）。
#include "auth/TtboxCanonical.hpp"
#include "test_util.hpp"

using namespace ttbox::core::auth;

// ① GET 空 body ⇒ 末尾是 body 前那个 "\n"，不再追加第二个 "\n"
TEST(canonical_get_empty_body_no_trailing_blank_line) {
    const std::string c = build_canonical(
        "GET", "/api/client/app-info?app_key=ttbox", "1699999999", "abc123", "");
    CHECK(c == std::string(
        "GET\n/api/client/app-info?app_key=ttbox\n1699999999\nabc123\n"));
    CHECK_EQ(c.back(), '\n');                     // 末尾是 body 前的分隔符
    CHECK_NE(c.rfind("\n\n"), c.size() - 2);      // 结尾**不是** "\n\n"
    CHECK(c.find("\n\n") == std::string::npos);   // 全文无空行
}

// ② POST 带 body ⇒ 末尾 = body，其后**无换行**
TEST(canonical_post_body_ends_at_body_without_newline) {
    const std::string body =
        "{\"appKey\":\"ttbox\",\"cardKey\":\"K\",\"machineCode\":\"M\"}";
    const std::string c = build_canonical(
        "POST", "/api/client/card-login", "1699999999", "nonce", body);
    CHECK(c == "POST\n/api/client/card-login\n1699999999\nnonce\n" + body);
    CHECK(c.substr(c.size() - body.size()) == body);
    CHECK_NE(c.back(), '\n');                     // body 非空 ⇒ 末尾不是换行
}

// ③ 反证（防止回归）：**带尾换行 = 错** —— 旧实现（末尾多拼 "\n"）必须被判不等
TEST(canonical_regression_trailing_newline_variant_is_wrong) {
    const std::string with_trailing_newline =
        "POST\n/api/client/card-login\n1699999999\nnonce\n{\"a\":1}\n";  // 多一个 "\n"
    CHECK(with_trailing_newline != build_canonical(
        "POST", "/api/client/card-login", "1699999999", "nonce", "{\"a\":1}"));
    // 并反向锁定正确实现：期望串（无尾换行）与 build_canonical 相等
    CHECK(build_canonical("POST", "/api/client/card-login",
                          "1699999999", "nonce", "{\"a\":1}") ==
          std::string("POST\n/api/client/card-login\n1699999999\nnonce\n{\"a\":1}"));
    // 尾字符为 body 尾字符 '}'，而非换行
    CHECK_EQ(build_canonical("POST", "/api/client/card-login",
                             "1699999999", "nonce", "{\"a\":1}").back(), '}');
}
