#include "test_util.hpp"
#include "mouse/PersonalMotion.hpp"

using namespace ttbox::core::aim;

TEST(personal_motion_disabled_is_identity) {
    PersonalMotion motion;
    PersonalMotionConfig cfg;
    cfg.enabled = false;
    cfg.knots = {0.2f, 0.8f};
    CHECK_EQ(motion.scale(80.0f, cfg), 1.0f);
}

TEST(personal_motion_interpolates_knots_by_error) {
    PersonalMotion motion;
    PersonalMotionConfig cfg;
    cfg.enabled = true;
    cfg.curve_blend = 1.0f;
    cfg.knots = {0.5f, 1.0f};
    CHECK_EQ(motion.scale(0.0f, cfg), 0.5f);
    CHECK_EQ(motion.scale(128.0f, cfg), 1.0f);
}

TEST(personal_motion_blend_keeps_default_component) {
    PersonalMotion motion;
    PersonalMotionConfig cfg;
    cfg.enabled = true;
    cfg.curve_blend = 0.5f;
    cfg.knots = {0.4f};
    CHECK_EQ(motion.scale(0.0f, cfg), 0.7f);
}

TEST(personal_motion_invalid_model_is_identity) {
    PersonalMotion motion;
    PersonalMotionConfig cfg;
    cfg.enabled = true;
    cfg.curve_blend = 1.0f;
    cfg.knots = {0.0f, 1.5f};
    CHECK_EQ(motion.scale(32.0f, cfg), 1.0f);
}

// knots 为空时用内置默认曲线（2026-09-26）：
// 面板从来没有 knots 编辑入口（vector 类型，表驱动不支持）⇒ 配置里它就是空的。
// 旧代码 valid() 因"knots 空"直接否决 ⇒ scale 恒 1.0 ⇒ **开关开了也是假开关**。
// 这条用例把兜底钉死：空 knots 也必须真的产生倍率。
TEST(personal_motion_empty_knots_uses_builtin_curve) {
    PersonalMotion motion;
    PersonalMotionConfig cfg;
    cfg.enabled = true;
    cfg.curve_blend = 1.0f;
    cfg.knots.clear();
    CHECK_EQ(motion.scale(0.0f, cfg), 0.75f);    // 贴脸：收力修得细
    CHECK_EQ(motion.scale(128.0f, cfg), 1.25f);  // 远距离：加力拉得快
    // 关掉必须回到 1.0（"不开就零影响"）
    cfg.enabled = false;
    CHECK_EQ(motion.scale(64.0f, cfg), 1.0f);
}

// 混合系数 0 ⇒ 完全按默认（不叠加曲线）
TEST(personal_motion_zero_blend_is_identity) {
    PersonalMotion motion;
    PersonalMotionConfig cfg;
    cfg.enabled = true;
    cfg.curve_blend = 0.0f;
    cfg.knots.clear();
    CHECK_EQ(motion.scale(128.0f, cfg), 1.0f);
}

int main() {
    return ttbox_test::run_all();
}
