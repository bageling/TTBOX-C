// test_rga.cpp — RgaProcessor 纯逻辑测试（错误路径；硬件链路见 test_rga_hw）
#include <string>

#include "rga/RgaProcessor.hpp"
#include "test_util.hpp"

using namespace ttbox::core;

TEST(rga_zero_output_size_rejected) {
    RgaProcessor rga;
    std::string err;
    // 输出尺寸必须来自模型/config；0 必须明确报错（禁止硬编码/静默兜底）
    bool ok = rga.init({0, 640}, &err);
    CHECK(!ok);
    CHECK(!err.empty());
    ok = rga.init({640, 0}, &err);
    CHECK(!ok);
    ok = rga.init({0, 0}, &err);
    CHECK(!ok);
    CHECK(!rga.initialized());
}

TEST(rga_uninitialized_process_rejected) {
    RgaProcessor rga;
    std::string err;
    FrameBuffer input;
    input.info.width = 1920;
    input.info.height = 1080;
    input.info.dma_fd = -1;
    RgaOutput out;
    bool ok = rga.process(input, &out, &err);
    CHECK(!ok);  // 未初始化必须拒绝
    CHECK(!err.empty());
}

TEST(rga_process_without_dma_fd_rejected) {
    RgaProcessor rga;
    std::string err;
    // 合法参数 + 已初始化时，无 dma_fd 输入必须明确报错（禁止 CPU 拷贝路径）
    bool inited = rga.init({640, 640}, &err);
    if (!inited) {
        // 环境条件不满足（本机无 RGA 硬件）—— 非"必需 fixture"，属**可选**跳过：计入 skipped、不失败。
        TEST_SKIP(std::string("本环境无 RGA 硬件，跳过硬件相关断言: ") + err);
    }
    FrameBuffer input;
    input.info.width = 1920;
    input.info.height = 1080;
    input.info.dma_fd = -1;  // 无 dma_fd
    RgaOutput out;
    bool ok = rga.process(input, &out, &err);
    CHECK(!ok);
    CHECK(err.find("dma_fd") != std::string::npos);
    rga.destroy();
}
