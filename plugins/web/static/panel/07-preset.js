// ==========================================================================
// 07-preset.js —— 面板「预设参数」（section#preset-page）
// ==========================================================================
// ★ 本页签的控件共 17 个（实测自 index.html 的 section#preset-page）。
//   专属逻辑只有 setPresetImportDialogOpen（导入弹窗开合）。
//
// ★ 加新功能放哪：预设相关 → 本文件；
//   预设的读写后端在 api/presets.py。
// ==========================================================================
function setPresetImportDialogOpen(open) {
  const dialog = $("presetImportDialog");
  if (!dialog) {
    return;
  }
  const form = $("presetImportForm");
  dialog.hidden = !open;
  setAnyModalOpen();
  if (open) {
    const fileInput = $("presetImportFile") || dialog.querySelector('input[name="file"]');
    if (fileInput) {
      fileInput.focus();
    }
  } else if (form) {
    form.reset();
  }
}

