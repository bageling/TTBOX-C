// HidPackageManifest.cpp — HID Package Manifest 实现
#include "hid/HidPackageManifest.hpp"

#include <chrono>
#include <fstream>

namespace ttbox::core {

// 包状态枚举转字符串
const char* hid_package_status_name(HidPackageStatus s) {
    switch (s) {
        case HidPackageStatus::kStaging: return "staging";
        case HidPackageStatus::kInstalled: return "installed";
        case HidPackageStatus::kActive: return "active";
        case HidPackageStatus::kInactive: return "inactive";
        case HidPackageStatus::kQuarantined: return "quarantined";
        case HidPackageStatus::kRollback: return "rollback";
        default: return "unknown";
    }
}

// 序列化 manifest 为 JSON
JsonValue HidPackageManifest::to_json() const {
    JsonValue root = JsonValue::object();
    root.set("package_id", JsonValue::string(package_id));
    root.set("version", JsonValue::string(version));
    root.set("status_name", JsonValue::string(status_name));
    root.set("architecture", JsonValue::string(architecture));
    root.set("hid_protocol_version", JsonValue::string(hid_protocol_version));
    root.set("min_runtime_version", JsonValue::string(min_runtime_version));
    root.set("max_runtime_version", JsonValue::string(max_runtime_version));
    root.set("kernel_abi", JsonValue::string(kernel_abi));
    root.set("sha256", JsonValue::string(sha256));
    root.set("signature", JsonValue::string(signature));
    root.set("signing_key_id", JsonValue::string(signing_key_id));
    root.set("origin", JsonValue::string(origin));
    root.set("release_channel", JsonValue::string(release_channel));
    root.set("rollback_version", JsonValue::string(rollback_version));
    root.set("status", JsonValue::number(static_cast<double>(static_cast<int>(status))));
    root.set("status_name_full", JsonValue::string(hid_package_status_name(status)));
    root.set("created_at", JsonValue::number(static_cast<double>(created_at)));
    return root;
}

// 从 JSON 反序列化 manifest（缺字段用默认值）
HidPackageManifest HidPackageManifest::from_json(const JsonValue& v) {
    HidPackageManifest m;
    if (!v.is_object()) return m;
    auto get = [&v](const char* key, const char* def) -> std::string {
        const JsonValue* p = v.find(key);
        return (p && p->is_string()) ? p->as_string(def) : def;
    };
    m.package_id = get("package_id", "ttbox-hid");
    m.version = get("version", "0.0.1");
    m.status_name = get("status_name", "development");
    m.architecture = get("architecture", "aarch64");
    m.hid_protocol_version = get("hid_protocol_version", "1");
    m.min_runtime_version = get("min_runtime_version", "0.0.1");
    m.max_runtime_version = get("max_runtime_version", "");
    m.kernel_abi = get("kernel_abi", "6.1-rockchip");
    m.sha256 = get("sha256", "");
    m.signature = get("signature", "");
    m.signing_key_id = get("signing_key_id", "");
    m.origin = get("origin", "local");
    m.release_channel = get("release_channel", "development");
    m.rollback_version = get("rollback_version", "");
    if (const JsonValue* s = v.find("status"); s && s->is_number()) {
        m.status = static_cast<HidPackageStatus>(s->as_int(0));
    }
    if (const JsonValue* t = v.find("created_at"); t && t->is_number()) {
        m.created_at = t->as_int(0);
    }
    return m;
}

// 读 VERSION 文件（去尾部空白；空内容返回 false）
bool hid_read_version(const std::string& path, std::string* out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string v;
    std::getline(f, v);
    // 去空白
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (out) *out = v;
    return !v.empty();
}

// 写 VERSION 文件（覆盖写入，追加换行）
bool hid_write_version(const std::string& path, const std::string& version) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << version << "\n";
    return f.good();
}

}  // namespace ttbox::core
