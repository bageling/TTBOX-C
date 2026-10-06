// EdidValidator.cpp — 见 EdidValidator.hpp 的文件头说明。
#include "edid/EdidValidator.hpp"

#include <cctype>
#include <cstdio>

#include "edid/EdidBuilder.hpp"

namespace ttbox::core::edid {

namespace {

std::string hex_of(const uint8_t* p, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += k[(p[i] >> 4) & 0xF];
        s += k[p[i] & 0xF];
    }
    return s;
}

std::string fmt2(const char* f, double a) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, a);
    return std::string(buf);
}

}  // namespace

std::vector<std::string> validate_edid(const std::vector<uint8_t>& edid) {
    std::vector<std::string> errors;
    if (edid.size() != 256) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "长度: 期望 256 字节, 实际 %zu", edid.size());
        errors.emplace_back(buf);
        return errors;
    }
    static const uint8_t kHdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    bool hdr_ok = true;
    for (int i = 0; i < 8; ++i) {
        if (edid[i] != kHdr[i]) {
            hdr_ok = false;
            break;
        }
    }
    if (!hdr_ok) errors.emplace_back("Header 错误: " + hex_of(&edid[0], 8));

    int base_sum = 0;
    for (int i = 0; i < 128; ++i) base_sum += edid[i];
    if (base_sum % 256 != 0) errors.emplace_back("Base checksum 错误");

    int ext_sum = 0;
    for (int i = 128; i < 256; ++i) ext_sum += edid[i];
    if (ext_sum % 256 != 0) errors.emplace_back("Extension checksum 错误");

    if (edid[18] < 1) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "EDID 版本号异常: %d.%d", edid[18], edid[19]);
        errors.emplace_back(buf);
    }
    if ((edid[20] & 0x80) == 0) errors.emplace_back("不是数字信号输入");

    const std::vector<uint8_t> vendor_bytes = {edid[8], edid[9]};
    const std::string vendor = pnp_decode(vendor_bytes);
    bool vendor_ok = (vendor.size() == 3);
    if (vendor_ok) {
        for (char c : vendor) {
            if (!std::isalpha(static_cast<unsigned char>(c))) {
                vendor_ok = false;
                break;
            }
        }
    }
    if (!vendor_ok) errors.emplace_back("厂商代码异常: " + vendor);

    if (edid[126] == 0) errors.emplace_back("无扩展块 (CTA-861 缺失)");
    if (edid[126] > 0 && edid[128] != 0x02) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "扩展块类型不是 CTA-861: 0x%02x", edid[128]);
        errors.emplace_back(buf);
    }
    const int pc = static_cast<int>(edid[54]) | (static_cast<int>(edid[55]) << 8);
    if (pc > 60000) {
        errors.emplace_back("DTD1 像素时钟异常: " + fmt2("%.2f", pc * 10 / 1000.0) + " MHz");
    }
    return errors;
}

bool verify_edid(const std::vector<uint8_t>& edid, std::vector<std::string>* errors) {
    const std::vector<std::string> e = validate_edid(edid);
    if (errors != nullptr) *errors = e;
    return e.empty();
}

}  // namespace ttbox::core::edid
