// test_license_store.cpp — T1.09 单元测试（施工图：t1.09-t1.10-impl-spec.md §4.1）
//
// 覆盖：原子写回环 / 掉电残留自愈 / state 损坏从 doc 重建基线 / 防降级纯逻辑 /
//       空目录=未激活 / POSIX 0600 权限。
// 框架：ttbox_test（TEST/CHECK，禁裸 assert —— NDEBUG 下 assert 被抹除 = 失效静默）。
// 不依赖 OpenSSL / 网络；仅文件 I-O，host 可跑。本 TU 编入 ttbox_core_tests（test_main.cpp 提供 main）。
#include "test_util.hpp"

#include "auth/LicenseStore.hpp"

#include <sys/stat.h>

#if defined(_WIN32)
#include <process.h>  // _getpid：夹具唯一化需真实 PID（跨进程可重入）
#else
#include <unistd.h>
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using ttbox::core::auth::LicenseStore;
using ttbox::core::auth::StoreLoadResult;

namespace {

namespace fs = std::filesystem;

long getpid_like() {
#if defined(_WIN32)
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

// 每个用例独立的临时目录（避免用例间串扰；**且跨进程唯一**）。
// ★ 夹具可重入化：此前仅用进程内 seq ⇒ 并发实例共用 /tmp/ttbox_lstore_<tag>_1，
//   两实例互踩（判据数字被污染）。加入真实 PID 使多实例目录互不可见。
std::string make_temp_dir(const char* tag) {
    static int seq = 0;
    const fs::path base = fs::temp_directory_path();
    const fs::path p = base / (std::string("ttbox_lstore_") + tag + "_" +
                               std::to_string(getpid_like()) + "_" +
                               std::to_string(++seq));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p.string();
}

void write_raw(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::string join(const std::string& dir, const char* name) {
    return (fs::path(dir) / name).string();
}

}  // namespace

// 原子写 → load 回读逐字节相等（文档原文 + 派生状态四项）。
TEST(store_atomic_write_roundtrip) {
    const std::string dir = make_temp_dir("rt");
    LicenseStore store(dir);
    const std::string doc = R"({"issued_at":1700000000000,"license_id":"lic_abc"})";
    std::string err;

    CHECK(store.save_doc(doc, &err));
    CHECK(store.save_state(1700000000000LL, 1699999999000LL, 1700003600000LL,
                           1700000600000LL, &err));

    const StoreLoadResult lr = store.load();
    CHECK(lr.doc_ok);
    CHECK(lr.state_ok);
    CHECK(!lr.recovered_fresh);
    CHECK(lr.doc_json == doc);                          // 逐字节相等（非解析等价）
    CHECK_EQ(lr.last_seen_issued_at, 1700000000000LL);
    CHECK_EQ(lr.server_time_floor_unix, 1699999999000LL);
    CHECK_EQ(lr.grant_expires_at, 1700003600000LL);
    CHECK_EQ(lr.grace_until, 1700000600000LL);
}

// 预置旧 state.json + 手工造半截 *.tmp.* → load 仍读旧值且删除 stale tmp。
// （验证：崩溃残留不被采信；旧文件不被半截 tmp 破坏。）
TEST(store_crash_mid_write_keeps_old) {
    const std::string dir = make_temp_dir("crash");
    LicenseStore store(dir);
    std::string err;
    // 旧的可信状态
    CHECK(store.save_state(111LL, 222LL, 333LL, 444LL, &err));

    // 手工造"上次崩溃残留"的半截 tmp
    const std::string stale = join(dir, "state.json.tmp.999999");
    write_raw(stale, "{\"last_seen_issued_at\":");
    CHECK(file_exists(stale));

    const StoreLoadResult lr = store.load();
    CHECK(lr.state_ok);                                  // 旧 state 完好
    CHECK_EQ(lr.last_seen_issued_at, 111LL);             // 读的是旧值，不是半截 tmp
    CHECK_EQ(lr.server_time_floor_unix, 222LL);
    CHECK(!file_exists(stale));                          // stale tmp 已被删除
}

// state.json 损坏（截断 JSON）→ recovered_fresh==true 且 last_seen_issued_at 从
// license.json 的 issued_at 重建（防降级：破坏 state 不能回放旧文档）。
TEST(store_corrupt_state_recovers_from_doc) {
    const std::string dir = make_temp_dir("recover");
    LicenseStore store(dir);
    std::string err;
    const std::string doc = R"({"issued_at":1700000000000,"license_id":"lic_x"})";
    CHECK(store.save_doc(doc, &err));

    // 写坏 state.json（截断 JSON）
    write_raw(join(dir, "state.json"), "{\"last_seen_issued_at\":");

    const StoreLoadResult lr = store.load();
    CHECK(!lr.state_ok);                                 // 损坏 ⇒ 不信任
    CHECK(lr.recovered_fresh);                           // 走自愈
    CHECK_EQ(lr.last_seen_issued_at, 1700000000000LL);   // 基线由 doc.issued_at 重建
}

// 防降级纯逻辑：同 license_id 下 issued_at 回退 = 降级。
TEST(store_downgrade_rejected) {
    CHECK(!LicenseStore::is_downgrade(/*doc=*/2000LL, /*last_seen=*/1000LL)); // 前进：非降级
    CHECK(LicenseStore::is_downgrade(/*doc=*/1000LL, /*last_seen=*/2000LL));  // 回退：降级
    CHECK(!LicenseStore::is_downgrade(1500LL, 1500LL));                       // 相等：非降级
}

// ---- ★ M2 QA 追加（2026-09-17 · F4 ①）：load() 读**嵌套** license.issued_at ----
//   M2 卡的 issued_at 在 license 对象内；只读顶层 ⇒ 基线恒 0 ⇒ 反降级形同虚设（F4 原缺陷）。
//   覆盖：① M2 嵌套形（state 损坏自愈路径取嵌套值）；② M1 顶层形兜底（回退兼容）；
//         ③ 两者并存 ⇒ 嵌套优先（权威）。
TEST(store_load_reads_nested_m2_issued_at) {
    {
        const std::string dir = make_temp_dir("m2nest");
        LicenseStore store(dir);
        std::string err;
        const std::string doc =
            R"({"license":{"license_id":"lic-m2","issued_at":1750000000},)"
            R"("key_id":"k","signature":"AA=="})";
        CHECK(store.save_doc(doc, &err));
        write_raw(join(dir, "state.json"), "{\"last_seen_issued_at\":");  // 损坏 ⇒ 自愈重建
        const StoreLoadResult lr = store.load();
        CHECK(lr.recovered_fresh);
        CHECK_EQ(lr.last_seen_issued_at, 1750000000LL);   // 取嵌套 license.issued_at
    }
    {
        const std::string dir = make_temp_dir("m1top");
        LicenseStore store(dir);
        std::string err;
        const std::string doc = R"({"issued_at":1700000000,"license_id":"lic_m1"})";
        CHECK(store.save_doc(doc, &err));
        write_raw(join(dir, "state.json"), "{\"last_seen_issued_at\":");
        const StoreLoadResult lr = store.load();
        CHECK(lr.recovered_fresh);
        CHECK_EQ(lr.last_seen_issued_at, 1700000000LL);   // M1 顶层兜底仍兼容
    }
    {
        const std::string dir = make_temp_dir("both");
        LicenseStore store(dir);
        std::string err;
        const std::string doc =
            R"({"issued_at":1111111111,"license":{"issued_at":2222222222}})";
        CHECK(store.save_doc(doc, &err));
        write_raw(join(dir, "state.json"), "{\"last_seen_issued_at\":");
        const StoreLoadResult lr = store.load();
        CHECK(lr.recovered_fresh);
        CHECK_EQ(lr.last_seen_issued_at, 2222222222LL);   // 嵌套优先（M2 权威）
    }
}

// 空目录 ⇒ 无文档、无状态 = 未激活（A23 的存储侧前提）。
TEST(store_missing_doc_is_unactivated) {
    const std::string dir = make_temp_dir("empty");
    LicenseStore store(dir);
    const StoreLoadResult lr = store.load();
    CHECK(!lr.doc_ok);
    CHECK(!lr.state_ok);
    CHECK(!lr.recovered_fresh);
    CHECK(lr.doc_json.empty());
    CHECK_EQ(lr.last_seen_issued_at, 0LL);
}

// POSIX：写后文件权限 0600（卡片文档为敏感数据）。
// ★ 平台口径（诚实计数）：Windows/MinGW 无 POSIX 权限模型（chmod 不产生 0600 语义，
//   stat.st_mode 恒为 0666）⇒ 本用例**仅在 POSIX 断言**；非 POSIX 显式 TEST_SKIP
//   （计入 skipped、**不计入 passed** —— 禁止"零断言静默 PASS"这一被反复点名的反模式）。
TEST(store_perm_0600) {
#ifdef _WIN32
    TEST_SKIP("POSIX 权限模型专属：Windows 无 0600 语义（本用例只在 POSIX 断言）");
#else
    const std::string dir = make_temp_dir("perm");
    LicenseStore store(dir);
    std::string err;
    CHECK(store.save_doc(R"({"issued_at":1})", &err));
    CHECK(store.save_state(1LL, 1LL, 1LL, 1LL, &err));

    struct stat st_doc {};
    CHECK(::stat(join(dir, "license.json").c_str(), &st_doc) == 0);
    CHECK_EQ(static_cast<int>(st_doc.st_mode & 0777), 0600);

    struct stat st_state {};
    CHECK(::stat(join(dir, "state.json").c_str(), &st_state) == 0);
    CHECK_EQ(static_cast<int>(st_state.st_mode & 0777), 0600);
#endif
}

// M2.07：cloud 形 license.json（source:"cloud" + card_mask + issued_at_ms 等新字段）
// load 必须宽容（doc_ok=true、原文逐字节保留）；未知字段不破坏既有基线语义。
TEST(store_cloud_doc_tolerated) {
    const std::string dir = make_temp_dir("cloud");
    LicenseStore store(dir);
    std::string err;
    const std::string cloud_doc =
        R"({"state":"valid","issued_at_ms":1760000000000,"expire_unix_ms":1760707200000,)"
        R"("features":["capture","inference","aim","ota"],"plan":"subscription",)"
        R"("ui_brand":"ttbox","card_mask":"LS-TT****M2X7","source":"cloud","max_devices":3})";
    CHECK(store.save_doc(cloud_doc, &err));

    const StoreLoadResult lr = store.load();
    CHECK(lr.doc_ok);
    CHECK(lr.doc_json == cloud_doc);  // 原文保留（不解析为授权语义，交给验证层）
    // cloud doc 无顶层/嵌套 issued_at ⇒ 基线为 0（cloud 激活不参与防降级，契约如此）
    CHECK_EQ(lr.last_seen_issued_at, 0LL);
}
