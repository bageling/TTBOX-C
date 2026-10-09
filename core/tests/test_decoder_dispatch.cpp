// test_decoder_dispatch.cpp — A-7 单元测试：decode_type 自动分发（禁止文件名判断）
#include "test_util.hpp"

#include <string>
#include <vector>

#include "model/ModelAdapter.hpp"

using namespace ttbox::core;

// 构造合成 RknnModelInfo（模拟 rknn_query 结果）
static RknnModelInfo make_info(uint32_t n_out,
                               std::vector<std::vector<uint32_t>> shapes,
                               uint32_t in_h = 640, int in_type = 1) {
    RknnModelInfo info;
    info.n_inputs = 1;
    info.input_dims = {1, in_h, in_h, 3};
    info.input_width = in_h;
    info.input_height = in_h;
    info.input_size = in_h * in_h * 3 * 2;
    info.input_type = in_type;
    info.input_fmt = 1;  // NHWC
    info.n_outputs = n_out;
    for (auto& s : shapes) {
        RknnOutputInfo oi;
        oi.dims = s;
        oi.type = in_type == 2 ? 2 : 1;
        oi.fmt = 0;
        info.outputs.push_back(oi);
    }
    return info;
}

// 1) yolo261n：单输出 (1,84,8400) → Single，80 类，xywh
TEST(decoder_dispatch_yolo261n_single) {
    ModelAdapterConfig cfg;
    auto info = make_info(1, {{1, 84, 8400}});
    ModelAdapter ad;
    std::string err;
    CHECK(ad.analyze(info, cfg, &err));
    if (err.empty()) {
        const auto& m = ad.metadata();
        CHECK(m.decode_type == DecodeType::kSingle);
        CHECK_EQ(m.class_count, 80u);
        CHECK(m.coordinate_format == CoordFormat::kXywh);
        CHECK(!m.dfl);
    }
}

// 2) v26m：单输出 [1,300,6] → E2E，xyxy
TEST(decoder_dispatch_v26m_e2e) {
    ModelAdapterConfig cfg;
    auto info = make_info(1, {{1, 300, 6}});
    ModelAdapter ad;
    std::string err;
    CHECK(ad.analyze(info, cfg, &err));
    if (err.empty()) {
        const auto& m = ad.metadata();
        CHECK(m.decode_type == DecodeType::kE2e);
        CHECK(m.coordinate_format == CoordFormat::kXyxy);
        CHECK(!m.dfl);
        CHECK(!m.objectness);
    }
}

// 3) 黄瓦：6 输出成对 box/cls → DFL，2 类，strides 推断
TEST(decoder_dispatch_huangwa_dfl) {
    ModelAdapterConfig cfg;
    auto info = make_info(6, {{1, 1, 4, 1600}, {1, 2, 40, 40},
                              {1, 1, 4, 400}, {1, 2, 20, 20},
                              {1, 1, 4, 100}, {1, 2, 10, 10}}, 320, 2);
    ModelAdapter ad;
    std::string err;
    CHECK(ad.analyze(info, cfg, &err));
    if (err.empty()) {
        const auto& m = ad.metadata();
        CHECK(m.decode_type == DecodeType::kDfl);
        CHECK_EQ(m.class_count, 2u);
        CHECK(m.dfl);
        CHECK(m.coordinate_format == CoordFormat::kLtrb);
        CHECK_EQ(m.strides.size(), 3u);
        if (m.strides.size() == 3) {
            CHECK_EQ(m.strides[0], 8u);
            CHECK_EQ(m.strides[1], 16u);
            CHECK_EQ(m.strides[2], 32u);
        }
    }
}

// 4) 带 objectness 单输出 (1,85,8400) → Single + objectness
TEST(decoder_dispatch_objectness) {
    ModelAdapterConfig cfg;
    auto info = make_info(1, {{1, 85, 8400}});
    ModelAdapter ad;
    std::string err;
    CHECK(ad.analyze(info, cfg, &err));
    if (err.empty()) {
        const auto& m = ad.metadata();
        CHECK(m.decode_type == DecodeType::kSingle);
        CHECK(m.objectness);
        CHECK_EQ(m.class_count, 80u);
    }
}

// 5) 不支持结构 → analyze 失败（正确报错）
TEST(decoder_dispatch_reject_unsupported) {
    ModelAdapterConfig cfg;
    auto info = make_info(1, {{1, 3, 640, 640}});  // 非解码输出
    ModelAdapter ad;
    std::string err;
    CHECK(!ad.analyze(info, cfg, &err));
    CHECK(!err.empty());
}

// 4b) 大腕256：9 输出 reg-bins DFL（reg/cls/aux ×3 尺度）→ DflDist，7 类
TEST(decoder_dispatch_dawant256_dfl_dist) {
    ModelAdapterConfig cfg;
    auto info = make_info(9, {{1, 64, 32, 32}, {1, 7, 32, 32}, {1, 1, 32, 32},
                              {1, 64, 16, 16}, {1, 7, 16, 16}, {1, 1, 16, 16},
                              {1, 64, 8, 8}, {1, 7, 8, 8}, {1, 1, 8, 8}}, 256, 2);
    ModelAdapter ad;
    std::string err;
    CHECK(ad.analyze(info, cfg, &err));
    if (err.empty()) {
        const auto& m = ad.metadata();
        CHECK(m.decode_type == DecodeType::kDflDist);
        CHECK_EQ(m.class_count, 7u);
        CHECK(m.dfl);
        CHECK(m.coordinate_format == CoordFormat::kLtrb);
        CHECK_EQ(m.strides.size(), 3u);
        if (m.strides.size() == 3) {
            CHECK_EQ(m.strides[0], 8u);
            CHECK_EQ(m.strides[1], 16u);
            CHECK_EQ(m.strides[2], 32u);
        }
    }
}

// 6) 用户 conf/iou 优先级：cfg 配置 > metadata 默认
TEST(decoder_dispatch_user_conf_iou) {
    auto info = make_info(1, {{1, 300, 6}});
    ModelAdapter ad;
    ModelAdapterConfig c2;
    c2.conf_thres = 0.7f; c2.iou_thres = 0.3f;
    std::string err;
    CHECK(ad.analyze(info, c2, &err));
    if (err.empty()) {
        CHECK_EQ(ad.effective_conf(), 0.7f);
        CHECK_EQ(ad.effective_iou(), 0.3f);
    }
}
