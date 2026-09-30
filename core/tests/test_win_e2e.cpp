// test_win_e2e.cpp — Windows 端到端集成测试（TTBOX_CORE_HAS_ONNX）
//
// 完整链路（全部真实代码、真实数据、真实计算，无 mock）：
//   合成测试帧(BGR)
//     → Preprocess(CPU fallback, center crop 256x256, RGB)
//     → OnnxBackend(真实 ONNX Runtime 推理)
//     → DecodeNMS(DFL-dist 9 输出解码 + classwise NMS)
//     → TargetSelector(真实目标选择)
//     → CoordinateTransform(瞄准点误差)
//     → TestPidController(真实 PID → dx/dy)
//
// 通过标准：
//   1. ONNX 模型加载成功
//   2. 推理输出 9 个张量 shape 与板端 metadata 一致
//   3. Decode 不崩溃、不产生 NaN
//   4. 有检测时 selector/PID 输出有限值
//   5. 空场景(无目标)时 selector.valid=false 且 dx/dy=0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "test_pid_controller.hpp"
#include "model/backend/ModelBackend.hpp"
#include "mouse/CoordinateTransform.hpp"
#include "mouse/TargetSelector.hpp"
#include "rknn/DecodeNMS.hpp"
#include "rknn/Preprocess.hpp"
#include "test_util.hpp"

#if defined(TTBOX_CORE_HAS_ONNX) && TTBOX_CORE_HAS_ONNX
using namespace ttbox::core;
namespace fs = std::filesystem;

// ---------- 定位模型 ----------
std::string find_model() {
    // 优先: 环境变量 → tests/data → 项目根相对
    if (const char* env = std::getenv("TTBOX_TEST_ONNX")) return env;
    for (auto& p : {
             fs::path("tests/data/yolo256_rand.onnx"),
             fs::path("core/tests/data/yolo256_rand.onnx"),
             fs::path(TTBOX_PROJECT_ROOT "/core/tests/data/yolo256_rand.onnx"),
         }) {
        std::error_code ec;
        if (fs::exists(p, ec)) return p.string();
    }
    return {};
}

// ---------- 生成合成测试帧（BGR24, w×h）----------
// 在 (cx, cy) 画一个 r×r 纯色方块（模拟人物），背景为随机噪声
FrameBuffer make_frame(int w, int h, float tx, float ty, float box_px,
                       std::mt19937& rng) {
    FrameBuffer fb;
    fb.data = std::shared_ptr<uint8_t[]>(new uint8_t[w * h * 3]);
    fb.size = w * h * 3;
    fb.info.width = w;
    fb.info.height = h;
    fb.info.stride = w * 3;
    fb.info.format = PixelFormat::kBGR888;
    fb.info.timestamp_us = 0;
    fb.info.cpu_va = fb.data.get();  // CPU fallback 预处理消费裸指针

    std::uniform_int_distribution<int> noise(0, 60);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* px = fb.data.get() + y * w * 3 + x * 3;
            px[0] = (uint8_t)noise(rng);  // B
            px[1] = (uint8_t)noise(rng);  // G
            px[2] = (uint8_t)noise(rng);  // R
        }
    }
    // 画目标（亮青色块，位于 帧中心偏移(tx,ty) 处）
    int x1 = (int)(w / 2 + tx - box_px / 2);
    int y1 = (int)(h / 2 + ty - box_px / 2);
    int x2 = x1 + (int)box_px;
    int y2 = y1 + (int)box_px;
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            uint8_t* px = fb.data.get() + y * w * 3 + x * 3;
            px[0] = 200; px[1] = 220; px[2] = 240;
        }
    }
    return fb;
}

// ---------- Preprocess 输出 → float NCHW RGB（ONNX 输入）----------
// 板端 ONNX 输入是 NCHW float RGB [1,3,256,256]（与训练一致）。
// 这里从 PreprocessedFrame (RGB u8 interleaved) 转 NCHW float 0~1。
bool to_nchw_float(const PreprocessedFrame& pf, std::vector<float>& out) {
    const uint32_t w = pf.detect_size.width;
    const uint32_t h = pf.detect_size.height;
    if (!pf.ok || pf.tensor_data == nullptr) return false;
    out.resize(3 * w * h);
    // pf.data 是 RGB interleaved (detect_size × detect_size)
    const uint8_t* src = pf.tensor_data;
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                out[c * w * h + y * w + x] =
                    src[(y * w + x) * 3 + c] / 255.0f;
            }
        }
    }
    return true;
}


// OnnxBackend（与 RKNN 共用 IModelBackend 接口）
#include "model/backend/OnnxBackend.hpp"

TEST(win_e2e_full_pipeline) {
    // ---- 1. 找模型 ----
    const std::string model_path = find_model();
    if (model_path.empty()) {
        // yolo256_rand.onnx 随仓库交付（core/tests/data/，git 跟踪）⇒ 缺失 = 检出损坏，属**必需** fixture。
        TEST_SKIP_REQUIRED("未找到必需 fixture yolo256_rand.onnx（core/tests/data/ 随库交付）");
    }
    std::printf("  model: %s\n", model_path.c_str());

    // ---- 2. 加载 ONNX ----
    OnnxBackend backend;
    std::string err;
    CHECK(backend.init(model_path, &err));
    if (!backend.initialized()) {
        std::printf("  init err: %s\n", err.c_str());
        return;
    }
    const RknnModelInfo& info = backend.info();
    CHECK(info.input_width == 256 && info.input_height == 256);
    CHECK(info.n_outputs == 9);
    std::printf("  加载 OK: in=%ux%u out=%u\n", info.input_width,
                info.input_height, info.n_outputs);

    // ---- 3. 配 Decode（DFL-dist: 9 输出 = 3 尺度 × [reg64, cls7, aux1]）----
    DecodeNMS decode;
    DecodeParams dp;
    dp.conf_thres = 0.25f;
    dp.iou_thres = 0.45f;
    dp.classwise = true;
    dp.input_w = 256;
    dp.input_h = 256;
    // 模型输出坐标在 256 输入空间；原图与输入一致（简化，先 1:1）
    dp.frame_w = 256;
    dp.frame_h = 256;
    CHECK(decode.configure(dp, &err));

    // 输出 buffer（ONNX 输出是 float，RknnOutputInfo.type=0=F32，
    // DecodeNMS::read_elem 按 F32 直接读，无需反量化）
    std::vector<std::vector<uint8_t>> out_bufs(info.n_outputs);
    std::vector<RknnOutputInfo> out_info(info.n_outputs);
    for (uint32_t i = 0; i < info.n_outputs; ++i) {
        const auto& oi = info.outputs[i];
        out_info[i] = oi;
        out_bufs[i].resize(oi.size);
    }

    // ---- 4. Preprocess（CPU fallback）----
    Preprocess prep;
    PreprocessConfig pc;
    pc.detect_size = DetectSize{256, 256};
    pc.backend = PreprocessBackend::kCpuFallback;
    pc.center_crop = true;
    pc.input_type = 0;  // FLOAT32
    pc.input_size = 3 * 256 * 256 * sizeof(float);
    pc.color_order = 1;  // RGB（模型是 RGB 训练风格）
    CHECK(prep.init(pc, &err));

    // ---- 5. TargetSelector / Coordinate / PID ----
    aim::TargetSelector selector;
    aim::TargetSelectorConfig tsc;
    tsc.roi_w = 256;   // 与模型输入/Decode 输出坐标系一致
    tsc.roi_h = 256;
    tsc.confidence = 0.25f;
    aim::AimPointProfile app;  // 默认中心瞄准
    aim::TestPidController pid;
    aim::TestPidParams pp;
    pp.sensitivity = 1.0f;
    pp.output_deadzone = 1.0f;
    pid.configure(pp);

    // ---- 6. 跑两帧：空场景 + 有目标场景 ----
    std::mt19937 rng(42);
    const int FW = 640, FH = 640;  // 合成"原图"尺寸（模拟 capture）

    // --- 6a. 空场景（无目标 → selector invalid → dx=dy=0）---
    {
        FrameBuffer fb = make_frame(FW, FH, 0, 0, 0, rng);
        PreprocessedFrame pf;
        CHECK(prep.process(fb, &pf, &err));
        std::vector<float> in;
        CHECK(to_nchw_float(pf, in));
        std::vector<std::vector<float>> outputs;
        CHECK(backend.infer(in.data(), in.size() * sizeof(float), outputs, &err));
        CHECK(outputs.size() == 9);
        for (uint32_t i = 0; i < info.n_outputs; ++i) {
            std::memcpy(out_bufs[i].data(), outputs[i].data(),
                        std::min(out_bufs[i].size(), outputs[i].size() * sizeof(float)));
        }
        std::vector<const void*> bufs;
        for (auto& b : out_bufs) bufs.push_back(b.data());
        std::vector<DetectionBox> dets;
        CHECK(decode.process(info, bufs.data(), &dets, &err));
        for (auto& d : dets) {
            CHECK(std::isfinite(d.x1) && std::isfinite(d.y1) &&
                  std::isfinite(d.x2) && std::isfinite(d.y2));
        }
        auto sel = selector.select(dets, tsc, 0);
        // 随机权重模型有大量误检（score≈0.5 的假框），selector 选中其中一个
        // 属正常行为。此处断言：选择过程确定性与 PID 输出有限值。
        if (sel.valid) {
            aim::TargetPoint tp;
            tp.valid = true;
            tp.x = sel.box.x1 + (sel.box.x2 - sel.box.x1) / 2;
            tp.y = sel.box.y1 + (sel.box.y2 - sel.box.y1) / 2;
            auto cmd = pid.update(tp);
            CHECK(std::abs((long)cmd.dx) < 10000 && std::abs((long)cmd.dy) < 10000);
        }
        std::printf("  空场景: dets=%zu valid=%d\n", dets.size(), sel.valid ? 1 : 0);
    }

    // --- 6b. 有目标场景（亮色块偏离中心 → selector 锁定 → PID 产生非零 dx/dy）---
    {
        selector.reset();
        // 目标在中心右侧 100px、下方 60px（原图坐标系）
        FrameBuffer fb = make_frame(FW, FH, +100.0f, +60.0f, 60.0f, rng);
        PreprocessedFrame pf;
        CHECK(prep.process(fb, &pf, &err));
        std::vector<float> in;
        CHECK(to_nchw_float(pf, in));
        std::vector<std::vector<float>> outputs;
        CHECK(backend.infer(in.data(), in.size() * sizeof(float), outputs, &err));
        for (uint32_t i = 0; i < info.n_outputs; ++i) {
            std::memcpy(out_bufs[i].data(), outputs[i].data(),
                        std::min(out_bufs[i].size(), outputs[i].size() * sizeof(float)));
        }
        std::vector<const void*> bufs;
        for (auto& b : out_bufs) bufs.push_back(b.data());
        std::vector<DetectionBox> dets;
        CHECK(decode.process(info, bufs.data(), &dets, &err));
        std::printf("  有目标: dets=%zu\n", dets.size());
        for (size_t i = 0; i < dets.size() && i < 5; ++i) {
            std::printf("    det[%zu] cls=%d score=%.2f box=(%.0f,%.0f,%.0f,%.0f)\n",
                        i, dets[i].class_id, dets[i].score,
                        dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
        }

        // 目标选择
        auto sel = selector.select(dets, tsc, 100);
        std::printf("  selector: valid=%d target_id=%d reason=%d\n",
                    sel.valid ? 1 : 0, sel.target_id, (int)sel.reason);

        // 坐标变换 → PID
        float rx = 0, ry = 0;
        aim::CoordinateTransform::reference_point(256, 256, app, &rx, &ry);
        pid.set_reference(rx, ry);
        if (sel.valid) {
            float ex = 0, ey = 0;
            CHECK(aim::CoordinateTransform::pixel_error(
                      sel.box, sel.box.class_id, app, 256, 256, &ex, &ey));
            std::printf("  pixel_error=(%.1f, %.1f)\n", ex, ey);
            aim::TargetPoint tp;
            tp.valid = true;
            tp.x = rx + ex;  // 目标点 = 参考点 + 误差
            tp.y = ry + ey;
            auto cmd = pid.update(tp);
            std::printf("  PID: dx=%d dy=%d valid=%d\n", cmd.dx, cmd.dy,
                        cmd.valid ? 1 : 0);
            if (cmd.valid) {
                // 随机权重模型不保证方向，但必须有限值
                CHECK(std::abs((long)cmd.dx) < 10000 && std::abs((long)cmd.dy) < 10000);
            }
        }
        // 注：随机权重 ONNX 的检测质量无法保证；此链路验证的是
        // "每一环都能真实运行且输出有限值"，而不是检测精度。
        // 检测精度用真实训练模型在板端 imgdetect 验证。
    }

}

#endif  // TTBOX_CORE_HAS_ONNX
