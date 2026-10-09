// test_model_metadata.cpp — A-7 单元测试：ModelMetadata 20 项字段
#include "test_util.hpp"

#include "model/ModelMetadata.hpp"

using namespace ttbox::core;

TEST(model_metadata_fields) {
    ModelMetadata m;

    // ---- 20 项字段存在性 + 默认值 ----
    CHECK_EQ(m.input_width, 0u); CHECK_EQ(m.input_height, 0u); CHECK_EQ(m.input_channels, 0u);
    CHECK_EQ(m.input_dtype, 0); CHECK_EQ(m.input_layout, 0);
    CHECK(m.color_order == ColorOrder::kBgr);
    CHECK(m.quantization_type == QuantType::kNone);
    CHECK_EQ(m.input_size, 0u);
    CHECK_EQ(m.output_count, 0u);
    CHECK(m.output_dtypes.empty());
    CHECK(m.output_layouts.empty());
    CHECK(m.output_shapes.empty());
    CHECK(m.decode_type == DecodeType::kUnknown);
    CHECK(m.strides.empty());
    CHECK_EQ(m.class_count, 0u);
    CHECK(!m.objectness);
    CHECK(!m.dfl);
    CHECK(m.nms_type == NmsType::kClasswise);
    CHECK(m.coordinate_format == CoordFormat::kXywh);
    CHECK_EQ(m.default_conf, 0.25f);
    CHECK_EQ(m.default_iou, 0.45f);
    CHECK(m.label.empty());
}

TEST(model_metadata_huangwa) {
    // 黄瓦：320 INT8 NHWC 6 输出 DFL 2 类 BGR
    ModelMetadata m;
    m.input_width = 320; m.input_height = 320; m.input_channels = 3;
    m.input_dtype = 2; m.input_layout = 1; m.input_size = 307200;
    m.color_order = ColorOrder::kBgr;
    m.quantization_type = QuantType::kInt8;
    m.output_count = 6;
    m.output_dtypes = {2, 2, 2, 2, 2, 2};
    m.output_shapes = {{1, 1, 4, 1600}, {1, 2, 40, 40}, {1, 1, 4, 400},
                       {1, 2, 20, 20}, {1, 1, 4, 100}, {1, 2, 10, 10}};
    m.decode_type = DecodeType::kDfl;
    m.strides = {8, 16, 32};
    m.class_count = 2;
    m.dfl = true;
    m.coordinate_format = CoordFormat::kLtrb;
    m.nms_type = NmsType::kClasswise;
    m.default_conf = 0.55f; m.default_iou = 0.45f;
    CHECK(m.decode_type == DecodeType::kDfl);
    CHECK_EQ(m.strides.size(), 3u);
    CHECK_EQ(m.strides[0], 8u);
    CHECK_EQ(m.strides[2], 32u);
    CHECK_EQ(m.class_count, 2u);
    CHECK(m.dfl);
}

TEST(model_metadata_v26m) {
    // v26m：640 RGB E2E [1,300,6] xyxy
    ModelMetadata m;
    m.input_width = 640; m.input_height = 640; m.input_channels = 3;
    m.color_order = ColorOrder::kRgb;
    m.quantization_type = QuantType::kInt8;
    m.output_count = 1;
    m.output_shapes = {{1, 300, 6}};
    m.decode_type = DecodeType::kE2e;
    m.coordinate_format = CoordFormat::kXyxy;
    CHECK(m.decode_type == DecodeType::kE2e);
    CHECK(m.coordinate_format == CoordFormat::kXyxy);
    CHECK(!m.dfl);
    CHECK(!m.objectness);
}
