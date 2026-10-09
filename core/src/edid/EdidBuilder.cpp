// EdidBuilder.cpp — 见 EdidBuilder.hpp 的文件头说明。
#include "edid/EdidBuilder.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "edid/EdidTiming.hpp"

namespace ttbox::core::edid {

namespace {

// Python int(s, 16) 的严格复刻：允许前后空白、可选符号、可选 0x 前缀、下划线分隔；
// 出现任何非法字符即失败。
// ★ 为什么不用 std::stoi：stoi("123abc", base=16) 会**静默**返回 0x123（只解析前缀），
//   而 Python 抛 ValueError。不补这一刀，非法 product_id 会静默变错值。
bool parse_py_int_hex(const std::string& raw, long long* out) {
    size_t i = 0;
    const size_t n = raw.size();
    while (i < n && std::isspace(static_cast<unsigned char>(raw[i]))) i++;
    size_t end = n;
    while (end > i && std::isspace(static_cast<unsigned char>(raw[end - 1]))) end--;
    if (i >= end) return false;
    std::string s = raw.substr(i, end - i);
    size_t p = 0;
    bool neg = false;
    if (s[p] == '+' || s[p] == '-') {
        neg = (s[p] == '-');
        p++;
    }
    if (p + 2 <= s.size() && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X')) p += 2;
    if (p >= s.size()) return false;
    long long v = 0;
    bool saw_digit = false;
    for (; p < s.size(); ++p) {
        const char c = s[p];
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c == '_') continue;  // Python 允许 1_000 形式
        else return false;
        v = v * 16 + d;
        saw_digit = true;
    }
    if (!saw_digit) return false;
    *out = neg ? -v : v;
    return true;
}

// Python bool(x)：字符串非空即 True（含 "false"）—— 与 C++ 直觉相反，必须复刻。
bool json_truthy(const JsonValue* v) {
    if (v == nullptr || v->is_null()) return false;
    if (v->is_bool()) return v->as_bool(false);
    if (v->is_number()) return v->as_number(0.0) != 0.0;
    if (v->is_string()) return !v->as_string("").empty();
    if (v->is_array()) return !v->as_array().empty();
    if (v->is_object()) return !v->as_object().empty();
    return false;
}

// DisplayTiming 按值相等（对应 Python frozen dataclass 的 ==）。
bool timing_equal(const DisplayTiming& a, const DisplayTiming& b) {
    return a.width == b.width && a.height == b.height && a.refresh == b.refresh &&
           a.pixel_clock == b.pixel_clock && a.h_front_porch == b.h_front_porch &&
           a.h_sync == b.h_sync && a.h_back_porch == b.h_back_porch &&
           a.v_front_porch == b.v_front_porch && a.v_sync == b.v_sync &&
           a.v_back_porch == b.v_back_porch && a.interlaced == b.interlaced &&
           a.h_pol == b.h_pol && a.v_pol == b.v_pol;
}

// 判断时序列表 v 中是否已存在与 t 逐字段相等的项。
bool timing_in(const std::vector<DisplayTiming>& v, const DisplayTiming& t) {
    for (const DisplayTiming& x : v) {
        if (timing_equal(x, t)) return true;
    }
    return false;
}

// Python str.strip().upper()[:3] or 'OPI'（vendor 归一）。
std::string upper_ascii(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return o;
}

// Python 的 ascii(..., 'replace')：非 ASCII 字符变 '?'。
// ★ 口径说明：Python 按 **Unicode 字符**替换，C++ 这里按 **UTF-8 字节**替换 ——
//   对多字节字符会产生更多 '?'。正常路径不受影响：配置里的 name/vendor/serial
//   已由 EdidConfig 的 _safe_ascii 过滤为纯 ASCII（与 Python 侧同一条归一链）。
std::vector<uint8_t> ascii_replace(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(c < 0x80 ? c : static_cast<uint8_t>('?'));
    return out;
}

}  // namespace

bool pnp_encode(const std::string& vendor, std::vector<uint8_t>* out) {
    // Python assert: len==3 and isascii() and isalpha()
    if (vendor.size() != 3) return false;
    int c[3];
    for (int i = 0; i < 3; ++i) {
        const unsigned char ch = static_cast<unsigned char>(vendor[i]);
        if (ch >= 0x80) return false;  // isascii()
        if (!std::isalpha(ch)) return false;
        c[i] = static_cast<int>(std::toupper(ch)) - 64;
    }
    const int val = (c[0] << 10) | (c[1] << 5) | c[2];
    if (out != nullptr) {
        out->assign(2, 0);
        (*out)[0] = static_cast<uint8_t>((val >> 8) & 0xFF);  // struct.pack('>H')
        (*out)[1] = static_cast<uint8_t>(val & 0xFF);
    }
    return true;
}

// 2 字节 PnP ID → 3 字母厂商码（不做合法性校验）。
std::string pnp_decode(const std::vector<uint8_t>& data) {
    if (data.size() < 2) return std::string();
    const int val = (static_cast<int>(data[0]) << 8) | static_cast<int>(data[1]);
    std::string r;
    r += static_cast<char>(((val >> 10) & 0x1F) + 64);
    r += static_cast<char>(((val >> 5) & 0x1F) + 64);
    r += static_cast<char>((val & 0x1F) + 64);
    return r;
}

// 计算补码校验字节，使 data 与返回值之和 mod 256 == 0。
uint8_t checksum(const std::vector<uint8_t>& data) {
    int sum = 0;
    for (uint8_t b : data) sum += b;
    return static_cast<uint8_t>((-sum) & 0xFF);
}

// 把 DisplayTiming 打包成 18 字节 DTD；像素时钟超 16bit 上限时返回 false（并填 err）。
bool pack_dtd(const DisplayTiming& t, std::vector<uint8_t>* out, std::string* err) {
    const int pc_10khz = t.pixel_clock_10khz();
    if (pc_10khz > 0xFFFF) {
        if (err != nullptr) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "pixel clock %.3f MHz 超出 DTD 16bit 上限 655.35MHz，"
                          "该模式无法写入 EDID DTD（HDMI 规范限制）",
                          t.pixel_clock);
            *err = buf;
        }
        return false;
    }
    std::vector<uint8_t> buf(18, 0);
    buf[0] = static_cast<uint8_t>(pc_10khz & 0xFF);
    buf[1] = static_cast<uint8_t>((pc_10khz >> 8) & 0xFF);
    const int h_active = t.h_active() & 0xFFF;
    const int h_blank = t.h_blank() & 0xFFF;
    const int v_active = t.v_active() & 0xFFF;
    const int v_blank = t.v_blank() & 0xFFF;
    const int h_sync_offset = t.h_front_porch & 0x3FF;
    const int h_sync_pulse = t.h_sync & 0x3FF;
    const int v_sync_offset = t.v_front_porch & 0x3F;
    const int v_sync_pulse = t.v_sync & 0x3F;
    buf[2] = static_cast<uint8_t>(h_active & 0xFF);
    buf[3] = static_cast<uint8_t>(h_blank & 0xFF);
    buf[4] = static_cast<uint8_t>((((h_active >> 8) & 0xF) << 4) | ((h_blank >> 8) & 0xF));
    buf[5] = static_cast<uint8_t>(v_active & 0xFF);
    buf[6] = static_cast<uint8_t>(v_blank & 0xFF);
    buf[7] = static_cast<uint8_t>((((v_active >> 8) & 0xF) << 4) | ((v_blank >> 8) & 0xF));
    buf[8] = static_cast<uint8_t>(h_sync_offset & 0xFF);
    buf[9] = static_cast<uint8_t>(h_sync_pulse & 0xFF);
    buf[10] =
        static_cast<uint8_t>(((v_sync_offset & 0xF) << 4) | (v_sync_pulse & 0xF));
    buf[11] = static_cast<uint8_t>((((h_sync_offset >> 8) & 0x3) << 6) |
                                   (((h_sync_pulse >> 8) & 0x3) << 4) |
                                   (((v_sync_offset >> 4) & 0x3) << 2) |
                                   ((v_sync_pulse >> 4) & 0x3));
    buf[12] = 0xA6;  // h_image_size lo（RK3588 实测基线）
    buf[13] = 0x7E;
    buf[14] = 0x21;  // v_image_size lo
    buf[15] = 0x00;
    buf[16] = 0;
    uint8_t flags = 0x18;  // 数字 + 分离同步
    if (t.v_pol) flags |= 0x02;
    if (t.h_pol) flags |= 0x01;
    buf[17] = static_cast<uint8_t>(flags | (t.interlaced ? 0x80 : 0x00));
    if (out != nullptr) *out = buf;
    return true;
}

// 生成 18 字节显示器名称描述符（0xFC）。
std::vector<uint8_t> monitor_name_dtd(const std::string& name) {
    std::vector<uint8_t> buf(18, 0);
    buf[3] = 0xFC;
    // Python: name[:12].ljust(12) + '\n'，再 encode('ascii','replace')，取前 13 字节
    std::string head = name.substr(0, name.size() < 12 ? name.size() : 12);
    while (head.size() < 12) head += ' ';
    head += '\n';
    const std::vector<uint8_t> text = ascii_replace(head);
    const size_t cnt = text.size() < 13 ? text.size() : 13;
    for (size_t i = 0; i < cnt; ++i) buf[5 + i] = text[i];
    return buf;
}

// 生成 18 字节序列号描述符（0xFF）。
std::vector<uint8_t> serial_dtd(const std::string& serial_text) {
    std::vector<uint8_t> buf(18, 0);
    buf[3] = 0xFF;
    // Python: serial_text[:11] + '\n ' → 13 字节
    std::string head = serial_text.substr(0, serial_text.size() < 11 ? serial_text.size() : 11);
    head += '\n';
    head += ' ';
    const std::vector<uint8_t> text = ascii_replace(head);
    const size_t cnt = text.size() < 13 ? text.size() : 13;
    for (size_t i = 0; i < cnt; ++i) buf[5 + i] = text[i];
    return buf;
}

// 返回 Established Timings 的 10 字节固定基线。
std::vector<uint8_t> ttbox_established() {
    // bytes.fromhex("cf74a3574cb02309484c")
    static const uint8_t kEst[10] = {0xCF, 0x74, 0xA3, 0x57, 0x4C, 0xB0, 0x23, 0x09, 0x48, 0x4C};
    return std::vector<uint8_t>(kEst, kEst + 10);
}

// 生成 0xFD Range Limits 描述符的基线版（18 字节）。
std::vector<uint8_t> range_limits_dtd_baseline(int min_v, int max_v, int min_h, int max_h,
                                               int max_clock_mhz) {
    std::vector<uint8_t> buf(18, 0);
    buf[3] = 0xFD;
    buf[4] = 0x00;
    buf[5] = static_cast<uint8_t>(min_v & 0xFF);
    buf[6] = static_cast<uint8_t>(max_v & 0xFF);
    buf[7] = static_cast<uint8_t>(min_h & 0xFF);
    buf[8] = static_cast<uint8_t>(max_h & 0xFF);
    // EDID 以 10MHz 为单位，向上取整（向下取整会把 594 声明成 590，部分源端拒绝）
    int pc = (max_clock_mhz + 9) / 10;
    if (pc < 1) pc = 1;
    if (pc > 255) pc = 255;
    buf[9] = static_cast<uint8_t>(pc);
    buf[10] = 0x0A;  // 扩展标志（实测基线）
    return buf;
}

// 按最高可广播时序自适应生成 0xFD Range Limits 描述符。
std::vector<uint8_t> range_limits_dtd(int max_clock_mhz) {
    if (max_clock_mhz == 0) max_clock_mhz = 600;
    if (max_clock_mhz < 25) max_clock_mhz = 25;
    if (max_clock_mhz > 600) max_clock_mhz = 600;
    // Python: max(t for t in TIMING_MAP.values() if int(round(t.pixel_clock)) <= max_clock_mhz,
    //              key=lambda t: t.refresh, default=None)
    // ★ Python max 在并列时取**第一个** ⇒ C++ 必须用 `>` 而非 `>=`。
    const DisplayTiming* best = nullptr;
    for (const auto& kv : timing_map()) {
        const DisplayTiming& t = kv.second;
        const int pc_rounded = static_cast<int>(py_round(t.pixel_clock));
        if (pc_rounded <= max_clock_mhz) {
            if (best == nullptr || t.refresh > best->refresh) best = &t;
        }
    }
    const int refresh = best != nullptr ? static_cast<int>(py_round(best->refresh)) : 240;
    int min_v = refresh - 1;
    if (min_v > 239) min_v = 239;
    if (min_v < 24) min_v = 24;
    int max_v = refresh + 1;
    if (max_v < min_v + 2) max_v = min_v + 2;
    if (max_v > 255) max_v = 255;
    int min_h = 0;
    int max_h = 0;
    if (refresh >= 200) {
        min_h = 254;
        max_h = 255;
    } else {
        if (best != nullptr) {
            // Python: int((max_t.h_total * refresh) / 1000) - 1  （浮点除法后向零取整）
            const double raw =
                (static_cast<double>(best->h_total()) * static_cast<double>(refresh)) / 1000.0;
            int v = static_cast<int>(raw) - 1;
            if (v > 254) v = 254;
            if (v < 24) v = 24;
            min_h = v;
        } else {
            min_h = 24;
        }
        max_h = min_h + 2;
        if (max_h < min_h + 1) max_h = min_h + 1;
        if (max_h > 255) max_h = 255;
    }
    return range_limits_dtd_baseline(min_v, max_v, min_h, max_h, max_clock_mhz);
}

std::vector<uint8_t> hdmi_vsdb(int /*max_tmds_mhz*/) {
    // 逐字节对齐 RK3588 实测成功版（1440p144 协商成功）：
    //   VSDB1 8B: 67 03 0c 00 10 00 00 77
    //   VSDB2 8B: 67 d8 5d c4 01 77 80 00
    static const uint8_t kVsdb[16] = {0x67, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x00, 0x77,
                                      0x67, 0xD8, 0x5D, 0xC4, 0x01, 0x77, 0x80, 0x00};
    return std::vector<uint8_t>(kVsdb, kVsdb + 16);
}

// 组装 128 字节 CTA-861 扩展块（含末尾校验字节）。
std::vector<uint8_t> build_cta_extension() {
    std::vector<uint8_t> ext(128, 0);
    ext[0] = 0x02;  // CTA-861
    ext[1] = 0x03;  // revision
    const std::vector<uint8_t> vsdb = hdmi_vsdb();
    for (size_t i = 0; i < vsdb.size() && (4 + i) < ext.size(); ++i) ext[4 + i] = vsdb[i];
    const int dtd_start = static_cast<int>(4 + vsdb.size());
    ext[2] = static_cast<uint8_t>(dtd_start & 0xFF);
    ext[3] = static_cast<uint8_t>((dtd_start >> 8) & 0xFF);
    // DTD 区留 0（实测基线；DTD0=0 = 无额外 DTD，合法）
    std::vector<uint8_t> head(ext.begin(), ext.begin() + 127);
    ext[127] = checksum(head);
    return ext;
}

// 由配置 JSON 构建完整 256 字节 EDID；失败时置 ok=false 并填 error。
BuildResult build_from_config(const JsonValue& config) {
    BuildResult r;

    // ---- 字段解析（严格对照 EdidBuilder.__init__）----
    auto get_str = [&config](const char* key, const char* def) -> std::string {
        const JsonValue* v = config.find(key);
        if (v == nullptr) return std::string(def);
        // Python str(x)：非字符串走字符串化。此处只处理常见形态。
        if (v->is_string()) return v->as_string(def);
        if (v->is_bool()) return v->as_bool(false) ? "True" : "False";
        if (v->is_number()) {
            char buf[64];
            const double d = v->as_number(0.0);
            if (d == static_cast<double>(static_cast<long long>(d))) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
            } else {
                std::snprintf(buf, sizeof(buf), "%g", d);
            }
            return std::string(buf);
        }
        return std::string(def);
    };

    std::string vendor = get_str("vendor", "OPI");
    // Python: str(...).strip().upper()[:3] or 'OPI'
    {
        size_t b = 0, e = vendor.size();
        while (b < e && std::isspace(static_cast<unsigned char>(vendor[b]))) b++;
        while (e > b && std::isspace(static_cast<unsigned char>(vendor[e - 1]))) e--;
        vendor = upper_ascii(vendor.substr(b, e - b));
        vendor = vendor.substr(0, vendor.size() < 3 ? vendor.size() : 3);
        if (vendor.empty()) vendor = "OPI";
    }

    long long product_id = 0;
    if (!parse_py_int_hex(get_str("product_id", "0x3588"), &product_id)) {
        r.error = "invalid literal for int() with base 16: '" + get_str("product_id", "0x3588") + "'";
        return r;
    }
    long long serial = 0;
    if (!parse_py_int_hex(get_str("serial", "0x20260414"), &serial)) {
        r.error = "invalid literal for int() with base 16: '" + get_str("serial", "0x20260414") + "'";
        return r;
    }

    std::string name = get_str("name", "TTBOX");
    name = name.substr(0, name.size() < 13 ? name.size() : 13);

    const std::string native = get_str("native_mode", "1080p60");
    const bool native_only = json_truthy(config.find("native_only"));

    // ---- 首选时序 ----
    ModeInfo info;
    if (!mode_info(native, &info)) {
        if (!mode_info("1080p60", &info)) {
            r.error = "EDID 生成失败: 内置 1080p60 缺失";
            return r;
        }
    }

    bool have_native_timing = false;
    DisplayTiming native_timing;
    // Python: lookup_timing(info[0]) if info[0] in TIMING_MAP else None
    for (const auto& kv : timing_map()) {
        if (kv.first == info.token) {
            native_timing = kv.second;
            have_native_timing = true;
            break;
        }
    }
    if (!have_native_timing) {
        // 动态模式：用 CVT-RB 像素时钟临时构造（hfp/hs/hbp/vfp/vs/vbp = 48/32/80/3/5/77）
        DisplayTiming t;
        t.width = info.width;
        t.height = info.height;
        t.refresh = static_cast<double>(info.refresh);
        t.pixel_clock = static_cast<double>(reduced_blanking_pixel_clock_khz(
                            info.width, info.height, info.refresh)) /
                        1000.0;
        t.h_front_porch = 48;
        t.h_sync = 32;
        t.h_back_porch = 80;
        t.v_front_porch = 3;
        t.v_sync = 5;
        t.v_back_porch = 77;
        native_timing = t;
    }

    std::vector<DisplayTiming> dtds;
    dtds.push_back(native_timing);

    // added_modes（配置里显式追加）
    const JsonValue* added = config.find("added_modes");
    if (added != nullptr && added->is_array()) {
        for (const JsonValue& tokv : added->as_array()) {
            if (!tokv.is_string()) continue;
            DisplayTiming t;
            if (!lookup_timing(tokv.as_string(""), &t)) continue;  // Python: except KeyError: pass
            if (!timing_equal(t, native_timing) && !timing_in(dtds, t)) dtds.push_back(t);
        }
    }
    if (!native_only && dtds.size() < 3) {
        static const char* kExtra[] = {"1080p60compat", "1440p60", "2160p60"};
        for (const char* tok : kExtra) {
            DisplayTiming t;
            if (!lookup_timing(tok, &t)) continue;
            if (!timing_equal(t, native_timing) && !timing_in(dtds, t)) dtds.push_back(t);
        }
    }

    // ---- 组装 Base Block ----
    std::vector<uint8_t> bb(128, 0);
    static const uint8_t kHdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    for (int i = 0; i < 8; ++i) bb[i] = kHdr[i];

    std::vector<uint8_t> pnp;
    if (!pnp_encode(vendor, &pnp)) {
        r.error = "EDID 生成失败: 非法厂商码 '" + vendor + "'";
        return r;
    }
    bb[8] = pnp[0];
    bb[9] = pnp[1];
    bb[10] = static_cast<uint8_t>(product_id & 0xFF);  // struct.pack_into('<H')
    bb[11] = static_cast<uint8_t>((product_id >> 8) & 0xFF);
    bb[12] = static_cast<uint8_t>(serial & 0xFF);  // '<I'
    bb[13] = static_cast<uint8_t>((serial >> 8) & 0xFF);
    bb[14] = static_cast<uint8_t>((serial >> 16) & 0xFF);
    bb[15] = static_cast<uint8_t>((serial >> 24) & 0xFF);
    bb[16] = 0x01;
    bb[17] = 0x22;
    bb[18] = 1;
    bb[19] = 4;  // EDID 1.4
    bb[20] = 0xA2;
    bb[21] = 0;
    bb[22] = 0;
    bb[23] = 0x78;
    bb[24] = 0x0A;
    {
        const std::vector<uint8_t> est = ttbox_established();
        for (size_t i = 0; i < est.size() && (25 + i) < bb.size(); ++i) bb[25 + i] = est[i];
    }
    bb[35] = 0x00;
    for (int idx = 38; idx < 54; ++idx) bb[idx] = 0x01;

    std::vector<uint8_t> dtd0;
    std::string dtd_err;
    if (!pack_dtd(dtds[0], &dtd0, &dtd_err)) {
        r.error = dtd_err;
        return r;
    }
    for (int i = 0; i < 18; ++i) bb[54 + i] = dtd0[i];

    const std::vector<uint8_t> name_dtd = monitor_name_dtd(name);
    for (int i = 0; i < 18; ++i) bb[72 + i] = name_dtd[i];

    char serial_text_buf[32];
    std::snprintf(serial_text_buf, sizeof(serial_text_buf), "%s%08llX", vendor.c_str(),
                  static_cast<unsigned long long>(serial & 0xFFFFFFFFu));
    std::string serial_text(serial_text_buf);
    serial_text = serial_text.substr(0, serial_text.size() < 13 ? serial_text.size() : 13);
    const std::vector<uint8_t> ser_dtd = serial_dtd(serial_text);
    for (int i = 0; i < 18; ++i) bb[90 + i] = ser_dtd[i];

    // Python: int(max((t.pixel_clock for t in dtds), default=600)) —— 注意是 int() 截断
    double max_pc = 600.0;
    bool first = true;
    for (const DisplayTiming& t : dtds) {
        if (first || t.pixel_clock > max_pc) {
            max_pc = t.pixel_clock;
            first = false;
        }
    }
    const std::vector<uint8_t> rl = range_limits_dtd(static_cast<int>(max_pc));
    for (int i = 0; i < 18; ++i) bb[108 + i] = rl[i];

    bb[126] = 1;  // 1 个扩展块
    {
        std::vector<uint8_t> head(bb.begin(), bb.begin() + 127);
        bb[127] = checksum(head);
    }

    const std::vector<uint8_t> ext = build_cta_extension();
    r.edid = bb;
    r.edid.insert(r.edid.end(), ext.begin(), ext.end());
    if (r.edid.size() != 256) {
        r.error = "EDID 长度异常";
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace ttbox::core::edid
