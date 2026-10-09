// motion_store_session.cpp — 训练会话 / 训练 / 激活 / 停用。
//
// 自 ttbox_motion/training.py 逐行移植（拆半）。
#include "web/infra/motion_store.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace ttbox::core::web {

namespace {

constexpr double kSessionLeaseSeconds = 30.0;

// uuid4().hex 等价（32 位小写十六进制）。
std::string uuid_hex() {
    static std::random_device rd;
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) out.push_back(kHex[rd() % 16]);
    return out;
}

// 四舍五入到 6 位小数。
double round6(double v) { return std::round(v * 1e6) / 1e6; }

// float(mix.get(key, def)) 等价；非数值抛 std::invalid_argument（→ 500，对齐 Python）。
double mix_num(const JsonValue& mix, const char* key, double def) {
    const JsonValue& v = field_or_null(mix, key);
    if (v.is_null()) return def;
    if (v.is_number()) return v.as_number();
    if (v.is_bool()) return v.as_bool() ? 1.0 : 0.0;
    if (v.is_string()) {
        try {
            return std::stod(v.as_string());
        } catch (...) {
            throw std::invalid_argument("mix values are outside the accepted range");
        }
    }
    throw std::invalid_argument("mix values are outside the accepted range");
}

}  // namespace

// 会话对外视图（无会话返回 inactive 默认）。
JsonValue MotionProfileStore::session_public() {
    JsonValue o = JsonValue::object();
    if (!has_session_) {
        o.set("active", JsonValue::boolean(false));
        o.set("id", JsonValue::string(""));
        o.set("lease_remaining_ms", JsonValue::number(0.0));
        o.set("profile_id", JsonValue::string(""));
        return o;
    }
    const int remaining =
        std::max(0, static_cast<int>((session_.lease_expires_at - now_seconds()) * 1000));
    o.set("active", JsonValue::boolean(remaining > 0));
    o.set("id", JsonValue::string(session_.id));
    o.set("lease_remaining_ms", JsonValue::number(static_cast<double>(remaining)));
    o.set("profile_id", JsonValue::string(session_.profile_id));
    return o;
}

// 校验会话有效（id 匹配且租约未过期），否则抛 MotionError。
const MotionProfileStore::Session& MotionProfileStore::require_session(const std::string& session_id,
                                                                       double now) const {
    if (!has_session_ || session_.id != session_id || session_.lease_expires_at <= now) {
        throw MotionError("training session lease expired");
    }
    return session_;
}

// 开启训练会话：校验档案存在、无未过期会话，随后发新 id 并置 30s 租约。
JsonValue MotionProfileStore::start_session(const std::string& profile_id, double now) {
    JsonValue profile = read_profile(profile_id);
    if (has_session_ && session_.lease_expires_at > now) {
        throw MotionError("active training session exists");
    }
    session_.id = uuid_hex();
    session_.profile_id = field_or_null(profile, "id").as_string("");
    session_.lease_expires_at = now + kSessionLeaseSeconds;
    has_session_ = true;
    JsonValue o = JsonValue::object();
    o.set("id", JsonValue::string(session_.id));
    o.set("profile_id", JsonValue::string(session_.profile_id));
    o.set("lease_expires_at", JsonValue::number(session_.lease_expires_at));
    return o;
}

// 续租会话（校验通过后顺延租约）。
JsonValue MotionProfileStore::heartbeat(const std::string& session_id, double now) {
    require_session(session_id, now);
    session_.lease_expires_at = now + kSessionLeaseSeconds;
    return session_public();
}

// 向会话追加样本：校验样本 → 规整记录 → 写档 → 重算统计 → 返回计数。
JsonValue MotionProfileStore::append_sample(const std::string& session_id, const JsonValue& payload,
                                            double now) {
    require_session(session_id, now);
    const JsonValue stats = validate_motion_sample(payload);
    JsonValue profile = read_profile(session_.profile_id);

    JsonValue rec = JsonValue::object();
    rec.set("schema", field_or_null(payload, "schema"));
    rec.set("mode", field_or_null(payload, "mode"));
    rec.set("completion", field_or_null(payload, "completion"));
    rec.set("canvas", field_or_null(payload, "canvas"));
    rec.set("start", field_or_null(payload, "start"));
    rec.set("target", field_or_null(payload, "target"));
    rec.set("radius", field_or_null(payload, "radius"));
    rec.set("browser", field_or_null(payload, "browser").is_object()
                           ? field_or_null(payload, "browser")
                           : JsonValue::object());
    rec.set("points", field_or_null(payload, "points"));
    rec.set("duration_ms", field_or_null(stats, "duration_ms"));
    rec.set("path_efficiency", field_or_null(stats, "path_efficiency"));
    rec.set("statistics", stats);

    JsonValue samples = field_or_null(profile, "samples");
    if (!samples.is_array()) samples = JsonValue::array();
    samples.push_back(std::move(rec));
    profile.set("samples", std::move(samples));
    profile.set("updated_at_ms",
                JsonValue::number(static_cast<double>(static_cast<int64_t>(now_seconds() * 1000))));
    summary(&profile);
    write_profile(profile);

    JsonValue out = JsonValue::object();
    out.set("sample_count", field_or_null(profile, "sample_count"));
    out.set("reaction_count", field_or_null(profile, "reaction_count"));
    out.set("continuous_count", field_or_null(profile, "continuous_count"));
    out.set("profile_statistics", field_or_null(profile, "statistics"));
    return out;
}

// 结束会话（校验通过后清 has_session_）。
JsonValue MotionProfileStore::stop_session(const std::string& session_id, double now) {
    require_session(session_id, now);
    has_session_ = false;
    JsonValue o = JsonValue::object();
    o.set("stopped", JsonValue::boolean(true));
    return o;
}

// 训练：要求 reaction+continuous 两种样本俱在，按样本速度分布插值出 32 个 knots 并评估质量。
JsonValue MotionProfileStore::train(const std::string& profile_id) {
    JsonValue profile = read_profile(profile_id);
    summary(&profile);

    const JsonValue& samples = field_or_null(profile, "samples");
    bool has_reaction = false;
    bool has_continuous = false;
    if (samples.is_array()) {
        for (const JsonValue& s : samples.as_array()) {
            const std::string mode = field_or_null(s, "mode").as_string("");
            if (mode == "reaction") has_reaction = true;
            if (mode == "continuous") has_continuous = true;
        }
    }
    if (!(has_reaction && has_continuous)) throw MotionError("both modes are required");

    std::vector<double> speeds;
    for (const JsonValue& s : samples.as_array()) {
        const double duration = std::max(field_or_null(s, "duration_ms").as_number(0.0), 1.0);
        double distance = 0.0;
        const JsonValue& pts = field_or_null(s, "points");
        if (pts.is_array()) {
            for (const JsonValue& p : pts.as_array()) {
                distance += std::hypot(field_or_null(p, "dx").as_number(0.0),
                                       field_or_null(p, "dy").as_number(0.0));
            }
        }
        speeds.push_back(distance / duration);
    }

    double mean_speed = 0.0;
    for (double v : speeds) mean_speed += v;
    mean_speed /= static_cast<double>(speeds.size());
    const double mean_eff =
        field_or_null(field_or_null(profile, "statistics"), "path_efficiency").as_number(0.0);
    const double quality = std::max(
        0.0, std::min(100.0, std::round(50.0 * std::min(1.0, static_cast<double>(speeds.size()) / 12.0) +
                                        50.0 * std::max(0.0, std::min(1.0, mean_eff)))));

    std::sort(speeds.begin(), speeds.end());
    const double base = std::max(mean_speed, 1e-6);
    const size_t n = speeds.size();
    JsonValue knots = JsonValue::array();
    for (int i = 0; i < 32; ++i) {
        const double idx = (i / 31.0) * static_cast<double>(n - 1);
        const size_t lo = static_cast<size_t>(std::floor(idx));
        const size_t hi = std::min(lo + 1, n - 1);
        const double frac = idx - static_cast<double>(lo);
        const double norm = (speeds[lo] * (1.0 - frac) + speeds[hi] * frac) / base;
        knots.push_back(JsonValue::number(
            round6(std::max(0.0, std::min(1.0, norm * (0.72 + 0.28 * i / 31.0))))));
    }

    JsonValue model = JsonValue::object();
    model.set("schema", JsonValue::string("ttbox.motion-model.v1"));
    model.set("version", JsonValue::number(1.0));
    model.set("knots", std::move(knots));
    model.set("quality", JsonValue::number(quality));
    model.set("ready", JsonValue::boolean(quality >= 60));
    JsonValue coverage = JsonValue::object();
    coverage.set("reaction", field_or_null(profile, "reaction_count"));
    coverage.set("continuous", field_or_null(profile, "continuous_count"));
    model.set("coverage", std::move(coverage));
    profile.set("model", std::move(model));
    write_profile(profile);
    return public_of(profile);
}

// 激活档案：模型须 ready，写入 active.json（profile_id + mix）。
JsonValue MotionProfileStore::activate(const std::string& profile_id, const JsonValue& mix) {
    JsonValue profile = read_profile(profile_id);
    if (!field_or_null(field_or_null(profile, "model"), "ready").as_bool(false)) {
        throw MotionError("model is not ready");
    }
    JsonValue values = JsonValue::object();
    const double curve = mix_num(mix, "curve_blend", 1.0);
    // ★ 2026-10-07 清理：speed_blend/reaction_blend/max_reaction_delay_ms 已删——
    //   core 的 PersonalMotion 只读 curve_blend/knots，这三项到 core 被静默忽略。
    if (!(0 <= curve && curve <= 1)) {
        throw MotionError("mix values are outside the accepted range");
    }
    values.set("curve", JsonValue::number(curve));

    JsonValue active = JsonValue::object();
    active.set("profile_id", JsonValue::string(profile_id));
    active.set("mix", values);
    atomic_write_file(root_ + "/active.json", active);

    JsonValue out = JsonValue::object();
    out.set("active_profile_id", JsonValue::string(profile_id));
    out.set("enabled", JsonValue::boolean(true));
    out.set("mix", std::move(values));
    return out;
}

// 取消激活（删除 active.json），返回停用后的公开状态。
JsonValue MotionProfileStore::deactivate() {
    std::error_code ec;
    std::filesystem::remove(root_ + "/active.json", ec);
    JsonValue out = JsonValue::object();
    out.set("active_profile_id", JsonValue::string(""));
    out.set("enabled", JsonValue::boolean(false));
    out.set("mix", mix());
    return out;
}

}  // namespace ttbox::core::web
