// test_crosshair_probe.cpp — 准星找色（CrosshairProbe）单元测试
//
// 锁死的性质：
//   Case1 关着（enabled=false）⇒ 恒不命中（不许沿用旧结果）
//   Case2 RGB888 中心纯红 ⇒ color_id=0（红）命中；换成绿色目标色不命中
//   Case3 BGR888 / RGBA8888 同样能取到（通道顺序不能错）
//   Case4 NV12 取色：把 RGB 转 NV12 再取，色相判对（Y/UV 换算不能反）
//   Case5 容差收紧 ⇒ 同一块颜色从命中变不命中（tolerance 真的在起作用）
//   Case6 检测半径：中心很近的色块命中，超出半径的不命中
//   Case7 白色目标（低饱和）用「不超过上限」判据，纯白命中、彩色不命中
//   Case8 越界/未知格式 ⇒ 返回 false（fail-closed，不是"当作命中"）
//   Case9 红色跨 0 环绕：H=175 与 H=5 都算红
#include <cmath>
#include <cstdio>
#include <vector>

#include "mouse/CrosshairProbe.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

namespace {

int failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); failures++; }
    else { std::printf("  PASS: %s\n", msg); }
}

// 造一张纯色 RGB888 图（stride = width*3）
std::vector<uint8_t> make_rgb(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < out.size(); i += 3) {
        out[i] = r; out[i + 1] = g; out[i + 2] = b;
    }
    return out;
}

// RGB888 → NV12（单 buffer：Y plane + 交错 UV plane）
std::vector<uint8_t> to_nv12(const std::vector<uint8_t>& rgb, int w, int h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3 / 2, 0);
    std::vector<float> ys(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            const float r = rgb[i], g = rgb[i + 1], b = rgb[i + 2];
            const float yy = 0.299f * r + 0.587f * g + 0.114f * b;
            ys[static_cast<size_t>(y) * w + x] = yy;
            out[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(yy < 0 ? 0 : (yy > 255 ? 255 : yy));
        }
    }
    const size_t uv_base = static_cast<size_t>(w) * h;
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            float sr = 0, sg = 0, sb = 0, sy = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int xx = x + dx, yy2 = y + dy;
                    if (xx >= w || yy2 >= h) continue;
                    const size_t i = (static_cast<size_t>(yy2) * w + xx) * 3;
                    sr += rgb[i]; sg += rgb[i + 1]; sb += rgb[i + 2];
                    sy += ys[static_cast<size_t>(yy2) * w + xx];
                }
            }
            sr *= 0.25f; sg *= 0.25f; sb *= 0.25f; sy *= 0.25f;
            const float u = (sb - sy) * 0.565f + 128.0f;
            const float v = (sr - sy) * 0.713f + 128.0f;
            const size_t o = uv_base + static_cast<size_t>(y / 2) * w + static_cast<size_t>(x / 2) * 2;
            out[o] = static_cast<uint8_t>(u < 0 ? 0 : (u > 255 ? 255 : u));
            out[o + 1] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    return out;
}

FrameInfo info_rgb(int w, int h, PixelFormat fmt) {
    FrameInfo fi;
    fi.width = static_cast<uint32_t>(w);
    fi.height = static_cast<uint32_t>(h);
    fi.stride = static_cast<uint32_t>(w) * (fmt == PixelFormat::kRGBA8888 ? 4u : 3u);
    fi.format = fmt;
    return fi;
}

}  // namespace

int main() {
    std::printf("[crosshair_probe]\n");
    const int W = 320, H = 320;
    const float cx = W / 2.0f, cy = H / 2.0f;

    // Case1：关着恒不命中
    {
        const auto buf = make_rgb(W, H, 255, 0, 0);
        CrosshairProbeParams p;
        p.enabled = false;
        p.color_id = 0;
        check(!crosshair_probe_hit(buf.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case1 未启用 ⇒ 不命中");
    }

    // Case2：红色图 + 红色目标 ⇒ 命中；红色图 + 绿色目标 ⇒ 不命中
    {
        const auto buf = make_rgb(W, H, 255, 0, 0);
        CrosshairProbeParams p;
        p.enabled = true;
        p.tolerance = 60.0f;
        p.range_px = 80.0f;
        p.color_id = 0;
        check(crosshair_probe_hit(buf.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case2 红图 + 红目标 ⇒ 命中");
        p.color_id = 1;  // 绿
        check(!crosshair_probe_hit(buf.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case2 红图 + 绿目标 ⇒ 不命中");
    }

    // Case3：BGR / RGBA 通道顺序
    {
        const int w = 160, h = 160;
        std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 3);
        for (size_t i = 0; i < bgr.size(); i += 3) {
            bgr[i] = 0; bgr[i + 1] = 255; bgr[i + 2] = 0;  // B=0 G=255 R=0 ⇒ 纯绿
        }
        CrosshairProbeParams p;
        p.enabled = true;
        p.tolerance = 60.0f;
        p.range_px = 40.0f;
        p.color_id = 1;
        FrameInfo fi = info_rgb(w, h, PixelFormat::kBGR888);
        check(crosshair_probe_hit(bgr.data(), fi, w / 2.0f, h / 2.0f, p),
              "Case3 BGR 绿图 + 绿目标 ⇒ 命中");

        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i] = 0; rgba[i + 1] = 255; rgba[i + 2] = 0; rgba[i + 3] = 255;
        }
        fi = info_rgb(w, h, PixelFormat::kRGBA8888);
        check(crosshair_probe_hit(rgba.data(), fi, w / 2.0f, h / 2.0f, p),
              "Case3 RGBA 绿图 + 绿目标 ⇒ 命中");
    }

    // Case4：NV12 取色（纯红转 NV12 后仍判成红）
    {
        const auto rgb = make_rgb(W, H, 230, 20, 20);
        const auto nv = to_nv12(rgb, W, H);
        FrameInfo fi;
        fi.width = W; fi.height = H; fi.stride = W; fi.format = PixelFormat::kNV12;
        CrosshairProbeParams p;
        p.enabled = true;
        p.tolerance = 60.0f;
        p.range_px = 80.0f;
        p.color_id = 0;
        check(crosshair_probe_hit(nv.data(), fi, cx, cy, p), "Case4 NV12 红图 ⇒ 判成红");
        p.color_id = 1;
        check(!crosshair_probe_hit(nv.data(), fi, cx, cy, p), "Case4 NV12 红图 ⇒ 不判成绿");
    }

    // Case5：容差真的在起作用
    {
        // 暗红（V 明显低于 255）：大容差命中，小容差不命中
        const auto buf = make_rgb(W, H, 150, 10, 10);
        CrosshairProbeParams p;
        p.enabled = true;
        p.color_id = 0;
        p.range_px = 80.0f;
        p.tolerance = 200.0f;
        const bool loose = crosshair_probe_hit(buf.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p);
        p.tolerance = 10.0f;
        const bool tight = crosshair_probe_hit(buf.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p);
        check(loose && !tight, "Case5 容差收紧 ⇒ 命中变不命中");
    }

    // Case6：检测半径（只在一小块区域画色，中心与它错开）
    {
        std::vector<uint8_t> buf(static_cast<size_t>(W) * H * 3, 0);
        // 只在 (cx+60, cy) 附近画一小块青色
        for (int y = static_cast<int>(cy) - 4; y <= static_cast<int>(cy) + 4; ++y) {
            for (int x = static_cast<int>(cx) + 56; x <= static_cast<int>(cx) + 64; ++x) {
                const size_t i = (static_cast<size_t>(y) * W + x) * 3;
                buf[i] = 0; buf[i + 1] = 255; buf[i + 2] = 255;
            }
        }
        CrosshairProbeParams p;
        p.enabled = true;
        p.color_id = 2;  // 青
        p.tolerance = 60.0f;
        p.range_px = 80.0f;
        check(crosshair_probe_hit(buf.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case6 半径 80 覆盖到 60px 外的色块 ⇒ 命中");
        p.range_px = 20.0f;
        check(!crosshair_probe_hit(buf.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case6 半径 20 覆盖不到 ⇒ 不命中");
    }

    // Case7：白色（低饱和）
    {
        const auto white = make_rgb(W, H, 255, 255, 255);
        CrosshairProbeParams p;
        p.enabled = true;
        p.color_id = 4;  // 白
        p.tolerance = 60.0f;
        p.range_px = 80.0f;
        check(crosshair_probe_hit(white.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case7 纯白 + 白目标 ⇒ 命中");
        const auto green = make_rgb(W, H, 0, 255, 0);
        check(!crosshair_probe_hit(green.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case7 纯绿 + 白目标 ⇒ 不命中（饱和度超上限）");
    }

    // Case8：越界 / 未知格式 ⇒ fail-closed
    {
        const auto buf = make_rgb(W, H, 255, 0, 0);
        CrosshairProbeParams p;
        p.enabled = true;
        p.color_id = 0;
        p.range_px = 80.0f;
        FrameInfo fi = info_rgb(W, H, PixelFormat::kUnknown);
        check(!crosshair_probe_hit(buf.data(), fi, cx, cy, p), "Case8 未知格式 ⇒ 不命中");
        check(!crosshair_probe_hit(nullptr, info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case8 空缓冲 ⇒ 不命中");
        // 中心远在图外（采样点全越界）
        check(!crosshair_probe_hit(buf.data(), info_rgb(W, H, PixelFormat::kRGB888), 100000.0f,
                                   100000.0f, p),
              "Case8 采样点全越界 ⇒ 不命中");
    }

    // Case9：红色跨 0 环绕（H≈175 与 H≈5 都算红）
    {
        // H=5 ⇒ RGB 约为 (255, 21, 0)
        const auto near_wrap = make_rgb(W, H, 255, 21, 0);
        // H=175 ⇒ RGB 约为 (255, 0, 21)
        const auto far_wrap = make_rgb(W, H, 255, 0, 21);
        CrosshairProbeParams p;
        p.enabled = true;
        p.color_id = 0;
        p.tolerance = 60.0f;
        p.range_px = 80.0f;
        check(crosshair_probe_hit(near_wrap.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case9 H≈5 ⇒ 判成红（环绕下界）");
        check(crosshair_probe_hit(far_wrap.data(), info_rgb(W, H, PixelFormat::kRGB888), cx, cy, p),
              "Case9 H≈175 ⇒ 判成红（环绕上界）");
    }

    std::printf("[crosshair_probe] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
