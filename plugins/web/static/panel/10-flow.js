// ==========================================================================
// 10-flow.js —— 面板「流程编排层」（原名10-flow.js，2026-10-03 改名）
// ==========================================================================
// ★ 为什么改名：原名 "shared"（共享）**误导**——它不是"大家共用的工具箱"，
//   而是**流程的上层编排**：config读写、状态应用、事件绑定总入口都在这里。
//   2026-10-03 实测（call graph）：
//     · 01..09 之间**零互相调用**（页签边界是干净的）
//     · 本文件→ 01..09 有 93 条调用边，其中 81% 是 5 个渲染入口
//       （renderLicensePanel / renderUpdateStatus / updateAimRangeOverlay /
//        setDisplayEdidModeDialogOpen / setModelGameSuggestionOpen）
//     ⇒ 那是「上层流程说：该刷新某页了」→ 下层页签执行刷新，
//       **方向本来就对**，只是文件名让方向看着反了。
//
// ★ 层次（加载顺序即依赖顺序，越靠前越底层）：
//   00-const.js   数据层：59 个 const/let/var（state + 默认值 + 表）
//   01..09        页签层：各页签的渲染 + 事件绑定（互不调用）
//   10-flow.js    流程层（本文件）：跨页签编排 —— 加载在最后
//   calib-bind.js 标定组：挂在流程层旁边的专用函数
//
// ★ 本文件装什么：
//   1 流程编排：main / bindEvents / collectConfig / populateForm
//     / applyConfigNow / applyFullState / applyLiveState / api / runUiAction
//   2 跨页签配置读写：collectConfig 与 populateForm 是**唯一定义点**
//   3 通用工具：$ / escapeHtml / on / setText / 各 label 映射
//
// ★ 加新功能放哪：
//   · 跨页签的配置项读写 → 本文件（collectConfig / populateForm 是唯一定义点）
//   · 某个页签专属的逻辑   → 那个页签自己的文件（01..09），别放这里
//   · 新增全局常量        → 00-const.js（不是本文件）
//
// ★ 本文件调用 01..09 的函数（updateAimRangeOverlay / renderModelPanel /
//   validateDisplayHardware / renderLicensePanel / renderFanControlStatus 等）——
//   这些调用**必须写在函数体内**，不能放顶层立即执行，
//   因为被调方在前面加载、但顶层立即执行时页面还没渲染完。
//
// ★ 硬约束（由test_web_panel_layers.py 守住）：
//   01..09 之间**禁止互相调用**。要共享就放本文件。
// ==========================================================================
function $(id) {
  return document.getElementById(id);
}

function overviewDefaults() {
  return {
    ...OVERVIEW_DEFAULTS,
    capture_crop_size: modelInputCropSize(currentModel()) || OVERVIEW_DEFAULTS.capture_crop_size,
  };
}

function setText(id, value) {
  const el = $(id);
  if (el) {
    el.textContent = value === undefined || value === null ? "" : String(value);
  }
}

function on(id, eventName, handler) {
  const el = $(id);
  if (el) {
    el.addEventListener(eventName, handler);
  }
}

function normalizeUiBrand(value) {
  const normalized = String(value || "").trim().toLowerCase();
  if (normalized === UI_BRAND_XH || normalized === UI_BRAND_XCSH) return normalized;
  return UI_BRAND_YU;
}

function brandFromPayload(payload) {
  if (!payload || typeof payload !== "object") {
    return normalizeUiBrand(document.documentElement.dataset.uiBrand);
  }
  if (payload.ui_brand) return normalizeUiBrand(payload.ui_brand);
  if (payload.ui && payload.ui.ui_brand) return normalizeUiBrand(payload.ui.ui_brand);
  if (payload.license && payload.license.ui_brand) return normalizeUiBrand(payload.license.ui_brand);
  if (payload.state && payload.state.license && payload.state.license.ui_brand) {
    return normalizeUiBrand(payload.state.license.ui_brand);
  }
  return normalizeUiBrand(document.documentElement.dataset.uiBrand);
}

function brandConfig(brand, ui) {
  // 皮肤闭集（yu/xh/xcsh）只决定**视觉**；文案一律取服务端 payload.ui.*（单一真相源）。
  // 面板不再内置任何品牌文案表 —— 新增渠道 = 改 ui_brands.json，零代码改动。
  const uiBrand = normalizeUiBrand(brand);
  const source = (ui && typeof ui === "object")
    ? ui
    : ((state.data && typeof state.data.ui === "object" && state.data.ui) || {});
  const defaultTheme = source.default_theme === "light" ? "light" : "dark";
  return {
    uiBrand,
    title: source.brand_title || "TTBOX 控制台",
    eyebrow: source.brand_eyebrow || "TTBOX SYSTEM",
    mark: source.brand_mark || "TT",
    appTitle: source.app_title || source.brand_title || "TTBOX 控制台",
    allowThemeSwitch: source.allow_theme_switch !== false,
    defaultTheme,
    defaultLocalName: source.default_local_name || "ttbox",
    defaultHotspotSsid: source.default_hotspot_ssid || "TTBOX",
    fallbackResetText: source.fallback_reset_text || "重置默认 Wi-Fi",
  };
}

function currentBrandConfig() {
  return brandConfig(state.uiBrand);
}

function storedTheme(fallback = "dark") {
  const fallbackTheme = fallback === "light" ? "light" : "dark";
  try {
    const value = localStorage.getItem(THEME_STORAGE_KEY);
    return value === "light" || value === "dark" ? value : fallbackTheme;
  } catch {
    return fallbackTheme;
  }
}

function saveStoredTheme(theme) {
  try {
    localStorage.setItem(THEME_STORAGE_KEY, theme);
  } catch {
    // localStorage can be disabled in private or locked-down browser modes.
  }
}

function updateThemeToggleLabel() {
  const button = $("themeToggleButton");
  if (!button) return;
  const isLight = state.theme === "light";
  button.textContent = isLight ? "深色" : "浅色";
  button.dataset.icon = isLight ? "moon" : "sun";
  button.setAttribute("data-symbol", isLight ? "\u263e" : "\u2600");
  button.setAttribute("aria-label", isLight ? "切换到深色主题" : "切换到浅色主题");
  button.hidden = state.allowThemeSwitch === false;
}

function applyTheme(theme, { persist = false } = {}) {
  const nextTheme = theme === "light" ? "light" : "dark";
  state.theme = nextTheme;
  document.documentElement.dataset.theme = nextTheme;
  if (persist && state.allowThemeSwitch !== false) {
    saveStoredTheme(nextTheme);
  }
  updateThemeToggleLabel();
}

function applyBrand(payload) {
  const payloadUi = (payload && typeof payload.ui === "object") ? payload.ui : null;
  const stateUi = (state.data && typeof state.data.ui === "object") ? state.data.ui : null;
  const ui = payloadUi || stateUi || {};
  const config = brandConfig(brandFromPayload(payload), ui);
  state.uiBrand = config.uiBrand;
  state.allowThemeSwitch = config.allowThemeSwitch;
  // ★ 签名守卫（2026-10-03 性能 B）：品牌配置没变就整段跳过。
  //   下面每一步都在写 DOM（dataset / classList ×3 / 3 个 textContent /
  //   document.title / placeholder / hidden / applyTheme），
  //   即使值没变，赋值也会触发 MutationObserver → 浏览器重排。
  //   实测：轮询每 1.5s 调一次，静止 12 秒产生 .brand 32 / .sidebar 32 /
  //   themeToggleButton[data-icon] 32 / lanHostnameInput[placeholder] 32 次变更。
  const nextSignature = [
    config.uiBrand, config.allowThemeSwitch ? 1 : 0,
    config.mark, config.eyebrow, config.title, config.appTitle,
    config.defaultLocalName, config.defaultTheme,
  ].join("\u001e");
  if (state.brandRenderSignature === nextSignature) {
    return;
  }
  state.brandRenderSignature = nextSignature;

  document.documentElement.dataset.uiBrand = config.uiBrand;
  document.body.classList.toggle("ui-brand-yu", config.uiBrand === UI_BRAND_YU);
  document.body.classList.toggle("ui-brand-xh", config.uiBrand === UI_BRAND_XH);
  document.body.classList.toggle("ui-brand-xcsh", config.uiBrand === UI_BRAND_XCSH);
  const mark = $("brandMark");
  const eyebrow = $("brandEyebrow");
  const title = $("brandTitle");
  if (mark) mark.textContent = config.mark;
  if (eyebrow) eyebrow.textContent = config.eyebrow;
  if (title) title.textContent = config.title;
  document.title = config.appTitle;

  const hostnameInput = $("lanHostnameInput");
  if (hostnameInput) hostnameInput.placeholder = config.defaultLocalName;

  const themeButton = $("themeToggleButton");
  if (themeButton) themeButton.hidden = !config.allowThemeSwitch;
  // 主题：可切换渠道沿用用户选择（无存储 ⇒ 用服务端 default_theme 兜底）；
  // 锁定渠道（allow_theme_switch=false）恒用服务端 default_theme，面板不可切换。
  applyTheme(config.allowThemeSwitch ? storedTheme(config.defaultTheme) : config.defaultTheme);
}

function initThemeControls() {
  const button = $("themeToggleButton");
  if (!button) return;
  button.addEventListener("click", () => {
    if (state.allowThemeSwitch === false) return;
    applyTheme(state.theme === "light" ? "dark" : "light", { persist: true });
  });
  updateThemeToggleLabel();
}

function escapeHtml(value) {
  return String(value === undefined || value === null ? "" : value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

function escapeAttr(value) {
  return escapeHtml(value);
}

async function copyTextToClipboard(text) {
  const value = String(text || "");
  if (!value) {
    throw new Error("没有可复制的内容");
  }
  if (navigator.clipboard && window.isSecureContext) {
    await navigator.clipboard.writeText(value);
    return;
  }
  const textarea = document.createElement("textarea");
  textarea.value = value;
  textarea.setAttribute("readonly", "");
  textarea.style.position = "fixed";
  textarea.style.left = "-9999px";
  textarea.style.top = "0";
  document.body.appendChild(textarea);
  textarea.select();
  const copied = document.execCommand("copy");
  textarea.remove();
  if (!copied) {
    throw new Error("浏览器拒绝复制，请手动复制设备码");
  }
}

function formatDateTime(value) {
  const raw = String(value || "").trim();
  if (!raw) return "--";
  const date = new Date(raw);
  if (Number.isNaN(date.getTime())) return raw;
  return date.toLocaleString("zh-CN", { hour12: false });
}

function stableStringify(value) {
  if (Array.isArray(value)) {
    return `[${value.map(stableStringify).join(",")}]`;
  }
  if (value && typeof value === "object") {
    return `{${Object.keys(value).sort().map((key) => {
      return `${JSON.stringify(key)}:${stableStringify(value[key])}`;
    }).join(",")}}`;
  }
  return JSON.stringify(value);
}

function cloneJson(value) {
  return value == null ? value : JSON.parse(JSON.stringify(value));
}

function showToast(message, isError = false) {
  const el = $("toast");
  if (!el) {
    return;
  }
  el.textContent = message;
  el.className = `toast ${isError ? "error" : ""}`;
  clearTimeout(showToast._timer);
  showToast._timer = setTimeout(() => {
    el.className = "toast hidden";
  }, 2600);
}

function markMouseModeSwitching(durationMs = MOUSE_MODE_SWITCH_SUPPRESS_MS) {
  state.mouseModeSwitchSuppressUntil = Math.max(
    state.mouseModeSwitchSuppressUntil || 0,
    Date.now() + durationMs,
  );
}

function isMouseModeSwitching() {
  return Date.now() < (state.mouseModeSwitchSuppressUntil || 0);
}

function isTransientDaemonBusy(message) {
  const text = String(message || "").toLowerCase();
  return text.includes("daemon socket unavailable")
    && (text.includes("timed out") || text.includes("resource temporarily unavailable") || text.includes("errno 11"));
}

function suppressMouseSwitchDaemonTimeout(message) {
  if (!isMouseModeSwitching() || !isTransientDaemonBusy(message)) {
    return false;
  }
  setHardwareStatus("mouseHardwareStatus", "切换中");
  return true;
}

function setAnyModalOpen() {
  const hasOpenModal = Array.from(document.querySelectorAll(".modal-backdrop"))
    .some((modal) => !modal.hidden);
  document.body.classList.toggle("modal-open", hasOpenModal);
}

function setDisclaimerDialogOpen(open) {
  const dialog = $("disclaimerDialog");
  if (!dialog) {
    return;
  }
  updateDisclaimerActions();
  dialog.hidden = !open;
  setAnyModalOpen();
  if (open) {
    const acceptButton = $("acceptDisclaimerButton");
    if (acceptButton) {
      acceptButton.focus();
    }
  }
}

function maybeShowDisclaimer() {
  try {
    if (window.localStorage && window.localStorage.getItem(DISCLAIMER_STORAGE_KEY) === "hidden") {
      window.setTimeout(() => maybeShowAnnouncement(), 120);
      return;
    }
  } catch {
    // Some embedded browsers can disable localStorage; showing the dialog is still fine.
  }
  window.setTimeout(() => setDisclaimerDialogOpen(true), 120);
}

function isLicenseValid() {
  return !!(state.data && state.data.state && state.data.state.license && state.data.state.license.valid);
}

function updateDisclaimerActions() {
  const hideButton = $("hideDisclaimerButton");
  if (!hideButton) {
    return;
  }
  const valid = isLicenseValid();
  hideButton.disabled = !valid;
  hideButton.title = valid ? "" : "设备激活后才可选择不再显示";
}

function licenseStateFromPayload(payload) {
  return (payload && payload.state && payload.state.license) || {};
}

function friendlyLicenseMessage(message, fallback = "设备未激活，请输入激活码后继续使用。") {
  const text = String(message || "").trim();
  if (!text) {
    return fallback;
  }
  let reason = text;
  if (text.startsWith("{")) {
    try {
      const payload = JSON.parse(text);
      reason = String(payload.error || (payload.data && payload.data.reason) || payload.message || text).trim();
    } catch {
      reason = text;
    }
  }
  if (reason === "valid license is required" || reason === "license is not active on this server") {
    return "授权校验失败，请点击授权修复或重新输入激活码。";
  }
  if (reason === "license key is already bound to another device") {
    return "激活码已绑定其他设备。";
  }
  return reason || fallback;
}

function activeLicenseMessage() {
  const license = licenseStateFromPayload(state.data);
  return friendlyLicenseMessage(license.message);
}

function syncLicenseKeyInputs(sourceInput) {
  const value = sourceInput ? sourceInput.value : "";
  ["licenseKeyInput", "licenseGateKeyInput"].forEach((id) => {
    const input = $(id);
    if (input && input !== sourceInput) {
      input.value = value;
    }
  });
}

function updateLicenseGateStatus(message) {
  const status = $("licenseGateStatus");
  if (status) {
    status.textContent = message || activeLicenseMessage();
  }
}

function focusLicenseGateInput() {
  const input = $("licenseGateKeyInput") || $("licenseKeyInput");
  if (input) {
    window.setTimeout(() => input.focus(), 40);
  }
}

function hideDisclaimer(remember) {
  if (remember) {
    if (!isLicenseValid()) {
      showToast("设备激活后才可选择不再显示", true);
      updateDisclaimerActions();
      return;
    }
    try {
      window.localStorage.setItem(DISCLAIMER_STORAGE_KEY, "hidden");
    } catch {
      showToast("当前浏览器无法保存不再显示设置", true);
    }
  }
  setDisclaimerDialogOpen(false);
  maybeShowAnnouncement();
}

function sleep(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function nextPaint() {
  return new Promise((resolve) => {
    window.requestAnimationFrame(() => window.requestAnimationFrame(resolve));
  });
}

function setActivationBusy(busy) {
  ["activateLicenseButton", "licenseGateActivateButton", "licenseGateRefreshButton"].forEach((id) => {
    const button = $(id);
    if (button) {
      button.disabled = !!busy;
    }
  });
  ["licenseKeyInput", "licenseGateKeyInput"].forEach((id) => {
    const input = $(id);
    if (input) {
      input.disabled = !!busy;
    }
  });
}

function clearActivationSetupProgress() {
  const overlay = $("activationSetupOverlay");
  if (overlay) {
    overlay.hidden = true;
  }
  document.body.classList.remove("modal-open");
  setAnyModalOpen();
}

async function runFirstActivationSetup() {
  setActivationSetupProgress(12, 0, "授权已通过，正在准备设备硬件身份。");
  await loadHardware().catch(() => {});

  setActivationSetupProgress(32, 1, "正在随机并应用显示器模式。");
  randomizeDisplayHardware();
  const displayConfig = validateDisplayHardware();
  const displayResult = await api("/api/hardware/display", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ config: displayConfig, apply: true, reboot_after_apply: false }),
  });
  populateDisplayHardware({
    available: true,
    config: displayResult.config,
    status: displayResult.result,
  });

  setActivationSetupProgress(58, 2, "显示器模式已应用，等待显示链路稳定 10 秒。");
  await sleep(10000);

  setActivationSetupProgress(82, 3, "正在读取鼠标透传状态（模式由板端服务决定，此处只读）。");
  // 2026-09-22：原来这里 PUT /api/hardware/mouse/mode —— 该端点恒返回 ok:false
  // （真源是 systemd 单元，Web 无 root 改不了），api() 见到 ok:false 会抛错，
  // 导致激活向导在 82% 处直接失败。改为只读 GET，不再制造假失败。
  const mouseState = await api("/api/hardware/mouse");
  await sleep(500);
  populateMouseHardware(mouseState);
  setActivationSetupProgress(100, 4, "初始化完成，Windows 会重新枚举完整鼠标设备，页面保持可用。");
}

function setAnnouncementDialogOpen(open) {
  const dialog = $("announcementDialog");
  if (!dialog) {
    return;
  }
  dialog.hidden = !open;
  setAnyModalOpen();
  if (open) {
    const ackButton = $("ackAnnouncementButton");
    if (ackButton) {
      ackButton.focus();
    }
  }
}

async function maybeShowAnnouncement() {
  const disclaimer = $("disclaimerDialog");
  const announcementDialog = $("announcementDialog");
  if ((disclaimer && !disclaimer.hidden) || (announcementDialog && !announcementDialog.hidden)) {
    return;
  }
  let announcement = null;
  try {
    announcement = await api("/api/announcement");
  } catch {
    return;
  }
  if (!announcement || !announcement.enabled || !announcement.content) {
    return;
  }
  renderAnnouncement(announcement);
  setAnnouncementDialogOpen(true);
}

function acknowledgeAnnouncement() {
  setAnnouncementDialogOpen(false);
}

function payloadContainsRevocation(payload) {
  if (!payload || typeof payload !== "object") {
    return false;
  }
  if (payload.revoked === true) {
    return true;
  }
  if (payload.revoked && typeof payload.revoked === "object" && payload.revoked.revoked) {
    return true;
  }
  if (payload.data && typeof payload.data === "object") {
    return payloadContainsRevocation(payload.data);
  }
  return false;
}

function isAuthorizationFailure(error) {
  if (!error) {
    return false;
  }
  if (payloadContainsRevocation(error.payload)) {
    return true;
  }
  const message = String(error.message || "").trim().toLowerCase();
  if (!message) {
    return error.status === 401;
  }
  const authorizationTerms = [
    "valid license is required",
    "license is not active on this server",
    "license key is disabled",
    "license revoked",
    "trial license expired",
    "device is frozen",
    "未激活",
    "已禁用",
    "已撤销",
    "已过期",
    "被冻结",
    "activation_required",
    "未授权",
  ];
  return error.status === 401 || authorizationTerms.some((term) => message.includes(term));
}

function activationPageUrl() {
  // TTBOX（U6）：激活入口 = 服务端 /activate 路由；上游的 ?activation=1 只写不读，本仓不实现。
  return "/activate";
}

function redirectToActivationPage() {
  if (state.activationRedirecting) {
    return;
  }
  state.activationRedirecting = true;
  document.title = "设备激活";
  document.body.classList.remove("license-loading");
  window.setTimeout(() => {
    window.location.replace(activationPageUrl());
  }, 0);
}

async function confirmAuthorizationFailure() {
  // 2026-09-20：OTA 安装会重启 core，重启瞬间 /api/state 可能瞬时 401「未激活」，
  // 旧逻辑一见授权类错误立刻跳激活页 ⇒ 用户升级中被误踢到激活页。改为二次确认：
  // 重查一次 /api/state，只有它也明确报授权失败才跳；网络抖动/服务重启中一律不跳。
  if (authConfirmInFlight) {
    return false;
  }
  authConfirmInFlight = true;
  try {
    const response = await fetch("/api/state", { cache: "no-store" });
    if (response.status === 401) {
      return true;
    }
    const data = await response.json().catch(() => ({}));
    const error = new Error(data.error || `HTTP ${response.status}`);
    error.status = response.status;
    return isAuthorizationFailure(error);
  } catch (error) {
    return false;
  } finally {
    window.setTimeout(() => {
      authConfirmInFlight = false;
    }, 1500);
  }
}

// ★ 2026-10-03 性能 A：默认超时。
//   原来 fetch 不带 AbortSignal ⇒ 请求挂住就一直等，
//   轮询（每 1.5s /api/state）撞上 core 重启时会长时间无响应，
//   表现为界面卡住 + 徽章被刷成「未连接」。
//   长耗时接口（OTA 安装、模型导入、标定）可以用 `timeoutMs` 覆盖 ——
//   绝不能给所有调用套同一个短超时。
const API_DEFAULT_TIMEOUT_MS = 8000;

async function api(path, options = {}) {
  const { timeoutMs, signal, ...rest } = options;
  const controller = typeof AbortController === "function" ? new AbortController() : null;
  let timer = null;
  let timedOut = false;
  const limit = typeof timeoutMs === "number" ? timeoutMs : API_DEFAULT_TIMEOUT_MS;
  if (controller && !signal && limit > 0) {
    timer = window.setTimeout(() => {
      timedOut = true;
      controller.abort();
    }, limit);
  }
  let response;
  try {
    response = await fetch(path, {
      ...rest,
      ...(controller ? { signal: signal || controller.signal } : {}),
    });
  } catch (error) {
    if (timedOut) {
      const timeoutError = new Error(`请求超时（${limit}ms）：${path}`);
      timeoutError.status = 0;
      timeoutError.timedOut = true;
      throw timeoutError;
    }
    throw error;
  } finally {
    if (timer !== null) {
      window.clearTimeout(timer);
    }
  }
  const data = await response.json().catch(() => ({}));
  if (!response.ok || data.ok === false) {
    const error = new Error(data.error || `HTTP ${response.status}`);
    error.payload = data.data;
    error.status = response.status;
    if (isAuthorizationFailure(error)) {
      const updateRunning = state.updateStatus && state.updateStatus.status === "running";
      if (!updateRunning) {
        confirmAuthorizationFailure().then((confirmed) => {
          if (confirmed) {
            redirectToActivationPage();
          }
        });
      }
    }
    throw error;
  }
  return data.data;
}

function setApplyStatus(mode, text) {
  const el = $("applyIndicator");
  if (!el) {
    return;
  }
  el.className = `sync-badge ${mode}`;
  el.textContent = text;
}

function setAimTraceStatus(text) {
  const el = $("recordAimTraceStatus");
  if (el) {
    el.textContent = text || "";
  }
}

function setUsbDiagnosticsStatus(text) {
  const el = $("usbDiagnosticsStatus");
  if (el) {
    el.textContent = text || "";
  }
}

function filenameFromContentDisposition(header, fallback) {
  const value = String(header || "");
  const utf8Match = value.match(/filename\*=UTF-8''([^;]+)/i);
  if (utf8Match) {
    try {
      return decodeURIComponent(utf8Match[1].trim().replace(/^"|"$/g, ""));
    } catch {
      return utf8Match[1].trim().replace(/^"|"$/g, "") || fallback;
    }
  }
  const plainMatch = value.match(/filename="?([^";]+)"?/i);
  return plainMatch ? plainMatch[1].trim() : fallback;
}

async function downloadUsbDiagnostics() {
  const button = $("downloadUsbDiagnosticsButton");
  try {
    if (button) {
      button.disabled = true;
    }
    setUsbDiagnosticsStatus("生成中...");
    const response = await fetch("/api/diagnostics/usb-proxy.zip");
    if (!response.ok) {
      const text = await response.text().catch(() => "");
      throw new Error(text.slice(0, 200) || `HTTP ${response.status}`);
    }
    const blob = await response.blob();
    const filename = filenameFromContentDisposition(
      response.headers.get("Content-Disposition"),
      "usb-proxy-diagnostics.zip",
    );
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
    setUsbDiagnosticsStatus("已下载");
    showToast("USB诊断日志已生成");
  } catch (error) {
    setUsbDiagnosticsStatus("下载失败");
    throw error;
  } finally {
    if (button) {
      button.disabled = false;
    }
  }
}

function fillOptions(select, values) {
  if (!select) {
    return;
  }
  const previous = select.value;
  select.innerHTML = "";
  values.forEach((value) => {
    const option = document.createElement("option");
    option.value = value;
    option.textContent = HOTKEY_LABELS[value] || value;
    select.appendChild(option);
  });
  if (values.includes(previous)) {
    select.value = previous;
  }
}

function setCheckbox(id, value) {
  const el = $(id);
  if (el) {
    el.checked = !!value;
  }
}

function setRadioValue(name, value) {
  const targetValue = String(value || "");
  let matched = false;
  document.querySelectorAll(`input[type="radio"][name="${name}"]`).forEach((input) => {
    const checked = input.value === targetValue;
    input.checked = checked;
    matched = matched || checked;
  });
  if (!matched) {
    const fallback = document.querySelector(`input[type="radio"][name="${name}"]`);
    if (fallback) {
      fallback.checked = true;
    }
  }
}

function decimalPlacesFromStep(step) {
  if (!step || step === "any") {
    return null;
  }
  const numericStep = Number(step);
  if (!Number.isFinite(numericStep) || numericStep <= 0) {
    return null;
  }
  const normalized = numericStep.toFixed(10).replace(/0+$/, "");
  const dotIndex = normalized.indexOf(".");
  return dotIndex === -1 ? 0 : normalized.length - dotIndex - 1;
}

function trimFixedNumber(value) {
  return value.replace(/(\.\d*?)0+$/, "$1").replace(/\.$/, "");
}

function formatControlValue(el, value) {
  if (value === undefined || value === null || value === "") {
    return "";
  }
  if (!el || (el.type !== "number" && el.type !== "range")) {
    return value;
  }
  const numericValue = Number(value);
  if (!Number.isFinite(numericValue)) {
    return value;
  }
  const decimals = decimalPlacesFromStep(el.step);
  if (decimals === null) {
    // 2026-09-20：无 step 的数字/滑杆输入回填浮点时会显示二进制残差
    // （如 0.30000001192092896汃 0.5699999928474426）。统一按最多 4 位小数格式化，
    // trimFixedNumber 会去掉多余尾零，整数/长小数保持原样。
    return trimFixedNumber(numericValue.toFixed(4));
  }
  if (decimals === 0) {
    return String(Math.round(numericValue));
  }
  return trimFixedNumber(numericValue.toFixed(decimals));
}

function setValue(id, value) {
  const el = $(id);
  if (el) {
    if (state.isPopulating && document.activeElement === el && el.matches("input, textarea, select")) {
      return;
    }
    el.value = formatControlValue(el, value);
  }
}

function getNumber(id, fallback = 0) {
  const el = $(id);
  const raw = el ? el.value : "";
  if (raw === "") {
    return fallback;
  }
  const value = Number(raw);
  return Number.isFinite(value) ? value : fallback;
}

function getNumberInRange(id, fallback = 0) {
  const limits = NUMERIC_RANGE_LIMITS[id];
  const value = getNumber(id, fallback);
  return limits ? clamp(value, limits[0], limits[1]) : value;
}

function clampNumberInputToLimits(input) {
  if (!input || !input.id || input.value === "") {
    return;
  }
  const limits = NUMERIC_RANGE_LIMITS[input.id];
  const value = Number(input.value);
  if (!limits || !Number.isFinite(value)) {
    return;
  }
  const clampedValue = clamp(value, limits[0], limits[1]);
  input.value = formatControlValue(input, clampedValue);
  if (input.id === "capture_crop_size") {
    syncCropSizePresetRange(clampedValue);
    return;
  }
  const rangeInput = $(`${input.id}_range`);
  if (rangeInput) {
    rangeInput.value = formatControlValue(rangeInput, clampedValue);
  }
}

function getString(id) {
  const el = $(id);
  return el ? el.value.trim() : "";
}

function printableAscii(value, maxLength) {
  return String(value || "")
    .replace(/[^\x20-\x7E]/g, "")
    .slice(0, maxLength);
}

function vendorCode(value) {
  return printableAscii(value, 8).toUpperCase().replace(/[^A-Z]/g, "").slice(0, 3);
}

function hexText(value, maxDigits) {
  const text = String(value || "").trim();
  const prefix = text.toLowerCase().startsWith("0x") ? "0x" : "";
  const digits = text.replace(/^0x/i, "").replace(/[^0-9a-fA-F]/g, "").slice(0, maxDigits);
  return `${prefix}${digits}`;
}

function normalizeHexValue(value, maxDigits) {
  const digits = String(value || "").trim().replace(/^0x/i, "");
  if (!new RegExp(`^[0-9a-fA-F]{1,${maxDigits}}$`).test(digits)) {
    return "";
  }
  const numeric = Number.parseInt(digits, 16);
  if (!Number.isFinite(numeric) || numeric <= 0) {
    return "";
  }
  return `0x${digits.toLowerCase().padStart(maxDigits, "0")}`;
}

function setValidation(containerId, fieldIds, messages) {
  fieldIds.forEach((id) => {
    const el = $(id);
    if (el) {
      el.classList.remove("is-invalid");
    }
  });
  const container = $(containerId);
  if (!container) {
    return;
  }
  if (messages.length === 0) {
    container.hidden = true;
    container.textContent = "";
    return;
  }
  messages.forEach(({ id }) => {
    const el = $(id);
    if (el) {
      el.classList.add("is-invalid");
    }
  });
  container.hidden = false;
  container.textContent = messages.map(({ text }) => text).join("\n");
}

function getCheckbox(id) {
  const el = $(id);
  return !!(el && el.checked);
}

function getRadioValue(name, fallback = "") {
  const checked = document.querySelector(`input[type="radio"][name="${name}"]:checked`);
  return checked ? checked.value : fallback;
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function numericAttr(el, attrName, fallback) {
  const value = Number(el.getAttribute(attrName));
  return Number.isFinite(value) ? value : fallback;
}

function rangeEventPoint(event) {
  if (event.touches && event.touches.length > 0) {
    return { x: event.touches[0].clientX, y: event.touches[0].clientY };
  }
  if (event.changedTouches && event.changedTouches.length > 0) {
    return { x: event.changedTouches[0].clientX, y: event.changedTouches[0].clientY };
  }
  if (Number.isFinite(event.clientX) && Number.isFinite(event.clientY)) {
    return { x: event.clientX, y: event.clientY };
  }
  return null;
}

function rangeThumbCenterX(rangeInput) {
  const rect = rangeInput.getBoundingClientRect();
  const min = numericAttr(rangeInput, "min", 0);
  const max = numericAttr(rangeInput, "max", 100);
  const value = clamp(Number(rangeInput.value), min, max);
  const span = max - min;
  const rawRatio = span > 0 ? (value - min) / span : 0;
  const ratio = getComputedStyle(rangeInput).direction === "rtl" ? 1 - rawRatio : rawRatio;
  const thumbSize = Math.min(RANGE_THUMB_SIZE_PX, Math.max(0, rect.width));
  const travel = Math.max(0, rect.width - thumbSize);
  return rect.left + thumbSize / 2 + travel * ratio;
}

function isRangeThumbHit(rangeInput, event) {
  const point = rangeEventPoint(event);
  if (!point) {
    return true;
  }

  const rect = rangeInput.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) {
    return true;
  }

  const centerX = rangeThumbCenterX(rangeInput);
  const centerY = rect.top + rect.height / 2;
  const hitRadius = RANGE_THUMB_SIZE_PX / 2 + RANGE_THUMB_HIT_SLOP_PX;
  return Math.abs(point.x - centerX) <= hitRadius &&
    Math.abs(point.y - centerY) <= rect.height / 2 + RANGE_THUMB_HIT_SLOP_PX;
}

function shouldUseThumbOnlyRange(event) {
  const isCoarsePointer = window.matchMedia && window.matchMedia("(pointer: coarse)").matches;
  if (event.type === "touchstart") {
    return true;
  }
  if (event.pointerType) {
    return event.pointerType !== "mouse" || isCoarsePointer;
  }
  return isCoarsePointer;
}

function stopRangeTrackEvent(event) {
  event.preventDefault();
  if (typeof event.stopImmediatePropagation === "function") {
    event.stopImmediatePropagation();
  } else {
    event.stopPropagation();
  }
}

function clearRangeTrackBlock(rangeInput) {
  delete rangeInput.dataset.blockedRangeValue;
  delete rangeInput.dataset.blockRangeUntil;
}

function hasActiveRangeTrackBlock(rangeInput) {
  const blockUntil = Number(rangeInput.dataset.blockRangeUntil || 0);
  return blockUntil > 0 && Date.now() <= blockUntil;
}

function blockRangeTrackInteraction(rangeInput, event) {
  const blockUntil = Date.now() + 700;
  rangeInput.dataset.blockedRangeValue = rangeInput.value;
  rangeInput.dataset.blockRangeUntil = String(blockUntil);
  stopRangeTrackEvent(event);
  window.setTimeout(() => {
    if (Number(rangeInput.dataset.blockRangeUntil || 0) === blockUntil) {
      clearRangeTrackBlock(rangeInput);
    }
  }, 760);
}

function restoreBlockedRangeValue(rangeInput, event) {
  if (!hasActiveRangeTrackBlock(rangeInput)) {
    return;
  }
  if (rangeInput.dataset.blockedRangeValue !== undefined) {
    rangeInput.value = rangeInput.dataset.blockedRangeValue;
  }
  stopRangeTrackEvent(event);
}

function enableThumbOnlyRangeInput(rangeInput) {
  if (!rangeInput || rangeInput.type !== "range" || rangeInput.dataset.thumbOnlyBound === "1") {
    return;
  }
  rangeInput.dataset.thumbOnlyBound = "1";

  const handleStart = (event) => {
    if (rangeInput.disabled) {
      return;
    }
    if (event.type === "mousedown" && event.button !== 0) {
      return;
    }
    if (!shouldUseThumbOnlyRange(event)) {
      return;
    }
    if (!isRangeThumbHit(rangeInput, event)) {
      blockRangeTrackInteraction(rangeInput, event);
      return;
    }
    clearRangeTrackBlock(rangeInput);
  };

  rangeInput.addEventListener("pointerdown", handleStart, { capture: true });
  rangeInput.addEventListener("mousedown", handleStart, { capture: true });
  rangeInput.addEventListener("touchstart", handleStart, { capture: true, passive: false });
  rangeInput.addEventListener("input", (event) => restoreBlockedRangeValue(rangeInput, event), true);
  rangeInput.addEventListener("change", (event) => restoreBlockedRangeValue(rangeInput, event), true);
  rangeInput.addEventListener("click", (event) => restoreBlockedRangeValue(rangeInput, event), true);
}

function enableThumbOnlyRangeInputs(root = document) {
  root.querySelectorAll("input[type='range']").forEach(enableThumbOnlyRangeInput);
}

function formatNumber(value, digits = 2) {
  const number = Number(value);
  return Number.isFinite(number) ? number.toFixed(digits) : (0).toFixed(digits);
}

function positiveNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? number : null;
}

function formatMaybe(value, fallback = "-") {
  return value === undefined || value === null || value === "" ? fallback : String(value);
}

function formatPercent(value) {
  const number = Number(value);
  return Number.isFinite(number) ? `${number.toFixed(1)}%` : "--";
}

function formatBytes(value) {
  const number = Number(value);
  if (!Number.isFinite(number) || number < 0) {
    return "--";
  }
  const units = ["B", "KB", "MB", "GB", "TB"];
  let scaled = number;
  let unitIndex = 0;
  while (scaled >= 1024 && unitIndex < units.length - 1) {
    scaled /= 1024;
    unitIndex += 1;
  }
  const digits = unitIndex <= 1 ? 0 : 1;
  return `${scaled.toFixed(digits)} ${units[unitIndex]}`;
}

function formatDuration(seconds) {
  const totalSeconds = Number(seconds);
  if (!Number.isFinite(totalSeconds) || totalSeconds < 0) {
    return "--";
  }
  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  if (days > 0) {
    return `${days} 天 ${hours} 时`;
  }
  if (hours > 0) {
    return `${hours} 时 ${minutes} 分`;
  }
  return `${minutes} 分`;
}

function formatStatus(status, running) {
  if (status === "reconnecting") {
    return STATUS_LABELS.reconnecting;
  }
  if (running) {
    return "运行中";
  }
  return STATUS_LABELS[status] || "已停止";
}

// 预处理路径 = **两段**，别混成一个数：
//   第 1 段 缩放/裁剪（2560×1440 → 模型输入）：RGA 硬件，耗时 = rga_ms（core 的 resize_ms）
//   第 2 段 RGA 输出 → NPU 输入：INT8/xor_shift128 不能零拷贝直绑 ⇒ 每帧一次 CPU XOR 拷贝，
//          耗时 = rknn_set_input_ms；只有 external_dma_bound 为真时才是 DMA 直绑（不算 CPU）
// 只显示第 1 段会让人以为整条预处理都是硬件干的，业主就是据此说"应该是 cpu 直拷"。
function formatPreprocessBackend(latencyState, detection) {
  const backend = latencyState && latencyState.raw_preprocess_backend;
  if (!backend) return "-";

  // 第 2 段：直绑生效 = DMA，否则必然是 CPU 直拷。布尔值由 core 判定，这里只选文案。
  const modelInput = detection && detection.model_input;
  const feed = modelInput && modelInput.external_dma_bound
    ? "DMA直绑"
    : `CPU直拷 ${formatNumber(latencyState.rknn_set_input_ms)} ms`;

  if (backend === "rga") {
    return `RGA ${formatNumber(latencyState.rga_ms)} · ${feed}`;
  }
  if (backend === "cpu_direct") {
    return `CPU直拷 ${formatNumber(latencyState.rga_ms)} · ${feed}`;
  }
  if (backend === "cpu_fallback") {
    // 不拼耗时：CPU 回退时 rga_ms 不累加（core 只在 using_rga() 为真时统计），
    // 拼上去会显示 0，看着像"预处理没花时间"。
    return `CPU回退（RGA 不可用） · ${feed}`;
  }
  if (backend === "failed") {
    const reason = latencyState && latencyState.raw_preprocess_error;
    const head = reason ? `硬件预处理失败：${reason}` : "硬件预处理失败";
    return `${head} · ${feed}`;
  }
  return "-";
}


function modelProfileLabel(value) {
  if (!value || value === "generic") {
    return "通用";
  }
  return value.replace(/_/g, " ").toUpperCase();
}

function modelFileName(model) {
  const value = (model && (model.display_name || model.file_name || model.id)) || "";
  const parts = String(value).split(/[\\/]/);
  return parts[parts.length - 1] || value || "未命名模型";
}

function modelDimension(model) {
  const width = Number(model && model.input_width);
  const height = Number(model && model.input_height);
  if (Number.isFinite(width) && Number.isFinite(height) && width > 0 && height > 0) {
    return `${width}x${height}`;
  }

  const source = [
    model && model.id,
    model && model.display_name,
    model && model.file_name,
  ].filter(Boolean).join(" ");
  const pair = source.match(/(?:^|[_\-\s])(\d{3,4})\s*[xX×]\s*(\d{3,4})(?=$|[_\-\s.])/);
  if (pair) {
    return `${pair[1]}x${pair[2]}`;
  }

  const candidates = Array.from(source.matchAll(/(?:^|[_\-\s])(\d{3,4})(?=$|[_\-\s.])/g))
    .map((match) => Number(match[1]))
    .filter((value) => Number.isFinite(value) && value >= 128 && value <= 2048);
  if (candidates.length > 0) {
    return `${candidates[0]}x${candidates[0]}`;
  }

  return "尺寸待检测";
}

function modelInputCropSize(model) {
  const width = Number(model && model.input_width);
  const height = Number(model && model.input_height);
  if (Number.isFinite(width) && Number.isFinite(height) && width > 0 && height > 0) {
    return normalizeCropSize(Math.max(width, height));
  }

  const dimension = modelDimension(model);
  const square = String(dimension).match(/^(\d{3,4})x\1$/);
  return square ? normalizeCropSize(Number(square[1])) : null;
}

function modelOutputLabel(model) {
  const outputCount = Number(model && model.output_count);
  const classCount = Number(model && model.class_count);
  const parts = [];
  if (Number.isFinite(outputCount) && outputCount > 0) {
    parts.push(`${outputCount} 路输出`);
  }
  if (Number.isFinite(classCount) && classCount > 0) {
    parts.push(`${classCount} 类`);
  }
  return parts.length > 0 ? parts.join(" / ") : "输出待检测";
}

function modelBackend(model) {
  const backend = String((model && model.backend) || "").trim().toLowerCase();
  if (backend === "hailo" || backend === "hef") {
    return "hailo";
  }
  const fileName = String((model && model.file_name) || "").toLowerCase();
  return fileName.endsWith(".hef") || fileName.endsWith(".hef.enc") ? "hailo" : "rknn";
}

function modelBackendLabel(model) {
  return modelBackend(model) === "hailo" ? "Hailo" : "RKNN";
}

function modelBackendFilterValue(model) {
  return modelBackend(model) === "hailo" ? "hef" : "rknn";
}

function modelClassLabel(model) {
  const classCount = Number(model && model.class_count);
  if (Number.isFinite(classCount) && classCount > 0) {
    return `${classCount} 类`;
  }
  const description = String((model && model.description) || "");
  const match = description.match(/(\d+)\s*(?:类|类别)/);
  if (match) {
    return `${match[1]} 类`;
  }
  return "类别待检测";
}

function modelListSignature(models) {
  return (models || []).map((model) => [
    model.id,
    model.backend || "",
    model.display_name,
    model.file_name,
    model.input_width || 0,
    model.input_height || 0,
    model.output_count || 0,
    model.class_count || 0,
    model.hailo_pipeline_depth || 0,
    Array.isArray(model.class_names) ? model.class_names.join(",") : "",
    model.game_profile || "",
    model.preset_name || "",
    model.description || "",
  ].join("|")).join(";");
}

function currentModelImportType(form) {
  const selected = form ? form.querySelector('input[name="model_type"]:checked') : null;
  if (!selected) {
    return "rknn";
  }
  return selected.value === "onnx" ? "onnx" : "rknn";
}

function setExportPresetLink(name) {
  const link = $("exportPresetButton");
  if (!link) {
    return;
  }
  if (!name) {
    link.href = "#";
    link.classList.add("is-disabled");
    return;
  }
  link.href = presetExportUrl(name);
  link.classList.remove("is-disabled");
}

function presetExportUrl(name) {
  return `/api/presets/${encodeURIComponent(name)}/export`;
}

function normalizeCropSize(value, fallback = 320) {
  const hasValue = value !== undefined && value !== null &&
    (typeof value !== "string" || value.trim() !== "");
  const numericValue = hasValue ? Number(value) : NaN;
  const hasFallback = fallback !== undefined && fallback !== null &&
    (typeof fallback !== "string" || fallback.trim() !== "");
  const fallbackNumber = hasFallback ? Number(fallback) : 320;
  const fallbackValue = Number.isFinite(fallbackNumber) ? fallbackNumber : 320;
  const safeValue = Number.isFinite(numericValue) ? numericValue : fallbackValue;
  return Math.round(clamp(safeValue, CROP_SIZE_MIN, CROP_SIZE_MAX));
}

function nearestCropSizeOptionIndex(value, fallback = 320) {
  const target = normalizeCropSize(value, fallback);
  let closestIndex = 0;
  CROP_SIZE_OPTIONS.forEach((candidate, index) => {
    const closest = CROP_SIZE_OPTIONS[closestIndex];
    if (Math.abs(candidate - target) < Math.abs(closest - target)) {
      closestIndex = index;
    }
  });
  return closestIndex;
}

function syncCropSizePresetRange(value) {
  const range = $("capture_crop_size_range");
  if (!range) {
    return;
  }
  range.min = "0";
  range.max = String(Math.max(0, CROP_SIZE_OPTIONS.length - 1));
  range.step = "1";
  range.value = String(nearestCropSizeOptionIndex(value));
}

function setCropSizeValue(value) {
  const input = $("capture_crop_size");
  if (!input) {
    return;
  }
  const cropSize = normalizeCropSize(value);
  input.value = formatControlValue(input, cropSize);
  syncCropSizePresetRange(cropSize);
}

function getCropSize(fallback = 320) {
  const input = $("capture_crop_size");
  if (!input) {
    return normalizeCropSize(fallback);
  }
  return normalizeCropSize(input.value, fallback);
}

// V1.0.13：getAimReferenceOffsetLimit / dynamicNumericRangeLimitsForId /
// updateDynamicOffsetControlLimits 三个函数已删（只服务于那四个偏移控件）。

function getPreviewImageLayout(stage, cropSize) {
  if (!stage) {
    return null;
  }
  const stageRect = stage.getBoundingClientRect();
  if (stageRect.width <= 0 || stageRect.height <= 0) {
    return null;
  }

  const preview = $("previewImage");
  const naturalWidth = preview && preview.naturalWidth > 0 ? preview.naturalWidth : cropSize;
  const naturalHeight = preview && preview.naturalHeight > 0 ? preview.naturalHeight : cropSize;
  const imageScale = Math.min(stageRect.width / naturalWidth, stageRect.height / naturalHeight);
  const imageWidth = naturalWidth * imageScale;
  const imageHeight = naturalHeight * imageScale;
  return {
    imageLeft: (stageRect.width - imageWidth) * 0.5,
    imageTop: (stageRect.height - imageHeight) * 0.5,
    imageWidth,
    imageHeight,
    xScale: imageWidth / cropSize,
    yScale: imageHeight / cropSize,
    cropScale: Math.min(imageWidth, imageHeight) / cropSize,
  };
}

function addRangeBinding(numberId, rangeId) {
  if (!numberId || !rangeId || RANGE_BINDINGS.some(([existingNumberId, existingRangeId]) => {
    return existingNumberId === numberId || existingRangeId === rangeId;
  })) {
    return;
  }
  RANGE_BINDINGS.push([numberId, rangeId]);
}

function rangeLimitsForInput(input) {
  const configured = NUMERIC_RANGE_LIMITS[input.id];
  const min = configured ? String(configured[0]) : input.min !== "" ? input.min : "0";
  const max = configured ? String(configured[1]) : input.max !== "" ? input.max : "100";
  const step = input.step && input.step !== "any" ? input.step : "1";
  return { min, max, step };
}

function enhanceNumericRangeControls() {
  document.querySelectorAll("[data-config][type='number']:not([readonly])").forEach((input) => {
    if (!input.id) {
      return;
    }
    const { min, max, step } = rangeLimitsForInput(input);
    input.min = min;
    input.max = max;
    input.step = step;
    if (input.id === "capture_crop_size") {
      syncCropSizePresetRange(input.value || OVERVIEW_DEFAULTS.capture_crop_size);
      const presetRange = $("capture_crop_size_range");
      if (presetRange) {
        enableThumbOnlyRangeInput(presetRange);
      }
      return;
    }

    const rangeId = `${input.id}_range`;
    let rangeInput = $(rangeId);
    if (!rangeInput) {
      rangeInput = document.createElement("input");
      rangeInput.id = rangeId;
      rangeInput.type = "range";
      rangeInput.className = "value-range";
      rangeInput.setAttribute("aria-label", `${input.id} 滑条`);
      input.insertAdjacentElement("afterend", rangeInput);
    }

    rangeInput.min = min;
    rangeInput.max = max;
    rangeInput.step = step;
    const rangeField = input.closest(".field, .slider-field");
    if (rangeField) {
      rangeField.classList.add("range-control-field");
    }
    enableThumbOnlyRangeInput(rangeInput);
    addRangeBinding(input.id, rangeId);
  });
}

function syncAllRangeFields() {
  RANGE_BINDINGS.forEach(([numberId, rangeId]) => {
    const numberInput = $(numberId);
    const rangeInput = $(rangeId);
    if (numberInput && rangeInput && numberInput.value !== "") {
      if (document.activeElement === numberInput) {
        if (Number.isFinite(Number(numberInput.value))) {
          rangeInput.value = formatControlValue(rangeInput, numberInput.value);
        }
        return;
      }
      const formattedValue = formatControlValue(numberInput, numberInput.value);
      numberInput.value = formattedValue;
      rangeInput.value = formatControlValue(rangeInput, formattedValue);
    }
  });
  syncCropSizePresetRange(getCropSize());
}

function initRangeBindings() {
  const cropSizeInput = $("capture_crop_size");
  const cropSizeRange = $("capture_crop_size_range");
  if (cropSizeInput && cropSizeRange) {
    enableThumbOnlyRangeInput(cropSizeRange);
    cropSizeRange.addEventListener("input", () => {
      const index = Math.round(clamp(Number(cropSizeRange.value), 0, CROP_SIZE_OPTIONS.length - 1));
      const cropSize = CROP_SIZE_OPTIONS[index] || OVERVIEW_DEFAULTS.capture_crop_size;
      cropSizeInput.value = formatControlValue(cropSizeInput, cropSize);
      cropSizeRange.value = String(index);
      updateAimRangeOverlay();
    });
    cropSizeRange.addEventListener("change", () => {
      const index = Math.round(clamp(Number(cropSizeRange.value), 0, CROP_SIZE_OPTIONS.length - 1));
      const cropSize = CROP_SIZE_OPTIONS[index] || OVERVIEW_DEFAULTS.capture_crop_size;
      cropSizeInput.value = formatControlValue(cropSizeInput, cropSize);
      cropSizeRange.value = String(index);
      requestConfigApply(70);
    });
    cropSizeInput.addEventListener("input", () => {
      if (cropSizeInput.value !== "" && Number.isFinite(Number(cropSizeInput.value))) {
        syncCropSizePresetRange(cropSizeInput.value);
      }
      updateAimRangeOverlay();
    });
    cropSizeInput.addEventListener("change", () => {
      if (cropSizeInput.value !== "") {
        clampNumberInputToLimits(cropSizeInput);
      }
      updateAimRangeOverlay();
    });
  }
  RANGE_BINDINGS.forEach(([numberId, rangeId]) => {
    const numberInput = $(numberId);
    const rangeInput = $(rangeId);
    if (!numberInput || !rangeInput) {
      return;
    }
    enableThumbOnlyRangeInput(rangeInput);
    const syncRangeToNumber = () => {
      const formattedValue = formatControlValue(numberInput, rangeInput.value);
      numberInput.value = formattedValue;
      rangeInput.value = formatControlValue(rangeInput, formattedValue);
      if (AIM_OVERLAY_CONFIG_IDS.has(numberId)) {
        updateAimRangeOverlay();
      }
    };
    rangeInput.addEventListener("input", syncRangeToNumber);
    rangeInput.addEventListener("change", () => {
      syncRangeToNumber();
      requestConfigApply(70);
    });
    numberInput.addEventListener("input", () => {
      if (numberInput.value !== "") {
        rangeInput.value = formatControlValue(rangeInput, numberInput.value);
      }
      if (AIM_OVERLAY_CONFIG_IDS.has(numberId)) {
        updateAimRangeOverlay();
      }
    });
    numberInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        numberInput.blur();
      }
    });
    numberInput.addEventListener("change", () => {
      if (numberInput.value !== "") {
        clampNumberInputToLimits(numberInput);
        const formattedValue = formatControlValue(numberInput, numberInput.value);
        numberInput.value = formattedValue;
        rangeInput.value = formatControlValue(rangeInput, formattedValue);
      }
      if (AIM_OVERLAY_CONFIG_IDS.has(numberId)) {
        updateAimRangeOverlay();
      }
    });
  });
}

function currentModel() {
  const models = state.data && Array.isArray(state.data.models) ? state.data.models : [];
  const selectedId = (state.config && state.config.model_id) ||
    (state.data && state.data.config && state.data.config.model_id) ||
    "";
  return models.find((model) => model.id === selectedId) || models[0] || null;
}

function findModelById(modelId) {
  const models = state.data && Array.isArray(state.data.models) ? state.data.models : [];
  return models.find((model) => model.id === modelId) || null;
}

function selectedModelDisplayName(payload, runtime, detection) {
  const selectedId = (runtime && runtime.selected_model_id) ||
    (payload && payload.config && payload.config.model_id) ||
    (state.config && state.config.model_id) ||
    (state.data && state.data.config && state.data.config.model_id) ||
    "";
  const models = (payload && Array.isArray(payload.models) ? payload.models : null) ||
    (state.data && Array.isArray(state.data.models) ? state.data.models : []);
  const model = models.find((item) => item.id === selectedId) || null;
  return (detection && detection.model_name) || (model ? modelFileName(model) : "") || selectedId;
}

function currentModelClassCount() {
  const model = currentModel();
  const count = Number(model && model.class_count);
  if (Number.isFinite(count) && count > 0) {
    return Math.min(AIM_CLASS_MAX_COUNT, Math.max(1, Math.floor(count)));
  }
  return 1;
}

function modelClassNames(model) {
  const names = model && Array.isArray(model.class_names) ? model.class_names : [];
  return names.slice(0, AIM_CLASS_MAX_COUNT).map((name) => String(name || "").trim());
}

function currentModelClassNames() {
  return modelClassNames(currentModel());
}

function currentModelClassRenderSignature(modelId = "") {
  const model = currentModel();
  const selectedId = modelId || (model && model.id) || "";
  return `${selectedId}:${currentModelClassCount()}:${currentModelClassNames().join("\u001f")}`;
}

function classDisplayName(classId, names = currentModelClassNames()) {
  const parsedName = names[classId];
  return parsedName ? parsedName : `类别 ${classId}`;
}

function modelClassEditCount(model) {
  const count = Number(model && model.class_count);
  if (Number.isFinite(count) && count > 0) {
    return Math.min(AIM_CLASS_MAX_COUNT, Math.max(1, Math.floor(count)));
  }
  return Math.min(AIM_CLASS_MAX_COUNT, Math.max(1, modelClassNames(model).length || 1));
}

function classMaskForCount(count) {
  const safeCount = Math.min(AIM_CLASS_MAX_COUNT, Math.max(1, Math.floor(Number(count) || 1)));
  return safeCount >= AIM_CLASS_MAX_COUNT ? AIM_CLASS_ALL_MASK : ((1 << safeCount) - 1);
}

function currentModelClassMask() {
  return classMaskForCount(currentModelClassCount());
}

function classMaskFromProfile(profile) {
  const mask = Number(profile && profile.class_filter_mask);
  const visibleMask = currentModelClassMask();
  if (!Number.isFinite(mask) || mask < 0) {
    return visibleMask;
  }
  const normalizedMask = mask & visibleMask;
  if (mask > 0 && normalizedMask === 0) {
    return visibleMask;
  }
  return normalizedMask;
}

function clampAimProfileOffset(value, fallback = 0) {
  const number = Number(value);
  return clamp(
    Number.isFinite(number) ? number : fallback,
    AIM_PROFILE_AXIS_OFFSET_MIN,
    AIM_PROFILE_AXIS_OFFSET_MAX
  );
}

function legacyAimPosToOffsetY(pos) {
  return clampAimProfileOffset(pos, 0.5);
}

function aimOffsetYToLegacyPos(offsetY) {
  return clampAimProfileOffset(offsetY, 0.5);
}

function aimProfileOffsetX(profile) {
  return clampAimProfileOffset(profile && profile.offset_x !== undefined ? profile.offset_x : 0.5, 0.5);
}

function aimProfileOffsetY(profile) {
  if (profile && profile.offset_y !== undefined) {
    return clampAimProfileOffset(profile.offset_y, 0.5);
  }
  return legacyAimPosToOffsetY(profile && profile.pos !== undefined ? profile.pos : 0.5);
}

function aimProfileClassOffsets(profile) {
  const classCount = currentModelClassCount();
  const byClass = new Map();
  const items = Array.isArray(profile && profile.class_offsets) ? profile.class_offsets : [];
  const fallbackX = aimProfileOffsetX(profile);
  const fallbackY = aimProfileOffsetY(profile);
  items.forEach((item) => {
    const classId = Math.floor(Number(item && item.class_id));
    if (!Number.isFinite(classId) || classId < 0 || classId >= classCount) {
      return;
    }
    byClass.set(classId, {
      class_id: classId,
      offset_x: clampAimProfileOffset(item.offset_x, fallbackX),
      offset_y: clampAimProfileOffset(item.offset_y, fallbackY),
    });
  });
  return Array.from(byClass.values()).sort((a, b) => a.class_id - b.class_id);
}

function aimProfileClassOffset(profile, classId) {
  return aimProfileClassOffsets(profile).find((offset) => offset.class_id === classId) || null;
}

function aimProfileFovScale(profile) {
  const value = Number(profile && profile.fov_scale !== undefined ? profile.fov_scale : 1);
  return clamp(
    Number.isFinite(value) ? value : 1,
    AIM_PROFILE_FOV_SCALE_MIN,
    AIM_PROFILE_FOV_SCALE_MAX
  );
}

// ---- profileTemplate 拆成四段（2026-10-03 S10 刀 5 · 第 1 步）----
// 原函数 122 行：前 52 行「算 7 个值 + 生成 classPickers」，后 68 行「拼 HTML」。
// ★ v1 手写传参列表漏了 offsetX/offsetY ⇒ 运行时报 not defined，
//   而 node --check 与全部 pytest 都发现不了（只有「实际调用并比对输出」能抓）。
//   v2 起：变量列表**从模板串自动扫**，不手写。

// 段 1：算数据（纯计算，不碰 DOM）
function profileTemplateValues(profile) {
  const classCount = currentModelClassCount();
  const classNames = currentModelClassNames();
  const mask = classMaskFromProfile(profile);
  const offsetX = aimProfileOffsetX(profile).toFixed(2);
  const offsetY = aimProfileOffsetY(profile).toFixed(2);
  const sensitivity = clamp(Number(profile && profile.sensitivity !== undefined ? profile.sensitivity : 1), 0.1, 3).toFixed(2);
  const fovScale = aimProfileFovScale(profile).toFixed(2);

  return {
    classCount,
    // ★ mask 也要返回：classPickers 用它算每个类别的勾选态
    mask,
    fovScale,
    offsetX,
    offsetY,
    sensitivity,
  };
}

// 段 2：生成类别选择器（每个类别一枚 chip）
//   ★ 需要 offsetX / offsetY：无类别专属偏移时回落到全局值（原来它们是外层 const）
function profileTemplateClassPickers(profile, classCount, classNames, mask, offsetX, offsetY) {
  const classPickers = Array.from({ length: classCount }, (_, classId) => {
    const checked = mask & (1 << classId) ? "checked" : "";
    const className = classDisplayName(classId, classNames);
    const classLabel = `${classId} ${className}`;
    const classOffset = aimProfileClassOffset(profile, classId);
    const classOffsetEnabled = Boolean(classOffset);
    const classOffsetX = (classOffset ? classOffset.offset_x : Number(offsetX)).toFixed(2);
    const classOffsetY = (classOffset ? classOffset.offset_y : Number(offsetY)).toFixed(2);
    const classOffsetActiveClass = classOffsetEnabled ? " has-class-offset" : "";
    const classOffsetEnabledValue = classOffsetEnabled ? "1" : "0";
    return `
      <div class="class-chip aim-class-chip${classOffsetActiveClass}" title="${escapeAttr(classLabel)}" data-class-id="${classId}" data-class-offset-enabled="${classOffsetEnabledValue}" data-class-offset-x="${classOffsetX}" data-class-offset-y="${classOffsetY}">
        <label class="class-chip-label">
          <input class="aim-profile-class" type="checkbox" data-class-id="${classId}" ${checked}>
          <span class="class-chip-name">${escapeHtml(classLabel)}</span>
        </label>
        <button class="class-offset-button" type="button" title="类别专属偏移" aria-label="${escapeAttr(`${classLabel} 类别专属偏移`)}">
          <svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7Z"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06A1.65 1.65 0 0 0 15 19.4a1.65 1.65 0 0 0-1 .6V20a2 2 0 0 1-4 0v-.1a1.65 1.65 0 0 0-1-.6 1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.6 15a1.65 1.65 0 0 0-.6-1H4a2 2 0 0 1 0-4h.1a1.65 1.65 0 0 0 .6-1 1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.6a1.65 1.65 0 0 0 1-.6V4a2 2 0 0 1 4 0v.1a1.65 1.65 0 0 0 1 .6 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9c.14.33.35.62.6 1H20a2 2 0 0 1 0 4h-.1a1.65 1.65 0 0 0-.5 1Z"/></svg>
        </button>
        <div class="class-offset-popover" hidden>
          <div class="class-offset-popover-head">
            <strong>${escapeHtml(classLabel)}</strong>
            <button class="mini-button class-offset-clear" type="button">清除</button>
          </div>
          <div class="class-offset-axis-grid">
            <div class="slider-field class-offset-axis-field">
              <div class="label-row">
                <label>X轴</label>
                <input class="aim-profile-class-offset-x value-input" type="number" min="0" max="1" step="0.01" autocomplete="off" value="${classOffsetX}">
              </div>
              <input class="aim-profile-class-offset-x-range" type="range" min="0" max="1" step="0.01" value="${classOffsetX}">
            </div>
            <div class="slider-field class-offset-axis-field">
              <div class="label-row">
                <label>Y轴</label>
                <input class="aim-profile-class-offset-y value-input" type="number" min="0" max="1" step="0.01" autocomplete="off" value="${classOffsetY}">
              </div>
              <input class="aim-profile-class-offset-y-range" type="range" min="0" max="1" step="0.01" value="${classOffsetY}">
            </div>
          </div>
        </div>
      </div>
    `;
  }).join("");
  return classPickers;
}

// 段 3：拼 HTML 模板
function profileTemplateHtml(v, classPickers, index) {
  const { classCount, fovScale, offsetX, offsetY, sensitivity } = v;
  return `
    <div class="aim-profile-head">
      <strong>热键 ${index + 1}</strong>
      <button class="mini-danger" type="button" data-remove-profile>删除</button>
    </div>
    <div class="form-subgrid">
      <div class="aim-profile-hotkey-grid span-all">
        <div class="hotkey-pair-grid">
          <label class="field">主按键
            <select class="aim-profile-hotkey"></select>
          </label>
          <label class="field">副按键
            <select class="aim-profile-hotkey2"></select>
          </label>
        </div>
        <label class="field">触发方式
          <select class="aim-profile-hotkey-mode">
            <option value="any">任一按键</option>
            <option value="all">同时按下</option>
          </select>
          <span class="field-hint">副按键可选不使用；选择副按键后决定任一触发还是同时按下触发。</span>
        </label>
      </div>
      <div class="slider-field span-all">
        <div class="label-row">
          <label>热键移动倍率</label>
          <input class="aim-profile-sensitivity value-input" type="number" min="0.1" max="3" step="0.01" autocomplete="off" value="${sensitivity}">
        </div>
        <input class="aim-profile-sensitivity-range" type="range" min="0.1" max="3" step="0.01" value="${sensitivity}">
        <span class="field-hint">当前热键生效时，在全局移动倍率之后再乘这个速度倍率。</span>
      </div>
      <div class="slider-field span-all">
        <div class="label-row">
          <label>热键 FOV 缩放</label>
          <input class="aim-profile-fov-scale value-input" type="number" min="0.1" max="1" step="0.01" autocomplete="off" value="${fovScale}">
        </div>
        <input class="aim-profile-fov-scale-range" type="range" min="0.1" max="1" step="0.01" value="${fovScale}">
        <span class="field-hint">在总览 FOV 半径上再乘这个倍率，只对本组热键生效。</span>
      </div>
      <div class="aim-profile-axis-grid span-all">
        <div class="slider-field aim-profile-axis-field">
          <div class="label-row">
            <label>X轴偏移</label>
            <input class="aim-profile-offset-x value-input" type="number" min="0" max="1" step="0.01" autocomplete="off" value="${offsetX}">
          </div>
          <input class="aim-profile-offset-x-range" type="range" min="0" max="1" step="0.01" value="${offsetX}">
          <span class="field-hint aim-profile-axis-hint">0 左 · 0.5 中 · 1 右</span>
        </div>
        <div class="slider-field aim-profile-axis-field">
          <div class="label-row">
            <label>Y轴偏移</label>
            <input class="aim-profile-offset-y value-input" type="number" min="0" max="1" step="0.01" autocomplete="off" value="${offsetY}">
          </div>
          <input class="aim-profile-offset-y-range" type="range" min="0" max="1" step="0.01" value="${offsetY}">
          <span class="field-hint aim-profile-axis-hint">0 上 · 0.5 中 · 1 下</span>
        </div>
      </div>
      <div class="class-picker span-all">
        <div class="class-picker-head">
          <span>目标类别 · ${classCount} 类</span>
          <button class="mini-button" type="button" data-select-all-classes>全选</button>
        </div>
        <div class="class-chip-grid">
          ${classPickers}
        </div>
      </div>
    </div>
  `;
}

// 段 4：编排（对外接口与原来完全一致）
function profileTemplate(profile, index) {
  const v = profileTemplateValues(profile);
  const classPickers = profileTemplateClassPickers(
    profile, v.classCount, v.classNames, v.mask, v.offsetX, v.offsetY);
  return profileTemplateHtml(
v, classPickers, index
  );
}

function renumberAimProfiles() {
  document.querySelectorAll(".aim-profile-card").forEach((card, index) => {
    const title = card.querySelector(".aim-profile-head strong");
    if (title) {
      title.textContent = `热键 ${index + 1}`;
    }
  });
}

function updateClassToggleButton(card) {
  const button = card.querySelector("[data-select-all-classes]");
  if (!button) {
    return;
  }
  const checkboxes = Array.from(card.querySelectorAll(".aim-profile-class"));
  const allChecked = checkboxes.length > 0 && checkboxes.every((checkbox) => checkbox.checked);
  button.textContent = allChecked ? "取消全选" : "全选";
}

function bindProfileNumberRange(card, numberSelector, rangeSelector, fallback, min, max, digits = 2) {
  const range = card.querySelector(rangeSelector);
  const number = card.querySelector(numberSelector);
  if (!range || !number) {
    return;
  }
  enableThumbOnlyRangeInput(range);
  range.addEventListener("input", () => {
    number.value = range.value;
  });
  range.addEventListener("change", () => {
    number.value = range.value;
    requestConfigApply(70);
  });
  number.addEventListener("input", () => {
    if (number.value !== "") {
      range.value = number.value;
    }
  });
  number.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      number.blur();
    }
  });
  number.addEventListener("change", () => {
    if (number.value !== "") {
      const formatted = clamp(Number(number.value || fallback), min, max).toFixed(digits);
      number.value = formatted;
      range.value = formatted;
    }
    requestConfigApply(90);
  });
}

function closeClassOffsetPopovers(scope = document, except = null) {
  const root = scope && scope.querySelectorAll ? scope : document;
  root.querySelectorAll(".class-offset-popover").forEach((popover) => {
    if (popover === except) {
      return;
    }
    popover.hidden = true;
    const chip = popover.closest(".class-chip");
    if (chip) {
      chip.classList.remove("class-offset-open");
    }
  });
}

function setClassOffsetChipEnabled(chip, enabled) {
  chip.dataset.classOffsetEnabled = enabled ? "1" : "0";
  chip.classList.toggle("has-class-offset", enabled);
  const button = chip.querySelector(".class-offset-button");
  if (button) {
    button.setAttribute("aria-pressed", enabled ? "true" : "false");
  }
}

function profileDefaultOffsetFromCard(card, axis) {
  const selector = axis === "x" ? ".aim-profile-offset-x" : ".aim-profile-offset-y";
  const input = card.querySelector(selector);
  return clampAimProfileOffset(input ? input.value : 0.5, 0.5);
}

function setClassOffsetChipValue(chip, axis, value) {
  const formatted = clampAimProfileOffset(value, 0.5).toFixed(2);
  const number = chip.querySelector(`.aim-profile-class-offset-${axis}`);
  const range = chip.querySelector(`.aim-profile-class-offset-${axis}-range`);
  if (number) {
    number.value = formatted;
  }
  if (range) {
    range.value = formatted;
  }
  if (axis === "x") {
    chip.dataset.classOffsetX = formatted;
  } else {
    chip.dataset.classOffsetY = formatted;
  }
}

function resetClassOffsetChipToProfileOffset(card, chip) {
  setClassOffsetChipValue(chip, "x", profileDefaultOffsetFromCard(card, "x"));
  setClassOffsetChipValue(chip, "y", profileDefaultOffsetFromCard(card, "y"));
  setClassOffsetChipEnabled(chip, false);
}

function updateClassOffsetChipDataset(chip, axis, value, fallback) {
  const number = Number(value);
  if (!Number.isFinite(number)) {
    return false;
  }
  const formatted = clampAimProfileOffset(number, fallback).toFixed(2);
  if (axis === "x") {
    chip.dataset.classOffsetX = formatted;
  } else {
    chip.dataset.classOffsetY = formatted;
  }
  return true;
}

function bindClassOffsetAxis(card, chip, axis) {
  const number = chip.querySelector(`.aim-profile-class-offset-${axis}`);
  const range = chip.querySelector(`.aim-profile-class-offset-${axis}-range`);
  if (!number || !range) {
    return;
  }
  enableThumbOnlyRangeInput(range);
  const enableWithValue = (value) => {
    const fallback = profileDefaultOffsetFromCard(card, axis);
    updateClassOffsetChipDataset(chip, axis, value, fallback);
    setClassOffsetChipEnabled(chip, true);
  };
  range.addEventListener("input", () => {
    setClassOffsetChipValue(chip, axis, range.value);
    setClassOffsetChipEnabled(chip, true);
  });
  range.addEventListener("change", () => {
    setClassOffsetChipValue(chip, axis, range.value);
    setClassOffsetChipEnabled(chip, true);
    requestConfigApply(70);
  });
  number.addEventListener("input", () => {
    if (number.value !== "") {
      range.value = number.value;
    }
    if (!isPendingNumberText(number.value)) {
      enableWithValue(number.value);
    }
  });
  number.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      number.blur();
    }
  });
  number.addEventListener("change", () => {
    const fallback = profileDefaultOffsetFromCard(card, axis);
    const formatted = clampAimProfileOffset(number.value, fallback).toFixed(2);
    number.value = formatted;
    range.value = formatted;
    enableWithValue(formatted);
    requestConfigApply(90);
  });
}

function bindClassOffsetChip(card, chip) {
  if (chip.dataset.classOffsetBound === "1") {
    return;
  }
  chip.dataset.classOffsetBound = "1";
  const button = chip.querySelector(".class-offset-button");
  const popover = chip.querySelector(".class-offset-popover");
  const clearButton = chip.querySelector(".class-offset-clear");
  if (!button || !popover) {
    return;
  }
  button.setAttribute("aria-pressed", chip.dataset.classOffsetEnabled === "1" ? "true" : "false");
  bindClassOffsetAxis(card, chip, "x");
  bindClassOffsetAxis(card, chip, "y");
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    const shouldOpen = popover.hidden;
    closeClassOffsetPopovers(document, shouldOpen ? popover : null);
    if (shouldOpen) {
      if (chip.dataset.classOffsetEnabled !== "1") {
        resetClassOffsetChipToProfileOffset(card, chip);
      }
      popover.hidden = false;
      chip.classList.add("class-offset-open");
    } else {
      popover.hidden = true;
      chip.classList.remove("class-offset-open");
    }
  });
  popover.addEventListener("click", (event) => {
    event.stopPropagation();
  });
  if (clearButton) {
    clearButton.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      resetClassOffsetChipToProfileOffset(card, chip);
      requestConfigApply(80);
    });
  }
}

function bindAimProfileCard(card, profile) {
  const hotkeySelect = card.querySelector(".aim-profile-hotkey");
  const hotkey2Select = card.querySelector(".aim-profile-hotkey2");
  const hotkeyModeSelect = card.querySelector(".aim-profile-hotkey-mode");
  // 主按键只给真实鼠标键：HOTKEYS 里的 "auto" 后端没有对应位掩码，
  // 存下去会被当作缺省值（右键）—— 看着选了「自动」其实变成右键，属于静默改配置。
  fillOptions(hotkeySelect, POINTER_HOTKEYS);
  fillOptions(hotkey2Select, OPTIONAL_POINTER_HOTKEYS);
  hotkeySelect.value = profile && POINTER_HOTKEYS.includes(profile.hotkey) ? profile.hotkey : "left";
  hotkey2Select.value = profile && OPTIONAL_POINTER_HOTKEYS.includes(profile.hotkey2) ? profile.hotkey2 : "";
  hotkeyModeSelect.value = profile && profile.hotkey_mode === "all" ? "all" : "any";
  hotkeySelect.addEventListener("change", () => requestConfigApply(80));
  hotkey2Select.addEventListener("change", () => requestConfigApply(80));
  hotkeyModeSelect.addEventListener("change", () => requestConfigApply(80));

  bindProfileNumberRange(card, ".aim-profile-offset-x", ".aim-profile-offset-x-range", 0.5, 0, 1);
  bindProfileNumberRange(card, ".aim-profile-offset-y", ".aim-profile-offset-y-range", 0.5, 0, 1);
  bindProfileNumberRange(card, ".aim-profile-sensitivity", ".aim-profile-sensitivity-range", 1, 0.1, 3);
  bindProfileNumberRange(card, ".aim-profile-fov-scale", ".aim-profile-fov-scale-range", 1, 0.1, 1);

  card.querySelectorAll(".aim-profile-class").forEach((checkbox) => {
    checkbox.addEventListener("change", () => {
      updateClassToggleButton(card);
      requestConfigApply(80);
    });
  });
  card.querySelectorAll(".class-chip").forEach((chip) => {
    bindClassOffsetChip(card, chip);
  });

  card.querySelector("[data-remove-profile]").addEventListener("click", () => {
    card.remove();
    renumberAimProfiles();
    requestConfigApply(80);
  });

  card.querySelector("[data-select-all-classes]").addEventListener("click", () => {
    const checkboxes = Array.from(card.querySelectorAll(".aim-profile-class"));
    const shouldCheck = !checkboxes.every((checkbox) => checkbox.checked);
    checkboxes.forEach((checkbox) => {
      checkbox.checked = shouldCheck;
    });
    updateClassToggleButton(card);
    requestConfigApply(80);
  });
  updateClassToggleButton(card);
}

function addAimProfileCard(profile = {}, emitChange = true) {
  const editor = $("aimProfilesEditor");
  if (!editor) {
    return;
  }
  const index = editor.querySelectorAll(".aim-profile-card").length;
  const card = document.createElement("div");
  card.className = "aim-profile-card";
  card.innerHTML = profileTemplate(profile, index);
  editor.appendChild(card);
  bindAimProfileCard(card, profile);
  if (emitChange && state.configReady) {
    requestConfigApply(80);
  }
}

// 单张卡的键位并集（主 ∪ 副）。未知 / 空值按 0 算。
function aimProfileKeyBits(hotkey, hotkey2) {
  const a = AIM_HOTKEY_BITS[String(hotkey || "").trim().toLowerCase()] || 0;
  const b = AIM_HOTKEY_BITS[String(hotkey2 || "").trim().toLowerCase()] || 0;
  return a | b;
}

// 档位表冲突校验：返回人话原因，没问题返回 ""。
// 规则与后端 validate_aim_profiles、core aim_profiles_overlap 同一口径：
//   ① 至少留一组、最多 AIM_PROFILE_MAX 组；
//   ② 每组必须有主按键；副按键不能与主按键相同；「同时按下」必须有副按键；
//   ③ 任意两组的键位并集必须互斥（按位与 == 0）—— **同一个键不能给两组**。
//     这条挡的是"单个键按下时判不出该用哪档"的配错，所以保存必须报错。
// ★ 互斥不保证选档唯一：同时按下两档各自的键（左键开火 + 右键瞄准）会两档都命中，
//   物理上禁不掉。那时 core 取**面板顺序里靠上的那张卡**，所以卡片顺序有语义，
//   别随手拖动 —— 想换优先级就换位置。
function aimProfilesConflict() {
  const cards = Array.from(document.querySelectorAll("#aimProfilesEditor .aim-profile-card"));
  if (cards.length === 0) {
    return "至少要保留一组热键";
  }
  if (cards.length > AIM_PROFILE_MAX) {
    return `热键最多 ${AIM_PROFILE_MAX} 组，当前 ${cards.length} 组`;
  }
  const names = {};
  Object.keys(AIM_HOTKEY_BITS).forEach((k) => { names[AIM_HOTKEY_BITS[k]] = HOTKEY_LABELS[k] || k; });
  const masks = [];
  for (let i = 0; i < cards.length; i += 1) {
    const hotkey = cards[i].querySelector(".aim-profile-hotkey");
    const hotkey2 = cards[i].querySelector(".aim-profile-hotkey2");
    const mode = cards[i].querySelector(".aim-profile-hotkey-mode");
    const a = aimProfileKeyBits(hotkey && hotkey.value, "");
    const b = aimProfileKeyBits(hotkey2 && hotkey2.value, "");
    if (a === 0) {
      return `热键 ${i + 1}：请选择主按键`;
    }
    if ((a & b) !== 0) {
      return `热键 ${i + 1}：副按键不能与主按键相同（同一个键等于没按）`;
    }
    if (mode && mode.value === "all" && b === 0) {
      return `热键 ${i + 1}：触发方式选了「同时按下」，必须再选一个副按键`;
    }
    masks.push(a | b);
  }
  for (let i = 0; i < masks.length; i += 1) {
    for (let j = i + 1; j < masks.length; j += 1) {
      const dup = masks[i] & masks[j];
      if (dup !== 0) {
        const dupNames = Object.keys(names)
          .filter((bit) => dup & Number(bit))
          .map((bit) => names[bit])
          .join("、");
        return `热键 ${i + 1} 与热键 ${j + 1} 不能共用按键（重复：${dupNames}）。`
          + "一个按键只能归一组 —— 请换一个键，或删掉其中一组";
      }
    }
  }
  return "";
}

// 新增卡片时挑一个还没被占用的主按键，避免刚点「添加」就撞车报错。
// 全占满了就退回 "left"（那时校验会直接拦下来，提示用户换键或删卡）。
function nextFreeAimHotkey() {
  let taken = 0;
  document.querySelectorAll("#aimProfilesEditor .aim-profile-card").forEach((card) => {
    const a = card.querySelector(".aim-profile-hotkey");
    const b = card.querySelector(".aim-profile-hotkey2");
    taken |= aimProfileKeyBits(a && a.value, b && b.value);
  });
  for (let i = 0; i < POINTER_HOTKEYS.length; i += 1) {
    const bit = AIM_HOTKEY_BITS[POINTER_HOTKEYS[i]] || 0;
    if (bit !== 0 && (taken & bit) === 0) {
      return POINTER_HOTKEYS[i];
    }
  }
  return "left";
}

function renderAimProfiles(profiles) {
  const editor = $("aimProfilesEditor");
  if (!editor) {
    return;
  }
  editor.innerHTML = "";
  const safeProfiles = Array.isArray(profiles) && profiles.length > 0
    ? profiles
    : [{
      hotkey: "left",
      class_filter_mask: currentModelClassMask(),
      offset_x: defaultAimProfileOffsetX(),
      offset_y: defaultAimProfileOffsetY(),
      sensitivity: 1,
      fov_scale: 1,
    }];
  safeProfiles.forEach((profile) => addAimProfileCard(profile, false));
  state.aimClassRenderSignature = currentModelClassRenderSignature();
}

function defaultAimProfileOffsetX() {
  const firstProfileOffsetX = document.querySelector(".aim-profile-offset-x");
  if (firstProfileOffsetX && firstProfileOffsetX.value !== "") {
    const value = Number(firstProfileOffsetX.value);
    if (Number.isFinite(value)) {
      return clampAimProfileOffset(value, 0.5);
    }
  }
  return 0.5;
}

function defaultAimProfileOffsetY() {
  const firstProfileOffsetY = document.querySelector(".aim-profile-offset-y");
  if (firstProfileOffsetY && firstProfileOffsetY.value !== "") {
    const value = Number(firstProfileOffsetY.value);
    if (Number.isFinite(value)) {
      return clampAimProfileOffset(value, 0.5);
    }
  }
  return legacyAimPosToOffsetY(getNumber("pos", 0.5));
}

function collectAimProfiles() {
  const editor = $("aimProfilesEditor");
  if (!editor) {
    return [{
      hotkey: "left",
      class_filter_mask: currentModelClassMask(),
      offset_x: defaultAimProfileOffsetX(),
      offset_y: defaultAimProfileOffsetY(),
      class_offsets: [],
      sensitivity: 1,
      fov_scale: 1,
    }];
  }
  const profiles = Array.from(editor.querySelectorAll(".aim-profile-card")).map((card) => {
    let mask = 0;
    card.querySelectorAll(".aim-profile-class").forEach((checkbox) => {
      if (checkbox.checked) {
        mask |= 1 << Number(checkbox.dataset.classId);
      }
    });
    const offsetX = clampAimProfileOffset(card.querySelector(".aim-profile-offset-x").value, 0.5);
    const offsetY = clampAimProfileOffset(card.querySelector(".aim-profile-offset-y").value, 0.5);
    const classOffsets = Array.from(card.querySelectorAll(".class-chip[data-class-id]"))
      .filter((chip) => chip.dataset.classOffsetEnabled === "1")
      .map((chip) => {
        const classId = Math.floor(Number(chip.dataset.classId));
        if (!Number.isFinite(classId) || classId < 0 || classId >= currentModelClassCount()) {
          return null;
        }
        return {
          class_id: classId,
          offset_x: clampAimProfileOffset(chip.dataset.classOffsetX, offsetX),
          offset_y: clampAimProfileOffset(chip.dataset.classOffsetY, offsetY),
        };
      })
      .filter(Boolean);
    return {
      hotkey: card.querySelector(".aim-profile-hotkey").value || "left",
      hotkey2: card.querySelector(".aim-profile-hotkey2").value || "",
      hotkey_mode: card.querySelector(".aim-profile-hotkey-mode").value || "any",
      class_filter_mask: mask & currentModelClassMask(),
      offset_x: offsetX,
      offset_y: offsetY,
      pos: aimOffsetYToLegacyPos(offsetY),
      class_offsets: classOffsets,
      sensitivity: clamp(Number(card.querySelector(".aim-profile-sensitivity").value || 1), 0.1, 3),
      fov_scale: aimProfileFovScale({
        fov_scale: card.querySelector(".aim-profile-fov-scale").value || 1,
      }),
    };
  });

  if (profiles.length === 0) {
    return [{
      hotkey: "left",
      class_filter_mask: currentModelClassMask(),
      offset_x: defaultAimProfileOffsetX(),
      offset_y: defaultAimProfileOffsetY(),
      class_offsets: [],
      sensitivity: 1,
      fov_scale: 1,
    }];
  }
  return profiles;
}

function bbCtrlId(prefix, field) {
  return `${prefix}_${field}`;
}

function bbModule(prefix) {
  const found = BB_CTRL_MODULES.find(([name]) => name === prefix);
  return found ? found[1] : [];
}

function bbReadField(prefix, field, kind, fallback) {
  const id = bbCtrlId(prefix, field);
  if (kind === "b") {
    return getCheckbox(id);
  }
  if (kind === "key") {
    return getString(id);
  }
  return getNumber(id, fallback);
}

function bbWriteField(prefix, field, kind, value) {
  const id = bbCtrlId(prefix, field);
  if (kind === "b") {
    setCheckbox(id, value);
  } else {
    setValue(id, value);
  }
}

// 回填：Core 子对象 → 面板（缺字段补 Core 默认值，保证首次打开显示的就是实际值）
function populateControllerModules(controller) {
  const src = controller || {};
  BB_CTRL_MODULES.forEach(([prefix, fields]) => {
    fields.forEach(([field, kind, dflt]) => {
      const id = bbCtrlId(prefix, field);
      const value = Object.prototype.hasOwnProperty.call(src, id) ? src[id] : dflt;
      bbWriteField(prefix, field, kind, value);
    });
  });
}

// 提交：面板 → body.ai.controller 的扁平键（后端按前缀收成 mouse 下的子对象）
function collectControllerModules() {
  const out = {};
  BB_CTRL_MODULES.forEach(([prefix, fields]) => {
    fields.forEach(([field, kind, dflt]) => {
      out[bbCtrlId(prefix, field)] = bbReadField(prefix, field, kind, dflt);
    });
  });
  return out;
}

function populateForm(config) {
  if (!config) {
    return;
  }
  state.isPopulating = true;
  state.config = config;

  const capture = config.capture || {};
  const ai = config.ai || {};
  const controller = ai.controller || {};
  const recoil = config.recoil || {};
  const hotkeyGuard = config.hotkey_guard || {};
  const mouseOutput = config.mouse_output || {};
  const latency = config.latency || {};

  setValue("capture_device", capture.device);
  setCropSizeValue(capture.crop_size);
  setValue("capture_format_preference", capture.format_preference);
  setCheckbox("capture_nv12_uv_swapped", capture.nv12_uv_swapped);
  setCheckbox("capture_nv12_bt709", capture.nv12_bt709);
  setCheckbox("capture_nv12_full_range", capture.nv12_full_range);
  setValue("video_detection_confidence", config.video_detection_confidence);
  setValue("video_detection_iou", config.video_detection_iou);
  setValue("sens", config.sens);
  setValue("range_factor", config.range_factor);
  setValue("pos", config.pos);
  setCheckbox("hotkey_guard_enabled", hotkeyGuard.enabled ?? HOTKEY_GUARD_DEFAULTS.enabled);
  setValue("hotkey_guard_toggle_hotkey", hotkeyGuard.toggle_hotkey ?? HOTKEY_GUARD_DEFAULTS.toggle_hotkey);
  renderAimProfiles(config.aim_profiles);

  // Kp / Kd / Rate 三项两轴共用：回填取 X 轴的值（权威），保存时同时写回两轴。
  // ★ 预判两轴独立，必须从各自的 _x / _y 回填 —— 否则「面板上调 X 的预判顺带改掉 Y」
  //   那个 bug 会在下一次保存时复活（Y 的预判在 pid1.cpp 里必须是 0）。
  setValue("controller_kp", controller.kp_x ?? CONTROLLER_DEFAULTS.kp);
  setValue("controller_kd", controller.kd_x ?? CONTROLLER_DEFAULTS.kd);
  setValue("controller_predict", controller.predict_x ?? CONTROLLER_DEFAULTS.predict);
  setValue("controller_predict_y", controller.predict_y ?? CONTROLLER_DEFAULTS.predict_y);
  setValue("controller_rate", controller.rate_x ?? CONTROLLER_DEFAULTS.rate);
  setValue("controller_output_deadzone", controller.output_deadzone ?? CONTROLLER_DEFAULTS.output_deadzone);
  setCheckbox("controller_pull_curve_enabled", controller.pull_curve_enabled ?? CONTROLLER_DEFAULTS.pull_curve_enabled);
  setValue("controller_pull_curve_strength", controller.pull_curve_strength ?? CONTROLLER_DEFAULTS.pull_curve_strength);
  setValue("controller_pull_curve_min_distance", controller.pull_curve_min_distance ?? CONTROLLER_DEFAULTS.pull_curve_min_distance);
  setValue("controller_selector_lost_grace_ms", controller.selector_lost_grace_ms ?? CONTROLLER_DEFAULTS.selector_lost_grace_ms);
  setCheckbox("controller_aim_at_head_box", controller.aim_at_head_box ?? CONTROLLER_DEFAULTS.aim_at_head_box);

  // 压枪：开关与触发键走 config.recoil，其余（拉力/速度/上限/渐出/门控）走
  // BB_CTRL_MODULES 的 recoil 前缀，即 controller_recoil_* 之外的扁平键 recoil_*。
  setCheckbox("recoil_enabled", recoil.enabled ?? RECOIL_DEFAULTS.enabled);
  setValue("recoil_hotkey", recoil.hotkey ?? RECOIL_DEFAULTS.hotkey);
  setValue("recoil_hotkey2", recoil.hotkey2 ?? RECOIL_DEFAULTS.hotkey2);
  setValue("recoil_hotkey_mode", recoil.hotkey_mode ?? RECOIL_DEFAULTS.hotkey_mode);

  populateControllerModules(controller);

  setValue("hailo_pipeline_depth", latency.hailo_pipeline_depth ?? 3);

  syncAllRangeFields();
  updateAimRangeOverlay();
  state.configReady = true;
  state.isPopulating = false;
  updateAssistModuleCollapseStates();
}

function syncRangeFieldsForIds(ids) {
  const idSet = new Set(ids);
  RANGE_BINDINGS.forEach(([numberId, rangeId]) => {
    if (!idSet.has(numberId)) {
      return;
    }
    const numberInput = $(numberId);
    const rangeInput = $(rangeId);
    if (numberInput && rangeInput && numberInput.value !== "") {
      const formattedValue = formatControlValue(numberInput, numberInput.value);
      numberInput.value = formattedValue;
      rangeInput.value = formatControlValue(rangeInput, formattedValue);
    }
  });
  if (idSet.has("capture_crop_size")) {
    syncCropSizePresetRange(getCropSize());
  }
}

// 新分区的数值输入不带进 NUMERIC_RANGE_LIMITS 手写表（一百多项，写两遍必错）：
// 统一按 HTML 上已经写好的 min/max 注册夹取范围，只要声明了任意一侧就生效。
function registerNumericLimitsFromMarkup() {
  document.querySelectorAll("[data-config][type='number']").forEach((el) => {
    if (!el.id || NUMERIC_RANGE_LIMITS[el.id]) {
      return;
    }
    const low = parseFloat(el.getAttribute("min"));
    const high = parseFloat(el.getAttribute("max"));
    if (!Number.isFinite(low) && !Number.isFinite(high)) {
      return;
    }
    NUMERIC_RANGE_LIMITS[el.id] = [
      Number.isFinite(low) ? low : -Number.MAX_SAFE_INTEGER,
      Number.isFinite(high) ? high : Number.MAX_SAFE_INTEGER,
    ];
  });
}

function resetFieldDefaults(fieldDefaults) {
  const changedIds = [];
  Object.entries(fieldDefaults).forEach(([id, value]) => {
    const el = $(id);
    if (!el) {
      return;
    }
    if (el.type === "checkbox") {
      setCheckbox(id, value);
    } else if (el.type === "radio") {
      el.checked = !!value;
    } else {
      setValue(id, value);
    }
    changedIds.push(id);
  });
  syncRangeFieldsForIds(changedIds);
}

function resetOverviewDefaults() {
  state.isPopulating = true;
  resetFieldDefaults(overviewDefaults());
  state.isPopulating = false;
  requestConfigApply(0);
  showToast("总览已恢复默认值");
}

function movementDefaultsForSection(sectionId) {
  const controller = MOVEMENT_CONTROL_DEFAULTS.controller;
  const defaultsBySection = {
    "control-section-pid": {
      sens: MOVEMENT_CONTROL_DEFAULTS.sens,
      controller_kp: controller.kp,
      controller_kd: controller.kd,
      controller_predict: controller.predict,
      controller_predict_y: controller.predict_y,
      controller_rate: controller.rate,
      controller_output_deadzone: controller.output_deadzone,
      controller_selector_lost_grace_ms: controller.selector_lost_grace_ms,
      controller_aim_at_head_box: controller.aim_at_head_box,
    },
  };
  return defaultsBySection[sectionId] || defaultsBySection["control-section-pid"];
}

function resetCurrentMovementSectionDefaults() {
  const sectionId = state.activeControlSectionId || "control-section-pid";
  state.isPopulating = true;
  resetFieldDefaults(movementDefaultsForSection(sectionId));
  state.isPopulating = false;
  requestConfigApply(0);
  showToast("当前移动控制子页面已恢复默认值");
}

// 分区里不属于 BB_CTRL_MODULES 的字段（压枪触发键、拉枪曲线）单独列。
function assistDefaultsForSection(sectionId) {
  const out = {};
  (BB_SECTION_MODULES[sectionId] || []).forEach((prefix) => {
    bbModule(prefix).forEach(([field, , dflt]) => {
      out[bbCtrlId(prefix, field)] = dflt;
    });
  });
  const extra = ASSIST_SECTION_EXTRA_DEFAULTS[sectionId];
  return extra ? { ...out, ...extra() } : out;
}

function assistSectionLabel(sectionId) {
  const labels = {
    "assist-section-recoil": "压枪",
    "assist-section-trigger": "自动开火",
    "assist-section-lead": "拉枪曲线",
    "assist-section-selector": "选靶",
  };
  return labels[sectionId] || "当前页";
}

function resetCurrentAssistSectionDefaults() {
  state.isPopulating = true;
  const sectionId = state.activeAssistSectionId || "assist-section-recoil";
  resetFieldDefaults(assistDefaultsForSection(sectionId));
  state.isPopulating = false;
  updateAssistModuleCollapseStates();
  requestConfigApply(0);
  showToast(`${assistSectionLabel(sectionId)}已恢复默认值`);
}

function collectConfig() {
  const previousCapture = state.config && state.config.capture ? state.config.capture : {};
  const previousLatency = state.config && state.config.latency ? state.config.latency : {};
  const selectedModel = (state.config && state.config.model_id) ||
    (state.data && state.data.config && state.data.config.model_id) ||
    "";
  return {
    model_id: selectedModel,
    video_detection_confidence: getNumber("video_detection_confidence", 0.25),
    video_detection_iou: getNumber("video_detection_iou", 0.45),
    capture: {
      device: getString("capture_device") || previousCapture.device || "/dev/video0",
      crop_size: getCropSize(previousCapture.crop_size || 320),
      format_preference: getString("capture_format_preference") || previousCapture.format_preference || "auto",
      nv12_uv_swapped: getCheckbox("capture_nv12_uv_swapped"),
      nv12_bt709: getCheckbox("capture_nv12_bt709"),
      nv12_full_range: getCheckbox("capture_nv12_full_range"),
    },
    latency: {
      mode: previousLatency.mode || "ultra",
      preview_policy: previousLatency.preview_policy || "degrade",
      realtime: previousLatency.realtime || "try",
      preview_interval_ms: Number.isFinite(Number(previousLatency.preview_interval_ms))
        ? Number(previousLatency.preview_interval_ms)
        : 66,
      hailo_pipeline_depth: Math.round(getNumberInRange(
        "hailo_pipeline_depth",
        previousLatency.hailo_pipeline_depth ?? 3
      )),
    },
    mouse_output: {
      mode: "passthrough",
    },
    sens: getNumber("sens", MOVEMENT_CONTROL_DEFAULTS.sens),
    range_factor: getNumber("range_factor", 1),
    pos: getNumber("pos", 0.5),
    hotkey_guard: {
      enabled: getCheckbox("hotkey_guard_enabled"),
      toggle_hotkey: getString("hotkey_guard_toggle_hotkey") || HOTKEY_GUARD_DEFAULTS.toggle_hotkey,
    },
    aim_profiles: collectAimProfiles(),
    ai: {
      controller: {
        // Kp / Kd / Rate 三项 X / Y 共用面板上的同一个输入框：一个框写两份。
        kp_x: getNumber("controller_kp", CONTROLLER_DEFAULTS.kp),
        kp_y: getNumber("controller_kp", CONTROLLER_DEFAULTS.kp),
        kd_x: getNumber("controller_kd", CONTROLLER_DEFAULTS.kd),
        kd_y: getNumber("controller_kd", CONTROLLER_DEFAULTS.kd),
        // ★ 预判两轴各一个输入框 —— 原来 predict_y 读的是 X 那个框，等于面板一调
        //   X 就把 Y 顶成同一个值；而 pid1.cpp 里 Y 的 predict 必须是 0。
        predict_x: getNumber("controller_predict", CONTROLLER_DEFAULTS.predict),
        predict_y: getNumber("controller_predict_y", CONTROLLER_DEFAULTS.predict_y),
        rate_x: getNumber("controller_rate", CONTROLLER_DEFAULTS.rate),
        rate_y: getNumber("controller_rate", CONTROLLER_DEFAULTS.rate),
        output_deadzone: getNumber("controller_output_deadzone", CONTROLLER_DEFAULTS.output_deadzone),
        pull_curve_enabled: getCheckbox("controller_pull_curve_enabled"),
        pull_curve_strength: getNumber("controller_pull_curve_strength", CONTROLLER_DEFAULTS.pull_curve_strength),
        pull_curve_min_distance: getNumber("controller_pull_curve_min_distance", CONTROLLER_DEFAULTS.pull_curve_min_distance),
        selector_lost_grace_ms: getNumber("controller_selector_lost_grace_ms", CONTROLLER_DEFAULTS.selector_lost_grace_ms),
        aim_at_head_box: getCheckbox("controller_aim_at_head_box"),
        // BB 对标模块（表驱动，见 BB_CTRL_MODULES）
        ...collectControllerModules(),
      },
    },
    recoil: {
      enabled: getCheckbox("recoil_enabled"),
      hotkey: getString("recoil_hotkey") || RECOIL_DEFAULTS.hotkey,
      hotkey2: getString("recoil_hotkey2"),
      hotkey_mode: getString("recoil_hotkey_mode") || RECOIL_DEFAULTS.hotkey_mode,
    },
  };
}

function hasInvalidConfigInput() {
  const fields = Array.from(document.querySelectorAll(
    "[data-config][type='number'], .aim-profile-offset-x, .aim-profile-offset-y, .aim-profile-class-offset-x, .aim-profile-class-offset-y, .aim-profile-sensitivity, .aim-profile-fov-scale"
  ));
  return fields.some((field) => {
    const value = field.value.trim();
    if (value === "") {
      return true;
    }
    if (document.activeElement === field && isPendingNumberText(value)) {
      return true;
    }
    return !Number.isFinite(Number(value));
  });
}

function isPendingNumberText(value) {
  const text = String(value || "").trim();
  return text === "" || text === "-" || text === "+" || text === "." ||
    text === "-." || text === "+." || text.endsWith(".") || /[eE][+-]?$/.test(text);
}

function requestApplyForNumberInput(input, delay = 90) {
  if (isPendingNumberText(input.value)) {
    setApplyStatus("pending", "等待数值");
    return;
  }
  clampNumberInputToLimits(input);
  requestConfigApply(delay);
}

function requestConfigApply(delay = 180) {
  if (!state.configReady || state.isPopulating) {
    return;
  }
  clearTimeout(state.applyTimer);
  setApplyStatus("pending", "待同步");
  state.applyTimer = setTimeout(() => applyConfigNow(), delay);
}

async function applyConfigNow() {
  if (!state.configReady) {
    return;
  }
  clearTimeout(state.applyTimer);
  if (hasInvalidConfigInput()) {
    setApplyStatus("pending", "等待数值");
    return;
  }
  // 多档位键位冲突 = 配错了，不能提交。提交上去后端也会返回 400
  // （plugins/web/bin/ttbox-web.py 的 validate_aim_profiles），
  // 在本地先拦住是为了即时给提示，不用等一趟网络往返。
  const profileConflict = aimProfilesConflict();
  if (profileConflict) {
    setApplyStatus("error", "热键冲突");
    if (state.aimProfileConflictNotified !== profileConflict) {
      showToast(profileConflict, true);
      state.aimProfileConflictNotified = profileConflict;
    }
    return;
  }
  state.aimProfileConflictNotified = "";
  if (state.isApplying) {
    state.applyQueued = true;
    return;
  }

  state.isApplying = true;
  setApplyStatus("saving", "同步中");
  try {
    const submittedConfig = collectConfig();
    const result = await api("/api/config", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(submittedConfig),
    });
    if (result && result.config) {
      state.config = result.config;
      if (stableStringify(result.config) !== stableStringify(submittedConfig)) {
        populateForm(result.config);
      }
    }
    if (result && result.state && state.data) {
      state.data = { ...state.data, state: result.state, config: state.config || state.data.config };
      renderRuntime(state.data);
    }
    if (result && Array.isArray(result.models)) {
      const selectedModelId = (state.config && state.config.model_id) || submittedConfig.model_id || "";
      state.data = { ...(state.data || {}), models: result.models, config: state.config || submittedConfig };
      renderModels({ models: result.models, selected_model_id: selectedModelId }, selectedModelId);
    }
    updateAimRangeOverlay();
    const autosaveName = queueCurrentPresetAutosave(state.config || submittedConfig);
    setApplyStatus("ready", autosaveName ? "已同步，保存预设中" : "已同步");
  } catch (error) {
    setApplyStatus("error", "同步失败");
    showToast(error.message || String(error), true);
  } finally {
    state.isApplying = false;
    if (state.applyQueued) {
      state.applyQueued = false;
      requestConfigApply(80);
    }
  }
}

function renderRuntime(payload) {
  if (!payload || !payload.state) {
    return;
  }
  state.data = { ...(state.data || {}), ...payload };
  applyBrand(payload);

  const runtime = payload.state || {};
  const capture = runtime.capture || {};
  const detection = runtime.detection || {};
  const aim = runtime.aim || {};
  const latencyState = runtime.latency || {};
  renderFanControlStatus(runtime.fan_control || {});
  setText("hotkeyGuardRuntimeStatus", aim.hotkeys_suspended ? "已禁用" : "未禁用");
  const isRunning = !!runtime.running;
  const isReconnecting = runtime.status === "reconnecting";
  const license = runtime.license || {};
  const licenseValid = !!license.valid;
  state.licenseStatusLoaded = true;
  setLicenseNavigationLock(!licenseValid);
  const lastError = runtime.last_error || aim.last_error || capture.last_error || "";
  const inferenceMs = isRunning ? detection.inference_ms : 0;
  const inferenceFps = isRunning ? detection.inference_fps : 0;
  const captureFps = isRunning ? capture.capture_fps : 0;
  const detections = isRunning ? detection.detections || 0 : 0;
  const tracks = isRunning ? detection.tracks || 0 : 0;
  // ★ 以下 5 段按「刷哪块 UI」拆开（2026-10-03），主函数只做取数与编排。
  renderRuntimeStatusBadge(runtime, isRunning, isReconnecting, license, licenseValid);
  renderRuntimeSummaryCard(payload, runtime, detection, capture, latencyState,
                           isRunning, lastError);
  renderRuntimePreview(runtime);
  renderRuntimeLatencyFps(latencyState, inferenceMs, inferenceFps, captureFps,
                          detections, tracks);
  renderRuntimePowerButton(runtime, isReconnecting, licenseValid);
}

// ---- 状态徽章 + 授权面板 ----
function renderRuntimeStatusBadge(runtime, isRunning, isReconnecting, license, licenseValid) {
  const badge = $("statusBadge");
  if (badge) {
    badge.textContent = formatStatus(runtime.status, runtime.running);
    badge.className = `status-badge ${runtime.status === "error" || runtime.status === "locked" ? "error" : (runtime.running || isReconnecting) ? "live" : "idle"}`;
  }
  renderLicensePanel({ license, core: runtime.core || {}, version: state.data && state.data.version });
}

// ---- 运行摘要卡（当前模型 / 推理状态 / 采集排队 / 预处理路径 / 最后错误）----
function renderRuntimeSummaryCard(payload, runtime, detection, capture, latencyState, isRunning, lastError) {
  const runtimeSummary = $("runtimeSummary");
  if (runtimeSummary) {
    const modelLabel = selectedModelDisplayName(payload, runtime, detection);
    const preprocessBackend = formatPreprocessBackend(latencyState, detection);
    runtimeSummary.innerHTML = `
      <div class="runtime-stat">
        <span>当前模型</span>
        <strong>${escapeHtml(formatMaybe(modelLabel, "未选择模型"))}</strong>
      </div>
      <div class="runtime-stat">
        <span>推理状态</span>
        <strong>${isRunning ? (detection.model_loaded ? "模型已加载" : "模型未加载") : "已停止"}</strong>
      </div>
      <div class="runtime-stat">
        <span>采集排队</span>
        <strong>${formatNumber(isRunning ? capture.buffer_age_ms : 0)} ms · ${isRunning ? (capture.last_dequeued_count || 0) : 0}/${capture.buffer_count || 0}</strong>
      </div>
      <div class="runtime-stat">
        <span>预处理路径</span>
        <strong>${escapeHtml(preprocessBackend)}</strong>
      </div>
      <div class="runtime-stat">
        <span>最后错误</span>
        <strong>${escapeHtml(lastError || latencyState.raw_preprocess_error || "无")}</strong>
      </div>
    `;
  }
}

// ---- 预览图 + 瞄准范围叠加 ----
function renderRuntimePreview(runtime) {
  const preview = $("previewImage");
  if (preview) {
    const src = runtime.preview_path || "/api/preview.mjpg";
    preview.style.visibility = "visible";
    if (preview.dataset.src !== src) {
      preview.dataset.src = src;
      preview.src = src;
    }
  }
  updateAimRangeOverlay();
}

// ---- 延迟 / 采集帧率 / 推理帧率 / 检出跟踪数 ----
function renderRuntimeLatencyFps(latencyState, inferenceMs, inferenceFps, captureFps, detections, tracks) {
  const latency = $("mobileLatency");
  if (latency) {
    const preprocessToTrackMs = positiveNumber(latencyState.preprocess_to_track_ms);
    const inferenceLatencyMs = positiveNumber(inferenceMs);
    const displayLatencyMs = preprocessToTrackMs ?? inferenceLatencyMs;
    latency.textContent = displayLatencyMs !== null ? `${formatNumber(displayLatencyMs)} ms` : "-- ms";
  }
  const mobileCaptureFps = $("mobileCaptureFps");
  if (mobileCaptureFps) {
    mobileCaptureFps.textContent = `${formatNumber(captureFps)} 帧/秒`;
  }
  const fps = $("mobileFps");
  if (fps) {
    fps.textContent = `${formatNumber(inferenceFps)} 帧/秒`;
  }
  const videoStatus = $("mobileVideoStatus");
  if (videoStatus) {
    videoStatus.textContent = `${detections} 检出 / ${tracks} 跟踪`;
  }
}

// ---- 启停按钮（未激活时禁用并改文案）----
function renderRuntimePowerButton(runtime, isReconnecting, licenseValid) {
  const startButton = $("startButton");
  if (startButton) {
    const shouldStop = runtime.running || runtime.status === "starting" || isReconnecting;
    startButton.disabled = !licenseValid;
    startButton.className = `power-button ${shouldStop ? "stop" : "start"}`;
    const label = startButton.querySelector("strong");
    if (label) {
      label.textContent = !licenseValid ? "未激活" : shouldStop ? "停止" : "启动";
    }
  }
}

function renderSystemStats(payload) {
  const summary = $("systemSummary");
  if (!summary) {
    return;
  }
  if (!payload) {
    summary.innerHTML = `
      <div class="runtime-stat">
        <span>硬件状态</span>
        <strong>读取失败</strong>
      </div>
    `;
    return;
  }

  const memory = payload.memory || {};
  const storage = payload.storage || {};
  const temperature = payload.temperature || {};
  const loadAverage = Array.isArray(payload.load_average) ? payload.load_average : [];
  const cpuTemp = Number(temperature.celsius);
  const loadText = loadAverage.length > 0
    ? loadAverage.map((value) => formatNumber(value, 2)).join(" / ")
    : "--";
  state.systemHostname = payload.hostname || "";
  state.webPort = Number(payload.web_port) || state.webPort || 8080;
  syncLanHostnameInputs(payload);
  syncWebPortInputs(payload);
  const lanUrl = String(payload.lan_url || payload.mdns_url || (payload.lan_ipv4 ? webUrl(payload.lan_ipv4, state.webPort) : "") || "");

  summary.innerHTML = `
    <div class="runtime-stat">
      <span>CPU 占用</span>
      <strong>${formatPercent(payload.cpu_percent)}</strong>
      <small>负载 ${escapeHtml(loadText)}</small>
    </div>
    <div class="runtime-stat">
      <span>内存占用</span>
      <strong>${formatPercent(memory.percent)}</strong>
      <small>${formatBytes(memory.used)} / ${formatBytes(memory.total)}</small>
    </div>
    <div class="runtime-stat">
      <span>CPU 温度</span>
      <strong>${Number.isFinite(cpuTemp) ? `${cpuTemp.toFixed(1)} °C` : "--"}</strong>
      <small>${escapeHtml(temperature.label || "thermal")}</small>
    </div>
    <div class="runtime-stat">
      <span>存储占用</span>
      <strong>${formatPercent(storage.percent)}</strong>
      <small>${formatBytes(storage.used)} / ${formatBytes(storage.total)}</small>
    </div>
    <div class="runtime-stat lan-url-stat">
      <span>局域网 IP</span>
      <strong>${escapeHtml(payload.lan_ipv4 || "--")}</strong>
      <small>${escapeHtml(payload.lan_url || payload.mdns_url || "")}</small>
      <button class="mini-button lan-url-copy-button" type="button" data-copy-lan-url="${escapeAttr(lanUrl)}" ${lanUrl ? "" : "disabled"}>复制</button>
    </div>
    <div class="runtime-stat">
      <span>运行时间</span>
      <strong>${formatDuration(payload.uptime_seconds)}</strong>
      <small>${escapeHtml(payload.hostname || "Orange Pi")}</small>
    </div>
  `;
  // 扩容面板由 /api/system/storage 单独驱动（见 refreshStorageStatus），此处**不碰**：
  // payload.storage 是磁盘占用简表、不含 rootfs，交给 renderStorageExpansion 只会被判成
  // "未读取"；而本函数每 2.5s 跑一次（initSystemPolling），会把刚读到的扩容状态反复抹掉 ⇒
  // 按钮永久禁用（板端实测 2026-09-19 P1-2）。
}

function storageUsage(storage) {
  const rootfs = (storage && storage.rootfs) || {};
  const rootUsage = rootfs.usage || {};
  return {
    total: storage && (storage.root_total ?? rootUsage.total ?? storage.total),
    used: storage && (storage.root_used ?? rootUsage.used ?? storage.used),
    free: storage && (storage.root_free ?? rootUsage.free ?? storage.free),
    percent: storage && (storage.root_percent ?? rootUsage.percent ?? storage.percent),
  };
}

function storageExpandLabel(rootfs) {
  if (!rootfs || Object.keys(rootfs).length === 0) {
    return "未读取";
  }
  if (rootfs.expandable && rootfs.action_available === false) {
    // 磁盘尾部真有未分配空间，但本仓还没把"改分区表 + resize2fs"装箱。
    // 报"待实现"而不是"可扩容"——后者会让用户以为按钮点一下就成了。
    return "待实现";
  }
  if (rootfs.expandable) {
    return "可扩容";
  }
  if (rootfs.ok === false) {
    return "检测失败";
  }
  if (rootfs.reason === "already_expanded") {
    return "已扩容";
  }
  if (rootfs.reason === "filesystem_needs_resize") {
    return "待完成";
  }
  if (rootfs.reason === "no_tail_space") {
    return "空间不足";
  }
  if (rootfs.reason === "missing_tools") {
    return "缺扩容工具";
  }
  if (rootfs.reason === "unsupported_filesystem" || rootfs.reason === "unsupported_root") {
    return "不支持";
  }
  return rootfs.supported === false ? "不可用" : "无需扩容";
}

function storageExpandLog(storage) {
  const rootfs = (storage && storage.rootfs) || {};
  if (!rootfs || Object.keys(rootfs).length === 0) {
    return "暂无扩容信息";
  }
  const root = rootfs.root || {};
  const lines = [
    rootfs.message || "等待检测",
    root.device ? `${root.label || "根分区"}：${root.device}` : "",
    root.disk ? `磁盘：${root.disk}` : "",
    Number.isFinite(Number(root.free_after_partition))
      ? `可扩尾部空间：${formatBytes(root.free_after_partition)}`
      : "",
    rootfs.method ? `扩容方式：${rootfs.method}` : "",
  ].filter(Boolean);
  if (Array.isArray(rootfs.log) && rootfs.log.length > 0) {
    lines.push("", rootfs.log.join("\n\n"));
  }
  return lines.join("\n");
}

async function refreshStorageStatus({ toast = false } = {}) {
  const storage = await api(`/api/system/storage${toast ? "?force=1" : ""}`);
  renderStorageExpansion(storage);
  if (toast) {
    showToast("容量状态已刷新");
  }
  return storage;
}

async function expandStorage() {
  const confirmed = window.confirm("扩容会修改当前系统盘分区并扩展文件系统。请确认设备供电稳定，过程中不要断电。确认现在扩容？");
  if (!confirmed) {
    return;
  }
  state.storageExpandBusy = true;
  const log = $("storageExpandLog");
  if (log) {
    log.textContent = "正在扩容根分区，请勿断电。";
  }
  renderStorageExpansion({ rootfs: { ok: true, message: "正在扩容根分区，请勿断电。" } });
  try {
    const storage = await api("/api/system/storage/expand", { method: "POST" });
    renderStorageExpansion(storage);
    const rootfs = (storage && storage.rootfs) || {};
    showToast(rootfs.message || "存储扩容完成");
    await refreshSystemStats().catch(() => {});
  } catch (error) {
    // 失败也要以"扩容数据源"形态渲染（必须带 rootfs），否则上面的门卫会直接返回，
    // 界面就只剩一句"扩容中"卡在原地，用户看不到失败原因。
    const payload = error && typeof error.payload === "object" && error.payload !== null ? error.payload : {};
    // 后端已按契约把 rootfs 放在 data.rootfs（共 409/501/500 三个失败分支都带），
    // 有就直接用它的真实值（reason/method/free_after_partition 都在里面），
    // 没有才退化成只剩一句人话。
    const rootfs = payload.rootfs && typeof payload.rootfs === "object"
      ? Object.assign({}, payload.rootfs, {
        ok: false,
        message: payload.rootfs.message || (error && error.message) || "扩容失败",
      })
      : { ok: false, message: (error && error.message) || "扩容失败" };
    renderStorageExpansion({ rootfs });
    throw error;
  } finally {
    state.storageExpandBusy = false;
    await refreshStorageStatus().catch(() => {});
  }
}

function syncLanHostnameInputs(payload) {
  const input = $("lanHostnameInput");
  const hint = $("lanHostnameHint");
  const hostname = (payload && payload.hostname) || "";
  if (input && document.activeElement !== input) {
    input.value = hostname;
  }
  if (hint) {
    const fallbackHost = defaultMdnsHost();
    const mdnsUrl = (payload && payload.mdns_url) || (hostname ? webUrl(`${hostname}.local`) : webUrl(fallbackHost));
    hint.textContent = `用于路由器设备列表和访问 ${mdnsUrl}，路由器列表可能要等网络刷新后更新`;
  }
  updateNetworkAccessButtonState();
}

function webUrl(host, port = state.webPort || 8080) {
  return `http://${host}:${port}/`;
}

function defaultMdnsHost() {
  return `${currentBrandConfig().defaultLocalName}.local`;
}

function syncWebPortInputs(payload) {
  const input = $("webPortInput");
  const hint = $("webPortHint");
  const port = Number(payload && payload.web_port) || state.webPort || 8080;
  state.webPort = port;
  if (input && document.activeElement !== input) {
    input.value = String(port);
  }
  if (hint) {
    const url = (payload && (payload.lan_url || payload.mdns_url)) || webUrl(defaultMdnsHost(), port);
    hint.textContent = `当前访问 ${url}；修改后 Web 服务会短暂重启。`;
  }
  updateNetworkAccessButtonState();
}

function validateLanHostname(value) {
  const hostname = String(value || "").trim().toLowerCase();
  if (!/^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/.test(hostname)) {
    throw new Error("局域网名称只能包含字母、数字和连字符，长度 1-63，且不能以连字符开头或结尾");
  }
  return hostname;
}

function validateWebPort(value) {
  const text = String(value || "").trim();
  if (!/^\d+$/.test(text)) {
    throw new Error("访问端口必须是 1024-65535 的数字");
  }
  const port = Number(text);
  if (!Number.isInteger(port) || port < 1024 || port > 65535) {
    throw new Error("访问端口必须在 1024-65535 之间");
  }
  return port;
}

function networkAccessDraft() {
  const hostnameInput = $("lanHostnameInput");
  const portInput = $("webPortInput");
  const hostname = validateLanHostname(hostnameInput ? hostnameInput.value : "");
  const port = validateWebPort(portInput ? portInput.value : "");
  return {
    hostname,
    port,
    hostnameChanged: hostname !== state.systemHostname,
    portChanged: port !== Number(state.webPort || 8080),
  };
}

function updateNetworkAccessButtonState() {
  const button = $("applyNetworkAccessButton");
  const hostnameInput = $("lanHostnameInput");
  const portInput = $("webPortInput");
  if (!button || !hostnameInput || !portInput) {
    return;
  }
  let disabled = true;
  let title = "";
  try {
    const draft = networkAccessDraft();
    disabled = !draft.hostnameChanged && !draft.portChanged;
    hostnameInput.classList.remove("is-invalid");
    portInput.classList.remove("is-invalid");
  } catch (error) {
    title = error.message || String(error);
    disabled = true;
    hostnameInput.classList.toggle("is-invalid", !HOSTNAME_PATTERN_FOR_UI.test(String(hostnameInput.value || "").trim().toLowerCase()));
    portInput.classList.toggle("is-invalid", !/^\d+$/.test(String(portInput.value || "").trim()) || Number(portInput.value) < 1024 || Number(portInput.value) > 65535);
  }
  button.disabled = disabled;
  button.title = title;
}

async function applyNetworkAccessSettings() {
  const draft = networkAccessDraft();
  if (!draft.hostnameChanged && !draft.portChanged) {
    return;
  }
  if (draft.portChanged) {
    const confirmed = window.confirm(`确认把 Web 访问端口修改为 ${draft.port}？页面会短暂断开，需要用新端口重新打开。`);
    if (!confirmed) {
      return;
    }
  }

  let payload = null;
  if (draft.hostnameChanged) {
    payload = await api("/api/system/hostname", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ hostname: draft.hostname }),
    });
    state.systemHostname = payload.hostname || draft.hostname;
    syncLanHostnameInputs(payload);
    syncWebPortInputs(payload);
  }

  if (draft.portChanged) {
    payload = await api("/api/system/web-port", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ port: draft.port }),
    });
    state.webPort = Number(payload.web_port) || draft.port;
    syncLanHostnameInputs(payload);
    syncWebPortInputs(payload);
    const nextUrl = payload.lan_url || payload.mdns_url || `${window.location.protocol}//${window.location.hostname}:${state.webPort}/`;
    showToast(`访问设置已应用，请访问 ${nextUrl}`);
    window.setTimeout(() => {
      window.location.href = nextUrl;
    }, 1800);
    return;
  }

  await refreshSystemStats().catch(() => {});
  showToast("网络访问设置已应用");
}

async function refreshSystemStats() {
  try {
    renderSystemStats(await api("/api/system"));
  } catch {
    renderSystemStats(null);
  }
}

function initSystemPolling() {
  refreshSystemStats();
  setInterval(refreshSystemStats, 2500);
  // 扩容面板的真源是 /api/system/storage（带 rootfs 检测结果），与上面的轮询无关。
  // 进页面时先取一次，免得面板一直停在上一次会话留下的"未读取"。
  refreshStorageStatus().catch(() => {});
}

async function refreshLicenseStatus() {
  const payload = await api("/api/license");
  renderLicensePanel(payload);
  if (state.data && state.data.state) {
    state.data.state.license = payload.license;
    state.data.state.core = payload.core || state.data.state.core;
    renderRuntime(state.data);
  }
  if (payload && payload.auto_recovered) {
    showToast("已自动修复当前设备授权");
    return;
  }
  showToast("授权状态已刷新");
}

function needsLicenseRecovery(payload) {
  const runtime = (payload && payload.state) || {};
  const license = runtime.license || (payload && payload.license) || {};
  const core = runtime.core || (payload && payload.core) || {};
  const text = `${license.status || ""} ${core.status || ""} ${license.message || ""} ${core.message || ""}`;
  return text.includes("device_mismatch") || text.includes("不属于当前设备");
}

function noteLicenseRecoveryResult(payload) {
  const recovery = payload && payload.recovery;
  if (!recovery || !recovery.message || recovery.message === state.lastLicenseRecoveryMessage) {
    return payload;
  }
  state.lastLicenseRecoveryMessage = recovery.message;
  showToast(recovery.message, !recovery.recovered);
  return payload;
}

async function maybeRunLicenseRecovery(payload) {
  noteLicenseRecoveryResult(payload);
  if (payload && payload.recovery && payload.recovery.message) {
    return payload;
  }
  if (!needsLicenseRecovery(payload) || state.licenseRecoveryInProgress) {
    return payload;
  }
  state.licenseRecoveryInProgress = true;
  state.lastLicenseRecoveryMessage = "检测到授权绑定信息变化，正在自动修复授权。";
  updateLicenseGateStatus(state.lastLicenseRecoveryMessage);
  setActivationSetupProgress(10, 0, state.lastLicenseRecoveryMessage);
  try {
    const recovered = await api("/api/license");
    const message = recovered && recovered.recovery && recovered.recovery.message
      ? recovered.recovery.message
      : recovered && recovered.auto_recovered
        ? "已自动修复当前设备授权"
        : "";
    if (message) {
      state.lastLicenseRecoveryMessage = message;
      showToast(message, !(recovered && recovered.auto_recovered));
    }
    if (recovered && recovered.auto_recovered) {
      const fresh = await api("/api/state");
      clearActivationSetupProgress();
      return fresh;
    }
  } catch (error) {
    state.lastLicenseRecoveryMessage = error.message || String(error);
  } finally {
    state.licenseRecoveryInProgress = false;
    clearActivationSetupProgress();
  }
  return payload;
}

async function activateLicenseFromInput(sourceInputId = "licenseKeyInput") {
  const sourceInput = $(sourceInputId) || $("licenseKeyInput") || $("licenseGateKeyInput");
  syncLicenseKeyInputs(sourceInput);
  const licenseKey = (sourceInput && sourceInput.value.trim()) || "";
  if (!licenseKey) {
    throw new Error("请输入卡密");
  }
  let activationSaved = false;
  setActivationBusy(true);
  setActivationSetupProgress(6, 0, "正在验证激活码并下载授权组件。");
  updateLicenseGateStatus("正在验证激活码，请稍候。");
  await nextPaint();
  try {
    const result = await api("/api/license/activate", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ license_key: licenseKey }),
    });
    activationSaved = true;
    setActivationSetupProgress(16, 0, "授权验证完成，正在保存核心模块和鼠标输出组件。");
    renderLicensePanel({ license: result.license, core: result.core, version: result.version });
    if (state.data && state.data.state) {
      state.data.state.license = result.license;
      state.data.state.core = result.core || state.data.state.core;
      renderRuntime(state.data);
    }
    updateDisclaimerActions();
    ["licenseKeyInput", "licenseGateKeyInput"].forEach((id) => {
      const input = $(id);
      if (input) {
        input.value = "";
      }
    });
    await nextPaint();
    await runFirstActivationSetup();
    const installedUpdate = await installFullUpdateAfterActivation();
    if (installedUpdate) {
      setActivationSetupProgress(96, 4, "完整更新包已安装，服务正在恢复。");
      showToast("设备已激活，完整更新包正在安装");
      window.setTimeout(() => refreshAll().catch(() => {}), 3500);
      return;
    }
    setActivationSetupProgress(100, 4, "初始化完成，正在刷新设备状态。");
    await sleep(650);
    await refreshAll();
    clearActivationSetupProgress();
    showToast("设备已激活");
  } catch (error) {
    if (activationSaved) {
      await refreshAll().catch(() => {});
    }
    clearActivationSetupProgress();
    throw error;
  } finally {
    setActivationBusy(false);
  }
}

function updateStatusLabel(status) {
  const value = String((status && status.status) || "idle");
  if (value === "running") return "更新中";
  if (value === "success") return "更新成功";
  if (value === "failed") return "更新失败";
  return "待检查";
}

// 2026-09-21 稳定化：轮询(1s)/监视器(5s)/检查结果/安装点击四方都会调本函数，
// 内容没变就直接跳过 DOM 写入；running 阶段进度只增不减，杜绝 65%→30% 倒退跳变。
// 2026-09-20 业主定案修正：面板结构恒定（进度头 + 进度条 + 消息常驻），空闲只是数值归零。
async function fetchUpdateStatus({ silent = false, render = true } = {}) {
  try {
    const status = await api("/api/update/status");
    if (render) {
      renderUpdateStatus(status);
    }
    return status;
  } catch (error) {
    if (!silent) {
      throw error;
    }
    return null;
  }
}

function stopUpdateStatusPolling() {
  if (state.updateStatusTimer) {
    clearInterval(state.updateStatusTimer);
    state.updateStatusTimer = null;
  }
}

// 2026-09-20：一次更新只允许自动刷新一次。
// 为什么加这个：更新成功有**两条**到达路径——「本页发起安装」的 1s 轮询，和常驻的 5s
// 完成监视器（后者为了发现"别处发起的更新"）。两条都能各自安排刷新；而刷新后新页面的
// state 是全新的，旧代码只有监视器那条写去重标记、轮询那条不写 ⇒ 页面刷一次后，
// 监视器又把这条 SUCCESS 判成"刚更新完"，于是再刷一次，用户看到连刷两次。
// 标记放 sessionStorage：同一标签页刷新后**仍然记得**（这正是防二次刷新的关键），
// 又不像 localStorage 那样跨标签页共享（多个标签页各自刷一次，不会被彼此吃掉）。
function otaRefreshAlreadyDone(version) {
  try {
    return window.sessionStorage.getItem(OTA_REFRESH_SEEN_PREFIX + version) === "1";
  } catch (error) {
    return false; // 拿不到 sessionStorage：退化为只靠本页实例的 updateRefreshScheduled 兜底
  }
}

function markOtaRefreshDone(version) {
  try {
    window.sessionStorage.setItem(OTA_REFRESH_SEEN_PREFIX + version, "1");
  } catch (error) {
    /* 忽略：最坏情况多刷一次，不会无限刷（监视器还有 finished_at 新鲜度窗口兜底） */
  }
}

// 返回 true = 本次真的安排了刷新（调用方据此决定要不要弹提示）。
function scheduleUpdatePageRefresh(version, delayMs = 2600) {
  const key = version || "_unknown";
  if (state.updateRefreshScheduled || otaRefreshAlreadyDone(key)) {
    return false;
  }
  markOtaRefreshDone(key);
  state.updateRefreshScheduled = true;
  window.setTimeout(() => {
    // 2026-09-20 业主反馈修正：刷新后要停在**当前这一页**，别跳来跳去。
    // 页签记忆已修成「用户点过的都记（含 08 系统状态）」，所以直接刷新就是对的：
    // 在 08 盯着更新就留在 08，在别处就留在别处，不再强制归位首页。
    window.location.reload();
  }, delayMs);
  return true;
}

// 两条路径（1s 轮询 / 5s 监视器）共用的唯一确认入口，防止重复弹提示、重复刷新。
function confirmUpdateSuccessAndRefresh(version) {
  if (!scheduleUpdatePageRefresh(version)) {
    return false;
  }
  showToast(version ? "检测到更新已完成（" + version + "），页面即将刷新"
                    : "更新成功，页面即将刷新");
  return true;
}

// 2026-09-20：OTA 完成监视器。原「成功→自动刷新」只挂在「本页发起安装」的轮询上；
// 更新在页面外完成（业主自装/另一端发起）或轮询因服务重启中断后，开着的面板
// 永远不会主动发现状态变化。此监视器每 5s 静默读一次状态：发现未确认的 SUCCESS
// 就提示并自动刷新（sessionStorage 按版本去重，防止刷新循环）。
function startUpdateCompletionWatcher() {
  if (state.updateCompletionTimer) {
    return;
  }
  // 本页打开时刻：用 state 里那一份（脚本加载时写入），两条路径共用同一个基准。
  const pageOpenedAtSec = Number(state.updatePageOpenedAtSec) || Math.floor(Date.now() / 1000);
  state.updateCompletionTimer = window.setInterval(async () => {
    if (document.visibilityState !== "visible") {
      return;
    }
    if (state.updateStatus && state.updateStatus.status === "running") {
      return; // 安装进行中，交给既有轮询（1s）处理
    }
    if (state.updateRefreshScheduled) {
      return;
    }
    // 有弹窗（免责声明/公告等）打开时不刷新，避免把用户正在看的弹窗顶掉、刷新后又弹一遍。
    const modalOpen = Array.from(document.querySelectorAll(".modal-backdrop"))
      .some((el) => !el.hidden);
    if (modalOpen) {
      return;
    }
    try {
      // render:false —— 这个监视器绝不能回写状态：它 5s 一次读到的多是 idle，
      // 一写就把上面「已是最新 / 发现更新」的 pill 冲成「待检查」，正是业主看到的
      // 「文字一会一变」。只有下面判定为「本次会话真升完」时才主动渲染。
      const status = await fetchUpdateStatus({ silent: true, render: false });
      if (!status || status.status !== "success" || !status.version) {
        return;
      }
      // 新鲜度判断：完成时间必须晚于本页打开时刻（容忍 60s 时钟/写入误差）。
      const finishedAt = Number(status.finished_at) || 0;
      if (!finishedAt || finishedAt < pageOpenedAtSec - 60) {
        return; // 历史残留的 SUCCESS，不提示也不刷新
      }
      // 去重：本标签页已经为这个版本刷过一次就不再动（sessionStorage 跨刷新保留，
      // 所以刷新后的新页面不会把同一条 SUCCESS 再当一次"刚更新完"）。
      if (otaRefreshAlreadyDone(status.version) || state.updateRefreshScheduled) {
        return;
      }
      stopUpdateStatusPolling();
      renderUpdateStatus(status);
      confirmUpdateSuccessAndRefresh(status.version);
    } catch (error) {
      /* 静默：网络抖动/服务重启期间读不到就等下一轮 */
    }
  }, 5000);
}

function startUpdateStatusPolling() {
  if (state.updateStatusTimer) {
    return;
  }
  state.updateStatusTimer = window.setInterval(async () => {
    if (state.updateStatusInFlight) {
      return;
    }
    state.updateStatusInFlight = true;
    try {
      const status = await fetchUpdateStatus({ silent: true });
      if (status && status.status === "success") {
        stopUpdateStatusPolling();
        // 走同一个确认入口：已刷过就只更新面板，不再弹提示、不再安排第二次刷新。
        if (!confirmUpdateSuccessAndRefresh(status.version)) {
          renderUpdateStatus(status);
        }
      } else if (status && status.status === "failed") {
        stopUpdateStatusPolling();
        showToast(status.error || "更新失败", true);
      }
    } finally {
      state.updateStatusInFlight = false;
    }
  }, 1000);
}

async function refreshInitialUpdateStatus() {
  const status = await fetchUpdateStatus({ silent: true, render: false });
  if (status && status.status === "running") {
    renderUpdateStatus(status);
    startUpdateStatusPolling();
  } else if (status && status.status === "failed") {
    renderUpdateStatus(status);
  } else {
    renderUpdateStatus({
      status: "idle",
      progress: 0,
      stateLabel: "待检查",
      message: "点「检查更新」开始。",
    });
  }
}

function updatePayloadHasInstallableUpdate(payload) {
  // 2026-09-18 定案：服务器一次查询回三样（latest_version/package_url/sign_url），
  // 有包地址且标记有更新即可安装。
  return !!(payload && payload.update_available && payload.package_url);
}

async function checkUpdate() {
  // 服务器即唯一真源（O11）：面板点检查时向写死地址查一次；无 prefer_full/target_version 概念。
  return api("/api/update/check", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({}),
  });
}

async function installUpdatePlan(plan) {
  const packageUrl = (plan && plan.package_url) || "";
  const keyId = (plan && plan.key_id) || "";
  try {
    renderUpdateStatus({
      status: "running",
      stage: "submit",
      message: "正在提交更新任务",
      progress: 1,
      version: (plan && plan.latest_version) || "",
    });
    // 必须在 running 渲染**之后**上闩：renderUpdateStatus 见到非 idle 会把闩清零。
    // 闩住「已提交 → 更新器写出 RUNNING」这段空窗，期间端点回 idle 不闪进度条。
    state.updateSessionDeadline = Date.now() + UPDATE_SESSION_TIMEOUT_MS;
    startUpdateStatusPolling();
    return await api("/api/update/install", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      // 显式带 version：更新器以此为准浇筑 releases/<ver>，避免 delta 场景
      // 从旁车签名取到 "x.y.z-delta-from-a.b.c" 污染目录名（2026-09-20 板端教训）
      body: JSON.stringify({ url: packageUrl, key_id: keyId, version: (plan && plan.latest_version) || "" }),
      // ★ 2026-10-03 性能 A：提交 OTA 任务时 core 会重启，连接可能挂几秒
      //   ⇒ 用长超时，不受 api() 默认 8 秒约束。
      timeoutMs: 30000,
    });
  } catch (error) {
    stopUpdateStatusPolling();
    renderUpdateStatus({
      status: "failed",
      stage: "failed",
      message: "更新失败",
      progress: 100,
      version: (plan && plan.latest_version) || "",
      error: error.message || String(error),
    });
    throw error;
  }
}

// 2026-09-18 定案：清理卡住状态功能删除（换链是秒级原子操作，无需手工清理）。

async function installFullUpdateAfterActivation() {
  setActivationSetupProgress(72, 0, "正在检查完整更新包。");
  const update = await checkUpdate();
  renderUpdateResult(update);
  if (!updatePayloadHasInstallableUpdate(update)) {
    return false;
  }
  if (!update.package_url) {
    throw new Error("检测到可用更新，但服务器未提供更新包地址");
  }
  const plan = {
    latest_version: update.latest_version || "",
    package_url: update.package_url,
    key_id: update.key_id || "",
  };
  state.updatePlan = plan;
  setActivationSetupProgress(84, 0, "发现可用更新，正在安装完整更新包。");
  await installUpdatePlan(plan);
  return true;
}

function uniqueModelGames(models) {
  return Array.from(new Set((models || []).map((model) => model.game_profile || "generic")))
    .sort((lhs, rhs) => modelProfileLabel(lhs).localeCompare(modelProfileLabel(rhs), "zh-Hans-CN"));
}

function filteredModelImportGames(games = state.modelGameOptions) {
  const input = $("modelImportGameProfile");
  const query = input ? input.value.trim().toLowerCase() : "";
  if (!query) {
    return games;
  }
  return games.filter((game) => {
    const value = String(game || "").toLowerCase();
    const label = modelProfileLabel(game).toLowerCase();
    return value.includes(query) || label.includes(query);
  });
}

function renderModelImportGameSuggestions(games = state.modelGameOptions) {
  state.modelGameOptions = Array.isArray(games) ? games : [];
  const list = $("modelGameSuggestionList");
  const toggle = $("modelGameSuggestionToggle");
  if (toggle) {
    toggle.disabled = state.modelGameOptions.length === 0;
  }
  if (!list) {
    return;
  }

  const visibleGames = filteredModelImportGames(state.modelGameOptions);
  list.innerHTML = "";
  visibleGames.forEach((game) => {
    const label = modelProfileLabel(game);
    const button = document.createElement("button");
    button.type = "button";
    button.className = "model-game-suggestion-option";
    button.dataset.modelGame = game;
    button.setAttribute("role", "option");
    button.innerHTML = `
      <span>${escapeHtml(label)}</span>
      ${label !== game ? `<small>${escapeHtml(game)}</small>` : ""}
    `;
    list.appendChild(button);
  });
  setModelGameSuggestionOpen(state.modelGameSuggestionOpen);
}

function renderModelGameFilters(models) {
  const filters = $("modelGameFilters");
  const games = uniqueModelGames(models);
  if (!games.includes(state.modelGameFilter) && state.modelGameFilter !== "all") {
    state.modelGameFilter = "all";
  }
  renderModelImportGameSuggestions(games);
  if (!filters) {
    return;
  }
  // ★ 签名守卫（2026-10-03 性能 B）：模型列表没变就别重建筛选条。
  //   实测：轮询每 1.5s 调一次本函数，每次 filters.innerHTML="" 整体重建，
  //   12 秒内 .model-filter-stack 被重建 112 次 —— 而列表签名从未变化。
  const nextSignature = [uniqueModelGames(models).join(","), state.modelGameFilter].join("\u001e");
  if (state.modelGameFiltersSignature === nextSignature) {
    return;
  }
  state.modelGameFiltersSignature = nextSignature;
  filters.innerHTML = "";
  [{ value: "all", label: "全部" }, ...games.map((game) => ({ value: game, label: modelProfileLabel(game) }))]
    .forEach((filter) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `model-filter-chip${state.modelGameFilter === filter.value ? " is-active" : ""}`;
      button.textContent = filter.label;
      button.addEventListener("click", () => {
        state.modelGameFilter = filter.value;
        renderModels({ models, selected_model_id: state.config && state.config.model_id }, state.config && state.config.model_id);
      });
      filters.appendChild(button);
    });
}

function renderModelBackendFilters(models) {
  const filters = $("modelBackendFilters");
  if (!["all", "rknn", "hef"].includes(state.modelBackendFilter)) {
    state.modelBackendFilter = "all";
  }
  if (!filters) {
    return;
  }
  // ★ 签名守卫（同 renderModelGameFilters）：后端筛选项是固定三个，
  //   只有「当前选中项」变了才需要重画is-active class。
  const nextSignature = state.modelBackendFilter;
  if (state.modelBackendFiltersSignature === nextSignature) {
    return;
  }
  state.modelBackendFiltersSignature = nextSignature;
  const options = [
    { value: "all", label: "全部" },
    { value: "rknn", label: "RKNN" },
    { value: "hef", label: "HEF" },
  ];
  filters.innerHTML = "";
  options.forEach((filter) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `model-filter-chip model-format-chip${state.modelBackendFilter === filter.value ? " is-active" : ""}`;
    button.textContent = filter.label;
    button.addEventListener("click", () => {
      state.modelBackendFilter = filter.value;
      renderModels({ models, selected_model_id: state.config && state.config.model_id }, state.config && state.config.model_id);
    });
    filters.appendChild(button);
  });
}

function modelMatchesActiveFilters(model) {
  const gameMatches = state.modelGameFilter === "all" ||
    (model.game_profile || "generic") === state.modelGameFilter;
  const backendMatches = state.modelBackendFilter === "all" ||
    modelBackendFilterValue(model) === state.modelBackendFilter;
  return gameMatches && backendMatches;
}

function trashIconSvg() {
  return `
    <svg viewBox="0 0 24 24" aria-hidden="true">
      <path d="M3 6h18"/>
      <path d="M8 6V4h8v2"/>
      <path d="m6 6 1 15h10l1-15"/>
      <path d="M10 11v6M14 11v6"/>
    </svg>
  `;
}

function renderModelCards(models, selectedModelId) {
  const list = $("modelCardList");
  const empty = $("modelEmptyState");
  if (!list) {
    return;
  }
  const visibleModels = models.filter(modelMatchesActiveFilters);
  list.innerHTML = "";
  if (empty) {
    empty.hidden = visibleModels.length > 0;
  }
  visibleModels.forEach((model) => {
    const isSelected = model.id === selectedModelId;
    const card = createModelCardShell(model, isSelected);
    card.innerHTML = modelCardHtml(model, isSelected);
    bindModelCardEvents(card, model);
    appendModelDeleteButton(card, model, isSelected);
    list.appendChild(card);
  });
}

function createModelCardShell(model, isSelected) {
  const isImporting = model.importing === true;
  const card = document.createElement("article");
  card.className = `model-card${isSelected ? " is-active" : ""}${isImporting ? " is-importing" : ""}`;
  card.dataset.modelId = model.id;
  card.tabIndex = 0;
  card.setAttribute("role", "button");
  const switchModel = () => runUiAction(async () => {
    // 1.5.61：模型还在导入（装进模型库）过程中，切过去会因为模型文件还没落到位
    // 而失败。这里直接拦住并提示，不让它发出请求。
    if (isImporting) {
      showToast("这个模型还在导入中，请等导入完成后再切换", true);
      return;
    }
    if (model.id === ((state.config && state.config.model_id) || "")) {
      return;
    }
    const result = await applySelectedModel(model.id, model);
    showToast(result && result.created_default_preset && result.created_default_preset_name
      ? `模型已应用，并创建默认预设 ${result.created_default_preset_name}`
      : result && result.applied_preset && result.applied_preset_name
      ? `模型已应用，并加载预设 ${result.applied_preset_name}`
      : result && result.cleared_missing_preset && result.cleared_missing_preset_name
        ? `模型已应用，已清除失效预设 ${result.cleared_missing_preset_name}`
      : "模型已应用");
  });
  card.addEventListener("click", switchModel);
  card.addEventListener("keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      switchModel();
    }
  });
  return card;
}

function modelCardHtml(model, isSelected) {
  const description = (model.description || "").trim() || "暂无描述";
  const presetName = String(model.preset_name || "");
  const presetLabel = presetName || "未绑定（自动创建）";
  const presetOptions = [
    { value: "", label: presetName ? "清空绑定" : "未绑定（自动创建）" },
    ...state.presetNames.map((name) => ({ value: name, label: name })),
  ];
  const isPresetOpen = state.modelPresetBindingOpenId === model.id;
  return `
      <span class="model-card-top">
        <span class="model-card-title">${escapeHtml(modelFileName(model))}</span>
        <span class="model-card-status">${isSelected ? "当前使用" : "可切换"}</span>
      </span>
	      <span class="model-card-meta">
	        <span>${escapeHtml(modelDimension(model))}</span>
	        <span>${escapeHtml(modelBackendLabel(model))}</span>
	        <span>${escapeHtml(modelClassLabel(model))}</span>
        <span>${escapeHtml(modelProfileLabel(model.game_profile))}</span>
      </span>
      <span class="model-card-description">${escapeHtml(description)}</span>
      <div class="model-preset-binding">
        <span>绑定预设</span>
        <div class="model-preset-combobox${isPresetOpen ? " is-open" : ""}">
          <button class="preset-select-button model-preset-select-button" type="button"
            data-model-preset-toggle="${escapeAttr(model.id)}"
            aria-controls="modelPresetList-${escapeAttr(model.id)}"
            aria-expanded="${isPresetOpen ? "true" : "false"}"
            aria-haspopup="listbox">
            <span>${escapeHtml(presetLabel)}</span>
            <svg viewBox="0 0 24 24" aria-hidden="true">
              <path d="m6 9 6 6 6-6"/>
            </svg>
          </button>
          <div id="modelPresetList-${escapeAttr(model.id)}" class="model-game-suggestion-list model-preset-option-list" role="listbox" ${isPresetOpen ? "" : "hidden"}>
            ${presetOptions.map((option) => `
              <button class="model-game-suggestion-option${option.value === presetName ? " is-active" : ""}" type="button"
                data-model-preset-option="${escapeAttr(model.id)}"
                data-preset-name="${escapeAttr(option.value)}"
                role="option"
                aria-selected="${option.value === presetName ? "true" : "false"}">
                <span>${escapeHtml(option.label)}</span>
              </button>
            `).join("")}
          </div>
        </div>
        <small>${escapeHtml(presetName ? `切换到此模型时自动加载：${presetLabel}` : "首次切换到此模型时自动创建默认预设")}</small>
      </div>
      <div class="model-card-actions">
        <button class="mini-button model-class-edit-button" type="button" data-model-class-edit="${escapeAttr(model.id)}">编辑类别</button>
      </div>
    `;
}

function bindModelCardEvents(card, model) {
  const presetBinding = card.querySelector(".model-preset-binding");
  if (presetBinding) {
    presetBinding.addEventListener("click", (event) => event.stopPropagation());
    presetBinding.addEventListener("keydown", (event) => event.stopPropagation());
  }
  const cardActions = card.querySelector(".model-card-actions");
  if (cardActions) {
    cardActions.addEventListener("click", (event) => event.stopPropagation());
    cardActions.addEventListener("keydown", (event) => event.stopPropagation());
  }
  const classEditButton = card.querySelector("[data-model-class-edit]");
  if (classEditButton) {
    classEditButton.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      setModelClassNamesDialogOpen(true, model.id);
    });
  }
  const presetToggle = card.querySelector("[data-model-preset-toggle]");
  if (presetToggle) {
    presetToggle.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      setModelPresetBindingOpen(state.modelPresetBindingOpenId !== model.id ? model.id : "");
    });
    presetToggle.addEventListener("keydown", (event) => {
      event.stopPropagation();
      if (event.key === "ArrowDown") {
        event.preventDefault();
        setModelPresetBindingOpen(model.id);
      } else if (event.key === "Escape") {
        event.preventDefault();
        setModelPresetBindingOpen("");
      }
    });
  }
  card.querySelectorAll("[data-model-preset-option]").forEach((option) => {
    option.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      runUiAction(async () => {
        const result = await bindModelPreset(model.id, option.dataset.presetName || "");
        const nextName = String(result && result.model && result.model.preset_name || "");
        state.modelPresetBindingOpenId = "";
        showToast(nextName ? `已绑定预设 ${nextName}` : "已取消模型预设绑定");
      });
    });
  });
}

function appendModelDeleteButton(card, model, isSelected) {
  const canDeleteId = Boolean(model.id);
  const deleteButton = document.createElement("button");
  deleteButton.type = "button";
  deleteButton.className = "icon-button delete-model-button";
  deleteButton.title = isSelected ? "当前模型不能删除" : (canDeleteId ? "删除模型" : "模型标识无效，无法删除");
  deleteButton.setAttribute("aria-label", isSelected ? "当前模型不能删除" : (canDeleteId ? `删除 ${modelFileName(model)}` : "模型标识无效，无法删除"));
  deleteButton.disabled = isSelected || !canDeleteId;
  deleteButton.innerHTML = trashIconSvg();
  deleteButton.addEventListener("click", (event) => {
    event.stopPropagation();
    if (isSelected) {
      return;
    }
    runUiAction(async () => {
      const confirmed = window.confirm(`确认完全删除模型 ${modelFileName(model)}？`);
      if (!confirmed) {
        return;
      }
      await deleteModel(model.id);
      showToast("模型已删除");
    });
  });
  card.appendChild(deleteButton);
}

function setModelPresetBindingOpen(modelId) {
  state.modelPresetBindingOpenId = modelId || "";
  rerenderCurrentModels();
}

function renderModels(payload, selectedModelId) {
  const models = payload && Array.isArray(payload.models) ? payload.models : [];
  const selected = selectedModelId ||
    (payload && payload.selected_model_id) ||
    (state.config && state.config.model_id) ||
    "";
  const nextSignature = modelListSignature(models);
  const modelExists = models.some((model) => model.id === selected);
  const nextSelected = modelExists ? selected : (models[0] && models[0].id) || "";
  if (state.modelPresetBindingOpenId && !models.some((model) => model.id === state.modelPresetBindingOpenId)) {
    state.modelPresetBindingOpenId = "";
  }
  if (state.config && state.config.model_id !== nextSelected) {
    state.config = { ...state.config, model_id: nextSelected };
  }
  if (state.data && state.data.config && state.data.config.model_id !== nextSelected) {
    state.data.config = { ...state.data.config, model_id: nextSelected };
  }
  state.modelListSignature = nextSignature;
  renderModelGameFilters(models);
  renderModelBackendFilters(models);
  renderModelPanel(models, nextSelected);
  const nextCardsRenderSignature = [
    nextSignature,
    nextSelected,
    state.modelGameFilter,
    state.modelBackendFilter,
    state.presetListSignature || presetListSignature(state.presetNames || []),
    state.modelPresetBindingOpenId,
  ].join("\u001e");
  if (state.modelCardsRenderSignature !== nextCardsRenderSignature) {
    state.modelCardsRenderSignature = nextCardsRenderSignature;
    renderModelCards(models, nextSelected);
  }

  const nextClassSignature = currentModelClassRenderSignature(nextSelected);
  if (state.configReady && state.aimClassRenderSignature && state.aimClassRenderSignature !== nextClassSignature) {
    renderAimProfiles(collectAimProfiles());
  }
  updatePresetCleanupButton();
}

function rerenderCurrentModels() {
  const models = state.data && Array.isArray(state.data.models) ? state.data.models : [];
  const selected = (state.config && state.config.model_id) ||
    (state.data && state.data.config && state.data.config.model_id) ||
    "";
  if (models.length > 0) {
    renderModels({ models, selected_model_id: selected }, selected);
  }
}

function presetListSignature(presets) {
  return presets.map((name) => String(name)).join("\u001f");
}

function presetBoundNameSet() {
  const models = state.data && Array.isArray(state.data.models) ? state.data.models : [];
  return new Set(models.map((model) => String(model && model.preset_name || "").trim()).filter(Boolean));
}

function unusedPresetNames(presets = state.presetNames) {
  if (!(state.data && Array.isArray(state.data.models))) {
    return [];
  }
  const boundNames = presetBoundNameSet();
  return (Array.isArray(presets) ? presets : [])
    .map((name) => String(name || "").trim())
    .filter((name) => name && !boundNames.has(name));
}

function updatePresetCleanupButton() {
  const button = $("cleanupUnusedPresetsButton");
  if (!button) {
    return;
  }
  const modelsReady = Boolean(state.data && Array.isArray(state.data.models));
  const unusedCount = unusedPresetNames().length;
  const nextDisabled = !modelsReady || unusedCount === 0;
  const nextTitle = !modelsReady
    ? "模型列表尚未加载"
    : unusedCount > 0
      ? `可清理 ${unusedCount} 个未绑定模型的预设`
      : "没有未使用预设";
  // ★ 签名守卫（2026-10-03 性能 B）：值没变就别写 disabled/title。
  //   实测：轮询每 1.5s 调一次，静止 12 秒产生
  //   #cleanupUnusedPresetsButton[disabled] 16 次 + [title] 16 次。
  const nextSignature = [nextDisabled ? 1 : 0, nextTitle].join("\u001e");
  if (state.presetCleanupButtonSignature === nextSignature) {
    return;
  }
  state.presetCleanupButtonSignature = nextSignature;
  button.disabled = nextDisabled;
  button.title = nextTitle;
}

function getSelectedPresetName() {
  return state.presetSelectedName || "";
}

function currentPresetNameForAutosave() {
  const name = getSelectedPresetName();
  return name && state.presetNames.includes(name) ? name : "";
}

function syncSelectedPresetFromModelResult(result) {
  const presetName = String(result && (result.applied_preset_name || result.created_default_preset_name) || "");
  if (presetName && state.presetNames.includes(presetName)) {
    setSelectedPresetName(presetName);
  }
}

function queueCurrentPresetAutosave(config) {
  const name = currentPresetNameForAutosave();
  if (!name || !config) {
    return "";
  }
  state.presetAutoSaveName = name;
  state.presetAutoSaveConfig = cloneJson(config);
  state.presetAutoSaveQueued = true;
  flushCurrentPresetAutosave();
  return name;
}

async function flushCurrentPresetAutosave() {
  if (state.presetAutoSaveInFlight) {
    return;
  }
  state.presetAutoSaveInFlight = true;
  try {
    while (state.presetAutoSaveQueued) {
      const name = state.presetAutoSaveName;
      const config = state.presetAutoSaveConfig;
      state.presetAutoSaveQueued = false;
      if (!name || !config) {
        continue;
      }
      await api("/api/presets", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name, config }),
      });
      if (getSelectedPresetName() === name) {
        setApplyStatus("ready", "已同步并保存预设");
      }
    }
  } catch (error) {
    setApplyStatus("error", "预设保存失败");
    showToast(`预设自动保存失败：${error.message || String(error)}`, true);
  } finally {
    state.presetAutoSaveInFlight = false;
    if (state.presetAutoSaveQueued) {
      flushCurrentPresetAutosave();
    }
  }
}

function setPresetSuggestionOpen(open) {
  const list = $("presetSuggestionList");
  const combo = $("presetCombobox");
  const button = $("presetSelectButton");
  const nextOpen = !!open && state.presetNames.length > 0;

  state.presetSuggestionOpen = nextOpen;
  if (list) {
    list.hidden = !nextOpen;
  }
  if (combo) {
    combo.classList.toggle("is-open", nextOpen);
  }
  if (button) {
    button.disabled = state.presetNames.length === 0;
    button.setAttribute("aria-expanded", nextOpen ? "true" : "false");
  }
}

function updatePresetOptionSelection() {
  const list = $("presetSuggestionList");
  if (!list) {
    return;
  }
  Array.from(list.querySelectorAll("[data-preset-name]")).forEach((option) => {
    const selected = option.dataset.presetName === state.presetSelectedName;
    option.classList.toggle("is-active", selected);
    option.setAttribute("aria-selected", selected ? "true" : "false");
  });
}

function updatePresetCardSelection() {
  document.querySelectorAll("[data-preset-card]").forEach((card) => {
    const selected = card.dataset.presetCard === state.presetSelectedName;
    card.classList.toggle("is-active", selected);
    const status = card.querySelector("[data-preset-card-status]");
    if (status) {
      status.textContent = selected ? "自动保存目标" : "可加载";
    }
    const note = card.querySelector("[data-preset-card-note]");
    if (note) {
      note.textContent = selected ? "调整参数会自动保存到此预设" : "加载后会成为自动保存目标";
    }
  });
}

function setSelectedPresetName(name) {
  const nextName = name || "";
  state.presetSelectedName = nextName;

  const value = $("presetSelectValue");
  if (value) {
    value.textContent = nextName || "暂无预设参数";
  }

  const button = $("presetSelectButton");
  if (button) {
    button.disabled = state.presetNames.length === 0;
    button.title = nextName || "暂无预设参数";
  }

  const meta = $("presetCurrentMeta");
  if (meta) {
    meta.textContent = nextName ? `当前选中：${nextName}，调整参数会自动保存` : "暂无";
  }

  setExportPresetLink(nextName);
  updatePresetOptionSelection();
  updatePresetCardSelection();
}

function renderPresetSuggestionList(presets) {
  const list = $("presetSuggestionList");
  if (!list) {
    return;
  }
  list.innerHTML = "";
  presets.forEach((name) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "model-game-suggestion-option";
    button.dataset.presetName = name;
    button.setAttribute("role", "option");
    button.innerHTML = `<span>${escapeHtml(name)}</span>`;
    list.appendChild(button);
  });
  updatePresetOptionSelection();
}

function renderPresetCards(presets) {
  const list = $("presetCardList");
  const empty = $("presetEmptyState");
  if (!list) {
    return;
  }
  const safePresets = Array.isArray(presets) ? presets.map((name) => String(name)).filter(Boolean) : [];
  list.innerHTML = "";
  if (empty) {
    empty.hidden = safePresets.length > 0;
  }
  safePresets.forEach((name) => {
    const selected = name === getSelectedPresetName();
    const card = document.createElement("article");
    card.className = `preset-card${selected ? " is-active" : ""}`;
    card.dataset.presetCard = name;
    card.innerHTML = `
      <div class="preset-card-head">
        <span class="preset-card-title">${escapeHtml(name)}</span>
        <span class="preset-card-status" data-preset-card-status>${selected ? "自动保存目标" : "可加载"}</span>
      </div>
      <small data-preset-card-note>${selected ? "调整参数会自动保存到此预设" : "加载后会成为自动保存目标"}</small>
      <div class="preset-card-actions">
        <button class="ghost-button" type="button" data-preset-action="load">加载</button>
        <button class="danger-button" type="button" data-preset-action="delete">删除</button>
        <a class="button-link" href="${escapeAttr(presetExportUrl(name))}" target="_blank" rel="noopener" data-preset-action="export">导出</a>
      </div>
    `;
    const loadButton = card.querySelector('[data-preset-action="load"]');
    if (loadButton) {
      loadButton.addEventListener("click", (event) => {
        event.preventDefault();
        event.stopPropagation();
        runUiAction(() => loadPresetByName(name));
      });
    }
    const deleteButton = card.querySelector('[data-preset-action="delete"]');
    if (deleteButton) {
      deleteButton.addEventListener("click", (event) => {
        event.preventDefault();
        event.stopPropagation();
        runUiAction(() => deletePresetByName(name));
      });
    }
    const exportLink = card.querySelector('[data-preset-action="export"]');
    if (exportLink) {
      exportLink.addEventListener("click", (event) => event.stopPropagation());
    }
    list.appendChild(card);
  });
}

function renderPresets(presets) {
  const safePresets = Array.isArray(presets) ? presets.map((name) => String(name)).filter(Boolean) : [];
  const previous = getSelectedPresetName();
  const boundPreset = String(currentModel() && currentModel().preset_name || "");
  const nextSignature = presetListSignature(safePresets);
  const nextSelected = previous && safePresets.includes(previous)
    ? previous
    : boundPreset && safePresets.includes(boundPreset)
      ? boundPreset
    : (safePresets[0] || "");

  state.presetNames = safePresets;
  if (nextSignature !== state.presetListSignature) {
    state.presetListSignature = nextSignature;
    renderPresetSuggestionList(safePresets);
  }

  const summary = $("presetLibrarySummary");
  if (summary) {
    summary.textContent = safePresets.length > 0 ? `${safePresets.length} 个预设参数` : "暂无预设参数";
  }
  setSelectedPresetName(nextSelected);
  updatePresetCleanupButton();
  setPresetSuggestionOpen(state.presetSuggestionOpen);
  renderPresetCards(safePresets);
  rerenderCurrentModels();
}

function setHardwareStatus(id, text, live = false) {
  const el = $(id);
  if (!el) {
    return;
  }
  el.textContent = text;
  el.className = `pill ${live ? "status-badge live" : ""}`;
}

function randomInt(min, max) {
  const lower = Math.ceil(min);
  const upper = Math.floor(max);
  return Math.floor(Math.random() * (upper - lower + 1)) + lower;
}

function randomHex(width, min = 1, max = null) {
  const limit = max === null ? (16 ** width) - 1 : max;
  return `0x${randomInt(min, limit).toString(16).padStart(width, "0")}`;
}

function randomLetters(length) {
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  let text = "";
  for (let i = 0; i < length; i += 1) {
    text += alphabet[randomInt(0, alphabet.length - 1)];
  }
  return text;
}

function randomSerial(prefix = "") {
  return `${prefix}${randomInt(0x10000000, 0xffffffff).toString(16).toUpperCase()}`.slice(0, 32);
}

function randomUsbId(min, max, excludedValues = []) {
  const excluded = new Set(excludedValues);
  let value = 0;
  do {
    value = randomInt(min, max);
  } while (excluded.has(value));
  return `0x${value.toString(16).padStart(4, "0")}`;
}

function randomMouseBrand() {
  const brands = ["Logitech", "Razer", "SteelSeries", "Corsair", "HyperX", "BenQ", "Pulsar", "Endgame"];
  return brands[randomInt(0, brands.length - 1)];
}

function displayModeLine(label, value) {
  return `<div><span>${escapeHtml(label)}</span><strong>${escapeHtml(value || "--")}</strong></div>`;
}

function parseDynamicDisplayModeToken(token) {
  const match = DISPLAY_DYNAMIC_MODE_RE.exec(String(token || "").trim());
  if (!match) {
    return null;
  }
  return {
    width: Number(match[1]),
    height: Number(match[2]),
    refresh: Number(match[3]),
  };
}

function displayDynamicModePixelClockKHz(width, height, refresh) {
  if (!width || !height || !refresh) {
    return 0;
  }
  return Math.floor(((width + DISPLAY_DYNAMIC_MODE_H_BLANK) * (height + DISPLAY_DYNAMIC_MODE_V_BLANK) * refresh + 500) / 1000);
}

function displayDynamicModeMaxRefresh(width, height) {
  if (!width || !height) {
    return DISPLAY_DYNAMIC_MODE_MIN_REFRESH;
  }
  const totalPixels = (width + DISPLAY_DYNAMIC_MODE_H_BLANK) * (height + DISPLAY_DYNAMIC_MODE_V_BLANK);
  if (!Number.isFinite(totalPixels) || totalPixels <= 0) {
    return DISPLAY_DYNAMIC_MODE_MIN_REFRESH;
  }
  const maxByClock = Math.floor((DISPLAY_DYNAMIC_MODE_MAX_PIXEL_CLOCK_KHZ * 1000) / totalPixels);
  return clamp(maxByClock, DISPLAY_DYNAMIC_MODE_MIN_REFRESH, DISPLAY_DYNAMIC_MODE_MAX_REFRESH);
}

function displayDynamicModeToken(width, height, refresh) {
  return `${Math.round(width)}x${Math.round(height)}@${Math.round(refresh)}`;
}

function displayDynamicModeLabel(width, height, refresh, suffix = "自定义") {
  return `${Math.round(width)}x${Math.round(height)} @ ${Math.round(refresh)} Hz ${suffix}`;
}

function displayDynamicModeLabelFromToken(token) {
  const mode = parseDynamicDisplayModeToken(token);
  return mode ? displayDynamicModeLabel(mode.width, mode.height, mode.refresh) : String(token || "");
}

function renderDisplayModeSummary(payload) {
  const el = $("displayModeSummary");
  if (!el) {
    return;
  }
  const info = (payload && payload.display_mode) || {};
  const monitor = info.real_monitor || {};
  const loopout = (payload && payload.loopout) || {};
  const config = (payload && payload.config) || {};
  const modes = Array.isArray(info.advertised_modes) ? info.advertised_modes : [];
  const modeText = modes.length
    ? modes.map((mode) => mode.label || `${mode.width || "--"}x${mode.height || "--"}@${mode.refresh || "--"}`).join(" / ")
    : "未读取";
  const current = loopout.width && loopout.height
    ? `${loopout.width}x${loopout.height}${loopout.refresh ? ` @ ${loopout.refresh} Hz` : ""}`
    : "--";
  el.innerHTML = [
    displayModeLine("真实显示器", monitor.connected ? (monitor.name || "已连接") : "未连接"),
    displayModeLine("硬件身份", `${monitor.vendor || "???"} ${monitor.product_id || "0x0000"} ${monitor.serial || ""}`.trim()),
    displayModeLine("Windows 可选模式", modeText),
    displayModeLine("环出颜色格式", loopout.enabled ? (loopout.pixel_format || config.loopout_pixel_format || "rgb888") : (config.loopout_pixel_format || loopout.pixel_format || "rgb888")),
    displayModeLine("环出状态", loopout.enabled ? `${loopout.status || "开启"}${loopout.fps ? ` · ${formatNumber(loopout.fps)} FPS` : ""}` : "关闭"),
    displayModeLine("当前环出", current),
    loopout.last_error ? displayModeLine("环出错误", loopout.last_error) : "",
  ].filter(Boolean).join("");
  el.hidden = false;
}

function modeOptionLabel(mode) {
  if (!mode) {
    return "";
  }
  if (mode.label) {
    return mode.label.replace("@", " @ ") + (mode.refresh ? " Hz" : "");
  }
  const width = Number(mode.width || 0);
  const height = Number(mode.height || 0);
  const refresh = Number(mode.refresh || 0);
  if (width && height && refresh) {
    return `${width}x${height} @ ${refresh} Hz`;
  }
  return String(mode.token || "");
}

function normalizeModeToken(token) {
  return String(token || "").trim();
}

function refreshDisplayNativeModeOptions(payload) {
  const select = $("display_native_mode");
  if (!select) {
    return;
  }
  const config = (payload && payload.config) || {};
  const selected = normalizeModeToken(config.native_mode || select.value);
  const info = (payload && payload.display_mode) || {};
  const loopoutEnabled = !!(info.loopout_enabled || (payload && payload.config && payload.config.loopout_enabled));
  const availableModes = Array.isArray(info.available_modes) ? info.available_modes : [];
  const options = new Map();
  options.set("", DISPLAY_NATIVE_MODE_LABELS[""]);
  if (!loopoutEnabled || availableModes.length === 0) {
    DISPLAY_NATIVE_MODES.forEach((token) => {
      options.set(token, DISPLAY_NATIVE_MODE_LABELS[token] || token);
    });
  }
  if (loopoutEnabled) {
    availableModes.forEach((mode) => {
      const token = normalizeModeToken(mode.token);
      if (!token || options.has(token)) {
        return;
      }
      options.set(token, modeOptionLabel(mode));
    });
  }
  if (selected && !options.has(selected) && (!loopoutEnabled || parseDynamicDisplayModeToken(selected))) {
    options.set(selected, displayDynamicModeLabelFromToken(selected));
  }
  select.innerHTML = "";
  options.forEach((label, value) => {
    const option = document.createElement("option");
    option.value = value;
    option.textContent = label;
    select.appendChild(option);
  });
  select.value = options.has(selected) ? selected : "";
}

function displayNativeModeAllowedValues() {
  const select = $("display_native_mode");
  if (!select) {
    return DISPLAY_NATIVE_MODES;
  }
  return Array.from(select.options || []).map((option) => option.value);
}

function setDisplayEdidModeStatus(message, isError = false) {
  const status = $("displayEdidModeStatus");
  if (!status) {
    return;
  }
  status.textContent = message || "";
  status.classList.toggle("is-error", !!isError);
}

function setDisplayEdidModeInvalidFields(ids = []) {
  const invalidIds = new Set(ids);
  DISPLAY_EDID_MODE_FIELD_IDS.forEach((id) => {
    const el = $(id);
    if (el) {
      el.classList.toggle("is-invalid", invalidIds.has(id));
    }
  });
}

function parseDisplayModeText(text) {
  const match = String(text || "").match(/(\d{3,4})\s*x\s*(\d{3,4})(?:\s*@\s*(\d{2,3}))?/i);
  if (!match) {
    return null;
  }
  return {
    width: Number(match[1]),
    height: Number(match[2]),
    refresh: match[3] ? Number(match[3]) : 0,
  };
}

function displayEdidModeDefaults() {
  const select = $("display_native_mode");
  let mode = select
    ? (parseDynamicDisplayModeToken(select.value) || parseDisplayModeText(select.selectedOptions && select.selectedOptions[0] && select.selectedOptions[0].textContent))
    : null;
  if (!mode) {
    const loopout = state.displayHardwarePayload && state.displayHardwarePayload.loopout;
    if (loopout && loopout.width && loopout.height) {
      mode = { width: Number(loopout.width), height: Number(loopout.height), refresh: Number(loopout.refresh || 0) };
    }
  }
  const width = clamp(
    Math.round((mode && mode.width) || 1920),
    DISPLAY_DYNAMIC_MODE_MIN_WIDTH,
    DISPLAY_DYNAMIC_MODE_MAX_WIDTH,
  );
  const height = clamp(
    Math.round((mode && mode.height) || 1080),
    DISPLAY_DYNAMIC_MODE_MIN_HEIGHT,
    DISPLAY_DYNAMIC_MODE_MAX_HEIGHT,
  );
  return {
    width,
    height,
    refresh: displayDynamicModeMaxRefresh(width, height),
  };
}

function addDisplayNativeModeOption(token, label) {
  const select = $("display_native_mode");
  if (!select) {
    return;
  }
  let option = Array.from(select.options || []).find((item) => item.value === token);
  if (!option) {
    option = document.createElement("option");
    option.value = token;
    select.appendChild(option);
  }
  option.textContent = label || displayDynamicModeLabelFromToken(token);
  select.value = token;
}

function saveDisplayEdidModeFromDialog() {
  const result = validateDisplayEdidModeInputs();
  if (!result.valid) {
    return false;
  }
  const token = displayDynamicModeToken(result.width, result.height, result.refresh);
  addDisplayNativeModeOption(token, displayDynamicModeLabel(result.width, result.height, result.refresh));
  state.displayHardwarePayload = {
    ...(state.displayHardwarePayload || {}),
    config: {
      ...((state.displayHardwarePayload && state.displayHardwarePayload.config) || {}),
      native_mode: token,
    },
  };
  setValidation("displayHardwareValidation", ["display_native_mode"], []);
  setDisplayEdidModeDialogOpen(false);
  showToast("EDID模式已加入首选模式，点击保存并应用写入设备");
  return true;
}

function updateDisplayCustomModeButtonUi() {
  const button = $("addDisplayEdidModeButton");
  if (!button) {
    return;
  }
  button.disabled = false;
  button.title = getCheckbox("display_loopout_enabled")
    ? "自定义模式会作为环出 EDID 首选模式写入"
    : "";
}

function realMonitorIdentityFromPayload(payload) {
  const monitor = payload && payload.display_mode && payload.display_mode.real_monitor;
  if (!monitor || !monitor.connected || !monitor.edid_valid) {
    return null;
  }
  const name = printableAscii(monitor.name || "", 13);
  const vendor = vendorCode(monitor.vendor || "");
  const productId = normalizeHexValue(monitor.product_id || "", 4);
  const serial = normalizeHexValue(monitor.serial || "", 8);
  if (!name || !vendor || !productId || !serial) {
    return null;
  }
  return { name, vendor, product_id: productId, serial };
}

function setDisplayIdentityReadonly(readonly) {
  DISPLAY_IDENTITY_FIELD_IDS.forEach((id) => {
    const el = $(id);
    if (!el) {
      return;
    }
    el.readOnly = readonly;
    el.setAttribute("aria-readonly", readonly ? "true" : "false");
    const field = el.closest(".display-identity-field");
    if (field) {
      field.classList.toggle("is-readonly", readonly);
    }
  });
  const randomButton = $("randomDisplayHardwareButton");
  if (randomButton) {
    randomButton.disabled = readonly;
  }
}

function populateMouseHardware(payload) {
  // 2026-09-22：认「实际在跑的模式」（effective_mode），不是单元里的请求值——
  // 单元写 full、启动脚本因没插物理鼠标自行降级合成时，必须按合成模式渲染。
  const mode = (payload && (payload.effective_mode || payload.mode)) || "full_passthrough";
  const isSynthetic = mode === "synthetic";
  const connected = !!(payload && payload.connected);
  // 合成模式（或没探到物理鼠标）下，面板该显示 usb-proxy 实际落盘的 gadget 配置
  // —— 那才是真源；透传模式用的是物理鼠标身份，仍按探测值显示。
  const gadget = (payload && payload.gadget_config) || {};
  const config = (isSynthetic || !connected) && Object.keys(gadget).length > 0
    ? gadget
    : ((payload && payload.config) || {});
  Object.entries(config).forEach(([key, value]) => {
    setValue(`mouse_${key}`, value);
  });
  const usingOriginalProfile = payload && payload.config_source === "original_mouse_profile";
  const usingPhysicalMouse = payload && payload.config_source === "sysfs_usb_mouse";
  // 2026-09-22：单选删除，模式改只读展示。展示的必须是**实际在跑**的模式：
  // 单元里写 full、启动脚本因无鼠标自行降级合成，只看单元会把降级说成完整透传。
  state.mouseProxyMode = (payload && (payload.effective_mode || payload.mode)) || "full_passthrough";
  const modeText = $("mouseProxyModeText");
  if (modeText) {
    const label = isSynthetic ? "合成模式" : "完整透传";
    modeText.textContent = payload && payload.mode_degraded
      ? `${label}（已降级合成：未找到物理鼠标）`
      : label;
  }
  setHardwareStatus(
    "mouseHardwareStatus",
    connected
      ? (isSynthetic
        ? "合成鼠标随机身份"
        : (usingOriginalProfile || usingPhysicalMouse ? "完整透传真实鼠标" : "完整透传"))
      : "未连接",
    connected,
  );
  updateMouseHardwareModeUi(payload);
}

function bindHardwareInputFilters() {
  const filters = {
    display_name: (value) => printableAscii(value, 13),
    display_vendor: vendorCode,
    display_product_id: (value) => hexText(value, 4),
    display_serial: (value) => hexText(value, 8),
    mouse_usb_vid: (value) => hexText(value, 4),
    mouse_usb_pid: (value) => hexText(value, 4),
    mouse_usb_manufacturer: (value) => printableAscii(value, 48),
    mouse_usb_product: (value) => printableAscii(value, 64),
    mouse_usb_serial: (value) => printableAscii(value, 64),
    mouse_usb_configuration: (value) => printableAscii(value, 32),
  };
  Object.entries(filters).forEach(([id, filter]) => {
    const el = $(id);
    if (!el) {
      return;
    }
    el.addEventListener("input", () => {
      const filtered = filter(el.value);
      if (el.value !== filtered) {
        el.value = filtered;
      }
    });
  });
}

async function loadHardware() {
  const [display, mouse] = await Promise.all([
    api("/api/hardware/display"),
    api("/api/hardware/mouse"),
  ]);
  populateDisplayHardware(display);
  populateMouseHardware(mouse);
}

async function refreshAll() {
  let payload = await api("/api/state");
  payload = await maybeRunLicenseRecovery(payload);
  state.data = payload;
  applyFullState(payload);
  await loadHardware();
  setApplyStatus("ready", "已同步");
}

function applyFullState(payload) {
  state.data = payload;
  applyBrand(payload);
  populateForm(payload.config);
  applyLiveState(payload);
}

function applyLiveState(payload) {
  state.data = payload;
  applyBrand(payload);
  renderRuntime(payload);
  renderLicensePanel({
    license: payload.state && payload.state.license,
    core: payload.state && payload.state.core,
    version: payload.version,
    recovery: payload.recovery,
  });
  renderPresets(payload.presets || []);
  renderModels({ models: payload.models, selected_model_id: payload.config.model_id }, payload.config.model_id);
}

function hardenInputAutofillHints(root = document) {
  root.querySelectorAll("input[type='number']").forEach((input) => {
    input.setAttribute("autocomplete", "off");
  });
  root.querySelectorAll("input[type='password']").forEach((input) => {
    input.setAttribute("autocomplete", "new-password");
    input.setAttribute("autocapitalize", "off");
    input.setAttribute("spellcheck", "false");
    input.setAttribute("data-lpignore", "true");
    input.setAttribute("data-1p-ignore", "true");
    input.setAttribute("data-bwignore", "true");
  });
}

async function runUiAction(fn) {
  try {
    await fn();
  } catch (error) {
    const message = error.message || String(error);
    if (suppressMouseSwitchDaemonTimeout(message)) {
      return;
    }
    showToast(message, true);
  }
}

function bindConfigAutoApply() {
  document.querySelectorAll("[data-config]").forEach((el) => {
    if (el.type === "number") {
      el.setAttribute("autocomplete", "off");
      el.addEventListener("keydown", (event) => {
        if (event.key === "Enter") {
          event.preventDefault();
          el.blur();
        }
      });
      el.addEventListener("input", () => {
        if (el.id === "capture_crop_size" && el.value !== "" && Number.isFinite(Number(el.value))) {
          syncCropSizePresetRange(el.value);
        }
        if (AIM_OVERLAY_CONFIG_IDS.has(el.id)) {
          updateAimRangeOverlay();
        }
      });
      el.addEventListener("change", () => {
        clampNumberInputToLimits(el);
        if (AIM_OVERLAY_CONFIG_IDS.has(el.id)) {
          updateAimRangeOverlay();
        }
        requestApplyForNumberInput(el, 90);
      });
      return;
    }
    const isInstant = el.matches("select") || el.type === "checkbox" || el.type === "radio";
    const eventName = isInstant ? "change" : "input";
    const delay = isInstant ? 70 : 180;
    el.addEventListener(eventName, () => {
      if (AIM_OVERLAY_CONFIG_IDS.has(el.id)) {
        updateAimRangeOverlay();
      }
      requestConfigApply(delay);
    });
  });
}

function updateAssistModuleCollapseStates() {
  document.querySelectorAll("#assist-page .assist-section").forEach((section) => {
    const active = section.id === (state.activeAssistSectionId || "assist-section-recoil");
    section.classList.toggle("is-active", active);
    section.hidden = !active;
  });
  document.querySelectorAll("[data-assist-section-target]").forEach((tab) => {
    const active = tab.dataset.assistSectionTarget === (state.activeAssistSectionId || "assist-section-recoil");
    tab.classList.toggle("is-active", active);
    tab.setAttribute("aria-selected", active ? "true" : "false");
  });
}

function bindAssistModuleCollapseState() {
  updateAssistModuleCollapseStates();
}

function initAssistSectionNavigation() {
  const tabs = Array.from(document.querySelectorAll("[data-assist-section-target]"));
  const sections = Array.from(document.querySelectorAll("[data-assist-section]"));
  if (tabs.length === 0 || sections.length === 0) {
    return;
  }

  function activate(sectionId, shouldScroll = false) {
    // 2026-09-22：内核未实现的区块不接受激活（state 被外部污染时也兜底回落到压枪）
    const target = tabs.find((tab) => tab.dataset.assistSectionTarget === sectionId);
    if (!target || target.disabled) {
      sectionId = "assist-section-recoil";
    }
    state.activeAssistSectionId = sectionId;
    updateAssistModuleCollapseStates();
    if (shouldScroll) {
      const assistPage = $("assist-page");
      if (assistPage) {
        assistPage.scrollIntoView({ block: "start", behavior: "smooth" });
      }
    }
  }

  tabs.forEach((tab) => {
    // 未实现区块页签为 disabled：不注册跳转（disabled 本身也不派发 click，这里再显式跳过一层）
    if (tab.disabled) {
      return;
    }
    tab.addEventListener("click", () => activate(tab.dataset.assistSectionTarget, true));
  });
  activate(state.activeAssistSectionId, false);
}

function initPageNavigation() {
  const tabs = Array.from(document.querySelectorAll("[data-page-target]"));
  const pages = Array.from(document.querySelectorAll("[data-page]"));
  if (tabs.length === 0 || pages.length === 0) {
    return;
  }

  // 2026-09-20 业主反馈修正：刷新后要**停在原来那一页**，不要再跳。
  // 旧逻辑把「系统状态」页排除在记忆之外（它同时是授权门禁页），于是你在 08 看更新、
  // 一刷新就落回更早停留的页签（本次是 07 预设参数）。
  // 现在：用户点过的每一页都记（含 08）；只有「授权门禁强制跳转」不记——
  // 那是被动进来的，记下来解锁后会一直停在授权页。
  function activate(pageId, options = {}) {
    // 门禁锁定时的一切切页都是被动的，不写记忆。
    const gateForced = !!state.navigationLockedToLicense;
    if (gateForced && pageId !== LICENSE_GATE_PAGE_ID) {
      pageId = LICENSE_GATE_PAGE_ID;
    }
    state.activePageId = pageId;
    pages.forEach((page) => {
      page.classList.toggle("is-active", page.id === pageId);
    });
    tabs.forEach((tab) => {
      tab.classList.toggle("is-active", tab.dataset.pageTarget === pageId);
    });
    if (!gateForced && options.persistMemory !== false) {
      try {
        if (window.localStorage) {
          window.localStorage.setItem(ACTIVE_PAGE_STORAGE_KEY, pageId);
        }
      } catch (_) {
        /* 私密模式 / 存储被禁用：静默跳过，不影响切页 */
      }
    }
    window.scrollTo({ top: 0, behavior: "smooth" });
  }

  tabs.forEach((tab) => {
    tab.addEventListener("click", () => activate(tab.dataset.pageTarget));
  });
  window.activatePage = activate;

  // 恢复上次停留的页签（没有记录或记录已失效 ⇒ 保持 HTML 默认的首页）。
  // 2026-09-20 修正：不再排除「系统状态」页——在 08 刷新就该停在 08。
  // 存储里能出现 08，只可能是用户自己点进去的（门禁被动跳转不写记忆）。
  try {
    if (window.localStorage) {
      const saved = window.localStorage.getItem(ACTIVE_PAGE_STORAGE_KEY);
      if (saved && pages.some((page) => page.id === saved)) {
        activate(saved, { persistMemory: false });
      }
    }
  } catch (_) {
    /* ignore */
  }
}

function initControlSectionNavigation() {
  const tabs = Array.from(document.querySelectorAll("[data-control-section-target]"));
  const sections = Array.from(document.querySelectorAll("[data-control-section]"));
  if (tabs.length === 0 || sections.length === 0) {
    return;
  }

  function activate(sectionId, shouldScroll = false) {
    state.activeControlSectionId = sectionId;
    sections.forEach((section) => {
      const active = section.id === sectionId;
      section.classList.toggle("is-active", active);
      section.hidden = !active;
    });
    tabs.forEach((tab) => {
      const active = tab.dataset.controlSectionTarget === sectionId;
      tab.classList.toggle("is-active", active);
      tab.setAttribute("aria-selected", active ? "true" : "false");
    });
    if (shouldScroll) {
      const controlPage = $("control-page");
      if (controlPage) {
        controlPage.scrollIntoView({ block: "start", behavior: "smooth" });
      }
    }
  }

  tabs.forEach((tab) => {
    tab.addEventListener("click", () => activate(tab.dataset.controlSectionTarget, true));
  });
  activate(state.activeControlSectionId, false);
}

function setLicenseNavigationLock(locked) {
  const wasLocked = state.navigationLockedToLicense;
  // ★ 幂等守卫（2026-10-03 性能 B）：锁定状态没变就直接返回。
  //   原来每 1.5s 轮询都会：classList.remove + 9 个 tab 的 disabled/aria-disabled
  //   全量重写 ⇒ 静止 12 秒产生 72 次 .module-tab[aria-disabled] 属性变更。
  //   注意：`wasLocked` 仍要更新（下面 locked 分支用它判断是否首次进入）。
  if (wasLocked === locked) {
    document.body.classList.remove("license-loading");
    return;
  }
  state.navigationLockedToLicense = locked;
  document.body.classList.remove("license-loading");
  if (locked) {
    redirectToActivationPage();
    return;
  }
  document.body.classList.toggle("license-locked", locked);
  const overlay = $("licenseGateOverlay");
  if (overlay) {
    overlay.hidden = !locked;
  }
  document.querySelectorAll("[data-page-target]").forEach((tab) => {
    const allowed = tab.dataset.pageTarget === LICENSE_GATE_PAGE_ID;
    tab.disabled = locked && !allowed;
    tab.setAttribute("aria-disabled", locked && !allowed ? "true" : "false");
  });
  if (locked) {
    updateLicenseGateStatus();
    if (window.activatePage) {
      window.activatePage(LICENSE_GATE_PAGE_ID);
    }
    if (!wasLocked) {
      focusLicenseGateInput();
    }
  }
  updateDisclaimerActions();
}

async function applySelectedModel(model_id, selectedModel = null) {
  if (!model_id) {
    throw new Error("没有可用模型");
  }
  setApplyStatus("saving", "切换模型");
  const result = await api("/api/models/select", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model_id }),
  });
  if (result && result.config) {
    state.config = result.config;
    populateForm(result.config);
  }
  if (result && Array.isArray(result.models)) {
    state.data = { ...(state.data || {}), models: result.models };
  }
  if (result && Array.isArray(result.presets)) {
    renderPresets(result.presets);
  }
  syncSelectedPresetFromModelResult(result);
  if (state.data) {
    state.data.config = state.config || state.data.config;
  }
  const models = (state.data && state.data.models) || [];
  const model = (result && result.model) || selectedModel || models.find((item) => item.id === model_id) || null;
  const cropSize = modelInputCropSize(model);
  if (cropSize !== null) {
    const nextCapture = {
      ...((state.config && state.config.capture) || {}),
      crop_size: cropSize,
    };
    state.config = {
      ...(state.config || {}),
      capture: nextCapture,
    };
    if (state.data) {
      state.data.config = state.config;
    }
    setCropSizeValue(cropSize);
    updateAimRangeOverlay();
    await applyConfigNow();
  }
  renderModels({ models: (state.data && state.data.models) || [], selected_model_id: model_id }, model_id);
  setApplyStatus("ready", "已同步");
  return result;
}

async function copyModelDeviceCode() {
  const result = await api("/api/models/device-code");
  const code = String(result && result.code ? result.code : "").trim();
  if (!code) {
    throw new Error("设备码为空");
  }
  await copyTextToClipboard(code);
  showToast("模型设备码已复制");
}

async function copyLanAccessUrl(button) {
  const url = String(button && button.dataset ? button.dataset.copyLanUrl || "" : "").trim();
  if (!url) {
    throw new Error("没有可复制的地址");
  }
  await copyTextToClipboard(url);
  showToast("访问地址已复制");
}

async function bindModelPreset(model_id, preset_name) {
  const result = await api("/api/models/bind-preset", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model_id, preset_name }),
  });
  if (state.data && Array.isArray(result.models)) {
    state.data.models = result.models;
  }
  renderModels({
    models: (result && result.models) || (state.data && state.data.models) || [],
    selected_model_id: (result && result.selected_model_id) || (state.config && state.config.model_id) || "",
  });
  const nextName = String(result && result.model && result.model.preset_name || "");
  const selectedModelId = String((result && result.selected_model_id) || (state.config && state.config.model_id) || "");
  if (nextName && selectedModelId === model_id && state.presetNames.includes(nextName)) {
    setSelectedPresetName(nextName);
  } else if (!nextName && selectedModelId === model_id) {
    setSelectedPresetName("");
  }
  return result;
}

async function deleteModel(model_id) {
  await api("/api/models/delete", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model_id }),
  });
  const payload = await api("/api/models");
  if (state.data) {
    state.data.models = payload.models || [];
  }
  renderModels(payload, payload.selected_model_id);
}

function renderModelClassNamesEditor(model) {
  const editor = $("modelClassNamesEditor");
  const subtitle = $("modelClassNamesSubtitle");
  if (!editor || !model) {
    return;
  }
  const count = modelClassEditCount(model);
  const names = modelClassNames(model);
  if (subtitle) {
    subtitle.textContent = `${modelFileName(model)} · ${count} 类`;
  }
  editor.innerHTML = "";
  for (let classId = 0; classId < count; classId += 1) {
    const row = document.createElement("label");
    row.className = "class-name-row";
    row.innerHTML = `
      <span>${classId}</span>
      <input class="model-class-name-input" type="text"
        data-class-id="${classId}"
        value="${escapeAttr(names[classId] || "")}"
        placeholder="${escapeAttr(`类别 ${classId}`)}">
    `;
    editor.appendChild(row);
  }
}

function setModelClassNamesDialogOpen(open, modelOrId = null) {
  const dialog = $("modelClassNamesDialog");
  if (!dialog) {
    return;
  }
  if (!open) {
    state.modelClassNamesEditModelId = "";
    dialog.hidden = true;
    setAnyModalOpen();
    return;
  }
  const model = typeof modelOrId === "string" ? findModelById(modelOrId) : modelOrId;
  if (!model || !model.id) {
    return;
  }
  state.modelClassNamesEditModelId = model.id;
  renderModelClassNamesEditor(model);
  dialog.hidden = false;
  setAnyModalOpen();
  const firstInput = dialog.querySelector(".model-class-name-input");
  if (firstInput) {
    firstInput.focus();
    firstInput.select();
  }
}

function collectModelClassNamesEditor() {
  const editor = $("modelClassNamesEditor");
  if (!editor) {
    return [];
  }
  const names = [];
  Array.from(editor.querySelectorAll(".model-class-name-input")).forEach((input) => {
    const classId = Number(input.dataset.classId);
    if (!Number.isInteger(classId) || classId < 0 || classId >= AIM_CLASS_MAX_COUNT) {
      return;
    }
    names[classId] = String(input.value || "").trim();
  });
  while (names.length > 0 && !names[names.length - 1]) {
    names.pop();
  }
  return names.map((name) => name || "");
}

function refreshClassNameDependentViews() {
  if (!state.configReady) {
    return;
  }
  renderAimProfiles(collectAimProfiles());
}

async function saveModelClassNames(modelId, classNames) {
  const result = await api("/api/models/class-names", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model_id: modelId, class_names: classNames }),
  });
  if (state.data && Array.isArray(result.models)) {
    state.data.models = result.models;
  }
  renderModels({
    models: (result && result.models) || (state.data && state.data.models) || [],
    selected_model_id: (result && result.selected_model_id) || (state.config && state.config.model_id) || "",
  });
  refreshClassNameDependentViews();
  return result;
}

async function loadPresetByName(name) {
  const presetName = String(name || "").trim();
  if (!presetName) {
    throw new Error("没有可加载的预设参数");
  }
  setSelectedPresetName(presetName);
  await api("/api/presets/load", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name: presetName }),
  });
  showToast(`已加载 ${presetName}`);
  await refreshAll();
}

function clearQueuedPresetAutosave(names) {
  const deleteNames = new Set((Array.isArray(names) ? names : [names]).map((name) => String(name || "").trim()).filter(Boolean));
  if (deleteNames.size === 0) {
    return;
  }
  if (deleteNames.has(state.presetAutoSaveName)) {
    state.presetAutoSaveQueued = false;
    state.presetAutoSaveName = "";
    state.presetAutoSaveConfig = null;
  }
}

async function deletePresetRequest(presetName) {
  return api("/api/presets", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name: presetName, action: "delete" }),
  });
}

async function refreshPresetLibraryAfterDelete(results = []) {
  renderPresets((await api("/api/presets")).presets || []);
  const unboundModels = results.some((result) =>
    result && Array.isArray(result.unbound_models) && result.unbound_models.length > 0
  );
  if (unboundModels) {
    const payload = await api("/api/models");
    if (state.data) {
      state.data.models = payload.models || [];
    }
    renderModels(payload, payload.selected_model_id);
  }
}

async function deletePresetByName(name, options = {}) {
  const presetName = String(name || "").trim();
  if (!presetName) {
    throw new Error("没有可删除的预设参数");
  }
  if (options.confirm !== false) {
    const confirmed = window.confirm(`确认删除预设「${presetName}」？\n删除后不可恢复；如果有模型绑定到它，也会自动清空绑定。`);
    if (!confirmed) {
      return;
    }
  }
  clearQueuedPresetAutosave(presetName);
  const result = await deletePresetRequest(presetName);
  showToast(`已删除 ${presetName}`);
  await refreshPresetLibraryAfterDelete([result]);
}

async function cleanupUnusedPresets() {
  const modelsPayload = await api("/api/models");
  if (state.data) {
    state.data.models = modelsPayload.models || [];
  }
  renderModels(modelsPayload, modelsPayload.selected_model_id);
  const unusedNames = unusedPresetNames();
  if (unusedNames.length === 0) {
    showToast("没有未使用预设");
    return;
  }
  const previewNames = unusedNames.slice(0, 8).join("、");
  const suffix = unusedNames.length > 8 ? ` 等 ${unusedNames.length} 个` : "";
  const confirmed = window.confirm(`确认清理 ${unusedNames.length} 个未绑定模型的预设？\n${previewNames}${suffix}\n删除后不可恢复。`);
  if (!confirmed) {
    return;
  }
  clearQueuedPresetAutosave(unusedNames);
  const results = [];
  for (const presetName of unusedNames) {
    results.push(await deletePresetRequest(presetName));
  }
  showToast(`已清理 ${unusedNames.length} 个未使用预设`);
  await refreshPresetLibraryAfterDelete(results);
}
// ★ 事件绑定总入口 —— 由 906 行拆成 10 个函数（2026-10-03 S10 刀 4）。
//   拆分依据：每个绑定块用到的控件 id 属于哪个页签（HTML section 结构事实）。
//   ★ 原来定义在本函数体内的 5 个「标定」函数已提升为顶层，
//     搬去了 static/panel/calib-bind.js（它们不是事件绑定）。
function bindEvents() {
  bindHomeEvents();
  bindHotkeyEvents();
  bindPointerEvents();
  bindAssistEvents();
  bindModelEvents();
  bindHardwareEvents();
  bindPresetEvents();
  bindLicenseEvents();
  bindFanEvents();
  bindGlobalEvents();
}

// ---- bindHomeEvents（总览）：7 个绑定块 ----
function bindHomeEvents() {
  const preview = $("previewImage");
  on("startButton", "click", () => runUiAction(async () => {
    const runtime = (state.data && state.data.state) || {};
    const shouldStop = runtime.running || runtime.status === "starting" || runtime.status === "reconnecting";
    if (!shouldStop && needsLicenseRecovery(state.data)) {
      await refreshLicenseStatus();
      await refreshAll();
    }
    const path = shouldStop ? "/api/control/stop" : "/api/control/start";
    const nextRuntime = await api(path, { method: "POST" });
    if (state.data) {
      state.data.state = {
        ...runtime,
        ...nextRuntime,
        license: nextRuntime.license || runtime.license,
        core: nextRuntime.core || runtime.core,
      };
      if (shouldStop) {
        state.data.state.running = false;
        state.data.state.status = "stopped";
      }
      renderRuntime(state.data);
    }
  }));

  on("rebootSystemButton", "click", () => runUiAction(async () => {
    if (!window.confirm("确认重启设备？")) {
      return;
    }
    await api("/api/system/reboot", { method: "POST" });
    showToast("重启指令已发送");
  }));

  on("poweroffSystemButton", "click", () => runUiAction(async () => {
    if (!window.confirm("确认关机？")) {
      return;
    }
    await api("/api/system/poweroff", { method: "POST" });
    showToast("关机指令已发送");
  }));

  on("resetOverviewDefaultsButton", "click", resetOverviewDefaults);
  on("refreshLicenseButton", "click", () => runUiAction(refreshLicenseStatus));
  on("activateLicenseButton", "click", () => runUiAction(() => activateLicenseFromInput("licenseKeyInput")));
}

// ---- bindHotkeyEvents（热键控制）：1 个绑定块 ----
function bindHotkeyEvents() {
  on("addAimProfileButton", "click", () => {
    const current = document.querySelectorAll("#aimProfilesEditor .aim-profile-card").length;
    if (current >= AIM_PROFILE_MAX) {
      showToast(`热键最多 ${AIM_PROFILE_MAX} 组`, true);
      return;
    }
    addAimProfileCard({
      // 挑一个还没被占的键，否则刚加完就冲突、自动保存立刻报错。
      hotkey: nextFreeAimHotkey(),
      class_filter_mask: currentModelClassMask(),
      offset_x: defaultAimProfileOffsetX(),
      offset_y: defaultAimProfileOffsetY(),
      sensitivity: 1,
      fov_scale: 1,
    });
  });

}

// ---- bindPointerEvents（移动控制）：6 个绑定块 ----
function bindPointerEvents() {
  on("resetControllerDefaultsButton", "click", resetCurrentMovementSectionDefaults);
  on("calibStartButton", "click", () => runUiAction(async () => {
    calibTraceLines.length = 0;
    const traceLog = $("calibTraceLog");
    if (traceLog) {
      traceLog.textContent = "—";
    }
    const fitsLog = $("calibFitsLog");
    if (fitsLog) {
      fitsLog.hidden = true;
      fitsLog.textContent = "";
    }
    await api("/api/control/calibration/start", { method: "POST" });
    state.calibKeepPolling = true;
    state.calibConfigSynced = false;
    scheduleCalibrationPolling(true);
    await refreshCalibration();
    showToast("标定已启动，过程中请保持目标静止");
  }));

  on("calibCancelButton", "click", () => runUiAction(async () => {
    await api("/api/control/calibration/cancel", { method: "POST" });
    await refreshCalibration();
    showToast("标定已取消");
  }));

  on("calibRefreshButton", "click", () => runUiAction(() => refreshCalibration({ toast: true })));

  on("calibClearButton", "click", () => runUiAction(async () => {
    const confirmed = window.confirm("清除标定结果？增益与推导参数的留档会被删掉（不影响当前 PID 设置）。");
    if (!confirmed) {
      return;
    }
    await api("/api/control/calibration", { method: "DELETE" });
    await refreshCalibration();
    showToast("标定结果已清除");
  }));

  on("calibManualApplyButton", "click", () => runUiAction(async () => {
    const gainX = Number($("calibManualGainX") && $("calibManualGainX").value);
    const gainY = Number($("calibManualGainY") && $("calibManualGainY").value);
    const delay = Number($("calibManualDelay") && $("calibManualDelay").value) || 0;
    if (!gainX || gainX <= 0 || !gainY || gainY <= 0) {
      throw new Error("请填写大于 0 的增益 X / 增益 Y");
    }
    await api("/api/control/calibration", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        gain_x_px_per_count: gainX,
        gain_y_px_per_count: gainY,
        response_delay_ms: delay,
      }),
    });
    await refreshCalibration();
    // 手动应用同样写回了 kp/kd/predict_x，面板要跟着热更新
    await syncConfigAfterCalibration();
    showToast("已按手动增益换算并应用 Kp / Kd / 预判");
  }));

}

// ---- bindAssistEvents（辅助功能）：1 个绑定块 ----
function bindAssistEvents() {
  on("resetAssistDefaultsButton", "click", resetCurrentAssistSectionDefaults);

}

// ---- bindModelEvents（模型库）：15 个绑定块 ----
function bindModelEvents() {
  on("openModelImportButton", "click", () => setModelImportDialogOpen(true));
  on("copyModelDeviceCodeButton", "click", () => runUiAction(copyModelDeviceCode));
  on("closeModelImportButton", "click", () => setModelImportDialogOpen(false));
  on("cancelModelImportButton", "click", () => setModelImportDialogOpen(false));
  on("closeModelClassNamesButton", "click", () => setModelClassNamesDialogOpen(false));
  on("cancelModelClassNamesButton", "click", () => setModelClassNamesDialogOpen(false));
  const modelImportDialog = $("modelImportDialog");
  const modelClassNamesDialog = $("modelClassNamesDialog");
  document.addEventListener("pointerdown", (event) => {
    const modelCombo = $("modelGameCombobox");
    if (state.modelGameSuggestionOpen && modelCombo && !modelCombo.contains(event.target)) {
      setModelGameSuggestionOpen(false);
    }
    const presetCombo = $("presetCombobox");
    if (state.presetSuggestionOpen && presetCombo && !presetCombo.contains(event.target)) {
      setPresetSuggestionOpen(false);
    }
    const modelPresetCombo = event.target.closest(".model-preset-combobox");
    if (state.modelPresetBindingOpenId && !modelPresetCombo) {
      setModelPresetBindingOpen("");
    }
  });

  const modelGameInput = $("modelImportGameProfile");
  const modelGameToggle = $("modelGameSuggestionToggle");
  const modelGameList = $("modelGameSuggestionList");
  const modelImportForm = $("modelImportForm");
  if (modelImportForm) {
    Array.from(modelImportForm.querySelectorAll('input[name="model_type"]')).forEach((radio) => {
      radio.addEventListener("change", updateModelImportMode);
    });
    updateModelImportMode();
    modelImportForm.addEventListener("submit", (event) => {
      event.preventDefault();
      const form = event.currentTarget;
      runUiAction(async () => {
        const gameProfileInput = $("modelImportGameProfile");
        if (gameProfileInput) {
          gameProfileInput.value = gameProfileInput.value.trim();
        }
        const formData = new FormData(form);
        const importType = currentModelImportType(form);
  	        setModelImportBusy(
  	          true,
  	          importType === "onnx"
  	            ? "转换中，完成后会自动清理上传文件..."
  	            : "导入中..."
  	        );
        try {
          await api(importType === "onnx" ? "/api/models/import-onnx" : "/api/models/import", {
            method: "POST",
            body: formData,
          });
          form.reset();
          setModelImportDialogOpen(false);
  	          showToast(importType === "onnx" ? "ONNX 已转换并导入" : "模型导入成功");
  	          const payload = await api("/api/models");
  	          if (state.data) {
  	            state.data.models = payload.models || [];
  	          }
  	          renderModels(payload, payload.selected_model_id);
  	          renderPresets((await api("/api/presets")).presets || []);
  	        } finally {
  	          setModelImportBusy(false);
  	        }
      });
    });
  }

  const modelClassNamesForm = $("modelClassNamesForm");
}

// ---- bindHardwareEvents（显示与鼠标）：10 个绑定块 ----
function bindHardwareEvents() {
  // ★ 本函数只做编排：按设备拆成 4 个子函数（2026-10-03）。
  bindDiagnosticsEvents();
  bindDisplayEvents();
  bindMouseEvents();
  bindHardwareSaveEvents();
}

// ---- 诊断 / 移动日志 / EDID 手动添加 / 硬件刷新 ----
function bindDiagnosticsEvents() {
on("downloadUsbDiagnosticsButton", "click", () => runUiAction(downloadUsbDiagnostics));

on("recordAimTraceButton", "click", () => runUiAction(async () => {
  const button = $("recordAimTraceButton");
  try {
    if (button) {
      button.disabled = true;
    }
    setAimTraceStatus("记录中...");
    const result = await api("/api/diagnostics/aim-trace", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ duration_sec: 10 }),
    });
    const durationMs = Math.max(1000, Math.round(((result && result.duration_sec) || 10) * 1000));
    showToast("移动日志已开始");
    window.setTimeout(() => {
      setAimTraceStatus(result && result.filename ? `已生成 ${result.filename}` : "记录完成");
      if (button) {
        button.disabled = false;
      }
    }, durationMs + 500);
  } catch (error) {
    if (button) {
      button.disabled = false;
    }
    setAimTraceStatus("记录失败");
    throw error;
  }
}));

on("addDisplayEdidModeButton", "click", () => setDisplayEdidModeDialogOpen(true));
on("refreshHardwareButton", "click", () => runUiAction(async () => {
  await loadHardware();
  showToast("硬件信息已刷新");
}));
}

// ---- 显示器身份：随机 + 环出开关联动 ----
function bindDisplayEvents() {
on("randomDisplayHardwareButton", "click", () => {
  if (getCheckbox("display_loopout_enabled")) {
    showToast("环出模式使用真实显示器身份，不能随机");
    return;
  }
  randomizeDisplayHardware();
  showToast("显示器身份已随机");
});

const displayLoopoutToggle = $("display_loopout_enabled");
if (displayLoopoutToggle) {
  displayLoopoutToggle.addEventListener("change", () => {
    refreshDisplayNativeModeOptions({
      ...(state.displayHardwarePayload || {}),
      config: {
        ...((state.displayHardwarePayload && state.displayHardwarePayload.config) || {}),
        native_mode: getString("display_native_mode"),
        loopout_enabled: getCheckbox("display_loopout_enabled"),
      },
    });
    applyDisplayLoopoutIdentityUi();
    setValidation("displayHardwareValidation", DISPLAY_IDENTITY_FIELD_IDS, []);
  });
}
}

// ---- 鼠标身份：随机 ----
function bindMouseEvents() {
on("randomMouseHardwareButton", "click", () => {
  if (state.mouseProxyMode !== "synthetic") {
    showToast("透传模式使用真实鼠标身份，不能随机");
    return;
  }
  randomizeMouseHardware();
  showToast("鼠标身份已随机");
});
}

// ---- 显示器 / 鼠标硬件信息保存并应用 ----
function bindHardwareSaveEvents() {
on("saveDisplayHardwareButton", "click", () => runUiAction(async () => {
  const confirmed = window.confirm("应用显示器模式不会重启设备；环出开关会立即切换。Windows 端可能需要重新检测 HDMI，确认现在保存并应用？");
  if (!confirmed) {
    return;
  }
  const config = validateDisplayHardware();
  setHardwareStatus("displayHardwareStatus", "应用中");
  const result = await api("/api/hardware/display", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ config, apply: true, reboot_after_apply: false }),
  });
  populateDisplayHardware({
    available: true,
    config: result.config,
    status: result.result,
    display_mode: result.display_mode,
    loopout: result.loopout,
  });
  showToast("显示器模式已保存并应用");
}));

on("saveMouseHardwareButton", "click", () => runUiAction(async () => {
  if (state.mouseProxyMode !== "synthetic") {
    throw new Error("透传模式使用真实鼠标身份，不能保存伪装信息");
  }
  const confirmed = window.confirm("应用鼠标硬件信息会重启 usb-proxy 服务，Windows 会重新枚举 USB 鼠标。确认现在保存并应用？");
  if (!confirmed) {
    return;
  }
  const config = validateMouseHardware();
  setHardwareStatus("mouseHardwareStatus", "应用中");
  const result = await api("/api/hardware/mouse", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ config, apply_now: true }),
  });
  populateMouseHardware(result);
  showToast("鼠标信息已保存并应用，Windows 会重新枚举");
}));
}

// ---- bindPresetEvents（预设参数）：7 个绑定块 ----
function bindPresetEvents() {
  on("openPresetImportButton", "click", () => setPresetImportDialogOpen(true));
  on("cleanupUnusedPresetsButton", "click", () => runUiAction(cleanupUnusedPresets));
  on("closePresetImportButton", "click", () => setPresetImportDialogOpen(false));
  on("cancelPresetImportButton", "click", () => setPresetImportDialogOpen(false));
  const presetImportDialog = $("presetImportDialog");
  on("savePresetButton", "click", () => runUiAction(async () => {
    const name = $("presetName").value.trim();
    if (!name) {
      throw new Error("请输入预设参数名称");
    }
    await applyConfigNow();
    const result = await api("/api/presets", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name, config: collectConfig() }),
    });
    showToast("预设参数已保存");
    renderPresets((await api("/api/presets")).presets || []);
    setSelectedPresetName((result && result.name) || name);
  }));

  const presetImportForm = $("presetImportForm");
}

// ---- bindLicenseEvents（授权与系统）：9 个绑定块 ----
function bindLicenseEvents() {
  const lanHostnameInput = $("lanHostnameInput");
  const webPortInput = $("webPortInput");
  on("applyNetworkAccessButton", "click", () => runUiAction(applyNetworkAccessSettings));

  on("reactivateDeviceButton", "click", () => runUiAction(async () => {
    const confirmed = window.confirm("授权修复会尝试修复更新后出现的设备身份变动问题，不会清除模型和预设参数。此功能仅用于提示授权不属于当前设备时使用，正常情况请勿点击。确认现在继续？");
    if (!confirmed) {
      return;
    }
    const result = await api("/api/system/reactivate", { method: "POST" });
    showToast(result.message || "授权修复已完成，请刷新页面后继续使用");
  }));

  on("checkUpdateButton", "click", () => runUiAction(async () => {
    const result = await checkUpdate();
    renderUpdateResult(result);
    showToast(result.update_available ? "发现可用更新" : "当前已是最新");
  }));

  on("installCustomUrlButton", "click", () => runUiAction(async () => {
    const input = $("customOtaUrlInput");
    const url = ((input && input.value) || "").trim();
    if (!url) {
      throw new Error("请先粘贴更新包 https 直链");
    }
    if (!/^https:\/\//i.test(url)) {
      throw new Error("仅支持 https 链接");
    }
    const confirmed = window.confirm(
      "将从该链接下载更新包并安装，安装会重启本地服务，过程中页面会短暂断开。确认现在安装？");
    if (!confirmed) {
      return;
    }
    renderUpdateStatus({
      status: "running",
      stage: "submit",
      message: "正在提交自定义链接更新任务",
      progress: 1,
      version: "",
    });
    // 与 installUpdatePlan 同口径：running 渲染之后上闩，避免空窗期进度条闪动。
    state.updateSessionDeadline = Date.now() + UPDATE_SESSION_TIMEOUT_MS;
    startUpdateStatusPolling();
    try {
      await api("/api/update/install", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ url }),
        // ★ 同上：提交任务时 core 会重启 ⇒ 长超时
        timeoutMs: 30000,
      });
    } catch (error) {
      stopUpdateStatusPolling();
      renderUpdateStatus({
        status: "failed",
        stage: "failed",
        message: "更新失败",
        progress: 100,
        version: "",
        error: error.message || String(error),
      });
      throw error;
    }
    showToast("更新任务已启动");
  }));

  on("installUpdateButton", "click", () => runUiAction(async () => {
    if (!state.updatePlan) {
      throw new Error("没有可安装的更新");
    }
    const confirmed = window.confirm("安装更新会重启本地服务，过程中页面会短暂断开。确认现在安装？");
    if (!confirmed) {
      return;
    }
    const button = $("installUpdateButton");
    if (button) {
      button.disabled = true;
    }
    const result = await installUpdatePlan(state.updatePlan);
    // 2026-09-21：不再在此硬画 progress=65 —— 轮询 1s 内会拉回真实阶段值，
    // 先 65 后 30 的倒退正是"UI 变来变去"的来源。进度完全由轮询单调驱动。
    showToast("更新任务已启动");
  }));

  on("refreshStorageButton", "click", () => runUiAction(() => refreshStorageStatus({ toast: true })));
  on("expandStorageButton", "click", () => runUiAction(expandStorage));

}

// ---- bindFanEvents（风扇控制）：11 个绑定块 ----
function bindFanEvents() {
  on("acceptDisclaimerButton", "click", () => hideDisclaimer(false));
  on("hideDisclaimerButton", "click", () => hideDisclaimer(true));
  on("ackAnnouncementButton", "click", acknowledgeAnnouncement);
  on("closeAnnouncementButton", "click", acknowledgeAnnouncement);

  on("closeDisplayEdidModeButton", "click", () => setDisplayEdidModeDialogOpen(false));
  on("cancelDisplayEdidModeButton", "click", () => setDisplayEdidModeDialogOpen(false));
  const displayEdidModeDialog = $("displayEdidModeDialog");
  const displayEdidModeForm = $("displayEdidModeForm");
  const displayEdidModeRefresh = $("displayEdidModeRefresh");
  on("licenseGateRefreshButton", "click", () => runUiAction(refreshLicenseStatus));

  on("licenseGateActivateButton", "click", () => runUiAction(() => activateLicenseFromInput("licenseGateKeyInput")));
}

// ---- bindGlobalEvents（窗口/键盘/指针等全局监听）：29 个绑定块 ----
function bindGlobalEvents() {
  // ★ 本函数只做编排：按关注点拆成 5 个子函数（2026-10-03）。
  // 拆分的判据是「这段监听归谁管」，不是行号。
  bindPreviewAndWindowEvents();
  bindNetworkAccessEvents();
  bindDialogEvents();
  bindModelGameSuggestEvents();
  bindPresetAndImportEvents();
}

// ---- 预览图 / 窗口尺寸 / 文档级点击（复制局域网地址） ----
function bindPreviewAndWindowEvents() {
  // ★ 必须显式取元素：裸写 `preview` 会命中 window.preview（HTML id 自动暴露的
  //   全局），img 未解析完成时那个全局不存在 ⇒ 直接 ReferenceError。
  //   同类问题见 bindModelGameSuggestEvents 的 modelGameInput/Toggle/List。
  const preview = $("previewImage");
  if (preview) {
  preview.addEventListener("load", () => {
    preview.style.visibility = "visible";
    updateAimRangeOverlay();
  });
  preview.addEventListener("error", () => {
    preview.style.visibility = "hidden";
  });
}
window.addEventListener("resize", updateAimRangeOverlay);
document.addEventListener("click", (event) => {
  closeClassOffsetPopovers();
  const copyButton = event.target && event.target.closest
    ? event.target.closest("[data-copy-lan-url]")
    : null;
  if (!copyButton || copyButton.disabled) {
    return;
  }
  runUiAction(() => copyLanAccessUrl(copyButton));
});

}

// ---- 局域网地址与端口输入 ----
function bindNetworkAccessEvents() {
if (lanHostnameInput) {
  lanHostnameInput.addEventListener("input", updateNetworkAccessButtonState);
  lanHostnameInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      runUiAction(applyNetworkAccessSettings);
    }
  });
}
if (webPortInput) {
  webPortInput.addEventListener("input", updateNetworkAccessButtonState);
  webPortInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      runUiAction(applyNetworkAccessSettings);
    }
  });
}
}

// ---- 弹窗：点外部关闭 / EDID 表单 / Esc 关闭 ----
function bindDialogEvents() {
if (modelImportDialog) {
  modelImportDialog.addEventListener("click", (event) => {
    if (event.target === modelImportDialog) {
      setModelImportDialogOpen(false);
    }
  });
}
if (modelClassNamesDialog) {
  modelClassNamesDialog.addEventListener("click", (event) => {
    if (event.target === modelClassNamesDialog) {
      setModelClassNamesDialogOpen(false);
    }
  });
}
if (presetImportDialog) {
  presetImportDialog.addEventListener("click", (event) => {
    if (event.target === presetImportDialog) {
      setPresetImportDialogOpen(false);
    }
  });
}
if (displayEdidModeDialog) {
  displayEdidModeDialog.addEventListener("click", (event) => {
    if (event.target === displayEdidModeDialog) {
      setDisplayEdidModeDialogOpen(false);
    }
  });
}
if (displayEdidModeForm) {
  displayEdidModeForm.addEventListener("submit", (event) => {
    event.preventDefault();
    saveDisplayEdidModeFromDialog();
  });
}
["displayEdidModeWidth", "displayEdidModeHeight"].forEach((id) => {
  const input = $(id);
  if (input) {
    input.addEventListener("input", updateDisplayEdidModeRefreshFromSize);
  }
});
if (displayEdidModeRefresh) {
  displayEdidModeRefresh.addEventListener("input", () => validateDisplayEdidModeInputs());
}
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && modelImportDialog && !modelImportDialog.hidden) {
    setModelImportDialogOpen(false);
  } else if (event.key === "Escape" && modelClassNamesDialog && !modelClassNamesDialog.hidden) {
    setModelClassNamesDialogOpen(false);
  } else if (event.key === "Escape" && presetImportDialog && !presetImportDialog.hidden) {
    setPresetImportDialogOpen(false);
  } else if (event.key === "Escape" && displayEdidModeDialog && !displayEdidModeDialog.hidden) {
    setDisplayEdidModeDialogOpen(false);
  }
});
}

// ---- 模型导入：游戏名自动补全 ----
function bindModelGameSuggestEvents() {
  // ★ 三个元素必须显式取：裸写会命中 window[id]（HTML id 自动暴露的全局），
  //   干净 context（元素尚未解析）下那个全局不存在 ⇒ ReferenceError。
  const modelGameInput = $("modelImportGameProfile");
  const modelGameToggle = $("modelGameSuggestionToggle");
  const modelGameList = $("modelGameSuggestionList");
if (modelGameInput) {
  modelGameInput.addEventListener("focus", () => setModelGameSuggestionOpen(true));
  modelGameInput.addEventListener("click", () => setModelGameSuggestionOpen(true));
  modelGameInput.addEventListener("input", () => {
    renderModelImportGameSuggestions();
    setModelGameSuggestionOpen(true);
  });
  modelGameInput.addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      setModelGameSuggestionOpen(true);
      const firstOption = modelGameList && modelGameList.querySelector("[data-model-game]");
      if (firstOption) {
        firstOption.focus();
      }
    } else if (event.key === "Escape") {
      event.stopPropagation();
      setModelGameSuggestionOpen(false);
    }
  });
}
if (modelGameToggle) {
  modelGameToggle.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    renderModelImportGameSuggestions();
    setModelGameSuggestionOpen(!state.modelGameSuggestionOpen);
    if (modelGameInput) {
      modelGameInput.focus();
    }
  });
}
if (modelGameList) {
  modelGameList.addEventListener("click", (event) => {
    const option = event.target.closest("[data-model-game]");
    if (!option) {
      return;
    }
    event.preventDefault();
    if (modelGameInput) {
      modelGameInput.value = option.dataset.modelGame || "";
      modelGameInput.dispatchEvent(new Event("input", { bubbles: true }));
      modelGameInput.focus();
    }
    setModelGameSuggestionOpen(false);
  });
  modelGameList.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.stopPropagation();
      setModelGameSuggestionOpen(false);
      if (modelGameInput) {
        modelGameInput.focus();
      }
    }
  });
}

}

// ---- 预设下拉 / 类别名表单 / 预设导入 / 卡密输入 / 标定页签 ----
function bindPresetAndImportEvents() {
const presetButton = $("presetSelectButton");
const presetList = $("presetSuggestionList");
if (presetButton) {
  presetButton.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    setPresetSuggestionOpen(!state.presetSuggestionOpen);
  });
  presetButton.addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      setPresetSuggestionOpen(true);
      const firstOption = presetList && presetList.querySelector("[data-preset-name]");
      if (firstOption) {
        firstOption.focus();
      }
    } else if (event.key === "Escape") {
      event.stopPropagation();
      setPresetSuggestionOpen(false);
    }
  });
}
if (presetList) {
  presetList.addEventListener("click", (event) => {
    const option = event.target.closest("[data-preset-name]");
    if (!option) {
      return;
    }
    event.preventDefault();
    setSelectedPresetName(option.dataset.presetName || "");
    setPresetSuggestionOpen(false);
    if (presetButton) {
      presetButton.focus();
    }
  });
  presetList.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.stopPropagation();
      setPresetSuggestionOpen(false);
      if (presetButton) {
        presetButton.focus();
      }
    }
  });
}

if (modelClassNamesForm) {
  modelClassNamesForm.addEventListener("submit", (event) => {
    event.preventDefault();
    runUiAction(async () => {
      const modelId = state.modelClassNamesEditModelId;
      if (!modelId) {
        throw new Error("请选择模型");
      }
      await saveModelClassNames(modelId, collectModelClassNamesEditor());
      setModelClassNamesDialogOpen(false);
      showToast("类别名称已保存");
    });
  });
}


on("loadPresetButton", "click", () => runUiAction(async () => {
  const name = getSelectedPresetName();
  await loadPresetByName(name);
}));

on("deletePresetButton", "click", () => runUiAction(async () => {
  const name = getSelectedPresetName();
  await deletePresetByName(name);
}));

if (presetImportForm) {
  presetImportForm.addEventListener("submit", (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    runUiAction(async () => {
      const formData = new FormData(form);
      const file = formData.get("file");
      const requestedName = String(formData.get("name") || "").trim();
      if (!requestedName && file && typeof file.name === "string") {
        formData.set("name", file.name.replace(/\.json$/i, ""));
      }
      const result = await api("/api/presets/import", {
        method: "POST",
        body: formData,
      });
      form.reset();
      setPresetImportDialogOpen(false);
      showToast("预设参数导入成功");
      renderPresets((await api("/api/presets")).presets || []);
      if (result && result.name) {
        setSelectedPresetName(result.name);
      }
    });
  });
}

["licenseKeyInput", "licenseGateKeyInput"].forEach((id) => {
  const input = $(id);
  if (!input) {
    return;
  }
  input.addEventListener("input", () => syncLicenseKeyInputs(input));
  input.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      runUiAction(() => activateLicenseFromInput(id));
    }
  });
});



document.querySelectorAll("[data-control-section-target='control-section-calib']").forEach((tab) => {
  tab.addEventListener("click", () => {
    refreshCalibration().catch(() => {});
  });
});

}


// ★ 2026-10-03 性能 A：连续失败多少次才把徽章刷成「未连接」。
//   原来**任何一次**失败就立刻改徽章 ⇒ 单次网络抖动 / core 重启瞬间，
//   用户就看到「未连接」闪一下（业主反馈"有时候会报core 未连接"）。
const LIVE_POLL_FAIL_THRESHOLD = 3;

function handleLivePollError(error) {
  const message = error && error.message ? error.message : String(error || "连接已断开");
  if (suppressMouseSwitchDaemonTimeout(message)) {
    return;
  }
  state.livePollFailCount = (state.livePollFailCount || 0) + 1;
  // ★ 未达阈值：只记数，不改徽章、不弹 toast —— 单次抖动不该惊动用户。
  if (state.livePollFailCount < LIVE_POLL_FAIL_THRESHOLD) {
    return;
  }
  if (!state.livePollErrorNotified) {
    showToast(message, true);
    state.livePollErrorNotified = true;
  }
  const badge = $("statusBadge");
  if (badge) {
    badge.textContent = "未连接";
    badge.className = "status-badge error";
  }
}

async function pollLiveState() {
  if (state.livePollInFlight) {
    return;
  }
  state.livePollInFlight = true;
  try {
    let payload = await api("/api/state");
    payload = await maybeRunLicenseRecovery(payload);
    applyLiveState(payload);
    // ★ 成功即清零：否则失败攒到阈值后，即使恢复也永远显示「未连接」
    state.livePollFailCount = 0;
    state.livePollErrorNotified = false;
  } catch (error) {
    handleLivePollError(error);
  } finally {
    state.livePollInFlight = false;
  }
}

function initLiveStatePolling() {
  if (state.livePollTimer) {
    window.clearInterval(state.livePollTimer);
  }
  state.livePollTimer = window.setInterval(() => {
    pollLiveState();
  }, 1500);
}

async function main() {
  applyBrand({ ui_brand: document.documentElement.dataset.uiBrand || UI_BRAND_YU });
  initThemeControls();
  fillOptions($("recoil_hotkey"), POINTER_HOTKEYS);
  fillOptions($("recoil_hotkey2"), OPTIONAL_POINTER_HOTKEYS);
  fillOptions($("hotkey_guard_toggle_hotkey"), POINTER_HOTKEYS);
  // 自动开火 / 自动开火 2.0 的按键下拉（长按键必须有键；「不使用」只在可选位出现）
  fillOptions($("trigger2_key1"), POINTER_HOTKEYS);
  fillOptions($("trigger2_key2"), OPTIONAL_POINTER_HOTKEYS);
  fillOptions($("trigger2_fire_button"), POINTER_HOTKEYS);
  registerNumericLimitsFromMarkup();
  initPageNavigation();
  initControlSectionNavigation();
  initAssistSectionNavigation();
  enhanceNumericRangeControls();
  hardenInputAutofillHints();
  enableThumbOnlyRangeInputs();
  initRangeBindings();
  bindHardwareInputFilters();
  bindConfigAutoApply();
  bindAssistModuleCollapseState();
  bindEvents();
  initSystemPolling();

  try {
    await refreshAll();
  } catch (error) {
    document.body.classList.remove("license-loading");
    setApplyStatus("error", "连接失败");
    const badge = $("statusBadge");
    if (badge) {
      badge.textContent = "未连接";
      badge.className = "status-badge error";
    }
    showToast(error.message || String(error), true);
  }

  maybeShowDisclaimer();
  refreshInitialUpdateStatus().catch(() => {});
  startUpdateCompletionWatcher();
  initLiveStatePolling();
}

window.addEventListener("DOMContentLoaded", () => {
  main().catch((error) => showToast(error.message || String(error), true));
});

