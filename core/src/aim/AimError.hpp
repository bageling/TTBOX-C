// AimError.hpp — 目标任务到画面中心的误差计算。
#pragma once
#include "pipeline/AimTargetTask.hpp"
namespace ttbox::core::aim {
struct AimError { float x=0.0f; float y=0.0f; };
// ★ 2026-10-04 代码体检：本函数**生产代码零引用**（全仓只有这一定义处），
//   两个测试也没用它。属预留便捷函数，保留不删（删除是业主的决定）。
//   记在这里是为了下次「为什么找不到调用点」时不必再查一遍。
inline AimError error_from_center(const AimTargetTask& task) {
    const float cx = static_cast<float>(task.frame_width) * 0.5f;
    const float cy = static_cast<float>(task.frame_height) * 0.5f;
    return {task.aim_point.x - cx, task.aim_point.y - cy};
}
}
