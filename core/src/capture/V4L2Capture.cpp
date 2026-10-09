// V4L2Capture.cpp — V4L2 MPLANE 采集实现（RK3588 /dev/video0）
#include "capture/V4L2Capture.hpp"

#if defined(_WIN32)
// Windows 占位：本阶段无 V4L2 硬件，CMake 仅在 Unix 编译本文件。
namespace ttbox::core {
}
#else

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "capture/DmaBuf.hpp"
#include "common/Logger.hpp"
#include "common/CpuAffinity.hpp"
#include "common/RtSched.hpp"

namespace ttbox::core {

namespace {

// ioctl 包装：统一错误处理
int ioctl_call(int fd, unsigned long request, void* arg) {
    return ::ioctl(fd, request, arg);
}

// 本机已启动秒数（读 /proc/uptime 第一列）。读不到返回 -1 =「未知」。
//
// ★ 用途：判断 hdmirx 是否**早已**完成开机过渡 —— open() 里那段 800ms 等待的判据之一。
//   hdmirx 的 format change 只发生在「盒子开机后 1~2 秒」（驱动注释），
//   所以「盒子已启动多久」比「距上次 close 多久」更贴近这件事的真实成因。
// ★ 失败契约：打不开 / 读失败一律返回 -1，**不抛不崩**；调用方把 -1 当「未知」，
//   走最保守的分支（照旧等满 800ms），因此读不到 /proc/uptime 只是慢、不会错。
double boot_uptime_sec() {
    std::FILE* fp = std::fopen("/proc/uptime", "r");
    if (fp == nullptr) {
        return -1.0;
    }
    double up = -1.0;
    if (std::fscanf(fp, "%lf", &up) != 1) {
        up = -1.0;
    }
    std::fclose(fp);
    return up;
}

}  // namespace

// ===========================================================================
// LatestFrame
// ===========================================================================

// 发布新帧（覆盖当前帧）并唤醒等待者，返回被替换的旧帧。
std::shared_ptr<FrameBuffer> LatestFrame::publish(std::shared_ptr<FrameBuffer> frame) {
    // 无锁化（C++17 原子 shared_ptr 自由函数）：capture 线程不再与 3 worker/preview 抢 mutex，
    // 消除 publish 阻塞导致的帧延迟抖动。
    auto old = std::atomic_load_explicit(&current_, std::memory_order_acquire);
    std::atomic_store_explicit(&current_, std::move(frame), std::memory_order_release);
    // 唤醒等待中的 consumer。只在"确实有人在等"时才碰这把锁，且锁内只做
    // notify_all（不做任何数据处理），采集线程被 consumer 阻塞的风险可忽略。
    if (waiters_.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> lk(notify_mutex_);
        notify_cv_.notify_all();
    }
    return old;
}

// 返回当前最新帧（原子读）。
std::shared_ptr<FrameBuffer> LatestFrame::get() const {
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}

// 阻塞等待序号 != after_seq 的新帧；超时返回 nullptr。
std::shared_ptr<FrameBuffer> LatestFrame::wait_new(uint32_t after_seq, int timeout_us) const {
    // 快路径：新帧已到（稳态下绝大多数是这一支），不碰锁。
    auto cur = std::atomic_load_explicit(&current_, std::memory_order_acquire);
    if (cur && cur->info.sequence != after_seq) return cur;

    waiters_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk(notify_mutex_);
    bool woken = true;
    if (timeout_us <= 0) {
        notify_cv_.wait(lk);
    } else {
        woken = notify_cv_.wait_for(lk, std::chrono::microseconds(timeout_us), [&] {
            auto c = std::atomic_load_explicit(&current_, std::memory_order_acquire);
            return c && c->info.sequence != after_seq;
        });
    }
    waiters_.fetch_sub(1, std::memory_order_relaxed);
    if (!woken) return nullptr;
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}

// 清空当前最新帧（原子写为空）。
void LatestFrame::clear() {
    std::atomic_store_explicit(&current_, std::shared_ptr<FrameBuffer>(), std::memory_order_release);
}

// ===========================================================================
// 内部结构
// ===========================================================================

namespace {

// 单个 plane 资源（mmap + dma-buf fd，RAII）
struct PlaneRes {
    void* addr = nullptr;
    size_t length = 0;
    DmaBufFd dma_fd;        // move-only
    uint32_t plane_index = 0;
};

// 单个 V4L2 buffer 资源
struct BufferRes {
    int index = -1;
    std::vector<PlaneRes> planes;
};

// 等待归还的 buffer（weak 引用：无强引用时方可安全 QBUF）
struct PendingRelease {
    int index = -1;
    std::vector<size_t> plane_lengths;
    std::weak_ptr<FrameBuffer> weak;
};

}  // namespace

// V4L2Capture 的设备资源与运行期状态（PIMPL，避免头文件暴露 V4L2 类型）。
struct V4L2Capture::Impl {
    std::vector<BufferRes> buffers;          // REQBUFS 后固定
    std::vector<PendingRelease> pending;     // 待归还队列
    // ★ 2026-10-06 并发修复：captured 此后**只由采集线程**与**join 之后的
    //   stop()/close()** 访问（两者都是同一线程、无并发），不再跨线程读写。
    std::vector<bool> captured;              // index 是否已 DQBUF 未归还
    // 跨线程的"已 DQBUF 未归还"计数职责由本原子计数器承担：采集线程在
    // DQBUF/QBUF 处 fetch_add/fetch_sub，IPC/status 线程只 load。
    // ★ 内存序用 relaxed 即可：它只是诊断计数，不用于发布数据
    //   （帧数据的跨线程发布由 LatestFrame 的原子 shared_ptr 负责）。
    std::atomic<uint32_t> captured_count{0};
};

// ===========================================================================
// 工具
// ===========================================================================

namespace {

// fourcc 数值 → 4 字符名字符串（thread_local 缓冲，返回后立即可用）。
const char* fourcc_name(uint32_t fourcc) {
    static thread_local char buf[5];
    buf[0] = static_cast<char>(fourcc & 0xFF);
    buf[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    buf[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    buf[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    buf[4] = '\0';
    return buf;
}

// V4L2 fourcc → 内部 PixelFormat 枚举（未知返回 kUnknown）。
PixelFormat map_pixel_format(uint32_t fourcc) {
    // V4L2_PIX_FMT_BGR24 = 'BGR3'
    if (fourcc == V4L2_PIX_FMT_BGR24) return PixelFormat::kBGR888;
    if (fourcc == V4L2_PIX_FMT_RGB24) return PixelFormat::kRGB888;
    if (fourcc == V4L2_PIX_FMT_RGB32 || fourcc == V4L2_PIX_FMT_BGR32) return PixelFormat::kRGBA8888;
    if (fourcc == V4L2_PIX_FMT_NV12) return PixelFormat::kNV12;
    return PixelFormat::kUnknown;
}

// timeval（秒 + 微秒）→ 毫秒（double）。
double tv_to_ms(const struct timeval& tv) {
    return static_cast<double>(tv.tv_sec) * 1000.0 +
           static_cast<double>(tv.tv_usec) / 1000.0;
}

}  // namespace

// 返回 pixelformat 的 fourcc 字符串。
std::string V4L2Capture::FormatInfo::fourcc_str() const {
    return fourcc_name(pixelformat);
}

// ===========================================================================
// 生命周期
// ===========================================================================

// 构造：仅创建内部 Impl。
V4L2Capture::V4L2Capture() : impl_(std::make_unique<Impl>()) {}

// 析构：先停采集线程再释放设备资源。
V4L2Capture::~V4L2Capture() {
    stop();
    close();
}

// 校验并保存采集参数（运行中禁止 re-configure；不打开设备）。
bool V4L2Capture::configure(const Params& params, std::string* error) {
    if (running_.load()) {
        if (error) *error = "capture 运行中，禁止 re-configure";
        return false;
    }
    if (params.device.empty()) {
        if (error) *error = "设备路径为空";
        return false;
    }
    if (params.num_buffers == 0 || params.num_buffers > 32) {
        if (error) *error = "num_buffers 必须在 1~32";
        return false;
    }
    params_ = params;
    return true;
}

// 打开设备：open → QUERYCAP → G_FMT → 格式稳定等待 → 可选 Selection/Crop
//          → REQBUFS → 每 plane QUERYBUF+mmap+EXPBUF。带格式不稳定重试循环。
bool V4L2Capture::open(std::string* error) {
    if (opened_) {
        if (error) *error = "设备已打开";
        return false;
    }

    // 重试循环：hdmirx 驱动在开机后需要 1~2 秒才能完成 format change
    // （从中间格式过渡到最终 2560x1440），如果格式不稳定就关闭重开。
    const int kMaxRetries = 5;
    for (int retry = 0; retry < kMaxRetries; ++retry) {
        // 上一次尝试的残余一律先清干净。★ 原来写成 `if (opened_) close();`，
        // 而 opened_ 只在成功后才为真 ⇒ 上一次**中途失败**留下的 fd/mmap/dma-buf
        // 永远不会在重试前被释放（close() 内部还有一道 opened_ 早退，已一并去掉）。
        // close() 现在幂等，无条件调用是安全的。
        close();

        // ---- 1. open ----
        fd_ = ::open(params_.device.c_str(), O_RDWR);
        if (fd_ < 0) {
            if (error) *error = "open(" + params_.device + ") 失败: " + std::string(std::strerror(errno));
            return false;
        }
        TTBOX_LOG_INFO("V4L2 设备已打开: " + params_.device + " fd=" + std::to_string(fd_));

        // ---- 2. QUERYCAP ----
        struct v4l2_capability cap {};
        if (ioctl_call(fd_, VIDIOC_QUERYCAP, &cap) != 0) {
            if (error) *error = "VIDIOC_QUERYCAP 失败";
            close();
            return false;
        }
        const uint32_t caps = cap.capabilities;
        const bool mplane_ok = (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
        const bool streaming_ok = (caps & V4L2_CAP_STREAMING) != 0;
        TTBOX_LOG_INFO("QUERYCAP: driver=" + std::string(reinterpret_cast<const char*>(cap.driver)) +
                       " card=" + std::string(reinterpret_cast<const char*>(cap.card)) +
                       " MPLANE=" + std::string(mplane_ok ? "yes" : "no") +
                       " STREAMING=" + std::string(streaming_ok ? "yes" : "no"));
        if (!mplane_ok || !streaming_ok) {
            if (error) *error = "设备缺少 V4L2_CAP_VIDEO_CAPTURE_MPLANE 或 V4L2_CAP_STREAMING";
            close();
            return false;
        }

        // ---- 3. G_FMT（读取实际格式，不强制改分辨率）----
        struct v4l2_format fmt {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (ioctl_call(fd_, VIDIOC_G_FMT, &fmt) != 0) {
            if (error) *error = "VIDIOC_G_FMT 失败";
            close();
            return false;
        }

        const uint32_t first_w = fmt.fmt.pix_mp.width;
        const uint32_t first_h = fmt.fmt.pix_mp.height;

        // ---- 4. 等待格式稳定（hdmirx 开机过渡）----
        // 原逻辑：无条件 sleep 800ms → 重新读 G_FMT 比对 → 变了就整体重试（最多 5 轮）。
        //
        // ★ 2026-10-04 性能修复（第二次）：把「无条件等」改成「**有证据才跳过**」。
        //
        //   为什么还要第二次修：上一版只让「热重开」（距上次 close < 5s）跳过，
        //   而真实使用里几乎**每次**都是冷路径 —— 人点了停止、隔十几秒再点启动，
        //   必然 > 5s。板端实测（V1.0.21）就是这个缺口：
        //     冷路径 start = **1.07 s**，热路径 = **0.25 s**，差的正是这 800ms。
        //
        //   实测证据（板端 192.168.0.120，uptime 4.6 天）：
        //     连续 40 次**全新 open** 读到的格式全部是 2560/1440 BGR3，一次都没变
        //     ⇒ 驱动早已过渡完毕，这 800ms 纯属白等。
        //
        //   新判据（按序，命中即跳过）：
        //     ① 热重开：距上次 close < kWarmReopenWindowMs(5s)。
        //        设备节点一直没断电（我们只 close 了自己的 fd），格式早稳定。
        //     ② 盒子已启动 >= kBootSettleSec(60s)。
        //        hdmirx 的 format change 只发生在「开机后 1~2 秒」（驱动注释），
        //        60s 有 30x 余量；且 ② 覆盖了绝大多数场景（盒子常年通电）。
        //     ③ 兜底：**照旧睡满 800ms**。开机不到 60s、或 /proc/uptime 读不到
        //        （boot_uptime_sec() 返回 -1 = 未知）都落到这一支 ——
        //        行为与修复前**逐字节一致**，不做任何削减。
        //   ⇒ 一句话：只在「真的可能还没稳」时才等；有证据说稳了就不等。
        //   ★ 跳过后仍会做 G_FMT 重读比对（下面 fmt2 那段照旧执行），
        //     一旦发现格式真的变了，仍走原有的重试逻辑 —— 只是不再「主动等」。
        const double now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        // -1 表示「本进程还没关过设备」⇒ 一定不是热重开（不能拿 0 当"刚刚关过"）。
        const double since_close_ms =
            (last_close_ms_ > 0.0) ? (now_ms - last_close_ms_) : -1.0;
        const bool warm_reopen =
            since_close_ms >= 0.0 && since_close_ms < kWarmReopenWindowMs;
        const double uptime_sec = boot_uptime_sec();
        const bool boot_settled = uptime_sec >= kBootSettleSec;

        if (warm_reopen) {
            TTBOX_LOG_INFO("V4L2 热重开（距上次关闭 "
                           + std::to_string(static_cast<int>(since_close_ms))
                           + " ms < 5s，跳过格式稳定等待）");
        } else if (boot_settled) {
            TTBOX_LOG_INFO("V4L2 冷打开，但盒子已启动 "
                           + std::to_string(static_cast<long long>(uptime_sec))
                           + " s（>= " + std::to_string(static_cast<int>(kBootSettleSec))
                           + " s，hdmirx 早已稳定），跳过格式稳定等待");
        } else {
            TTBOX_LOG_WARN("V4L2 冷打开且盒子启动仅 "
                           + std::to_string(static_cast<long long>(uptime_sec))
                           + " s（< " + std::to_string(static_cast<int>(kBootSettleSec))
                           + " s 或未知），按保守路径等满 800ms 等 hdmirx 过渡");
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }

        struct v4l2_format fmt2 {};
        fmt2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (ioctl_call(fd_, VIDIOC_G_FMT, &fmt2) != 0) {
            if (error) *error = "VIDIOC_G_FMT(重试) 失败";
            close();
            return false;
        }

        if (fmt2.fmt.pix_mp.width != first_w || fmt2.fmt.pix_mp.height != first_h) {
            TTBOX_LOG_WARN("V4L2 格式不稳定: " + std::to_string(first_w) + "x" + std::to_string(first_h) +
                           " -> " + std::to_string(fmt2.fmt.pix_mp.width) + "x" + std::to_string(fmt2.fmt.pix_mp.height) +
                           "，重试 open (" + std::to_string(retry + 1) + "/" + std::to_string(kMaxRetries) + ")");
            ::close(fd_);
            fd_ = -1;
            continue;
        }

        // 格式稳定，使用 fmt2 继续
        fmt = fmt2;

        // 可选硬件 Selection/Crop：失败时保留完整帧，后续由 RGA fallback。
        format_.selection_supported = false;
        format_.selection_applied = false;
        if (params_.crop_width > 0 && params_.crop_height > 0) {
            struct v4l2_selection sel {};
            sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            sel.target = V4L2_SEL_TGT_CROP;
            sel.r.left = static_cast<__s32>(params_.crop_x);
            sel.r.top = static_cast<__s32>(params_.crop_y);
            sel.r.width = params_.crop_width;
            sel.r.height = params_.crop_height;
            // 居中：帧尺寸只有到这一步（G_FMT 之后）才知道，所以不能让调用方填。
            if (params_.crop_center) {
                const uint32_t fw = fmt.fmt.pix_mp.width;
                const uint32_t fh = fmt.fmt.pix_mp.height;
                sel.r.left = fw > params_.crop_width
                                 ? static_cast<__s32>((fw - params_.crop_width) / 2) : 0;
                sel.r.top = fh > params_.crop_height
                                 ? static_cast<__s32>((fh - params_.crop_height) / 2) : 0;
            }
            if (ioctl_call(fd_, VIDIOC_S_SELECTION, &sel) == 0) {
                format_.selection_supported = true;
                format_.selection_applied = true;
                TTBOX_LOG_INFO("V4L2 Selection/Crop 已应用: " +
                               std::to_string(sel.r.left) + "," + std::to_string(sel.r.top) + " " +
                               std::to_string(sel.r.width) + "x" + std::to_string(sel.r.height));
                if (ioctl_call(fd_, VIDIOC_G_FMT, &fmt) != 0) {
                    if (error) *error = "Selection 成功后 VIDIOC_G_FMT 失败";
                    close();
                    return false;
                }
            } else {
                TTBOX_LOG_WARN("V4L2 Selection/Crop 不可用，回退完整帧 + Preprocess/RGA: " +
                               std::string(std::strerror(errno)));
            }
        }

        format_.width = fmt.fmt.pix_mp.width;
        format_.height = fmt.fmt.pix_mp.height;
        format_.pixelformat = fmt.fmt.pix_mp.pixelformat;
        format_.num_planes = fmt.fmt.pix_mp.num_planes;
        format_.bytesperline.clear();
        format_.sizeimage.clear();
        for (uint32_t p = 0; p < format_.num_planes; ++p) {
            format_.bytesperline.push_back(fmt.fmt.pix_mp.plane_fmt[p].bytesperline);
            format_.sizeimage.push_back(fmt.fmt.pix_mp.plane_fmt[p].sizeimage);
        }
        if (format_.num_planes == 0 || format_.num_planes > 8) {
            if (error) *error = "G_FMT 返回非法 num_planes=" + std::to_string(format_.num_planes);
            close();
            return false;
        }
        {
            std::string log = "G_FMT: " + std::to_string(format_.width) + "x" +
                              std::to_string(format_.height) + " fourcc=" +
                              format_.fourcc_str() + " planes=" + std::to_string(format_.num_planes);
            for (uint32_t p = 0; p < format_.num_planes; ++p) {
                log += " [p" + std::to_string(p) + " bpl=" +
                       std::to_string(format_.bytesperline[p]) + " size=" +
                       std::to_string(format_.sizeimage[p]) + "]";
            }
            TTBOX_LOG_INFO(log);
        }

        // ---- 5. REQBUFS（MMAP，默认 8，驱动实际为准）----
        struct v4l2_requestbuffers req {};
        req.count = params_.num_buffers;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl_call(fd_, VIDIOC_REQBUFS, &req) != 0) {
            if (error) *error = "VIDIOC_REQBUFS 失败";
            close();
            return false;
        }
        buffer_count_ = req.count;
        if (buffer_count_ == 0) {
            if (error) *error = "REQBUFS 返回 0 个 buffer（驱动拒绝 MMAP）";
            close();
            return false;
        }
        if (buffer_count_ < params_.num_buffers) {
            TTBOX_LOG_WARN("REQBUFS: 请求 " + std::to_string(params_.num_buffers) +
                           " 个 buffer，驱动实际提供 " + std::to_string(buffer_count_) +
                           " 个，使用实际数量");
        } else {
            TTBOX_LOG_INFO("REQBUFS: " + std::to_string(buffer_count_) + " 个 buffer");
        }

        // ---- 6. 每 buffer 每 plane：QUERYBUF + mmap + EXPBUF ----
        impl_->buffers.resize(buffer_count_);
        impl_->captured.assign(buffer_count_, false);
        impl_->captured_count.store(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < buffer_count_; ++i) {
            struct v4l2_buffer buf {};
            struct v4l2_plane planes[8] {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            buf.length = format_.num_planes;
            buf.m.planes = planes;
            if (ioctl_call(fd_, VIDIOC_QUERYBUF, &buf) != 0) {
                if (error) *error = "VIDIOC_QUERYBUF[" + std::to_string(i) + "] 失败";
                close();
                return false;
            }

            BufferRes& bres = impl_->buffers[i];
            bres.index = static_cast<int>(i);
            bres.planes.resize(format_.num_planes);
            for (uint32_t p = 0; p < format_.num_planes; ++p) {
                PlaneRes& pres = bres.planes[p];
                pres.plane_index = p;
                pres.length = planes[p].length;
                pres.addr = ::mmap(nullptr, pres.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                                   static_cast<off_t>(planes[p].m.mem_offset));
                if (pres.addr == MAP_FAILED) {
                    pres.addr = nullptr;
                    pres.length = 0;
                    if (error) *error = "mmap[buf=" + std::to_string(i) + ",plane=" + std::to_string(p) + "] 失败";
                    metrics_.errors.fetch_add(1);
                    close();
                    return false;
                }

                // EXPBUF：导出 DMA-BUF fd（失败则该 plane 无 fd，不整体失败）
                struct v4l2_exportbuffer eb {};
                eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                eb.index = i;
                eb.plane = p;
                eb.flags = 0;
                if (ioctl_call(fd_, VIDIOC_EXPBUF, &eb) != 0) {
                    TTBOX_LOG_WARN("VIDIOC_EXPBUF[buf=" + std::to_string(i) + ",plane=" +
                                   std::to_string(p) + "] 失败: " + std::string(std::strerror(errno)));
                    metrics_.errors.fetch_add(1);
                } else {
                    pres.dma_fd = DmaBufFd(static_cast<int>(eb.fd), static_cast<uint32_t>(pres.length));
                }
                TTBOX_LOG_INFO("buffer[" + std::to_string(i) + "] plane[" + std::to_string(p) +
                               "] mmap=" + std::to_string(reinterpret_cast<uintptr_t>(pres.addr)) +
                               " len=" + std::to_string(pres.length) +
                               " dma_fd=" + std::to_string(pres.dma_fd.fd()));
            }
        }
        opened_ = true;
        TTBOX_LOG_INFO("V4L2Capture open 完成: " + std::to_string(buffer_count_) + " buffers");
        return true;
    }

    // 所有重试均失败
    if (error) *error = "V4L2 格式始终不稳定（" + std::to_string(kMaxRetries) + " 次重试后放弃）";
    return false;
}

// 启动采集：QBUF 全部 buffer → STREAMON → 复位指标 → 启动采集线程。
bool V4L2Capture::start(std::string* error) {
    if (!opened_) {
        if (error) *error = "设备未 open";
        return false;
    }
    if (running_.load()) {
        if (error) *error = "capture 已在运行";
        return false;
    }

    // ---- QBUF 全部 buffer ----
    for (uint32_t i = 0; i < buffer_count_; ++i) {
        struct v4l2_buffer buf {};
        struct v4l2_plane planes[8] {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = format_.num_planes;
        buf.m.planes = planes;
        for (uint32_t p = 0; p < format_.num_planes; ++p) {
            planes[p].length = static_cast<uint32_t>(impl_->buffers[i].planes[p].length);
        }
        if (ioctl_call(fd_, VIDIOC_QBUF, &buf) != 0) {
            if (error) *error = "VIDIOC_QBUF[" + std::to_string(i) + "] 失败";
            return false;
        }
    }
    TTBOX_LOG_INFO("QBUF 全部 " + std::to_string(buffer_count_) + " 个 buffer");

    // ---- STREAMON ----
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl_call(fd_, VIDIOC_STREAMON, &type) != 0) {
        if (error) *error = "VIDIOC_STREAMON 失败: " + std::string(std::strerror(errno));
        return false;
    }
    TTBOX_LOG_INFO("STREAMON OK");

    // 重置采集指标（G1 纪律：每次 start 都是新采集会话，FPS=本会话帧数/本会话秒数）。
    // 若不复位，capture_frames 是进程级累计值，restart 后 start_time 归零而帧数不归零，
    // 导致 capture_fps 虚高（曾实测 149 万 fps，实为累计帧数/秒的假象）。
    metrics_.capture_frames = 0;
    metrics_.dqbuf_frames = 0;
    metrics_.qbuf_frames = 0;
    metrics_.superseded_latest_frames = 0;
    metrics_.poll_timeouts = 0;
    metrics_.errors = 0;
    metrics_.capture_fps = 0.0;
    fps_window_frames_ = 0;
    fps_window_start_ms_ = 0.0;

    running_.store(true);
    capture_thread_ = std::thread(&V4L2Capture::capture_loop, this);
    // ★ 绑大核和上 RT 都在 capture_loop() 内部做，不在这里做：
    // sched_setaffinity / pthread_setschedparam 作用于**调用线程**，在 start()
    // 里调只会绑到启动采集的那个线程（main），采集线程自己一条都没生效。
    TTBOX_LOG_INFO("capture thread 已启动");
    return true;
}

// 停止采集：置退出标志 → join 线程 → STREAMOFF → 归还所有占用 buffer。
void V4L2Capture::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    // STREAMOFF（即使失败也继续清理）
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl_call(fd_, VIDIOC_STREAMOFF, &type) != 0) {
        TTBOX_LOG_WARN("STREAMOFF 失败: " + std::string(std::strerror(errno)));
    }
    // 归还所有仍被占用的 buffer（stop 语义：consumer 应已停止引用）
    for (uint32_t i = 0; i < buffer_count_; ++i) {
        if (impl_->captured[i]) {
            struct v4l2_buffer buf {};
            struct v4l2_plane planes[8] {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            buf.length = format_.num_planes;
            buf.m.planes = planes;
            ioctl_call(fd_, VIDIOC_QBUF, &buf);
            impl_->captured[i] = false;
            impl_->captured_count.fetch_sub(1, std::memory_order_relaxed);
            metrics_.qbuf_frames.fetch_add(1);
        }
    }
    impl_->pending.clear();
    latest_.clear();
    TTBOX_LOG_INFO("capture 已停止（STREAMOFF + 归还全部 buffer）");
}

// 关闭设备：stop → munmap 全部 plane → 关闭全部 dma-buf fd → 关闭设备 fd（幂等）。
void V4L2Capture::close() {
    // 本次 close 是否真的持有设备（决定要不要更新时间戳，见函数末尾注释）
    const bool had_device = (fd_ >= 0) || !impl_->buffers.empty() || opened_;
    stop();
    // ★ 2026-09-23 修：原来是 `if (!opened_) return;`，而 opened_ 只在 open() **末尾**
    //   才置真 ⇒ open() 中途失败（QUERYBUF / mmap / EXPBUF / REQBUFS，见 :335/:364/:382
    //   等处的 close() 调用）时这里直接早退，设备 fd、已 mmap 的 plane、已 EXPBUF 的
    //   dma-buf fd 一个都不释放。叠加 open() 的 5 次 hdmirx 重试 ⇒ 失败一次漏一套，
    //   CMA/ fd 耗尽后 V4L2 再也打不开设备。
    //   改成按**实际持有状态**逐项释放：没开过时 buffers 为空、fd_ < 0，逐项判断天然
    //   是空操作，因此幂等，可以无条件跑。
    // munmap + close dma fds（RAII：BufferRes 析构自动 close dma_fd）
    for (auto& bres : impl_->buffers) {
        for (auto& pres : bres.planes) {
            if (pres.addr != nullptr) {
                ::munmap(pres.addr, pres.length);
                pres.addr = nullptr;
                pres.length = 0;
            }
        }
    }
    impl_->buffers.clear();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    opened_ = false;
    // 记下关闭时刻：供下次 open() 判断是"热重开"（跳过 800ms 格式稳定等待）
    // 还是"冷打开"（照旧等满）。见头文件 last_close_ms_ 注释。
    // ★ 只在**真的持有过设备**时记录：open() 的重试循环里每轮开头都会调 close()，
    //   那时若还没成功打开过设备就更新时间戳，会把"冷启动"误判成"热重开" ⇒
    //   开机第一次反而跳过等待 ⇒ 可能拿到过渡中的格式。所以判据是 fd_ 曾 >= 0。
    if (had_device) {
        last_close_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    }
    TTBOX_LOG_INFO("V4L2Capture close 完成（munmap + dma fd 已关闭）");
}

// ===========================================================================
// capture loop
// ===========================================================================

// 采集线程主循环：绑核/RT → poll → DQBUF → 构造 FrameBuffer → 发布 latest → 归还。
void V4L2Capture::capture_loop() {
    using clock = std::chrono::steady_clock;

    // 采集线程调度策略：必须在**本线程内部**设置（在 start() 里设会作用于调用线程，
    // 见上方 start() 的注释）。顺序：先绑大核，再上 SCHED_FIFO。
    {
        std::string aerr;
        if (!CpuAffinity::set_thread_affinity(CpuAffinity::kBigCoreMask, &aerr)) {
            TTBOX_LOG_WARN("capture 线程绑定大核失败: " + aerr);
        } else {
            TTBOX_LOG_INFO("capture 线程已绑定大核 (cpu4-7)");
        }
    }
    {
        // 默认 60；上限 70（低于 usb-proxy 的 98，别反过来抢鼠标通路）；
        // 失败降级为普通调度，不让采集起不来。
        std::string detail;
        RtSched::apply_fifo("CAPTURE", 60, -1, &detail);
    }

    // 滚动 1s 窗口：有帧则 +1，无帧（poll 超时/EAGAIN）也照常推进窗口，
    // 保证停流 1s 后 capture_fps 归零，Web 立刻能看到 degraded 而不是旧均值。
    auto update_fps_window = [this](bool has_frame) {
        if (has_frame) ++fps_window_frames_;
        const double now_ms =
            std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
        if (fps_window_start_ms_ == 0.0) fps_window_start_ms_ = now_ms;
        const double span = now_ms - fps_window_start_ms_;
        if (span >= 1000.0) {
            metrics_.capture_fps.store(fps_window_frames_ * 1000.0 / span);
            fps_window_frames_ = 0;
            fps_window_start_ms_ = now_ms;
        }
    };

    while (running_.load()) {
        update_fps_window(false);

        // 1. 归还可归还的旧 buffer
        release_ready_buffers();

        // 2. poll。有待归还 buffer 时用短超时轮询（A-5 并发修复）：
        //    多消费者场景下全部 buffer 可能同时被 current_/worker 持有，
        //    capture 会阻塞在 poll；只有及时回来调用 release_ready_buffers()
        //    归还已过期的 buffer，才能避免 capture 饥饿（否则降到 ~1fps）。
        const int eff_timeout = impl_->pending.empty()
                                    ? params_.poll_timeout_ms
                                    : std::min<int>(params_.poll_timeout_ms, 50);
        struct pollfd pfd { fd_, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, eff_timeout);
        if (pr == 0) {
            metrics_.poll_timeouts.fetch_add(1);
            continue;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            metrics_.errors.fetch_add(1);
            TTBOX_LOG_ERROR("poll 失败: " + std::string(std::strerror(errno)));
            break;
        }

        // 3. DQBUF
        struct v4l2_buffer buf {};
        struct v4l2_plane planes[8] {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = format_.num_planes;
        buf.m.planes = planes;
        if (ioctl_call(fd_, VIDIOC_DQBUF, &buf) != 0) {
            if (errno == EAGAIN) continue;
            metrics_.errors.fetch_add(1);
            TTBOX_LOG_ERROR("DQBUF 失败: " + std::string(std::strerror(errno)));
            continue;
        }
        const int index = static_cast<int>(buf.index);
        if (index < 0 || index >= static_cast<int>(buffer_count_)) {
            metrics_.errors.fetch_add(1);
            continue;
        }
        metrics_.dqbuf_frames.fetch_add(1);
        impl_->captured[index] = true;
        impl_->captured_count.fetch_add(1, std::memory_order_relaxed);

        // 4. 构造 FrameBuffer（zero-copy：data 为空，metadata + dma_fd）
        auto frame = std::make_shared<FrameBuffer>();
        frame->info.width = format_.width;
        frame->info.height = format_.height;
        frame->info.stride = format_.bytesperline.empty() ? format_.width * 3 : format_.bytesperline[0];
        frame->info.format = map_pixel_format(format_.pixelformat);
        frame->info.sequence = buf.sequence;
        frame->info.timestamp_ms = tv_to_ms(buf.timestamp);
        frame->info.frame_number = static_cast<uint64_t>(buf.sequence);
        frame->info.timestamp_us = static_cast<uint64_t>(frame->info.timestamp_ms * 1000.0);
        frame->info.buffer_index = static_cast<uint32_t>(index);
        frame->info.num_planes = format_.num_planes;
        frame->info.dma_fd = impl_->buffers[index].planes.empty() ? -1
                               : impl_->buffers[index].planes[0].dma_fd.fd();
        frame->info.cpu_va = impl_->buffers[index].planes.empty() ? nullptr
                               : impl_->buffers[index].planes[0].addr;
        if (!format_.sizeimage.empty()) {
            frame->size = format_.sizeimage[0];
        }

        // 5. 发布到 LatestFrame（旧帧被覆盖 → 进入待归还）
        // 记录最新帧时间戳（v4l2 单调时钟，与 steady_clock 同基准）——
        // 供 buffer_age_ms（帧龄）计算：采集健康时 ≈ 0~7ms，停流/积压时持续增大
        metrics_.last_frame_ts_ms.store(static_cast<int64_t>(frame->info.timestamp_ms));
        auto old = latest_.publish(std::move(frame));
        if (old) {
            metrics_.superseded_latest_frames.fetch_add(1);
            impl_->pending.push_back(PendingRelease{
                index,
                plane_lengths_of(old->info.buffer_index),
                std::weak_ptr<FrameBuffer>(old),
            });
        }
        metrics_.capture_frames.fetch_add(1);
        update_fps_window(true);

        // 6. 尝试立即归还
        release_ready_buffers();
    }
}

std::vector<size_t> V4L2Capture::plane_lengths_of(uint32_t index) {
    // 返回 buffer[index] 各 plane length（供 QBUF 回填）
    std::vector<size_t> lens;
    if (index < impl_->buffers.size()) {
        for (const auto& p : impl_->buffers[index].planes) {
            lens.push_back(p.length);
        }
    }
    return lens;
}

// 归还所有 weak 已失效（无消费者引用）的待回收 buffer；仍被引用的跳过。
void V4L2Capture::release_ready_buffers() {
    for (auto it = impl_->pending.begin(); it != impl_->pending.end();) {
        if (it->weak.lock() != nullptr) {
            ++it;  // 仍有消费者/最新帧引用 → 不能归还
            continue;
        }
        // 无任何强引用 → 安全归还给驱动
        struct v4l2_buffer buf {};
        struct v4l2_plane planes[8] {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = it->index;
        buf.length = format_.num_planes;
        buf.m.planes = planes;
        for (size_t p = 0; p < it->plane_lengths.size() && p < format_.num_planes; ++p) {
            planes[p].length = static_cast<uint32_t>(it->plane_lengths[p]);
        }
        if (ioctl_call(fd_, VIDIOC_QBUF, &buf) == 0) {
            metrics_.qbuf_frames.fetch_add(1);
        } else {
            metrics_.errors.fetch_add(1);
            TTBOX_LOG_WARN("QBUF[" + std::to_string(it->index) + "] 失败: " +
                           std::string(std::strerror(errno)));
        }
        impl_->captured[it->index] = false;
        impl_->captured_count.fetch_sub(1, std::memory_order_relaxed);
        it = impl_->pending.erase(it);
    }
}

// ===========================================================================
// 查询
// ===========================================================================

uint32_t V4L2Capture::in_use_count() const {
    // ★ 2026-10-06 并发修复：直接读原子计数器，不再遍历 std::vector<bool>。
    //   原实现无锁遍历位压缩的 captured，与采集线程的位写入构成真正的数据竞争
    //   （相邻 bit 共享同一机器字），而不是"最多读到旧值"。语义完全一致：
    //   仍返回"当前已 DQBUF、未 QBUF 归还的 buffer 数"。
    return impl_->captured_count.load(std::memory_order_relaxed);
}

// 调试/验收：返回每个 buffer 主 plane 的 DMA-BUF fd 列表。
std::vector<int> V4L2Capture::dma_fds() const {
    std::vector<int> fds;
    for (const auto& bres : impl_->buffers) {
        if (!bres.planes.empty()) {
            fds.push_back(bres.planes[0].dma_fd.fd());
        }
    }
    return fds;
}

}  // namespace ttbox::core

#endif  // !_WIN32
