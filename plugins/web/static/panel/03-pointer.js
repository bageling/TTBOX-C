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
  setValue("controller_kp", controller.kp);
  setValue("controller_kd", controller.kd);
  setValue("controller_predict", controller.predict);
  setValue("controller_predict_y", controller.predict_y);
  setValue("controller_rate", controller.rate);
  setValue("controller_output_deadzone", controller.output_deadzone);
  setCheckbox("controller_pull_curve_enabled", controller.pull_curve_enabled);
  setValue("controller_pull_curve_strength", controller.pull_curve_strength);
  setValue("controller_pull_curve_min_distance", controller.pull_curve_min_distance);
  setValue("controller_selector_lost_grace_ms", controller.selector_lost_grace_ms);
}

