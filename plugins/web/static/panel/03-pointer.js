// ==========================================================================
// 03-pointer.js —— 面板「移动控制」（section#control-page）
// ==========================================================================
// ★ 本页签的控件共 28 个（实测自 index.html 的 section#control-page）。
//   专属逻辑只有 setMovementControlDefaultsToForm。
//
// ★ 加新功能放哪：
//   · 移动控制页专属 → 本文件
//   · 配置项收发 → 10-flow.js
// ==========================================================================
function setMovementControlDefaultsToForm() {
  const controller = MOVEMENT_CONTROL_DEFAULTS.controller;
  setValue("sens", MOVEMENT_CONTROL_DEFAULTS.sens);
  // ★★★ V1.0.41：pid1 删除，换 SmoothAimController 4 参数。
  setValue("controller_aim_gain", controller.aim_gain);
  setValue("controller_aim_alpha", controller.aim_alpha);
  setValue("controller_aim_max_move", controller.aim_max_move);
  setValue("controller_aim_deadzone_ratio", controller.aim_deadzone_ratio);
  setValue("controller_output_deadzone", controller.output_deadzone);
  setCheckbox("controller_pull_curve_enabled", controller.pull_curve_enabled);
  setValue("controller_pull_curve_strength", controller.pull_curve_strength);
  setValue("controller_pull_curve_min_distance", controller.pull_curve_min_distance);
  setValue("controller_selector_lost_grace_ms", controller.selector_lost_grace_ms);
}

