// CrosshairProbe.hpp — 准星找色（急停检测用）：在画面中心附近找指定颜色的准星
//
// 来源与定位：
//   HEX 3.2.9 的 `准星数据.json` 是「HSV 上下界 + 红色跨 0 环绕」的取色表，
//   用来做「准星变色 = 该停手」的判定。TTBOX 的 trigger2 早已有 stop_detect_* 四个配置项
//   （enabled / color_id / tolerance / range / interval），但 core 侧一直**恒真**——
//   因为 AimThread 只收 AimTargetMailbox 的小型任务（"任务只含小型检测结果，不传图像"），
//   拿不到像素。本模块把取色放在**有帧的那一侧**（推理 worker），只把 bool 结果随任务带回。
//
// 语义（与配置项一一对应）：
//   color_id   —— 内置色表下标（见 crosshair_color_key），不是任意 HSV。
//   tolerance  —— H/S/V 三通道的容差（H 单位是 OpenCV 尺度 0..179 的 1/2 度，S/V 是 0..255）。
//   range_px   —— 以画面中心为原点的正方形半边长（像素，采集帧原图坐标系）。
//   interval   —— 每 N 帧才真取一次色（取色要读几十个像素，别每帧都做）。
//
// 只支持 core 实际会拿到的格式：NV12（V4L2 采集主路径）、RGB888、BGR888、RGBA8888。
// 未知格式一律返回 false（fail-closed：判不出颜色 = 不算命中）。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "common/Types.hpp"

namespace ttbox::core::aim {

struct CrosshairProbeParams {
    bool enabled = false;
    int color_id = 2;             // 默认青色（与 RuntimeProfile 的默认值一致）
    float tolerance = 60.0f;      // H/S/V 容差
    float range_px = 80.0f;       // 半边长（px）
    int interval_frames = 10;     // 每 N 帧取一次
};

// 内置色表（H 用 OpenCV 尺度 0..179；S/V 用 0..255）。
// wrap_h=true 表示色相跨 0 边界（红色 170..179 与 0..10 都算红）。
struct CrosshairColorKey {
    float h = 0.0f;
    float s = 255.0f;
    float v = 255.0f;
    bool wrap_h = false;
    const char* name = "未知";
};

inline CrosshairColorKey crosshair_color_key(int color_id) {
    switch (color_id) {
        case 0: return CrosshairColorKey{0.0f, 255.0f, 255.0f, true, "红色准星"};
        case 1: return CrosshairColorKey{75.0f, 255.0f, 255.0f, false, "绿色准星"};
        case 2: return CrosshairColorKey{90.0f, 255.0f, 255.0f, false, "青色准星"};
        case 3: return CrosshairColorKey{30.0f, 255.0f, 255.0f, false, "黄色准星"};
        case 4: return CrosshairColorKey{0.0f, 40.0f, 255.0f, false, "白色准星"};
        case 5: return CrosshairColorKey{150.0f, 255.0f, 255.0f, false, "粉色准星"};
        default: return CrosshairColorKey{90.0f, 255.0f, 255.0f, false, "青色准星"};
    }
}

// RGB(0..255) → HSV（H 尺度 0..179，与 HEX / OpenCV 一致）
inline void rgb_to_hsv(float r, float g, float b, float* h, float* s, float* v) {
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d = mx - mn;
    *v = mx;
    if (mx <= 0.0f) { *h = 0.0f; *s = 0.0f; return; }
    *s = d * 255.0f / mx;
    if (d <= 0.0f) { *h = 0.0f; return; }
    float hh = 0.0f;
    if (mx == r) hh = (g - b) / d;
    else if (mx == g) hh = 2.0f + (b - r) / d;
    else hh = 4.0f + (r - g) / d;
    hh *= 60.0f;                     // ×60 ⇒ 0..360 度（OpenCV 公式的度）
    if (hh < 0.0f) hh += 360.0f;
    *h = hh * 0.5f;                  // 折半到 0..179（与 HEX / OpenCV 同尺度）
}

inline bool hsv_matches(float h, float s, float v, const CrosshairColorKey& key, float tol) {
    if (v < key.v - tol) return false;       // 太暗一律不算（准星是亮色）
    if (std::fabs(v - key.v) > tol && v < key.v) return false;
    if (std::fabs(s - key.s) > tol && s < key.s) return false;
    // 白色这类「低饱和」目标：只要求 s 不超过上限，不要求接近
    if (key.s <= 60.0f) {
        if (s > key.s + tol) return false;
        return true;
    }
    if (s < key.s - tol) return false;
    const float tol_h = std::max(1.0f, tol * 0.5f);  // H 单位更细，容差同步收一半
    if (key.wrap_h) {
        const float d1 = std::fabs(h - key.h);
        const float d2 = 179.0f - d1;
        return std::min(d1, d2) <= tol_h;
    }
    return std::fabs(h - key.h) <= tol_h;
}

// 取一个像素的 RGB（返回 false = 越界/格式不支持）。NV12 的 UV 是 2x2 共享。
inline bool sample_rgb(const uint8_t* data, const FrameInfo& info, int x, int y,
                       float* r, float* g, float* b) {
    if (!data || x < 0 || y < 0) return false;
    const int w = static_cast<int>(info.width);
    const int h = static_cast<int>(info.height);
    if (x >= w || y >= h) return false;
    const uint32_t stride = info.stride > 0 ? info.stride : info.width;
    switch (info.format) {
        case PixelFormat::kRGB888: {
            const size_t off = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
            *r = data[off]; *g = data[off + 1]; *b = data[off + 2];
            return true;
        }
        case PixelFormat::kBGR888: {
            const size_t off = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
            *b = data[off]; *g = data[off + 1]; *r = data[off + 2];
            return true;
        }
        case PixelFormat::kRGBA8888: {
            const size_t off = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
            *r = data[off]; *g = data[off + 1]; *b = data[off + 2];
            return true;
        }
        case PixelFormat::kNV12: {
            const size_t y_off = static_cast<size_t>(y) * stride + static_cast<size_t>(x);
            const float yy = static_cast<float>(data[y_off]);
            // UV plane 紧跟 Y plane（单 buffer 布局），2x2 共享一组 UV
            const size_t uv_base = static_cast<size_t>(stride) * static_cast<size_t>(h);
            const size_t uv_off = uv_base + static_cast<size_t>(y / 2) * stride +
                                  static_cast<size_t>(x / 2) * 2;
            const float u = static_cast<float>(data[uv_off]) - 128.0f;
            const float vv = static_cast<float>(data[uv_off + 1]) - 128.0f;
            *r = yy + 1.402f * vv;
            *g = yy - 0.344136f * u - 0.714136f * vv;
            *b = yy + 1.772f * u;
            return true;
        }
        default:
            return false;
    }
}

// 在中心 ±range_px 的方形里网格取样，命中一个就算命中（准星是细线，取到即可）。
// 网格固定 9×9 = 81 点（步长 range/4），与半径无关 ⇒ 开销恒定，不会因 range 调大而
// 拖慢主循环。取 5×5 时步长 = range/2，实测会整格跳过 80px 半径里那条 9px 宽的准星
// （取样点落在 ±40/±80，色块在 +56..+64 ⇒ 漏检，2026-09-26 用例钉住）。
inline bool crosshair_probe_hit(const uint8_t* data, const FrameInfo& info,
                               float center_x, float center_y,
                               const CrosshairProbeParams& p) {
    if (!p.enabled || !data) return false;
    if (info.width == 0 || info.height == 0) return false;
    const CrosshairColorKey key = crosshair_color_key(p.color_id);
    const float range = p.range_px > 1.0f ? p.range_px : 1.0f;
    for (int iy = -4; iy <= 4; ++iy) {
        for (int ix = -4; ix <= 4; ++ix) {
            const float fx = center_x + range * (static_cast<float>(ix) / 4.0f);
            const float fy = center_y + range * (static_cast<float>(iy) / 4.0f);
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (!sample_rgb(data, info, static_cast<int>(std::lround(fx)),
                            static_cast<int>(std::lround(fy)), &r, &g, &b)) {
                continue;
            }
            float h = 0.0f, s = 0.0f, v = 0.0f;
            rgb_to_hsv(std::clamp(r, 0.0f, 255.0f), std::clamp(g, 0.0f, 255.0f),
                       std::clamp(b, 0.0f, 255.0f), &h, &s, &v);
            if (hsv_matches(h, s, v, key, p.tolerance)) return true;
        }
    }
    return false;
}

}  // namespace ttbox::core::aim
