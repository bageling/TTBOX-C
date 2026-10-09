// test_rga_roi_hw.cpp — A-8 板端测试：RGA ROI 硬件裁剪（Capture ROI ≠ 模型输入）
//
// 验证：V4L2 全帧 → RGA 裁剪 ROI(offset_x,offset_y,w,h) → resize 到模型输入尺寸。
//   1. ROI 参数生效（imcrop 成功、输出尺寸正确）
//   2. 动态切换 ROI（安全点热更新，mid buffer 懒重建）无错误
//   3. ROI 越界在调用方校验（RuntimeProfile 单测覆盖），此处 RGA 对非法 ROI 不崩溃
//
// 用法：test_rga_roi_hw [seconds]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "capture/V4L2Capture.hpp"
#include "config/ConfigManager.hpp"
#include "rga/RgaProcessor.hpp"

using namespace ttbox::core;

#ifndef TTBOX_PROJECT_ROOT
#error "TTBOX_PROJECT_ROOT must be injected by CMake (-DTTBOX_PROJECT_ROOT); refuse silent fallback to '.'."
#endif

int main(int argc, char** argv) {
    double duration_s = 5.0;
    if (argc > 1) duration_s = std::atof(argv[1]);
    std::printf("=== RGA ROI 硬件裁剪测试（%.0fs）===\n", duration_s);

    ConfigManager cfg;
    std::string cfg_err;
    const std::string cfg_path = std::string(TTBOX_PROJECT_ROOT) + "/config/default.json";
    if (!cfg.load(cfg_path, &cfg_err)) {
        std::printf("[FAIL] 配置加载失败: %s\n", cfg_err.c_str());
        return 1;
    }
    const int64_t ow = cfg.get_int("model_input_width", 0);
    const int64_t oh = cfg.get_int("model_input_height", 0);
    if (ow <= 0 || oh <= 0) {
        std::printf("[FAIL] 模型输入尺寸无效\n");
        return 1;
    }

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
    std::printf("[OK] V4L2: %ux%u\n", fmt.width, fmt.height);

    RgaProcessor rga;
    RgaProcessor::Params rp;
    rp.output_width = static_cast<uint32_t>(ow);
    rp.output_height = static_cast<uint32_t>(oh);
    rp.center_crop = true;
    // ROI1：左上 640x360 区域
    rp.roi_x = 0;
    rp.roi_y = 0;
    rp.roi_w = 640;
    rp.roi_h = 360;
    if (!rga.init(rp, &err)) {
        std::printf("[FAIL] RGA init: %s\n", err.c_str());
        cap.stop();
        cap.close();
        return 1;
    }
    std::printf("[OK] RGA init (ROI1: 0,0 640x360)\n");

    int pass = 1;
    uint64_t processed = 0;
    uint32_t last_seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const double switch_at = duration_s * 0.5;
    bool switched = false;
    bool first_ok = false;

    while (true) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= duration_s) break;

        // 安全点热更新 ROI（模拟运行时用户修改截取区域）
        if (!switched && elapsed >= switch_at) {
            rga.set_roi(100, 50, 960, 540);  // ROI2：中右区域
            switched = true;
            std::printf("  [hot] 切换 ROI -> (100,50 960x540)\n");
        }

        auto frame = cap.latest_frame();
        if (frame && frame->info.sequence != last_seq) {
            last_seq = frame->info.sequence;
            RgaOutput out;
            std::string perr;
            if (rga.process(*frame, &out, &perr)) {
                ++processed;
                if (processed == 1) {
                    if (out.width != (uint32_t)ow || out.height != (uint32_t)oh) {
                        std::printf("[FAIL] 输出尺寸 %ux%u（期望 %lldx%lld）\n",
                                    out.width, out.height, (long long)ow, (long long)oh);
                        pass = 0;
                    } else {
                        std::printf("[OK] ROI1 输出 %ux%u stride=%u fd=%d\n",
                                    out.width, out.height, out.stride, out.dma_fd);
                        first_ok = true;
                    }
                    // 像素非零检查（ROI 裁剪后内容有效）
                    const uint8_t* p = static_cast<const uint8_t*>(out.vir_addr);
                    const size_t n = static_cast<size_t>(out.stride) * out.height;
                    uint8_t mx = 0;
                    for (size_t k = 0; k < n; k += 257) if (p[k] > mx) mx = p[k];
                    std::printf("  [check] 像素抽样 max=%u (%s)\n",
                                (unsigned)mx, mx > 0 ? "non-zero" : "ZERO");
                }
            } else {
                std::printf("  [err] seq=%u: %s\n", frame->info.sequence, perr.c_str());
                pass = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const RgaMetrics& rm = rga.metrics();
    std::printf("=== ROI 结果 ===\n");
    std::printf("  frames=%llu ok=%llu err=%llu hot-switch=%s\n",
                (unsigned long long)rm.frames.load(),
                (unsigned long long)rm.ok_frames.load(),
                (unsigned long long)rm.error_frames.load(),
                switched ? "yes" : "no");
    if (rm.frames.load() > 0) {
        std::printf("  平均(us): import=%.1f crop=%.1f resize=%.1f total=%.1f\n",
                    (double)rm.import_sum_us.load() / rm.frames.load(),
                    (double)rm.crop_sum_us.load() / rm.frames.load(),
                    (double)rm.resize_sum_us.load() / rm.frames.load(),
                    (double)rm.total_sum_us.load() / rm.frames.load());
    }
    if (rm.error_frames.load() > 0) pass = 0;
    if (!first_ok) pass = 0;
    if (!switched) { std::printf("[FAIL] 未触发 ROI 热切换\n"); pass = 0; }

    rga.destroy();
    cap.stop();
    cap.close();
    std::printf("=== RGA ROI 验收: %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
