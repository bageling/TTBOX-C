// ConfigManager.cpp — 配置读取实现
/*
 * TTBOX 文件说明
 *
 * 文件：ConfigManager.cpp
 *
 * 作用：
 *   读取和管理 JSON 格式的配置文件。
 *
 * 小白理解：
 *   TTBOX 的所有参数都存在 JSON 文件里。
 *   这个模块负责读这些文件，并把参数分发给各个模块。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "config/ConfigManager.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ttbox::core {

namespace {

// 深合并：src 的键覆盖 dst；双方均为 object 时递归，否则整体替换。
// 与 SET_CONFIG 的现有语义一致（runtime_profile 等嵌套对象按键合并）。
//
// ★ 2026-10-04 修正（cppcheck nullPointerRedundantCheck 报的"理论崩溃"）：
//   原判据 `if (!dst || !dst->is_object() || !src.is_object()) { *dst = src; return; }`
//   —— 短路求值下，`!dst` 为真时**下一行 `*dst = src` 立刻解引用空指针**。
//   即"判空保护"本身是空指针崩溃的入口。
//   实际无法触发：全部 3 个调用点都传栈上对象的地址（&merged / &base / &child），
//   不存在 null。但**保护写成崩溃入口**本身就是坑 —— 后来者以为"这里判过空了"，
//   实际判空后立刻崩。
//   ⇒ 改为显式前置契约：dst 必须有效（nullptr 直接 return，不静默、也不假装合并）。
void deep_merge(JsonValue* dst, const JsonValue& src) {
    if (dst == nullptr) {
        // 只有一处能传 null：递归调用（&child，永不为 null）与两个栈上对象地址。
        // 真传进来说明调用方有 bug —— 不静默兜底、不崩，直接返回让上层看出异常。
        return;
    }
    if (!dst->is_object() || !src.is_object()) {
        *dst = src;
        return;
    }
    for (const auto& [key, value] : src.as_object()) {
        const JsonValue* existing = dst->find(key);
        if (existing && existing->is_object() && value.is_object()) {
            JsonValue child = *existing;
            deep_merge(&child, value);
            dst->set(key, std::move(child));
        } else {
            dst->set(key, value);
        }
    }
}

// view 相对 base 的差异（R3）：只保留与基线不同的键。
//   - 键不在 base 中        → 保留（设备层自有键）
//   - 双方均为 object        → 递归取差异；子差异为空则整键剔除
//   - 值相同（含标量/数组）  → 剔除（基线已提供，不快照冻结进设备层）
//   - 值不同                → 保留 view 侧新值
// base 为空对象时返回 view 本身（单文件模式 = 全量写回，行为不变）。
JsonValue layer_diff(const JsonValue& view, const JsonValue& base) {
    JsonValue out = JsonValue::object();
    if (!view.is_object() || !base.is_object()) return view;
    for (const auto& [key, value] : view.as_object()) {
        const JsonValue* b = base.find(key);
        if (b == nullptr) {
            out.set(key, value);
            continue;
        }
        if (value.is_object() && b->is_object()) {
            JsonValue child = layer_diff(value, *b);
            if (!child.as_object().empty()) {
                out.set(key, std::move(child));
            }
        } else if (value != *b) {
            out.set(key, value);
        }
    }
    return out;
}

}  // namespace

bool ConfigManager::load(const std::string& path, std::string* error) {
    // --config 指向目录（如 /etc/ttbox/config.d/）→ 分层加载
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return load_layered(path, error);
    }

    auto set_error = [error](const std::string& msg) {
        if (error) *error = msg;
    };

    JsonParseResult parsed = json_parse_file(path);
    if (!parsed.ok) {
        set_error("配置加载失败: " + parsed.error);
        loaded_ = false;
        return false;
    }
    if (!parsed.value.is_object()) {
        set_error("配置加载失败: 根节点必须是 JSON 对象 (" + path + ")");
        loaded_ = false;
        return false;
    }

    root_ = std::move(parsed.value);
    path_ = path;
    loaded_ = true;
    // 单文件模式：无分层状态（persist 走全量写回，行为与历史一致）
    layered_ = false;
    layer_base_ = JsonValue::object();
    return true;
}

bool ConfigManager::load_layered(const std::string& dir, std::string* error) {
    auto set_error = [error](const std::string& msg) {
        if (error) *error = msg;
    };

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        set_error("配置目录不存在: " + dir);
        loaded_ = false;
        return false;
    }

    // 收集 *.json，按文件名升序（00-factory < 10-device < ...）依次深合并
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string p = entry.path().string();
        if (p.size() > 5 && p.compare(p.size() - 5, 5, ".json") == 0) {
            files.push_back(p);
        }
    }
    if (ec) {
        set_error("配置目录读取失败: " + dir + " (" + ec.message() + ")");
        loaded_ = false;
        return false;
    }
    if (files.empty()) {
        set_error("配置目录为空（无 *.json）: " + dir);
        loaded_ = false;
        return false;
    }
    std::sort(files.begin(), files.end());

    JsonValue merged = JsonValue::object();
    // 基线视图（R3）：除写回目标（10-device.json）外所有层的深合并。
    // persist 用它做被减数，只把"与基线不同的键"写进设备层。
    JsonValue base = JsonValue::object();
    const std::string target =
        (std::filesystem::path(dir) / "10-device.json").string();
    for (const auto& file : files) {
        JsonParseResult parsed = json_parse_file(file);
        if (!parsed.ok) {
            set_error("分层配置加载失败 [" + file + "]: " + parsed.error);
            loaded_ = false;
            return false;
        }
        if (!parsed.value.is_object()) {
            set_error("分层配置根节点必须是 JSON 对象 (" + file + ")");
            loaded_ = false;
            return false;
        }
        deep_merge(&merged, parsed.value);
        // 写回目标自身的贡献不算基线（它是唯一可写层）
        if (std::filesystem::path(file) != std::filesystem::path(target)) {
            deep_merge(&base, parsed.value);
        }
    }

    root_ = std::move(merged);
    // 写回目标固定为客户层 10-device.json：00-factory.json 是只读出厂基线。
    // 该文件不存在时，首个 persist 落盘会创建它（目录需对运行用户可写）。
    path_ = target;
    layer_base_ = std::move(base);
    layered_ = true;
    loaded_ = true;
    return true;
}

bool ConfigManager::persist(const JsonValue& view, std::string* error) {
    if (!loaded_ || path_.empty()) {
        if (error) *error = "配置未加载，无写回目标";
        return false;
    }
    if (!view.is_object()) {
        if (error) *error = "持久化视图必须是 JSON 对象";
        return false;
    }

    // 写回内容：分层模式只写与基线的差异（R3），单文件模式全量（行为不变）。
    // 写回目标：恒为 path_（文件）——B1 防线，与本类的加载来源（目录/文件）解耦。
    const JsonValue out_view = layered_ ? layer_diff(view, layer_base_) : view;
    const std::string text = out_view.dump();
    const std::filesystem::path final_path(path_);
    const std::filesystem::path tmp_path = final_path.string() + ".tmp";

#if defined(_WIN32)
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "无法创建配置临时文件: " + tmp_path.string();
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
            if (error) *error = "配置临时文件写入失败: " + tmp_path.string();
            return false;
        }
    }
#else
    // Linux 板端使用 write + fsync，确保临时文件内容真正进入存储设备后才 rename。
    const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (error) *error = "无法创建配置临时文件: " + tmp_path.string();
        return false;
    }
    size_t written = 0;
    bool write_ok = true;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n <= 0) {
            write_ok = false;
            break;
        }
        written += static_cast<size_t>(n);
    }
    if (write_ok) write_ok = (::fsync(fd) == 0);
    if (::close(fd) != 0) write_ok = false;
    if (!write_ok) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp_path, rm_ec);
        if (error) *error = "配置临时文件写入或同步失败: " + tmp_path.string();
        return false;
    }
#endif

    std::error_code ec;
#if defined(_WIN32)
    // Windows 的 std::filesystem::rename 不覆盖现有文件，直接使用系统替换语义，
    // 避免“先删旧文件、再改名”产生配置暂时不存在的窗口。
    if (!::MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
    }
#else
    std::filesystem::rename(tmp_path, final_path, ec);
#endif
    if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp_path, rm_ec);
        if (error) *error = "配置原子替换失败: " + ec.message();
        return false;
    }
#if !defined(_WIN32)
    // rename 的目录项也要同步，确保断电后新文件名仍然存在。
    const std::filesystem::path parent = final_path.has_parent_path()
                                             ? final_path.parent_path()
                                             : std::filesystem::path(".");
    const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0 || ::fsync(dir_fd) != 0) {
        if (dir_fd >= 0) ::close(dir_fd);
        // 目录 fsync 失败仅降级为告警（文件本体已原子替换成功）
        std::fprintf(stderr, "[ConfigManager] 配置已原子替换，但父目录同步失败: %s\n",
                     parent.string().c_str());
    } else {
        ::close(dir_fd);
    }
#endif
    return true;
}

std::string ConfigManager::get_string(const std::string& key, const std::string& def) const {
    const JsonValue* v = root_.find(key);
    return v ? v->as_string(def) : def;
}

double ConfigManager::get_double(const std::string& key, double def) const {
    const JsonValue* v = root_.find(key);
    return v ? v->as_number(def) : def;
}

int64_t ConfigManager::get_int(const std::string& key, int64_t def) const {
    const JsonValue* v = root_.find(key);
    return v ? v->as_int(def) : def;
}

bool ConfigManager::get_bool(const std::string& key, bool def) const {
    const JsonValue* v = root_.find(key);
    return v ? v->as_bool(def) : def;
}

std::vector<std::pair<std::string, std::string>> ConfigManager::flatten() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [key, value] : root_.as_object()) {
        switch (value.type()) {
            case JsonType::kString:
                out.emplace_back(key, value.as_string());
                break;
            case JsonType::kNumber: {
                std::ostringstream ss;
                ss << value.as_number();
                out.emplace_back(key, ss.str());
                break;
            }
            case JsonType::kBool:
                out.emplace_back(key, value.as_bool() ? "true" : "false");
                break;
            case JsonType::kNull:
                out.emplace_back(key, "null");
                break;
            case JsonType::kArray:
            case JsonType::kObject:
                out.emplace_back(key, value.dump());  // 嵌套结构按紧凑 JSON 输出
                break;
        }
    }
    return out;
}

bool ConfigManager::device_layer_has(const std::string& key) const {
    // 单文件模式不存在"继承"关系，恒为未显式设置。
    if (!layered_ || path_.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path_, ec)) return false;
    JsonParseResult parsed = json_parse_file(path_);
    if (!parsed.ok || !parsed.value.is_object()) return false;
    return parsed.value.find(key) != nullptr;
}

}  // namespace ttbox::core
