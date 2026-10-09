// test_decode_align.cpp — 板端 Decode/NMS 与 Python Demo 对齐验证（RK3588）
//
// 用法：test_decode_align --input <640x640x3 u8 raw> --model <path>
//                         [--conf 0.25] [--iou 0.45]
//
// 流程：加载固定输入 → u8->FP16（精确 LUT）→ RKNN raw 输出 → C++ Decode/NMS
// 输出：检测列表（class score x1 y1 x2 y2，score 降序），供与 Python 对照。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "capture/V4L2Capture.hpp"
#include "rga/RgaProcessor.hpp"
#include "rknn/DecodeNMS.hpp"
#include "rknn/RKNNEngine.hpp"

using namespace ttbox::core;

namespace {

uint16_t float_to_half(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    const uint32_t sign = (b >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((b >> 23) & 0xFF) - 127 + 15;
    uint32_t man = b & 0x7FFFFFu;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        man = (man | 0x800000u) >> (1 - exp);
        return static_cast<uint16_t>(sign | (man >> 13));
    }
    const uint32_t rem = man & 0x1FFFu;
    man >>= 13;
    if (rem > 0x1000u || (rem == 0x1000u && (man & 1u))) ++man;
    if (man > 0x3FFu) { man = 0; ++exp; }
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | man);
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path, model_path, capture_path, dump_raw_path;
    float conf = 0.25f, iou = 0.45f;
    bool pass_through = false;  // A-6：false=由 runtime 转换（正确）；true=零拷贝（需内部布局）
    bool layout_nchw = false;
    bool info_only = false;     // 仅打印模型输入/输出信息后退出
    uint32_t in_w = 640, in_h = 640;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺少参数: %s\n", n); std::exit(1); }
            return argv[++i];
        };
        if (a == "--input") input_path = next("--input");
        else if (a == "--capture") capture_path = next("--capture");
        else if (a == "--dump-raw") dump_raw_path = next("--dump-raw");
        else if (a == "--model") model_path = next("--model");
        else if (a == "--conf") conf = static_cast<float>(std::atof(next("--conf").c_str()));
        else if (a == "--iou") iou = static_cast<float>(std::atof(next("--iou").c_str()));
        else if (a == "--passthrough") pass_through = (std::atoi(next("--passthrough").c_str()) != 0);
        else if (a == "--layout") layout_nchw = (next("--layout") == "nchw");
        else if (a == "--info-only") info_only = true;
        else if (a == "--in-w") in_w = static_cast<uint32_t>(std::atoi(next("--in-w").c_str()));
        else if (a == "--in-h") in_h = static_cast<uint32_t>(std::atoi(next("--in-h").c_str()));
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); return 1; }
    }
    if (model_path.empty() || (!info_only && input_path.empty() && capture_path.empty())) {
        std::fprintf(stderr, "用法: test_decode_align (--input <raw.bin> | --capture <out.bin>) --model <model> [--conf] [--iou] [--passthrough 0|1] [--layout nhwc|nchw] [--dump-raw <out>] [--info-only] [--in-w N] [--in-h N]\n");
        return 1;
    }

    std::vector<uint8_t> img;
    if (!info_only && !capture_path.empty()) {
        // 实时采集一帧：V4L2 (1920x1080) → RGA center-crop+resize → 640x640 BGR
        V4L2Capture cap;
        std::string cerr;
        V4L2Capture::Params cp;
        cp.device = "/dev/video0";
        cp.num_buffers = 4;
        if (!cap.configure(cp, &cerr) || !cap.open(&cerr) || !cap.start(&cerr)) {
            std::fprintf(stderr, "[FAIL] capture: %s\n", cerr.c_str());
            return 1;
        }
        RgaProcessor rga;
        if (!rga.init({in_w, in_h, true}, &cerr)) {
            std::fprintf(stderr, "[FAIL] rga: %s\n", cerr.c_str());
            cap.stop(); cap.close();
            return 1;
        }
        std::shared_ptr<FrameBuffer> frame;
        for (int k = 0; k < 200 && !frame; ++k) {
            frame = cap.latest_frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!frame) {
            std::fprintf(stderr, "[FAIL] 无帧\n");
            rga.destroy(); cap.stop(); cap.close();
            return 1;
        }
        RgaOutput rga_out;
        std::string perr;
        if (!rga.process(*frame, &rga_out, &perr)) {
            std::fprintf(stderr, "[FAIL] rga.process: %s\n", perr.c_str());
            rga.destroy(); cap.stop(); cap.close();
            return 1;
        }
        std::printf("捕获帧 seq=%u -> rga %ux%u stride=%u\n",
                    frame->info.sequence, rga_out.width, rga_out.height, rga_out.stride);
        img.resize(in_w * in_h * 3);
        const uint8_t* src = static_cast<const uint8_t*>(rga_out.vir_addr);
        for (uint32_t y = 0; y < in_h; ++y) {
            std::memcpy(img.data() + static_cast<size_t>(y) * in_w * 3,
                        src + static_cast<size_t>(y) * rga_out.stride, in_w * 3);
        }
        std::ofstream of(capture_path, std::ios::binary);
        of.write(reinterpret_cast<const char*>(img.data()), img.size());
        std::printf("已保存 %s (%zu B)\n", capture_path.c_str(), img.size());
        rga.destroy();
        cap.stop();
        cap.close();
    } else if (!info_only) {
        std::ifstream f(input_path, std::ios::binary);
        if (!f) { std::fprintf(stderr, "无法打开输入: %s\n", input_path.c_str()); return 1; }
        img.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (!info_only && img.size() != static_cast<size_t>(in_w) * in_h * 3) {
        std::fprintf(stderr, "输入大小错误: %zu（期望 %ux%ux3）\n", img.size(), in_w, in_h);
        return 1;
    }

    // RKNN 推理（与 worker 同路径：u8->FP16 精确 LUT + pass_through 零拷贝 + raw 输出）
    RKNNEngine engine;
    std::string err;
    RKNNEngine::Params ep;
    ep.model_path = model_path;
    ep.core_mask = 0;
    ep.pass_through = pass_through;
    if (!engine.init(ep, &err)) { std::fprintf(stderr, "[FAIL] engine: %s\n", err.c_str()); return 1; }
    std::printf("模型输入 %ux%u type=%d fmt=%d | 输出 %u 个 | pass_through=%d\n",
                engine.info().input_width, engine.info().input_height,
                engine.info().input_type, engine.info().input_fmt,
                engine.info().n_outputs, pass_through ? 1 : 0);
    for (uint32_t i = 0; i < engine.info().n_outputs; ++i) {
        const auto& oi = engine.info().outputs[i];
        std::printf("  out[%u]: type=%d size=%u n_elems=%u scale=%.9f zp=%d dims=", i, oi.type, oi.size, oi.n_elems, oi.scale, oi.zp);
        for (auto d : oi.dims) std::printf("%u ", d);
        std::printf("\n");
    }
    if (info_only) {
        return 0;
    }

    // 输入准备（与 Worker 链路一致：以模型实际输入类型为准，不猜格式）
    //   INT8/UINT8（黄瓦 320x320）：原始 u8（BGR NHWC）直喂，零转换
    //   FLOAT16 等（yolo261n 640x640）：u8 -> FP16 精确 LUT
    const uint8_t* input_ptr = nullptr;
    size_t input_bytes = 0;
    std::vector<uint16_t> fp16;
    const int itype = engine.info().input_type;
    if (itype == 2 || itype == 3) {  // RKNN_TENSOR_INT8 / UINT8
        input_ptr = img.data();
        input_bytes = img.size();
    } else {
        fp16.resize(in_w * in_h * 3);
        if (layout_nchw) {
            // NCHW：按通道平面排列（每个平面 w*h 像素）
            for (uint32_t c = 0; c < 3; ++c) {
                for (uint32_t p = 0; p < in_w * in_h; ++p) {
                    fp16[static_cast<size_t>(c) * in_w * in_h + p] =
                        float_to_half(static_cast<float>(img[p * 3 + c]));
                }
            }
        } else {
            for (size_t i = 0; i < img.size(); ++i) {
                fp16[i] = float_to_half(static_cast<float>(img[i]));
            }
        }
        input_ptr = reinterpret_cast<const uint8_t*>(fp16.data());
        input_bytes = engine.info().input_size;
    }

    if (!engine.set_input(input_ptr, input_bytes, &err) ||
        !engine.run(&err)) { std::fprintf(stderr, "[FAIL] infer: %s\n", err.c_str()); return 1; }
    std::vector<std::vector<uint8_t>> raw(engine.info().n_outputs);
    std::vector<void*> ptrs(engine.info().n_outputs);
    std::vector<size_t> sizes(engine.info().n_outputs);
    for (uint32_t i = 0; i < engine.info().n_outputs; ++i) {
        raw[i].resize(engine.info().outputs[i].size);
        ptrs[i] = raw[i].data();
        sizes[i] = engine.info().outputs[i].size;
    }
    if (!engine.get_raw_outputs(ptrs.data(), sizes.data(), &err)) {
        std::fprintf(stderr, "[FAIL] raw outputs: %s\n", err.c_str());
        return 1;
    }
    if (!dump_raw_path.empty()) {
        // 多输出模型（黄瓦 6 输出）：逐一 dump 为 <path>.N，供与 rknnlite 原生输出对照
        for (uint32_t i = 0; i < engine.info().n_outputs; ++i) {
            const std::string fp = dump_raw_path + "." + std::to_string(i);
            std::ofstream of(fp, std::ios::binary);
            of.write(reinterpret_cast<const char*>(raw[i].data()), raw[i].size());
        }
        std::printf("已 dump raw 输出 (%u 个, 主文件 %s)\n", engine.info().n_outputs,
                    dump_raw_path.c_str());
    }

    // C++ Decode/NMS（不映射，640 空间；阈值与 Python 一致）
    DecodeNMS decode;
    DecodeParams dp;
    dp.conf_thres = conf;
    dp.iou_thres = iou;
    dp.classwise = true;
    dp.input_w = in_w;
    dp.input_h = in_h;
    dp.frame_w = in_w;
    dp.frame_h = in_h;
    if (!decode.configure(dp, &err)) { std::fprintf(stderr, "[FAIL] decode: %s\n", err.c_str()); return 1; }
    std::vector<DetectionBox> dets;
    if (!decode.process(engine.info(), ptrs.data(), &dets, &err)) {
        std::fprintf(stderr, "[FAIL] process: %s\n", err.c_str());
        return 1;
    }
    std::printf("C++ detections: %zu (candidates=%llu)\n", dets.size(),
                (unsigned long long)decode.stats().candidates.load());
    // score 降序输出（便于与 Python 对照）
    std::sort(dets.begin(), dets.end(), [](const DetectionBox& a, const DetectionBox& b) {
        return a.score > b.score;
    });
    for (const auto& d : dets) {
        std::printf("  class=%d score=%.6f box=(%.2f, %.2f, %.2f, %.2f)\n",
                    d.class_id, d.score, d.x1, d.y1, d.x2, d.y2);
    }
    return 0;
}
