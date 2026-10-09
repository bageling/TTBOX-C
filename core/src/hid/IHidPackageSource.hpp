// IHidPackageSource.hpp — A9-P2 HID Package 来源抽象（云端更新接口预留）
//
// 当前实现 LocalHidPackageSource；CloudHidPackageSource 仅接口占位（不连真实云端）。
// 未来云端链路：检查版本 → 下载 Package → SHA256 → 签名 → 兼容性 → staging → activate → health → commit
#pragma once

#include <string>

#include "hid/HidPackageManifest.hpp"

namespace ttbox::core {

// HID Package 来源接口
class IHidPackageSource {
public:
    virtual ~IHidPackageSource() = default;

    // 来源标识（"local:<path>" / "cloud:<pkg>"）
    virtual std::string source_id() const = 0;

    // 获取包到 staging 目录（dest_dir 应为 <hid_root>/staging/<version>）。
    // 成功返回 true，并填充实际清单。
    virtual bool fetch_to(const std::string& dest_dir, HidPackageManifest* manifest,
                          std::string* error = nullptr) = 0;
};

// 本地包源：从本地目录（已含 manifest.json/bin/config/descriptors）导入 staging
class LocalHidPackageSource : public IHidPackageSource {
public:
    explicit LocalHidPackageSource(std::string package_dir) : dir_(std::move(package_dir)) {}
    std::string source_id() const override { return "local:" + dir_; }
    bool fetch_to(const std::string& dest_dir, HidPackageManifest* manifest,
                  std::string* error = nullptr) override;

private:
    std::string dir_;
};

// 云端包源（预留：A9-P2 不连接真实云端）
class CloudHidPackageSource : public IHidPackageSource {
public:
    explicit CloudHidPackageSource(std::string package_id) : pkg_(std::move(package_id)) {}
    std::string source_id() const override { return "cloud:" + pkg_; }
    bool fetch_to(const std::string& /*dest*/, HidPackageManifest* /*m*/,
                  std::string* error = nullptr) override {
        if (error) *error = "CloudHidPackageSource 未实现（A9-P2 仅接口占位）";
        return false;
    }

private:
    std::string pkg_;
};

}  // namespace ttbox::core
