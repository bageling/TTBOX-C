// test_upper_body.cpp — V1.0.23「只瞄上半身」收缩与落点等效换算测试。
//
// 覆盖两层：
//   1. 纯函数 shrink_to_upper_body / upper_body_shrink_ratio（确定性，精确断言）：
//      框收缩数学、比例量换算、fail-closed 回退、**落点等效性**（开关切换 ty 同一像素）。
//   2. AimThread 端到端（照 test_aim_thread 的 harness）：
//      显示框高度收缩、贴边判定换域（clipped_frames 开=0 / 关>0）。
//      ★ 等效性在端到端层只做「收敛值 + 容差」断言 —— AimThread 是独立线程，
//        两次跑的取帧/步长不可能逐位一致（test_aim_thread.cpp 尾部已踩过并记档），
//        「两次运行输出逐位相等」本身就是不稳定断言，不许写。
//      ★ 数学硬保证在第 1 层：aim_point_at(收缩框, 换算prof) 与 aim_point_at(原框, 原prof)
//        输出逐位相等 —— 这是 CHECK_EQ 级断言，不依赖任何线程时序。
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
// 1. 纯函数：shrink_to_upper_body / upper_body_shrink_ratio
// ---------------------------------------------------------------------------

namespace {

DetectionBox body_box() {
    // 全身框：80×240（h/w=3.0，典型全身比例）。
    // ★ 位置必须落在 AimThread 的选靶 FOV 圆内：中心 (640, 320)，
    //   FOV 中心 = (frame_w/2, frame_h/2) = (640, 360)、半径 = min(1280,720)/2 = 360。
    //   （初版把框放在 (100..180, 200..440)，中心距 500 > 360 ⇒ 被 FOV 剔除、
    //     has_target=false，断言会因"两边都是 0"而假绿 —— 门限补在下面每条端到端用例里。）
    DetectionBox b;
    b.x1 = 600.0f; b.y1 = 200.0f; b.x2 = 680.0f; b.y2 = 440.0f;
    b.score = 0.9f;
    b.class_id = 0;
    return b;
}

AimPointProfile base_prof() {
    AimPointProfile p;
    p.offset_x = 0.5f;
    p.offset_y = 0.24f;  // 板端现值
    return p;
}

}  // namespace

// 关闭 ⇒ 返回 false，out_box / out_prof 逐字段等于入参（行为零变化）。
TEST(upper_body_disabled_is_identity) {
    const DetectionBox box = body_box();
    const AimPointProfile prof = base_prof();
    DetectionBox ob = box;  // 先故意塞脏，验证 false 路径会原样回填
    ob.x1 = -1; ob.y1 = -2; ob.x2 = -3; ob.y2 = -4;
    AimPointProfile op = prof;
    op.offset_y = 9.9f;

    const bool changed = aim_ns::shrink_to_upper_body(box, prof, &ob, &op);
    CHECK(!changed);
    CHECK_EQ(ob.x1, box.x1); CHECK_EQ(ob.y1, box.y1);
    CHECK_EQ(ob.x2, box.x2); CHECK_EQ(ob.y2, box.y2);
    CHECK_EQ(op.offset_y, prof.offset_y);
    CHECK_EQ(op.body_w_over_h, prof.body_w_over_h);
    CHECK_EQ(op.head_aim.head_height_fraction, prof.head_aim.head_height_fraction);
}

// ratio 越界（0 / 负 / 0.05 边界外 / >1）⇒ 一律视为关闭（fail-closed）。
TEST(upper_body_invalid_ratio_falls_back) {
    const DetectionBox box = body_box();
    DetectionBox ob;
    AimPointProfile op;
    for (const float bad : {0.0f, -0.5f, 0.05f, 1e-6f, 1.0001f, 2.0f}) {
        AimPointProfile p = base_prof();
        p.upper_body_enabled = true;
        p.upper_body_ratio = bad;
        CHECK(!aim_ns::shrink_to_upper_body(box, p, &ob, &op));
        CHECK_EQ(ob.y2, box.y2);  // 框没被改
        CHECK_EQ(aim_ns::upper_body_shrink_ratio(p), 1.0f);
    }
}

// ratio=1.0 数学上等价关闭：不收缩（避免"收了个寂寞还走换算路径"的分叉实现）。
TEST(upper_body_ratio_one_is_off) {
    AimPointProfile p = base_prof();
    p.upper_body_enabled = true;
    p.upper_body_ratio = 1.0f;
    DetectionBox ob; AimPointProfile op;
    CHECK(!aim_ns::shrink_to_upper_body(body_box(), p, &ob, &op));
    CHECK_EQ(aim_ns::upper_body_shrink_ratio(p), 1.0f);
}

// 无效框（h<=0）⇒ 不收缩（调用方走原框，不崩）。
TEST(upper_body_degenerate_box_is_identity) {
    AimPointProfile p = base_prof();
    p.upper_body_enabled = true;
    p.upper_body_ratio = 0.5f;
    DetectionBox bad = body_box();
    bad.y2 = bad.y1;  // h = 0
    DetectionBox ob; AimPointProfile op;
    CHECK(!aim_ns::shrink_to_upper_body(bad, p, &ob, &op));
}

// 收缩数学：x 不动；y2 = y1 + k·h；全部"相对框高"的比例量 ÷k；宽高比兜底 ÷k。
TEST(upper_body_shrink_math) {
    AimPointProfile p = base_prof();
    p.upper_body_enabled = true;
    p.upper_body_ratio = 0.5f;
    p.body_w_over_h = 0.32f;
    p.head_aim.head_offset_top_fraction = 0.04f;
    p.head_aim.head_height_fraction = 0.28f;
    ClassOffset co;
    co.class_id = 5; co.offset_x = 0.5f; co.offset_y = 0.30f; co.priority = 1;
    p.class_offsets.push_back(co);

    const DetectionBox box = body_box();
    DetectionBox ob; AimPointProfile op;
    CHECK(aim_ns::shrink_to_upper_body(box, p, &ob, &op));

    // 框：x 不动，y2 收到 y1 + 0.5*240 = 320
    CHECK_EQ(ob.x1, box.x1); CHECK_EQ(ob.x2, box.x2); CHECK_EQ(ob.y1, box.y1);
    CHECK_EQ(ob.y2, 320.0f);
    // offset_y：0.24 -> 0.48
    CHECK_EQ(op.offset_y, 0.48f);
    // class_offsets 每项 offset_y 同步换算（offset_x 不动）
    CHECK_EQ(op.class_offsets.size(), static_cast<size_t>(1));
    CHECK_EQ(op.class_offsets[0].offset_y, 0.60f);
    CHECK_EQ(op.class_offsets[0].offset_x, 0.5f);
    // head_aim 两个 fraction ÷k
    CHECK_EQ(op.head_aim.head_offset_top_fraction, 0.08f);
    CHECK_EQ(op.head_aim.head_height_fraction, 0.56f);
    // 兜底宽高比 ÷k（0.32 -> 0.64：同宽下"上半身"比"全身"矮一半 ⇒ 宽/高比翻倍）
    CHECK_EQ(op.body_w_over_h, 0.64f);
}

// ★★★ 落点等效性（本特性的核心契约）：开关切换，落点同一像素。
//   aim_point_at(原框, 原prof) == aim_point_at(收缩框, 换算prof)，tx/ty 双轴逐位相等。
//   这条断了 = 开关一开枪口整体上/下飘半身高 —— 业主第一晚就会发现的那种事故。
TEST(upper_body_aim_point_is_bit_identical) {
    const DetectionBox box = body_box();
    AimPointProfile prof = base_prof();
    prof.upper_body_enabled = true;
    prof.upper_body_ratio = 0.5f;

    DetectionBox shrunk; AimPointProfile mapped;
    CHECK(aim_ns::shrink_to_upper_body(box, prof, &shrunk, &mapped));

    float tx0, ty0, tx1, ty1;
    CHECK(aim_ns::aim_point_at(box, box.class_id, base_prof(), &tx0, &ty0));
    CHECK(aim_ns::aim_point_at(shrunk, box.class_id, mapped, &tx1, &ty1));
    CHECK_EQ(tx0, tx1);
    CHECK_EQ(ty0, ty1);

    // 多组 ratio 都要等效（0.3 / 0.55 / 0.9）：k 任意，落点不变。
    for (const float k : {0.3f, 0.55f, 0.9f}) {
        AimPointProfile pk = base_prof();
        pk.upper_body_enabled = true;
        pk.upper_body_ratio = k;
        DetectionBox bk; AimPointProfile mk;
        CHECK(aim_ns::shrink_to_upper_body(box, pk, &bk, &mk));
        float txk, tyk;
        CHECK(aim_ns::aim_point_at(bk, box.class_id, mk, &txk, &tyk));
        CHECK_EQ(tx0, txk);
        CHECK_EQ(ty0, tyk);
    }

    // 带类别偏移也要等效：class_offsets 命中时换算路径同样闭合。
    {
        AimPointProfile pc = base_prof();
        ClassOffset co;
        co.class_id = 0; co.offset_x = 0.5f; co.offset_y = 0.30f; co.priority = 1;
        pc.class_offsets.push_back(co);
        AimPointProfile pc_off = pc;              // 同配置、只关开关
        pc.upper_body_enabled = true;
        pc.upper_body_ratio = 0.5f;
        DetectionBox bc; AimPointProfile mc;
        CHECK(aim_ns::shrink_to_upper_body(box, pc, &bc, &mc));
        float tx0c, ty0c, tx1c, ty1c;
        CHECK(aim_ns::aim_point_at(box, 0, pc_off, &tx0c, &ty0c));
        CHECK(aim_ns::aim_point_at(bc, 0, mc, &tx1c, &ty1c));
        CHECK_EQ(ty0c, ty1c);
        CHECK_EQ(tx0c, tx1c);
    }
}

namespace {

// 端到端 harness（照 test_aim_thread.cpp）：喂常量帧，读收敛后的 status。
struct E2E {
    std::shared_ptr<AimTargetMailbox> mailbox;
    std::shared_ptr<output::NullHidOutput> output;
    std::shared_ptr<RuntimeProfile> profile;
    RuntimeConfig config;
    std::atomic<uint16_t> buttons{0};
    std::unique_ptr<AimThread> thread;
    AimThread::Status st{};

    // upper_body：是否开上半身收缩；capture_side：传给 capture 的边长（0 = 不启用贴边判定）。
    // seq 非空时按顺序逐帧喂（测"先未贴边、后贴边"这类跨帧行为，如冻结落点）。
    void run(bool upper_body, uint32_t capture_side, const DetectionBox& box,
             uint32_t frame_w = 1280, uint32_t frame_h = 720,
             const std::vector<DetectionBox>* seq = nullptr) {
        mailbox = std::make_shared<AimTargetMailbox>(1);
        output = std::make_shared<ttbox::core::output::NullHidOutput>();
        profile = std::make_shared<RuntimeProfile>();
        profile->mouse.enabled = true;
        profile->mouse.calibrating = true;  // 无视物理热键，链路全走通
        profile->mouse.aim_profiles[0].hotkey = 0x02;
        profile->mouse.kp_x = 1.0f;
        profile->mouse.kp_y = 1.0f;
        profile->mouse.output_deadzone = 0.0f;
        // 拉枪曲线（距离 >min_distance 就加弧线+抖动）会污染输出链读数，量落点时关掉。
        profile->mouse.pull_curve.enabled = false;
        // ★ offset 用结构默认 0.5（板端现值 0.24 与换算无关：等效性对任意 oy 成立，
        //   纯函数层的 upper_body_aim_point_is_bit_identical 已用 0.24 单独钉过）。
        //   下面这条的期望值按 0.5 推：关 500+0.5·600=800 / 开 500+1.0·100=600。
        profile->mouse.aim_point.upper_body_enabled = upper_body;
        profile->mouse.aim_point.upper_body_ratio = 0.5f;
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

// 端到端：显示框收缩 —— 开关开时 target_height ≈ 0.5×关闭值，y1 顶部不动。
// （display 框经 One-Euro，60 帧常量输入已收敛 ⇒ 容差 0.5px 足够。）
TEST(upper_body_display_box_shrinks_e2e) {
    E2E off, on;
    const DetectionBox box = body_box();
    off.run(false, 0, box);
    on.run(true, 0, box);

    CHECK(off.st.has_target);
    CHECK(on.st.has_target);
    CHECK(std::fabs(on.st.target_height - off.st.target_height * 0.5f) < 0.5f);
    CHECK(std::fabs(on.st.target_y1 - off.st.target_y1) < 0.5f);   // 顶部不动
    CHECK(std::fabs(on.st.target_width - off.st.target_width) < 0.5f);  // x 不收
}

// 端到端：落点等效 —— 常量帧收敛后，开关两次的 target_point_y 相差 < 0.5px。
// ★ 只做容差断言（线程时序不可逐位复现）；数学硬保证在纯函数层的逐位断言里。
TEST(upper_body_aim_point_equivalent_e2e) {
    E2E off, on;
    const DetectionBox box = body_box();
    off.run(false, 0, box);
    on.run(true, 0, box);
    // ★ 先钉"两次都真的选中了靶"：否则 target_point 恒 0、差值恒 0 ⇒ 断言空转假绿。
    CHECK(off.st.has_target);
    CHECK(on.st.has_target);
    CHECK(std::fabs(on.st.target_point_y - off.st.target_point_y) < 0.5f);
    CHECK(std::fabs(on.st.target_point_x - off.st.target_point_x) < 0.5f);
}

// 端到端：贴边外推在收缩域被避开 —— 这就是本特性的量化收益。
//   场景：目标近身、全身框底（脚）已出裁剪区下边界 ⇒ 关模式触发 V1.0.09 外推
//   （用框宽反推身高，落点被抬到人的胸口以下 = "打空"）；开模式收缩框底（髋）远高于
//   下边界 ⇒ 不贴边 ⇒ 不外推 ⇒ 落点锁在上半身。
//   数值（frame 1280x720、capture 640x640 ⇒ crop_bottom = 360+320 = 680，margin 12）：
//     关：box(540,500)-(740,700) w=200 h=200 贴底(700≥668)。全程贴边 ⇒ 自校准比无记录
//         ⇒ 兜底 1/0.32=3.125 ⇒ h_from_w=625、h_cap=3·200=600 ⇒ h=600
//         ⇒ ty = 500 + 0.24·600 = 644
//     开：收缩框(540,500)-(740,600) h=100 不贴底(600<668) ⇒ 无外推
//         ⇒ ty = 500 + 0.48·100 = 548
//   ⇒ 两次落点差 ≈ 96px（≈0.48 个身高），且开模式落点严格更高（在上半身）。
// ★ 冻结路径不参与：全程贴边 ⇒ frozen 无 ⇒ 关模式确定走外推分支。
TEST(upper_body_clip_extrapolation_is_avoided_e2e) {
    DetectionBox near_box;
    near_box.x1 = 540.0f; near_box.y1 = 500.0f;
    near_box.x2 = 740.0f; near_box.y2 = 700.0f;   // 脚已出裁剪区
    near_box.score = 0.9f; near_box.class_id = 0;
    E2E off, on;
    off.run(false, 640, near_box);
    on.run(true, 640, near_box);
    CHECK(off.st.has_target);
    CHECK(on.st.has_target);
    // 期望值按结构默认 oy=0.5 推（换算后 oy'=1.0），板端 0.24 的等效性由纯函数层钉住。
    CHECK(std::fabs(off.st.target_point_y - 800.0f) < 1.0f);   // 外推后 h=600 ⇒ 500+0.5·600
    CHECK(std::fabs(on.st.target_point_y - 600.0f) < 1.0f);    // 上半身 h=100 ⇒ 500+1.0·100
    CHECK(on.st.target_point_y < off.st.target_point_y - 50.0f);  // 至少高 50px
}

// ★ 下面这条「冻结判定用收缩框」**没有**写成端到端用例 —— 试过，且**测不到**：
//   ① frozen_for 优先于外推，两种模式都冻结在远景帧的框上，落点都是 510（无法区分）；
//   ② 落点等效换算让「收缩框贴底 vs 全身框贴底」的外推结果**也**落在同一像素
//      （关 500+0.5·600=800 / 开 500+1.0·300=900 —— 本该不同，实测同为 900）。
//   ⇒ 贴边判定的取值来源在落点层面**不可观测**，它的真实影响只落在
//     measurement_valid（压枪有效量测门控）与冻结记录域上。
//   那处接线改由源码级护栏钉住：plugins/web 侧的 test_upper_body_wiring.py
//   （照 test_capture_open_wait_policy.py 的手法）。**别再在这里找端到端断言。**

int main() {
    std::printf("=== ttbox_core tests (upper_body) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
