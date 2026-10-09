// test_util.hpp — 极简测试框架（零第三方依赖，C++17）
//
// ★ P0-2（"绿=真绿"）：SKIP 必须**单独计数**、在摘要行打印，且**永不**计入 passed。
//   背景：先前某用例若因缺 fixture 提前 `return`（return 前零断言），`run_all()` 记它 PASS
//   —— 这是"零断言静默跳过"，与已定口径 E3 同款反模式（静默跳过 = 病）。
//   本框架按 fixture 是否"必需"分两类，**两者都绝不并入 passed、都计入 skipped**：
//     - TEST_SKIP_REQUIRED(...)：**被声明为必需**的 fixture 缺失 ⇒ 计入 skipped **且**进程非零退出；
//     - TEST_SKIP(...)          ：可选 fixture / 环境条件不满足 ⇒ 计入 skipped（WARN），不失败。
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace ttbox_test {

// 断言失败时的值显示（2026-10-07 新增）。
//   动机：CHECK_EQ/CHECK_NE 原来直接 std::to_string(v)，这对**裸枚举**成立（隐式转 int），
//   但对 `enum class` 就不成立了（没有隐式转换 ⇒ 重载决议失败 ⇒ 整个测试目标编译不过）。
//   这里用 if constexpr 统一处理：枚举一律按底层整型打印，标量走 std::to_string。
//   效果是测试框架从此对两种枚举都免疫，不必为每次 enum class 化去改宏。
template <typename T>
inline std::string to_display(const T& v) {
    if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else {
        return std::to_string(v);
    }
}

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int& failure_counter() {
    static int failures = 0;
    return failures;
}

inline int& skip_counter() {
    static int skips = 0;
    return skips;
}

inline int& required_skip_counter() {
    static int required_skips = 0;
    return required_skips;
}

inline void report_failure(const char* file, int line, const std::string& expr) {
    ++failure_counter();
    std::printf("  [FAIL] %s:%d  %s\n", file, line, expr.c_str());
}

// 记录一次 SKIP。required=true 表示"被声明为必需的 fixture 缺失"，须令进程非零退出。
inline void report_skip(const char* file, int line, const std::string& why, bool required) {
    ++skip_counter();
    if (required) ++required_skip_counter();
    std::printf("  [SKIP%s] %s:%d  %s\n",
                required ? " ·REQUIRED" : "", file, line, why.c_str());
}

inline int failure_count() { return failure_counter(); }
inline int skip_count() { return skip_counter(); }
inline int required_skip_count() { return required_skip_counter(); }

inline int run_all() {
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const auto& tc : registry()) {
        const int fail_before = failure_count();
        const int skip_before = skip_count();
        std::printf("== RUN: %s\n", tc.name);
        std::fflush(stdout);
        tc.fn();
        const int fail_after = failure_count();
        const int skip_after = skip_count();
        if (fail_after > fail_before) {
            ++failed;
            std::printf("== FAIL: %s (%d assertion(s) failed)\n", tc.name,
                        fail_after - fail_before);
        } else if (skip_after > skip_before) {
            ++skipped;   // ★ 跳过 ≠ 通过：绝不并入 passed
            std::printf("== SKIP: %s\n", tc.name);
        } else {
            ++passed;
            std::printf("== OK: %s\n", tc.name);
        }
    }
    // ★ 摘要行：passed / skipped / failed 三态分离（skipped 独立计数，永不冒充 passed）。
    std::printf(
        "\n=== ttbox_test summary: passed=%d skipped=%d failed=%d "
        "(total assertion failures=%d) ===\n",
        passed, skipped, failed, failure_count());
    if (required_skip_count() > 0) {
        std::printf("*** %d 个【必需】fixture 缺失 ⇒ 退出码非零（本结果不可视为通过）***\n",
                    required_skip_count());
    }
    // 非零退出：有断言失败 **或** 有必需 fixture 缺失。
    return failed + required_skip_count();
}

}  // namespace ttbox_test

// 断言宏（计数失败，不中断）
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            ::ttbox_test::report_failure(__FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        auto va = (a); \
        auto vb = (b); \
        if (!(va == vb)) { \
            ::ttbox_test::report_failure(__FILE__, __LINE__, \
                std::string(#a " == " #b " (") + ::ttbox_test::to_display(va) + " vs " + ::ttbox_test::to_display(vb) + ")"); \
        } \
    } while (0)

#define CHECK_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            ::ttbox_test::report_failure(__FILE__, __LINE__, #a " != " #b); \
        } \
    } while (0)

#define TEST(name) \
    static void test_##name(); \
    static ::ttbox_test::Registrar reg_##name(#name, &test_##name); \
    static void test_##name()

// ---- SKIP 宏（P0-2）：两者都计入 skipped、都**不**计入 passed ----
// 必需 fixture 缺失：计入 skipped **且**令进程非零退出。
#define TEST_SKIP_REQUIRED(why) \
    do { \
        ::ttbox_test::report_skip(__FILE__, __LINE__, (why), true); \
        return; \
    } while (0)

// 可选 fixture / 环境条件不满足：计入 skipped（WARN 语义），不失败。
#define TEST_SKIP(why) \
    do { \
        ::ttbox_test::report_skip(__FILE__, __LINE__, (why), false); \
        return; \
    } while (0)
