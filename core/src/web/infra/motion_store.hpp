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

// 运动曲线领域契约错误（路由层按消息分流 409/422）。
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

// 当前墙钟秒（对齐 Python time.time()）。
inline double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 取对象成员；缺失/非对象返回 null（对齐 dict.get 的 None 语义）。
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

// 个人运动曲线的本地文件存储（<root>/<profile-id>/profile.json + active.json）。
class MotionProfileStore {
public:
    // 以存储根目录构造。
    explicit MotionProfileStore(std::string root) : root_(std::move(root)) {}

    // 查询 / CRUD（公开面镜像 api/motion.py 调用）。
    // 列出全部档案（目录为空时自动建默认档）。
    JsonValue list_profiles();
    // 读取单个档案详情。
    JsonValue list_profile(const std::string& profile_id);
    // 重命名档案。
    JsonValue rename_profile(const std::string& profile_id, const std::string& name);
    // 删除档案（若为激活档则先停用）。
    JsonValue remove_profile(const std::string& profile_id);
    // 清空档案样本并重置其模型。
    JsonValue clear_samples(const std::string& profile_id);

    // 训练会话。
    // 开启训练会话（带 30s 租约）。
    JsonValue start_session(const std::string& profile_id, double now);
    // 续租会话。
    JsonValue heartbeat(const std::string& session_id, double now);
    // 向当前会话追加一个样本。
    JsonValue append_sample(const std::string& session_id, const JsonValue& payload, double now);
    // 结束会话。
    JsonValue stop_session(const std::string& session_id, double now);

    // 训练 / 激活。
    // 依据样本训练曲线模型。
    JsonValue train(const std::string& profile_id);
    // 激活档案并写入 mix。
    JsonValue activate(const std::string& profile_id, const JsonValue& mix);
    // 取消激活（删除 active.json）。
    JsonValue deactivate();

    // 当前 mix（active.json 的 mix，或默认值）。路由层写 personal_motion 时读取。
    JsonValue mix() const;

private:
    std::string root_;  // 存储根目录
    // 训练会话（单会话模型）。
    struct Session {
        std::string id;
        std::string profile_id;
        double lease_expires_at = 0.0;
    };
    bool has_session_ = false;
    Session session_;

    // 档案文件路径（含 profile_id 合法性校验）。
    std::string path_of(const std::string& profile_id) const;
    // 读档案文件（损坏/缺失抛 MotionError）。
    JsonValue read_profile(const std::string& profile_id);
    // 写档案文件（按档案内 id 定位路径）。
    void write_profile(const JsonValue& profile);
    // 原子写：临时文件 + rename。
    void atomic_write_file(const std::string& path, const JsonValue& payload);
    // 就地重算样本统计（计数/均值）写回 profile。
    void summary(JsonValue* profile);
    // 档案 → 对外公开视图（剥离 samples）。
    static JsonValue public_of(const JsonValue& profile);
    // 新建默认档案（分配 profile-N 名称并落盘）。
    JsonValue create_profile(const std::string& name);
    // 当前激活档案 id（读 active.json）。
    std::string active_id() const;
    // 会话对外视图。
    JsonValue session_public();
    // 取有效会话，否则抛 MotionError（租约过期/不匹配）。
    const Session& require_session(const std::string& session_id, double now) const;
};

}  // namespace ttbox::core::web
