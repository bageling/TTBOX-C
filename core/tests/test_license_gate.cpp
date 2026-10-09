// test_license_gate.cpp — T1.10 单元测试（施工图：t1.09-t1.10-impl-spec.md §4.2 / §3.2 / §3.3）
//
// 覆盖：网络失败 fail-open / 权威否定 fail-closed / 防降级 fail-closed / 绑定不匹配 fail-closed /
//       未激活默认（A23 host 侧等价）/ wire 态映射（8 态逐行）/ 空 features ⇒ 一律 false。
// 框架：ttbox_test（TEST/CHECK，禁裸 assert）。本 TU 并入 ttbox_core_tests（test_main.cpp 提供 main）。
#include "test_util.hpp"

#include "auth/LicenseGate.hpp"
#include "auth/LicenseStateMachine.hpp"
#include "auth/LicenseStore.hpp"

#include <cstdint>
#include <string>

using ttbox::core::auth::LicenseGate;
using ttbox::core::auth::LicenseSnapshot;
using ttbox::core::auth::LicenseState;
using ttbox::core::auth::LicenseStatus;
using ttbox::core::auth::StoreLoadResult;
using ttbox::core::auth::to_snapshot;
using ttbox::core::auth::wire_activated;
using ttbox::core::auth::wire_state_name;

// 网络类失败 ⇒ fail-open：kValid 上态 + 本次 req_ok=false ⇒ 状态机落 kFallback ⇒ 仍允许 AI。
TEST(gate_network_failure_is_fail_open) {
    LicenseStatus status;
    status.state = LicenseState::kValid;
    status.verified_at_ms = 1000;
    status.expire_unix_ms = 10'000'000;  // 仍在有效期

    LicenseStatus out;                 // 客户端本次"原始结论"
    out.state = LicenseState::kValid;  // 即便客户端错报成功，req_ok=false 使其不可信
    std::string err;
    int64_t backoff = 0;
    ttbox::core::auth::apply_check_result(status, out, /*req_ok=*/false, /*now_ms=*/2000,
                                          /*heartbeat_ms=*/60'000, /*backoff_base=*/30'000,
                                          /*backoff_cap=*/1'800'000, backoff);

    CHECK(status.state == LicenseState::kFallback);  // 网络类 + 历史有效 ⇒ fail-open
    const LicenseSnapshot snap = to_snapshot(status, StoreLoadResult{}, /*now_ms=*/2000);
    CHECK(snap.ai_allowed());                        // 不因网络失败降 AI
    CHECK(wire_state_name(snap) == "active");
}

// 服务端权威否定（kExpired）⇒ fail-closed：无宽限则不允许。
TEST(gate_server_expired_is_fail_closed) {
    LicenseStatus status;
    status.state = LicenseState::kExpired;
    status.expire_unix_ms = 1;  // 已过期
    const LicenseSnapshot snap = to_snapshot(status, StoreLoadResult{}, /*now_ms=*/2000);
    CHECK(!snap.ai_allowed());
    CHECK(wire_state_name(snap) == "restricted");
    CHECK(!wire_activated(snap));
}

// kExpired 但仍在宽限窗口内 ⇒ 允许（grace 是 fail-closed 的唯一例外）。
TEST(gate_expired_in_grace_allows) {
    LicenseStatus status;
    status.state = LicenseState::kExpired;
    StoreLoadResult lr;
    lr.grace_until = 5000;  // now(2000) < grace(5000) ⇒ 窗口内
    const LicenseSnapshot snap = to_snapshot(status, lr, /*now_ms=*/2000);
    CHECK(snap.grace_until_ms == 5000);
    CHECK(snap.ai_allowed());

    const LicenseSnapshot snap2 = to_snapshot(status, lr, /*now_ms=*/6000);  // 超宽限
    CHECK(snap2.grace_until_ms == 0);
    CHECK(!snap2.ai_allowed());
}

// 防降级（本地篡改）⇒ fail-closed：issued_at 回退 = 降级，拒绝该文档。
TEST(gate_tamper_downgrade_fail_closed) {
    using ttbox::core::auth::LicenseStore;
    // 文档 issued_at(1000) < 已见基线(2000) ⇒ 降级（拒绝）
    CHECK(LicenseStore::is_downgrade(/*doc=*/1000, /*last_seen=*/2000));
    // 拒绝后回上一可信态；若无可信态则未激活 ⇒ 不允许。
    LicenseStatus unactivated;  // 默认 kUnknown
    const LicenseSnapshot snap = to_snapshot(unactivated, StoreLoadResult{}, /*now_ms=*/1);
    CHECK(!snap.ai_allowed());
}

// 绑定不匹配（kBoundElsewhere）⇒ restricted_hard + fail-closed。
TEST(gate_bind_mismatch_fail_closed) {
    LicenseStatus status;
    status.state = LicenseState::kBoundElsewhere;
    const LicenseSnapshot snap = to_snapshot(status, StoreLoadResult{}, /*now_ms=*/1);
    CHECK(wire_state_name(snap) == "restricted_hard");
    CHECK(!wire_activated(snap));
    CHECK(!snap.ai_allowed());
}

// 未激活默认（A23 host 侧等价）：空恢复 + 默认状态 ⇒ unactivated / activated=false。
TEST(gate_unactivated_default) {
    LicenseStatus status;  // 默认 kUnknown
    const LicenseSnapshot snap = to_snapshot(status, StoreLoadResult{}, /*now_ms=*/1);
    CHECK(wire_state_name(snap) == "unactivated");
    CHECK(!wire_activated(snap));
    CHECK(!snap.ai_allowed());
    CHECK(snap.plan == "none");
    CHECK(snap.features.empty());

    // LicenseGate 未 publish 时的默认快照亦为 unactivated。
    LicenseGate& gate = LicenseGate::instance();
    const LicenseSnapshot def = gate.snapshot();
    CHECK(wire_state_name(def) == "unactivated");
    CHECK(!gate.ai_allowed());
}

// wire 态映射（§3.2）逐行相等。
TEST(gate_wire_state_mapping) {
    struct Row { LicenseState st; const char* plan; const char* wire; };
    const Row rows[] = {
        {LicenseState::kUnknown, "none", "unactivated"},
        {LicenseState::kInvalidCard, "none", "unactivated"},
        {LicenseState::kChecking, "none", "unactivated"},
        {LicenseState::kValid, "permanent", "active"},
        {LicenseState::kValid, "trial", "trial_active"},
        {LicenseState::kFallback, "permanent", "active"},
        {LicenseState::kFallback, "trial", "trial_active"},
        {LicenseState::kExpired, "permanent", "restricted"},
        {LicenseState::kBoundElsewhere, "permanent", "restricted_hard"},
        {LicenseState::kNetworkError, "none", "net_unreachable"},
    };
    for (const Row& r : rows) {
        LicenseSnapshot s;
        s.state = r.st;
        s.plan = r.plan;
        CHECK(wire_state_name(s) == r.wire);
    }
}

// features 空集 ⇒ 一律 false（M1 诚实空集）。
TEST(gate_feature_empty_is_false) {
    LicenseSnapshot s;
    s.features.clear();
    CHECK(!s.feature_enabled("aim"));
    CHECK(!s.feature_enabled("inference"));
    CHECK(!s.feature_enabled(nullptr));

    s.features = {"aim"};
    CHECK(s.feature_enabled("aim"));
    CHECK(!s.feature_enabled("ota"));
}
