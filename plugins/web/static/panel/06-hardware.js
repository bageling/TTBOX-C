// ==========================================================================
// 06-hardware.js —— 面板「显示与鼠标」（section#hardware-page）
// ==========================================================================
// ★ 本页签的控件共 45 个（实测自 index.html 的 section#hardware-page）
//   —— 是 9 个页签里控件最多的。
//
// ★ 本文件装什么（鼠标 / 显示器的读写与校验）：
//   applyDisplayLoopoutIdentityUi / populateDisplayHardware
//   validateDisplayHardware / collectDisplayHardware
//   randomizeDisplayHardware / updateMouseHardwareModeUi
//   validateMouseHardware / collectMouseHardware / randomizeMouseHardware
//
// ★ 加新功能放哪：硬件相关的 → 本文件。
//   ★ 与后端对应：api/hardware.py（同样按 probe 拆过）
// ==========================================================================
function applyDisplayLoopoutIdentityUi() {
  const loopoutEnabled = getCheckbox("display_loopout_enabled");
  const identity = state.displayRealMonitor;
  if (loopoutEnabled && identity) {
    setValue("display_name", identity.name);
    setValue("display_vendor", identity.vendor);
    setValue("display_product_id", identity.product_id);
    setValue("display_serial", identity.serial);
  }
  setDisplayIdentityReadonly(loopoutEnabled);
  updateDisplayCustomModeButtonUi();
}

function populateDisplayHardware(payload) {
  const config = (payload && payload.config) || {};
  state.displayHardwarePayload = payload || null;
  state.displayRealMonitor = realMonitorIdentityFromPayload(payload);
  setValue("display_device", config.device || "auto");
  setValue("display_profile", config.profile || "boot-safe-full");
  setCheckbox("display_native_only", !!config.native_only);
  setCheckbox("display_loopout_enabled", !!config.loopout_enabled);
  setValue("display_loopout_pixel_format", config.loopout_pixel_format || "rgb888");
  refreshDisplayNativeModeOptions(payload);
  setValue("display_native_mode", config.native_mode || "");
  setValue("display_name", config.name || "OPI-COMPAT");
  setValue("display_vendor", config.vendor || "OPI");
  setValue("display_product_id", config.product_id || "0x3588");
  setValue("display_serial", config.serial || "0x20260414");

  const status = payload && payload.status;
  const log = $("displayHardwareLog");
  if (log) {
    log.textContent = status && status.output ? status.output.trim() || "无输出" : "等待读取";
  }
  setHardwareStatus("displayHardwareStatus", payload && payload.available ? "可用" : "未安装", !!(payload && payload.available));
  renderDisplayModeSummary(payload);
  applyDisplayLoopoutIdentityUi();
}

function collectDisplayHardware() {
  const vendor = vendorCode(getString("display_vendor")) || "OPI";
  return {
    device: getString("display_device") || "auto",
    profile: getString("display_profile") || "boot-safe-full",
    native_mode: getString("display_native_mode"),
    native_only: getCheckbox("display_native_only"),
    loopout_enabled: getCheckbox("display_loopout_enabled"),
    loopout_pixel_format: getString("display_loopout_pixel_format") || "rgb888",
    name: printableAscii(getString("display_name"), 13) || "OPI-COMPAT",
    vendor,
    product_id: hexText(getString("display_product_id"), 4) || "0x3588",
    serial: hexText(getString("display_serial"), 8) || "0x20260414",
  };
}

function validateDisplayHardware() {
  const fieldIds = ["display_native_mode", "display_name", "display_vendor", "display_product_id", "display_serial", "display_native_only", "display_loopout_enabled", "display_loopout_pixel_format"];
  const messages = [];
  const nativeMode = getString("display_native_mode");
  const nativeOnly = getCheckbox("display_native_only");
  const loopoutEnabled = getCheckbox("display_loopout_enabled");
  const loopoutPixelFormat = getString("display_loopout_pixel_format") || "rgb888";
  if (loopoutEnabled && state.displayRealMonitor) {
    applyDisplayLoopoutIdentityUi();
  }
  const name = getString("display_name");
  const vendor = getString("display_vendor").toUpperCase();
  const productId = normalizeHexValue(getString("display_product_id"), 4);
  const serial = normalizeHexValue(getString("display_serial"), 8);

  if (!displayNativeModeAllowedValues().includes(nativeMode)) {
    messages.push({ id: "display_native_mode", text: "首选模式必须从列表中选择。" });
  }
  if (nativeOnly && !nativeMode) {
    messages.push({ id: "display_native_only", text: "仅输出首选模式需要先选择一个首选模式。" });
  }
  if (!["bgr888", "rgb888"].includes(loopoutPixelFormat)) {
    messages.push({ id: "display_loopout_pixel_format", text: "环出颜色格式必须选择 BGR888 或 RGB888。" });
  }
  if (!name || name !== printableAscii(name, 13)) {
    messages.push({ id: "display_name", text: "显示器名称只能填写 1-13 个英文、数字或 ASCII 符号。" });
  }
  if (!/^[A-Z]{3}$/.test(vendor)) {
    messages.push({ id: "display_vendor", text: "厂商代码必须是 3 个大写英文字母，例如 LVP、OPI、DEL。" });
  }
  if (!productId) {
    messages.push({ id: "display_product_id", text: "产品 ID 必须是非零十六进制，范围 0x0001-0xffff。" });
  }
  if (!serial) {
    messages.push({ id: "display_serial", text: "序列号必须是非零十六进制，范围 0x00000001-0xffffffff。" });
  }

  setValidation("displayHardwareValidation", fieldIds, messages);
  if (messages.length > 0) {
    throw new Error("显示器硬件信息不符合规范");
  }

  setValue("display_vendor", vendor);
  setValue("display_product_id", productId);
  setValue("display_serial", serial);
  return {
    device: getString("display_device") || "auto",
    profile: getString("display_profile") || "boot-safe-full",
    native_mode: nativeMode,
    native_only: nativeOnly,
    loopout_enabled: loopoutEnabled,
    loopout_pixel_format: loopoutPixelFormat,
    name,
    vendor,
    product_id: productId,
    serial,
  };
}

function randomizeDisplayHardware() {
  if (getCheckbox("display_loopout_enabled")) {
    applyDisplayLoopoutIdentityUi();
    return;
  }
  const vendor = randomLetters(3);
  setValue("display_vendor", vendor);
  setValue("display_name", `${vendor}-${randomInt(0x100000, 0xffffff).toString(16).toUpperCase()}`.slice(0, 13));
  setValue("display_product_id", randomHex(4, 1, 0xfffe));
  setValue("display_serial", randomHex(8, 1, 0xffffffff));
  setValidation("displayHardwareValidation", ["display_name", "display_vendor", "display_product_id", "display_serial"], []);
}

function updateMouseHardwareModeUi(payload = null) {
  // 2026-09-22：不再读单选（已删除），只认实际模式。
  const mode = (payload && (payload.effective_mode || payload.mode))
    || state.mouseProxyMode || "full_passthrough";
  const isSynthetic = mode === "synthetic";
  const editableIds = [
    "mouse_usb_vid",
    "mouse_usb_pid",
    "mouse_usb_manufacturer",
    "mouse_usb_product",
    "mouse_usb_serial",
    "mouse_usb_configuration",
  ];
  editableIds.forEach((id) => {
    const el = $(id);
    if (el) {
      el.disabled = !isSynthetic;
    }
  });
  const randomButton = $("randomMouseHardwareButton");
  const saveButton = $("saveMouseHardwareButton");
  if (randomButton) {
    randomButton.disabled = !isSynthetic;
  }
  if (saveButton) {
    saveButton.disabled = !isSynthetic;
  }
  const hint = $("mouseHardwareModeHint");
  if (hint) {
    if (isSynthetic) {
      hint.textContent = "合成模式可保存随机身份，并通过标准 HID 鼠标输出。";
    } else {
      hint.textContent = "完整透传会暴露原始复合 HID 接口，用于厂商驱动和真实鼠标信息。";
    }
  }
}

function collectMouseHardware() {
  return {
    usb_vid: hexText(getString("mouse_usb_vid"), 4),
    usb_pid: hexText(getString("mouse_usb_pid"), 4),
    usb_bcd_usb: getString("mouse_usb_bcd_usb"),
    usb_bcd_device: getString("mouse_usb_bcd_device"),
    usb_device_class: getNumber("mouse_usb_device_class", 0),
    usb_device_subclass: getNumber("mouse_usb_device_subclass", 0),
    usb_device_protocol: getNumber("mouse_usb_device_protocol", 0),
    usb_max_power: getNumber("mouse_usb_max_power", 250),
    hid_protocol: getNumber("mouse_hid_protocol", 2),
    hid_subclass: getNumber("mouse_hid_subclass", 1),
    hid_report_length: getNumber("mouse_hid_report_length", 4),
    hid_interval: getNumber("mouse_hid_interval", 1),
    usb_manufacturer: printableAscii(getString("mouse_usb_manufacturer"), 48),
    usb_product: printableAscii(getString("mouse_usb_product"), 64),
    usb_serial: printableAscii(getString("mouse_usb_serial"), 64),
    usb_configuration: printableAscii(getString("mouse_usb_configuration"), 32) || "Mouse",
    hid_report_desc_hex: getString("mouse_hid_report_desc_hex").replace(/\s+/g, ""),
  };
}

function validateMouseHardware() {
  const fieldIds = [
    "mouse_usb_vid",
    "mouse_usb_pid",
    "mouse_usb_manufacturer",
    "mouse_usb_product",
    "mouse_usb_serial",
    "mouse_usb_configuration",
  ];
  const messages = [];
  const vid = normalizeHexValue(getString("mouse_usb_vid"), 4);
  const pid = normalizeHexValue(getString("mouse_usb_pid"), 4);
  const manufacturer = getString("mouse_usb_manufacturer");
  const product = getString("mouse_usb_product");
  const serial = getString("mouse_usb_serial");
  const configuration = getString("mouse_usb_configuration");

  if (!vid) {
    messages.push({ id: "mouse_usb_vid", text: "VID 必须是非零十六进制，范围 0x0001-0xffff。" });
  }
  if (!pid) {
    messages.push({ id: "mouse_usb_pid", text: "PID 必须是非零十六进制，范围 0x0001-0xffff。" });
  }
  [
    ["mouse_usb_manufacturer", "制造商", manufacturer, 48],
    ["mouse_usb_product", "产品名", product, 64],
    ["mouse_usb_serial", "序列号", serial, 64],
    ["mouse_usb_configuration", "配置名", configuration, 32],
  ].forEach(([id, label, value, maxLength]) => {
    if (!value || value !== printableAscii(value, maxLength)) {
      messages.push({ id, text: `${label}只能填写 1-${maxLength} 个英文、数字或 ASCII 符号。` });
    }
  });

  setValidation("mouseHardwareValidation", fieldIds, messages);
  if (messages.length > 0) {
    throw new Error("鼠标硬件信息不符合规范");
  }

  setValue("mouse_usb_vid", vid);
  setValue("mouse_usb_pid", pid);
  return {
    ...collectMouseHardware(),
    usb_vid: vid,
    usb_pid: pid,
    usb_manufacturer: manufacturer,
    usb_product: product,
    usb_serial: serial,
    usb_configuration: configuration,
  };
}

function randomizeMouseHardware() {
  const brand = randomMouseBrand();
  setValue("mouse_usb_vid", randomUsbId(0x1000, 0xefff, [0x1d6b]));
  setValue("mouse_usb_pid", randomUsbId(0x0001, 0xfffe));
  setValue("mouse_usb_manufacturer", brand);
  setValue("mouse_usb_product", `${brand} USB Optical Mouse`);
  setValue("mouse_usb_serial", randomSerial(brand.slice(0, 2).toUpperCase()));
  setValidation("mouseHardwareValidation", [
    "mouse_usb_vid",
    "mouse_usb_pid",
    "mouse_usb_manufacturer",
    "mouse_usb_product",
    "mouse_usb_serial",
    "mouse_usb_configuration",
  ], []);
}

