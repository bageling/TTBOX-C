// test_license_state_machine.cpp — T1.08 单元测试
//
// 目的：锁定「失败的检查不得抹掉上一次成功验证的时间戳」这一修复，
//       并验证 kFallback（网络错误 fail-open）语义，对应 design.md §A8.0 验收：
//         ① 成功 → 网络失败 → 允许运行（kFallback 触发）
//         ③ 服务器明确 expired → 停 AI
//
// 纯函数 apply_check_result()（auth/LicenseStateMachine.hpp）不依赖 OpenSSL，
// 故默认构建（TTBOX_CORE_BUILD_AUTH=OFF）即可运行全部纯逻辑用例。
// 以 -DTTBOX_CORE_BUILD_AUTH=ON 构建时，额外启用基于真实 LicenseDaemon 的集成用例
// （见文件末尾 #ifdef TTBOX_CORE_AUTH_TESTS）。

#include "test_util.hpp"

#include "auth/LicenseStateMachine.hpp"

using namespace ttbox::core::auth;

namespace {

constexpr int64_t kMinute = 60 * 1000;
constexpr int64_t kHour = 60 * kMinute;
constexpr int64_t kBase = 30 * 1000;    // 退避基数 30s
constexpr int64_t kCap = 1800 * 1000;   // 退避上限 30m

// 构造一次"成功验证"的服务端下发结果。
LicenseStatus make_valid_out(int64_t expire_ms, int heartbeat_s = 60) {
    LicenseStatus out;
    out.state = LicenseState::kValid;
    out.is_pro = true;
    out.expire_unix_ms = expire_ms;
    out.heartbeat_interval = heartbeat_s;
    return out;
}

// 与 LicenseDaemon::allow_run()（LicenseDaemon.cpp:35-42）一致的判定映射：
//   kValid | kFallback → 允许运行 AI；其余一律不允许。
// 抽成函数以便验收断言直读"是否允许运行"的语义。
bool allows_run(LicenseState s) {
    return s == LicenseState::kValid || s == LicenseState::kFallback;
}

}  // namespace

// 验收①（正向）：成功验证 → 网络失败 → 必须落到 kFallback（= 允许运行）
TEST(license_sm_success_then_network_error_falls_back) {
    LicenseStatus status;   // 初始：verified=0, expire=0
    int64_t backoff = 0;

    const int64_t t0 = 1'000'000'000'000LL;
    const int64_t expire = t0 + 48 * kHour;

    // 第 1 次：成功
    apply_check_result(status, make_valid_out(expire), /*req_ok=*/true,
                       t0, kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kValid);
    CHECK_EQ(status.verified_at_ms, t0);
    CHECK_EQ(status.expire_unix_ms, expire);
    CHECK(allows_run(status.state));

    // 第 2 次：网络失败（请求未成功），时间只前进 1 分钟
    LicenseStatus net_out;   // 默认：state=kUnknown, expire=0
    net_out.state = LicenseState::kNetworkError;
    apply_check_result(status, net_out, /*req_ok=*/false,
                       t0 + kMinute, kHour, kBase, kCap, backoff);

    // 关键断言：时间戳被保留，且进入 kFallback
    CHECK_EQ(status.verified_at_ms, t0);              // 未被清零
    CHECK_EQ(status.expire_unix_ms, expire);          // 未被清零
    CHECK(status.state == LicenseState::kFallback);   // 修复前恒不可达
    CHECK(allows_run(status.state));                  // allow_run()==true
}

// 回归（反向）：修复前的 bug 会把历史时间戳带成 0，使 kFallback 不可达。
// 此处直接验证"失败结果不会抹掉历史时间戳"——这是 kFallback 可达的充要前提。
TEST(license_sm_failure_preserves_history) {
    LicenseStatus status;
    int64_t backoff = 0;
    const int64_t t0 = 2'000'000'000'000LL;
    const int64_t expire = t0 + 10 * kHour;

    apply_check_result(status, make_valid_out(expire), true, t0, kHour, kBase, kCap, backoff);
    CHECK_EQ(status.verified_at_ms, t0);

    // 一次普通网络失败，out 里 expire=0（客户端无法给出）
    LicenseStatus net_out;
    net_out.state = LicenseState::kNetworkError;
    apply_check_result(status, net_out, false, t0 + kMinute, kHour, kBase, kCap, backoff);
    CHECK_EQ(status.verified_at_ms, t0);
    CHECK_EQ(status.expire_unix_ms, expire);
}

// 验收③：服务器明确 expired → 停 AI（kFallback 绝不覆盖权威否定）
TEST(license_sm_explicit_expired_is_not_fallback) {
    LicenseStatus status;
    int64_t backoff = 0;
    const int64_t t0 = 3'000'000'000'000LL;

    // 先成功（expire 在未来）
    apply_check_result(status, make_valid_out(t0 + kHour), true, t0, kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kValid);

    // 服务器明确 expired：req_ok=true，state=kExpired，expire 落在过去
    LicenseStatus exp_out;
    exp_out.state = LicenseState::kExpired;
    exp_out.expire_unix_ms = t0 - kMinute;
    apply_check_result(status, exp_out, true, t0, kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kExpired);
    CHECK(!allows_run(status.state));
}

// ★ 关键边界：服务器明确否定（kExpired/kInvalidCard/kBoundElsewhere）但未给出
// expire 字段（=0）时，历史 expire 仍落在未来；若不做"仅网络类失败才 fail-open"
// 的区分，kFallback 会被错误触发 → 授权被"洗白"。此用例锁死该红线。
TEST(license_sm_explicit_negative_without_expire_never_falls_back) {
    LicenseStatus status;
    int64_t backoff = 0;
    const int64_t t0 = 4'000'000'000'000LL;

    for (LicenseState s : {LicenseState::kExpired,
                           LicenseState::kInvalidCard,
                           LicenseState::kBoundElsewhere}) {
        // 每轮先恢复到"成功且未到期"状态
        apply_check_result(status, make_valid_out(t0 + 24 * kHour), true,
                           t0, kHour, kBase, kCap, backoff);

        LicenseStatus neg;
        neg.state = s;
        neg.expire_unix_ms = 0;   // 明确否定但未下发 expire
        apply_check_result(status, neg, /*req_ok=*/true, t0 + kMinute,
                           kHour, kBase, kCap, backoff);
        CHECK(status.state == s);
        CHECK(!allows_run(status.state));
    }
}

// 网络失败但本地缓存已过期 → 不得 fail-open
TEST(license_sm_network_error_after_expiry_no_fallback) {
    LicenseStatus status;
    int64_t backoff = 0;
    const int64_t t0 = 5'000'000'000'000LL;
    const int64_t expire = t0 + kHour;
    apply_check_result(status, make_valid_out(expire), true, t0, kHour, kBase, kCap, backoff);

    LicenseStatus net_out;
    net_out.state = LicenseState::kNetworkError;
    // now 已越过 expire
    apply_check_result(status, net_out, false, expire + kMinute, kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kNetworkError);
    CHECK(!allows_run(status.state));
}

// 从未成功验证过（verified=0）→ 网络失败不得凭空空转成 kFallback
TEST(license_sm_never_verified_network_error_no_fallback) {
    LicenseStatus status;   // verified=0, expire=0
    int64_t backoff = 0;
    LicenseStatus net_out;
    net_out.state = LicenseState::kNetworkError;
    net_out.expire_unix_ms = 123456;   // 即便给出 expire，缺 verified 也不 fail-open
    apply_check_result(status, net_out, false, 1000, kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kNetworkError);
    CHECK_EQ(status.verified_at_ms, 0);
    CHECK(!allows_run(status.state));
}

// 退避：30s → 60s → 120s → … 封顶 30m；成功清零
TEST(license_sm_backoff_progression_and_reset) {
    LicenseStatus status;
    int64_t backoff = 0;
    LicenseStatus net_out;
    net_out.state = LicenseState::kNetworkError;
    const int64_t t0 = 6'000'000'000'000LL;

    apply_check_result(status, net_out, false, t0, kHour, kBase, kCap, backoff);
    CHECK_EQ(backoff, kBase);        // 30s
    apply_check_result(status, net_out, false, t0, kHour, kBase, kCap, backoff);
    CHECK_EQ(backoff, 60 * 1000);    // 2x
    apply_check_result(status, net_out, false, t0, kHour, kBase, kCap, backoff);
    CHECK_EQ(backoff, 120 * 1000);

    for (int i = 0; i < 20; ++i) {   // 反复失败直到封顶
        apply_check_result(status, net_out, false, t0, kHour, kBase, kCap, backoff);
    }
    CHECK_EQ(backoff, kCap);         // 封顶 30m

    // 成功后清零
    apply_check_result(status, make_valid_out(t0 + kHour), true, t0, kHour, kBase, kCap, backoff);
    CHECK_EQ(backoff, 0);
}

// 成功时刷新 next_check_ms（服务端心跳优先，否则用默认 heartbeat_ms）
TEST(license_sm_success_sets_next_check) {
    LicenseStatus status;
    int64_t backoff = 12345;
    const int64_t t0 = 7'000'000'000'000LL;

    // 服务端下发心跳 300s
    LicenseStatus out = make_valid_out(t0 + kHour, /*heartbeat_s=*/300);
    apply_check_result(status, out, true, t0, kHour, kBase, kCap, backoff);
    CHECK_EQ(status.next_check_ms, t0 + 300 * 1000);
    CHECK_EQ(backoff, 0);

    // 服务端未下发（heartbeat_interval <= 0）→ 用默认 heartbeat_ms
    LicenseStatus out2 = make_valid_out(t0 + kHour, /*heartbeat_s=*/0);
    apply_check_result(status, out2, true, t0, /*heartbeat_ms=*/5000,
                       kBase, kCap, backoff);
    CHECK_EQ(status.next_check_ms, t0 + 5000);
}

// 健壮性①：请求本身失败（req_ok=false）时，即便客户端回报 state=kValid
// （如响应被截断/错位解析导致的"假成功"），也**绝不**采纳 kValid。
// 因属网络类失败且历史仍在有效期内 → 应落到 kFallback（fail-open）。
TEST(license_sm_req_failed_never_yields_valid) {
    LicenseStatus status;
    int64_t backoff = 0;
    const int64_t t0 = 8'000'000'000'000LL;

    // 先建立历史成功，使 fail-open 可达
    apply_check_result(status, make_valid_out(t0 + 24 * kHour), true,
                       t0, kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kValid);

    // 请求失败，但 out 谎报 kValid
    LicenseStatus lying = make_valid_out(t0 + 24 * kHour);
    apply_check_result(status, lying, /*req_ok=*/false, t0 + kMinute,
                       kHour, kBase, kCap, backoff);

    CHECK(status.state != LicenseState::kValid);          // 核心：绝不 kValid
    CHECK(status.state == LicenseState::kFallback);       // 网络类失败 fail-open
    CHECK(allows_run(status.state));
}

// 健壮性①（无历史）：请求失败 + 谎报 kValid，且从未成功验证过
// → 既不得 kValid，也不得凭空空转 kFallback。
TEST(license_sm_req_failed_never_valid_no_history) {
    LicenseStatus status;   // verified=0, expire=0
    int64_t backoff = 0;
    LicenseStatus lying;
    lying.state = LicenseState::kValid;
    lying.expire_unix_ms = 9'000'000'000'000LL;
    apply_check_result(status, lying, /*req_ok=*/false, 1000, kHour, kBase, kCap,
                       backoff);
    CHECK(status.state != LicenseState::kValid);
    CHECK(status.state == LicenseState::kNetworkError);
    CHECK(!allows_run(status.state));
}

// 健壮性②：expire == 0 视为无期限（永久授权）→ 网络失败时亦 fail-open；
// 但"服务端明确否定"仍不得 fail-open。
TEST(license_sm_zero_expire_is_permanent_and_falls_back) {
    LicenseStatus status;
    int64_t backoff = 0;
    const int64_t t0 = 10'000'000'000'000LL;

    // 服务端返回成功但 expire_time=0（无期限）
    apply_check_result(status, make_valid_out(/*expire_ms=*/0), true,
                       t0, kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kValid);
    CHECK_EQ(status.expire_unix_ms, 0);

    // 网络失败：历史有效且无到期时间 → 应 kFallback（修复前恒为 kNetworkError）
    LicenseStatus net_out;
    net_out.state = LicenseState::kNetworkError;
    apply_check_result(status, net_out, /*req_ok=*/false, t0 + kMinute,
                       kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kFallback);
    CHECK(allows_run(status.state));

    // 对照：明确的服务端否定（kExpired）即便 expire==0 也不得 fail-open
    apply_check_result(status, make_valid_out(0), true, t0, kHour, kBase, kCap,
                       backoff);
    LicenseStatus neg;
    neg.state = LicenseState::kExpired;
    neg.expire_unix_ms = 0;
    apply_check_result(status, neg, /*req_ok=*/true, t0 + kMinute,
                       kHour, kBase, kCap, backoff);
    CHECK(status.state == LicenseState::kExpired);
    CHECK(!allows_run(status.state));
}

// 注：真实 LicenseDaemon 的端到端集成用例见 tests/test_license_daemon.cpp
// （独立 ctest 目标 test_license_daemon）。本文件只覆盖不依赖 OpenSSL 的纯状态机逻辑。
