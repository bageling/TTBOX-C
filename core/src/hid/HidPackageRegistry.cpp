// HidPackageRegistry.cpp — HID Package 注册表实现
#include "hid/HidPackageRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace ttbox::core {

namespace {

std::string now_ms() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
}

bool read_file(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

}  // namespace

// 解析根目录：opts.root > TTBOX_HID_ROOT 环境变量 > 编译期 TTBOX_PROJECT_ROOT/hid
HidPackageRegistry::HidPackageRegistry(HidPackageRegistryOptions opts)
    : opts_(std::move(opts)) {
    if (!opts_.root.empty()) {
        root_ = opts_.root;
        return;
    }
    // P1 环境变量 TTBOX_HID_ROOT（对应配置键 hid_package_root 的 CLI/env 覆盖链）：
    // 此前只能靠编译期宏，换目录必须重编译；现在运行期即可覆盖。
    if (const char* env_root = std::getenv("TTBOX_HID_ROOT")) {
        if (*env_root) {
            root_ = env_root;
            return;
        }
    }
    // P3 编译期兜底
    root_ = std::string(TTBOX_PROJECT_ROOT) + "/hid";
}

// 路径是否存在（不抛异常）
bool HidPackageRegistry::exists(const std::string& path) const {
    std::error_code ec;
    return fs::exists(path, ec);
}

// 创建 registry/staging/quarantine 等全部子目录
bool HidPackageRegistry::ensure_dirs(std::string* error) {
    for (const char* sub : {"registry", "staging", "quarantine", "config", "descriptors",
                            "bin", "validation", "profiles", "runtime", "packages"}) {
        std::error_code ec;
        const std::string p = root_ + "/" + sub;
        if (!fs::create_directories(p, ec) && ec) {
            if (error) *error = "创建目录失败: " + p;
            return false;
        }
    }
    return true;
}

// 初始化：建根目录与子目录（幂等）
bool HidPackageRegistry::init(std::string* error) {
    if (root_.empty()) {
        if (error) *error = "HID 包根目录为空";
        return false;
    }
    std::error_code ec;
    if (!fs::create_directories(root_, ec) && ec) {
        if (error) *error = "创建 HID 包目录失败: " + root_;
        return false;
    }
    return ensure_dirs(error);
}

// 递归复制目录（覆盖同名文件）
bool HidPackageRegistry::copy_dir(const std::string& src, const std::string& dst,
                                  std::string* error) const {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        if (error) *error = "创建目录失败: " + dst;
        return false;
    }
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "复制失败 " + src + " -> " + dst + ": " + ec.message();
        return false;
    }
    return true;
}

// 已安装包目录 packages/<version>
std::string HidPackageRegistry::package_dir(const std::string& version) const {
    return root_ + "/packages/" + version;
}
// 待校验包目录 staging/<version>
std::string HidPackageRegistry::staging_dir(const std::string& version) const {
    return root_ + "/staging/" + version;
}
// 隔离包目录 quarantine/<version>
std::string HidPackageRegistry::quarantine_dir(const std::string& version) const {
    return root_ + "/quarantine/" + version;
}

// 读 JSON 文件指定字符串键（缺/类型不符返回空串）
std::string HidPackageRegistry::read_json_str(const std::string& file, const char* key) const {
    std::string text;
    if (!read_file(file, &text)) return "";
    auto res = json_parse(text);
    if (!res.ok || !res.value.is_object()) return "";
    const JsonValue* v = res.value.find(key);
    if (!v || !v->is_string()) return "";
    return v->as_string();
}

// 写 active.json（version + activated_at）
bool HidPackageRegistry::write_active(const std::string& version, std::string* error) {
    JsonValue a = JsonValue::object();
    a.set("version", JsonValue::string(version));
    a.set("activated_at", JsonValue::number(static_cast<double>(std::stoll(now_ms()))));
    if (!write_file(root_ + "/registry/active.json", a.dump())) {
        if (error) *error = "写 active.json 失败";
        return false;
    }
    return true;
}

// 写 previous.json（version + recorded_at）
bool HidPackageRegistry::write_previous(const std::string& version, std::string* error) {
    JsonValue p = JsonValue::object();
    p.set("version", JsonValue::string(version));
    p.set("recorded_at", JsonValue::number(static_cast<double>(std::stoll(now_ms()))));
    if (!write_file(root_ + "/registry/previous.json", p.dump())) {
        if (error) *error = "写 previous.json 失败";
        return false;
    }
    return true;
}

// 当前激活版本（无则空串）
std::string HidPackageRegistry::get_active() const {
    return read_json_str(root_ + "/registry/active.json", "version");
}
// 回滚目标版本（无则空串）
std::string HidPackageRegistry::get_previous() const {
    return read_json_str(root_ + "/registry/previous.json", "version");
}

// 校验 staging/<version>：调 validator，通过后写 validation/ok.json
bool HidPackageRegistry::validate(const std::string& version, std::string* error) {
    if (!validator_) {
        if (error) *error = "未设置 validator（生产环境应注入包结构/健康校验）";
        return false;
    }
    const std::string sd = staging_dir(version);
    if (!exists(sd + "/manifest.json")) {
        if (error) *error = "staging 包不存在: " + version;
        return false;
    }
    std::string mtext;
    if (!read_file(sd + "/manifest.json", &mtext)) {
        if (error) *error = "读 manifest 失败";
        return false;
    }
    auto res = json_parse(mtext);
    if (!res.ok) {
        if (error) *error = "manifest 解析失败: " + res.error;
        return false;
    }
    HidPackageManifest m = HidPackageManifest::from_json(res.value);
    std::string verr;
    if (!validator_(sd, m, &verr)) {
        if (error) *error = "包校验失败(" + version + "): " + verr;
        return false;
    }
    // 写校验结果
    JsonValue ok = JsonValue::object();
    ok.set("ok", JsonValue::boolean(true));
    ok.set("validated_at", JsonValue::number(static_cast<double>(std::stoll(now_ms()))));
    std::error_code vec;
    fs::create_directories(sd + "/validation", vec);
    write_file(sd + "/validation/ok.json", ok.dump());
    return true;
}

// 安装：把已校验的 staging/<version> 复制进 packages/<version> 并置状态 installed
bool HidPackageRegistry::install(const std::string& version, std::string* error) {
    const std::string sd = staging_dir(version);
    const std::string pd = package_dir(version);
    if (!exists(sd + "/manifest.json")) {
        if (error) *error = "staging 包不存在（先 validate）: " + version;
        return false;
    }
    if (!exists(sd + "/validation/ok.json")) {
        if (error) *error = "包未校验（先 validate）: " + version;
        return false;
    }
    if (exists(pd)) {
        if (error) *error = "版本已安装: " + version;
        return false;
    }
    if (!copy_dir(sd, pd, error)) return false;
    // 更新 manifest 状态为 installed
    std::string mtext;
    if (read_file(pd + "/manifest.json", &mtext)) {
        auto res = json_parse(mtext);
        if (res.ok) {
            HidPackageManifest m = HidPackageManifest::from_json(res.value);
            m.status = HidPackageStatus::kInstalled;
            write_file(pd + "/manifest.json", m.to_json().dump());
        }
    }
    return true;
}

// 激活指定版本：校验失败则隔离并恢复旧版本；成功则记录 previous 并更新各状态
bool HidPackageRegistry::activate(const std::string& version, std::string* error) {
    const std::string pd = package_dir(version);
    if (!exists(pd + "/manifest.json")) {
        if (error) *error = "版本未安装，无法激活: " + version;
        return false;
    }
    const std::string old_active = get_active();
    if (old_active == version) {
        return true;  // 已激活
    }
    // 激活前校验（health check）：失败 → quarantine + 保持旧 active
    if (validator_) {
        std::string mtext;
        if (read_file(pd + "/manifest.json", &mtext)) {
            auto res = json_parse(mtext);
            HidPackageManifest m = HidPackageManifest::from_json(res.value);
            std::string verr;
            if (!validator_(pd, m, &verr)) {
                // 激活失败：隔离新版本，恢复旧版本
                std::error_code ec;
                fs::remove_all(quarantine_dir(version), ec);
                fs::rename(pd, quarantine_dir(version), ec);
                if (!old_active.empty()) write_previous(old_active, nullptr);
                if (error) *error = "激活校验失败(" + version + ")，已隔离并恢复旧版本(" +
                                     old_active + "): " + verr;
                return false;
            }
        }
    }
    // 记录 previous（双版本回滚）
    if (!old_active.empty()) write_previous(old_active, nullptr);
    if (!write_active(version, error)) {
        // 写失败：恢复旧值
        if (!old_active.empty()) write_active(old_active, nullptr);
        return false;
    }
    // 更新 manifest 状态
    std::string mtext;
    if (read_file(pd + "/manifest.json", &mtext)) {
        auto res = json_parse(mtext);
        if (res.ok) {
            HidPackageManifest m = HidPackageManifest::from_json(res.value);
            m.status = HidPackageStatus::kActive;
            m.rollback_version = old_active;
            write_file(pd + "/manifest.json", m.to_json().dump());
        }
    }
    // 旧 active 包状态 → inactive（保证 list 中只有一个是 active）
    if (!old_active.empty() && old_active != version) {
        const std::string opd = package_dir(old_active);
        std::string omtext;
        if (read_file(opd + "/manifest.json", &omtext)) {
            auto ores = json_parse(omtext);
            if (ores.ok) {
                HidPackageManifest om = HidPackageManifest::from_json(ores.value);
                om.status = HidPackageStatus::kInactive;
                write_file(opd + "/manifest.json", om.to_json().dump());
            }
        }
    }
    // previous 自指（deactivate→重新激活同一版本）时清空，保证 previous≠active
    if (get_previous() == version) write_previous("", nullptr);
    return true;
}

// 停用：记录 previous，清空 active
bool HidPackageRegistry::deactivate(std::string* error) {
    const std::string act = get_active();
    if (!act.empty()) write_previous(act, nullptr);
    return write_active("", error);
}

// 删除指定版本（active 版本禁止删除）
bool HidPackageRegistry::remove(const std::string& version, std::string* error) {
    if (!get_active().empty() && get_active() == version) {
        if (error) *error = "禁止删除 active HID Package: " + version;
        return false;
    }
    const std::string pd = package_dir(version);
    if (!exists(pd)) {
        if (error) *error = "版本未安装: " + version;
        return false;
    }
    std::error_code ec;
    fs::remove_all(pd, ec);
    if (ec) {
        if (error) *error = "删除失败: " + ec.message();
        return false;
    }
    return true;
}

// 回滚：激活 previous 版本
bool HidPackageRegistry::rollback(std::string* error) {
    const std::string prev = get_previous();
    if (prev.empty()) {
        if (error) *error = "无可回滚版本（previous 为空）";
        return false;
    }
    return activate(prev, error);
}

// 列举所有已安装版本的 manifest（按 version 排序）
std::vector<HidPackageManifest> HidPackageRegistry::list() const {
    std::vector<HidPackageManifest> out;
    const std::string dir = root_ + "/packages";
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        const std::string mp = entry.path().string() + "/manifest.json";
        std::string text;
        if (!read_file(mp, &text)) continue;
        auto res = json_parse(text);
        if (!res.ok) continue;
        HidPackageManifest m = HidPackageManifest::from_json(res.value);
        if (m.version.empty()) m.version = entry.path().filename().string();
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(),
              [](const HidPackageManifest& a, const HidPackageManifest& b) {
                  return a.version < b.version;
              });
    return out;
}

}  // namespace ttbox::core
