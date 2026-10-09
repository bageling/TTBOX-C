// HidPackageRegistry.hpp — A9-P2 HID Package 注册表
//
// HID Package 独立版本管理（不与 AI Runtime 写死到系统目录）。
// 目录结构（root = <project>/hid，可用 options 覆盖）：
//   hid/
//   ├── registry/          # active.json / previous.json（双版本回滚）
//   ├── packages/<ver>/    # 每个版本：manifest.json + bin/ + config/ + descriptors/ + validation/
//   ├── staging/           # 待校验包
//   ├── quarantine/        # 校验/激活失败包
//   ├── config/            # 独立配置 hid_config.json
//   └── descriptors/       # HID 描述符（keyboard.desc / mouse.desc）
//
// 支持：list / install / validate / activate / deactivate / remove / rollback / get_active
// 规则：禁止删除 active；激活失败自动恢复 previous。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "hid/HidPackageManifest.hpp"

namespace ttbox::core {

struct HidPackageRegistryOptions {
    std::string root = "";   // hid/ 根目录（空 = TTBOX_PROJECT_ROOT/hid）
    bool create_dirs = true;
};

class HidPackageRegistry {
public:
    explicit HidPackageRegistry(HidPackageRegistryOptions opts = {});

    bool init(std::string* error = nullptr);

    // 校验回调：校验 staging/installed 包。签名：
    //   bool(const std::string& package_dir, const HidPackageManifest&, std::string* error)
    // 生产环境 = 检查结构完整性 + 可选 health check。
    void set_validator(std::function<bool(const std::string&, const HidPackageManifest&, std::string*)> v) {
        validator_ = std::move(v);
    }

    // ---- 操作 ----
    // install：把 staging/<ver> 校验通过后移入 packages/<ver>
    bool install(const std::string& version, std::string* error = nullptr);

    // validate：校验 staging/<ver>（写 validation/ok.json）
    bool validate(const std::string& version, std::string* error = nullptr);

    // activate：激活指定版本（先记录 previous=当前 active，再校验新版本；
    // 校验失败 → quarantine + 恢复 previous）。返回是否成功。
    bool activate(const std::string& version, std::string* error = nullptr);

    // deactivate：停用（记录 previous，清空 active）
    bool deactivate(std::string* error = nullptr);

    // remove：删除指定版本（active 版本禁止删除）
    bool remove(const std::string& version, std::string* error = nullptr);

    // rollback：回滚到 previous 版本（激活 previous；失败保持现状）
    bool rollback(std::string* error = nullptr);

    // 当前激活版本（无返回空串）
    std::string get_active() const;
    // 回滚目标（previous 版本）
    std::string get_previous() const;

    // list：所有已安装版本 manifest
    std::vector<HidPackageManifest> list() const;

    // ---- 路径辅助 ----
    std::string root_dir() const { return root_; }
    std::string package_dir(const std::string& version) const;
    std::string staging_dir(const std::string& version) const;
    std::string quarantine_dir(const std::string& version) const;

private:
    // 创建注册表所需的全部子目录
    bool ensure_dirs(std::string* error);
    // 写 registry/active.json（当前激活版本）
    bool write_active(const std::string& version, std::string* error);
    // 写 registry/previous.json（回滚目标版本）
    bool write_previous(const std::string& version, std::string* error);
    // 读 JSON 文件中的字符串字段（缺/类型不符返回空串）
    std::string read_json_str(const std::string& file, const char* key) const;
    // 路径是否存在
    bool exists(const std::string& path) const;
    // 递归复制目录
    bool copy_dir(const std::string& src, const std::string& dst, std::string* error) const;

    std::string root_;
    HidPackageRegistryOptions opts_;
    std::function<bool(const std::string&, const HidPackageManifest&, std::string*)> validator_;
};

}  // namespace ttbox::core
