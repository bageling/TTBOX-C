// domain_paths_internal.hpp — 运行根前缀 / 预设目录 / 状态目录 / 模型库根路径解析。
//
// 自 domain_internal.hpp 拆出（因超 300 行）。全部 inline，domain_internal.hpp 顶层包含，
// 供所有 domain 层 .cpp 继续经 domain_internal.hpp 使用。
#pragma once

#include <cstdlib>
#include <string>

namespace ttbox::core::web {

// 运行根前缀 / 预设目录 / 状态目录（对齐 Python paths.py 与 core Paths.hpp）。
inline std::string ttbox_prefix() {
    const char* v = std::getenv("TTBOX_PREFIX");
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string("/opt/ttbox");
}
inline std::string presets_dir() {
    const char* v = std::getenv("TTBOX_PRESETS_DIR");
    return (v != nullptr && *v != '\0') ? std::string(v) : ttbox_prefix() + "/presets";
}
inline std::string config_dir() {
    const char* v = std::getenv("TTBOX_CONFIG_DIR");
    return (v != nullptr && *v != '\0') ? std::string(v) : ttbox_prefix() + "/config";
}
inline std::string motion_profiles_dir() {
    const char* v = std::getenv("TTBOX_MOTION_PROFILES_DIR");
    return (v != nullptr && *v != '\0') ? std::string(v)
                                        : ttbox_prefix() + "/config/motion-profiles";
}
inline std::string state_dir() {
    const char* v = std::getenv("TTBOX_STATE");
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string("/opt/ttbox/state");
}
inline std::string join_path(const std::string& base, const std::string& name) {
    if (base.empty()) return name;
    if (base.back() == '/') return base + name;
    return base + "/" + name;
}

// 模型库根（V-04）：TTBOX_MODELS_ROOT > <prefix>/models。
inline std::string models_root() {
    const char* v = std::getenv("TTBOX_MODELS_ROOT");
    return (v != nullptr && *v != '\0') ? std::string(v) : ttbox_prefix() + "/models";
}

}  // namespace ttbox::core::web
