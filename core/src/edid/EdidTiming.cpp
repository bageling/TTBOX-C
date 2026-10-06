// EdidTiming.cpp — 见 EdidTiming.hpp 的文件头说明。
#include "edid/EdidTiming.hpp"

#include <cmath>
#include <cstdio>
#include <regex>

namespace ttbox::core::edid {

namespace {

std::string fmt_token(int w, int h, int r) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d@%d", w, h, r);
    return std::string(buf);
}

// 动态模式正则：^(\d{3,4})x(\d{3,4})@(\d{2,3})$
// ★ 用 std::regex 而非手写解析，保证与 Python re 的语义严格一致（含 \d 的贪婪 + $ 锚定）。
const std::regex& dynamic_mode_re() {
    static const std::regex re(R"(^(\d{3,4})x(\d{3,4})@(\d{2,3})$)");
    return re;
}

// ^(\d+)x(\d+)@(\d+)$ —— lookup_timing 的反查用（注意：它先 strip）
const std::regex& lookup_re() {
    static const std::regex re(R"(^(\d+)x(\d+)@(\d+)$)");
    return re;
}

}  // namespace

double py_round(double x) {
    // Python round() = 银行家舍入（round-half-to-even）；C++ std::round 是「远离零」。
    // 对拍基准来自 Python ⇒ 必须复刻其语义，否则正好 .5 的取值会差 1。
    const double f = std::floor(x);
    const double d = x - f;
    if (d > 0.5) return f + 1.0;
    if (d < 0.5) return f;
    return (std::fmod(f, 2.0) == 0.0) ? f : f + 1.0;  // 恰好 .5 → 取偶数
}

int DisplayTiming::pixel_clock_10khz() const {
    return static_cast<int>(py_round(pixel_clock * 100.0));
}

std::string DisplayTiming::label() const {
    return fmt_token(width, height, static_cast<int>(py_round(refresh)));
}

std::string DisplayTiming::token() const { return label(); }

std::string DisplayTiming::verify() const {
    if (h_active() <= 0 || h_total() <= h_active()) return "水平参数异常";
    if (v_active() <= 0 || v_total() <= v_active()) return "垂直参数异常";
    if (h_front_porch < 0 || h_sync < 0 || h_back_porch < 0) return "水平空白参数负数";
    if (v_front_porch < 0 || v_sync < 0 || v_back_porch < 0) return "垂直空白参数负数";
    if (pixel_clock <= 0.0 || pixel_clock > 600.0) return "像素时钟超范围 (0-600 MHz)";
    const double actual_hz = (pixel_clock * 1000000.0) /
                             (static_cast<double>(h_total()) * static_cast<double>(v_total()));
    if (std::fabs(actual_hz - refresh) > 1.0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "刷新率不匹配: 计算=%.2fHz, 声称=%.1fHz", actual_hz,
                      refresh);
        return std::string(buf);
    }
    return std::string();
}

const std::vector<std::pair<std::string, DisplayTiming>>& timing_map() {
    // 顺序与 Python TIMING_MAP 字面量一致（lookup_timing 反查时依赖遍历顺序）。
    static const std::vector<std::pair<std::string, DisplayTiming>> kMap = [] {
        std::vector<std::pair<std::string, DisplayTiming>> v;
        auto add = [&v](const char* name, DisplayTiming t) {
            v.emplace_back(std::string(name), t);
        };

        DisplayTiming t1080p60;
        t1080p60.width = 1920; t1080p60.height = 1080; t1080p60.refresh = 60.0;
        t1080p60.pixel_clock = 148.5; t1080p60.h_front_porch = 88; t1080p60.h_sync = 44;
        t1080p60.h_back_porch = 148; t1080p60.v_front_porch = 4; t1080p60.v_sync = 5;
        t1080p60.v_back_porch = 36;
        add("1080p60", t1080p60);

        DisplayTiming t1080p60c = t1080p60;
        t1080p60c.pixel_clock = 145.392;
        add("1080p60compat", t1080p60c);

        DisplayTiming t1080p90 = t1080p60;
        t1080p90.refresh = 90.0; t1080p90.pixel_clock = 222.75;
        add("1080p90", t1080p90);

        DisplayTiming t1080p120 = t1080p60;
        t1080p120.refresh = 120.0; t1080p120.pixel_clock = 297.0;
        add("1080p120", t1080p120);

        DisplayTiming t1080p120c = t1080p60;
        t1080p120c.refresh = 120.0; t1080p120c.pixel_clock = 290.784;
        add("1080p120compat", t1080p120c);

        DisplayTiming t1080p144;
        t1080p144.width = 1920; t1080p144.height = 1080; t1080p144.refresh = 144.0;
        t1080p144.pixel_clock = 348.941; t1080p144.h_front_porch = 48; t1080p144.h_sync = 32;
        t1080p144.h_back_porch = 80; t1080p144.v_front_porch = 3; t1080p144.v_sync = 5;
        t1080p144.v_back_porch = 77;
        add("1080p144", t1080p144);

        DisplayTiming t1080p240 = t1080p60;
        t1080p240.refresh = 240.0; t1080p240.pixel_clock = 594.0;
        t1080p240.h_pol = false; t1080p240.v_pol = false;
        add("1080p240", t1080p240);

        DisplayTiming t1080p240c = t1080p60;
        t1080p240c.refresh = 240.0; t1080p240c.pixel_clock = 581.568;
        add("1080p240compat", t1080p240c);

        DisplayTiming t1440p60;
        t1440p60.width = 2560; t1440p60.height = 1440; t1440p60.refresh = 60.0;
        t1440p60.pixel_clock = 248.87; t1440p60.h_front_porch = 48; t1440p60.h_sync = 32;
        t1440p60.h_back_porch = 80; t1440p60.v_front_porch = 3; t1440p60.v_sync = 5;
        t1440p60.v_back_porch = 77;
        add("1440p60", t1440p60);

        DisplayTiming t1440p120 = t1440p60;
        t1440p120.refresh = 120.0; t1440p120.pixel_clock = 497.75;
        add("1440p120", t1440p120);

        DisplayTiming t1440p144 = t1440p60;
        t1440p144.refresh = 144.0; t1440p144.pixel_clock = 586.345;
        t1440p144.v_back_porch = 49; t1440p144.h_pol = false; t1440p144.v_pol = false;
        add("1440p144", t1440p144);

        DisplayTiming t1440p165 = t1440p60;
        t1440p165.refresh = 165.0; t1440p165.pixel_clock = 663.75;
        t1440p165.v_back_porch = 49;
        add("1440p165", t1440p165);

        DisplayTiming t2160p60;
        t2160p60.width = 3840; t2160p60.height = 2160; t2160p60.refresh = 60.0;
        t2160p60.pixel_clock = 594.0; t2160p60.h_front_porch = 176; t2160p60.h_sync = 88;
        t2160p60.h_back_porch = 296; t2160p60.v_front_porch = 8; t2160p60.v_sync = 10;
        t2160p60.v_back_porch = 72; t2160p60.h_pol = false; t2160p60.v_pol = false;
        add("2160p60", t2160p60);

        return v;
    }();
    return kMap;
}

const std::vector<SafeMode>& safe_modes() {
    static const std::vector<SafeMode> kModes = {
        {"2160p60", 3840, 2160, 60, 594000},
        {"1440p144", 2560, 1440, 144, 586345},
        {"1440p120", 2560, 1440, 120, 497750},
        {"1440p60", 2560, 1440, 60, 248875},
        {"1080p240compat", 1920, 1080, 240, 581568},
        {"1080p144", 1920, 1080, 144, 348941},
        {"1080p120", 1920, 1080, 120, 297000},
        {"1080p60compat", 1920, 1080, 60, 145392},
    };
    return kModes;
}

int64_t reduced_blanking_pixel_clock_khz(int width, int height, int refresh) {
    if (width <= 0 || height <= 0 || refresh <= 0) return 0;
    // Python: ((w+48+32+80) * (h+3+5+77) * r + 500) // 1000   （整数地板除）
    const int64_t hb = static_cast<int64_t>(width) + 48 + 32 + 80;
    const int64_t vb = static_cast<int64_t>(height) + 3 + 5 + 77;
    const int64_t prod = hb * vb * static_cast<int64_t>(refresh);
    return (prod + 500) / 1000;  // 两操作数皆非负 ⇒ C++ 整除即 Python 地板除
}

bool pixel_clock_fits_hdmi_rx(int64_t khz) { return khz >= 25000 && khz <= 600000; }

bool is_dynamic_mode_token(const std::string& token) {
    return std::regex_match(token, dynamic_mode_re());
}

bool mode_info(const std::string& token, ModeInfo* out) {
    // 1) SAFE_MODES 优先（与 Python 同序）
    for (const SafeMode& m : safe_modes()) {
        if (m.token == token) {
            if (out != nullptr) {
                out->token = m.token;
                out->width = m.width;
                out->height = m.height;
                out->refresh = m.refresh;
                out->pixel_clock_khz = m.pixel_clock_khz;
            }
            return true;
        }
    }
    // 2) TIMING_MAP
    for (const auto& kv : timing_map()) {
        if (kv.first == token) {
            const DisplayTiming& t = kv.second;
            if (out != nullptr) {
                out->token = kv.first;
                out->width = t.width;
                out->height = t.height;
                out->refresh = static_cast<int>(py_round(t.refresh));
                out->pixel_clock_khz = static_cast<int64_t>(py_round(t.pixel_clock * 1000.0));
            }
            return true;
        }
    }
    // 3) 动态 WxH@Hz
    std::smatch m;
    if (!std::regex_match(token, m, dynamic_mode_re())) return false;
    const int width = std::stoi(m[1].str());
    const int height = std::stoi(m[2].str());
    const int refresh = std::stoi(m[3].str());
    if (width < 640 || width > 4095 || height < 400 || height > 4095 || refresh < 24 ||
        refresh > 360) {
        return false;
    }
    const int64_t pc_khz = reduced_blanking_pixel_clock_khz(width, height, refresh);
    if (!pixel_clock_fits_hdmi_rx(pc_khz)) return false;
    if (out != nullptr) {
        out->token = fmt_token(width, height, refresh);
        out->width = width;
        out->height = height;
        out->refresh = refresh;
        out->pixel_clock_khz = pc_khz;
    }
    return true;
}

bool lookup_timing(const std::string& token, DisplayTiming* out) {
    // 内置表直查
    for (const auto& kv : timing_map()) {
        if (kv.first == token) {
            if (out != nullptr) *out = kv.second;
            return true;
        }
    }
    // "WxH@R" 反查（注意 Python 此处先 strip，与 mode_info 不同）
    std::string s = token;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' ||
                          s.back() == '\r' || s.back() == '\f' || s.back() == '\v')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' ||
                            s[i] == '\f' || s[i] == '\v')) {
        i++;
    }
    const std::string stripped = s.substr(i);
    std::smatch m;
    if (std::regex_match(stripped, m, lookup_re())) {
        const int w = std::stoi(m[1].str());
        const int h = std::stoi(m[2].str());
        const int r = std::stoi(m[3].str());
        for (const auto& kv : timing_map()) {
            const DisplayTiming& t = kv.second;
            if (t.width == w && t.height == h &&
                static_cast<int>(py_round(t.refresh)) == r) {
                if (out != nullptr) *out = t;
                return true;
            }
        }
    }
    return false;  // 对应 Python 的 KeyError
}

}  // namespace ttbox::core::edid
