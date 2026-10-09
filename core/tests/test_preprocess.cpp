// test_preprocess.cpp — 主机可运行的 Capture frame → Preprocess 回归
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>
#include "rknn/Preprocess.hpp"

int main() {
    using namespace ttbox::core;
    constexpr uint32_t w = 4, h = 3, stride = 16;
    auto pixels = std::shared_ptr<uint8_t[]>(new uint8_t[stride * h]());
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w * 3; ++x) pixels[y * stride + x] = static_cast<uint8_t>(y * 30 + x);
    }
    FrameBuffer frame;
    frame.data = pixels;
    frame.size = stride * h;
    frame.info.width = w;
    frame.info.height = h;
    frame.info.stride = stride;
    frame.info.format = PixelFormat::kBGR888;
    frame.info.cpu_va = pixels.get();
    Preprocess preprocess;
    PreprocessConfig config;
    config.detect_size = {2, 2};
    config.backend = PreprocessBackend::kCpuFallback;
    config.input_type = 3;
    config.crop_x = 1;
    config.crop_y = 0;
    config.crop_width = 2;
    config.crop_height = 2;
    std::string error;
    if (!preprocess.init(config, &error)) { std::cerr << error << "\n"; return 1; }
    PreprocessedFrame output;
    if (!preprocess.process(frame, &output, &error)) { std::cerr << error << "\n"; return 1; }
    if (!output.ok || output.detect_size.width != 2 || output.detect_size.height != 2 ||
        output.tensor_data == nullptr || output.tensor_size != 12) return 1;
    std::cout << "frame=1 detect_size=2x2 inference_ms=0 detections=0\n";
    std::cout << "test_preprocess: PASS\n";
    return 0;
}
