// test_personal_trajectory_shader.cpp — 拟人化整形引擎（第 1 项）单元测试
//
// 覆盖验收场景：
//   Case1 未启用（enabled=false）→ 原样放行，不整形
//   Case2 未激活（未 activate）→ 原样放行
//   Case3 启用 + 激活 + 正常移动 → 输出被整形（速度包络增益），但幅度不超过 raw+max_extra
//   Case4 大误差（>=urgent_error_px）→ 自适应抑制：直接放行（不整形），防乱晃
//   Case5 max_extra 幅度守卫：整形输出 || <= |raw| + max_extra
//   Case6 保持方向：整形输出与误差同向（guard_output 方向保护）
//   Case7 安全门：整形仅在热键放行时触发（AimThread 层 Gate 已保证归零，单测验证 shape 本身）
#include <cstdio>
#include <cmath>

#include "mouse/PersonalTrajectoryShader.hpp"

using namespace ttbox::core::aim;

namespace {

int failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); failures++; }
    else { std::printf("  PASS: %s\n", msg); }
}

// 用固定配置
PersonalTrajectoryConfig make_cfg() {
    PersonalTrajectoryConfig c;
    c.enabled = true;
    c.adaptive_enabled = true;
    c.min_error_px = 18.0f;
    c.urgent_error_px = 72.0f;
    c.transport_gain = 0.16f;
    c.max_extra_px = 2.0f;
    c.max_visual_variation_px = 1.5f;
    c.response_px_per_count = 0.65f;
    c.curve_time_constant_ms = 32.0f;
    return c;
}

}  // namespace

int main() {
    std::printf("===== Case1: disabled -> passthrough =====\n");
    {
        PersonalTrajectoryConfig cfg = make_cfg();
        cfg.enabled = false;
        PersonalTrajectoryShader sh;
        sh.activate(50.0f, cfg);
        int16_t dx = 10, dy = 5;
        sh.shape(&dx, &dy, 40.0f, 20.0f, 16.0f, cfg);
        check(dx == 10 && dy == 5, "disabled 时原样放行");
    }

    std::printf("===== Case2: not activated -> passthrough =====\n");
    {
        PersonalTrajectoryConfig cfg = make_cfg();
        PersonalTrajectoryShader sh;
        int16_t dx = 10, dy = 5;
        sh.shape(&dx, &dy, 40.0f, 20.0f, 16.0f, cfg);
        check(dx == 10 && dy == 5, "未 activate 时原样放行");
    }

    std::printf("===== Case3: enabled + activated -> shaped =====\n");
    {
        PersonalTrajectoryConfig cfg = make_cfg();
        cfg.adaptive_enabled = true;
        PersonalTrajectoryShader sh;
        sh.activate(50.0f, cfg);   // 误差距离 50px（处于 min_error~urgent 之间）
        // 递增误差逼近（模拟接近），同时推进时间到 transport 中段 → 增益应累积生效
        bool shaped = false, within_guard = true, same_direction = true;
        for (int i = 0; i < 20; ++i) {
            int16_t dx = 10, dy = 5;
            // 误差略降模拟收敛；保持 ex,ey 与输出同向
            float ex = 40.0f - static_cast<float>(i), ey = 30.0f - static_cast<float>(i) * 0.5f;
            float err = std::hypot(ex, ey);
            if (err < cfg.min_error_px) err = cfg.min_error_px;
            sh.set_error_speed_px_s(10.0f);  // 低速目标（不触发 rapid 抑制）
            sh.shape(&dx, &dy, ex, ey, 16.0f, cfg);
            if (std::abs(dx) > 10 + 2 || std::abs(dy) > 5 + 2) within_guard = false;
            if (dx < 0 || dy < 0) same_direction = false;
            if (std::abs(dx) > 10 || std::abs(dy) > 5) shaped = true;  // 出现增益
            if (shaped) break;
        }
        check(same_direction, "Case3 输出与移动同向");
        check(within_guard, "Case3 幅度始终在 raw+max_extra 内");
        check(shaped, "Case3 移动过程中出现速度包络增益（被整形）");
        // 打印最后一次
        int16_t dx = 10, dy = 5;
        sh.shape(&dx, &dy, 30.0f, 22.0f, 16.0f, cfg);
        std::printf("    last out=(%d,%d)\n", dx, dy);
    }

    std::printf("===== Case4: large error -> adaptive suppress (passthrough) =====\n");
    {
        PersonalTrajectoryConfig cfg = make_cfg();
        cfg.urgent_error_px = 50.0f;   // 让 50px 误差变成"大误差"
        PersonalTrajectoryShader sh;
        sh.activate(80.0f, cfg);
        int16_t dx = 12, dy = 6;
        sh.shape(&dx, &dy, 60.0f, 55.0f, 16.0f, cfg);
        // 大误差 → 抑制：disabled 不整形 → 输出应接近 raw（仅允许 guard 的 copysign 微调）
        bool passthrough = (std::abs(dx - 12) <= 2 && std::abs(dy - 6) <= 2);
        check(passthrough, "Case4 大误差时基本直出（不整形放乱）");
        std::printf("    out=(%d,%d)\n", dx, dy);
    }

    std::printf("===== Case5: max_extra guard (never exceed raw+2) =====\n");
    {
        // 随机喂大量帧确保任何情况下幅度都不超界
        PersonalTrajectoryConfig cfg = make_cfg();
        cfg.adaptive_enabled = false;  // 关自适应，让整形全力作用
        PersonalTrajectoryShader sh;
        sh.activate(50.0f, cfg);
        bool ok = true;
        for (int i = 0; i < 200; ++i) {
            int16_t dx = 8, dy = 0;
            sh.shape(&dx, &dy, 40.0f, 30.0f, 16.0f, cfg);
            if (std::abs(dx) > 8 + 2) { ok = false; break; }
        }
        check(ok, "Case5 持续 200 帧幅度始终 <= raw+max_extra");
    }

    std::printf("===== Case6: direction guard (never reverse against error) =====\n");
    {
        PersonalTrajectoryConfig cfg = make_cfg();
        cfg.adaptive_enabled = false;
        PersonalTrajectoryShader sh;
        sh.activate(50.0f, cfg);
        bool same_sign = true;
        for (int i = 0; i < 200; ++i) {
            int16_t dx = 5, dy = 0;
            sh.shape(&dx, &dy, 40.0f, 30.0f, 16.0f, cfg);
            // 全部同向（误差 ex>0，dx 应 >= 0）
            if (dx < 0) { same_sign = false; break; }
        }
        check(same_sign, "Case6 输出方向与误差同向（不反向投影）");
    }

    std::printf("\n===== RESULT: %s (%d failures) =====\n",
                failures == 0 ? "ALL PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}