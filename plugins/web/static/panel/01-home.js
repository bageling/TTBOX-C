// ==========================================================================
// 01-home.js —— 面板「总览」（section#home-page）
// ==========================================================================
// ★ 本页签的控件共 31 个（实测自index.html 的 section#home-page）。
//   专属逻辑只有 updateAimRangeOverlay（实时预览的瞄准框投影）。
//
// ★ 加新功能放哪：
//   · 总览页专属的渲染 / 事件 → 本文件
//   · 涉及配置项的读写→ 10-flow.js（collectConfig / populateForm）
//
// ★ 本文件被 10-flow.js 调用 14 次（最多），是总览刷新链的落点。
// ==========================================================================
function updateAimRangeOverlay() {
  const overlay = $("aimRangeOverlay");
  const dot = $("aimReferenceDot");
  const stage = (overlay || dot) ? (overlay || dot).closest(".preview-stage") : null;
  if (!stage) {
    return;
  }

  const cropSize = getCropSize(state.config && state.config.capture ? state.config.capture.crop_size : 320);
  const layout = getPreviewImageLayout(stage, cropSize);
  if (!layout) {
    return;
  }

  if (overlay) {
    // 下限必须与保存侧同值（OVERVIEW_FOV_FACTOR_MIN）：倍率 0 在 core 侧撞
    // 「FOV 半径必须在 (0,1]」会被整份拒收；画布上也会把圆缩成一个点。
    const rangeFactor = clamp(
      getNumber("range_factor", state.config ? state.config.range_factor : 1),
      OVERVIEW_FOV_FACTOR_MIN,
      1
    );
    // 边长 = 截取尺寸 × 倍率 = 瞄准范围圆的**直径**（倍率 1.00 即截取区内接圆）。
    const side = cropSize * rangeFactor * layout.cropScale;
    overlay.style.left = `${layout.imageLeft + layout.imageWidth * 0.5}px`;
    overlay.style.top = `${layout.imageTop + layout.imageHeight * 0.5}px`;
    overlay.style.width = `${side}px`;
  }

  if (dot) {
    // ★ V1.0.13：准星 = 裁剪区正中心（core 侧 aim_offset_* 已删）。这个点是**只读**的
    //   几何提示，不再跟着任何输入框走 —— 面板上没有能移它的控件了。
    const referenceX = cropSize * 0.5;
    const referenceY = cropSize * 0.5;
    dot.style.left = `${layout.imageLeft + referenceX * layout.xScale}px`;
    dot.style.top = `${layout.imageTop + referenceY * layout.yScale}px`;
  }
}

