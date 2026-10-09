// test_license_features.cpp — M2 签名卡内容（features / plan / ui_brand）单元测试
//
// 覆盖三条不变量（对标矩阵 G6/G7「字段已备、门控未建」的收口）：
//   ① 可信态才放行：kValid / kFallback，以及 kExpired 且宽限内 ⇒ 卡内容投影出去；
//      其余（kUnknown / kChecking / kInvalidCard / kBoundElsewhere / kExpired 超宽限）
//      ⇒ 一律 fail-closed 清空，且品牌回落默认值（降级设备不得留下上次的品牌痕迹）。
//   ② 卡内容不可信字符必须被拦在唯一闸门（to_snapshot）：
//      闭集外 feature 名丢弃、ui_brand 不符合 [A-Za-z0-9_-] 或超长即回落默认。
//   ③ 网络抖动不得清空已授权功能位（fail-open），权威否定必须清空（fail-closed）。
//
// 框架：ttbox_test（TEST/CHECK，禁裸 assert）。本 TU 并入 ttbox_core_tests（test_main.cpp 提供 main）。
#include "test_util.hpp"

#include "auth/LicenseGate.hpp"
#include "auth/LicenseShortCode.hpp"
#include "auth/LicenseStateMachine.hpp"
#include "auth/LicenseStore.hpp"

#include <cstdint>
#include <string>
#include <vector>

using ttbox::core::auth::apply_check_result;
using ttbox::core::auth::default_ui_brand;
using ttbox::core::auth::is_known_feature;
using ttbox::core::auth::known_features;
using ttbox::core::auth::LicenseSnapshot;
using ttbox::core::auth::LicenseState;
using ttbox::core::auth::LicenseStatus;
using ttbox::core::auth::normalize_features;
using ttbox::core::auth::StoreLoadResult;
using ttbox::core::auth::to_snapshot;

namespace {

LicenseStatus make_status(LicenseState st) {
    LicenseStatus s;
    s.state = st;
    s.verified_at_ms = 1'000;
    s.expire_unix_ms = 10'000'000;
    return s;
}

bool has(const std::vector<std::string>& v, const std::string& x) {
    for (const auto& e : v) {
        if (e == x) return true;
    }
    return false;
}

}  // namespace

// ---- 闭集是唯一权威定义（Metrics / LicenseGate 的注释均指向此处，防各写一份）----
TEST(license_features_closed_set_is_single_source) {
    const auto& k = known_features();
    CHECK(k.size() == 4);
    CHECK(is_known_feature("capture"));
    CHECK(is_known_feature("inference"));
    CHECK(is_known_feature("aim"));
    CHECK(is_known_feature("ota"));
    CHECK(!is_known_feature(""));
    CHECK(!is_known_feature("Aim"));      // 大小写敏感
    CHECK(!is_known_feature("teleport")); // 闭集外

    const auto n = normalize_features({"", "ota", "ota", "nope"});
    CHECK(n.size() == 1);
    CHECK(n[0] == "ota");
}

// ---- ① 可信态：卡内容投影出去，field 名与取值逐项可见 ----
TEST(license_features_valid_projects_card_content) {
    LicenseStatus s = make_status(LicenseState::kValid);
    s.features = {"aim", "ota"};
    s.plan = "subscription";
    s.ui_brand = "yuai";

    const LicenseSnapshot snap = to_snapshot(s, StoreLoadResult{}, /*now_ms=*/2000);
    CHECK(snap.features.size() == 2);
    CHECK(has(snap.features, "aim"));
    CHECK(has(snap.features, "ota"));
    CHECK(snap.feature_enabled("aim"));
    CHECK(snap.feature_enabled("ota"));
    CHECK(!snap.feature_enabled("capture"));  // 未授予 ⇒ false
    CHECK(!snap.feature_enabled(nullptr));    // 空指针安全
    CHECK(snap.plan == "subscription");
    CHECK(snap.ui_brand == "yuai");
    CHECK(snap.ai_allowed());
}

// ---- ① 未激活：即便结构体残留内容也不得投影（防内存复用/异常路径泄漏）----
TEST(license_features_unactivated_clears_everything) {
    LicenseStatus s = make_status(LicenseState::kUnknown);
    s.features = {"aim", "ota"};
    s.plan = "subscription";
    s.ui_brand = "yuai";

    const LicenseSnapshot snap = to_snapshot(s, StoreLoadResult{}, 2000);
    CHECK(snap.features.empty());
    CHECK(!snap.feature_enabled("aim"));
    CHECK(snap.plan == "none");
    CHECK(snap.ui_brand == std::string(default_ui_brand()));
    CHECK(!snap.ai_allowed());
}

// ---- ① 权威否定 / 校验中：一律清空（fail-closed）----
TEST(license_features_authoritative_denial_clears_everything) {
    const LicenseState rows[] = {LicenseState::kInvalidCard, LicenseState::kBoundElsewhere,
                                 LicenseState::kChecking};
    for (LicenseState st : rows) {
        LicenseStatus s = make_status(st);
        s.features = {"aim"};
        s.plan = "permanent";
        s.ui_brand = "yuai";
        const LicenseSnapshot snap = to_snapshot(s, StoreLoadResult{}, 2000);
        CHECK(snap.features.empty());
        CHECK(snap.plan == "none");
        CHECK(snap.ui_brand == std::string(default_ui_brand()));
        CHECK(!snap.feature_enabled("aim"));
    }
}

// ---- ① 过期：宽限内放行、超宽限清空（同一状态、两个 now 的边界对）----
TEST(license_features_expired_grace_boundary) {
    LicenseStatus s = make_status(LicenseState::kExpired);
    s.features = {"aim"};
    s.ui_brand = "yuai";

    StoreLoadResult in_grace;
    in_grace.grace_until = 5000;  // now=2000 < 5000 ⇒ 仍在宽限窗口
    const LicenseSnapshot a = to_snapshot(s, in_grace, 2000);
    CHECK(a.grace_until_ms == 5000);
    CHECK(a.features.size() == 1);
    CHECK(a.ui_brand == "yuai");
    CHECK(a.ai_allowed());  // 宽限是服务端给的延长期 ⇒ 卡内容仍有效

    StoreLoadResult out_grace;
    out_grace.grace_until = 1000;  // now=2000 >= 1000 ⇒ 已超宽限
    const LicenseSnapshot b = to_snapshot(s, out_grace, 2000);
    CHECK(b.grace_until_ms == 0);
    CHECK(b.features.empty());
    CHECK(b.ui_brand == std::string(default_ui_brand()));
    CHECK(!b.ai_allowed());
}

// ---- ② 归一化：空串 / 闭集外 / 重复 全部丢弃，且保首次出现顺序 ----
TEST(license_features_normalize_drops_unknown_and_dupes) {
    LicenseStatus s = make_status(LicenseState::kValid);
    s.features = {"aim", "", "aim", "AIM", "teleport", "ota", "capture", "capture"};

    const LicenseSnapshot snap = to_snapshot(s, StoreLoadResult{}, 2000);
    CHECK(snap.features.size() == 3);
    CHECK(!snap.feature_enabled("AIM"));
    CHECK(!snap.feature_enabled("teleport"));
    CHECK(snap.features[0] == "aim");
    CHECK(snap.features[1] == "ota");
    CHECK(snap.features[2] == "capture");
}

// ---- ② plan：卡下发优先；缺失才按 is_pro 粗分（M1 兼容路径语义不变）----
TEST(license_features_plan_falls_back_to_is_pro) {
    LicenseStatus s = make_status(LicenseState::kValid);

    s.plan.clear();
    s.is_pro = false;
    CHECK(to_snapshot(s, StoreLoadResult{}, 2000).plan == "permanent");

    s.is_pro = true;
    CHECK(to_snapshot(s, StoreLoadResult{}, 2000).plan == "subscription");

    s.plan = "trial";
    CHECK(to_snapshot(s, StoreLoadResult{}, 2000).plan == "trial");
}

// ---- ② ui_brand：字符集闸门（会被前端渲染，故必须拦注入与非 ASCII）----
TEST(license_features_ui_brand_charset_guard) {
    struct Row {
        const char* in;
        const char* want;
    };
    const Row rows[] = {
        {"yuai", "yuai"},
        {"XC-SH_2", "XC-SH_2"},
        {"a", "a"},
        {"", "ttbox"},                                    // 空 ⇒ 默认
        {"<script>alert(1)</script>", "ttbox"},           // HTML/JS 注入
        {"yu ai", "ttbox"},                               // 空格
        {"-lead", "ttbox"},                               // 首字符非字母数字
        {"_lead", "ttbox"},                               // 同上
        {"中文品牌", "ttbox"},                             // 非 ASCII
        {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ttbox"},   // 33 字符 > 上限 32
    };
    for (const Row& r : rows) {
        LicenseStatus s = make_status(LicenseState::kValid);
        s.ui_brand = r.in;
        const LicenseSnapshot snap = to_snapshot(s, StoreLoadResult{}, 2000);
        CHECK(snap.ui_brand == std::string(r.want));
    }

    // 32 字符边界恰好放行（上限是闭区间）
    LicenseStatus edge = make_status(LicenseState::kValid);
    edge.ui_brand = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // 恰好 32
    CHECK(to_snapshot(edge, StoreLoadResult{}, 2000).ui_brand.size() == 32);
}

// ---- ③ 网络类失败 ⇒ fail-open：已授权功能位与品牌必须原样保留 ----
TEST(license_features_network_failure_preserves_card_content) {
    LicenseStatus status = make_status(LicenseState::kValid);
    status.features = {"aim", "ota"};
    status.plan = "subscription";
    status.ui_brand = "yuai";

    LicenseStatus out;  // 本轮请求失败 ⇒ 客户端解析失败路径不填这三项
    out.state = LicenseState::kNetworkError;
    int64_t backoff = 0;
    apply_check_result(status, out, /*req_ok=*/false, /*now_ms=*/2000,
                       /*heartbeat_ms=*/60'000, /*backoff_base=*/30'000,
                       /*backoff_cap=*/1'800'000, backoff);

    CHECK(status.state == LicenseState::kFallback);
    CHECK(status.features.size() == 2);   // ★ 不得被网络抖动清空
    CHECK(status.plan == "subscription");
    CHECK(status.ui_brand == "yuai");

    const LicenseSnapshot snap = to_snapshot(status, StoreLoadResult{}, 2000);
    CHECK(snap.ai_allowed());
    CHECK(snap.feature_enabled("aim"));  // 断网仍保留已授权功能位
    CHECK(snap.ui_brand == "yuai");
}

// ---- ③ 权威否定 ⇒ fail-closed：状态机层即清空（不靠 to_snapshot 兜底）----
TEST(license_features_authoritative_negation_clears_status) {
    LicenseStatus status = make_status(LicenseState::kValid);
    status.features = {"aim"};
    status.plan = "subscription";
    status.ui_brand = "yuai";

    LicenseStatus out;
    out.state = LicenseState::kBoundElsewhere;  // 权威否定：卡已绑到别的设备
    int64_t backoff = 0;
    apply_check_result(status, out, /*req_ok=*/true, 2000, 60'000, 30'000, 1'800'000, backoff);

    CHECK(status.features.empty());  // ★ 权威否定必须清空，不回落
    CHECK(status.plan.empty());
    CHECK(status.ui_brand.empty());

    const LicenseSnapshot snap = to_snapshot(status, StoreLoadResult{}, 2000);
    CHECK(!snap.feature_enabled("aim"));
    CHECK(snap.ui_brand == std::string(default_ui_brand()));
}

// ---- ③ 契约锁：成功响应不带 features ⇒ 视为「不授予」（**有意行为**，非意外）----
//
// 这条用例的作用是把服务端契约要求变成可执行的断言：
//   card-login / heartbeat 的**成功响应必须携带** features / plan / uiBrand
//   （features 允许空数组，表示明确不授予任何功能位）。
// 若在此改成静默回落，就把「服务端撤销」也变成回落 ⇒ fail-open 过头、撤销失效。
// 变更本行为前必须先改服务端契约（见 TtboxLicenseClient.cpp 的 parse_card_content 注释）。
TEST(license_features_absent_on_success_is_empty_by_contract) {
    LicenseStatus status = make_status(LicenseState::kValid);
    status.features = {"aim"};
    status.ui_brand = "yuai";

    LicenseStatus out;  // 成功响应，但三个字段全缺
    out.state = LicenseState::kValid;
    out.expire_unix_ms = 20'000'000;
    int64_t backoff = 0;
    apply_check_result(status, out, /*req_ok=*/true, 2000, 60'000, 30'000, 1'800'000, backoff);

    CHECK(status.state == LicenseState::kValid);
    CHECK(status.features.empty());  // 契约行为：不授予
    CHECK(status.ui_brand.empty());

    const LicenseSnapshot snap = to_snapshot(status, StoreLoadResult{}, 2000);
    CHECK(snap.features.empty());
    CHECK(snap.ui_brand == std::string(default_ui_brand()));  // 品牌回落默认，不留上次痕迹
}

// ---- M2.03：会话级 gate = feature_enabled("capture")（等价收窄，非语义改写）----
TEST(license_features_pipeline_allowed_is_capture_gated) {
    using ttbox::core::auth::feature_name::kCapture;
    LicenseStatus s = make_status(LicenseState::kValid);

    s.features = {"inference", "aim"};
    CHECK(!to_snapshot(s, StoreLoadResult{}, 2000).pipeline_allowed());  // 无帧源 ⇒ 不起整链
    s.features = {"capture"};
    CHECK(to_snapshot(s, StoreLoadResult{}, 2000).pipeline_allowed());
    s.features = {"capture", "ai", "teleport"};  // 闭集外名字被丢弃，capture 仍有效
    const LicenseSnapshot snap = to_snapshot(s, StoreLoadResult{}, 2000);
    CHECK(snap.pipeline_allowed());
    CHECK(snap.feature_enabled(kCapture));
    CHECK(!snap.feature_enabled("teleport"));

    // 不可信态：features 清空 ⇒ gate 恒 false（已隐含 state 门）。
    LicenseStatus unact = make_status(LicenseState::kInvalidCard);
    unact.features = {"capture"};
    CHECK(!to_snapshot(unact, StoreLoadResult{}, 2000).pipeline_allowed());
}

// ---- M2.05：短码投影只在可信态出现（与 ui_brand 同闸门）----
TEST(license_features_short_code_trusted_only) {
    const std::string id = "ttbox-lic-20260917-3842ff";
    const std::string want = ttbox::core::auth::license_short_code_derive(id);
    CHECK(want == std::string("TTB-006Z-0C33"));  // 跨语言向量（同 test_license_shortcode.cpp）

    // 可信态（kValid）⇒ 非空且等于派生值。
    LicenseStatus solid = make_status(LicenseState::kValid);
    solid.license_id = id;
    CHECK(to_snapshot(solid, StoreLoadResult{}, 2000).short_code == want);

    // 可信态但无 id（M1 单串卡 / 未填）⇒ 空串（不臆造）。
    LicenseStatus no_id = make_status(LicenseState::kValid);
    CHECK(to_snapshot(no_id, StoreLoadResult{}, 2000).short_code.empty());

    // 不可信态（kUnknown / kInvalidCard / kBoundElsewhere）⇒ 即便残留 id 也不得投影。
    const LicenseState rows[] = {LicenseState::kUnknown, LicenseState::kInvalidCard,
                                 LicenseState::kBoundElsewhere, LicenseState::kChecking};
    for (LicenseState st : rows) {
        LicenseStatus s = make_status(st);
        s.license_id = id;
        CHECK(to_snapshot(s, StoreLoadResult{}, 2000).short_code.empty());
    }
}
