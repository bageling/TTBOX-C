// test_license_daemon.cpp — T1.08 集成测试（真实 LicenseDaemon 端到端）
//
// 用脚本化 ILicenseClient 注入"先成功、再断网、最后服务器明确 expired"的序列，
// 直接驱动真实 LicenseDaemon（core/src/auth/LicenseDaemon.cpp），端到端验证
// do_check_cycle_locked() / verify_now_blocking() 的状态迁移与 allow_run()。
//
// 本目标自行编译 LicenseDaemon.cpp + DeviceFingerprint.cpp（二者均不依赖 OpenSSL），
// 因此**在默认构建（TTBOX_CORE_BUILD_AUTH=OFF）下即可运行**，不受授权层其它历史
// 文件（在线授权层）既有历史编译问题的影响。
// 见 core/CMakeLists.txt 中目标 test_license_daemon。
//
// ★ M2 QA 追加（2026-09-17 · QA 严过关）：F2 / F4 的 activate() 宿主级覆盖。
//   此前 F2/F4 仅板端验收（B12/B14）实证，宿主单测缺口：
//     · activate() 解析级拒绝 ⇒ last_error 非空（F2 ①）
//     · activate() 验签级拒绝（bound elsewhere）后，thread_loop 空卡分支
//       在下一退避周期**不得**把真因冲成 "card not set"（F2 ②）
//     · activate() kValid ⇒ save_doc + save_state 基线前进（F4 ②）
//     · 更旧 issued_at 卡 ⇒ 拒绝 + **不落盘**（license.json/state.json 前后不变）（F4 ③）
//     · 等值 issued_at（重复激活）与更晚换卡 ⇒ 不误判降级（F4 边界）
//   磁盘路径 = LicenseStore 默认目录 /var/lib/ttbox/license（LicenseDaemon 硬编码），
//   每用例前后清理，不留残渣。

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "auth/LicenseDaemon.hpp"
#include "auth/LicenseStore.hpp"
#include "test_util.hpp"

using namespace ttbox::core::auth;

namespace {

// 脚本化客户端：按预设序列逐次返回 verify_once 结果。
class ScriptedClient : public ILicenseClient {
public:
    struct Step {
        bool req_ok;
        LicenseState state;
        int64_t expire_ms;
    };
    std::vector<Step> steps;
    std::size_t idx = 0;

    bool verify_once(const std::string&, const std::string&,
                     LicenseStatus& out, std::string* err) override {
        out = LicenseStatus{};
        if (idx >= steps.size()) {
            if (err) *err = "no more scripted steps";
            return false;
        }
        const Step& s = steps[idx++];
        out.state = s.state;
        out.is_pro = (s.state == LicenseState::kValid);
        out.expire_unix_ms = s.expire_ms;
        out.heartbeat_interval = 60;
        return s.req_ok;
    }
};

int64_t now_unix_ms_now() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

// ============================================================================
// ★ M2.07.1 T3：resolve_license_card() store 分支守卫（D-D）宿主单测
//   该守卫此前**无宿主单测**（函数未链接进本目标）⇒ 变异不变红，只靠板端 E2E 兜。
//   现判定唯一实现 = auth::offline_card_doc(StoreLoadResult)（LicenseStore.hpp）；
//   Application::resolve_license_card() 直接调用它 ⇒ 本处测同一实现，变异即变红。
//   反向（护栏）：cloud 形文档（doc_is_cloud=true）**不得**当离线卡交出。
//   正向：普通离线信封（doc_is_cloud=false）⇒ 正常交出。
// ============================================================================
TEST(resolve_card_rejects_cloud_doc) {
    StoreLoadResult cloud;
    cloud.doc_ok = true;
    cloud.doc_is_cloud = true;
    cloud.doc_json = "{\"source\":\"cloud\",\"card_mask\":\"TTB-XXXX-XXXX\"}";
    // ★ 反向护栏：cloud 形文档绝不能作为“离线卡”交出（否则重启后云激活被抹，D-D）。
    CHECK(offline_card_doc(cloud).empty());
}

TEST(resolve_card_accepts_offline_envelope) {
    StoreLoadResult off;
    off.doc_ok = true;
    off.doc_is_cloud = false;
    off.doc_json = "{\"license\":{\"license_id\":\"x\"},\"signature\":\"y\"}";
    // 正向：非 cloud 形 + 可用 ⇒ 原样交出。
    CHECK(offline_card_doc(off) == off.doc_json);
}

TEST(resolve_card_rejects_missing_or_empty_doc) {
    StoreLoadResult missing;                 // 全默认：doc_ok=false
    CHECK(offline_card_doc(missing).empty());
    StoreLoadResult empty;
    empty.doc_ok = true;                     // doc_ok=true 但 doc_json 空
    CHECK(offline_card_doc(empty).empty());
}

// 端到端验收①+③：
//   ① 验证成功 → kValid，allow_run()==true
//   ② 网络失败 → kFallback（时间戳保留），allow_run()==true（fail-open 红线）
//   ③ 服务器明确 expired → kExpired，allow_run()==false
TEST(license_daemon_fallback_then_expired) {
    ScriptedClient client;
    const int64_t now_ms = now_unix_ms_now();

    client.steps.push_back({true, LicenseState::kValid, now_ms + 3600 * 1000});
    client.steps.push_back({false, LicenseState::kNetworkError, 0});
    client.steps.push_back({true, LicenseState::kExpired, now_ms - 1000});

    LicenseDaemon daemon(client);
    daemon.set_card("TTBOX-TEST-CARD-0001");
    std::string err;

    // ① 成功
    CHECK(daemon.verify_now_blocking(&err));
    CHECK(daemon.allow_run());
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == LicenseState::kValid);
        CHECK(st.verified_at_ms > 0);
        CHECK(st.expire_unix_ms > 0);
    }

    // ② 网络失败 → fail-open：时间戳保留，落到 kFallback，允许运行
    CHECK(!daemon.verify_now_blocking(&err));
    CHECK(daemon.allow_run());
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == LicenseState::kFallback);
        CHECK(st.verified_at_ms > 0);   // 修复前此处会被清零 → kFallback 不可达
        CHECK(st.expire_unix_ms > 0);
    }

    // ③ 服务器明确 expired → 停 AI
    CHECK(daemon.verify_now_blocking(&err));
    CHECK(!daemon.allow_run());
    CHECK(daemon.status_snapshot().state == LicenseState::kExpired);
}

// 回归：在真实 LicenseDaemon 上验证"失败不抹时间戳"（修复前 verified_at_ms 会归零）
TEST(license_daemon_failure_preserves_verified_at) {
    ScriptedClient client;
    const int64_t now_ms = now_unix_ms_now();
    client.steps.push_back({true, LicenseState::kValid, now_ms + 7200 * 1000});
    client.steps.push_back({false, LicenseState::kNetworkError, 0});

    LicenseDaemon daemon(client);
    daemon.set_card("TTBOX-TEST-CARD-0002");
    std::string err;

    CHECK(daemon.verify_now_blocking(&err));
    const int64_t verified_after_success = daemon.status_snapshot().verified_at_ms;
    CHECK(verified_after_success > 0);

    CHECK(!daemon.verify_now_blocking(&err));
    const LicenseStatus st = daemon.status_snapshot();
    CHECK_EQ(st.verified_at_ms, verified_after_success);   // 未被清零
    CHECK(st.state == LicenseState::kFallback);
}

// ============================================================================
// ★ M2 QA 追加（2026-09-17 · QA 严过关）：F2 / F4 的 activate() 宿主级覆盖
//    （此前仅板端 B12/B14 实证；本节补齐宿主单测，防回归）
// ============================================================================
#ifndef _WIN32
#include <unistd.h>   // ::unlink（磁盘用例仅 POSIX；Windows 无 /var/lib 语义）

namespace {

// LicenseDaemon 硬编码默认 store 目录（LicenseStore 默认构造）。
constexpr const char* kQaStoreDir = "/var/lib/ttbox/license";

// 清空 store 落盘物（用例前后各调一次，不留残渣；目录缺失时无害 no-op）。
void qa_wipe_store() {
    ::unlink((std::string(kQaStoreDir) + "/license.json").c_str());
    ::unlink((std::string(kQaStoreDir) + "/state.json").c_str());
}

// 读 license.json 原文（不存在 ⇒ 空串；供"不落盘"的字节级比对）。
std::string qa_read_doc() {
    std::FILE* f = std::fopen((std::string(kQaStoreDir) + "/license.json").c_str(), "rb");
    if (f == nullptr) return {};
    std::string data;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    std::fclose(f);
    return data;
}

// 构造一张"parse 必过、验签交给脚本化客户端"的信封（issued_at 可注入）。
// signature = base64(64B 零)（内容无关——ScriptedClient 不真验签）。
std::string qa_envelope(const std::string& license_id, int64_t issued_at) {
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%lld", static_cast<long long>(issued_at));
    return std::string("{\"license\":{\"license_id\":\"") + license_id +
           "\",\"device\":\"TESTSERIAL0001\",\"plan\":\"subscription\",\"is_pro\":true,"
           "\"features\":[\"capture\",\"inference\",\"aim\",\"ota\"],\"ui_brand\":\"ttbox\","
           "\"issued_at\":" + ts + ",\"expires_at\":0}," +
           "\"key_id\":\"ttbox-license-2026a\",\"signature\":\"" + std::string(84, 'A') +
           "AA==\"}";
}

// cloud 形文档（顶层 source:"cloud"；**不是** Ed25519 信封）。字段与
// LicenseDaemon::activate_cloud() 落盘的一致（供 D-D 重启恢复回归用）。
std::string qa_cloud_doc(int64_t expire_unix_ms) {
    char exp[32];
    std::snprintf(exp, sizeof(exp), "%lld", static_cast<long long>(expire_unix_ms));
    return std::string("{\"source\":\"cloud\",\"card_mask\":\"TTB-XXXX-XXXX\","
                       "\"expire_unix_ms\":") + exp +
           ",\"features\":[\"capture\",\"inference\",\"aim\",\"ota\"],"
           "\"plan\":\"subscription\"}";
}

// 恒 kValid 客户端（req_ok=true ⇒ apply_check_result 采纳 kValid）。
class QaValidClient : public ILicenseClient {
public:
    bool verify_once(const std::string&, const std::string&,
                     LicenseStatus& out, std::string*) override {
        out = LicenseStatus{};
        out.state = LicenseState::kValid;
        out.is_pro = true;
        out.expire_unix_ms = 0;           // 永久
        out.heartbeat_interval = 60;
        return true;
    }
};

// 验签级权威拒绝客户端（bound to another device；B12 同族真因）。
class QaRejectClient : public ILicenseClient {
public:
    bool verify_once(const std::string&, const std::string&,
                     LicenseStatus& out, std::string*) override {
        out = LicenseStatus{};
        out.state = LicenseState::kBoundElsewhere;
        out.last_error = "offline card: bound to another device";
        return true;                      // 请求本身成功，服务器明确拒绝
    }
};

}  // namespace

// ---- F2 ①：解析级拒绝 ⇒ last_error 非空且指向真因 ----
TEST(activate_parse_reject_sets_last_error) {
    QaValidClient client;
    LicenseDaemon daemon(client);
    std::string err;
    CHECK(!daemon.activate("{ definitely not json", &err));
    const LicenseStatus st = daemon.status_snapshot();
    CHECK(st.state == LicenseState::kInvalidCard);
    CHECK(!st.last_error.empty());                     // F2：诊断必须可见
    CHECK(!err.empty());
}

// ---- F2 ②：验签级拒绝后，thread_loop 空卡分支在下一退避周期不得冲掉真因 ----
TEST(activate_verify_reject_reason_survives_backoff_cycle) {
    QaRejectClient client;
    LicenseDaemon daemon(client);
    daemon.set_backoff_base_ms(30);     // 空卡分支 sleep = min(base,cap)，快速过周期
    std::string err;
    CHECK(!daemon.activate(qa_envelope("lic-f2-1", 1750000000), &err));
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == LicenseState::kBoundElsewhere);
        CHECK(st.last_error.find("bound to another device") != std::string::npos);
    }
    // 起后台线程 ≥1 个退避周期：空卡分支会把 state 归一成 kInvalidCard，
    // 但 F2 要求 last_error 保留真因、不得被 "card not set" 覆盖。
    CHECK(daemon.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    daemon.stop();
    const LicenseStatus st2 = daemon.status_snapshot();
    CHECK(st2.state == LicenseState::kInvalidCard);    // 空卡分支确实跑过
    CHECK(st2.last_error.find("bound to another device") != std::string::npos);
    CHECK(st2.last_error.find("card not set") == std::string::npos);  // 未被泛化冲掉
}

// ---- F4 ②：kValid 激活 ⇒ save_doc 落盘 + save_state 基线前进 ----
TEST(activate_valid_persists_doc_and_baseline) {
    qa_wipe_store();
    QaValidClient client;
    LicenseDaemon daemon(client);
    std::string err;
    const std::string env = qa_envelope("lic-f4-a", 1750000000);
    CHECK(daemon.activate(env, &err));
    CHECK(daemon.status_snapshot().state == LicenseState::kValid);

    LicenseStore store;                 // 默认目录 = daemon 所写目录
    const StoreLoadResult lr = store.load();
    CHECK(lr.doc_ok);
    CHECK(lr.doc_json == env);                           // license.json = 信封原文
    CHECK(lr.state_ok);
    CHECK_EQ(lr.last_seen_issued_at, 1750000000LL);      // 基线 = 本次卡 issued_at
    qa_wipe_store();
}

// ---- F4 ③：更旧卡 ⇒ 拒绝 + 真的不落盘（字节级比对，非只看错误码） ----
TEST(activate_older_card_rejected_not_persisted) {
    qa_wipe_store();
    QaValidClient client;
    LicenseDaemon daemon(client);
    std::string err;
    CHECK(daemon.activate(qa_envelope("lic-f4-a", 1750000000), &err));

    const std::string doc_before = qa_read_doc();
    const int64_t baseline_before = LicenseStore().load().last_seen_issued_at;

    const std::string env_old = qa_envelope("lic-f4-old", 1750000000 - 86400);
    CHECK(!daemon.activate(env_old, &err));
    CHECK(err.find("LICENSE_DOWNGRADE_REJECTED") != std::string::npos);
    const LicenseStatus st = daemon.status_snapshot();
    CHECK(st.last_error.find("downgrade rejected") != std::string::npos);
    CHECK(st.last_error.find("1750000000") != std::string::npos);   // 消息含时间戳比较

    CHECK(qa_read_doc() == doc_before);                  // 不落盘：doc 字节不变
    CHECK_EQ(LicenseStore().load().last_seen_issued_at, baseline_before);  // 基线不变
    qa_wipe_store();
}

// ---- F4 边界：等值（重复激活幂等）与更晚换卡 ⇒ 不误判降级 ----
TEST(activate_equal_and_newer_card_not_downgrade) {
    qa_wipe_store();
    QaValidClient client;
    LicenseDaemon daemon(client);
    std::string err;
    CHECK(daemon.activate(qa_envelope("lic-f4-a", 1750000000), &err));
    // ① 同卡重复激活（issued_at 相等）⇒ 幂等放行（B9 ① 同语义）
    CHECK(daemon.activate(qa_envelope("lic-f4-a", 1750000000), &err));
    // ② 换卡：新 license_id + 更晚 issued_at ⇒ 前进放行，基线前进
    CHECK(daemon.activate(qa_envelope("lic-f4-b", 1750003600), &err));
    CHECK_EQ(LicenseStore().load().last_seen_issued_at, 1750003600LL);
    qa_wipe_store();
}

// ============================================================================
// ★ M2.07 D-D 回归锚（2026-09-17）：cloud 形文档重启恢复后，启动期的一次性验卡
//   **不得**抹掉云态。
//
//   忠实复现 Application 的真实路径：
//     ① resolve_license_card() 读回 store 的 cloud 文档（本用例直接落盘 cloud 文档）
//     ② set_card(该文档) —— 显式模拟"card_plain_ 非空"的最坏情形，以同时覆盖
//        thread_loop 的**非空分支**守卫（修 D-D 前该分支会把云态抹掉）
//     ③ start() → restore_cloud_doc_locked() 置 state=kValid、cloud_license_=true
//     ④ verify_now_blocking()（离线客户端对 cloud 文档必拒：kBoundElsewhere）
//     ⑤ 跑若干 thread_loop 周期
//   修复前：④/⑤ 会把 kValid 覆盖成 kBoundElsewhere（板端现场 = 重启后 activated 变 false）。
//   修复后：三处云态守卫短路 ⇒ 全程仍 kValid。
// ============================================================================
TEST(cloud_doc_restore_survives_verify_now_blocking) {
    qa_wipe_store();
    const std::string cloud_doc =
        qa_cloud_doc(now_unix_ms_now() + 30LL * 24 * 3600 * 1000);

    // ① 落盘 cloud 形文档（真实原子写），并断言 load() 的 doc_is_cloud（单一真源）
    {
        LicenseStore store;
        std::string werr;
        CHECK(store.save_doc(cloud_doc, &werr));
        const StoreLoadResult lr = store.load();
        CHECK(lr.doc_ok);
        CHECK(lr.doc_is_cloud);            // ★ 单一真源登记正确
    }

    // ②/③ 模拟 Application：把 cloud 文档当卡交给 daemon，再 start()
    QaRejectClient client;                  // 离线客户端对 cloud 文档必拒
    LicenseDaemon daemon(client);
    daemon.set_card(cloud_doc);             // card_plain_ 非空（最坏情形）
    CHECK(daemon.start());                  // → restore_cloud_doc_locked(): kValid
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == LicenseState::kValid);   // 云态恢复成功
        CHECK(daemon.allow_run());
    }

    // ④ 启动期一次性验卡：修复前会把云态抹成 kBoundElsewhere
    {
        std::string err;
        (void)daemon.verify_now_blocking(&err);
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == LicenseState::kValid);   // ★ D-D 回归锚（verify 守卫）
        CHECK(daemon.allow_run());
    }

    // ⑤ 给 thread_loop 至少数个周期：非空分支守卫必须阻止离线验签抹掉云态
    daemon.set_backoff_base_ms(20);         // 云态分支 sleep = min(base,cap) ⇒ 快速巡
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == LicenseState::kValid);   // ★ D-D 回归锚（thread_loop 非空分支守卫）
        CHECK(daemon.allow_run());
    }
    daemon.stop();
    qa_wipe_store();
}

// ---- D-D 反向护栏：非 cloud 文档不得被标记为 doc_is_cloud（防单一真源误判） ----
TEST(store_load_marks_offline_envelope_not_cloud) {
    qa_wipe_store();
    LicenseStore store;
    std::string werr;
    CHECK(store.save_doc(qa_envelope("lic-offline-x", 1750000000), &werr));
    const StoreLoadResult lr = store.load();
    CHECK(lr.doc_ok);
    CHECK(!lr.doc_is_cloud);                // 离线信封 ⇒ 不是 cloud 形
    qa_wipe_store();
}

// ---- M2.07.1 T3：Application::resolve_license_card() store 分支实测（真磁盘往返） ----
//   直接驱动与 Application 相同的路径：save_doc → load() → offline_card_doc()。
TEST(resolve_card_store_branch_guard_roundtrip) {
    qa_wipe_store();
    LicenseStore store;
    std::string werr;

    // ① 反向：cloud 形文档落盘 ⇒ load ⇒ 判定**不得交出**（D-D 护栏）
    CHECK(store.save_doc(qa_cloud_doc(now_unix_ms_now() + 3600LL * 1000), &werr));
    {
        const StoreLoadResult lr = store.load();
        CHECK(lr.doc_ok);
        CHECK(lr.doc_is_cloud);
        CHECK(offline_card_doc(lr).empty());     // ★ 变异点：注释掉守卫即变红
    }

    // ② 正向：离线信封落盘 ⇒ load ⇒ 正常交出
    const std::string env = qa_envelope("lic-offline-y", 1750000000);
    CHECK(store.save_doc(env, &werr));
    {
        const StoreLoadResult lr = store.load();
        CHECK(lr.doc_ok);
        CHECK(!lr.doc_is_cloud);
        CHECK(offline_card_doc(lr) == env);
    }
    qa_wipe_store();
}
// ============================================================================
// ★ F11（2026-09-17）：云激活态下，被拒的 ACTIVATE 不得污染授权态。
//   面板免密（M2.07 需求①）⇒ 局域网任何人可 POST /api/license/activate。
//   修前：parse 失败 / 降级 / 验签失败三条拒绝路径都会把云激活打掉
//   （state→kInvalidCard、cloud_license_ 丢失）⇒ 一个空 body 或坏签名卡即可关 AI。
//   契约：**调用前是云激活**时，任一被拒 ACTIVATE 后 status_ 逐字段不变、
//   allow_run() 仍 true、license.json 不落盘；**调用前非云激活**时保持既有行为
//   （把拒绝原因写进状态，F2 意图）；**合法离线卡仍必须能接管**。
// ============================================================================

// ---- F11 ①：云激活态下 parse 级 + 验签级拒绝 ⇒ 授权态逐字段不回退 ----
TEST(activate_reject_preserves_cloud_state) {
    qa_wipe_store();
    QaRejectClient client;            // 验签级拒绝（bound elsewhere）
    LicenseDaemon daemon(client);
    std::string err;

    // 云激活前置
    CHECK(daemon.activate_cloud(now_unix_ms_now() + 30LL * 24 * 3600 * 1000,
                                {"capture", "inference", "aim", "ota"},
                                "subscription", "TTB-AAAA-BBBB", &err));
    const LicenseStatus before = daemon.status_snapshot();
    CHECK(before.state == LicenseState::kValid);
    CHECK(daemon.allow_run());
    const std::string doc_before = qa_read_doc();

    // ① parse 级拒绝（空 body / 非 JSON）
    CHECK(!daemon.activate("", &err));
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == before.state);
        CHECK_EQ(st.verified_at_ms, before.verified_at_ms);
        CHECK_EQ(st.expire_unix_ms, before.expire_unix_ms);
        CHECK(st.last_error == before.last_error);
        CHECK(st.features == before.features);
    }
    CHECK(daemon.allow_run());
    CHECK(qa_read_doc() == doc_before);          // 不落盘

    // ② 验签级拒绝（badsig：结构过、客户端明确拒）
    CHECK(!daemon.activate(qa_envelope("lic-f11-bad", 1750000000), &err));
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == before.state);          // ★ 云态未被踩成 kInvalidCard
        CHECK_EQ(st.verified_at_ms, before.verified_at_ms);
        CHECK_EQ(st.expire_unix_ms, before.expire_unix_ms);
        CHECK(st.last_error == before.last_error); // message 不被写成拒绝原因
        CHECK(st.features == before.features);
    }
    CHECK(daemon.allow_run());
    CHECK(qa_read_doc() == doc_before);

    // 云态仍生效：verify_now_blocking() 走云短路（不回退到离线验签）⇒ 仍 true
    CHECK(daemon.verify_now_blocking(&err));
    CHECK(daemon.allow_run());
    qa_wipe_store();
}

// ---- F11 ②：云激活态下防降级拒绝 ⇒ 授权态不回退 ----
TEST(activate_downgrade_reject_preserves_cloud_state) {
    qa_wipe_store();
    QaValidClient client;            // 离线卡路径恒 kValid（仅用于建立基线）
    LicenseDaemon daemon(client);
    std::string err;

    // ① 先建防回滚基线（合法离线激活，issued_at=T）
    CHECK(daemon.activate(qa_envelope("lic-base", 1750000000), &err));
    CHECK_EQ(LicenseStore().load().last_seen_issued_at, 1750000000LL);

    // ② 转云激活
    CHECK(daemon.activate_cloud(now_unix_ms_now() + 30LL * 24 * 3600 * 1000,
                                {"capture", "inference", "aim", "ota"},
                                "subscription", "TTB-CCCC-DDDD", &err));
    const LicenseStatus before = daemon.status_snapshot();
    const std::string doc_before = qa_read_doc();

    // ③ 更旧卡 ⇒ 降级拒绝，且**不得**踩云态
    CHECK(!daemon.activate(qa_envelope("lic-old", 1750000000 - 86400), &err));
    CHECK(err.find("LICENSE_DOWNGRADE_REJECTED") != std::string::npos);
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == before.state);
        CHECK_EQ(st.verified_at_ms, before.verified_at_ms);
        CHECK(st.last_error == before.last_error);
    }
    CHECK(daemon.allow_run());
    CHECK(qa_read_doc() == doc_before);
    qa_wipe_store();
}

// ---- F11 反向护栏 ①：非云激活态下的拒绝，既有"写拒绝原因"行为不得丢（F2 意图）----
TEST(activate_reject_when_not_cloud_keeps_diagnostic_state) {
    qa_wipe_store();
    QaRejectClient client;
    LicenseDaemon daemon(client);
    std::string err;

    // 验签级拒绝（未激活基线）：state 落为拒绝态 + last_error 可见
    CHECK(!daemon.activate(qa_envelope("lic-f11-x", 1750000000), &err));
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == LicenseState::kBoundElsewhere);
        CHECK(st.last_error.find("bound to another device") != std::string::npos);
    }
    CHECK(!daemon.allow_run());

    // parse 级拒绝：state=kInvalidCard + last_error 非空
    CHECK(!daemon.activate("{ not json", &err));
    {
        const LicenseStatus st = daemon.status_snapshot();
        CHECK(st.state == LicenseState::kInvalidCard);
        CHECK(!st.last_error.empty());
    }
    qa_wipe_store();
}

// ---- F11 反向护栏 ②：合法离线卡仍必须能**接管**云态（cloud 关闭 + 落盘离线信封）----
TEST(activate_valid_offline_card_takes_over_cloud) {
    qa_wipe_store();
    QaValidClient client;
    LicenseDaemon daemon(client);
    std::string err;

    CHECK(daemon.activate_cloud(now_unix_ms_now() + 30LL * 24 * 3600 * 1000,
                                {"capture", "inference", "aim", "ota"},
                                "subscription", "TTB-EEEE-FFFF", &err));
    CHECK(daemon.allow_run());

    const std::string env = qa_envelope("lic-takeover", 1750000000);
    CHECK(daemon.activate(env, &err));                 // 合法离线卡 ⇒ 接管
    CHECK(daemon.status_snapshot().state == LicenseState::kValid);
    {
        const StoreLoadResult lr = LicenseStore().load();
        CHECK(lr.doc_ok);
        CHECK(lr.doc_json == env);                     // 落盘换成离线信封
        CHECK(!lr.doc_is_cloud);                       // 已非云态
    }

    // 接管后 thread_loop 走离线卡路径，仍 kValid（cloud 已关，不再有云短路）
    daemon.set_backoff_base_ms(20);
    CHECK(daemon.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    daemon.stop();
    CHECK(daemon.allow_run());
    qa_wipe_store();
}

#endif  // !_WIN32
