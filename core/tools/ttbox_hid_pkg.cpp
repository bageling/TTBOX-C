// ttbox_hid_pkg.cpp — A9-P2 HID Package Registry CLI
//
// 用法：
//   ttbox-hid-pkg [--root <hid_root>] <cmd> [args]
//   cmd: init | import <src_dir> <version> | validate <version> | install <version>
//        | activate <version> | deactivate | remove <version> | rollback
//        | list | get-active | health
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hid/HidPackageManifest.hpp"
#include "hid/HidPackageRegistry.hpp"
#include "hid/IHidPackageSource.hpp"

#ifndef TTBOX_PROJECT_ROOT
#error "TTBOX_PROJECT_ROOT must be injected by CMake (-DTTBOX_PROJECT_ROOT); refuse silent fallback to '.'."
#endif

using namespace ttbox::core;

namespace {

// 默认 validator：检查包结构完整性（VERSION + manifest 可解析）
bool pkg_validator(const std::string& dir, const HidPackageManifest& m,
                   std::string* error) {
    if (m.version.empty()) {
        if (error) *error = "manifest 无版本";
        return false;
    }
    return true;
}

int usage() {
    std::printf(
        "usage: ttbox-hid-pkg [--root <hid_root>] <cmd> [args]\n"
        "  init                         创建 registry 目录\n"
        "  import <src_dir> <ver>       本地包源 → staging\n"
        "  validate <ver>               校验 staging 包\n"
        "  install <ver>                staging → installed\n"
        "  activate <ver>               激活版本（失败自动回滚 previous）\n"
        "  deactivate                   停用\n"
        "  remove <ver>                 删除（active 禁止）\n"
        "  rollback                     回滚到 previous\n"
        "  list                         列出已安装版本\n"
        "  get-active                   当前激活版本\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string root;
    int i = 1;
    if (i < argc && std::strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
        root = argv[i + 1];
        i += 2;
    }
    if (root.empty()) root = std::string(TTBOX_PROJECT_ROOT) + "/hid";
    if (i >= argc) return usage();

    HidPackageRegistry reg(HidPackageRegistryOptions{root});
    reg.set_validator(pkg_validator);
    std::string err;
    const std::string cmd = argv[i++];

    if (cmd == "init") {
        return reg.init(&err) ? 0 : (std::printf("[FAIL] %s\n", err.c_str()), 1);
    }
    if (cmd == "get-active") {
        std::printf("%s\n", reg.get_active().c_str());
        return 0;
    }
    if (cmd == "list") {
        const auto pkgs = reg.list();
        std::printf("已安装 %zu 个 HID Package：\n", pkgs.size());
        for (const auto& p : pkgs) {
            std::printf("  %s  status=%s rollback=%s\n", p.version.c_str(),
                        hid_package_status_name(p.status), p.rollback_version.c_str());
        }
        return 0;
    }
    if (cmd == "import") {
        if (i + 1 >= argc) return usage();
        const std::string src = argv[i++];
        const std::string ver = argv[i++];
        LocalHidPackageSource src_pkg(src);
        HidPackageManifest m;
        if (!reg.init(&err)) { std::printf("[FAIL] init: %s\n", err.c_str()); return 1; }
        if (!src_pkg.fetch_to(reg.staging_dir(ver), &m, &err)) {
            std::printf("[FAIL] import %s: %s\n", ver.c_str(), err.c_str());
            return 1;
        }
        std::printf("[OK] import %s → staging（manifest 版本 %s）\n", ver.c_str(),
                    m.version.c_str());
        return 0;
    }
    if (cmd == "validate" || cmd == "install" || cmd == "activate" ||
        cmd == "remove") {
        if (i >= argc) return usage();
        const std::string ver = argv[i++];
        bool ok = false;
        if (cmd == "validate") ok = reg.validate(ver, &err);
        else if (cmd == "install") ok = reg.install(ver, &err);
        else if (cmd == "activate") ok = reg.activate(ver, &err);
        else ok = reg.remove(ver, &err);
        if (!ok) { std::printf("[FAIL] %s %s: %s\n", cmd.c_str(), ver.c_str(), err.c_str()); return 1; }
        std::printf("[OK] %s %s\n", cmd.c_str(), ver.c_str());
        return 0;
    }
    if (cmd == "deactivate") {
        if (!reg.deactivate(&err)) { std::printf("[FAIL] %s\n", err.c_str()); return 1; }
        std::printf("[OK] deactivate\n");
        return 0;
    }
    if (cmd == "rollback") {
        if (!reg.rollback(&err)) { std::printf("[FAIL] rollback: %s\n", err.c_str()); return 1; }
        std::printf("[OK] rollback → active=%s\n", reg.get_active().c_str());
        return 0;
    }
    std::printf("未知命令: %s\n", cmd.c_str());
    return usage();
}
