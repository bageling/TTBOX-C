// ==========================================================================
// 08-license.js —— 面板「授权与系统」license-page
// ==========================================================================
// ★ 本页签的控件共 24 个（实测自 index.html 的 section#license-page）。
//
// ★ 本文件装什么（授权 / 存储扩容 / 检查更新）：
//   renderUpdateStatus / renderLicensePanel / renderStorageExpansion
//   renderUpdateResult
//
// ★ 注意：授权的**唯一真相**是后端 core IPC（lib/branding.py 的
//   _license_block）。前端只渲染，不推导 —— 改这里不能改授权语义。
// ==========================================================================
function renderStorageExpansion(storage) {
  const pill = $("storageExpandPill");
  const summary = $("storageExpandSummary");
  const button = $("expandStorageButton");
  const log = $("storageExpandLog");
  if (!pill && !summary && !button && !log) {
    return;
  }
  // ★ 只认"扩容数据源"（/api/system/storage 的响应带 rootfs 对象）。其余调用方（如
  //   /api/system 的磁盘简表、扩容接口的 {message,detail} 回执）**不含 rootfs**，此时必须
  //   原样保留既有扩容 UI —— 否则空 rootfs 会被 storageExpandLabel 判成"未读取"、按钮
  //   disabled=true，且 2.5s 轮询反复覆盖，用户点"刷新"读到的状态活不过一个轮询周期。
  if (!storage || typeof storage.rootfs !== "object" || storage.rootfs === null) {
    return;
  }
  const rootfs = storage.rootfs;
  const usage = storageUsage(storage || {});
  // 按钮可点 = 物理上可扩 且 执行通道已装箱；只满足前者时保持禁用（后端会返 501）。
  const expandable = !!rootfs.expandable && rootfs.action_available !== false;
  if (pill) {
    pill.textContent = state.storageExpandBusy ? "扩容中" : storageExpandLabel(rootfs);
    pill.className = `pill${rootfs.ok === false ? " danger-pill" : ""}`;
  }
  if (summary) {
    summary.innerHTML = `
      <div class="runtime-stat">
        <span>根分区容量</span>
        <strong>${formatBytes(usage.used)} / ${formatBytes(usage.total)}</strong>
        <small>已用 ${formatPercent(usage.percent)}</small>
      </div>
      <div class="runtime-stat">
        <span>剩余容量</span>
        <strong>${formatBytes(usage.free)}</strong>
        <small>${escapeHtml(rootfs.message || "等待检测根分区状态")}</small>
      </div>
    `;
  }
  if (button) {
    button.disabled = state.storageExpandBusy || !expandable;
  }
  if (log) {
    log.textContent = storageExpandLog(storage);
  }
}

function renderLicensePanel(payload) {
  const license = (payload && payload.license) || {};
  state.currentVersion = (payload && payload.version) || state.currentVersion || "";
  applyBrand(payload);
  const statusPill = $("licenseStatusPill");
  const plan = $("licensePlan");
  const summary = $("licenseSummary");
  const installButton = $("installUpdateButton");
  const activated = !!license.valid;
  const recovery = (payload && payload.recovery) || {};
  const recoveryMessage = recovery.message || "";
  const statusText = recovery.recovered
    ? "已自动修复当前设备授权"
    : recoveryMessage
      ? friendlyLicenseMessage(recoveryMessage, recoveryMessage)
      : friendlyLicenseMessage(license.message, activated ? "已激活" : "未激活");
  if (statusPill) {
    statusPill.textContent = activated ? "已激活" : "未激活";
    statusPill.className = `pill${activated ? "" : " danger-pill"}`;
  }
  if (plan) {
    plan.textContent = license.plan === "permanent"
      ? "永久授权"
      : (license.plan === "trial" ? "试用授权" : (license.plan || "未授权"));
  }
  if (summary) {
    const expiresAt = license.expires_at ? formatDateTime(license.expires_at) : "--";
    summary.innerHTML = `
      <div class="runtime-stat">
        <span>授权状态</span>
        <strong>${escapeHtml(statusText)}</strong>
      </div>
      <div class="runtime-stat">
        <span>到期时间</span>
        <strong>${escapeHtml(expiresAt)}</strong>
      </div>
      <div class="runtime-stat">
        <span>当前版本</span>
        <strong>${escapeHtml((payload && payload.version) || "--")}</strong>
      </div>
    `;
  }
  if (installButton && !state.updatePlan) {
    installButton.disabled = true;
  }
  updateLicenseGateStatus(statusText || (activated ? "已激活" : "设备未激活，请输入激活码后继续使用。"));
}

function renderUpdateResult(payload) {
  // 2026-09-18 定案：check 返回 {update_available, current_version, latest_version,
  // package_url, sign_url, key_id}（服务器一次查询回三样，O11 契约）。
  // 2026-09-20 业主定案：删除独立版本行 summary，版本信息并入详情面板消息，只留一套 UI。
  const pill = $("updateStatusPill");
  const notes = $("updateReleaseNotes");
  const installButton = $("installUpdateButton");
  const hasUpdate = !!(payload && payload.update_available && payload.package_url);
  state.updatePlan = hasUpdate ? {
    latest_version: payload.latest_version || "",
    package_url: payload.package_url || "",
    key_id: payload.key_id || "",
  } : null;
  const versionLine = payload
    ? `当前版本 ${(payload && payload.current_version) || "--"}　·　最新版本 ${(payload && payload.latest_version) || "--"}`
    : "";
  if (pill) {
    // 2026-09-21 修复：failed 后 danger-pill 残留会让"已是最新"显示成红色
    pill.textContent = hasUpdate ? "发现更新" : "已是最新";
    pill.className = "pill";
  }
  if (notes) {
    const nextNotes = (payload && (payload.release_notes || payload.notes)) || "暂无更新信息";
    if (notes.textContent !== nextNotes) {
      notes.textContent = nextNotes;
    }
  }
  if (installButton) {
    installButton.disabled = !hasUpdate;
  }
  if (!state.updateStatus || state.updateStatus.status !== "running") {
    renderUpdateStatus({
      status: "idle",
      progress: 0,
      stateLabel: hasUpdate ? "发现更新" : "已是最新",
      message: versionLine || "点「检查更新」开始。",
    });
  }
}

// 2026-09-18 定案：切换版本/版本列表功能随「禁止降级、服务器即唯一真源」删除
// （updateVersions / 版本选择对话框 / switchToSelectedUpdateVersion 整组移除）。

function renderUpdateStatus(status) {
  const payload = status || {};
  let statusValue = String(payload.status || "idle");
  // 2026-09-22 业主定案：陈旧的成功一律不渲染。ota_status.json 永久停在 SUCCESS，
  // 照实画就会让每次开新页面都看到一条「更新成功 100%」的绿条，并与空闲态来回跳。
  // 判据同下方监视器：finished_at 必须晚于本页打开时刻（容忍 60s 时钟/写入误差）。
  if (statusValue === "success") {
    const finishedAt = Number(payload.finished_at) || 0;
    const openedAt = Number(state.updatePageOpenedAtSec) || 0;
    if (!finishedAt || !openedAt || finishedAt < openedAt - 60) {
      statusValue = "idle";
      payload.progress = 0;
    }
  }
  // 更新会话闩：从「安装已提交」到「更新器写出 RUNNING」之间，状态端点会先返回 idle。
  // 若照实渲染，进度条就会闪没、下一拍又冒出来。窗口内一律忽略 idle。
  if (statusValue === "idle" && state.updateSessionDeadline && Date.now() < state.updateSessionDeadline) {
    return;
  }
  if (statusValue !== "idle") {
    state.updateSessionDeadline = 0;
  }
  const prev = state.updateStatus;
  let progress = Math.max(0, Math.min(100, Number(payload.progress) || 0));
  if (statusValue === "running" && prev && prev.status === "running") {
    progress = Math.max(progress, Number(prev.progress) || 0);
  }
  const message = payload.error ? `原因：${payload.error}` : (payload.message || "点「检查更新」开始。");
  const renderKey = [statusValue, Math.round(progress), message, payload.version || ""].join("|");
  if (renderKey === updateRenderKey) {
    return;
  }
  updateRenderKey = renderKey;
  state.updateStatus = { ...payload, progress };
  const panel = $("updateProgressPanel");
  const percent = $("updateProgressPercent");
  const bar = $("updateProgressBar");
  const messageEl = $("updateProgressMessage");
  const pill = $("updateStatusPill");
  const notes = $("updateReleaseNotes");
  const installButton = $("installUpdateButton");
  const checkButton = $("checkUpdateButton");
  const running = statusValue === "running";
  const finished = statusValue === "success" || statusValue === "failed";
  if (statusValue === "success") {
    state.updatePlan = null;
  }

  if (panel) {
    panel.classList.toggle("is-success", statusValue === "success");
    panel.classList.toggle("is-failed", statusValue === "failed");
    // 2026-09-20 修正：空闲态不再隐藏进度头/进度条（那正是"一会有一会没有"），
    // 只把数值归零、整体淡一档，面板结构保持恒定。
    panel.classList.toggle("is-idle", !running && !finished);
  }
  if (percent) {
    // 数值只在真正有进度时出现；空闲留空但由 .update-progress-head 的 min-height 占位，
    // 高度不塌陷 ⇒ 布局不跳。
    percent.textContent = running || finished ? `${Math.round(progress)}%` : "";
  }
  if (bar) {
    bar.style.width = `${running || finished ? progress : 0}%`;
  }
  if (messageEl) {
    messageEl.textContent = message;
  }
  if (pill) {
    if (running || finished) {
      pill.textContent = updateStatusLabel(payload);
      pill.className = `pill${statusValue === "failed" ? " danger-pill" : ""}`;
    } else if (payload.stateLabel) {
      pill.textContent = payload.stateLabel;
      pill.className = "pill";
    } else {
      // 2026-09-20：idle 且无 stateLabel 时不再保留上一轮的 danger-pill 红底
      // （失败后回到「待检查」，文字已变而底色还红）。
      pill.className = "pill";
    }
  }
  if (notes && payload.error) {
    notes.textContent = `更新失败\n${payload.error}`;
  } else if (notes && statusValue === "success") {
    const version = payload.version ? `版本：${payload.version}` : "";
    notes.textContent = ["更新安装成功", version, "页面即将刷新。"].filter(Boolean).join("\n");
  }
  if (installButton) {
    installButton.disabled = running || !state.updatePlan;
  }
  if (checkButton) {
    checkButton.disabled = running;
  }
}

