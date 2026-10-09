// EdidConfig.cpp — 见 EdidConfig.hpp 的文件头说明。
#include "edid/EdidConfig.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <set>

#include "edid/EdidTiming.hpp"

namespace ttbox::core::edid {

namespace {

// Python int(text, base) 的严格复刻（base ∈ {10,16}）：
// 允许前后空白、可选符号、可选下划线分隔；出现任何非法字符即失败。
// ★ 为什么不用 std::stoi：stoi("123abc", base) 会**静默**只解析前缀并返回，
//   而 Python 抛 ValueError ⇒ 不补这刀，非法值会静默变成另一个合法值。
bool parse_py_int_base(const std::string& raw, int base, long long* out) {
    size_t i = 0;
    const size_t n = raw.size();
    while (i < n && std::isspace(static_cast<unsigned char>(raw[i]))) i++;
    size_t end = n;
    while (end > i && std::isspace(static_cast<unsigned char>(raw[end - 1]))) end--;
    if (i >= end) return false;
    const std::string s = raw.substr(i, end - i);
    size_t p = 0;
    bool neg = false;
    if (s[p] == '+' || s[p] == '-') {
        neg = (s[p] == '-');
        p++;
    }
    if (base == 16 && p + 2 <= s.size() && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X')) {
        p += 2;
    }
    if (p >= s.size()) return false;
    long long v = 0;
    bool saw = false;
    for (; p < s.size(); ++p) {
        const char c = s[p];
        if (c == '_') continue;
        int d = -1;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return false;
        }
        if (d >= base) return false;
        v = v * base + d;
        saw = true;
    }
    if (!saw) return false;
    *out = neg ? -v : v;
    return true;
}

// ASCII 转小写。
std::string to_lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return o;
}

// ASCII 转大写。
std::string to_upper(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return o;
}

// 判是否为恰好 3 个大写字母（合规 PnP 厂商码）。
bool ascii_is_upper3(const std::string& s) {
    if (s.size() != 3) return false;
    for (char c : s) {
        if (c < 'A' || c > 'Z') return false;
    }
    return true;
}

// 去重追加：token 非空且未出现过才推入。
void add_unique(std::vector<std::string>* v, const std::string& token) {
    if (token.empty()) return;
    for (const std::string& x : *v) {
        if (x == token) return;
    }
    v->push_back(token);
}

// 判列表 v 中是否含字符串 s。
bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v) {
        if (x == s) return true;
    }
    return false;
}

// Python _bool_value 的"字符串形态"分支（数字/布尔分支在公共函数里处理）。
bool bool_from_text(const std::string& raw, bool fallback) {
    // str(value or "").strip().lower()
    size_t b = 0, e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) e--;
    const std::string t = to_lower(raw.substr(b, e - b));
    if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
    if (t == "0" || t == "false" || t == "no" || t == "off") return false;
    return fallback;
}

// Python str(value or "")：此处只覆盖 JSON 标量（string/number/bool），其余按 def。
std::string json_to_py_str(const JsonValue* v, const std::string& def) {
    if (v == nullptr || v->is_null()) return std::string();  // str(None or "") == ""
    if (v->is_string()) return v->as_string("");
    if (v->is_bool()) return v->as_bool(false) ? "True" : "False";
    if (v->is_number()) {
        const double d = v->as_number(0.0);
        char buf[64];
        if (d == static_cast<double>(static_cast<long long>(d))) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        } else {
            std::snprintf(buf, sizeof(buf), "%g", d);
        }
        return std::string(buf);
    }
    return def;
}

// edid_apply.sh::PYEOF 里的 PROFILE_FIRST 映射（原为 shell 内联字典，此处成为唯一真源）。
const std::vector<std::pair<std::string, std::string>>& profile_first() {
    static const std::vector<std::pair<std::string, std::string>> kMap = {
        {"boot-safe-1080p240", "1080p240compat"}, {"boot-safe-full", "1080p60compat"},
        {"standard-dual", "1080p120"},            {"single-1440p60", "1440p60"},
        {"single-1080p120", "1080p120"},          {"single-1080p144", "1080p144"},
        {"single-1080p240", "1080p240"},          {"single-1440p144", "1440p144"},
        {"single-2160p60", "2160p60"},
    };
    return kMap;
}

}  // namespace

// ---------------- 归一工具 ----------------

std::string safe_ascii(const std::string& value, size_t limit, const std::string& fallback) {
    std::string out;
    for (unsigned char c : value) {
        if (c >= 32 && c <= 126) out += static_cast<char>(c);
        if (out.size() >= limit) break;
    }
    if (out.size() > limit) out = out.substr(0, limit);
    return out.empty() ? fallback : out;
}

std::string hex_text(const std::string& value, int width, const std::string& fallback) {
    // text = str(value or "").strip()
    size_t b = 0, e = value.size();
    while (b < e && std::isspace(static_cast<unsigned char>(value[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(value[e - 1]))) e--;
    const std::string text = value.substr(b, e - b);
    const int base = (to_lower(text).rfind("0x", 0) == 0) ? 16 : 10;
    long long number = 0;
    if (!parse_py_int_base(text, base, &number)) return fallback;
    const long long limit = 1LL << (width * 4);
    if (number <= 0 || number >= limit) return fallback;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%0*llx", width, number);
    return std::string(buf);
}

bool bool_value(const JsonValue& value, bool fallback) {
    if (value.is_bool()) return value.as_bool(fallback);
    if (value.is_number()) return value.as_number(0.0) != 0.0;
    if (value.is_string()) return bool_from_text(value.as_string(""), fallback);
    return fallback;
}

// ---------------- 常量表 ----------------

const std::vector<std::string>& profiles_set() {
    static const std::vector<std::string> kSet = {
        "boot-safe-1080p240", "boot-safe-full",         "standard-dual",
        "single-1440p60",     "single-1080p120-compat", "single-1080p60-compat"};
    return kSet;
}

const std::vector<std::string>& native_modes_set() {
    static const std::vector<std::string> kSet = {
        "",           "1080p60",    "1080p60compat", "1080p90",   "1080p120",
        "1080p120compat", "1080p144", "1080p240",      "1080p240compat",
        "1440p60",    "1440p120",   "1440p144",      "2160p60"};
    return kSet;
}

int max_advertised_modes() { return 6; }

JsonValue advertised_modes_json() {
    // 自旧 Python hardware.py::_probe_edid_modes 移植。那一版解析 `hdmirx_edid --list`
    // 的文本，而该命令逐行打印的正是 TIMING_MAP（见 hdmirx_edid.py::cmd_list）
    // ⇒ 这里直接由时序表生成，等价且免去外部进程与文本解析。
    std::vector<JsonValue> out;
    out.reserve(timing_map().size());
    for (const auto& kv : timing_map()) {
        const DisplayTiming& t = kv.second;
        JsonValue e = JsonValue::object();
        e.set("token", JsonValue::string(kv.first));
        // label = "宽x高@刷新"（旧版从 --list 行里正则取 (\d+)x(\d+)@(\d+)）
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%dx%d@%d", t.width, t.height,
                      static_cast<int>(t.refresh));
        e.set("label", JsonValue::string(buf));
        e.set("width", JsonValue::number(static_cast<double>(t.width)));
        e.set("height", JsonValue::number(static_cast<double>(t.height)));
        e.set("refresh", JsonValue::number(static_cast<double>(static_cast<int>(t.refresh))));
        e.set("pixel_clock_khz",
              JsonValue::number(static_cast<double>(py_round(t.pixel_clock * 1000.0))));
        out.push_back(std::move(e));
    }
    return JsonValue::array(std::move(out));
}

JsonValue advertised_modes_json_truncated(size_t limit) {
    JsonValue all = advertised_modes_json();
    const std::vector<JsonValue>& arr = all.as_array();
    if (arr.size() <= limit) return all;
    std::vector<JsonValue> cut(arr.begin(), arr.begin() + static_cast<long>(limit));
    return JsonValue::array(std::move(cut));
}

// ---------------- E 段：native_mode 保护 ----------------

std::string resolve_native_mode(const std::string& profile, const std::string& native_mode) {
    ModeInfo mi;
    if (!native_mode.empty() && native_mode != "auto" && mode_info(native_mode, &mi)) {
        return native_mode;
    }
    for (const auto& kv : profile_first()) {
        if (kv.first == profile) return kv.second;
    }
    return "1080p60compat";
}

// ---------------- 配置归一（load_config）----------------

DisplayConfig load_config(const JsonValue& raw) {
    DisplayConfig c;

    // device：仅接受 "auto" 或 /dev/videoN
    {
        static const std::regex kDev(R"(^/dev/video\d+$)");
        const std::string dev = safe_ascii(json_to_py_str(raw.find("device"), "auto"), 48, "auto");
        c.device = (dev == "auto" || std::regex_match(dev, kDev)) ? dev : "auto";
    }

    // profile：必须在白名单内
    {
        const std::string p =
            safe_ascii(json_to_py_str(raw.find("profile"), "boot-safe-full"), 32, "boot-safe-full");
        c.profile = contains(profiles_set(), p) ? p : "boot-safe-full";
    }

    // native_mode："" | "auto"(→"") | 白名单内 | mode_info 可解析；否则 ""
    {
        std::string nm = safe_ascii(json_to_py_str(raw.find("native_mode"), ""), 32, "");
        if (nm == "auto") {
            nm.clear();
        } else if (!nm.empty() && !contains(native_modes_set(), nm)) {
            ModeInfo mi;
            if (!mode_info(nm, &mi)) nm.clear();
        }
        c.native_mode = nm;
    }

    const JsonValue* no = raw.find("native_only");
    c.native_only = no != nullptr ? bool_value(*no, false) : false;
    const JsonValue* lo = raw.find("loopout_enabled");
    c.loopout_enabled = lo != nullptr ? bool_value(*lo, false) : false;

    c.name = safe_ascii(json_to_py_str(raw.find("name"), "OPI-COMPAT"), 13, "OPI-COMPAT");
    {
        std::string v = to_upper(safe_ascii(json_to_py_str(raw.find("vendor"), "OPI"), 3, "OPI"));
        if (!ascii_is_upper3(v)) v = "OPI";
        c.vendor = v;
    }
    c.product_id = hex_text(json_to_py_str(raw.find("product_id"), "0x3588"), 4, "0x3588");
    c.serial = hex_text(json_to_py_str(raw.find("serial"), "0x20260414"), 8, "0x20260414");
    return c;
}

JsonValue DisplayConfig::to_json() const {
    JsonValue o = JsonValue::object();
    o.set("device", JsonValue::string(device));
    o.set("profile", JsonValue::string(profile));
    o.set("native_mode", JsonValue::string(native_mode));
    o.set("native_only", JsonValue::boolean(native_only));
    o.set("loopout_enabled", JsonValue::boolean(loopout_enabled));
    o.set("name", JsonValue::string(name));
    o.set("vendor", JsonValue::string(vendor));
    o.set("product_id", JsonValue::string(product_id));
    o.set("serial", JsonValue::string(serial));
    if (!added_modes.empty()) {
        std::vector<JsonValue> arr;
        arr.reserve(added_modes.size());
        for (const std::string& m : added_modes) arr.push_back(JsonValue::string(m));
        o.set("added_modes", JsonValue::array(std::move(arr)));
    }
    return o;
}

// ---------------- 模式策略 ----------------

// 判显示器模式列表里是否存在该宽高的分辨率。
bool monitor_has_resolution(const MonitorInfo& m, int width, int height) {
    for (const MonitorMode& mm : m.modes) {
        if (mm.width == width && mm.height == height) return true;
    }
    return false;
}

// 判显示器在该宽高下是否支持 ≥ refresh（refresh==0 的未知项按 ≤60Hz 规则特判）。
bool monitor_supports_refresh(const MonitorInfo& m, int width, int height, int refresh) {
    bool saw_unknown = false;
    for (const MonitorMode& mm : m.modes) {
        if (mm.width != width || mm.height != height) continue;
        const int r = mm.refresh;
        if (r == 0) {
            saw_unknown = true;
            continue;
        }
        if (r + 1 >= refresh) return true;
    }
    return saw_unknown && refresh <= 60;
}

// 判 (width,height) 是否不超过显示器原生分辨率（原生未知时放行）。
bool at_or_below_native(const MonitorInfo& m, int width, int height) {
    if (m.native_width <= 0 || m.native_height <= 0) return true;
    return width <= m.native_width && height <= m.native_height;
}

namespace {

// 由显示器报告的模式生成去重后的 "WxH@R" token 列表。
std::vector<std::string> available_tokens_for_monitor(const MonitorInfo& m) {
    std::vector<std::string> tokens;
    for (const MonitorMode& mm : m.modes) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%dx%d@%d", mm.width, mm.height, mm.refresh);
        add_unique(&tokens, std::string(buf));
    }
    return tokens;
}

// 判 token 是否为可解析的模式（内置表或动态 WxH@R）。
bool dynamic_or_known(const std::string& token) {
    ModeInfo mi;
    return mode_info(token, &mi);
}

}  // namespace

std::vector<std::string> build_display_mode_tokens(const DisplayConfig& cfg,
                                                   const MonitorInfo& monitor) {
    const std::string requested_native = cfg.native_mode;
    const bool native_only = cfg.native_only;

    // 分支 1：已连接但 EDID 无效
    if (monitor.connected && !monitor.edid_valid) {
        std::vector<std::string> tokens;
        if (dynamic_or_known(requested_native)) add_unique(&tokens, requested_native);
        if (tokens.empty()) add_unique(&tokens, "1080p60compat");
        if (!native_only) add_unique(&tokens, "1080p60compat");
        return tokens;
    }

    const std::vector<std::string> available = available_tokens_for_monitor(monitor);
    std::vector<std::string> safe_tokens;
    if (!(monitor.connected && !available.empty())) {
        for (const SafeMode& sm : safe_modes()) {
            if (at_or_below_native(monitor, sm.width, sm.height) &&
                monitor_has_resolution(monitor, sm.width, sm.height) &&
                monitor_supports_refresh(monitor, sm.width, sm.height, sm.refresh)) {
                safe_tokens.push_back(sm.token);
            }
        }
    }
    if (safe_tokens.empty()) safe_tokens.push_back("1080p60compat");
    if (!monitor.connected) return safe_tokens;

    std::vector<std::string> tokens;
    ModeInfo req;
    const bool req_ok = mode_info(requested_native, &req);
    if (req_ok && (is_dynamic_mode_token(requested_native) ||
                   contains(available, requested_native) || contains(safe_tokens, requested_native) ||
                   (available.empty() && at_or_below_native(monitor, req.width, req.height) &&
                    monitor_has_resolution(monitor, req.width, req.height)))) {
        add_unique(&tokens, requested_native);
    }
    if (tokens.empty() && !available.empty()) {
        add_unique(&tokens, available[0]);
    }
    if (tokens.empty()) add_unique(&tokens, safe_tokens[0]);
    if (!native_only) {
        for (const std::string& t : available) {
            add_unique(&tokens, t);
            if (static_cast<int>(tokens.size()) >= max_advertised_modes()) break;
        }
        if (available.empty() && tokens.size() < 2) {
            for (const std::string& t : safe_tokens) {
                add_unique(&tokens, t);
                if (tokens.size() >= 2) break;
            }
        }
        if (static_cast<int>(tokens.size()) > max_advertised_modes()) {
            tokens.resize(static_cast<size_t>(max_advertised_modes()));
        }
    }
    if (tokens.empty()) tokens.push_back("1080p60compat");
    return tokens;
}

DisplayConfig apply_monitor_to_config(DisplayConfig cfg, const MonitorInfo& monitor) {
    if (!cfg.loopout_enabled) return cfg;
    if (!(monitor.connected && monitor.edid_valid)) return cfg;

    cfg.name = safe_ascii(monitor.name, 13, cfg.name);
    {
        const std::string v = to_upper(safe_ascii(monitor.vendor, 3, cfg.vendor));
        if (ascii_is_upper3(v)) cfg.vendor = v;
    }
    cfg.product_id = hex_text(monitor.product_id, 4, cfg.product_id);
    cfg.serial = hex_text(monitor.serial, 8, cfg.serial);

    const std::vector<std::string> tokens = build_display_mode_tokens(cfg, monitor);
    if (!tokens.empty()) {
        cfg.native_mode = tokens[0];
        cfg.added_modes.assign(tokens.begin() + 1, tokens.end());
    }
    return cfg;
}

}  // namespace ttbox::core::edid
