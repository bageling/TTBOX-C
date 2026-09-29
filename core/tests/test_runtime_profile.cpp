// test_runtime_profile.cpp — A-8 单元测试：RuntimeProfile JSON/校验/RuntimeConfig 热更新
#include "test_util.hpp"

#include <limits>
#include <cmath>
#include "model/RuntimeProfile.hpp"

using namespace ttbox::core;

TEST(runtime_profile_json_roundtrip) {
    RuntimeProfile p;
    p.model_id = "huangwa";
    p.capture.width = 640;
    p.capture.height = 640;
    p.capture.offset_x = 100;
    p.capture.offset_y = 50;
    p.inference.confidence = 0.55f;
    p.inference.iou = 0.45f;
    p.inference.class_filter = {0, 1};
    p.inference.max_detections = 50;
    p.fov.enabled = true;
    p.fov.shape = FovShape::kCircle;
    p.fov.radius = 0.4f;
    p.fov.center_x = 0.5f;
    p.fov.center_y = 0.5f;

    const std::string text = p.to_json().dump();
    auto res = json_parse(text);
    CHECK(res.ok);
    if (!res.ok) return;
    RuntimeProfile q = RuntimeProfile::from_json(res.value);
    CHECK(q.model_id == "huangwa");
    CHECK_EQ(q.capture.width, 640u);
    CHECK_EQ(q.capture.offset_x, 100);
    CHECK_EQ(q.inference.confidence, 0.55f);
    CHECK_EQ(q.inference.iou, 0.45f);
    CHECK_EQ(q.inference.class_filter.size(), 2u);
    if (q.inference.class_filter.size() == 2) {
        CHECK_EQ(q.inference.class_filter[0], 0);
        CHECK_EQ(q.inference.class_filter[1], 1);
    }
    CHECK_EQ(q.inference.max_detections, 50);
    CHECK(q.fov.enabled);
    CHECK(q.fov.shape == FovShape::kCircle);
    CHECK_EQ(q.fov.radius, 0.4f);
}

TEST(runtime_profile_validate_bounds) {
    RuntimeProfile p;
    p.inference.confidence = 0.5f;
    p.inference.iou = 0.4f;
    std::string err;
    CHECK(p.validate(&err));

    RuntimeProfile bad;
    bad.inference.confidence = 1.5f;
    CHECK(!bad.validate(&err));
    CHECK(!err.empty());

    RuntimeProfile bad2;
    bad2.inference.iou = -0.1f;
    CHECK(!bad2.validate(&err));

    RuntimeProfile bad3;
    bad3.fov.enabled = true;
    bad3.fov.center_x = 1.2f;
    CHECK(!bad3.validate(&err));

    RuntimeProfile bad4;
    bad4.inference.class_filter = {2, -1};
    CHECK(!bad4.validate(&err));
}

TEST(runtime_profile_roi_bounds) {
    CaptureProfile cap;
    cap.width = 640;
    cap.height = 640;
    cap.offset_x = 0;
    cap.offset_y = 0;
    std::string err;
    CHECK(cap.valid(1920, 1080, &err));  // 全帧内

    CaptureProfile out;
    out.width = 2000;  // 越界
    out.offset_x = 0;
    out.offset_y = 0;
    CHECK(!out.valid(1920, 1080, &err));
    CHECK(!err.empty());

    // offset 语义为"相对屏幕中心的偏移"：大偏移会被 clamp 到全帧内（合法）
    CaptureProfile offset_out;
    offset_out.width = 100;
    offset_out.offset_x = 1900;  // 相对中心偏移 1900，clamp 后界内
    offset_out.offset_y = 0;
    CHECK(offset_out.valid(1920, 1080, &err));

    // 负偏移同样合法（clamp 到左上角）
    CaptureProfile neg;
    neg.width = 100;
    neg.offset_x = -500;
    neg.offset_y = -500;
    CHECK(neg.valid(1920, 1080, &err));

    CaptureProfile zero;  // 0x0 = 全帧，合法
    CHECK(zero.valid(1920, 1080, &err));
    CaptureProfile full;  // 0x0 = 全帧，合法
    CHECK(full.valid(1920, 1080, &err));
}

TEST(runtime_config_hot_update) {
    RuntimeConfig cfg;
    CHECK(cfg.empty());

    RuntimeProfile p1;
    p1.inference.confidence = 0.4f;
    cfg.update(std::make_shared<RuntimeProfile>(p1));
    CHECK(!cfg.empty());
    {
        auto s = cfg.snapshot();
        CHECK_EQ(s->inference.confidence, 0.4f);
    }

    // 热更新：替换配置（内存原子交换）
    RuntimeProfile p2;
    p2.inference.confidence = 0.7f;
    p2.fov.enabled = true;
    p2.fov.radius = 0.3f;
    cfg.update(std::make_shared<RuntimeProfile>(p2));
    {
        auto s = cfg.snapshot();
        CHECK_EQ(s->inference.confidence, 0.7f);
        CHECK(s->fov.enabled);
        CHECK_EQ(s->fov.radius, 0.3f);
    }
    // 旧快照仍持有（RAII 安全）
    auto old = cfg.snapshot();
    RuntimeProfile p3;
    p3.inference.confidence = 0.9f;
    cfg.update(std::make_shared<RuntimeProfile>(p3));
    CHECK_EQ(old->inference.confidence, 0.7f);  // 旧对象未被破坏
}

TEST(runtime_profile_validate_rejects_nonfinite) {
    // C12 修复回归：inf/NaN 配置必须被 validate 拒绝（否则 PID 输出乱飞）
    RuntimeProfile p;
    // 手动塞入 inf（绕过 from_json 的默认路径，模拟 JSON 1e999 解析结果）
    p.mouse.kp_x = std::numeric_limits<float>::infinity();
    std::string err;
    CHECK(!p.validate(&err));
    CHECK(err.find("非有限") != std::string::npos);

    p.mouse.kp_x = 17.0f;
    p.mouse.kd_y = std::numeric_limits<float>::quiet_NaN();
    CHECK(!p.validate(&err));
}

TEST(runtime_profile_validate_rejects_degenerate_capture) {
    // 事故回归（2026-09-10）：capture=1×1 退化配置曾静默摧毁整条流水线
    // （AI ROI 缩成 1 像素 → 推理停摆；预览裁成 1×1），且 validate 全程放行。
    // 现在必须 fail-closed 拒绝；0=全帧仍然合法；from_json 对历史坏值自愈为 0。
    std::string err;

    RuntimeProfile ok_full;  // 0×0 = 全帧，合法
    CHECK(ok_full.validate(&err));

    RuntimeProfile ok_640;
    ok_640.capture.width = 640;
    ok_640.capture.height = 640;
    CHECK(ok_640.validate(&err));

    RuntimeProfile bad_1x1;  // 事故值：1×1 必须被拒绝
    bad_1x1.capture.width = 1;
    bad_1x1.capture.height = 1;
    CHECK(!bad_1x1.validate(&err));
    CHECK(err.find("capture") != std::string::npos);

    RuntimeProfile bad_small;  // 63×63：低于硬下限 64，拒绝
    bad_small.capture.width = 63;
    bad_small.capture.height = 63;
    CHECK(!bad_small.validate(&err));

    RuntimeProfile bad_huge;  // 超上限 3840，拒绝
    bad_huge.capture.width = 4000;
    bad_huge.capture.height = 4000;
    CHECK(!bad_huge.validate(&err));

    // from_json 自愈：磁盘上遗留的 1×1 坏配置加载时纠正为 0（全帧），
    // 保证 Core 重启不会因历史坏值瞎跑或起不来。
    const std::string legacy = R"({"capture":{"width":1,"height":1,"offset_x":0,"offset_y":0}})";
    auto res = json_parse(legacy);
    CHECK(res.ok);
    if (res.ok) {
        RuntimeProfile healed = RuntimeProfile::from_json(res.value);
        CHECK_EQ(healed.capture.width, 0u);
        CHECK_EQ(healed.capture.height, 0u);
        CHECK(healed.validate(&err));  // 自愈后必须能通过校验
    }
}

TEST(runtime_profile_personal_motion_roundtrip) {
    RuntimeProfile p;
    p.mouse.personal_motion.enabled = true;
    p.mouse.personal_motion.curve_blend = 0.8f;
    p.mouse.personal_motion.knots = {0.1f, 0.4f, 0.9f};
    // speed_blend / reaction_blend / max_reaction_delay_ms 已删（core 从不读），
    // 往返只验在用的字段。

    auto parsed = json_parse(p.to_json().dump());
    CHECK(parsed.ok);
    if (!parsed.ok) return;
    RuntimeProfile q = RuntimeProfile::from_json(parsed.value);
    CHECK(q.mouse.personal_motion.enabled);
    CHECK_EQ(q.mouse.personal_motion.curve_blend, 0.8f);
    CHECK_EQ(q.mouse.personal_motion.knots.size(), 3u);
}

TEST(runtime_profile_personal_motion_validate_bounds) {
    RuntimeProfile p;
    std::string err;
    p.mouse.personal_motion.curve_blend = 1.1f;
    CHECK(!p.validate(&err));
    p.mouse.personal_motion.curve_blend = 0.5f;
    p.mouse.personal_motion.knots = {1.2f};
    CHECK(!p.validate(&err));
}

// ★ 2026-09-29 出货前补的升级路径护栏（1.5.70）：
//   1.5.69 删掉了一代提前量 Lead1 与 v7.26 扳机 AutoTrigger 的整段配置
//   （RuntimeProfile 的 lead1 / trigger 三段 + recoil_y_offset_px / target_height_px
//   等死接线）。★ 而 config.d **不被 OTA 覆盖** ⇒ 板端老配置里这些键还在，
//   盒子从 1.5.68 直升上来时 from_json 必须原样忽略它们、照常加载并通过 validate。
//   此前没有任何用例钉这条路径，故补上（删键类版本升级的通用护栏）。
TEST(runtime_profile_legacy_removed_keys_still_load) {
    // 模拟板端 1.5.68 写下的 mouse 段：含 1.5.69 已删的全部键
    const char* legacy = R"JSON({
      "model_id": "EP",
      "capture": {"width": 640, "height": 640, "offset_x": 0, "offset_y": 0},
      "inference": {"confidence": 0.35, "iou": 0.45, "max_detections": 30},
      "mouse": {
        "enabled": true,
        "sensitivity": 1.25,
        "kp_x": 0.5, "kp_y": 0.4,
        "lead1": {"enabled": true, "frames": 3, "gain": 1.4, "max_offset": 44,
                  "activation_distance": 120},
        "lead1_enabled": true,
        "lead1_frames": 3,
        "trigger": {"enabled": true, "fire_button": "left", "fire_interval": 5,
                    "confidence": 0.6, "fire_count": 2, "fire_random": 0.2},
        "trigger_enabled": true,
        "recoil_y_offset_px": 7,
        "target_height_px": 120,
        "trigger_recoil_offset_px": 3,
        "personal_motion": {"speed_blend": 0.3, "reaction_blend": 0.2,
                            "max_reaction_delay_ms": 40}
      }
    })JSON";

    auto res = json_parse(legacy);
    CHECK(res.ok);
    if (!res.ok) return;

    RuntimeProfile p = RuntimeProfile::from_json(res.value);

    // ① 还在用的键照常读出来（删键不能连累正常字段）
    CHECK(p.model_id == "EP");
    CHECK_EQ(p.capture.width, 640u);
    CHECK_EQ(p.capture.height, 640u);
    CHECK(p.mouse.enabled);
    CHECK_EQ(p.mouse.sensitivity, 1.25f);
    CHECK_EQ(p.mouse.kp_x, 0.5f);
    CHECK_EQ(p.inference.confidence, 0.35f);
    CHECK_EQ(p.inference.max_detections, 30);

    // ② 合法老配置必须能通过校验（否则升级即拒载 / 服务起不来）
    std::string err;
    CHECK(p.validate(&err));

    // ③ 序列化回去不再吐出这些已删键（防止它们经面板合并再被写回配置）
    const std::string out = p.to_json().dump();
    CHECK(out.find("lead1") == std::string::npos);
    CHECK(out.find("\"trigger\":") == std::string::npos);
    CHECK(out.find("recoil_y_offset_px") == std::string::npos);
    CHECK(out.find("target_height_px") == std::string::npos);
    CHECK(out.find("trigger_recoil_offset_px") == std::string::npos);
    // 反向：仍在用的二代（lead2 / trigger2）不能连坐被删
    CHECK(out.find("\"lead2\"") != std::string::npos);
    CHECK(out.find("\"trigger2\"") != std::string::npos);
}
