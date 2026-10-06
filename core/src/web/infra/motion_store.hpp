// motion_store.hpp — 个人运动曲线训练领域模型（TTBOX 本地数据层，零 IPC / 零外部 ABI）。
//
// 自 ttbox_motion/training.py 逐行移植。文件布局：<root>/<profile-id>/profile.json，
// <root>/active.json（激活档案 + mix）。契约错误统一抛 MotionError（路由层按消息分流 409/422）。
#pragma once

#include <cctype>
#include <chrono>
#include <stdexcept>
#include <string>

#include "common/Json.hpp"

namespace ttbox::core::web {

class MotionError : public std::runtime_error {
public:
    explicit MotionError(const std::string& msg) : std::runtime_error(msg) {}
};

// 段内共享小工具（两 .cpp 共用，自包含、不依赖 domain 层）。
inline std::string trim_copy(const std::string& raw) {
    size_t b = 0;
    size_t e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1])) != 0) --e;
    return raw.substr(b, e - b);
}

inline double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline JsonValue field_or_null(const JsonValue& obj, const char* key) {
    const JsonValue* v = obj.is_object() ? obj.find(key) : nullptr;
    return v != nullptr ? *v : JsonValue::null();
}

// PROFILE_ID_RE = [a-z0-9][a-z0-9._-]{0,63}（fullmatch）。
inline bool valid_profile_id(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    const auto lower = [](char c) {
        return c >= 'a' && c <= 'z';
    };
    const auto digit = [](char c) {
        return c >= '0' && c <= '9';
    };
    if (!lower(s[0]) && !digit(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (!lower(c) && !digit(c) && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

// 训练样本契约校验（对齐 validate_motion_sample）。合法返回统计对象；非法抛 MotionError。
JsonValue validate_motion_sample(const JsonValue& payload);

class MotionProfileStore {
public:
    explicit MotionProfileStore(std::string root) : root_(std::move(root)) {}

    // 查询 / CRUD（公开面镜像 api/motion.py 调用）。
    JsonValue list_profiles();
    JsonValue list_profile(const std::string& profile_id);
    JsonValue rename_profile(const std::string& profile_id, const std::string& name);
    JsonValue remove_profile(const std::string& profile_id);
    JsonValue clear_samples(const std::string& profile_id);

    // 训练会话。
    JsonValue start_session(const std::string& profile_id, double now);
    JsonValue heartbeat(const std::string& session_id, double now);
    JsonValue append_sample(const std::string& session_id, const JsonValue& payload, double now);
    JsonValue stop_session(const std::string& session_id, double now);

    // 训练 / 激活。
    JsonValue train(const std::string& profile_id);
    JsonValue activate(const std::string& profile_id, const JsonValue& mix);
    JsonValue deactivate();

    // 当前 mix（active.json 的 mix，或默认值）。路由层写 personal_motion 时读取。
    JsonValue mix() const;

private:
    std::string root_;
    struct Session {
        std::string id;
        std::string profile_id;
        double lease_expires_at = 0.0;
    };
    bool has_session_ = false;
    Session session_;

    std::string path_of(const std::string& profile_id) const;
    JsonValue read_profile(const std::string& profile_id);
    void write_profile(const JsonValue& profile);
    void atomic_write_file(const std::string& path, const JsonValue& payload);
    void summary(JsonValue* profile);
    static JsonValue public_of(const JsonValue& profile);
    JsonValue create_profile(const std::string& name);
    std::string active_id() const;
    JsonValue session_public();
    const Session& require_session(const std::string& session_id, double now) const;
};

}  // namespace ttbox::core::web
