// EdidBuilder.hpp — EDID 256 字节构造（自 scripts/edid/builder.py 逐行移植）。
//
// 输出 = Base Block(128B) + CTA-861 Extension(128B)。
// 逐字节等价性由 core/tests/test_edid_golden.cpp 对 frozen 黄金样本断言。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/Json.hpp"

namespace ttbox::core::edid {

// 构建结果。Python 侧失败时抛异常（ValueError/AssertionError），C++ 用 ok=false + error 表达。
// ★ 对拍口径：只断言 ok 与成功时的 256 字节；error 文案不参与对拍（不是接口）。
struct BuildResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> edid;
};

// Python build_from_config(config: dict) -> bytes
// config 键：vendor / product_id / serial / name / native_mode / native_only / added_modes
BuildResult build_from_config(const JsonValue& config);

// ---- 以下为 Python 模块级函数，公开以便单测逐项对拍 ----

// 3 字母厂商码 → 2 字节 PnP ID。非 [A-Z]{3} 时返回 false（Python 侧是 assert 失败）。
bool pnp_encode(const std::string& vendor, std::vector<uint8_t>* out);

// 2 字节 PnP ID → 3 字母。
std::string pnp_decode(const std::vector<uint8_t>& data);

// 使 block 所有字节之和 mod 256 = 0。
uint8_t checksum(const std::vector<uint8_t>& data);

// 18 字节 DTD。像素时钟超 DTD 16bit 上限（655.35MHz）时返回 false。
bool pack_dtd(const struct DisplayTiming& t, std::vector<uint8_t>* out, std::string* err);

// 显示器名称描述符（0xFC）。
std::vector<uint8_t> monitor_name_dtd(const std::string& name);

// 序列号描述符（0xFF）。
std::vector<uint8_t> serial_dtd(const std::string& serial_text);

// Established timings（byte25-34，10 字节，逐字节对齐 RK3588 实测成功版）。
std::vector<uint8_t> ttbox_established();

// Range Limits 描述符（0xFD）—— 按最高广播时序自适应。
std::vector<uint8_t> range_limits_dtd(int max_clock_mhz);

// Range Limits 基线版（0xFD）—— 逐字节对齐实测 1440p144 成功版。
std::vector<uint8_t> range_limits_dtd_baseline(int min_v = 239, int max_v = 241, int min_h = 254,
                                               int max_h = 255, int max_clock_mhz = 600);

// HDMI VSDB（HDMI1.4 VSDB 8B + HF-VSDB 8B），逐字节对齐实测。
std::vector<uint8_t> hdmi_vsdb(int max_tmds_mhz = 600);

// CTA-861 扩展块（128B）。
std::vector<uint8_t> build_cta_extension();

}  // namespace ttbox::core::edid
