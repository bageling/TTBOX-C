// test_shell_quote.cpp — shell_quote 单引号转义回归（H4）
//
// 背景：shell_quote 曾只做 "'"+s+"'"，不转义内嵌单引号 ⇒ 输入 "a';id;'" 会因引号
// 提前闭合而逃逸执行（调用方是 root 进程、把结果拼进 /bin/sh -c）。本测试钉死：
//   1. 逐值断言转义后的字面形态（含空串 / 纯 ' / 连续 ''）；
//   2. POSIX 单引号语义参考解析器做往返还原（不依赖外部 shell）；
//   3. 真实 /bin/sh：printf %s <quoted> 的输出必须等于原输入；
//   4. 反例：不转义实现对 "a';echo PWNED;'" 会真的执行 echo（注入），现版本不会。
#include <cstdio>
#include <string>

#include "common/Shell.hpp"
#include "test_util.hpp"

namespace {

// 经 /bin/sh 执行 `printf %s <quoted>` 并返回 stdout（popen 借用 sh 解析 quoted）。
// 无 shell / popen 不可用返回 false（供 TEST_SKIP 判定，而非误报失败）。
bool sh_printf_arg(const std::string& quoted, std::string* out) {
    const std::string cmd = "printf %s " + quoted;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (p == nullptr) return false;
    std::string s;
    char buf[256];
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), p)) > 0) s.append(buf, n);
    ::pclose(p);
    if (out != nullptr) *out = s;
    return true;
}

// 参考实现：按 POSIX 单引号规则解析一段 shell 输入（子集：单引号段 + 段外 \ 转义）。
// 用于在**不依赖外部 shell** 的情况下验证 quote→unquote 往返。
std::string sh_unquote_ref(const std::string& q) {
    std::string out;
    bool inq = false;
    for (size_t i = 0; i < q.size(); ++i) {
        const char c = q[i];
        if (inq) {
            if (c == '\'') {
                inq = false;
            } else {
                out.push_back(c);
            }
        } else if (c == '\'') {
            inq = true;
        } else if (c == '\\' && i + 1 < q.size()) {
            out.push_back(q[++i]);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// 宿主 popen 是否走 POSIX sh。
// 判据用手写的 POSIX 转义字面量 'a'\''b'（**不依赖被测实现**）：POSIX sh 解析为 a'b；
// mingw/ucrt64 宿主经 cmd.exe 时不是（PATH 里即使有 printf.exe 也不具备 POSIX 引号语义）。
// 真实 shell 往返/注入用例据此 SKIP（而非误报失败），在板端 / Linux host 上自动生效。
bool posix_sh_ready() {
    std::string out;
    if (!sh_printf_arg("'a'\\''b'", &out)) return false;
    return out == "a'b";
}

}  // namespace

// 无单引号输入：应被成对单引号包裹，内容不变。
TEST(shell_quote_no_quote) {
    CHECK(ttbox::core::shell_quote("abc") == std::string("'abc'"));
    CHECK(ttbox::core::shell_quote("/var/lib/ttbox/ota/jobs") ==
          std::string("'/var/lib/ttbox/ota/jobs'"));
}

// 空串 → ''（成对空引号，仍可安全作为单个空参数）。
TEST(shell_quote_empty_string) {
    CHECK(ttbox::core::shell_quote("") == std::string("''"));
}

// 纯单引号 → ''\'''（闭合-转义-重开）。
TEST(shell_quote_only_quote) {
    CHECK(ttbox::core::shell_quote("'") == std::string("''\\'''"));
}

// 内嵌单引号 → 'a'\''b'。
TEST(shell_quote_embedded_quote) {
    CHECK(ttbox::core::shell_quote("a'b") == std::string("'a'\\''b'"));
}

// 连续单引号：'' 与 a''b（锁住「逐字符扫描、不二次匹配」的实现细节）。
TEST(shell_quote_consecutive_quotes) {
    CHECK(ttbox::core::shell_quote("''") == std::string("''\\'''\\'''"));
    CHECK(ttbox::core::shell_quote("a''b") == std::string("'a'\\'''\\''b'"));
}

// 参考解析器往返：任意输入 → quote → unquote 必须原样还原（含注入串）。
TEST(shell_quote_ref_roundtrip) {
    const char* cases[] = {"",      "'",     "''",     "'''",
                           "a'b",   "a''b",  "'; rm -rf /'",
                           "/opt/ttbox/jobs/o'brien", "no-quote", "'start", "end'"};
    for (const char* s : cases) {
        const std::string quoted = ttbox::core::shell_quote(s);
        CHECK(sh_unquote_ref(quoted) == std::string(s));
    }
}

// 真实 /bin/sh 往返：printf %s <quoted> 的输出应等于原输入（无截断、无逃逸）。
// 仅在宿主 popen 走 POSIX sh 时执行（Windows mingw 经 cmd.exe ⇒ SKIP）。
TEST(shell_quote_sh_roundtrip) {
    if (!posix_sh_ready()) {
        TEST_SKIP("宿主 popen 非 POSIX sh（Windows mingw 走 cmd.exe）；真实往返在板端/Linux 生效");
    }
    const char* cases[] = {"",     "'",    "''",     "a'b",
                           "a''b", "'start", "end'", "no-quote"};
    for (const char* s : cases) {
        std::string got;
        CHECK(sh_printf_arg(ttbox::core::shell_quote(s), &got));
        CHECK(got == std::string(s));
    }
}

// 反例（回归锁）：不转义实现对 "a';echo PWNED;'" 会真的执行 echo（注入），现版本不会。
// 仅在宿主 popen 走 POSIX sh 时执行。
TEST(shell_quote_injection_counterexample) {
    if (!posix_sh_ready()) {
        TEST_SKIP("宿主 popen 非 POSIX sh（Windows mingw 走 cmd.exe）；注入反例在板端/Linux 生效");
    }

    const std::string evil = "a';echo PWNED;'";

    // 旧实现（不转义）：引号提前闭合 ⇒ 分号后的 echo PWNED 被执行。
    std::string naive_out;
    CHECK(sh_printf_arg("'" + evil + "'", &naive_out));
    CHECK(naive_out != evil);                                    // 往返失败 = 逃逸证据
    CHECK(naive_out.find("PWNED") != std::string::npos);         // 注入命令确实执行了

    // 现实现：整串被当作字面量，还原等于输入、无逃逸。
    std::string safe_out;
    CHECK(sh_printf_arg(ttbox::core::shell_quote(evil), &safe_out));
    CHECK(safe_out == evil);
}
