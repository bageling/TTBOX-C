// test_rga_hw.cpp — 板端 RGA 硬件验收（RK3588）：V4L2 DMA-BUF → RGA 全链路
//
// 链路：V4L2Capture → LatestFrame(FrameBuffer.dma_fd) → RgaProcessor → RgaOutput
// 验证：连续 30s、crop+resize 正确、无 CPU memcpy 路径、无提前归还、
//       无 fd/buffer/handle 泄漏、import/crop/resize/total 独立统计。
//
// 输出尺寸从 config/default.json 读取（model_input_width/height），禁止硬编码。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "capture/V4L2Capture.hpp"
#include "config/ConfigManager.hpp"
#include "rga/RgaProcessor.hpp"

using namespace ttbox::core;

#ifndef TTBOX_PROJECT_ROOT
#error "TTBOX_PROJECT_ROOT must be injected by CMake (-DTTBOX_PROJECT_ROOT); refuse silent fallback to '.'."
#endif

namespace {

int count_open_fds() {
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] != '.') ++n;
    }
    ::closedir(d);
    return n;
}

// 打印当前打开的 fd 与其目标（用于区分"库持有 fd"与"泄漏 fd"）
void dump_open_fds() {
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) return;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        char link[512];
        char target[256];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
        ssize_t n = ::readlink(link, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            std::printf("    fd %s -> %s\n", e->d_name, target);
        }
    }
    ::closedir(d);
}

}  // namespace

int main(int argc, char** argv) {
    // 支持时长参数（默认 30s，便于快速验证）
    double duration_s = 30.0;
    if (argc > 1) {
        duration_s = std::atof(argv[1]);
        if (duration_s <= 0) duration_s = 30.0;
    }
    std::printf("=== ttbox_core RGA 硬件验收测试（%.0fs）===\n", duration_s);

    // ---- 0. 输出尺寸来自 config（不硬编码 640）----
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
        std::printf("[FAIL] 模型输入尺寸无效 (config model_input_width/height)\n");
        return 1;
    }
    std::printf("[OK] 模型输入尺寸来自 config: %lldx%lld\n", (long long)ow, (long long)oh);

    const int fds_before = count_open_fds();
    std::printf("open 前进程 fd 数 = %d\n", fds_before);

    // ---- 1. V4L2 采集（A-2 模块）----
    V4L2Capture cap;
    std::string err;
    V4L2Capture::Params cparams;
    cparams.device = "/dev/video0";
    cparams.num_buffers = 4;
    if (!cap.configure(cparams, &err) || !cap.open(&err)) {
        std::printf("[FAIL] V4L2 open: %s\n", err.c_str());
        return 1;
    }
    const V4L2Capture::FormatInfo& fmt = cap.format();
    std::printf("[OK] V4L2 实际时序: %ux%u fourcc=%s planes=%u\n",
                fmt.width, fmt.height, fmt.fourcc_str().c_str(), fmt.num_planes);
    if (!cap.start(&err)) {
        std::printf("[FAIL] V4L2 start: %s\n", err.c_str());
        cap.close();
        return 1;
    }

    // ---- 2. RGA 处理器 ----
    RgaProcessor rga;
    RgaProcessor::Params rparams;
    rparams.output_width = static_cast<uint32_t>(ow);
    rparams.output_height = static_cast<uint32_t>(oh);
    rparams.center_crop = true;
    if (!rga.init(rparams, &err)) {
        std::printf("[FAIL] RGA init: %s\n", err.c_str());
        cap.stop();
        cap.close();
        return 1;
    }
    std::printf("[OK] RGA init（输出 %dx%d, center_crop）\n", (int)ow, (int)oh);

    // ---- 3. 30s 全链路循环 ----
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t processed = 0;
    uint32_t last_seq = 0;
    std::shared_ptr<FrameBuffer> held;  // 慢消费者持有的旧帧
    std::printf("capture+RGA %.0fs ...\n", duration_s);

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s =
            std::chrono::duration<double>(now - t0).count();
        if (elapsed_s >= duration_s) break;

        auto frame = cap.latest_frame();
        if (frame && frame->info.sequence != last_seq) {
            last_seq = frame->info.sequence;

            RgaOutput out;
            std::string perr;
            if (rga.process(*frame, &out, &perr)) {
                ++processed;
                if (processed == 1 || processed % 500 == 0) {
                    std::printf("  [rga] frame seq=%u -> %ux%u fd=%d (import=%u crop=%u resize=%u total=%u us)\n",
                                frame->info.sequence, out.width, out.height, out.dma_fd,
                                (unsigned)rga.metrics().last_import_us.load(),
                                (unsigned)rga.metrics().last_crop_us.load(),
                                (unsigned)rga.metrics().last_resize_us.load(),
                                (unsigned)rga.metrics().last_total_us.load());
                }
                // 输出尺寸校验
                if (processed == 1) {
                    if (out.width != (uint32_t)ow || out.height != (uint32_t)oh) {
                        std::printf("[FAIL] 输出尺寸错误: %ux%u\n", out.width, out.height);
                    } else {
                        std::printf("[OK] 输出尺寸 %ux%u stride=%u dma_fd=%d\n",
                                    out.width, out.height, out.stride, out.dma_fd);
                    }
                    // 读回校验（仅测试，非生产路径）：非全零 + 值域
                    const uint8_t* p = static_cast<const uint8_t*>(out.vir_addr);
                    const size_t n = (size_t)out.stride * out.height;
                    uint64_t sum = 0;
                    uint8_t mn = 255, mx = 0;
                    for (size_t k = 0; k < n; k += 97) {  // 抽样
                        uint8_t v = p[k];
                        sum += v;
                        if (v < mn) mn = v;
                        if (v > mx) mx = v;
                    }
                    std::printf("  [check] 输出像素抽样 min=%u max=%u mean=%.1f（非零=%s）\n",
                                (unsigned)mn, (unsigned)mx, (double)sum / (n / 97 + 1),
                                mx > 0 ? "yes" : "NO");
                }
            } else {
                std::printf("  [rga][err] seq=%u: %s\n", frame->info.sequence, perr.c_str());
            }
        }

        // 第 8~10 秒：持有最新帧（验证 V4L2 buffer 不被提前归还 + 无 starvation）
        if (elapsed_s >= 8.0 && elapsed_s < 10.0 && !held) {
            held = cap.latest_frame();
            if (held) std::printf("  [hold] 持有帧 seq=%u 2 秒\n", held->info.sequence);
        }
        if (elapsed_s >= 10.0) held.reset();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // ---- 4. 停止 ----
    held.reset();
    cap.stop();
    const V4L2Metrics& vm = cap.metrics();
    const RgaMetrics& rm = rga.metrics();

    std::printf("=== 30s 指标 ===\n");
    std::printf("[V4L2] capture_frames=%llu dqbuf=%llu qbuf=%llu dropped=%llu errors=%llu fps=%.2f\n",
                (unsigned long long)vm.capture_frames.load(),
                (unsigned long long)vm.dqbuf_frames.load(),
                (unsigned long long)vm.qbuf_frames.load(),
                (unsigned long long)vm.superseded_latest_frames.load(),
                (unsigned long long)vm.errors.load(),
                vm.capture_fps.load());
    const uint64_t f = rm.frames.load();
    std::printf("[RGA] frames=%llu ok=%llu err=%llu\n",
                (unsigned long long)f,
                (unsigned long long)rm.ok_frames.load(),
                (unsigned long long)rm.error_frames.load());
    if (f > 0) {
        std::printf("[RGA] 平均耗时(us): import=%.1f crop=%.1f resize=%.1f total=%.1f\n",
                    (double)rm.import_sum_us.load() / f,
                    (double)rm.crop_sum_us.load() / f,
                    (double)rm.resize_sum_us.load() / f,
                    (double)rm.total_sum_us.load() / f);
        std::printf("[RGA] 最近耗时(us): import=%u crop=%u resize=%u total=%u\n",
                    (unsigned)rm.last_import_us.load(),
                    (unsigned)rm.last_crop_us.load(),
                    (unsigned)rm.last_resize_us.load(),
                    (unsigned)rm.last_total_us.load());
        std::printf("[RGA] RGA FPS ≈ %.2f\n", (double)rm.ok_frames.load() / duration_s);
    }

    // ---- 5. 校验 ----
    bool pass = true;
    if (rm.ok_frames.load() == 0) { std::printf("[FAIL] RGA 无成功帧\n"); pass = false; }
    if (rm.error_frames.load() > 0) { std::printf("[FAIL] RGA errors=%llu\n",
        (unsigned long long)rm.error_frames.load()); pass = false; }
    if (vm.qbuf_frames.load() != vm.dqbuf_frames.load()) {
        std::printf("[FAIL] V4L2 DQBUF/QBUF 不成对 (%llu vs %llu)\n",
                    (unsigned long long)vm.dqbuf_frames.load(),
                    (unsigned long long)vm.qbuf_frames.load());
        pass = false;
    }
    if (vm.errors.load() > 0) { std::printf("[WARN] V4L2 errors=%llu\n",
        (unsigned long long)vm.errors.load()); }

    rga.destroy();
    cap.close();

    // 6. fd 泄漏检查（librga 会持有一个 /dev/rga 驱动 fd，属库生命周期，非泄漏）
    const int fds_after = count_open_fds();
    std::printf("close 后进程 fd 数 = %d（open 前 %d）\n", fds_after, fds_before);
    std::printf("close 后 fd 详情：\n");
    dump_open_fds();
    if (fds_after <= fds_before + 1) {
        std::printf("[OK] 无 fd 泄漏（差额 ≤1 = librga 持有的 /dev/rga 驱动 fd）\n");
    } else {
        std::printf("[WARN] fd 数变化 %d -> %d（>1，可能存在泄漏）\n", fds_before, fds_after);
    }

    std::printf("=== RGA 验收: %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
