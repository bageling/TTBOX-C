// RgaProcessor.cpp — RGA 硬件缩放实现（librga/im2d，RK3588）
#include "rga/RgaProcessor.hpp"

#if defined(_WIN32)
// Windows 占位：无 RGA 硬件，CMake 仅在 Unix 编译本文件。
namespace ttbox::core {
}
#else

#include <cerrno>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <im2d.hpp>

#include "capture/DmaBuf.hpp"
#include "common/Logger.hpp"

namespace ttbox::core {

namespace {

using clock = std::chrono::steady_clock;

// 16 像素对齐（BGR888 stride 硬件要求，A-2 probe 已验证）
uint32_t align16(uint32_t v) {
    return (v + 15) & ~15u;
}

// IM_STATUS 枚举 → 可读字符串（日志/错误信息用）。
const char* im_status_str(IM_STATUS st) {
    switch (st) {
        case IM_STATUS_NOERROR: return "NOERROR";
        case IM_STATUS_SUCCESS: return "SUCCESS";
        case IM_STATUS_NOT_SUPPORTED: return "NOT_SUPPORTED";
        case IM_STATUS_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case IM_STATUS_INVALID_PARAM: return "INVALID_PARAM";
        case IM_STATUS_ILLEGAL_PARAM: return "ILLEGAL_PARAM";
        case IM_STATUS_ERROR_VERSION: return "ERROR_VERSION";
        case IM_STATUS_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

// 从 dma_heap 分配一块连续内存，返回 fd（RAII 接管）与 mmap 虚拟地址
bool alloc_dma_heap(size_t len, DmaBufFd* fd_out, void** va_out, std::string* error) {
    int hfd = ::open("/dev/dma_heap/cma", O_RDWR);
    if (hfd < 0) {
        if (error) *error = "open /dev/dma_heap/cma 失败: " + std::string(std::strerror(errno));
        return false;
    }
    struct dma_heap_allocation_data d {};
    d.len = len;
    d.fd_flags = O_RDWR | O_CLOEXEC;
    if (::ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
        if (error) *error = "DMA_HEAP_IOCTL_ALLOC 失败: " + std::string(std::strerror(errno));
        ::close(hfd);
        return false;
    }
    ::close(hfd);
    int fd = static_cast<int>(d.fd);
    void* va = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (va == MAP_FAILED) {
        if (error) *error = "mmap dma-heap 失败: " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }
    *fd_out = DmaBufFd(fd, static_cast<uint32_t>(len));
    *va_out = va;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl：常驻输出 buffer + 懒分配中间（crop）buffer
// ---------------------------------------------------------------------------

struct RgaProcessor::Impl {
    // 输出 buffer（模型输入尺寸，常驻复用）
    DmaBufFd out_fd;
    void* out_va = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    uint32_t out_stride_px = 0;   // 物理 wstride（16 对齐）
    rga_buffer_handle_t out_handle = 0;
    // V4L2 DMA-BUF fd 在一次采集会话内稳定；缓存 import handle，避免每帧
    // importbuffer_fd/releasebuffer_handle 的用户态和驱动开销。
    std::unordered_map<int, rga_buffer_handle_t> input_handles;

    // 中间 crop buffer（宽高随输入/ROI 变化，懒分配/重建）
    DmaBufFd mid_fd;
    void* mid_va = 0;
    uint32_t mid_w = 0;           // 当前 mid 逻辑宽
    uint32_t mid_h = 0;           // 当前 mid 逻辑高
    uint32_t mid_stride_px = 0;   // 物理 wstride（16 对齐）
    rga_buffer_handle_t mid_handle = 0;
};

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

// 构造：仅创建内部 Impl。
RgaProcessor::RgaProcessor() : impl_(std::make_unique<Impl>()) {}

// 析构：destroy() 释放全部硬件资源。
RgaProcessor::~RgaProcessor() {
    destroy();
}

// 初始化：校验参数 + 分配/导入常驻输出 buffer（dma_heap + importbuffer_fd）。
bool RgaProcessor::init(const Params& params, std::string* error) {
    if (inited_) {
        if (error) *error = "RgaProcessor 已初始化";
        return false;
    }
    if (params.output_width == 0 || params.output_height == 0) {
        if (error) *error = "输出尺寸必须 > 0（请从模型/config 读取，禁止硬编码）";
        return false;
    }
    if (params.output_width > 8192 || params.output_height > 8192) {
        if (error) *error = "输出尺寸超出合理范围";
        return false;
    }

    params_ = params;
    Impl& i = *impl_;
    i.out_w = params.output_width;
    i.out_h = params.output_height;
    i.out_stride_px = align16(i.out_w);
    const size_t out_len = static_cast<size_t>(i.out_stride_px) * i.out_h * 3;  // BGR888

    // 输出 buffer：dma_heap 分配 + mmap + import（常驻复用）
    if (!alloc_dma_heap(out_len, &i.out_fd, &i.out_va, error)) {
        return false;
    }
    i.out_handle = importbuffer_fd(i.out_fd.fd(), static_cast<int>(i.out_w),
                                   static_cast<int>(i.out_h),
                                   params_.out_color ? RK_FORMAT_RGB_888 : RK_FORMAT_BGR_888);
    if (i.out_handle == 0) {
        if (error) *error = "importbuffer_fd(输出) 失败";
        destroy();
        return false;
    }
    TTBOX_LOG_INFO("RgaProcessor init: out " + std::to_string(i.out_w) + "x" +
                   std::to_string(i.out_h) + " stride_px=" + std::to_string(i.out_stride_px) +
                   " dma_fd=" + std::to_string(i.out_fd.fd()) +
                   " center_crop=" + std::string(params.center_crop ? "on" : "off") +
                   " color=" + std::string(params_.out_color ? "RGB888" : "BGR888"));
    inited_ = true;
    return true;
}

// 释放全部 RGA handle / dma-buf fd / mmap（幂等，可重复调用）。
void RgaProcessor::destroy() {
    Impl& i = *impl_;
    if (!inited_ && !impl_->out_handle && !impl_->mid_handle &&
        !impl_->out_va && !impl_->mid_va && !impl_->out_fd.valid() &&
        !impl_->mid_fd.valid()) {
        return;  // 幂等：无资源
    }
    for (const auto& entry : i.input_handles) {
        if (entry.second != 0) releasebuffer_handle(entry.second);
    }
    i.input_handles.clear();
    if (i.mid_handle != 0) {
        releasebuffer_handle(i.mid_handle);
        i.mid_handle = 0;
    }
    if (i.mid_va != 0 && i.mid_w > 0 && i.mid_h > 0) {
        ::munmap(i.mid_va, static_cast<size_t>(i.mid_stride_px) * i.mid_h * 3);
        i.mid_va = 0;
    }
    i.mid_fd = DmaBufFd();  // 关闭 fd
    i.mid_w = i.mid_h = 0;

    if (i.out_handle != 0) {
        releasebuffer_handle(i.out_handle);
        i.out_handle = 0;
    }
    if (i.out_va != 0 && i.out_h > 0) {
        ::munmap(i.out_va, static_cast<size_t>(i.out_stride_px) * i.out_h * 3);
        i.out_va = 0;
    }
    i.out_fd = DmaBufFd();  // 关闭 fd
    i.out_w = i.out_h = i.out_stride_px = 0;
    inited_ = false;
}

// ---------------------------------------------------------------------------
// process
// ---------------------------------------------------------------------------

// 处理一帧：import 输入 fd → (crop+resize 或直接拉伸) → 写入常驻输出 buffer。
// 返回 true 时 output 指向内部复用 buffer（下次 process 覆盖）。
bool RgaProcessor::process(const FrameBuffer& input, RgaOutput* output,
                           std::string* error) {
    if (!inited_) {
        if (error) *error = "RgaProcessor 未初始化";
        return false;
    }
    if (output == 0) {
        if (error) *error = "output 为空";
        return false;
    }
    metrics_.frames.fetch_add(1);
    Impl& i = *impl_;
    const int out_fmt = params_.out_color ? RK_FORMAT_RGB_888 : RK_FORMAT_BGR_888;

    const uint32_t w = input.info.width;
    const uint32_t h = input.info.height;
    if (w == 0 || h == 0) {
        if (error) *error = "输入尺寸非法";
        metrics_.error_frames.fetch_add(1);
        return false;
    }
    if (input.info.dma_fd < 0) {
        if (error) *error = "输入无 DMA-BUF fd（本阶段要求 dma_fd 路径，禁止 CPU 拷贝输入）";
        metrics_.error_frames.fetch_add(1);
        return false;
    }

    // 输入 wstride（像素）：V4L2 bytesperline / 3；缺失时用 width
    const uint32_t stride_bytes = input.info.stride;
    const uint32_t in_wstride_px =
        (stride_bytes >= w * 3) ? (stride_bytes / 3) : w;

    // ---- 1. import 输入 fd（按稳定 DMA-BUF fd 缓存 handle）----
    const auto t0 = clock::now();
    rga_buffer_handle_t in_handle = 0;
    const auto cached = i.input_handles.find(input.info.dma_fd);
    if (cached != i.input_handles.end()) {
        in_handle = cached->second;
    } else {
        in_handle = importbuffer_fd(input.info.dma_fd, static_cast<int>(w), static_cast<int>(h),
                                    RK_FORMAT_BGR_888);
        if (in_handle != 0) i.input_handles.emplace(input.info.dma_fd, in_handle);
    }
    if (in_handle == 0) {
        if (error) *error = "importbuffer_fd(输入 fd=" +
                            std::to_string(input.info.dma_fd) + ") 失败";
        metrics_.error_frames.fetch_add(1);
        return false;
    }
    const auto t_import = clock::now();
    const uint32_t import_us =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  t_import - t0).count());

    // 输入 handle 归 RgaProcessor 缓存管理；错误路径也不能释放缓存句柄。
    auto release_input = []() {};

    rga_buffer_t src = wrapbuffer_handle(in_handle, static_cast<int>(w),
                                         static_cast<int>(h), RK_FORMAT_BGR_888,
                                         static_cast<int>(in_wstride_px),
                                         static_cast<int>(h));

    uint32_t crop_us = 0, resize_us = 0, single_us = 0;
    bool single_used = false;
    const bool roi_enabled = (params_.roi_w > 0 && params_.roi_h > 0);
    // ROI 显式启用时强制走 crop+resize（如预览跟随截取区域；center_crop=false 全画面拉伸仅对无 ROI 场景生效）
    if (params_.center_crop || roi_enabled) {
        // ---- 2. crop：ROI（A-8）或 center crop（原语义）→ resize ----
        // 目标裁剪区域（在输入全帧坐标系）
        uint32_t cw = 0, ch = 0;
        im_rect rect = {};
        if (roi_enabled) {
            cw = params_.roi_w;
            ch = params_.roi_h;
            rect = {static_cast<int>(params_.roi_x), static_cast<int>(params_.roi_y),
                    static_cast<int>(params_.roi_w), static_cast<int>(params_.roi_h)};
        } else {
            cw = ch = (w < h) ? w : h;
            rect = {static_cast<int>((w - cw) / 2), static_cast<int>((h - ch) / 2),
                    static_cast<int>(cw), static_cast<int>(ch)};
        }
        // ---- 2.5 单段 improcess：crop+resize 一次完成（失败自动回退两段）----
        if (params_.single_pass) {
            rga_buffer_t dst = wrapbuffer_handle(i.out_handle, static_cast<int>(i.out_w),
                                                 static_cast<int>(i.out_h), out_fmt,
                                                 static_cast<int>(i.out_stride_px),
                                                 static_cast<int>(i.out_h));
            im_rect dst_rect = {0, 0, static_cast<int>(i.out_w), static_cast<int>(i.out_h)};
            const auto t1 = clock::now();
            const IM_STATUS st = improcess(src, dst, rga_buffer_t{}, rect, dst_rect,
                                           im_rect{}, IM_SYNC);
            const auto t_single = clock::now();
            single_us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  t_single - t1).count());
            if (st == IM_STATUS_SUCCESS) {
                single_used = true;
            } else {
                metrics_.fallback_count.fetch_add(1);
                if (metrics_.fallback_count.load() <= 3) {
                    TTBOX_LOG_WARN("improcess 单段 crop+resize 失败(" +
                                   std::string(im_status_str(st)) + ")，回退 imcrop+imresize");
                }
            }
        }
        if (!single_used) {
        const uint32_t cw_align = align16(cw);
        if (i.mid_w != cw || i.mid_h != ch) {
            // 中间 buffer 懒分配/重建（仅输入/ROI 变化时）
            if (i.mid_handle != 0) {
                releasebuffer_handle(i.mid_handle);
                i.mid_handle = 0;
            }
            if (i.mid_va != 0) {
                ::munmap(i.mid_va, static_cast<size_t>(i.mid_stride_px) * i.mid_h * 3);
                i.mid_va = 0;
            }
            i.mid_fd = DmaBufFd();
            std::string alloc_err;
            const size_t mid_len = static_cast<size_t>(cw_align) * ch * 3;
            if (!alloc_dma_heap(mid_len, &i.mid_fd, &i.mid_va, &alloc_err)) {
                release_input();
                if (error) *error = "分配中间 buffer 失败: " + alloc_err;
                metrics_.error_frames.fetch_add(1);
                return false;
            }
            i.mid_handle = importbuffer_fd(i.mid_fd.fd(), static_cast<int>(cw),
                                           static_cast<int>(ch), RK_FORMAT_BGR_888);
            if (i.mid_handle == 0) {
                release_input();
                if (error) *error = "importbuffer_fd(中间) 失败";
                metrics_.error_frames.fetch_add(1);
                return false;
            }
            i.mid_w = cw;
            i.mid_h = ch;
            i.mid_stride_px = cw_align;
        }
        rga_buffer_t mid = wrapbuffer_handle(i.mid_handle, static_cast<int>(cw),
                                             static_cast<int>(ch), RK_FORMAT_BGR_888,
                                             static_cast<int>(i.mid_stride_px),
                                             static_cast<int>(ch));
        const auto t1 = clock::now();
        const IM_STATUS st1 = imcrop(src, mid, rect);
        const auto t_crop = clock::now();
        crop_us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            t_crop - t1).count());
        if (st1 != IM_STATUS_SUCCESS) {
            release_input();
            if (error) *error = "imcrop 失败: " + std::string(im_status_str(st1));
            metrics_.error_frames.fetch_add(1);
            return false;
        }

        // ---- 3. resize：crop → 模型输入尺寸 ----
        rga_buffer_t dst = wrapbuffer_handle(i.out_handle, static_cast<int>(i.out_w),
                                             static_cast<int>(i.out_h), out_fmt,
                                             static_cast<int>(i.out_stride_px),
                                             static_cast<int>(i.out_h));
        const auto t2 = clock::now();
        const IM_STATUS st2 = imresize(mid, dst);
        const auto t_resize = clock::now();
        resize_us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              t_resize - t2).count());
        if (st2 != IM_STATUS_SUCCESS) {
            release_input();
            if (error) *error = "imresize 失败: " + std::string(im_status_str(st2));
            metrics_.error_frames.fetch_add(1);
            return false;
        }
        }
    } else {
        // ---- 直接拉伸（保持当前 resize 语义，不做 crop）----
        rga_buffer_t dst = wrapbuffer_handle(i.out_handle, static_cast<int>(i.out_w),
                                             static_cast<int>(i.out_h), out_fmt,
                                             static_cast<int>(i.out_stride_px),
                                             static_cast<int>(i.out_h));
        const auto t2 = clock::now();
        const IM_STATUS st = imresize(src, dst);
        const auto t_resize = clock::now();
        resize_us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              t_resize - t2).count());
        if (st != IM_STATUS_SUCCESS) {
            release_input();
            if (error) *error = "imresize(直接拉伸) 失败: " + std::string(im_status_str(st));
            metrics_.error_frames.fetch_add(1);
            return false;
        }
    }
    release_input();  // 输入 handle 由 destroy() 统一释放

    const auto t_end = clock::now();
    const uint32_t total_us =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  t_end - t0).count());

    // ---- 统计 ----
    metrics_.ok_frames.fetch_add(1);
    metrics_.import_sum_us.fetch_add(import_us);
    metrics_.total_sum_us.fetch_add(total_us);
    metrics_.last_import_us.store(import_us);
    metrics_.last_total_us.store(total_us);
    if (single_used) {
        metrics_.single_ok.fetch_add(1);
        metrics_.single_sum_us.fetch_add(single_us);
        metrics_.resize_sum_us.fetch_add(single_us);  // resize_ms 口径 = 预处理 RGA 阶段
        metrics_.last_resize_us.store(single_us);
    } else {
        metrics_.crop_sum_us.fetch_add(crop_us);
        metrics_.resize_sum_us.fetch_add(resize_us);
        metrics_.last_crop_us.store(crop_us);
        metrics_.last_resize_us.store(resize_us);
    }

    // ---- 输出 ----
    output->ok = true;
    output->dma_fd = i.out_fd.fd();
    output->vir_addr = i.out_va;
    output->width = i.out_w;
    output->height = i.out_h;
    output->stride = i.out_stride_px * 3;  // 物理行字节数
    return true;
}

}  // namespace ttbox::core

#endif  // !_WIN32
