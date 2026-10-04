// PreviewModule.cpp — Capture 中心可调尺寸预览实现
#include "preview/PreviewModule.hpp"

#if defined(_WIN32)
namespace ttbox::core {
}
#else

#include <algorithm>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstring>

#include <jpeglib.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "common/Logger.hpp"
#include "common/CpuAffinity.hpp"
#include "common/FrameRateMeter.hpp"

namespace ttbox::core {

namespace {

using clock = std::chrono::steady_clock;

struct JpegErrorManager {
    jpeg_error_mgr base;
    jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr info) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(info->err);
    longjmp(manager->jump, 1);
}

bool encode_bgr_jpeg(const uint8_t* bgr, uint32_t width, uint32_t height,
                     uint32_t stride, int quality, std::vector<uint8_t>* output,
                     std::string* error) {
    if (bgr == nullptr || output == nullptr || width == 0 || height == 0 ||
        stride < width * 3) {
        if (error) *error = "Preview JPEG 输入无效";
        return false;
    }

    jpeg_compress_struct compressor{};
    JpegErrorManager manager{};
    compressor.err = jpeg_std_error(&manager.base);
    manager.base.error_exit = jpeg_error_exit;
    if (setjmp(manager.jump)) {
        jpeg_destroy_compress(&compressor);
        if (error) *error = "Preview JPEG 编码失败";
        return false;
    }

    jpeg_create_compress(&compressor);
    unsigned char* encoded = nullptr;
    unsigned long encoded_size = 0;
    jpeg_mem_dest(&compressor, &encoded, &encoded_size);
    compressor.image_width = width;
    compressor.image_height = height;
    compressor.input_components = 3;
    compressor.in_color_space = JCS_RGB;
    jpeg_set_defaults(&compressor);
    jpeg_set_quality(&compressor, std::clamp(quality, 1, 100), TRUE);
    jpeg_start_compress(&compressor, TRUE);

    // 预览编码只由单个线程调用，thread_local 让行缓冲跨帧复用，
    // 同时不扩大 PreviewModule 的锁范围。批量 scanline 一次写多行，
    // 减少 jpeg_write_scanlines 的调用次数（libjpeg-turbo 推荐 16 行/批）。
    thread_local std::vector<uint8_t> rgb_rows;
    const size_t row_bytes = static_cast<size_t>(width) * 3;
    constexpr size_t kRowsPerBatch = 16;
    if (rgb_rows.size() < kRowsPerBatch * row_bytes) {
        rgb_rows.resize(kRowsPerBatch * row_bytes);
    }
    JSAMPROW row_ptrs[kRowsPerBatch];
    while (compressor.next_scanline < compressor.image_height) {
        const size_t rows_here =
            std::min<size_t>(kRowsPerBatch,
                             compressor.image_height - compressor.next_scanline);
        for (size_t r = 0; r < rows_here; ++r) {
            const auto* source =
                bgr + static_cast<size_t>(compressor.next_scanline + r) * stride;
            uint8_t* rgb_row = rgb_rows.data() + r * row_bytes;
            for (uint32_t x = 0; x < width; ++x) {
                rgb_row[x * 3] = source[x * 3 + 2];
                rgb_row[x * 3 + 1] = source[x * 3 + 1];
                rgb_row[x * 3 + 2] = source[x * 3];
            }
        }
        for (size_t r = 0; r < rows_here; ++r) {
            row_ptrs[r] = rgb_rows.data() + r * row_bytes;
        }
        jpeg_write_scanlines(&compressor, row_ptrs,
                             static_cast<JDIMENSION>(rows_here));
    }
    jpeg_finish_compress(&compressor);
    jpeg_destroy_compress(&compressor);

    if (encoded == nullptr || encoded_size == 0) {
        if (error) *error = "Preview JPEG 输出为空";
        return false;
    }
    output->assign(encoded, encoded + encoded_size);
    std::free(encoded);
    return true;
}

// ★ M2.03：受限预览水印用的内嵌 5×7 位图字体（列主序；每列 1 字节，bit0 = 顶行）。
//   仅覆盖水印所需字符子集：空格 / '-' / '_' / 0-9 / A-Z（brand 已由 Application 归一化并大写）。
//   内嵌 = 不依赖任何外部字体文件（板端最小 rootfs 亦可用）。
const uint8_t kWatermarkFont[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x08, 0x08, 0x08, 0x08, 0x08},  // '-'
    {0x40, 0x40, 0x40, 0x40, 0x40},  // '_'
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46},  // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39},  // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03},  // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36},  // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // '9'
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31},  // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // 'V'
    {0x7F, 0x20, 0x18, 0x20, 0x7F},  // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63},  // 'X'
    {0x03, 0x04, 0x78, 0x04, 0x03},  // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43},  // 'Z'
};

// 字符 → 字体表索引（未知字符回落到空格 ' '）。
int watermark_glyph_index(char c) {
    if (c == ' ') return 0;
    if (c == '-') return 1;
    if (c == '_') return 2;
    if (c >= '0' && c <= '9') return 3 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 13 + (c - 'A');
    return 0;
}

}  // namespace

void PreviewModule::resolve_preview_geometry(uint32_t frame_w, uint32_t frame_h,
                                             PreviewRoi* roi, uint32_t* out_width,
                                             uint32_t* out_height) const {
    if (roi == nullptr || out_width == nullptr || out_height == nullptr) return;

    uint32_t cap_w = 0;
    uint32_t cap_h = 0;
    uint32_t max_w = params_.crop_width;
    uint32_t max_h = params_.crop_height;
    if (params_.runtime_config != nullptr) {
        if (auto profile = params_.runtime_config->snapshot()) {
            // 裁剪范围的真源：capture（面板上的「截取尺寸」）。
            // 2026-09-20 业主定案：主页「截取尺寸」改多少，低帧预览就裁多少——
            // 预览必须和 AI 推理看的是同一块。公式与 WorkerPool 逐字一致
            // （见 preview/PreviewRoi.hpp::compute_preview_roi）。
            cap_w = profile->capture.width;
            cap_h = profile->capture.height;
            // V1.0.13：capture.offset_x/y 已删 ⇒ ROI 恒以屏幕中心为心。
            // preview.width/height 降级为**输出上限**：ROI 超过它才等比缩小，
            // 不再决定"裁哪一块"（那由 capture 决定）。
            if (profile->preview.width > 0) max_w = profile->preview.width;
            if (profile->preview.height > 0) max_h = profile->preview.height;
        }
    }
    *roi = compute_preview_roi(frame_w, frame_h, cap_w, cap_h);
    fit_preview_output(roi->w, roi->h, max_w, max_h, out_width, out_height);
}

void PreviewModule::draw_aim_box(uint8_t* crop, uint32_t width, uint32_t height,
                                 uint32_t stride,
                                 const DetectionBox& box,
                                 uint32_t origin_x, uint32_t origin_y) const {
    if (crop == nullptr || width == 0 || height == 0 || stride < width * 3) return;

    const int x1 = std::clamp(static_cast<int>(box.x1) - static_cast<int>(origin_x),
                              0, static_cast<int>(width - 1));
    const int y1 = std::clamp(static_cast<int>(box.y1) - static_cast<int>(origin_y),
                              0, static_cast<int>(height - 1));
    const int x2 = std::clamp(static_cast<int>(box.x2) - static_cast<int>(origin_x),
                              0, static_cast<int>(width - 1));
    const int y2 = std::clamp(static_cast<int>(box.y2) - static_cast<int>(origin_y),
                              0, static_cast<int>(height - 1));
    if (x2 <= x1 || y2 <= y1) return;

    cv::Mat image(static_cast<int>(height), static_cast<int>(width), CV_8UC3,
                  crop, stride);
    // ★ V1.0.25：单框 = 醒目亮绿加粗，颜色**不再按 class_id 分派**。
    //   旧实现「class 1 加粗、标成头色」是出厂 2 类模型（EP：0=身体 1=头）的约定；
    //   板端现役模型 class_count=7 且 class_names 为空 ⇒ 那个映射无从谈起，
    //   继续按它上色等于在画面上编一个不存在的语义给用户看。
    const cv::Scalar color(0, 255, 0);
    cv::rectangle(image, cv::Point(x1, y1), cv::Point(x2, y2), color, 3, cv::LINE_8);

    // 角标：只标类别号（Status 里没有"选中目标"的置信度字段，标 score 只能填 0 误导人）。
    char label[32];
    std::snprintf(label, sizeof(label), "cls %d", box.class_id);
    int baseline = 0;
    const cv::Size text_size = cv::getTextSize(
        label, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &baseline);
    const int label_x = x1;
    const int label_y = std::max(text_size.height + 4, y1 - 2);
    cv::rectangle(image,
                  cv::Point(label_x, label_y - text_size.height - 2),
                  cv::Point(label_x + text_size.width + 4, label_y + baseline),
                  cv::Scalar(0, 0, 0), cv::FILLED, cv::LINE_8);
    cv::putText(image, label, cv::Point(label_x + 2, label_y - 2),
                cv::FONT_HERSHEY_SIMPLEX, 0.45, color, 1, cv::LINE_8);
}

// ★ M2.03：受限态水印（内嵌位图字体；纯像素写入，**不分配、不抛异常** ⇒ never block 帧输出）。
void PreviewModule::draw_watermark(uint8_t* crop, uint32_t width, uint32_t height,
                                   uint32_t stride) const {
    if (crop == nullptr || width == 0 || height == 0 || stride < width * 3) return;
    if (!params_.watermark || params_.watermark_text.empty()) return;

    const int scale = std::max(1, static_cast<int>(width) / 240);  // 640 → 2
    const int glyph_w = 5;
    const int glyph_h = 7;
    const int gap = 1;
    const int text_len = static_cast<int>(params_.watermark_text.size());
    const int adv = (glyph_w + gap) * scale;
    const int block_w = adv * text_len;
    const int block_h = (glyph_h + 2) * scale;

    // 右下角对齐（留边距）；窗口过小时回落到左上角附近，保证可见。
    int x0 = static_cast<int>(width) - block_w - 4 * scale;
    if (x0 < 2 * scale) x0 = 2 * scale;
    int y0 = static_cast<int>(height) - block_h - 4 * scale;
    if (y0 < 2 * scale) y0 = 2 * scale;

    const auto put = [&](int px, int py, uint8_t b, uint8_t g, uint8_t r) {
        if (px < 0 || py < 0 || px >= static_cast<int>(width) ||
            py >= static_cast<int>(height)) {
            return;
        }
        uint8_t* p = crop + static_cast<size_t>(py) * stride + static_cast<size_t>(px) * 3;
        p[0] = b;
        p[1] = g;
        p[2] = r;
    };
    const auto fill = [&](int px, int py, int w, int h, uint8_t b, uint8_t g, uint8_t r) {
        for (int yy = 0; yy < h; ++yy) {
            for (int xx = 0; xx < w; ++xx) put(px + xx, py + yy, b, g, r);
        }
    };

    // 深色底衬（提升任意背景下可读性）。
    fill(x0 - scale, y0 - scale, block_w + 2 * scale, block_h + 2 * scale, 0, 0, 0);

    int px = x0;
    for (int i = 0; i < text_len; ++i) {
        const uint8_t* glyph = kWatermarkFont[watermark_glyph_index(params_.watermark_text[i])];
        for (int col = 0; col < glyph_w; ++col) {
            const uint8_t bits = glyph[col];
            for (int row = 0; row < glyph_h; ++row) {
                if (bits & (1u << row)) {
                    fill(px + col * scale, y0 + row * scale, scale, scale, 255, 255, 255);
                }
            }
        }
        px += adv;
    }
}

bool PreviewModule::start(const LatestFrame* frame_source, const Params& params,
                          std::string* error) {
    if (frame_source == nullptr) {
        if (error) *error = "预览帧源为空";
        return false;
    }
    if (params.fps <= 0 || params.fps > 60 || params.jpeg_quality < 1 ||
        params.jpeg_quality > 100 || params.crop_width == 0 || params.crop_height == 0) {
        if (error) *error = "预览参数无效";
        return false;
    }
    if (running_.exchange(true)) return false;

    latest_ = frame_source;
    params_ = params;
    metrics_.width.store(params.crop_width);
    metrics_.height.store(params.crop_height);
    metrics_.bytes.store(0);
    metrics_.frames.store(0);
    metrics_.dropped.store(0);
    metrics_.fps.store(0.0);
    metrics_.encode_ms.store(0.0);
    {
        std::lock_guard<std::mutex> lock(jpeg_mutex_);
        jpeg_.clear();
    }
    thread_ = std::thread(&PreviewModule::loop, this);
    return true;
}

void PreviewModule::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    latest_ = nullptr;
    std::lock_guard<std::mutex> lock(jpeg_mutex_);
    jpeg_.clear();
    metrics_.bytes.store(0);
    metrics_.width.store(0);
    metrics_.height.store(0);
}

bool PreviewModule::snapshot(std::vector<uint8_t>* jpeg_out) const {
    if (jpeg_out == nullptr) return false;
    std::lock_guard<std::mutex> lock(jpeg_mutex_);
    if (jpeg_.empty()) return false;
    *jpeg_out = jpeg_;
    return true;
}

void PreviewModule::loop() {
    // JPEG 编码/裁剪是每帧必经的 CPU 工作，固定大核避免跨核抖动。
    {
        std::string aerr;
        if (!CpuAffinity::set_thread_affinity(CpuAffinity::kBigCoreMask, &aerr)) {
            TTBOX_LOG_WARN("Preview 线程绑定大核失败: " + aerr);
        }
    }
    const auto start_time = clock::now();
    // ★ 1000/fps 的整数毫秒取整会偏快（15 fps → 66 ms ⇒ 实际 15.15 fps，方向是超标）。
    //   改用微秒周期；并对 fps <= 0 兜底，避免除零或 0 周期忙等。（2026-09-23 审查复核 #35）
    const int preview_fps = params_.fps > 0 ? params_.fps : 15;
    const auto interval = std::chrono::microseconds(1000000 / preview_fps);
    auto next_tick = start_time;
    // ★ 瞬时帧率计（2026-09-23 审查复核 #12）：原来用「帧数 ÷ 启动至今秒数」——那是累计平均
    //   （与 FrameRateMeter 头注释里点名的反面教材同款），预览帧率会从低往高一直爬、断线也不回落。
    //   改成滚动窗口，与采集/推理侧口径一致。本对象只在预览线程内使用，无需再加锁。
    FrameRateMeter fps_meter;

    // ★ 连续异常计数 + 静默开关（2026-09-23）
    //   encode_frame() 内部会构造 cv::Mat、做 cv::resize、扩容 crop_buffer_/jpeg_out，
    //   这些**都会抛异常**（std::bad_alloc / cv::Exception）。异常一旦穿过线程函数就是
    //   std::terminate() —— 预览只是锦上添花，却能把采集 + NPU 推理 + HID 注入一起带走。
    //   所以这里必须兜住，且失败最多只意味着"这一帧不出图"。
    //   连续失败到阈值后转入静默：不再尝试编码（不再刷日志、不再抛），
    //   **但线程继续活着** —— 因为 stop() 靠 running_.exchange(false) 判断是否 join，
    //   若在循环里自己把 running_ 置 false，stop() 会直接早退、线程不被 join，
    //   析构时 joinable 的 std::thread 照样 terminate（与 HidForwarder 是同一类坑）。
    constexpr uint32_t kMaxEncodeFaults = 30;
    uint32_t consecutive_faults = 0;
    bool fault_quiet = false;

    while (running_.load()) {
        const auto now = clock::now();
        if (now < next_tick) std::this_thread::sleep_for(next_tick - now);
        next_tick = clock::now() + interval;

        if (fault_quiet) continue;

        auto frame = latest_ ? latest_->get() : nullptr;
        if (!frame || frame->size == 0 || frame->info.cpu_va == nullptr) {
            metrics_.dropped.fetch_add(1);
            continue;
        }

        const auto encode_start = clock::now();
        std::vector<uint8_t> jpeg;
        std::string error;
        try {
            if (!encode_frame(*frame, &jpeg, &error)) {
                metrics_.dropped.fetch_add(1);
                if (metrics_.dropped.load() <= 3) TTBOX_LOG_WARN("Preview 编码失败: " + error);
                continue;
            }
        } catch (const std::exception& e) {
            metrics_.dropped.fetch_add(1);
            ++consecutive_faults;
            if (consecutive_faults >= kMaxEncodeFaults) {
                fault_quiet = true;
                TTBOX_LOG_ERROR("Preview 编码连续 " + std::to_string(consecutive_faults) +
                                " 次抛异常（最后一条: " + e.what() +
                                "）⇒ 预览转入静默，AI 链路不受影响");
            } else if (consecutive_faults <= 3) {
                TTBOX_LOG_WARN(std::string("Preview 编码抛异常（已忽略，不中断预览线程）: ") + e.what());
            }
            continue;
        } catch (...) {
            metrics_.dropped.fetch_add(1);
            ++consecutive_faults;
            if (consecutive_faults >= kMaxEncodeFaults) {
                fault_quiet = true;
                TTBOX_LOG_ERROR("Preview 编码连续 " + std::to_string(consecutive_faults) +
                                " 次抛未知异常 ⇒ 预览转入静默，AI 链路不受影响");
            } else if (consecutive_faults <= 3) {
                TTBOX_LOG_WARN("Preview 编码抛未知异常（已忽略，不中断预览线程）");
            }
            continue;
        }
        consecutive_faults = 0;

        {
            std::lock_guard<std::mutex> lock(jpeg_mutex_);
            jpeg_ = std::move(jpeg);
            metrics_.bytes.store(static_cast<uint32_t>(jpeg_.size()));
        }
        metrics_.frames.fetch_add(1);
        metrics_.encode_ms.store(std::chrono::duration<double, std::milli>(
            clock::now() - encode_start).count());
        fps_meter.tick();
        double fps_now = fps_meter.fps();
        if (fps_now <= 0.0) {
            // 窗口不足 2 帧（刚启动）才回退累计平均，避免面板显示 0；稳态走滚动窗口值。
            const double elapsed =
                std::chrono::duration<double>(clock::now() - start_time).count();
            if (elapsed > 0.0) fps_now = static_cast<double>(metrics_.frames.load()) / elapsed;
        }
        metrics_.fps.store(fps_now);
    }
}

bool PreviewModule::encode_frame(const FrameBuffer& frame,
                                 std::vector<uint8_t>* jpeg_out,
                                 std::string* error) {
    const uint32_t frame_width = frame.info.width;
    const uint32_t frame_height = frame.info.height;
    const uint32_t frame_stride = frame.info.stride ? frame.info.stride : frame_width * 3;
    if (frame_width == 0 || frame_height == 0 || frame_stride < frame_width * 3 ||
        frame.info.cpu_va == nullptr) {
        if (error) *error = "Preview 输入帧不可用（尺寸/步长/CPU 映射）";
        return false;
    }

    PreviewRoi roi;
    uint32_t out_width = 0;
    uint32_t out_height = 0;
    resolve_preview_geometry(frame_width, frame_height, &roi, &out_width, &out_height);
    if (roi.w == 0 || roi.h == 0 || roi.x + roi.w > frame_width ||
        roi.y + roi.h > frame_height) {
        if (error) *error = "Preview 裁剪矩形超出 Capture";
        return false;
    }

    const uint32_t crop_stride = roi.w * 3;
    const size_t crop_bytes = static_cast<size_t>(crop_stride) * roi.h;
    if (crop_buffer_.size() < crop_bytes) crop_buffer_.resize(crop_bytes);
    uint8_t* crop = crop_buffer_.data();
    const auto* source = static_cast<const uint8_t*>(frame.info.cpu_va);
    for (uint32_t y = 0; y < roi.h; ++y) {
        const auto* source_row = source + static_cast<size_t>(roi.y + y) * frame_stride +
                                 static_cast<size_t>(roi.x) * 3;
        std::memcpy(crop + static_cast<size_t>(y) * crop_stride,
                    source_row, crop_stride);
    }

    // ★ V1.0.25：预览只画**实际瞄准的那一个框**（业主定口径：预览 = 瞄准框）。
    //   没有选中目标 ⇒ 不画（不再"丢失后保留 3 帧旧框"——那是画出已经不瞄的框）。
    //   也不再二次平滑：传进来的框已由 AimThread 的 One-Euro 平滑过。
    if (params_.draw_detections) {
        DetectionBox aim_box;
        bool has_aim = false;
        {
            std::lock_guard<std::mutex> lock(provider_mutex_);
            if (aim_box_provider_) has_aim = aim_box_provider_(&aim_box);
        }
        if (has_aim) {
            draw_aim_box(crop, roi.w, roi.h, crop_stride, aim_box, roi.x, roi.y);
        }
    }

    // ★ M2.03：受限态水印叠加。水印是**附加绘制**，任何情况下都不影响后续编码与帧输出
    //   （draw_watermark 为纯数组写入、无分配/无异常）。
    if (params_.watermark && !params_.watermark_text.empty()) {
        try {
            draw_watermark(crop, roi.w, roi.h, crop_stride);
        } catch (...) {
            // 水印失败绝不影响帧输出（never block）。
        }
    }

    // 输出：ROI 没超过上限就原尺寸直出（**不放大**——浏览器自己会把画面缩放到面板，
    // 放大只是白烧 CPU）；超过（如 0×0 回退的中心正方形 1440）才用 INTER_AREA 等比缩小。
    const uint8_t* out_data = crop;
    uint32_t out_stride = crop_stride;
    if (out_width != roi.w || out_height != roi.h) {
        cv::Mat src(static_cast<int>(roi.h), static_cast<int>(roi.w), CV_8UC3, crop,
                    crop_stride);
        cv::Mat dst(static_cast<int>(out_height), static_cast<int>(out_width), CV_8UC3);
        cv::resize(src, dst, dst.size(), 0, 0, cv::INTER_AREA);
        out_stride = out_width * 3;
        const size_t need = static_cast<size_t>(out_stride) * out_height;
        if (scaled_buffer_.size() < need) scaled_buffer_.resize(need);
        std::memcpy(scaled_buffer_.data(), dst.data, need);
        out_data = scaled_buffer_.data();
    }

    metrics_.width.store(out_width);
    metrics_.height.store(out_height);
    return encode_bgr_jpeg(out_data, out_width, out_height, out_stride,
                           params_.jpeg_quality, jpeg_out, error);
}

}  // namespace ttbox::core
#endif  // !_WIN32
