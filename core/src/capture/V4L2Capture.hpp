// V4L2Capture.hpp — V4L2 → MMAP → DQBUF → VIDIOC_EXPBUF → DMA-BUF fd → LatestFrame
//
// 阶段 A-2 采集模块。边界设计：
//   V4L2Capture → LatestFrame(FrameBuffer{info.dma_fd}) → RgaProcessor（下一阶段）
//
// 关键语义：
//   - latest-frame：新帧覆盖旧帧，无队列；consumer 永远拿最新帧
//   - DMA-BUF fd 对应的 V4L2 buffer 不能在 consumer 仍使用时 QBUF 归还：
//     通过 shared_ptr/weak_ptr 引用计数判定（weak.lock() 为空 = 无消费者引用 → 可归还）
//   - 不允许 V4L2 buffer starvation：多 buffer 池（默认 4）提供余量；
//     全部被占时 poll 超时等待，不忙等、不崩溃
//
// 线程模型：capture thread（内部）+ 任意 consumer。start()/stop() 负责线程生命周期。
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/Types.hpp"
#include "capture/ICapture.hpp"

namespace ttbox::core {

// ---------------------------------------------------------------------------
// LatestFrame：线程安全 latest-frame 容器
// ---------------------------------------------------------------------------

class LatestFrame {
public:
    // 发布新帧：替换当前帧（旧帧被覆盖）。返回被替换的旧帧（可为空）。
    std::shared_ptr<FrameBuffer> publish(std::shared_ptr<FrameBuffer> frame);

    // 获取当前最新帧（shared_ptr 保活：旧帧在 consumer 使用时不会提前归还）。
    std::shared_ptr<FrameBuffer> get() const;

    // 阻塞等待"sequence != after_seq"的新帧，最多等 timeout_us 微秒；
    // 超时返回 nullptr（调用方据此检查退出标志）。
    //
    // 存在的理由：consumer（3 个推理 worker）此前是固定 400 µs 轮询 get()，
    // 平均要空等 ~200 µs 才能发现新帧 —— 板端实测 queue_wait ≈ 0.239 ms 正是
    // 这笔开销（2026-09-23）。改成事件唤醒后，帧到达即被唤醒（微秒级），
    // 且锁/原子操作次数从 ~7500 次/秒降到 ~600 次/秒。
    // 仍保留超时（不永久 wait）：① 停止时能退出；② 极端情况下丢通知也能自愈。
    std::shared_ptr<FrameBuffer> wait_new(uint32_t after_seq, int timeout_us) const;

    void clear();

private:
    std::shared_ptr<FrameBuffer> current_;
    // 仅用于"有新帧"这一事件的通知，**不保护 current_**（current_ 走原子 shared_ptr）。
    // publish 持本锁的时间只有一次 notify_all，不会让采集线程阻塞在 consumer 上。
    mutable std::mutex notify_mutex_;
    mutable std::condition_variable notify_cv_;
    mutable std::atomic<int> waiters_{0};
};

// ---------------------------------------------------------------------------
// 采集指标（线程安全计数）
// ---------------------------------------------------------------------------

struct V4L2Metrics {
    std::atomic<uint64_t> capture_frames{0};        // 成功发布到 latest 的帧数
    std::atomic<uint64_t> dqbuf_frames{0};          // DQBUF 成功次数
    std::atomic<uint64_t> qbuf_frames{0};           // QBUF 归还次数
    // 被更新帧覆盖掉的帧数（latest 语义）。正常行为，不是丢帧；稳态下 ≈ capture_frames。
    std::atomic<uint64_t> superseded_latest_frames{0};
    std::atomic<uint64_t> poll_timeouts{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<double> capture_fps{0.0};
    std::atomic<int64_t> last_frame_ts_ms{0};   // 最新帧 v4l2 单调时间戳（ms，供 buffer_age_ms 计算）
};

// ---------------------------------------------------------------------------
// V4L2Capture
// ---------------------------------------------------------------------------

class V4L2Capture : public capture::ICapture {
public:
    struct Params {
        // ★ 2026-10-03 补注：默认值 /dev/video0 是**RK3588 HDMI-RX 的硬约束**
        //   （DRM card/render 节点只服务 loopout 与 NPU）。生产上真正的取值来自
        //   配置项 capture_device，由 Application 校验后回填；
        //   这里只是「没传就用它」的兜底，**不要照风扇 hwmon / UDC 那套改成扫 /sys 猜节点**。
        std::string device = "/dev/video0";
        uint32_t num_buffers = 8;   // 请求 buffer 数（以驱动实际为准，实际更少时降级）
        int poll_timeout_ms = 1000; // poll 超时（ms）
        // 可选 V4L2 Selection/Crop；0 表示不请求硬件裁剪。
        uint32_t crop_x = 0;
        uint32_t crop_y = 0;
        uint32_t crop_width = 0;
        uint32_t crop_height = 0;
        // true = 忽略 crop_x/crop_y，按实际帧尺寸把裁剪窗居中（自瞄画面的语义就是中心）。
        // 居中要在 open() 里拿到 G_FMT 之后才算得出，所以放在这里而不是让调用方填。
        bool crop_center = true;
    };

    // 实际协商格式（open 后有效，不强制改分辨率）
    struct FormatInfo {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pixelformat = 0;   // fourcc
        uint32_t num_planes = 0;
        std::vector<uint32_t> bytesperline;
        std::vector<uint32_t> sizeimage;
        bool selection_supported = false;
        bool selection_applied = false;
        std::string fourcc_str() const;
    };

    V4L2Capture();
    ~V4L2Capture();
    // ★ 本类**不要派生**：析构里调虚函数 stop() / close()
    //   （cppcheck virtualCallInConstructor 告警，见 IHidCapture 的 open/start/stop/close）。
    //   C++ 规定：派生类析构期间动态派发已停止到基类⇒ 派生类的 stop()/close()
    //   **不会**被执行 ⇒ 派生类自己 open 的 dma-buf fd / mmap 会在析构里泄漏
    //   （采集设备泄漏 = 设备节点占死，板端表现为重启后 /dev/video0 打不开）。
    //   ⇒ 真要派生时，改成"基类析构只清基类资源，派生资源由派生类析构自己清"。
    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    // configure：保存参数（不打开设备）
    bool configure(const Params& params, std::string* error = nullptr);

    // open：QUERYCAP → G_FMT → REQBUFS → 每 plane QUERYBUF+mmap+EXPBUF
    bool open(std::string* error = nullptr) override;

    // start：QBUF 全部 buffer → STREAMON → 启动 capture thread
    bool start(std::string* error = nullptr) override;

    // stop：请求退出 → join 线程 → STREAMOFF → 归还所有已 DQBUF 的 buffer
    void stop() override;

    // close：munmap 全部 plane → close 全部 dma-buf fd → close 设备 fd（幂等）
    void close() override;

    bool running() const { return running_.load(); }

    // 消费者入口：最新帧（info.dma_fd 供下一阶段 RgaProcessor 消费）
    std::shared_ptr<FrameBuffer> latest_frame() const override { return latest_.get(); }

    // 共享 LatestFrame 引用（A-5 Worker 池：多 worker 并发取最新帧，无队列）。
    // 生命周期：调用方必须保证本 capture 在 worker 池停止前存活。
    LatestFrame* latest_frame_ref() { return &latest_; }

    const FormatInfo& format() const { return format_; }
    const V4L2Metrics& metrics() const { return metrics_; }
    uint32_t buffer_count() const { return buffer_count_; }
    // 当前被占用（已 DQBUF 未 QBUF 归还）的 buffer 数（排队深度探测）
    uint32_t in_use_count() const;

    // 调试/验收：每个 buffer 主 plane 的 DMA-BUF fd 列表
    std::vector<int> dma_fds() const;

private:
    void capture_loop();
    void release_ready_buffers();
    std::vector<size_t> plane_lengths_of(uint32_t index);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    Params params_;
    FormatInfo format_;
    V4L2Metrics metrics_;
    LatestFrame latest_;
    std::atomic<bool> running_{false};
    std::thread capture_thread_;
    uint32_t buffer_count_ = 0;
    int fd_ = -1;
    bool opened_ = false;

    // ---- 格式稳定等待的冷热判定（2026-10-04 性能修复）----
    // 背景：open() 里有一处 **800ms 硬等**（等 hdmirx 从中间格式过渡到最终格式）。
    //   它对**开机后的第一次打开**是必需的 —— 但 stop→start 时设备一直没断电、
    //   格式早稳定，每次再等 800ms 就是纯浪费。
    //   实测（板端 192.168.0.120，V1.0.20）：start 稳定在 974~1019ms（中位 997），
    //   其中 800ms 是这个等待（占 80%），模型加载只占 ~200ms。
    // last_close_ms_：上次 close() 的单调时刻；0 = 从未打开过（= 冷启动）。
    // 判定：距上次关闭不足 kWarmReopenWindowMs ⇒ 视为"热重开"，跳过等待。
    double last_close_ms_ = 0.0;
    // 窗口取 5s：远大于 stop→start 的人工间隔（~1s），
    // 又远小于"设备真的可能重新过渡"的场景（换信号源/拔插通常 >5s）。
    static constexpr double kWarmReopenWindowMs = 5000.0;

    // 滚动 1s 窗口帧率统计（仅 capture_loop 线程访问）。
    // 用滚动窗口而不是“累计帧数/会话耗时”，否则采集卡死后累计均值仍显示
    // 冻结前的 240fps，Web 会把停流误判成健康。
    uint64_t fps_window_frames_ = 0;
    double fps_window_start_ms_ = 0.0;
};

}  // namespace ttbox::core
