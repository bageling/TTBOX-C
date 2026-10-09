// ==========================================================================
// 09-fan.js —— 面板「风扇控制」fan-page
// ==========================================================================
// ★ 本页签的控件共 42 个（实测自 index.html 的 section#fan-page）。
//   ★ 连「EDID 对话框」与「激活进度浮层」的 DOM 也在这个 section 内
//     （HTML 结构就是这样），所以本文件还装它们的逻辑。
//
// ★ 本文件装什么：
//   validateDisplayEdidModeInputs / setDisplayEdidModeDialogOpen
//   updateDisplayEdidModeRefreshFromSize / renderFanControlStatus
//   setActivationSetupProgress 等
//
// ★ 加新功能放哪：风扇曲线 / EDID 模式 → 本文件。
// ==========================================================================
function setActivationSetupProgress(progress, activeStep, message) {
  const overlay = $("activationSetupOverlay");
  if (overlay) {
    overlay.hidden = false;
  }
  document.body.classList.add("modal-open");
  const progressEl = $("activationSetupProgress");
  if (progressEl) {
    progressEl.style.width = `${Math.max(0, Math.min(100, progress))}%`;
  }
  ["License", "Display", "Wait", "Mouse"].forEach((name, index) => {
    const stepEl = $(`activationSetupStep${name}`);
    if (!stepEl) {
      return;
    }
    stepEl.classList.toggle("is-done", index < activeStep);
    stepEl.classList.toggle("is-active", index === activeStep);
  });
  const messageEl = $("activationSetupMessage");
  if (messageEl) {
    messageEl.textContent = message || "请保持设备供电，不要关闭页面。";
  }
}

function renderAnnouncement(announcement) {
  const title = $("announcementTitle");
  const body = $("announcementBody");
  if (title) {
    title.textContent = announcement.title || "公告";
  }
  if (body) {
    body.textContent = announcement.content || "";
  }
  const dialog = $("announcementDialog");
  if (dialog) {
    dialog.dataset.version = announcement.version || "";
  }
}

function renderFanControlStatus(fanControl) {
  const fan = fanControl || {};
  const controlAvailable = !!fan.control_available;
  const enabled = !!fan.enabled;
  const temp = Number(fan.temperature_celsius);
  const pwmPercent = Number(fan.pwm_percent);
  const pwmRaw = Number(fan.pwm_raw);
  const rpm = Number(fan.fan_rpm);
  // 状态文案以真实 PWM 占空比为准：Core 开机即把 pwm 写成满转（Application.cpp
  // 的"风扇满转"段），此前这里恒显"未启用"，与风扇正在转的事实相反。
  const pwmText = Number.isFinite(pwmPercent)
    ? `${Math.max(0, Math.min(100, Math.round(pwmPercent)))}%`
    : "--";
  setText("fanControlPill", controlAvailable ? `${enabled ? "运行中" : "未启用"} ${pwmText}` : "未检测到PWM接口");
  setText("fanRuntimeMode", enabled ? (pwmPercent >= 100 ? "全速" : pwmText) : "未启用");
  setText("fanControlAvailableValue", controlAvailable ? "PWM接口可用" : "未检测到PWM接口");
  setText("fanTemperatureSourceValue", fan.source_label || "--");
  setText("fanTemperatureValue", Number.isFinite(temp) && temp > 0 ? `${temp.toFixed(1)} °C` : "--");
  setText(
    "fanPwmValue",
    Number.isFinite(pwmPercent)
      ? `${Math.max(0, Math.min(100, Math.round(pwmPercent)))}%${Number.isFinite(pwmRaw) ? ` (${Math.round(pwmRaw)})` : ""}`
      : "--"
  );
  setText(
    "fanRpmValue",
    fan.tachometer_available
      ? (Number.isFinite(rpm) && rpm > 0 ? `${Math.round(rpm)} RPM` : "0 RPM")
      : "无转速反馈"
  );
  setText("fanErrorValue", fan.last_error || "正常");
}

// ---------------------------------------------------------------------------
// BB 模块读写（表驱动，见 BB_CTRL_MODULES）
// 老规矩：setCheckbox / setValue / getCheckbox / getNumber 都有 if (el) 兜底，
// 元素缺失时读回默认值、写入静默跳过 —— 所以表和界面短暂不同步不会炸页面。
// ---------------------------------------------------------------------------
function readDisplayEdidModeInputs() {
  const width = Number(getString("displayEdidModeWidth"));
  const height = Number(getString("displayEdidModeHeight"));
  const refresh = Number(getString("displayEdidModeRefresh"));
  return {
    width: Number.isFinite(width) ? Math.round(width) : 0,
    height: Number.isFinite(height) ? Math.round(height) : 0,
    refresh: Number.isFinite(refresh) ? Math.round(refresh) : 0,
  };
}

function validateDisplayEdidModeInputs({ updateUi = true } = {}) {
  const { width, height, refresh } = readDisplayEdidModeInputs();
  const messages = [];
  const invalidIds = [];
  const widthValid = width >= DISPLAY_DYNAMIC_MODE_MIN_WIDTH && width <= DISPLAY_DYNAMIC_MODE_MAX_WIDTH;
  const heightValid = height >= DISPLAY_DYNAMIC_MODE_MIN_HEIGHT && height <= DISPLAY_DYNAMIC_MODE_MAX_HEIGHT;
  const refreshValid = refresh >= DISPLAY_DYNAMIC_MODE_MIN_REFRESH && refresh <= DISPLAY_DYNAMIC_MODE_MAX_REFRESH;

  if (!widthValid) {
    invalidIds.push("displayEdidModeWidth");
    messages.push(`宽度范围 ${DISPLAY_DYNAMIC_MODE_MIN_WIDTH}-${DISPLAY_DYNAMIC_MODE_MAX_WIDTH}`);
  }
  if (!heightValid) {
    invalidIds.push("displayEdidModeHeight");
    messages.push(`高度范围 ${DISPLAY_DYNAMIC_MODE_MIN_HEIGHT}-${DISPLAY_DYNAMIC_MODE_MAX_HEIGHT}`);
  }
  if (!refreshValid) {
    invalidIds.push("displayEdidModeRefresh");
    messages.push(`刷新率范围 ${DISPLAY_DYNAMIC_MODE_MIN_REFRESH}-${DISPLAY_DYNAMIC_MODE_MAX_REFRESH} Hz`);
  }

  const maxRefresh = widthValid && heightValid ? displayDynamicModeMaxRefresh(width, height) : 0;
  const pixelClockKHz = widthValid && heightValid && refreshValid
    ? displayDynamicModePixelClockKHz(width, height, refresh)
    : 0;
  if (widthValid && heightValid && refreshValid) {
    if (pixelClockKHz < DISPLAY_DYNAMIC_MODE_MIN_PIXEL_CLOCK_KHZ) {
      invalidIds.push("displayEdidModeRefresh");
      messages.push("当前刷新率的像素时钟低于 EDID DTD 下限 25 MHz");
    } else if (pixelClockKHz > DISPLAY_DYNAMIC_MODE_MAX_PIXEL_CLOCK_KHZ) {
      invalidIds.push("displayEdidModeRefresh");
      messages.push(`当前组合约 ${formatNumber(pixelClockKHz / 1000, 2)} MHz，超过 HDMI RX 600 MHz 上限`);
    }
  }

  if (updateUi) {
    setDisplayEdidModeInvalidFields(invalidIds);
    if (messages.length) {
      setDisplayEdidModeStatus(messages.join("；"), true);
    } else {
      setDisplayEdidModeStatus(
        `自动计算最高 ${maxRefresh} Hz；当前约 ${formatNumber(pixelClockKHz / 1000, 2)} MHz / 600 MHz`,
        false,
      );
    }
  }
  return {
    valid: messages.length === 0,
    width,
    height,
    refresh,
    maxRefresh,
    pixelClockKHz,
  };
}

function updateDisplayEdidModeRefreshFromSize() {
  const width = Number(getString("displayEdidModeWidth"));
  const height = Number(getString("displayEdidModeHeight"));
  if (
    Number.isFinite(width)
    && Number.isFinite(height)
    && width >= DISPLAY_DYNAMIC_MODE_MIN_WIDTH
    && width <= DISPLAY_DYNAMIC_MODE_MAX_WIDTH
    && height >= DISPLAY_DYNAMIC_MODE_MIN_HEIGHT
    && height <= DISPLAY_DYNAMIC_MODE_MAX_HEIGHT
  ) {
    setValue("displayEdidModeRefresh", displayDynamicModeMaxRefresh(Math.round(width), Math.round(height)));
  }
  validateDisplayEdidModeInputs();
}

function setDisplayEdidModeDialogOpen(open) {
  const dialog = $("displayEdidModeDialog");
  if (!dialog) {
    return;
  }
  dialog.hidden = !open;
  setAnyModalOpen();
  if (open) {
    const defaults = displayEdidModeDefaults();
    setValue("displayEdidModeWidth", defaults.width);
    setValue("displayEdidModeHeight", defaults.height);
    setValue("displayEdidModeRefresh", defaults.refresh);
    validateDisplayEdidModeInputs();
    const widthInput = $("displayEdidModeWidth");
    if (widthInput) {
      widthInput.focus();
      widthInput.select();
    }
  } else {
    setDisplayEdidModeInvalidFields([]);
  }
}

