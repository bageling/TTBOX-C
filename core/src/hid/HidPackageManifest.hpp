// HidPackageManifest.hpp — A9-P2 HID Package Manifest（版本管理）
//
// HID Package 独立于 AI Runtime 进行版本管理/安装/升级/回滚。
// manifest 字段对齐用户要求，sha256/signature 等安全字段预留（开发版不伪造）。
#pragma once

#include <cstdint>
#include <string>

#include "common/Json.hpp"

namespace ttbox::core {

// HID Package 状态（生命周期）
enum class HidPackageStatus : int {
    kUnknown = 0,
    kStaging = 1,     // 上传/校验中
    kInstalled = 2,   // 已安装（未激活）
    kActive = 3,      // 当前激活
    kInactive = 4,    // 已停用
    kQuarantined = 5, // 校验/激活失败
    kRollback = 6,    // 回滚中/已回滚
};

const char* hid_package_status_name(HidPackageStatus s);

// HID Package Manifest
struct HidPackageManifest {
    // 基础
    std::string package_id = "ttbox-hid";
    std::string version = "0.0.1";
    std::string status_name = "development";   // development / stable / release
    std::string architecture = "aarch64";
    std::string hid_protocol_version = "1";
    // 兼容性（Runtime / Kernel ABI）
    std::string min_runtime_version = "0.0.1";
    std::string max_runtime_version;           // 空 = 无上限
    std::string kernel_abi = "6.1-rockchip";
    // 安全字段（预留；开发版不伪造，允许为空）
    std::string sha256;                        // 包内容完整性
    std::string signature;                     // 签名（预留）
    std::string signing_key_id;                // 签名密钥标识（预留）
    std::string origin = "local";              // local / cloud:<id>
    std::string release_channel = "development";
    // 版本状态
    std::string rollback_version;              // 可回滚到的版本（空 = 无）
    HidPackageStatus status = HidPackageStatus::kInstalled;
    int64_t created_at = 0;                    // epoch ms

    // 序列化为 JSON
    JsonValue to_json() const;
    // 从 JSON 构造（缺字段用默认值）
    static HidPackageManifest from_json(const JsonValue& v);
};

// VERSION 文件读写（纯文本 "0.0.1\n"）
bool hid_read_version(const std::string& version_file, std::string* out);
bool hid_write_version(const std::string& version_file, const std::string& version);

}  // namespace ttbox::core
