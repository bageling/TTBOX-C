// ==========================================================================
// 05-model.js —— 面板「模型库」（section#model-page）
// ==========================================================================
// ★ 本页签的控件共 33 个（实测自 index.html 的 section#model-page）。
//
// ★ 本文件装什么（模型导入 / 导入弹窗 / 游戏配置建议）：
//   updateModelImportMode / setModelImportBusy / renderModelPanel
//   setModelImportDialogOpen / setModelGameSuggestionOpen
//
// ★ 加新功能放哪：模型相关的渲染 / 导入流程→ 本文件
// ==========================================================================
function updateModelImportMode() {
  const form = $("modelImportForm");
  if (!form) {
    return;
  }
  const importType = currentModelImportType(form);
  const fileInput = $("modelImportFile") || form.querySelector('input[name="file"]');
  const zipField = $("modelCalibrationZipField");
  const zipInput = $("modelCalibrationZip");
  const hint = $("modelImportHint");
  const submit = $("submitModelImportButton");
  const isOnnx = importType === "onnx";

  if (fileInput) {
    fileInput.accept = isOnnx ? ".onnx" : ".rknn,.rknn.enc,.enc";
  }
  if (zipField) {
    zipField.hidden = !isOnnx;
  }
  if (zipInput) {
    zipInput.required = false;
    if (!isOnnx) {
      zipInput.value = "";
    }
  }
  if (hint) {
    hint.hidden = !isOnnx;
  }
  if (submit && !submit.disabled) {
    submit.textContent = isOnnx ? "转换并导入" : "导入";
  }
}

function setModelImportBusy(busy, message = "") {
  const form = $("modelImportForm");
  const status = $("modelImportStatus");
  const submit = $("submitModelImportButton");
  const cancel = $("cancelModelImportButton");
  const close = $("closeModelImportButton");

  if (form) {
    Array.from(form.elements).forEach((element) => {
      element.disabled = busy;
    });
  }
  if (submit) {
    submit.disabled = busy;
    const importType = currentModelImportType(form);
    submit.textContent = busy ? "转换中..." : (importType === "onnx" ? "转换并导入" : "导入");
  }
  if (cancel) {
    cancel.disabled = busy;
  }
  if (close) {
    close.disabled = busy;
  }
  if (status) {
    status.hidden = !busy;
    status.textContent = message || "转换中...";
  }
}

function renderModelPanel(models, selectedModelId) {
  const availableModels = models || [];
  const currentModel = availableModels.find((model) => model.id === selectedModelId);
  const name = $("modelCurrentName");
  const meta = $("modelCurrentMeta");
  const summary = $("modelLibrarySummary");

  // ★ 签名守卫（2026-10-03 性能 B）：显示内容没变就别写 DOM。
  //   textContent 赋值即使值相同也会触发 MutationObserver → 浏览器重排。
  //   实测：轮询每 1.5s 调一次，静止 12 秒产生 997 次 DOM 变更。
  const nextText = currentModel
    ? [
      modelDimension(currentModel),
      modelBackendLabel(currentModel),
      modelOutputLabel(currentModel),
      modelProfileLabel(currentModel.game_profile),
      currentModel.preset_name ? `绑定预设：${currentModel.preset_name}` : "",
    ].filter(Boolean).join(" / ")
    : "等待模型信息";
  const nextSignature = [
    currentModel ? modelFileName(currentModel) : "",
    nextText,
    availableModels.length,
  ].join("\u001e");
  if (state.modelPanelSignature === nextSignature) {
    return;
  }
  state.modelPanelSignature = nextSignature;

  if (name) {
    name.textContent = currentModel ? modelFileName(currentModel) : "尚未选择模型";
  }
  if (meta) {
    meta.textContent = nextText;
  }
  if (summary) {
    summary.textContent = `${availableModels.length} 个可用`;
  }
}

function setModelGameSuggestionOpen(open) {
  const input = $("modelImportGameProfile");
  const list = $("modelGameSuggestionList");
  const combo = $("modelGameCombobox");
  const toggle = $("modelGameSuggestionToggle");
  const hasVisibleOptions = filteredModelImportGames().length > 0;
  const nextOpen = !!open && hasVisibleOptions;

  state.modelGameSuggestionOpen = nextOpen;
  if (list) {
    list.hidden = !nextOpen;
  }
  if (input) {
    input.setAttribute("aria-expanded", nextOpen ? "true" : "false");
  }
  if (combo) {
    combo.classList.toggle("is-open", nextOpen);
  }
  if (toggle) {
    toggle.disabled = state.modelGameOptions.length === 0;
    toggle.setAttribute("aria-expanded", nextOpen ? "true" : "false");
  }
}

function setModelImportDialogOpen(open) {
  const dialog = $("modelImportDialog");
  if (!dialog) {
    return;
  }
  const form = $("modelImportForm");
  if (!open) {
    setModelImportBusy(false);
    setModelGameSuggestionOpen(false);
  }
  dialog.hidden = !open;
  setAnyModalOpen();
  if (open) {
    updateModelImportMode();
    renderModelImportGameSuggestions();
    const fileInput = $("modelImportFile") || dialog.querySelector('input[name="file"]');
    if (fileInput) {
      fileInput.focus();
    }
  } else if (form) {
    form.reset();
    renderModelImportGameSuggestions();
    updateModelImportMode();
  }
}

