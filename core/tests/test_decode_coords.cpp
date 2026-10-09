// test_decode_coords.cpp — 用真实 DecodeNMS 做合成坐标闭环验证。
// 构造与板上 jwdl_sjzv11 完全一致的 6 输出 INT8 DFL-dist 布局，
// 在 5 个已知位置注入目标，验证：模型坐标 → DFL 解码 → map_coords → 原图坐标。
#include "rknn/DecodeNMS.hpp"
#include "common/Types.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

using namespace ttbox::core;

// ---- 与板上 jwdl_sjzv11 一致的输出结构 ----
static RknnModelInfo make_info() {
    RknnModelInfo info;
    info.n_inputs = 1;
    info.n_outputs = 6;
    info.input_width = 256; info.input_height = 256;
    info.input_type = 2; info.input_fmt = 1; // INT8 NHWC
    info.input_size = 196608;
    // scale 32 / 16 / 8: [reg(64,H,W), cls(7,H,W)]
    const uint32_t grids[3] = {32, 16, 8};
    const float reg_scale[3] = {0.0854986f, 0.0641861f, 0.0437353f};
    const int reg_zp[3] = {-48, -61, -85};
    const float cls_scale[3] = {0.166704f, 0.301925f, 0.102245f};
    const int cls_zp[3] = {116, 120, 127};
    for (uint32_t s = 0; s < 3; ++s) {
        RknnOutputInfo reg, cls;
        reg.dims = {1, 64, grids[s], grids[s]}; reg.type = 2; reg.fmt = 0;
        reg.scale = reg_scale[s]; reg.zp = reg_zp[s]; reg.n_elems = 64 * grids[s] * grids[s];
        reg.size = reg.n_elems;
        cls.dims = {1, 7, grids[s], grids[s]}; cls.type = 2; cls.fmt = 0;
        cls.scale = cls_scale[s]; cls.zp = cls_zp[s]; cls.n_elems = 7 * grids[s] * grids[s];
        cls.size = cls.n_elems;
        info.outputs.push_back(reg); info.outputs.push_back(cls);
    }
    return info;
}

// 目标位置（模型坐标 0~256）
struct Target { float cx, cy, w, h; int cls; };

// 注入一个目标到 scale s（32→0）：在 (col,row) 网格，DLF 距离 dist(单位网格)
static int fill_target(std::vector<uint8_t>& reg, std::vector<uint8_t>& cls,
                       const RknnModelInfo& info, int s, uint32_t grid,
                       int col, int row, int cls_id,
                       int dl, int dt, int dr, int db) {
    const auto& ro = info.outputs[s * 2];
    const auto& co = info.outputs[s * 2 + 1];
    const uint32_t n = grid * grid;
    // 所有 reg 初始为 zp（浮点0）；所有 cls 初始为很低（浮点大负）
    const int8_t reg_zp = (int8_t)ro.zp, cls_zp = (int8_t)co.zp;
    for (uint32_t a = 0; a < n; ++a) {
        for (uint32_t e = 0; e < 4; ++e) {
            for (uint32_t b = 0; b < 16; ++b) {
                reg[(e * 16 + b) * n + a] = (uint8_t)reg_zp;
            }
        }
        for (uint32_t c = 0; c < 7; ++c) {
            cls[c * n + a] = (uint8_t)(-120); // 浮点大负 → sigmoid≈0
        }
    }
    // 目标锚点处：4 边在 bin 索引 dl/dt/dr/db 置高（软max 占优）
    const uint32_t a = row * grid + col;
    const int8_t peak = 120; // 浮点 ~ (120-zp)*scale > 10
    reg[(0 * 16 + dl) * n + a] = (uint8_t)peak;
    reg[(1 * 16 + dt) * n + a] = (uint8_t)peak;
    reg[(2 * 16 + dr) * n + a] = (uint8_t)peak;
    reg[(3 * 16 + db) * n + a] = (uint8_t)peak;
    cls[cls_id * n + a] = (uint8_t)127; // 目标类高 → sigmoid 高
    return 0;
}

static float decode_dist(const std::vector<uint8_t>& reg, const RknnModelInfo& info,
                         int s, uint32_t grid, uint32_t a, uint32_t edge) {
    const auto& ro = info.outputs[s * 2];
    const uint32_t n = grid * grid;
    // 反量化
    auto val = [&](uint32_t b)->float {
        int8_t v = (int8_t)reg[(edge * 16 + b) * n + a];
        return ((float)v - (float)ro.zp) * ro.scale;
    };
    float mx = -1e30f; for (uint32_t b = 0; b < 16; ++b) mx = std::max(mx, val(b));
    float sum = 0; for (uint32_t b = 0; b < 16; ++b) sum += std::exp(val(b) - mx);
    float d = 0; for (uint32_t b = 0; b < 16; ++b) d += b * std::exp(val(b) - mx) / sum;
    return d;
}

int main() {
    const auto info = make_info();
    DecodeParams dp;
    dp.conf_thres = 0.55f;
    dp.iou_thres = 0.45f;
    dp.classwise = true;
    dp.input_w = 256; dp.input_h = 256;
    dp.frame_w = 2560; dp.frame_h = 1440;
    // ROI = 屏幕中心 500×500（与运行时 profile 一致）
    const uint32_t roi_x = 1030, roi_y = 470, roi_w = 500, roi_h = 500;
    dp.roi_x = roi_x; dp.roi_y = roi_y; dp.roi_w = roi_w; dp.roi_h = roi_h;

    DecodeNMS dec;
    std::string err;
    if (!dec.configure(dp, &err)) { printf("configure fail: %s\n", err.c_str()); return 1; }

    // 5 组目标：模型空间位置
    Target targets[5] = {
        {128.0f, 128.0f, 64.0f, 64.0f, 0},   // 中心
        { 32.0f,  32.0f, 48.0f, 48.0f, 1},   // 左上
        {224.0f,  32.0f, 48.0f, 48.0f, 2},   // 右上
        { 32.0f, 224.0f, 48.0f, 48.0f, 3},   // 左下
        {224.0f, 224.0f, 48.0f, 48.0f, 4},   // 右下
    };

    // 用 scale 32（grid 32，stride 8）注入目标。
    const uint32_t grid = 32; const float stride = 8.0f; const int scale = 0;
    // 6 输出：3 组 [reg,cls]。只注入 scale32，其余尺度填 0（无目标）。
    std::vector<std::vector<uint8_t>> outputs;
    std::vector<const void*> bufs;
    for (uint32_t o = 0; o < info.n_outputs; ++o) {
        outputs.emplace_back(info.outputs[o].size, 0);
        bufs.push_back(outputs.back().data());
    }
    // scale32 = output 0/1
    std::vector<uint8_t>& reg = outputs[0];
    std::vector<uint8_t>& cls = outputs[1];

    int pass = 0;
    for (int t = 0; t < 5; ++t) {
        const Target& T = targets[t];
        // 目标锚点：最近网格中心
        int col = (int)std::lround(T.cx / stride - 0.5f);
        int row = (int)std::lround(T.cy / stride - 0.5f);
        col = std::min(std::max(col, 0), (int)grid - 1);
        row = std::min(std::max(row, 0), (int)grid - 1);
        // dl/dt/dr/db（单位网格）
        int dl = (int)std::lround(T.w / 2 / stride);
        int dt = (int)std::lround(T.h / 2 / stride);
        int dr = dl, db = dt;
        fill_target(reg, cls, info, scale, grid, col, row, T.cls, dl, dt, dr, db);

        std::vector<DetectionBox> dets;
        if (!dec.process(info, bufs.data(), &dets, &err)) { printf("T%d process fail: %s\n", t, err.c_str()); return 1; }
        if (dets.empty()) { printf("T%d: NO detection!\n", t); continue; }

        // 期望模型空间框（锚点中心 + DFL 距离*stride）
        const float acx = ((float)col + 0.5f) * stride;
        const float acy = ((float)row + 0.5f) * stride;
        const float m_x1 = acx - dl * stride, m_y1 = acy - dt * stride;
        const float m_x2 = acx + dr * stride, m_y2 = acy + db * stride;
        // 期望原图框：ROI 空间 + 偏移
        const float sx = (float)roi_w / dp.input_w, sy = (float)roi_h / dp.input_h;
        const float o_x1 = m_x1 * sx + roi_x, o_y1 = m_y1 * sy + roi_y;
        const float o_x2 = m_x2 * sx + roi_x, o_y2 = m_y2 * sy + roi_y;

        const DetectionBox& d = dets[0];
        const float cx_err = std::fabs((d.x1 + d.x2) / 2 - (o_x1 + o_x2) / 2);
        const float cy_err = std::fabs((d.y1 + d.y2) / 2 - (o_y1 + o_y2) / 2);
        const bool ok = (d.class_id == T.cls) && cx_err < 2.0f && cy_err < 2.0f;
        printf("T%d model=(%.0f,%.0f) class=%d conf=%.3f box=[%.1f,%.1f,%.1f,%.1f] center=(%.1f,%.1f) err=(%.2f,%.2f) %s\n",
               t, T.cx, T.cy, d.class_id, d.score, d.x1, d.y1, d.x2, d.y2,
               (d.x1 + d.x2) / 2, (d.y1 + d.y2) / 2, cx_err, cy_err, ok ? "PASS" : "FAIL");
        if (ok) pass++;
    }
    printf("\n%s (%d/5)\n", pass == 5 ? "ALL PASS" : "FAIL", pass);
    return pass == 5 ? 0 : 1;
}
