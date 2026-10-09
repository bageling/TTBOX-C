// test_startup_intent.cpp — core 启动意图裁决（R5/R6）纯函数测试。
//
// 覆盖现场关心的问题：「更新后到底跑不跑 AI 流水线」。
// 判错的两个方向都是事故：更新完自己开跑（业主报的 bug）、或该跑的不跑。
#include <string>

#include "app/RuntimeIntent.hpp"
#include "common/Json.hpp"
#include "test_util.hpp"

using ttbox::core::decide_startup_intent;
using ttbox::core::JsonValue;
using ttbox::core::json_parse;
using ttbox::core::ota_terminal_state;

namespace {

JsonValue parse(const std::string& text) {
    const auto r = json_parse(text);
    return r.ok ? r.value : JsonValue::null();
}

constexpr const char* kCur = "1.5.16";

}  // namespace

// 首次开机（无标记、无更新器记录）⇒ 不判定为更新，保持自动启动。
TEST(first_boot_without_any_record_keeps_running) {
    const auto d = decide_startup_intent("", kCur, nullptr);
    CHECK(!d.just_updated);
    CHECK(d.want_running);
}

// 标记不存在但更新器记着"本版本 SUCCESS" ⇒ 升到本特性首个版本的那一次，判为刚更新。
TEST(first_boot_after_ota_success_same_version_is_update) {
    const JsonValue st = parse(R"({"state":"SUCCESS","progress":100,"version":"1.5.16"})");
    const auto d = decide_startup_intent("", kCur, &st);
    CHECK(d.just_updated);
    CHECK(!d.want_running);
}

// 更新器记录的是别的版本 ⇒ 不是"刚更新到本版本"，不拦。
TEST(first_boot_ota_success_other_version_not_update) {
    const JsonValue st = parse(R"({"state":"SUCCESS","version":"1.5.15"})");
    const auto d = decide_startup_intent("", kCur, &st);
    CHECK(!d.just_updated);
    CHECK(d.want_running);
}

// 更新器还没跑完 / 跑失败 ⇒ 不算更新完成，保持自动启动。
TEST(first_boot_ota_running_or_failed_not_update) {
    for (const char* body : {R"({"state":"RUNNING","progress":40,"version":"1.5.16"})",
                             R"({"state":"FAILED","error":"delta_base_mismatch"})"}) {
        const JsonValue st = parse(body);
        const auto d = decide_startup_intent("", kCur, &st);
        CHECK(!d.just_updated);
        CHECK(d.want_running);
    }
}

// 状态大小写不敏感（更新器/别处写入风格不一致时不该误判）。
TEST(ota_state_is_case_insensitive) {
    const JsonValue st = parse(R"({"state":"success","version":"1.5.16"})");
    const auto d = decide_startup_intent("", kCur, &st);
    CHECK(d.just_updated);
    CHECK(!d.want_running);
}

// 字段缺失 / 非对象 JSON ⇒ 一律不判定为更新（fail-open 到"自动启动"）。
TEST(malformed_ota_status_never_blocks) {
    for (const char* body : {"{}", R"({"state":"SUCCESS"})", R"({"version":"1.5.16"})", "[1,2,3]"}) {
        const JsonValue st = parse(body);
        const auto d = decide_startup_intent("", kCur, &st);
        CHECK(!d.just_updated);
        CHECK(d.want_running);
    }
}

// 标记与当前版本一致 ⇒ 普通重启，交给 R5 按用户意愿处理。
TEST(same_boot_version_is_plain_restart) {
    const JsonValue st = parse(R"({"state":"SUCCESS","version":"1.5.16"})");
    const auto d = decide_startup_intent(kCur, kCur, &st);
    CHECK(!d.just_updated);
    CHECK(d.want_running);
}

// ★ 核心场景：标记是上一版 ⇒ 刚做完 OTA，更新后必须停在停止态。
TEST(boot_version_changed_means_update_and_stops) {
    const auto d = decide_startup_intent("1.5.15", kCur, nullptr);
    CHECK(d.just_updated);
    CHECK(!d.want_running);
    CHECK(!d.reason.empty());  // 要有可读的日志理由
}

// 跨多个版本（1.5.10 → 1.5.16）同样判为更新。
TEST(multi_version_jump_also_detected) {
    const auto d = decide_startup_intent("1.5.10", kCur, nullptr);
    CHECK(d.just_updated);
    CHECK(!d.want_running);
}

// 降级/回滚（标记版本高于当前）也算换过版本 ⇒ 同样停在停止态。
TEST(downgrade_also_detected) {
    const auto d = decide_startup_intent("1.5.17", kCur, nullptr);
    CHECK(d.just_updated);
    CHECK(!d.want_running);
}

// 标记存在时忽略 ota_status（避免"历史上某次 SUCCESS"盖过真实结论）。
TEST(marker_takes_precedence_over_ota_status) {
    const JsonValue st = parse(R"({"state":"SUCCESS","version":"1.5.16"})");
    const auto d = decide_startup_intent("1.5.16", kCur, &st);
    CHECK(!d.just_updated);
    CHECK(d.want_running);
}

// ---- 更新冒烟自检：更新器终态判定（方案B，2026-09-20）----
// 命中"刚更新过"后流水线先跑起来，等这里判定出终态才收尾停回停止态。
// 判错的后果：提前收尾 ⇒ 健康门禁来不及通过 ⇒ 更新回滚。

// 本版本 SUCCESS/FAILED ⇒ 终态成立（自检可以收尾）。
TEST(ota_terminal_state_success_and_failed) {
    const JsonValue ok = parse(R"({"state":"SUCCESS","progress":100,"version":"1.5.19"})");
    CHECK(ota_terminal_state(ok, "1.5.19") == "SUCCESS");
    const JsonValue bad = parse(R"({"state":"FAILED","error":"health_check_failed","version":"1.5.19"})");
    CHECK(ota_terminal_state(bad, "1.5.19") == "FAILED");
}

// 运行中（更新还没判完）⇒ 不是终态，自检必须继续跑。
TEST(ota_terminal_state_running_is_not_terminal) {
    const JsonValue st = parse(R"({"state":"RUNNING","progress":85,"version":"1.5.19"})");
    CHECK(ota_terminal_state(st, "1.5.19").empty());
}

// ★ 版本不符（上一次更新残留的 SUCCESS）⇒ 不算终态，否则会提前收尾害本次更新回滚。
TEST(ota_terminal_state_stale_other_version_rejected) {
    const JsonValue st = parse(R"({"state":"SUCCESS","progress":100,"version":"1.5.16"})");
    CHECK(ota_terminal_state(st, "1.5.19").empty());
}

// 大小写不敏感 + 缺字段/非对象/空期望版本一律不算终态（fail-open 到"继续跑"）。
TEST(ota_terminal_state_edge_cases) {
    const JsonValue lower = parse(R"({"state":"success","version":"1.5.19"})");
    CHECK(ota_terminal_state(lower, "1.5.19") == "SUCCESS");
    for (const char* body : {"{}", R"({"state":"SUCCESS"})", R"({"version":"1.5.19"})", "[1,2,3]"}) {
        CHECK(ota_terminal_state(parse(body), "1.5.19").empty());
    }
    const JsonValue ok = parse(R"({"state":"SUCCESS","version":"1.5.19"})");
    CHECK(ota_terminal_state(ok, "").empty());
}

int main() {
    std::printf("=== test_startup_intent (R5/R6) ===\n");
    const int failed = ::ttbox_test::run_all();
    return failed == 0 ? 0 : 1;
}
