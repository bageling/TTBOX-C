// test_rknn_hw.cpp — 板端 RKNN C API 单 Worker 硬件验收（RK3588）
//
// 模式：
//   raw   ：纯推理吞吐（固定输入 buffer，两种 pass_through 对比；N 帧）
//   chain ：capture → RGA → RKNN 串行链路（RGA 输出 mmap va 零拷贝直喂 RKNN；30s）
//
// 输出：模型加载时间、set_input/run/output/total 统计（min/avg/p50/p95/p99/max）、
//       理论吞吐 FPS、实际吞吐 FPS、NPU Core0/1/2 利用率。
//
// 用法：test_rknn_hw --mode raw|chain --model <path> [--frames N]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "capture/V4L2Capture.hpp"
#include "common/Stats.hpp"
#include "config/ConfigManager.hpp"
#include "rga/RgaProcessor.hpp"
#include "rknn/NpuMonitor.hpp"
#include "rknn/RKNNEngine.hpp"

using namespace ttbox::core;

#ifndef TTBOX_PROJECT_ROOT
#error "TTBOX_PROJECT_ROOT must be injected by CMake (-DTTBOX_PROJECT_ROOT); refuse silent fallback to '.'."
#endif

namespace {

const char* kDefaultModel = TTBOX_PROJECT_ROOT "/models/yolo261n-rk3588.rknn";

void print_stage(const char* tag, const StatsCollector& s) {
    std::printf("  %-10s N=%-5zu  min=%6llu  avg=%8.1f  p50=%6llu  p95=%6llu  p99=%6llu  max=%6llu us\n",
                tag, s.count(),
                (unsigned long long)s.min(),
                s.avg(),
                (unsigned long long)s.percentile(50.0),
                (unsigned long long)s.percentile(95.0),
                (unsigned long long)s.percentile(99.0),
                (unsigned long long)s.max());
}

}  // namespace

// ---------------------------------------------------------------------------
// raw：纯推理吞吐
// ---------------------------------------------------------------------------
static int run_raw(const std::string& model_path, int core_mask,
                   uint32_t in_w, uint32_t in_h, int frames) {
    std::printf("=== raw 模式：纯推理吞吐（%d 帧）===\n", frames);

    // 模型输入层为 FP16（size=640*640*3*2）。C API 严格校验 size，
    // uint8 输入（1228800B）会被拒绝 → 统一使用 FP16 输入 buffer。
    // pass_through=1：零拷贝直喂；pass_through=0：runtime 内部处理（对照）。
    std::vector<uint8_t> in_u8(in_w * in_h * 3);
    for (size_t i = 0; i < in_u8.size(); ++i) {
        in_u8[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);  // 渐变
    }
    std::vector<uint16_t> in_fp16(in_w * in_h * 3);
    for (size_t i = 0; i < in_fp16.size(); ++i) {
        const float v = static_cast<float>(in_u8[i]);
        // float -> IEEE half（简化：截 float 高 16 位，仅作吞吐测试）
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        in_fp16[i] = static_cast<uint16_t>((bits >> 13) & 0xFFFF);
    }

    for (int pass = 1; pass >= 0; --pass) {
        RKNNEngine engine;
        std::string err;
        RKNNEngine::Params p;
        p.model_path = model_path;
        p.core_mask = core_mask;
        p.pass_through = (pass == 1);
        const auto t_load0 = std::chrono::steady_clock::now();
        if (!engine.init(p, &err)) {
            std::printf("[FAIL] engine.init(pass_through=%d): %s\n", pass, err.c_str());
            return 1;
        }
        const double load_ms = engine.load_ms();
        (void)t_load0;
        std::printf("--- pass_through=%d（%s）---\n", pass,
                    engine.pass_through_active() ? "零拷贝" : "runtime 复制/转换");
        std::printf("  模型加载+init: %.1f ms | 输入 %ux%u type=%d fmt=%d size=%uB | 输出 %u 个\n",
                    load_ms, engine.info().input_width, engine.info().input_height,
                    engine.info().input_type, engine.info().input_fmt, engine.info().input_size,
                    engine.info().n_outputs);

        // 预热
        std::vector<std::vector<float>> outputs;
        // 两轮均使用 FP16 输入（模型输入层 FP16）；pass 仅控制 pass_through 标志
        const void* buf = in_fp16.data();
        const size_t buf_size = engine.info().input_size;
        bool warmup_ok = true;
        for (int i = 0; i < 5; ++i) {
            if (!engine.infer(buf, buf_size, outputs, &err)) {
                warmup_ok = false;
                break;
            }
        }
        if (!warmup_ok) {
            std::printf("[FAIL] 预热失败（pass_through=%d）: %s\n", pass, err.c_str());
            engine.destroy();
            continue;  // 记录并继续下一轮（如 pass_through=0 不可用）
        }
        engine.reset_stats();  // 预热不计入统计

        NpuMonitor npu;
        npu.start(200, &err);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) {
            if (!engine.infer(buf, buf_size, outputs, &err)) {
                std::printf("[FAIL] infer[%d]: %s\n", i, err.c_str());
                npu.stop();
                return 1;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        npu.stop();
        const double elapsed_s =
            std::chrono::duration<double>(t1 - t0).count();

        const auto& st = engine.stats();
        print_stage("set_input", st.set_input);
        print_stage("run", st.run);
        print_stage("output", st.output);
        print_stage("total", st.total);

        const double total_avg_us = st.total.avg();
        const double theo_fps = total_avg_us > 0 ? 1000000.0 / total_avg_us : 0.0;
        const double real_fps = static_cast<double>(frames) / elapsed_s;
        std::printf("  理论吞吐 FPS ≈ %.1f（1000/total_avg） | 实际吞吐 FPS ≈ %.1f（%d 帧 / %.2fs）\n",
                    theo_fps, real_fps, frames, elapsed_s);

        const NpuLoadSummary npu_s = npu.summary();
        std::printf("  NPU 利用率（%zu 次采样）: Core0=%.1f%% Core1=%.1f%% Core2=%.1f%%\n",
                    npu_s.samples, npu_s.core0, npu_s.core1, npu_s.core2);
        engine.destroy();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// chain：capture → RGA → RKNN（单 Worker 串行，零拷贝）
// ---------------------------------------------------------------------------
static int run_chain(const std::string& model_path, int core_mask,
                     uint32_t in_w, uint32_t in_h, double duration_s) {
    std::printf("=== chain 模式：capture → RGA → RKNN 串行链路（%.0fs）===\n", duration_s);

    V4L2Capture cap;
    std::string err;
    V4L2Capture::Params cp;
    cp.device = "/dev/video0";
    cp.num_buffers = 4;
    if (!cap.configure(cp, &err) || !cap.open(&err) || !cap.start(&err)) {
        std::printf("[FAIL] capture: %s\n", err.c_str());
        return 1;
    }
    const V4L2Capture::FormatInfo& fmt = cap.format();
    std::printf("  V4L2 实际时序: %ux%u fourcc=%s\n", fmt.width, fmt.height,
                fmt.fourcc_str().c_str());

    RgaProcessor rga;
    if (!rga.init({in_w, in_h, true}, &err)) {
        std::printf("[FAIL] rga.init: %s\n", err.c_str());
        cap.stop();
        cap.close();
        return 1;
    }

    RKNNEngine engine;
    RKNNEngine::Params ep;
    ep.model_path = model_path;
    ep.core_mask = core_mask;
    // 模型输入层 FP16：RGA 输出 uint8 → CPU 查表转 FP16 → pass_through=1 零拷贝直喂
    ep.pass_through = true;
    if (!engine.init(ep, &err)) {
        std::printf("[FAIL] engine.init: %s\n", err.c_str());
        rga.destroy();
        cap.stop();
        cap.close();
        return 1;
    }
    std::printf("  模型加载: %.1f ms | 输入 %ux%u | pass_through=%s\n",
                engine.load_ms(), engine.info().input_width, engine.info().input_height,
                ep.pass_through ? "on(零拷贝)" : "off");

    // uint8 -> FP16 查表（仅 256 个值，转换极快；非精度路径，吞吐测试用）
    uint16_t u8_to_half[256];
    for (int i = 0; i < 256; ++i) {
        const float f = static_cast<float>(i);
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        u8_to_half[i] = static_cast<uint16_t>((b >> 13) & 0xFFFF);
    }
    // FP16 输入 buffer（模型输入 size：W*H*3*2）
    std::vector<uint16_t> fp16_buf(engine.info().input_size / 2);
    StatsCollector convert_stats;

    NpuMonitor npu;
    npu.start(200, &err);

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t processed = 0;
    uint32_t last_seq = 0;
    std::vector<std::vector<float>> outputs;
    while (true) {
        const double elapsed_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed_s >= duration_s) break;

        auto frame = cap.latest_frame();
        if (!frame || frame->info.sequence == last_seq) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last_seq = frame->info.sequence;

        RgaOutput rga_out;
        std::string perr;
        if (!rga.process(*frame, &rga_out, &perr)) {
            std::printf("  [err] rga: %s\n", perr.c_str());
            continue;
        }
        // uint8 BGR -> FP16（查表）
        const auto tc0 = std::chrono::steady_clock::now();
        {
            const uint8_t* src = static_cast<const uint8_t*>(rga_out.vir_addr);
            const uint32_t w = rga_out.width;
            const uint32_t h = rga_out.height;
            const uint32_t src_stride = rga_out.stride;              // 字节
            const uint32_t dst_stride_px = src_stride / 3;           // 像素（16 对齐）
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* srow = src + static_cast<size_t>(y) * src_stride;
                uint16_t* drow = fp16_buf.data() + static_cast<size_t>(y) * dst_stride_px * 3;
                for (uint32_t x = 0; x < w * 3; ++x) {
                    drow[x] = u8_to_half[srow[x]];
                }
            }
        }
        convert_stats.add(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - tc0).count());

        // 零拷贝：FP16 buffer 直喂 RKNN
        if (!engine.infer(fp16_buf.data(), engine.info().input_size, outputs, &err)) {
            std::printf("  [err] infer: %s\n", err.c_str());
            continue;
        }
        ++processed;
        if (processed == 1 || processed % 100 == 0) {
            std::printf("  [chain] 帧 %llu seq=%u -> rga %ux%u -> fp16 -> rknn (total=%u us)\n",
                        (unsigned long long)processed, frame->info.sequence,
                        rga_out.width, rga_out.height,
                        (unsigned)engine.stats().total.max());
        }
    }
    npu.stop();
    cap.stop();
    const double elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const auto& st = engine.stats();
    print_stage("set_input", st.set_input);
    print_stage("run", st.run);
    print_stage("output", st.output);
    print_stage("total", st.total);
    print_stage("convert(u8->fp16)", convert_stats);
    const double real_fps = processed / elapsed_s;
    const double theo_fps = st.total.avg() > 0 ? 1000000.0 / st.total.avg() : 0.0;
    std::printf("  实际链路 FPS ≈ %.1f（%llu 帧 / %.2fs） | 理论(单帧) FPS ≈ %.1f\n",
                real_fps, (unsigned long long)processed, elapsed_s, theo_fps);
    const NpuLoadSummary npu_s = npu.summary();
    std::printf("  NPU 利用率（%zu 次采样）: Core0=%.1f%% Core1=%.1f%% Core2=%.1f%%\n",
                npu_s.samples, npu_s.core0, npu_s.core1, npu_s.core2);
    const V4L2Metrics& vm = cap.metrics();
    std::printf("  V4L2: capture=%llu dqbuf=%llu qbuf=%llu errors=%llu fps=%.1f\n",
                (unsigned long long)vm.capture_frames.load(),
                (unsigned long long)vm.dqbuf_frames.load(),
                (unsigned long long)vm.qbuf_frames.load(),
                (unsigned long long)vm.errors.load(),
                vm.capture_fps.load());

    engine.destroy();
    rga.destroy();
    cap.close();
    return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string mode = "raw";
    std::string model = kDefaultModel;
    int frames = 300;
    double duration = 30.0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "缺少参数: %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--mode") mode = next("--mode");
        else if (a == "--model") model = next("--model");
        else if (a == "--frames") frames = std::atoi(next("--frames").c_str());
        else if (a == "--duration") duration = std::atof(next("--duration").c_str());
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); return 1; }
    }

    // 配置（core_mask / 模型输入尺寸来自 config，不写死）
    ConfigManager cfg;
    std::string cerr;
    const std::string cfg_path = std::string(TTBOX_PROJECT_ROOT) + "/config/default.json";
    if (!cfg.load(cfg_path, &cerr)) {
        std::printf("[FAIL] 配置加载失败: %s\n", cerr.c_str());
        return 1;
    }
    const int core_mask = static_cast<int>(cfg.get_int("core_mask", 0));
    const int64_t iw = cfg.get_int("model_input_width", 0);
    const int64_t ih = cfg.get_int("model_input_height", 0);
    if (iw <= 0 || ih <= 0) {
        std::printf("[FAIL] config 模型输入尺寸无效\n");
        return 1;
    }
    std::printf("=== ttbox_core RKNN C API 硬件验收（mode=%s, model=%s, core_mask=%d, in=%lldx%lld）===\n",
                mode.c_str(), model.c_str(), core_mask, (long long)iw, (long long)ih);

    if (mode == "raw") {
        return run_raw(model, core_mask, static_cast<uint32_t>(iw),
                       static_cast<uint32_t>(ih), frames);
    }
    if (mode == "chain") {
        return run_chain(model, core_mask, static_cast<uint32_t>(iw),
                         static_cast<uint32_t>(ih), duration);
    }
    std::fprintf(stderr, "未知 mode: %s（raw|chain）\n", mode.c_str());
    return 1;
}
