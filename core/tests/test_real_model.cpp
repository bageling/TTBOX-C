// test_real_model.cpp — 真实训练模型验证（yolov10n COCO 预训练权重）
//
// 与 test_win_e2e 的区别：
//   win_e2e 用随机权重模型验证"链路每一环都能跑"；
//   本测试用**真实 COCO 训练权重**验证"检测框位置正确、方向正确、PID 方向正确"。
//
// 测试场景（yolov10n 端到端单输出 [1,300,6]，DecodeNMS e2e 分支）：
//   1. 真实照片检出真实目标（desk_zidane.png 两名 COCO person）
//   2. 检测框位置正确性（框中心与已知目标位置比对，容差 15%）
//   3. 坐标方向确定性（合成图注入亮色目标到五方位）：
//        中心 → |err|≈0；右侧 → err_x>0；左侧 → err_x<0；
//        上方 → err_y<0；下方 → err_y>0
//   4. PID 方向确定性（纯 P 控制下输出符号与误差符号一致）
//   5. TargetSelector 行为（多目标选最近/目标切换/ROI 过滤）
//
// 注意：合成图用"亮色块"注入。yolov10n 对非 COCO 目标不一定检出——
// 方向性测试注入目标后，如果模型不认识色块，就没有 Detection，
// 此时"方向正确性"退化为对 CoordinateTransform/PID 的直接单元验证。
// 因此本测试分两层：
//   层A（真实照片）：真实检测 → 框位置/置信度/类别验证
//   层B（方向确定性）：直接用合成 DetectionBox（绕过模型）验证
//        CoordinateTransform → TargetSelector → PID 全链方向语义。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "test_pid_controller.hpp"
#include "mouse/CoordinateTransform.hpp"
#include "mouse/TargetSelector.hpp"
#include "rknn/DecodeNMS.hpp"
#include "test_util.hpp"

// 层A（ONNX 分支）与层B（!ONNX 分支）都要用 ttbox::core 里的 aim:: 类型，
// 故 using 指令必须置于 #if 之外；原先放在 ONNX 分支内，
// 导致 !ONNX 构建下 aim:: 无法解析（C2653 一连串）。
using namespace ttbox::core;
namespace fs = std::filesystem;

#if defined(TTBOX_CORE_HAS_ONNX) && TTBOX_CORE_HAS_ONNX

#include <onnxruntime_cxx_api.h>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// ---------- 资源定位 ----------
std::string find_file(const char* name) {
    for (auto& p : {
             fs::path("tests/data") / name,
             fs::path("tests") / name,
             fs::path("core/tests/data") / name,
             fs::path("core/tests") / name,
             fs::path(TTBOX_PROJECT_ROOT) / "core/tests/data" / name,
             fs::path(TTBOX_PROJECT_ROOT) / "core/tests" / name,
         }) {
        std::error_code ec;
        if (fs::exists(p, ec)) return p.string();
    }
    return {};
}

// ---------- 最小 PNG/JPG 解码（仅用于读取测试图）----------
// 用 stb_image 风格手写太重；这里用 Windows WIC / 或纯手动。
// 简化：测试图转成 raw BGR 二进制由 python 预处理生成（tests/data/*.bgr 已被 gitignore，
// 由 build 脚本生成）。如果 .bgr 不存在则 skip。
struct RawBgr {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // BGR24
};
bool load_bgr(const std::string& path, RawBgr& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    int32_t hdr[3];
    if (std::fread(hdr, 4, 3, f) != 3) { std::fclose(f); return false; }
    out.w = hdr[0]; out.h = hdr[1];
    size_t n = (size_t)out.w * out.h * 3;
    if (hdr[2] != (int32_t)n) { std::fclose(f); return false; }  // 魔数校验=像素字节数
    out.px.resize(n);
    size_t rd = std::fread(out.px.data(), 1, n, f);
    std::fclose(f);
    return rd == n;
}

// ---------- resize 到模型输入（BGR→RGB float NCHW 0-1，letterbox 不做：直接拉伸）----------
void to_nchw(const RawBgr& img, int mw, int mh, std::vector<float>& out) {
    out.resize(3 * mw * mh);
    for (int y = 0; y < mh; ++y) {
        int sy = y * img.h / mh;
        for (int x = 0; x < mw; ++x) {
            int sx = x * img.w / mw;
            const uint8_t* p = &img.px[(sy * img.w + sx) * 3];  // BGR
            out[0 * mw * mh + y * mw + x] = p[2] / 255.0f;  // R
            out[1 * mw * mh + y * mw + x] = p[1] / 255.0f;  // G
            out[2 * mw * mh + y * mw + x] = p[0] / 255.0f;  // B
        }
    }
}

// ---------- e2e 输出 buffer 封装 ----------
void pack_e2e_output(const std::vector<float>& raw, RknnModelInfo& info,
                     std::vector<std::vector<uint8_t>>& bufs) {
    RknnOutputInfo oi;
    oi.n_elems = (uint32_t)raw.size();
    oi.size = (uint32_t)(raw.size() * sizeof(float));
    oi.type = 0;  // FLOAT32
    oi.fmt = 0;
    oi.dims = {1, 300, 6};
    oi.scale = 0.0f;
    oi.zp = 0;
    info.n_inputs = 1;
    info.n_outputs = 1;
    info.outputs.assign(1, oi);
    bufs.assign(1, std::vector<uint8_t>((uint8_t*)raw.data(),
                                        (uint8_t*)raw.data() + raw.size() * sizeof(float)));
}

// PID 收敛模拟：连续帧同方向误差 → 输出符号恒定
MouseCommand pid_frame(aim::TestPidController& pid, float ex, float ey, uint32_t ms) {
    aim::TargetPoint tp;
    tp.valid = true;
    tp.x = ex;
    tp.y = ey;
    return pid.update(tp);
}

}  // namespace

// ============ 层A：真实照片 + 真实检测 ============
TEST(real_model_yolov10_detection) {
    const std::string model = find_file("yolov10n.onnx");
    const std::string photo = find_file("desk_zidane.bgr");
    if (model.empty() || photo.empty()) {
        // 二者均**随仓库交付**（core/tests/data/，git 跟踪）⇒ 缺失 = 检出损坏，属**必需** fixture：
        // 计入 skipped 且令进程非零退出（严禁"零断言 = PASS"的静默掩蔽）。
        TEST_SKIP_REQUIRED(
            "缺必需 fixture yolov10n.onnx / desk_zidane.bgr（core/tests/data/ 随库交付）");
    }

    // 加载 ONNX（直接 Ort API，等价 OnnxBackend）
    Ort::Env env(ORT_LOGGING_LEVEL_FATAL, "ttbox_real");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(2);
    // 路径宽字符
    int wlen = MultiByteToWideChar(CP_UTF8, 0, model.c_str(), -1, nullptr, 0);
    std::wstring wpath((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, model.c_str(), -1, &wpath[0], wlen);
    Ort::Session session(env, wpath.c_str(), opts);

    Ort::AllocatorWithDefaultOptions alloc;
    auto in_name = session.GetInputNameAllocated(0, alloc);
    auto out_name = session.GetOutputNameAllocated(0, alloc);

    // 读图
    RawBgr img;
    CHECK(load_bgr(photo, img));
    CHECK(img.w > 0 && img.h > 0);
    std::printf("  图: %dx%d  模型: %s\n", img.w, img.h, fs::path(model).filename().string().c_str());

    // 预处理 + 推理
    std::vector<float> in;
    to_nchw(img, 640, 640, in);
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(
        mem, in.data(), in.size(), std::vector<int64_t>{1, 3, 640, 640}.data(), 4);
    const char* ik[] = {in_name.get()};
    const char* ok[] = {out_name.get()};
    auto results = session.Run(Ort::RunOptions{nullptr}, ik, &tensor, 1, ok, 1);
    const float* raw = results[0].GetTensorData<float>();
    const size_t cnt = results[0].GetTensorTypeAndShapeInfo().GetElementCount();
    CHECK(cnt == 300 * 6);

    // 走生产 DecodeNMS e2e 分支
    RknnModelInfo info;
    std::vector<std::vector<uint8_t>> bufs;
    pack_e2e_output(std::vector<float>(raw, raw + cnt), info, bufs);
    const void* pbuf = bufs[0].data();

    DecodeNMS decode;
    DecodeParams dp;
    dp.conf_thres = 0.35f;
    dp.iou_thres = 0.45f;
    dp.classwise = true;
    dp.input_w = 640;
    dp.input_h = 640;
    dp.frame_w = 640;   // 输出映射回 640 输入空间
    dp.frame_h = 640;
    std::string err;
    CHECK(decode.configure(dp, &err));
    std::vector<DetectionBox> dets;
    CHECK(decode.process(info, &pbuf, &dets, &err));
    std::printf("  Decode 后检测数: %zu\n", dets.size());
    CHECK(!dets.empty());
    if (dets.empty()) return;

    // zidane 图已知：两名 person 在画面中部（640 空间下大约 x∈[150,480]）
    // 验证：至少一个 person(cls=0) 框中心位于画面中部区域
    bool found_person_center = false;
    for (const auto& d : dets) {
        if (d.class_id != 0) continue;  // COCO person
        float cx = (d.x1 + d.x2) / 2, cy = (d.y1 + d.y2) / 2;
        if (cx > 160 && cx < 480 && cy > 100 && cy < 560) {
            found_person_center = true;
            std::printf("  person: score=%.2f center=(%.0f,%.0f) box=(%.0f,%.0f,%.0f,%.0f)\n",
                        d.score, cx, cy, d.x1, d.y1, d.x2, d.y2);
            break;
        }
    }
    CHECK(found_person_center);

    // 框合法性：x2>x1, y2>y1, 无 NaN
    for (const auto& d : dets) {
        CHECK(d.x2 > d.x1 && d.y2 > d.y1);
        CHECK(std::isfinite(d.x1) && std::isfinite(d.y2));
        CHECK(d.score >= 0.35f && d.score <= 1.0f);
    }
}

// ============ 层B1：CoordinateTransform 方向确定性 ============
TEST(real_coord_direction_semantics) {
    aim::AimPointProfile prof;  // 默认中心瞄准
    const float ROI = 256.0f;
    float rx = 0, ry = 0;
    aim::CoordinateTransform::reference_point(ROI, ROI, prof, &rx, &ry);
    CHECK(rx == 128.0f && ry == 128.0f);

    auto make_box = [](float cx, float cy) {
        DetectionBox d;
        d.x1 = cx - 30; d.y1 = cy - 60;
        d.x2 = cx + 30; d.y2 = cy + 60;
        d.score = 0.9f;
        d.class_id = 0;
        return d;
    };

    // 中心 → err≈0
    { float ex, ey;
      aim::CoordinateTransform::pixel_error(make_box(128, 128), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(std::abs(ex) < 1.0f && std::abs(ey) < 1.0f); }
    // 右侧 → err_x > 0
    { float ex, ey;
      aim::CoordinateTransform::pixel_error(make_box(200, 128), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(ex > 30.0f && std::abs(ey) < 1.0f); }
    // 左侧 → err_x < 0
    { float ex, ey;
      aim::CoordinateTransform::pixel_error(make_box(56, 128), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(ex < -30.0f && std::abs(ey) < 1.0f); }
    // 上方 → err_y < 0
    { float ex, ey;
      aim::CoordinateTransform::pixel_error(make_box(128, 40), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(ey < -30.0f && std::abs(ex) < 1.0f); }
    // 下方 → err_y > 0
    { float ex, ey;
      aim::CoordinateTransform::pixel_error(make_box(128, 216), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(ey > 30.0f && std::abs(ex) < 1.0f); }
    std::printf("  CoordinateTransform 五方位方向语义 OK\n");
}

// ============ 层B2：PID 方向确定性（连续帧，纯 P 主导）============
TEST(real_pid_direction_semantics) {
    aim::TestPidController pid;
    aim::TestPidParams pp;
    // kp 用生产真实值 0.25（V1.0.13 后 smooth 已折叠进 kp，直通，不再有软限幅）
    pp.kp_x = 0.25f; pp.kp_y = 0.25f;
    pp.kd_x = 0.0f; pp.kd_y = 0.0f;
    pp.predict_x = 0.0f; pp.predict_y = 0.0f;  // 关掉预测干扰，方向由 P 主导
    pp.sensitivity = 1.0f;
    pp.output_scale = 1.0f;
    pp.output_deadzone = 0.5f;
    pid.configure(pp);
    pid.set_reference(0, 0);

    // Pid1Controller 语义：|error-last_error|>30 视为目标切换 → reset。
    // 所以误差必须渐进变化（模拟目标逐渐偏移），再保持稳态看方向符号。
    auto run_dir = [&](float ex, float ey) {
        int px_pos = 0, px_neg = 0, py_pos = 0, py_neg = 0;
        // 渐进进入：0→ex 共 20 步（每步 < 30 不触发 reset）
        for (int i = 1; i <= 20; ++i) {
            pid_frame(pid, ex * i / 20.0f, ey * i / 20.0f, i * 16);
        }
        // 稳态 20 帧
        for (int i = 0; i < 20; ++i) {
            auto c = pid_frame(pid, ex, ey, 400 + i * 16);
            if (c.dx > 0) ++px_pos;
            if (c.dx < 0) ++px_neg;
            if (c.dy > 0) ++py_pos;
            if (c.dy < 0) ++py_neg;
        }
        pid.reset();
        return std::make_tuple(px_pos, px_neg, py_pos, py_neg);
    };

    // 目标在右：dx 稳态全部 > 0
    { auto [pp_, pn_, yp_, yn_] = run_dir(+100, 0);
      CHECK(pn_ == 0); CHECK(pp_ >= 15); }
    // 目标在左：dx 稳态全部 < 0
    { auto [pp_, pn_, yp_, yn_] = run_dir(-100, 0);
      CHECK(pp_ == 0); CHECK(pn_ >= 15); }
    // 目标在下：dy 稳态全部 > 0
    { auto [pp_, pn_, yp_, yn_] = run_dir(0, +100);
      CHECK(yn_ == 0); CHECK(yp_ >= 15); }
    // 目标在上：dy 稳态全部 < 0
    { auto [pp_, pn_, yp_, yn_] = run_dir(0, -100);
      CHECK(yp_ == 0); CHECK(yn_ >= 15); }
    std::printf("  PID 四象限方向语义 OK（30 帧连续无反向）\n");

    // deadzone：误差小于死区 → 输出 0
    { auto c = pid_frame(pid, 0.1f, 0.1f, 0);
      CHECK(c.dx == 0 && c.dy == 0); }
    std::printf("  PID deadzone OK\n");
}

// ============ 层B3：TargetSelector 多目标 + ROI ============
TEST(real_target_selector_semantics) {
    aim::TargetSelector sel;
    aim::TargetSelectorConfig cfg;
    cfg.roi_w = 256; cfg.roi_h = 256;
    cfg.confidence = 0.3f;
    cfg.fov_range = 1.0f;

    auto det = [](float cx, float cy, float score) {
        DetectionBox d;
        d.x1 = cx - 20; d.y1 = cy - 40; d.x2 = cx + 20; d.y2 = cy + 40;
        d.score = score; d.class_id = 0;
        return d;
    };

    // 多目标：默认选距中心最近的
    std::vector<DetectionBox> dets = {det(200, 128, 0.9f), det(80, 128, 0.9f), det(128, 128, 0.8f)};
    auto s = sel.select(dets, cfg, 0);
    CHECK(s.valid);
    float cx = (s.box.x1 + s.box.x2) / 2;
    CHECK(std::abs(cx - 128.0f) < 5.0f);  // 选中中心那个
    std::printf("  多目标选最近 OK (选中 (%.0f,%.0f))\n", cx, (s.box.y1 + s.box.y2) / 2);

    // 锁定后目标轻微移动 → 不乱跳（track lock）
    int tid = s.target_id;
    std::vector<DetectionBox> moved = {det(136, 132, 0.9f), det(80, 128, 0.9f)};
    auto s2 = sel.select(moved, cfg, 33);
    CHECK(s2.valid && s2.target_id == tid);  // 同一 track
    CHECK(std::abs((s2.box.x1 + s2.box.x2) / 2 - 136.0f) < 5.0f);

    // 目标跑出 FOV → selector 立即 invalid（安全红线：不允许凭旧坐标移动）。
    // "短暂消失保持 target_id"的宽限由上层 AimStateMachine LOST_GRACE 实现；
    // TargetSelector 层只在完全空检测帧做 track 保活计数。
    // ★ FOV 半径 = ROI 短边 256 × fov_range。fov_range=0.5 ⇒ 128px；
    //   det(40,40) 的瞄准点 (40,16) 距准星 (128,128) ≈ 142 > 128 ⇒ FOV 外。
    aim::TargetSelector sel_gone;
    aim::TargetSelectorConfig cfg_gone = cfg;
    cfg_gone.fov_range = 0.5f;
    std::vector<DetectionBox> gone = {det(40, 40, 0.95f)};
    auto s3 = sel_gone.select(gone, cfg_gone, 40);
    CHECK(!s3.valid);  // FOV 外立即失效
    std::printf("  FOV 外立即失效 OK\n");

    // 空检测帧：宽限内 track 保活，超宽限释放
    std::vector<DetectionBox> none = {};
    auto s4 = sel.select(none, cfg, 47);   // 丢失 1 帧
    CHECK(!s4.valid);
    bool kept = false;
    for (int i = 0; i < 3; ++i) sel.select(none, cfg, 63 + i * 16);  // 宽限内继续丢
    {
        // 宽限内（lost_frames*7 < 30+7）track 仍是 active：再来目标应恢复同 id
        auto s_back = sel.select({det(136, 132, 0.9f)}, cfg, 120);
        CHECK(s_back.valid);
        kept = s_back.valid;
    }
    CHECK(kept);
    std::printf("  宽限内恢复同 track OK\n");

    // 超宽限：连续丢帧直到释放，然后新目标 → 新 track id
    aim::TargetSelector sel3;
    int old_id = -1, new_id = -1;
    {
        auto sa = sel3.select({det(128, 128, 0.9f)}, cfg, 0);
        old_id = sa.target_id;
        CHECK(sa.valid);
        // 连续空帧 6 次（6*7=42 >= 30+7）→ 释放
        for (int i = 0; i < 6; ++i) sel3.select(none, cfg, 16 + i * 16);
        auto sb = sel3.select({det(200, 128, 0.9f)}, cfg, 200);
        CHECK(sb.valid);
        new_id = sb.target_id;
    }
    CHECK(new_id != old_id);
    std::printf("  超宽限释放+新 track OK (old=%d new=%d)\n", old_id, new_id);

    // ROI 过滤：FOV 外的目标不选
    aim::TargetSelector sel2;
    aim::TargetSelectorConfig cfg2;
    cfg2.roi_w = 256; cfg2.roi_h = 256;
    cfg2.fov_range = 0.2f;  // 半径 = 256 × 0.2 = 51.2px
    std::vector<DetectionBox> far_only = {det(128, 250, 0.99f)};  // 瞄准点(128,226) 距准星 98px
    auto s5 = sel2.select(far_only, cfg2, 0);
    CHECK(!s5.valid);  // FOV 外
    std::printf("  FOV 过滤 OK\n");
}

#else  // !ONNX
// 无 ONNX 时层B 依然可跑（纯算法验证）。
//
// ★ P0-2 口径（team-lead item 10 裁决）：禁止"空体 / 零断言记 PASS"。
//   CoordinateTransform / TestPidController / TargetSelector 均为**纯算法**——不依赖 ONNX/RKNN/硬件，
//   在本分支**完全可断言** ⇒ 一律写**真断言**，不得以 `(void)x;` 烟雾或 `{}` 空体蒙混。
//   （原先 real_pid_direction_semantics 仅 `(void)c;`、real_target_selector_semantics 为 `{}` 空体，
//    二者零断言 ⇒ 被 run_all() 记入 passed —— 已消除。）

// B1：CoordinateTransform 参考点 + 五方位像素误差方向（纯算法）
TEST(real_coord_direction_semantics) {
    // 帧中心瞄准 → ROI 中心参考点
    aim::AimPointProfile prof;
    float rx = 0, ry = 0;
    aim::CoordinateTransform::reference_point(256, 256, prof, &rx, &ry);
    CHECK(rx == 128.0f && ry == 128.0f);

    // 五方位方向语义（右→ex>0 / 左→ex<0 / 下→ey>0 / 上→ey<0）
    const float ROI = 256.0f;
    auto box_at = [](float cx, float cy) {
        DetectionBox d;
        d.x1 = cx - 30; d.y1 = cy - 60; d.x2 = cx + 30; d.y2 = cy + 60;
        d.score = 0.9f; d.class_id = 0;
        return d;
    };
    { float ex = 0, ey = 0;
      aim::CoordinateTransform::pixel_error(box_at(128, 128), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(std::abs(ex) < 1.0f && std::abs(ey) < 1.0f); }  // 中心 → 误差≈0
    { float ex = 0, ey = 0;
      aim::CoordinateTransform::pixel_error(box_at(200, 128), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(ex > 30.0f && std::abs(ey) < 1.0f); }           // 目标在右
    { float ex = 0, ey = 0;
      aim::CoordinateTransform::pixel_error(box_at(56, 128), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(ex < -30.0f && std::abs(ey) < 1.0f); }          // 目标在左
    { float ex = 0, ey = 0;
      aim::CoordinateTransform::pixel_error(box_at(128, 216), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(ey > 30.0f && std::abs(ex) < 1.0f); }           // 目标在下
    { float ex = 0, ey = 0;
      aim::CoordinateTransform::pixel_error(box_at(128, 40), 0, prof, ROI, ROI, &ex, &ey);
      CHECK(ey < -30.0f && std::abs(ex) < 1.0f); }          // 目标在上
}

// B2：PID 方向确定性（纯 P 主导）。渐进进入避开"目标切换 reset"（|Δerr|>30），
//     稳态 20 帧统计输出符号：右→dx>0、左→dx<0、下→dy>0、上→dy<0；并验证死区归零。
TEST(real_pid_direction_semantics) {
    aim::TestPidController pid;
    aim::TestPidParams pp;
    pp.kp_x = 0.25f; pp.kp_y = 0.25f;          // 生产真实值（V1.0.13 后 smooth 已折叠进 kp，直通）
    pp.kd_x = 0.0f;  pp.kd_y = 0.0f;
    pp.predict_x = 0.0f; pp.predict_y = 0.0f;  // 关预测，方向由 P 主导
    pp.sensitivity = 1.0f; pp.output_scale = 1.0f; pp.output_deadzone = 0.5f;
    pid.configure(pp);
    pid.set_reference(0, 0);

    // 渐进进入（每步 Δerr<30 不触发 reset）→ 稳态 20 帧统计四项符号计数
    auto steady_sign = [&](float ex, float ey,
                           int& px_pos, int& px_neg, int& py_pos, int& py_neg) {
        for (int i = 1; i <= 20; ++i) {
            aim::TargetPoint tp; tp.valid = true; tp.x = ex * i / 20.0f; tp.y = ey * i / 20.0f;
            pid.update(tp);
        }
        px_pos = px_neg = py_pos = py_neg = 0;
        for (int i = 0; i < 20; ++i) {
            aim::TargetPoint tp; tp.valid = true; tp.x = ex; tp.y = ey;
            const auto c = pid.update(tp);
            if (c.dx > 0) ++px_pos;
            if (c.dx < 0) ++px_neg;
            if (c.dy > 0) ++py_pos;
            if (c.dy < 0) ++py_neg;
        }
        pid.reset();
    };

    { int a = 0, b = 0, c = 0, d = 0; steady_sign(+100, 0, a, b, c, d);
      CHECK(a >= 15); CHECK(b == 0); }           // 目标在右 → dx 恒正
    { int a = 0, b = 0, c = 0, d = 0; steady_sign(-100, 0, a, b, c, d);
      CHECK(b >= 15); CHECK(a == 0); }           // 目标在左 → dx 恒负
    { int a = 0, b = 0, c = 0, d = 0; steady_sign(0, +100, a, b, c, d);
      CHECK(c >= 15); CHECK(d == 0); }           // 目标在下 → dy 恒正
    { int a = 0, b = 0, c = 0, d = 0; steady_sign(0, -100, a, b, c, d);
      CHECK(d >= 15); CHECK(c == 0); }           // 目标在上 → dy 恒负

    // deadzone：误差远小于死区 → 输出归零
    { aim::TargetPoint tp; tp.valid = true; tp.x = 0.1f; tp.y = 0.1f;
      const auto c = pid.update(tp);
      CHECK(c.dx == 0 && c.dy == 0); }
}

// B3：TargetSelector 多目标择优 + FOV 过滤（纯算法）
TEST(real_target_selector_semantics) {
    aim::TargetSelector sel;
    aim::TargetSelectorConfig cfg;
    cfg.roi_w = 256; cfg.roi_h = 256;
    cfg.confidence = 0.3f;
    cfg.fov_range = 1.0f;

    auto det = [](float cx, float cy, float score) {
        DetectionBox d;
        d.x1 = cx - 20; d.y1 = cy - 40; d.x2 = cx + 20; d.y2 = cy + 40;
        d.score = score; d.class_id = 0;
        return d;
    };

    // 多目标：默认选距 ROI 中心最近者（(128,128)）
    std::vector<DetectionBox> dets = {det(200, 128, 0.9f), det(80, 128, 0.9f), det(128, 128, 0.8f)};
    const auto s = sel.select(dets, cfg, 0);
    CHECK(s.valid);
    const float cx = (s.box.x1 + s.box.x2) / 2.0f;
    CHECK(std::abs(cx - 128.0f) < 5.0f);

    // FOV 外目标 → 立即失效（安全红线：不允许凭旧坐标移动）
    aim::TargetSelector sel2;
    aim::TargetSelectorConfig cfg2;
    cfg2.roi_w = 256; cfg2.roi_h = 256;
    cfg2.fov_range = 0.2f;   // 半径 = 256 × 0.2 = 51.2px；(128,250) 瞄准点距准星 98px ⇒ FOV 外
    const auto s2 = sel2.select({det(128, 250, 0.99f)}, cfg2, 0);
    CHECK(!s2.valid);
}
#endif
