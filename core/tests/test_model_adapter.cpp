// test_model_adapter.cpp — A-7 硬件测试：三模型真实 metadata 验证（板端）
// 用法: test_model_adapter --model <rknn> --color bgr|rgb [--expect single|dfl|e2e] [--classes N]
#include <cstdio>
#include <cstring>
#include <string>
#include "model/ModelAdapter.hpp"
#include "rknn/RKNNEngine.hpp"

using namespace ttbox::core;

int main(int argc, char** argv) {
    std::string model;
    std::string color = "bgr";
    std::string expect;
    int expect_classes = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺少参数: %s\n", n); std::exit(1); }
            return argv[++i];
        };
        if (a == "--model") model = next("--model");
        else if (a == "--color") color = next("--color");
        else if (a == "--expect") expect = next("--expect");
        else if (a == "--classes") expect_classes = std::atoi(next("--classes").c_str());
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); return 1; }
    }
    if (model.empty()) { std::fprintf(stderr, "缺少 --model\n"); return 1; }

    int fail = 0;
    RKNNEngine engine;
    std::string err;
    RKNNEngine::Params ep;
    ep.model_path = model;
    ep.core_mask = 0;
    ep.pass_through = false;
    if (!engine.init(ep, &err)) {
        std::printf("[FAIL] RKNN init: %s\n", err.c_str());
        return 1;
    }

    ModelAdapter adapter;
    ModelAdapterConfig cfg;
    cfg.color_order = (color == "rgb") ? ColorOrder::kRgb : ColorOrder::kBgr;
    cfg.conf_thres = 0.55f;  // 用户配置
    cfg.iou_thres = 0.45f;
    if (!adapter.analyze(engine.info(), cfg, &err)) {
        std::printf("[FAIL] adapter.analyze: %s\n", err.c_str());
        return 1;
    }
    const ModelMetadata& m = adapter.metadata();
    std::printf("model=%s\n  input %ux%ux%u dtype=%d layout=%d size=%u color=%s quant=%d\n",
                model.c_str(), m.input_width, m.input_height, m.input_channels,
                m.input_dtype, m.input_layout, m.input_size,
                m.color_order == ColorOrder::kRgb ? "RGB" : "BGR",
                static_cast<int>(m.quantization_type));
    std::printf("  output=%u decode=%s classes=%u strides=",
                m.output_count, ModelAdapter::decode_type_name(m.decode_type), m.class_count);
    for (auto s : m.strides) std::printf("%u ", s);
    std::printf("\n  objectness=%d dfl=%d coord=%d nms=%d\n",
                m.objectness ? 1 : 0, m.dfl ? 1 : 0,
                static_cast<int>(m.coordinate_format), static_cast<int>(m.nms_type));
    std::printf("  effective conf=%.2f iou=%.2f (user config)\n",
                adapter.effective_conf(), adapter.effective_iou());

    // 断言
    if (!expect.empty()) {
        const std::string got = ModelAdapter::decode_type_name(m.decode_type);
        if (got != expect) {
            fail++; std::printf("[FAIL] decode_type 期望 %s 实际 %s\n", expect.c_str(), got.c_str());
        }
    }
    if (expect_classes >= 0 && m.class_count != static_cast<uint32_t>(expect_classes)) {
        fail++; std::printf("[FAIL] classes 期望 %d 实际 %u\n", expect_classes, m.class_count);
    }

    // decoder 创建（校验配置注入）
    auto decoder = adapter.create_decoder(&err);
    if (!decoder) {
        fail++; std::printf("[FAIL] create_decoder: %s\n", err.c_str());
    } else {
        std::printf("  decoder created OK\n");
    }

    engine.destroy();
    if (fail == 0) {
        std::printf("test_model_adapter PASS\n");
        return 0;
    }
    std::printf("test_model_adapter FAIL (%d)\n", fail);
    return 1;
}
