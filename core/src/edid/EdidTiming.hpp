// EdidTiming.hpp — EDID 显示时序库（自 scripts/edid/timing_db.py 逐行移植）。
//
// ★ 为什么存在：V1.0.53 把 EDID 从 Python 移植到 C++（业主令「去掉所有 Python 代码」）。
//   本文件是移植的**取数层**：时序常量表、CVT-RB 像素时钟公式、模式 token 解析。
//   逐字节等价性由 core/tests/test_edid_golden.cpp 对 frozen 黄金样本（185 例）断言。
//
// ★ CPixelClock 公式是**单一真源**（业主 2026-10-06 裁定）：
//   前端 plugins/web/static/panel/10-flow.js 曾另有一份同公式副本（displayDynamicModePixelClockKHz），
//   移植后前端不再自算，一律以后端为准 —— 避免「界面说能跑 165Hz、卡里写的却是另一个数」。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttbox::core::edid {

// 时序描述（对应 Python 的 DisplayTiming dataclass）。
// 派生量（h_blank/h_total/…）用方法而非字段，与 Python 的 @property 同义。
struct DisplayTiming {
    int width = 0;
    int height = 0;
    double refresh = 0.0;
    double pixel_clock = 0.0;  // MHz
    int h_front_porch = 0;
    int h_sync = 0;
    int h_back_porch = 0;
    int v_front_porch = 0;
    int v_sync = 0;
    int v_back_porch = 0;
    bool interlaced = false;
    bool h_pol = true;
    bool v_pol = true;

    int h_active() const { return width; }
    int h_blank() const { return h_front_porch + h_sync + h_back_porch; }
    int h_total() const { return h_active() + h_blank(); }
    int v_active() const { return height; }
    int v_blank() const { return v_front_porch + v_sync + v_back_porch; }
    int v_total() const { return v_active() + v_blank(); }
    int pixel_clock_10khz() const;  // round(pixel_clock * 100)
    std::string label() const;      // "WxH@R"
    std::string token() const;      // 与 label 同值（对齐 Python 的两个 property）

    // 返回空串表示通过；非空为错误说明（对应 Python 的 verify() -> Optional[str]）。
    std::string verify() const;
};

// 内置时序表（键与 Python TIMING_MAP 完全一致，含 4 个 *compat 变体）。
const std::vector<std::pair<std::string, DisplayTiming>>& timing_map();

// 安全模式表：(token, w, h, refresh, pc_khz) —— 对应 Python SAFE_MODES。
struct SafeMode {
    std::string token;
    int width;
    int height;
    int refresh;
    int64_t pixel_clock_khz;
};
const std::vector<SafeMode>& safe_modes();

// 解析结果（对应 Python mode_info 返回的 5 元组）。
struct ModeInfo {
    std::string token;
    int width = 0;
    int height = 0;
    int refresh = 0;
    int64_t pixel_clock_khz = 0;
};

// Python mode_info()：先查 SAFE_MODES，再查 TIMING_MAP，最后按动态正则解析。
// 返回 false 表示 Python 侧的 None（非法 token）。
bool mode_info(const std::string& token, ModeInfo* out);

// Python round()：**银行家舍入**（round-half-to-even）。
// ★ C++ 的 std::round / std::lround 是「远离零」⇒ 遇到正好 .5 会差 1 ⇒ 破坏逐字节等价。
//   凡需要与 Python 输出一致处（像素时钟 / 刷新率的整数化）一律用本函数。
double py_round(double x);

// Python is_dynamic_mode_token()。
bool is_dynamic_mode_token(const std::string& token);

// Python lookup_timing()：内置表直查，否则按 "WxH@R" 反查；未命中返回 false（对应 KeyError）。
bool lookup_timing(const std::string& token, DisplayTiming* out);

// Python reduced_blanking_pixel_clock_khz()：CVT-RB 估算。
// ★ 单一真源 —— 前端不再实现同公式（见文件头）。
int64_t reduced_blanking_pixel_clock_khz(int width, int height, int refresh);

// Python pixel_clock_fits_hdmi_rx()：25MHz ~ 600MHz。
bool pixel_clock_fits_hdmi_rx(int64_t khz);

}  // namespace ttbox::core::edid
