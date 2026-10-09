// HidTypes.hpp — A9 HID 透传基础类型
//
// 原则：
//   - 透传链路独立于 AI 推理（禁止"鼠标→AI→再生成鼠标"）
//   - 所有 HID Report 带 monotonic timestamp（与 AI 帧同基准，供 E2E latency 分析）
//   - RAW 优先（保持原始 report）；EVDEV 路径用于兼容/调试
//   - 坐标系严格区分：物理鼠标 / 屏幕 / Capture ROI / 模型输入 / Detection
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ttbox::core {

// ---------------------------------------------------------------------------
// HID 报告类型
// ---------------------------------------------------------------------------
enum class HidKind : uint8_t { kUnknown = 0, kKeyboard = 1, kMouse = 2, kConsumer = 3 };

// 单个 HID 原始报告（固定容量避免每事件堆分配；RAW 透传单元）
struct HidReport {
    uint64_t timestamp_us = 0;   // monotonic 时钟（us），RX 接收时刻
    uint32_t seq = 0;            // 单调序号
    uint8_t size = 0;            // 有效字节数
    uint8_t kind = 0;            // HidKind
    std::array<uint8_t, 16> data{0};  // 原始 report（键盘 8B / 鼠标 ≤8B）

    // 按下标读写原始字节（调试/解析便捷访问）
    uint8_t& operator[](size_t i) { return data[i]; }
    const uint8_t& operator[](size_t i) const { return data[i]; }
};

// 解析后的鼠标状态（MouseCoordinateProcessor 输出；相对移动）
struct MouseState {
    int8_t dx = 0;          // 相对 X 移动（HID 原始值）
    int8_t dy = 0;          // 相对 Y 移动
    uint8_t buttons = 0;    // bit0=左 1=右 2=中 3=侧1 4=侧2
    int8_t wheel = 0;       // 垂直滚轮（有符号）
    int8_t hwheel = 0;      // 水平滚轮（有符号，如存在）
    uint64_t timestamp_us = 0;
};

// 解析后的键盘状态（boot keyboard report：modifier + reserved + 6 keys）
struct KeyboardState {
    uint8_t modifiers = 0;  // bit0..7: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui
    std::array<uint8_t, 6> keys{0};  // 最多 6 个同时按键（usage id）
    uint64_t timestamp_us = 0;
};

// ---------------------------------------------------------------------------
// 坐标系（严格分离，禁止混用）
// ---------------------------------------------------------------------------
enum class CoordSpace : uint8_t {
    kPhysicalMouse = 0,  // 物理鼠标相对位移（dx/dy，无绝对位置）
    kScreen = 1,         // 屏幕坐标（像素，全帧 1920×1080）
    kCaptureRoi = 2,     // Capture ROI 坐标（相对 ROI 左上角）
    kModelInput = 3,     // 模型输入坐标（640×640 / 320×320）
    kDetection = 4,      // 检测结果坐标（原图像素，DecodeNMS 输出）
};

// 坐标转换器（A9 只定义接口/变换，不实现自动控制逻辑）
struct CoordinateTransform {
    // 屏幕 → Capture ROI（平移+裁剪）
    static bool screen_to_roi(float sx, float sy,
                              uint32_t roi_x, uint32_t roi_y,
                              uint32_t roi_w, uint32_t roi_h,
                              float* ox, float* oy);
    // Capture ROI → 模型输入（缩放）
    static bool roi_to_model(float rx, float ry,
                             uint32_t roi_w, uint32_t roi_h,
                             uint32_t model_w, uint32_t model_h,
                             float* ox, float* oy);
    // 模型输入 → 检测结果（缩放+ROI 偏移，与 DecodeNMS::map_coords 一致）
    static bool model_to_detection(float mx, float my,
                                   uint32_t model_w, uint32_t model_h,
                                   uint32_t roi_x, uint32_t roi_y,
                                   uint32_t roi_w, uint32_t roi_h,
                                   float* ox, float* oy);
    // 屏幕 → 检测（级联）
    static bool screen_to_detection(float sx, float sy,
                                    uint32_t roi_x, uint32_t roi_y,
                                    uint32_t roi_w, uint32_t roi_h,
                                    uint32_t model_w, uint32_t model_h,
                                    float* ox, float* oy);
};

// ---------------------------------------------------------------------------
// AI 检测结果接口（A9 只建接口，不写业务逻辑）
// ---------------------------------------------------------------------------
// AI 检测结果（单帧内的一个检测框）
struct DetectionResult {
    int class_id = 0;
    float score = 0.0f;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // 原图（Detection 坐标系）
    // 时间戳（与 HID 同单调时钟，供 E2E latency）
    uint64_t timestamp_us = 0;
    uint64_t frame_id = 0;
};

}  // namespace ttbox::core
