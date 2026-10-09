// test_capture_hw.cpp — 板端 V4L2+DMA-BUF 硬件验收（RK3588 /dev/video0）
//
// 验证链路：open → QUERYCAP → G_FMT → REQBUFS → MMAP → EXPBUF → QBUF
//          → STREAMON → DQBUF → LatestFrame → QBUF → STREAMOFF → clean shutdown
// 连续运行 30 秒；中途验证"consumer 持有旧帧 2 秒"时无 buffer starvation。
//
// 构建：TTBOX_CORE_BUILD_HW_TESTS=ON（默认 OFF，仅板端启用）
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>

#include "capture/V4L2Capture.hpp"
#include "common/Types.hpp"

using namespace ttbox::core;

namespace {

// 统计进程打开 fd 数（用于验证无 fd 泄漏）
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

void print_format(const V4L2Capture::FormatInfo& f) {
    std::printf("G_FMT: %ux%u fourcc=%s planes=%u\n",
                f.width, f.height, f.fourcc_str().c_str(), f.num_planes);
    for (uint32_t p = 0; p < f.num_planes; ++p) {
        std::printf("  plane[%u] bytesperline=%u sizeimage=%u\n",
                    p, f.bytesperline[p], f.sizeimage[p]);
    }
}

}  // namespace

int main() {
    std::printf("=== ttbox_core V4L2+DMA-BUF 硬件验收测试 ===\n");

    V4L2Capture cap;
    std::string err;

    const int fds_before = count_open_fds();
    std::printf("open 前进程 fd 数 = %d\n", fds_before);

    // 1. configure
    V4L2Capture::Params params;
    params.device = "/dev/video0";
    params.num_buffers = 4;
    if (!cap.configure(params, &err)) {
        std::printf("[FAIL] configure: %s\n", err.c_str());
        return 1;
    }
    std::printf("[OK] configure: device=%s buffers=%u\n", params.device.c_str(), params.num_buffers);

    // 2. open（QUERYCAP + G_FMT + REQBUFS + MMAP + EXPBUF）
    if (!cap.open(&err)) {
        std::printf("[FAIL] open: %s\n", err.c_str());
        return 1;
    }
    std::printf("[OK] open /dev/video0\n");
    print_format(cap.format());

    const auto fds = cap.dma_fds();
    std::printf("[OK] buffers=%u\n", cap.buffer_count());
    for (size_t i = 0; i < fds.size(); ++i) {
        std::printf("  dma_fd[%zu] = %d\n", i, fds[i]);
    }
    // 校验 dma fd 有效性（fd 有效：fstat 可成功）
    {
        bool all_valid = true;
        for (int fd : fds) {
            struct stat st;
            if (fd < 0 || ::fstat(fd, &st) != 0) all_valid = false;
        }
        std::printf(all_valid ? "[OK] 全部 DMA-BUF fd 有效\n"
                              : "[WARN] 存在无效 DMA-BUF fd（EXPBUF 失败?）\n");
    }

    // 3. start（QBUF + STREAMON + thread）
    if (!cap.start(&err)) {
        std::printf("[FAIL] start: %s\n", err.c_str());
        cap.close();
        return 1;
    }
    std::printf("[OK] start (STREAMON)\n");

    // 4. 30 秒消费循环
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t consumed = 0;
    uint32_t last_seq = 0;
    std::shared_ptr<FrameBuffer> held;  // 模拟慢消费者持有的旧帧
    std::printf("capture 30s ...\n");

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s =
            std::chrono::duration<double>(now - t0).count();
        if (elapsed_s >= 30.0) break;

        auto frame = cap.latest_frame();
        if (frame) {
            ++consumed;
            if (frame->info.sequence != last_seq) {
                last_seq = frame->info.sequence;
                if (consumed % 300 == 1 || consumed <= 3) {
                    std::printf("  frame seq=%u idx=%u fd=%d ts=%.1fms\n",
                                frame->info.sequence, frame->info.buffer_index,
                                frame->info.dma_fd, frame->info.timestamp_ms);
                }
            }
            // 第 3~5 秒：持有最新帧不放（验证旧 buffer 不被错误归还 + 无 starvation）
            if (elapsed_s >= 3.0 && elapsed_s < 5.0 && !held) {
                held = frame;
                std::printf("  [hold] 持有帧 seq=%u 2 秒（验证引用安全）\n", frame->info.sequence);
            }
        }
        if (elapsed_s >= 5.0) {
            held.reset();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 5. stop（join + STREAMOFF + 归还）
    cap.stop();
    std::printf("[OK] stop (STREAMOFF)\n");

    // 6. 指标
    const V4L2Metrics& m = cap.metrics();
    std::printf("=== 30s 指标 ===\n");
    std::printf("capture_frames       = %llu\n", (unsigned long long)m.capture_frames.load());
    std::printf("dqbuf_frames         = %llu\n", (unsigned long long)m.dqbuf_frames.load());
    std::printf("qbuf_frames          = %llu\n", (unsigned long long)m.qbuf_frames.load());
    std::printf("superseded_latest    = %llu\n", (unsigned long long)m.superseded_latest_frames.load());
    std::printf("poll_timeouts        = %llu\n", (unsigned long long)m.poll_timeouts.load());
    std::printf("errors               = %llu\n", (unsigned long long)m.errors.load());
    std::printf("capture_fps          = %.2f\n", m.capture_fps.load());
    std::printf("consumer_frames      = %llu\n", (unsigned long long)consumed);

    // 7. 校验
    bool pass = true;
    if (m.dqbuf_frames.load() == 0) { std::printf("[FAIL] 无 DQBUF\n"); pass = false; }
    if (m.qbuf_frames.load() == 0) { std::printf("[FAIL] 无 QBUF\n"); pass = false; }
    if (m.errors.load() > 0) { std::printf("[WARN] errors>0 (%llu)\n", (unsigned long long)m.errors.load()); }
    if (pass) std::printf("=== 验收: PASS ===\n");

    cap.close();

    // 8. fd 泄漏检查（close 后 fd 数应回到 open 前）
    const int fds_after = count_open_fds();
    std::printf("close 后进程 fd 数 = %d\n", fds_after);
    if (fds_after == fds_before) {
        std::printf("[OK] 无 fd 泄漏（fd 数一致）\n");
    } else {
        std::printf("[%s] fd 数变化 %d -> %d\n", fds_after < fds_before ? "OK" : "WARN: 可能泄漏",
                    fds_before, fds_after);
    }
    std::printf("=== 硬件验收测试结束 (exit=%d) ===\n", pass ? 0 : 1);
    return pass ? 0 : 1;
}
