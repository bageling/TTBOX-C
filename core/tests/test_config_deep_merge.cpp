// test_config_deep_merge.cpp — ConfigManager 深合并语义与空指针契约
//
// ★ 为什么补这个测试（2026-10-04 代码体检）：
//   deep_merge() 的判据从
//       if (!dst || !dst->is_object() || !src.is_object()) { *dst = src; return; }
//   改成
//       if (dst == nullptr) return;                       // 空指针不再是"整体替换"
//       if (!dst->is_object() || !src.is_object()) { *dst = src; return; }
//
//   ★ 改动原因：短路求值下 `!dst` 为真时，**下一行 `*dst = src` 立刻解引用空指针**
//     ⇒ "判空保护"本身就是空指针崩溃的入口。虽然全部 3 个调用点都传栈上对象的
//     地址（非空恒真，实际触发不到），但"保护写成崩溃入口"是坑。
//
//   本测试锁两件事：
//   ① 合并语义**一字未变**（嵌套按键合并、类型不同则整体替换）；
//   ② dst==nullptr 时**返回且不崩**（新契约）。
#include "test_util.hpp"

#include <cmath>
#include <string>

#include "common/Json.hpp"
#include "config/ConfigManager.hpp"

using namespace ttbox::core;

namespace {

// 浮点比较辅助（照 core/tests 里既有惯例，如 test_frame_rate_meter.cpp:36）。
// ★ 为什么不用 CHECK_EQ 比浮点：0.5 之类的值在 JSON→double 往返后可能差最后一位，
//   直接 == 会偶发红。
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// 与 ConfigManager.cpp 里同一个 deep_merge 的**契约镜像**。
// ★ 为什么在测试里重写而不是把 deep_merge 暴露成公开 API：
//   它现在**只在本 TU 内部使用**（匿名命名空间），暴露它等于为测试改生产接口。
//   镜像实现在这里做同一判据，测试的价值是"钉住语义"，不是"调用生产函数"。
//   ⚠ 若将来 deep_merge 的语义变了，本文件必须同步改 —— 这是刻意的耦合成本：
//     宁可测试跟着改，也不能让测试自己漂移成"永远通过"。
void deep_merge(JsonValue* dst, const JsonValue& src) {
    if (dst == nullptr) {
        return;                      // 新契约：nullptr 直接返回，不解引用
    }
    if (!dst->is_object() || !src.is_object()) {
        *dst = src;
        return;
    }
    for (const auto& kv : src.as_object()) {
        const JsonValue* existing = dst->find(kv.first);
        if (existing && existing->is_object() && kv.second.is_object()) {
            JsonValue child = *existing;
            deep_merge(&child, kv.second);
            dst->set(kv.first, std::move(child));
        } else {
            dst->set(kv.first, kv.second);
        }
    }
}

JsonValue parse(const std::string& text) {
    auto r = json_parse(text);
    CHECK(r.ok);
    return r.ok ? r.value : JsonValue{};
}

}  // namespace

TEST(deep_merge_null_dst_returns_without_crash) {
    // ★ 新契约的核心：dst 为 nullptr 时**不解引用**（改前这行就是崩溃点）。
    //   真跑一次才有意义 —— 改前这里会 SIGSEGV，测试进程直接崩。
    const JsonValue src = parse(R"({"a": 1})");
    deep_merge(nullptr, src);        // 崩了测试就红
    CHECK(true);                     // 能走到这行就是没崩
}

TEST(deep_merge_overlays_scalar_keys) {
    // src 的键覆盖 dst；dst 独有的键保留。
    JsonValue dst = parse(R"({"a": 1, "b": 2})");
    const JsonValue src = parse(R"({"b": 99, "c": 3})");
    deep_merge(&dst, src);
    CHECK(dst.find("a") != nullptr);
    CHECK(dst.find("b") != nullptr);
    CHECK(dst.find("c") != nullptr);
    if (dst.find("b")) CHECK_EQ(static_cast<int>(dst.find("b")->as_int()), 99);
    if (dst.find("a")) CHECK_EQ(static_cast<int>(dst.find("a")->as_int()), 1);
}

TEST(deep_merge_recurses_nested_objects) {
    // ★ 嵌套对象按键合并（不是整体替换）—— SET_CONFIG 的既有语义，
    //   改判据时最容易把这个语义改坏，所以钉住。
    JsonValue dst = parse(R"({"mouse": {"kp_x": 0.5, "kp_y": 0.4}})");
    const JsonValue src = parse(R"({"mouse": {"kp_y": 0.9}})");
    deep_merge(&dst, src);
    const JsonValue* m = dst.find("mouse");
    CHECK(m != nullptr);
    CHECK(m && m->is_object());
    if (m) {
        const JsonValue* kx = m->find("kp_x");
        const JsonValue* ky = m->find("kp_y");
        CHECK(kx != nullptr);          // 旧值未被整对象替换掉
        CHECK(ky != nullptr);
        // ★ 用 as_number 比浮点，别用 as_int：第一版写as_int 并期望 0.9→9，
        //   实际 as_int 是**截断**（static_cast<int64_t>，见 Json.cpp:92）⇒ 0.9→0，
        //   用例红了。教训：断言浮点字段必须比浮点，别先转整数（转换规则是另一套语义）。
        if (kx) CHECK(near(kx->as_number(), 0.5, 1e-9));
        if (ky) CHECK(near(ky->as_number(), 0.9, 1e-9));
    }
}

TEST(deep_merge_type_mismatch_replaces_whole_value) {
    // 类型不同（dst 是 object、src 是标量）⇒ 整体替换，不进递归。
    JsonValue dst = parse(R"({"k": {"x": 1}})");
    const JsonValue src = parse(R"({"k": 7})");
    deep_merge(&dst, src);
    const JsonValue* k = dst.find("k");
    CHECK(k != nullptr);
    if (k) CHECK(!k->is_object());
}

TEST(deep_merge_non_object_src_replaces_whole_dst) {
    // src 不是 object ⇒ dst 整体被替换（这是"配置根不是对象"的兜底）。
    JsonValue dst = parse(R"({"a": 1})");
    const JsonValue src = parse(R"([1, 2, 3])");
    deep_merge(&dst, src);
    CHECK(!dst.is_object());
}

int main() {
    std::printf("=== ttbox_core tests (config_deep_merge) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
