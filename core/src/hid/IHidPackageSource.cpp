// IHidPackageSource.cpp — HID Package 来源实现（本地）
#include "hid/IHidPackageSource.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace ttbox::core {

namespace {

// 读取整个文件到字符串（打开失败返回 false）
bool read_file(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// 递归复制目录（覆盖同名文件）
bool copy_dir(const std::string& src, const std::string& dst, std::string* error) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        if (error) *error = "创建目标目录失败: " + dst;
        return false;
    }
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "复制失败: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

// 本地包导入：校验 manifest 存在 → 复制到 staging → 回填清单
bool LocalHidPackageSource::fetch_to(const std::string& dest_dir,
                                     HidPackageManifest* manifest,
                                     std::string* error) {
    if (!fs::exists(dir_ + "/manifest.json")) {
        if (error) *error = "本地包缺少 manifest.json: " + dir_;
        return false;
    }
    if (!copy_dir(dir_, dest_dir, error)) return false;
    if (manifest) {
        std::string text;
        if (read_file(dest_dir + "/manifest.json", &text)) {
            auto res = json_parse(text);
            if (res.ok) *manifest = HidPackageManifest::from_json(res.value);
        }
    }
    return true;
}

}  // namespace ttbox::core
