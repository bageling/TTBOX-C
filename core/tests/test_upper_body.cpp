// test_upper_body.cpp — V1.0.24「只要人物上半身主体」算法测试。
//
// ★ 这版的设计前提（2026-10-04 业主口径）：**没有开关、没有配置项**。
//   上半身比例是算法常量（kUpperBodyRatio，见 AimPointProfile.cpp），
//   控制链与显示框无条件使用收缩框。所以本文件测的是「算法行为」，
//   不是「开关切换行为」—— 断言里不该再出现 enabled / 可调 ratio 之类。
//
// 覆盖两层：
//   1. 纯函数 shrink_to_upper_body（确定性，逐位断言）：
//      收缩数学、退化框、**落点等效性**（收缩前后 ty 同一像素）。
//   2. AimThread 端到端（照 test_aim_thread 的 harness）：无条件收缩下，
//      显示框 = 控制框、落点位置、近身贴边不再触发外推。
//      ★ 端到端**不可比"两次运行逐位相等"**（AimThread 独立线程，取帧/步长不可复现，
//        test_aim_thread.cpp 尾部已踩过并记档）⇒ 只做"收敛值 + 容差"断言。
//      ★ 测试框必须在选靶 FOV 圆内（中心=帧中心、半径=min(roi_w,roi_h)/2），
//        放外面 ⇒ has_target=false ⇒ "比较两次"的断言比两个 0 之差 ⇒ 恒绿假绿。
#include "test_util.hpp"

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#include "aim/AimThread.hpp"
#include "mouse/AimPointProfile.hpp"
#include "mouse/MouseTypes.hpp"
#include "output/IHidOutput.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;
namespace aim_ns = ttbox::core::aim;

// ---------------------------------------------------------------------------
// 1. 纯函数
// ---------------------------------------------------------------------------
namespace {

DetectionBox body_box() {
    // 全身框 80×240（h/w=3.0，典型全身比例）；中心 (640,320) 落在 FOV 圆内
    // （FOV 中心 (640,360)、半径 min(1280,720)/2=360）。
    DetectionBox b;
    b.x1 = 600.0f; b.y1 = 200.0f; b.x2 = 680.0f; b.y2 = 440.0f;
    b.score = 0.9f;
    b.class_id = 0;
    return b;
}

AimPointProfile base_prof() {
    AimPointProfile p;
    p.offset_x = 0.5f;
    p.offset_y = 0.5f;  // 结构默认（板端现值 0.24 是配置写进去的；等效性对任意 oy 成立）
    return p;
}

}  // namespace

// 比例是算法常量：不在配置里（业主明确不要开关/参考物）。
TEST(upper_body_ratio_is_an_algorithm_constant) {
    const float k = aim_ns::upper_body_ratio();
    CHECK(k > 0.05f);
    CHECK(k <= 1.0f);
    // 人体几何依据：头顶到髋约占全身高 0.5（头 0.13 + 颈肩到髋 0.40）。
    // 卡上界 0.6 是为了防止有人把比例调到 >0.6 —— 切进胸口意味着落点被顶高，
    // 是最糟的方向（V1.0.09 修的正是"落点上飘"这个症状）。
    CHECK(k <= 0.6f);
    CHECK_EQ(aim_ns::upper_body_ratio(), k);  // 恒定：不是从配置/环境读的
}

// 无效框（h<=0）⇒ 原样返回，调用方走原框（不崩、不产生负高）。
TEST(upper_body_degenerate_box_is_identity) {
    DetectionBox bad = body_box();
    bad.y2 = bad.y1;  // h = 0
    DetectionBox ob; AimPointProfile op;
    const bool changed = aim_ns::shrink_to_upper_body(bad, base_prof(), &ob, &op);
    CHECK(!changed);
    CHECK_EQ(ob.y1, bad.y1);
    CHECK_EQ(ob.y2, bad.y2);
}

// 收缩数学：x 不动；y2 = y1 + k·h；所有"相对框高"的比例量 ÷k；宽高比兜底 ÷k。
TEST(upper_body_shrink_math) {
    const float k = aim_ns::upper_body_ratio();
    AimPointProfile p = base_prof();
    p.body_w_over_h = 0.32f;
    p.head_aim.head_offset_top_fraction = 0.04f;
    p.head_aim.head_height_fraction = 0.28f;
    ClassOffset co;
    co.class_id = 5; co.offset_x = 0.5f; co.offset_y = 0.30f; co.priority = 1;
    p.class_offsets.push_back(co);

    const DetectionBox box = body_box();
    DetectionBox ob; AimPointProfile op;
    CHECK(aim_ns::shrink_to_upper_body(box, p, &ob, &op));

    // 框：x 不动，y2 收到 y1 + k·240
    CHECK_EQ(ob.x1, box.x1); CHECK_EQ(ob.x2, box.x2); CHECK_EQ(ob.y1, box.y1);
    CHECK_EQ(ob.y2, 200.0f + k * 240.0f);
    CHECK(ob.y2 < box.y2);
    // offset_y ÷k
    CHECK_EQ(op.offset_y, 0.5f / k);
    // class_offsets 每项 offset_y 同步换算（offset_x 不动）
    CHECK_EQ(op.class_offsets.size(), static_cast<size_t>(1));
    CHECK_EQ(op.class_offsets[0].offset_y, 0.30f / k);
    CHECK_EQ(op.class_offsets[0].offset_x, 0.5f);
    // head_aim 两个 fraction ÷k
    CHECK_EQ(op.head_aim.head_offset_top_fraction, 0.04f / k);
    CHECK_EQ(op.head_aim.head_height_fraction, 0.28f / k);
    // 兜底宽高比 ÷k（同宽下"上半身"比"全身"矮一半 ⇒ 宽/高比翻倍）
    CHECK_EQ(op.body_w_over_h, 0.32f / k);
}

// ★★★ 落点等效性（本算法的核心契约）：收缩前后落点同一像素。
//   ty = y1 + oy·h ；ty' = y1 + (oy/k)·(k·h) = y1 + oy·h。
//   这条断了 = 一上线枪口就整体上飘 k 倍身高 —— 业主第一晚就会发现的那种事故。
TEST(upper_body_aim_point_is_bit_identical) {
    const float k = aim_ns::upper_body_ratio();
    const DetectionBox box = body_box();
    const AimPointProfile prof = base_prof();

    DetectionBox shrunk; AimPointProfile mapped;
    CHECK(aim_ns::shrink_to_upper_body(box, prof, &shrunk, &mapped));

    float tx0, ty0, tx1, ty1;
    CHECK(aim_ns::aim_point_at(box, box.class_id, prof, &tx0, &ty0));
    CHECK(aim_ns::aim_point_at(shrunk, box.class_id, mapped, &tx1, &ty1));
    CHECK_EQ(tx0, tx1);
    CHECK_EQ(ty0, ty1);

    // 板端现值 oy=0.24 也要闭合（不是只对结构默认 0.5 成立）。
    {
        AimPointProfile p24 = base_prof();
        p24.offset_y = 0.24f;
        DetectionBox b24; AimPointProfile m24;
        CHECK(aim_ns::shrink_to_upper_body(box, p24, &b24, &m24));
        float a, b2, c, d;
        CHECK(aim_ns::aim_point_at(box, 0, p24, &a, &b2));
        CHECK(aim_ns::aim_point_at(b24, 0, m24, &c, &d));
        CHECK_EQ(b2, d);
    }
    // 类别偏移命中时同样闭合。
    {
        AimPointProfile pc = base_prof();
        ClassOffset co;
        co.class_id = 0; co.offset_x = 0.5f; co.offset_y = 0.30f; co.priority = 1;
        pc.class_offsets.push_back(co);
        DetectionBox bc; AimPointProfile mc;
        CHECK(aim_ns::shrink_to_upper_body(box, pc, &bc, &mc));
        float a, b2, c, d;
        CHECK(aim_ns::aim_point_at(box, 0, pc, &a, &b2));
        CHECK(aim_ns::aim_point_at(bc, 0, mc, &c, &d));
        CHECK_EQ(b2, d);
        CHECK_EQ(a, c);
    }
    // 收缩量确实生效（k<1），否则上面全是恒等式空转。
    CHECK(k < 1.0f);
    CHECK(shrunk.y2 < box.y2);
}

// ---------------------------------------------------------------------------
// 2. AimThread 端到端（无条件收缩）
// ---------------------------------------------------------------------------
namespace {

struct E2E {
    std::shared_ptr<AimTargetMailbox> mailbox;
    std::shared_ptr<output::NullHidOutput> output;
    std::shared_ptr<RuntimeProfile> profile;
    RuntimeConfig config;
    std::atomic<uint16_t> buttons{0};
    std::unique_ptr<AimThread> thread;
    AimThread::Status st{};

    // capture_side：传给 capture 的边长（0 = 不启用贴边判定）。
    // seq 非空时按顺序逐帧喂（测跨帧行为，如"先未贴边、后贴边"）。
    void run(uint32_t capture_side, const DetectionBox& box,
             uint32_t frame_w = 1280, uint32_t frame_h = 720,
             const std::vector<DetectionBox>* seq = nullptr) {
        mailbox = std::make_shared<AimTargetMailbox>(1);
        output = std::make_shared<output::NullHidOutput>();
        profile = std::make_shared<RuntimeProfile>();
        profile->mouse.enabled = true;
        profile->mouse.calibrating = true;  // 无视物理热键，链路全走通
        profile->mouse.aim_profiles[0].hotkey = 0x02;
        profile->mouse.kp_x = 1.0f;
        profile->mouse.kp_y = 1.0f;
        profile->mouse.output_deadzone = 0.0f;
        // 拉枪曲线（距离 >min_distance 就加弧线+抖动）会污染读数，量落点时关掉。
        profile->mouse.pull_curve.enabled = false;
        if (capture_side > 0) {
            profile->capture.width = capture_side;
            profile->capture.height = capture_side;
        }
        config.update(profile);

        const int frames = seq ? static_cast<int>(seq->size()) : 60;
        thread = std::make_unique<AimThread>();
        CHECK(thread->start(&*mailbox, output, 1000, &config, &buttons));
        for (uint64_t f = 1; f <= static_cast<uint64_t>(frames); ++f) {
            AimTargetTask t;
            t.frame_number = f;
            t.timestamp_us = 1000ULL * f;
            t.frame_width = frame_w;
            t.frame_height = frame_h;
            t.has_target = true;
            t.target = seq ? (*seq)[static_cast<size_t>(f - 1)] : box;
            t.detections.push_back(t.target);
            mailbox->offer(0, t);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        thread->stop();
        st = thread->status();
    }
};

}  // namespace

// 端到端：显示框就是控制用的上半身框 —— 高度 = 原始框高 ×k，顶部不动、宽度不变。
// ★ 宽度不变这条同时钉住"显示框不再做多框并集"：并集会把关联框（头/躯干局部框）
//   拉进来使 x1/x2 外扩；这里 detections 里只有本体框，任何并集都无框可并 ⇒ 恒等。
//   真正防"并集复活"的护栏在 core/tests/test_upper_body_wiring.py（源码级）。
TEST(upper_body_display_box_is_the_control_box_e2e) {
    const float k = aim_ns::upper_body_ratio();
    E2E e;
    e.run(0, body_box());
    CHECK(e.st.has_target);
    const float raw_h = 240.0f;
    CHECK(std::fabs(e.st.target_height - raw_h * k) < 0.5f);
    CHECK(std::fabs(e.st.target_y1 - 200.0f) < 0.5f);           // 顶部不动
    CHECK(std::fabs(e.st.target_width - 80.0f) < 0.5f);         // 宽度不收
    CHECK(std::fabs(e.st.target_y2 - (200.0f + raw_h * k)) < 0.5f);
}

// 端到端：落点落在上半身框内、位置 = y1 + (oy/k)·(k·h) —— 与收缩前同一像素。
TEST(upper_body_aim_point_lands_at_chest_e2e) {
    E2E e;
    e.run(0, body_box());
    CHECK(e.st.has_target);
    const float k = aim_ns::upper_body_ratio();
    // 落点 = y1 + (oy/k)·(k·h) = y1 + oy·h（等效）。oy=0.5（结构/档位默认）、h=240
    // ⇒ 200 + 0.5·240 = 320。★ 别把 (oy/k)·(k·h) 误算成 oy·h —— 那正是等效换算
    //   要保证的恒等式，算错会把"落点上飘 k 倍"当成正确。
    CHECK(std::fabs(e.st.target_point_y - (200.0f + 0.5f * 240.0f)) < 0.5f);
    CHECK(std::fabs(e.st.target_point_x - 640.0f) < 0.5f);
}

// 端到端：近身贴边时**不**触发 V1.0.09 外推（收缩框底在髋，够不到裁剪区下边界）。
//   场景：box(540,500)-(740,700)，全身框底 y2=700 已出裁剪区
//   （capture 640×640 + frame 720 ⇒ crop_bottom=360+320=680，margin 12）。
//   收缩后 y2 = 500 + 0.5·200 = 600 < 668 ⇒ 不贴边 ⇒ 落点不被外推抬高。
//   （若有人把贴边判定改回全身框，这里会红。）
TEST(upper_body_near_clip_no_longer_extrapolates_e2e) {
    DetectionBox near_box;   // h/w=1.0 的大胖框，y2 贴边
    near_box.x1 = 540.0f; near_box.y1 = 500.0f;
    near_box.x2 = 740.0f; near_box.y2 = 700.0f;
    near_box.score = 0.9f; near_box.class_id = 0;
    E2E e;
    e.run(640, near_box);
    CHECK(e.st.has_target);
    // 不外推 ⇒ 落点 = 500 + 0.5·200 = 600；外推（h 拉到 3 倍）会抬到 500+0.5·600=800。
    CHECK(std::fabs(e.st.target_point_y - 600.0f) < 1.0f);
}

int main() {
    std::printf("=== ttbox_core tests (upper_body) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
