// motion_store.cpp — 运动训练数据层（文件 / 查询 / CRUD）。
//
// 自 ttbox_motion/training.py 逐行移植（拆三）。样本校验在 motion_store_validate.cpp，
// 会话 / 训练 / 激活在 motion_store_session.cpp。
#include "web/infra/motion_store.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace ttbox::core::web {

namespace {

bool read_text(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool write_text(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << content;
    out.flush();
    return out.good();
}

}  // namespace

// 原子写（对齐 _atomic_json）：建父目录 + 临时文件 + rename。dump 为紧凑 JSON。
void MotionProfileStore::atomic_write_file(const std::string& path, const JsonValue& payload) {
    const std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const std::string temp = path + "." + std::to_string(getpid()) + ".tmp";
    write_text(temp, payload.dump());
    std::filesystem::rename(temp, path, ec);
    if (ec) std::filesystem::remove(temp, ec);
}

std::string MotionProfileStore::path_of(const std::string& profile_id) const {
    if (!valid_profile_id(profile_id)) throw MotionError("profile_id is invalid");
    return root_ + "/" + profile_id + "/profile.json";
}

JsonValue MotionProfileStore::read_profile(const std::string& profile_id) {
    const std::string path = path_of(profile_id);
    std::string text;
    if (!read_text(path, &text)) throw MotionError("failed to open " + path);
    JsonParseResult r = json_parse(text);
    if (!r.ok || !r.value.is_object()) throw MotionError("profile is damaged");
    return r.value;
}

void MotionProfileStore::write_profile(const JsonValue& profile) {
    atomic_write_file(path_of(field_or_null(profile, "id").as_string("")), profile);
}

void MotionProfileStore::summary(JsonValue* profile) {
    const JsonValue& samples = field_or_null(*profile, "samples");
    int sample_count = 0;
    int reaction_count = 0;
    int continuous_count = 0;
    double duration_sum = 0.0;
    double eff_sum = 0.0;
    int eff_count = 0;
    if (samples.is_array()) {
        for (const JsonValue& s : samples.as_array()) {
            ++sample_count;
            const std::string mode = field_or_null(s, "mode").as_string("");
            if (mode == "reaction") {
                ++reaction_count;
                const JsonValue& stat = field_or_null(s, "statistics");
                duration_sum += field_or_null(stat, "duration_ms").as_number(0.0);
            } else if (mode == "continuous") {
                ++continuous_count;
            }
            const JsonValue& pe = field_or_null(s, "path_efficiency");
            if (pe.is_number()) {
                eff_sum += pe.as_number(0.0);
                ++eff_count;
            }
        }
    }
    profile->set("sample_count", JsonValue::number(static_cast<double>(sample_count)));
    profile->set("reaction_count", JsonValue::number(static_cast<double>(reaction_count)));
    profile->set("continuous_count", JsonValue::number(static_cast<double>(continuous_count)));
    JsonValue stats = JsonValue::object();
    if (reaction_count > 0) {
        stats.set("reaction_mean_ms", JsonValue::number(duration_sum / reaction_count));
    } else {
        stats.set("reaction_mean_ms", JsonValue::null());
    }
    if (eff_count > 0) {
        stats.set("path_efficiency", JsonValue::number(eff_sum / eff_count));
    } else {
        stats.set("path_efficiency", JsonValue::null());
    }
    profile->set("statistics", std::move(stats));
}

JsonValue MotionProfileStore::public_of(const JsonValue& profile) {
    JsonValue out = JsonValue::object();
    for (const auto& [k, v] : profile.as_object()) {
        if (k == "samples") continue;
        out.set(k, v);
    }
    const JsonValue model = field_or_null(out, "model");
    JsonValue m = JsonValue::object();
    if (model.is_object()) {
        for (const auto& [k, v] : model.as_object()) m.set(k, v);
    }
    const JsonValue knots = field_or_null(m, "knots");
    m.set("exists", JsonValue::boolean(knots.is_array() && !knots.as_array().empty()));
    if (m.find("sample_count") == nullptr) m.set("sample_count", JsonValue::number(0.0));
    if (m.find("coverage") == nullptr) m.set("coverage", JsonValue::object());
    out.set("model", std::move(m));
    return out;
}

JsonValue MotionProfileStore::create_profile(const std::string& name) {
    const std::string n = trim_copy(name);
    if (n.empty()) throw MotionError("name is required");
    int index = 1;
    std::error_code ec;
    while (std::filesystem::exists(root_ + "/profile-" + std::to_string(index), ec)) ++index;
    const int64_t now_ms = static_cast<int64_t>(now_seconds() * 1000);

    JsonValue profile = JsonValue::object();
    profile.set("schema", JsonValue::string("ttbox.motion-profile.v1"));
    profile.set("id", JsonValue::string("profile-" + std::to_string(index)));
    profile.set("name", JsonValue::string(n));
    profile.set("created_at_ms", JsonValue::number(static_cast<double>(now_ms)));
    profile.set("updated_at_ms", JsonValue::number(static_cast<double>(now_ms)));
    profile.set("samples", JsonValue::array());
    JsonValue model = JsonValue::object();
    model.set("schema", JsonValue::string("ttbox.motion-model.v1"));
    model.set("ready", JsonValue::boolean(false));
    model.set("quality", JsonValue::number(0.0));
    model.set("knots", JsonValue::array());
    model.set("exists", JsonValue::boolean(false));
    model.set("sample_count", JsonValue::number(0.0));
    model.set("coverage", JsonValue::object());
    profile.set("model", std::move(model));
    JsonValue stats = JsonValue::object();
    stats.set("reaction_mean_ms", JsonValue::null());
    stats.set("path_efficiency", JsonValue::null());
    profile.set("statistics", std::move(stats));
    summary(&profile);
    write_profile(profile);
    return profile;
}

JsonValue MotionProfileStore::list_profiles() {
    std::vector<std::string> names;
    std::error_code ec;
    if (std::filesystem::is_directory(root_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            if (std::filesystem::exists(entry.path().string() + "/profile.json", ec)) {
                names.push_back(entry.path().filename().string());
            }
        }
    }
    std::sort(names.begin(), names.end());

    std::vector<JsonValue> profiles;
    for (const std::string& name : names) {
        std::string text;
        const std::string path = root_ + "/" + name + "/profile.json";
        if (!read_text(path, &text)) continue;
        JsonParseResult r = json_parse(text);
        if (!r.ok || !r.value.is_object()) continue;
        JsonValue profile = r.value;
        summary(&profile);
        profiles.push_back(std::move(profile));
    }
    if (profiles.empty()) profiles.push_back(create_profile("默认曲线"));

    const std::string active = active_id();
    std::sort(profiles.begin(), profiles.end(), [&active](const JsonValue& a, const JsonValue& b) {
        const std::string ida = field_or_null(a, "id").as_string("");
        const std::string idb = field_or_null(b, "id").as_string("");
        const bool a_first = (ida == active);
        const bool b_first = (idb == active);
        if (a_first != b_first) return a_first;
        return ida < idb;
    });

    JsonValue pub = JsonValue::array();
    for (const JsonValue& p : profiles) pub.push_back(public_of(p));
    JsonValue out = JsonValue::object();
    out.set("profiles", std::move(pub));
    out.set("active_profile_id", JsonValue::string(active));
    out.set("enabled", JsonValue::boolean(!active.empty()));
    out.set("mix", mix());
    out.set("session", session_public());
    return out;
}

JsonValue MotionProfileStore::list_profile(const std::string& profile_id) {
    return read_profile(profile_id);
}

JsonValue MotionProfileStore::rename_profile(const std::string& profile_id, const std::string& name) {
    JsonValue profile = read_profile(profile_id);
    const std::string n = trim_copy(name);
    if (n.empty()) throw MotionError("name is required");
    profile.set("name", JsonValue::string(n));
    profile.set("updated_at_ms",
                JsonValue::number(static_cast<double>(static_cast<int64_t>(now_seconds() * 1000))));
    write_profile(profile);
    return public_of(profile);
}

JsonValue MotionProfileStore::remove_profile(const std::string& profile_id) {
    read_profile(profile_id);  // 校验存在
    if (active_id() == profile_id) deactivate();
    std::error_code ec;
    std::filesystem::remove_all(root_ + "/" + profile_id, ec);
    JsonValue out = JsonValue::object();
    out.set("deleted", JsonValue::boolean(true));
    out.set("profile_id", JsonValue::string(profile_id));
    return out;
}

JsonValue MotionProfileStore::clear_samples(const std::string& profile_id) {
    JsonValue profile = read_profile(profile_id);
    profile.set("samples", JsonValue::array());
    JsonValue model = JsonValue::object();
    model.set("schema", JsonValue::string("ttbox.motion-model.v1"));
    model.set("ready", JsonValue::boolean(false));
    model.set("quality", JsonValue::number(0.0));
    model.set("knots", JsonValue::array());
    model.set("exists", JsonValue::boolean(false));
    model.set("sample_count", JsonValue::number(0.0));
    model.set("coverage", JsonValue::object());
    profile.set("model", std::move(model));
    summary(&profile);
    profile.set("updated_at_ms",
                JsonValue::number(static_cast<double>(static_cast<int64_t>(now_seconds() * 1000))));
    write_profile(profile);
    return public_of(profile);
}

std::string MotionProfileStore::active_id() const {
    std::string text;
    if (!read_text(root_ + "/active.json", &text)) return "";
    JsonParseResult r = json_parse(text);
    if (!r.ok || !r.value.is_object()) return "";
    return field_or_null(r.value, "profile_id").as_string("");
}

JsonValue MotionProfileStore::mix() const {
    std::string text;
    if (read_text(root_ + "/active.json", &text)) {
        JsonParseResult r = json_parse(text);
        if (r.ok && r.value.is_object()) {
            const JsonValue& m = field_or_null(r.value, "mix");
            if (m.is_object()) return m;
        }
    }
    JsonValue d = JsonValue::object();
    d.set("curve", JsonValue::number(1.0));
    d.set("speed", JsonValue::number(1.0));
    d.set("reaction", JsonValue::number(0.7));
    d.set("max_reaction_delay_ms", JsonValue::number(250.0));
    return d;
}

}  // namespace ttbox::core::web
