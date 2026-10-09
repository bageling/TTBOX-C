// test_hid_package.cpp — A9-P2 单元测试：HID Package（manifest/registry/config/source/回滚）
#include "test_util.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "hid/HidPackageConfig.hpp"
#include "hid/HidPackageManifest.hpp"
#include "hid/HidPackageRegistry.hpp"
#include "hid/IHidPackageSource.hpp"

namespace fs = std::filesystem;
using namespace ttbox::core;

namespace {

std::string g_root;

std::string unique_root() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/tmp/ttbox_hid_pkg_%d", static_cast<int>(::getpid()));
    return buf;
}

// 夹具可重入化：包源目录并入 PID 唯一前缀，避免并发测试实例共用同一 /tmp/pkg_src_* 互踩。
std::string unique_src_base() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/tmp/ttbox_hid_src_%d", static_cast<int>(::getpid()));
    return buf;
}

// 构造一个最小包目录（manifest + VERSION + config），返回其目录
std::string make_pkg_dir(const std::string& base, const std::string& version) {
    const std::string dir = base + "/pkg_src_" + version;
    fs::create_directories(dir + "/config");
    fs::create_directories(dir + "/descriptors");
    HidPackageManifest m;
    m.version = version;
    m.status = HidPackageStatus::kInstalled;
    std::ofstream(dir + "/manifest.json", std::ios::trunc) << m.to_json().dump();
    std::ofstream(dir + "/VERSION", std::ios::trunc) << version << "\n";
    std::ofstream(dir + "/config/hid_config.json", std::ios::trunc)
        << "{\"device\":{\"report_rate_hz\":500}}\n";
    return dir;
}

// fake validator：目录含 VERSION 且 manifest 可解析即通过
bool fake_validator(const std::string& pkg_dir, const HidPackageManifest& m,
                    std::string* error) {
    if (!fs::exists(pkg_dir + "/VERSION")) {
        if (error) *error = "缺少 VERSION";
        return false;
    }
    if (m.version.empty()) {
        if (error) *error = "manifest 版本为空";
        return false;
    }
    return true;
}

struct Fix {
    std::string root;
    HidPackageRegistry reg;
    explicit Fix() : root(unique_root()), reg(HidPackageRegistryOptions{root}) {
        std::string err;
        reg.init(&err);
        reg.set_validator(fake_validator);
    }
    ~Fix() { std::error_code ec; fs::remove_all(root, ec); }
};

}  // namespace

TEST(hid_manifest_fields) {
    HidPackageManifest m;
    CHECK(m.package_id == "ttbox-hid");
    CHECK(m.version == "0.0.1");
    CHECK(m.architecture == "aarch64");
    CHECK(m.kernel_abi == "6.1-rockchip");
    CHECK(m.sha256.empty());      // 预留不伪造
    CHECK(m.signature.empty());   // 预留不伪造
    CHECK(m.signing_key_id.empty());
    CHECK(m.origin == "local");
    CHECK(m.release_channel == "development");
    CHECK(m.rollback_version.empty());
    // JSON 往返（含安全字段）
    m.sha256 = "abc";
    m.signature = "sig";
    m.rollback_version = "0.0.0";
    const auto j = m.to_json();
    auto res = json_parse(j.dump());
    CHECK(res.ok);
    if (res.ok) {
        HidPackageManifest n = HidPackageManifest::from_json(res.value);
        CHECK(n.sha256 == "abc");
        CHECK(n.signature == "sig");
        CHECK(n.rollback_version == "0.0.0");
        CHECK(n.package_id == "ttbox-hid");
    }
}

TEST(hid_version_file) {
    const std::string f = "/tmp/hid_ver_test_" + std::to_string(static_cast<int>(::getpid())) + ".txt";
    CHECK(hid_write_version(f, "0.0.1"));
    std::string v;
    CHECK(hid_read_version(f, &v));
    CHECK(v == "0.0.1");
    std::remove(f.c_str());
}

TEST(hid_package_registry_lifecycle) {
    Fix fx;
    // 构造 0.0.1 包源 → LocalHidPackageSource → staging → validate → install → activate
    const std::string src = make_pkg_dir(unique_src_base(), "0.0.1");
    LocalHidPackageSource lsrc(src);
    HidPackageManifest m;
    std::string err;
    CHECK(lsrc.fetch_to(fx.reg.staging_dir("0.0.1"), &m, &err));
    CHECK(m.version == "0.0.1");
    CHECK(fx.reg.validate("0.0.1", &err));
    CHECK(fx.reg.install("0.0.1", &err));
    CHECK(fx.reg.activate("0.0.1", &err));
    CHECK(fx.reg.get_active() == "0.0.1");
    // 双版本：再装 0.0.0（回滚目标）
    const std::string src0 = make_pkg_dir(unique_src_base(), "0.0.0");
    LocalHidPackageSource lsrc0(src0);
    CHECK(lsrc0.fetch_to(fx.reg.staging_dir("0.0.0"), &m, &err));
    CHECK(fx.reg.validate("0.0.0", &err));
    CHECK(fx.reg.install("0.0.0", &err));
    // 激活 0.0.0 → previous 应为 0.0.1
    CHECK(fx.reg.activate("0.0.0", &err));
    CHECK(fx.reg.get_active() == "0.0.0");
    CHECK(fx.reg.get_previous() == "0.0.1");
    // rollback → 恢复 0.0.1
    CHECK(fx.reg.rollback(&err));
    CHECK(fx.reg.get_active() == "0.0.1");
    CHECK(fx.reg.get_previous() == "0.0.0");
    // list
    const auto pkgs = fx.reg.list();
    CHECK_EQ(pkgs.size(), 2u);
    // 禁止删除 active
    CHECK(!fx.reg.remove("0.0.1", &err));
    CHECK(!err.empty());
    // deactivate 后可删除
    CHECK(fx.reg.deactivate(&err));
    CHECK(fx.reg.get_active().empty());
    CHECK(fx.reg.remove("0.0.1", &err));
    CHECK_EQ(fx.reg.list().size(), 1u);
    std::error_code ec;
    fs::remove_all(unique_src_base() + "/pkg_src_0.0.1", ec);
    fs::remove_all(unique_src_base() + "/pkg_src_0.0.0", ec);
}

TEST(hid_package_activate_failure_rollback) {
    Fix fx;
    // 0.0.1 正常
    const std::string src1 = make_pkg_dir(unique_src_base(), "0.0.1");
    LocalHidPackageSource s1(src1);
    HidPackageManifest m;
    std::string err;
    CHECK(s1.fetch_to(fx.reg.staging_dir("0.0.1"), &m, &err));
    CHECK(fx.reg.validate("0.0.1", &err));
    CHECK(fx.reg.install("0.0.1", &err));
    CHECK(fx.reg.activate("0.0.1", &err));

    // 0.0.2 坏包：安装时 validator 通过，激活时失败（缺少 VERSION）
    const std::string src2 = make_pkg_dir(unique_src_base(), "0.0.2");
    fs::remove((src2 + "/VERSION").c_str());  // 破坏包
    LocalHidPackageSource s2(src2);
    CHECK(s2.fetch_to(fx.reg.staging_dir("0.0.2"), &m, &err));
    // staging validate 也失败（缺 VERSION）→ 需绕过：改用直接构造 staging（含 validation/ok.json）
    // 简化：直接复制 + 写 ok.json 模拟"安装时校验通过"
    std::error_code ec;
    fs::copy(src2, fx.reg.staging_dir("0.0.2"), fs::copy_options::recursive, ec);
    fs::create_directories(fx.reg.staging_dir("0.0.2") + "/validation", ec);
    std::ofstream(fx.reg.staging_dir("0.0.2") + "/validation/ok.json", std::ios::trunc)
        << "{\"ok\":true}\n";
    CHECK(fx.reg.install("0.0.2", &err));
    // 激活 0.0.2 → validator 失败 → quarantine + 恢复 0.0.1
    CHECK(!fx.reg.activate("0.0.2", &err));
    CHECK(fx.reg.get_active() == "0.0.1");
    CHECK(fs::exists(fx.reg.quarantine_dir("0.0.2")));
    CHECK(!fs::exists(fx.reg.package_dir("0.0.2")));
    // 仍有可用包（不出现"无 HID Package"）
    CHECK(fx.reg.get_active() == "0.0.1");
    fs::remove_all(unique_src_base() + "/pkg_src_0.0.1", ec);
    fs::remove_all(unique_src_base() + "/pkg_src_0.0.2", ec);
}

TEST(hid_package_config_independent) {
    // 独立配置（不写 default.json）
    HidPackageConfig c;
    c.report_rate_hz = 1000;
    c.cpu_affinity = 3;
    const std::string path =
        "/tmp/hid_cfg_test_" + std::to_string(static_cast<int>(::getpid())) + ".json";
    std::string err;
    CHECK(c.save(path, &err));
    HidPackageConfig n = HidPackageConfig::load(path, &err);
    CHECK_EQ(n.report_rate_hz, 1000);
    CHECK_EQ(n.cpu_affinity, 3);
    CHECK(n.gadget_name == "ttbox-hid");
    std::remove(path.c_str());
}

TEST(hid_cloud_source_placeholder) {
    CloudHidPackageSource cloud("ttbox-hid");
    CHECK(cloud.source_id() == "cloud:ttbox-hid");
    std::string err;
    CHECK(!cloud.fetch_to("/tmp/x", nullptr, &err));  // 预留：未实现
    CHECK(!err.empty());
}
