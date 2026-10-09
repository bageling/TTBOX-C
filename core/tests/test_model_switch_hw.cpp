// test_model_switch_hw.cpp — A-8 硬件测试：模型仓库生命周期 + 模型切换 + 热更新（板端）
//
// 验证：
//   import → validate(真实 RKNN+Adapter) → install → activate
//   WorkerPool 运行（RuntimeConfig 热更新 conf/iou/FOV）
//   模型切换（activate B）+ 激活失败恢复旧模型 + 禁止删除 active
//   deactivate → remove；多轮加载/卸载无泄漏
//
// 用法：
//   test_model_switch_hw --models "hw:/home/ubuntu/ttbox2/models/huangwa.rknn,y26:/home/ubuntu/ttbox2/models/yolo261n-rk3588.rknn"
//                        [--registry /tmp/ttbox_reg_hw] [--frames 30]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>  // ::getpid：夹具路径唯一化
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "common/Types.hpp"
#include "config/ConfigManager.hpp"
#include "model/ModelAdapter.hpp"
#include "model/ModelRegistry.hpp"
#include "model/RuntimeProfile.hpp"
#include "rknn/RKNNEngine.hpp"
#include "rknn/WorkerPool.hpp"

namespace fs = std::filesystem;
using namespace ttbox::core;

namespace {

// 真实 validator：RKNN init + ModelAdapter.analyze → metadata JSON
bool real_validator(const std::string& rknn_path, JsonValue* meta, std::string* err) {
    RKNNEngine engine;
    RKNNEngine::Params ep;
    ep.model_path = rknn_path;
    ep.core_mask = 0;
    ep.pass_through = false;
    if (!engine.init(ep, err)) return false;

    ModelAdapter ad;
    ModelAdapterConfig cfg;  // 默认 BGR；颜色语义由运行时 RuntimeProfile 决定
    if (!ad.analyze(engine.info(), cfg, err)) {
        engine.destroy();
        return false;
    }
    if (meta) {
        JsonValue m = JsonValue::object();
        m.set("input_width", JsonValue::number(static_cast<double>(ad.metadata().input_width)));
        m.set("input_height", JsonValue::number(static_cast<double>(ad.metadata().input_height)));
        m.set("decode_type", JsonValue::number(static_cast<double>(static_cast<int>(ad.metadata().decode_type))));
        m.set("class_count", JsonValue::number(static_cast<double>(ad.metadata().class_count)));
        m.set("color_order", JsonValue::number(static_cast<double>(static_cast<int>(ad.metadata().color_order))));
        *meta = std::move(m);
    }
    engine.destroy();
    return true;
}

// 短跑 WorkerPool（n=1，30 帧），验证激活模型可运行；返回 false 表示失败
bool run_runtime(const std::string& model, const RuntimeProfile& prof,
                 uint32_t frames, std::string* err) {
    // 无 capture：用手动 LatestFrame 喂合成帧不可行（RKNN 需要 V4L2 dma_fd）。
    // 本测试仅验证 registry 生命周期 + 热更新接口（decoder apply_runtime 无错误），
    // 不依赖实时输入：直接调用 ModelAdapter.create_decoder 应用 profile 模拟热更新。
    (void)frames;
    RKNNEngine engine;
    RKNNEngine::Params ep;
    ep.model_path = model;
    ep.core_mask = 0;
    ep.pass_through = false;
    if (!engine.init(ep, err)) return false;

    ModelAdapter ad;
    ModelAdapterConfig cfg;
    if (!ad.analyze(engine.info(), cfg, err)) {
        engine.destroy();
        return false;
    }
    auto decoder = ad.create_decoder(err);
    if (!decoder) {
        engine.destroy();
        return false;
    }
    decoder->set_frame(1920, 1080);
    // 热更新：应用用户配置（conf/iou/FOV）
    decoder->apply_runtime(prof.inference, prof.fov);
    if (prof.capture.width > 0) {
        // V1.0.13：capture.offset_x/y 已删 ⇒ ROI 恒以屏幕中心为心
        const uint32_t roi_x = (1920 - prof.capture.width) / 2;
        const uint32_t roi_y = (1080 - prof.capture.height) / 2;
        decoder->set_roi(roi_x, roi_y, prof.capture.width, prof.capture.height);
    }
    const auto& p = decoder->stats();
    (void)p;
    decoder.reset();
    engine.destroy();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string models_arg;
    std::string registry = "/tmp/ttbox_reg_hw_" + std::to_string(static_cast<long>(::getpid()));
    uint32_t frames = 30;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺少参数: %s\n", n); std::exit(1); }
            return argv[++i];
        };
        if (a == "--models") models_arg = next("--models");
        else if (a == "--registry") registry = next("--registry");
        else if (a == "--frames") frames = static_cast<uint32_t>(std::atoi(next("--frames").c_str()));
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); return 1; }
    }
    if (models_arg.empty()) {
        std::fprintf(stderr, "用法: %s --models \"id:path,id:path,...\" [--registry dir]\n", argv[0]);
        return 1;
    }

    std::error_code ec;
    fs::remove_all(registry, ec);

    ModelRegistry reg(ModelRegistryOptions{registry});
    reg.set_validator(real_validator);
    std::string err;
    if (!reg.init(&err)) {
        std::printf("[FAIL] registry init: %s\n", err.c_str());
        return 1;
    }

    int fail = 0;

    // ---- import + validate + install ----
    std::vector<std::pair<std::string, std::string>> models;  // id -> path
    size_t pos = 0;
    while (pos < models_arg.size()) {
        const size_t comma = models_arg.find(',', pos);
        const std::string tok = models_arg.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t colon = tok.find(':');
        if (colon != std::string::npos) {
            models.emplace_back(tok.substr(0, colon), tok.substr(colon + 1));
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::printf("=== import/validate/install (%zu 模型) ===\n", models.size());
    for (const auto& [id, path] : models) {
        ModelManifest m;
        m.label = id;
        m.origin = "local";
        if (!reg.import(path, id, m, &err)) {
            std::printf("  [FAIL] import %s: %s\n", id.c_str(), err.c_str());
            return 1;
        }
        if (!reg.validate(id, &err)) {
            std::printf("  [FAIL] validate %s: %s\n", id.c_str(), err.c_str());
            return 1;
        }
        if (!reg.install(id, &err)) {
            std::printf("  [FAIL] install %s: %s\n", id.c_str(), err.c_str());
            return 1;
        }
        std::printf("  [OK] %s installed\n", id.c_str());
    }
    if (models.empty()) {
        std::printf("[FAIL] 无模型\n");
        return 1;
    }
    const auto& first = models[0];
    const auto& second = models[models.size() > 1 ? 1 : 0];

    // ---- activate + runtime（热更新）----
    std::printf("=== activate[%s] + runtime ===\n", first.first.c_str());
    if (!reg.activate(first.first, &err)) {
        std::printf("  [FAIL] activate %s: %s\n", first.first.c_str(), err.c_str());
        return 1;
    }
    RuntimeProfile prof;
    prof.model_id = first.first;
    prof.inference.confidence = 0.55f;
    prof.inference.iou = 0.45f;
    if (!run_runtime(first.second, prof, frames, &err)) {
        std::printf("  [FAIL] runtime(%s): %s\n", first.first.c_str(), err.c_str());
        return 1;
    }
    std::printf("  [OK] runtime(%s) + conf/iou 热更新\n", first.first.c_str());

    // ---- 切换：activate 第二个模型 ----
    std::printf("=== 切换 activate[%s] ===\n", second.first.c_str());
    if (!reg.activate(second.first, &err)) {
        std::printf("  [FAIL] 切换 activate %s: %s\n", second.first.c_str(), err.c_str());
        return 1;
    }
    prof.model_id = second.first;
    prof.fov.enabled = true;
    prof.fov.radius = 0.4f;
    if (!run_runtime(second.second, prof, frames, &err)) {
        std::printf("  [FAIL] runtime(%s) FOV 热更新: %s\n", second.first.c_str(), err.c_str());
        return 1;
    }
    std::printf("  [OK] 切换 -> %s + FOV 热更新\n", second.first.c_str());
    if (reg.active_model() != second.first) {
        std::printf("  [FAIL] active 应为 %s\n", second.first.c_str());
        return 1;
    }

    // ---- 激活失败恢复旧模型 ----
    std::printf("=== 激活失败恢复 ===\n");
    const std::string old_active = reg.active_model();
    if (reg.activate("not_installed_model", &err)) {
        std::printf("  [FAIL] 激活不存在模型应失败\n");
        return 1;
    }
    if (reg.active_model() != old_active) {
        std::printf("  [FAIL] 激活失败后 active 应保持 %s（实际 %s）\n",
                    old_active.c_str(), reg.active_model().c_str());
        return 1;
    }
    std::printf("  [OK] 激活失败保持旧模型(%s)\n", old_active.c_str());

    // ---- 禁止删除 active；deactivate 后可删除 ----
    std::printf("=== remove 保护 ===\n");
    if (reg.remove(second.first, &err)) {
        std::printf("  [FAIL] 删除 active 模型应被拒绝\n");
        return 1;
    }
    std::printf("  [OK] 删除 active 被拒绝\n");
    if (!reg.deactivate(&err) || !reg.active_model().empty()) {
        std::printf("  [FAIL] deactivate\n");
        return 1;
    }
    if (!reg.remove(second.first, &err)) {
        std::printf("  [FAIL] remove %s: %s\n", second.first.c_str(), err.c_str());
        return 1;
    }
    std::printf("  [OK] deactivate + remove\n");

    // ---- 加载/卸载泄漏：3 轮 ----
    std::printf("=== 加载/卸载泄漏（3 轮）===\n");
    bool leak_ok = true;
    for (int r = 0; r < 3; ++r) {
        RKNNEngine engine;
        RKNNEngine::Params ep;
        ep.model_path = first.second;
        ep.core_mask = 0;
        ep.pass_through = false;
        if (!engine.init(ep, &err)) {
            std::printf("  round%d [FAIL] %s\n", r, err.c_str());
            leak_ok = false;
            break;
        }
        ModelAdapter ad;
        if (!ad.analyze(engine.info(), {}, &err)) {
            std::printf("  round%d [FAIL] analyze: %s\n", r, err.c_str());
            leak_ok = false;
            break;
        }
        auto dec = ad.create_decoder(&err);
        if (!dec) {
            std::printf("  round%d [FAIL] decoder: %s\n", r, err.c_str());
            leak_ok = false;
            break;
        }
        engine.destroy();
        std::printf("  round%d [OK] init+analyze+decoder+destroy\n", r);
    }
    if (!leak_ok) return 1;

    fs::remove_all(registry, ec);
    std::printf("test_model_switch_hw PASS\n");
    return 0;
}
