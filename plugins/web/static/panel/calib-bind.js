// calib-bind.js —— 自动标定的 5 个函数（2026-10-03 从 bindEvents 体内提升）
//
// ★ 它们原来定义在 bindEvents() 函数体**内部**，每次绑定时顺带重新定义。
//   拆bindEvents 时若不单独拎出来，会被关进某个 bind 函数里
//   （语法合法、所有测试都绿，但结构是错的）。
//
// 本文件装：syncConfigAfterCalibration / calibPhaseLabel /
//renderCalibration / scheduleCalibrationPolling / refreshCalibration
//
// 加载顺序：10-flow.js 之后、01-home.js 之前（都是顶层 function，会 hoist）。

// syncConfigAfterCalibration（2026-10-03 从 bindEvents 体内提升为顶层）
async function syncConfigAfterCalibration() {
  try {
    const cfg = await api("/api/config");
    if (!cfg) {
      return;
    }
    populateForm(cfg);
    setApplyStatus("ready", "已同步");
    showToast("标定已把 Kp / Kd / 预判写回面板");
  } catch {
    showToast("参数已写入 Core，但面板刷新失败，可手动刷新页面", true);
  }
}

// calibPhaseLabel（2026-10-03 从 bindEvents 体内提升为顶层）
function calibPhaseLabel(payload) {
  const phase = payload && payload.phase;
  if (phase === "done" || phase === "saved") {
    return "已保存";
  }
  return CALIB_PHASE_LABELS[phase] || phase || "未知";
}

// renderCalibration（2026-10-03 从 bindEvents 体内提升为顶层）
// renderCalibration（2026-10-03 从 bindEvents 体内提升为顶层；
//   同日按「刷什么」拆成 4 段，见下方 4 个子函数）
function renderCalibration(payload) {
  if (!payload) {
    return;
  }
  const runtime = payload.runtime || {};
  const saved = payload.calibration || {};
  const els = calibRenderElements();
  const { pill, bar, savedPill, savedSummary } = els;
  if (!pill || !bar) {
    return;
  }
  const running = !!runtime.running;
  const state2 = runtime.state || "idle";
  // ★ 不能只用「线程存活」当禁用判据：worker 因异常穿透而死时 running=false 但
  //   state 停在非终态（如 sampling_x），此时取消按钮曾被禁用 ⇒ 用户点不掉、也没人复位。
  //   判据改成「后端认为还在进行」：线程活着，或 state 停在非终态。
  const terminal = ["completed", "failed", "cancelled"].includes(state2);
  const active = running || (state2 !== "idle" && !terminal);
  calibRenderProgress(els, runtime, running, active, state2);
  calibRenderTrace(els, runtime, running, state2);
  calibRenderSaved(els, saved);
  calibHandleTerminalState(state2, running);
  calibRenderAxisFits(runtime);
  scheduleCalibrationPolling(running);
}

// ---- renderCalibration 的元素查找（每次渲染都重新查，元素可能被重建）----
function calibRenderElements() {
  return {
    pill: $("calibStatusPill"),
    bar: $("calibProgressBar"),
    phaseText: $("calibPhaseText"),
    traceLog: $("calibTraceLog"),
    startButton: $("calibStartButton"),
    cancelButton: $("calibCancelButton"),
    savedPill: $("calibSavedPill"),
    savedSummary: $("calibSavedSummary"),
  };
}

// ---- 段A：状态胶囊 / 进度条 / 起停按钮 / 阶段文字 ----
function calibRenderProgress(els, runtime, running, active, state2) {
  const { pill, bar, phaseText, startButton, cancelButton } = els;
  if (pill) {
    pill.textContent = running ? `运行中 ${Math.round((runtime.progress || 0) * 100)}%` : calibPhaseLabel(runtime);
  }
  if (startButton) {
    startButton.disabled = active;
  }
  if (cancelButton) {
    cancelButton.disabled = !active;
  }
  if (bar) {
    bar.style.width = `${Math.round((runtime.progress || 0) * 100)}%`;
  }
  if (phaseText) {
    const parts = [calibPhaseLabel(runtime)];
    if (running && runtime.total_rounds) {
      parts.push(`第 ${runtime.round || 0}/${runtime.total_rounds} 轮（${runtime.current_axis ? runtime.current_axis.toUpperCase() : ""}轴）`);
    }
    if (runtime.valid_sample_count) {
      parts.push(`有效样本 ${runtime.valid_sample_count}${runtime.dropped_sample_count ? `（丢弃 ${runtime.dropped_sample_count}）` : ""}`);
    }
    if (!running && runtime.reason && runtime.reason !== "not_running") {
      parts.push(String(runtime.reason));
    }
    phaseText.textContent = parts.join(" · ");
  }
}

// ---- 段B：采样轨迹滚动日志（最多 60 行）----
function calibRenderTrace(els, runtime, running, state2) {
  const { traceLog } = els;
  if (traceLog) {
    const line = [
      new Date().toLocaleTimeString(),
      `phase=${runtime.phase || "-"}`,
      `state=${state2}`,
      running ? `round=${runtime.round || 0}/${runtime.total_rounds || 0}` : "",
      running ? `amp=${runtime.amplitude_px != null ? runtime.amplitude_px.toFixed(1) : "-"}px` : "",
      `valid=${runtime.valid_sample_count || 0}`,
      runtime.reason && runtime.reason !== "not_running" ? `reason=${runtime.reason}` : "",
    ].filter(Boolean).join("  ");
    const last = calibTraceLines[calibTraceLines.length - 1];
    if (line !== last) {
      calibTraceLines.push(line);
      if (calibTraceLines.length > 60) {
        calibTraceLines.shift();
      }
    }
    traceLog.textContent = calibTraceLines.join("\n") || "—";
    traceLog.scrollTop = traceLog.scrollHeight;
  }
}

// ---- 段C：已标定摘要（有效则列数值，无效则如实报当前生效增益）----
function calibRenderSaved(els, saved) {
  const { savedPill, savedSummary } = els;
  if (savedPill) {
    savedPill.textContent = saved.valid ? "已标定" : "未标定";
  }
  if (savedSummary) {
    // null 一律显示「—」：接口对未知量给 null（不再拿 0.55 / 8.333 这类硬编码默认
    // 冒充"当前标定"，那既不是标定结果也不是生效值）。
    const num = (v, digits, unit) => (v == null || v === "" ? "—" : `${Number(v).toFixed(digits)}${unit}`);
    if (saved.valid) {
      const rows = [
        ["增益 X", num(saved.gain_x_px_per_count, 3, " px/count")],
        ["增益 Y", num(saved.gain_y_px_per_count, 3, " px/count")],
        ["响应延迟", num(saved.response_delay_ms, 1, " ms")],
        ["置信度", saved.confidence != null ? `${Math.round(Number(saved.confidence) * 100)}%` : "—"],
        ["标定时间", saved.calibrated_at || "—"],
        ["模型", saved.model_id || "—"],
      ];
      const pidSaved = saved.pid_params || {};
      if (pidSaved.kp != null) {
        rows.push(["推导 PID", `Kp ${Number(pidSaved.kp).toFixed(3)} · Kd ${Number(pidSaved.kd ?? 0).toFixed(3)} · 预判 ${Number(pidSaved.predict ?? 0).toFixed(3)}`]);
      }
      savedSummary.innerHTML = rows.map(([k, v]) => `<div class="runtime-stat"><span>${escapeHtml(k)}</span><strong>${escapeHtml(v)}</strong></div>`).join("");
    } else {
      // 「未标定」≠「增益未知」：运行配置里的 gain 照样在生效（压枪都在用它），
      // 如实报出来，免得用户以为"没标定就等于没有增益"。
      const eff = saved.effective || {};
      const lines = ["尚未保存标定结果（未跑过自动标定，或已清除）。"];
      if (eff.gain_x_px_per_count != null) {
        lines.push(`当前运行配置里生效的增益：X ${num(eff.gain_x_px_per_count, 3, "")} / Y ${num(eff.gain_y_px_per_count, 3, "")} px/count。`);
      } else {
        lines.push("读不到当前生效增益（推理服务未运行）。");
      }
      savedSummary.textContent = lines.join("");
    }
  }
}

// ---- 段D：终态处理（停轮询 + 标定成功后热更新 kp/kd）+ 轴拟合结果 ----
function calibHandleTerminalState(state2, running) {
  // 终态后停止持续轮询（下次进入分区/点刷新再拉）
  if (["completed", "failed", "cancelled"].includes(state2) && !running) {
    state.calibKeepPolling = false;
    // ★ 标定成功写回了 kp/kd/predict_x，面板输入框必须跟着变 —— 否则用户看到的
    //   还是旧值，只能刷新页面（走 applyFullState → populateForm）才对得上。
    //   这里拉一次 /api/config 回填，等于"热更新"，不用刷新。
    if (state2 === "completed" && !state.calibConfigSynced) {
      state.calibConfigSynced = true;
      syncConfigAfterCalibration();
    }
  }
}

// ---- 轴拟合结果（运行中/结束时都能看，独立展示避免与轨迹日志互相污染）----
function calibRenderAxisFits(runtime) {
  const fitsLog = $("calibFitsLog");
  const fits = runtime.axis_fits || {};
  const fitRows = ["x", "y"].map((axis) => {
    const fit = fits[axis];
    if (!fit) {
      return null;
    }
    const status = fit.converged ? "✓" : `✗ ${fit.failure_reason || ""}`;
    return `${axis.toUpperCase()} 轴：gain=${Number(fit.gain_px_per_count || 0).toFixed(3)} px/count · 延迟=${Number(fit.response_delay_ms || 0).toFixed(1)} ms · 一致性=${Math.round((fit.consistency || 0) * 100)}% · 样本=${fit.sample_count || 0} ${status}`;
  }).filter(Boolean);
  if (fitsLog) {
    fitsLog.hidden = fitRows.length === 0;
    fitsLog.textContent = fitRows.join("\n");
  }
}

// scheduleCalibrationPolling（2026-10-03 从 bindEvents 体内提升为顶层）
function scheduleCalibrationPolling(running) {
  const shouldPoll = running || state.calibKeepPolling;
  if (shouldPoll && calibPollTimer == null) {
    calibPollTimer = setInterval(async () => {
      try {
        const payload = await api("/api/control/calibration");
        renderCalibration(payload);
      } catch {
        // 网络抖动：保持轮询，下一轮再试
      }
    }, 800);
  } else if (!shouldPoll && calibPollTimer != null) {
    clearInterval(calibPollTimer);
    calibPollTimer = null;
  }
}

// refreshCalibration（2026-10-03 从 bindEvents 体内提升为顶层）
async function refreshCalibration({ toast = false } = {}) {
  const payload = await api("/api/control/calibration");
  renderCalibration(payload);
  if (toast) {
    showToast("标定状态已刷新");
  }
  return payload;
}

// ---- 前置局部常量（2026-10-03 从 bindEvents 体内一起拎出来）----
// ★ 它们原来定义在 calibPhaseLabel 之前、和它同属一段局部区域。
//   拆函数时若只搬 function 不搬 const，calibPhaseLabel 一调用就报
//   ReferenceError ——而 node --check 与全部测试都发现不了。

const CALIB_PHASE_LABELS = {
  idle: "未运行",
  preparing: "准备标定环境",
  stabilize_x: "等待目标稳定（X 轴）",
  sampling_x: "注入偏置采样（X 轴）",
  analyzing_x: "拟合 X 轴增益",
  stabilize_y: "等待目标稳定（Y 轴）",
  sampling_y: "注入偏置采样（Y 轴）",
  analyzing_y: "拟合 Y 轴增益",
  validating: "校验测量结果",
  applying: "写入参数",
  completed: "标定完成",
  cancelled: "已取消",
  failed: "标定失败",
};

// ---- 标定组的共享状态（2026-10-03 从 bindEvents 体内一起拎出来）----
// ★ 这三个原本和5 个标定函数同处一段局部区域：
//   - CALIB_PHASE_LABELS  是 calibPhaseLabel 的查表
//   - calibTraceLines    renderCalibration 的滚动日志
//   - calibPollTimer scheduleCalibrationPolling 的计时器句柄
//   拆函数时若只搬 function 不搬它们，被移动的函数一调用就报
//   ReferenceError —— node --check 与全部 pytest 都发现不了。

let calibPollTimer = null;
const calibTraceLines = [];
