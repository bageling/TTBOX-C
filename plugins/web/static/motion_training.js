(() => {
  "use strict";

  const canvas = document.getElementById("motionTrainingCanvas");
  if (!canvas) return;

  const $ = (id) => document.getElementById(id);
  const ui = {
    section: $("control-section-motion-training"),
    lease: $("motionTrainingLeaseBadge"),
    shell: $("motionTrainingCanvasShell"),
    unavailable: $("motionTrainingUnavailable"),
    start: $("motionTrainingStart"),
    stop: $("motionTrainingStop"),
    retry: $("motionTrainingRetry"),
    message: $("motionTrainingMessage"),
    progress: $("motionTrainingProgress"),
    reactionCount: $("motionTrainingReactionCount"),
    continuousCount: $("motionTrainingContinuousCount"),
    reactionMean: $("motionTrainingReactionMean"),
    efficiency: $("motionTrainingPathEfficiency"),
    qualityBadge: $("motionTrainingQualityBadge"),
    qualityBar: $("motionTrainingQualityBar"),
    coverage: $("motionTrainingModelCoverage"),
    modelState: $("motionTrainingModelState"),
    train: $("motionTrainingTrain"),
    activate: $("motionTrainingActivate"),
    clear: $("motionTrainingClear"),
    curve: $("motionTrainingCurveBlend"),
    speed: $("motionTrainingSpeedBlend"),
    reaction: $("motionTrainingReactionBlend"),
    delay: $("motionTrainingMaxDelay"),
    curveValue: $("motionTrainingCurveValue"),
    speedValue: $("motionTrainingSpeedValue"),
    reactionValue: $("motionTrainingReactionValue"),
    delayValue: $("motionTrainingDelayValue"),
  };

  const ctx = canvas.getContext("2d", { alpha: false, desynchronized: true });
  const goals = { reaction: 72, continuous: 96 };
  const state = {
    profiles: [],
    activeProfileId: "",
    enabled: false,
    sessionId: "",
    collecting: false,
    uploadPending: false,
    uploadFailed: false,
    pendingSample: null,
    pausedForRetry: false,
    finishing: false,
    allowPointerUnlock: false,
    heartbeat: 0,
    currentMode: "reaction",
    movementEvent: "onpointerrawupdate" in window ? "pointerrawupdate" : "pointermove",
    supportsCoalesced: typeof PointerEvent !== "undefined" && "getCoalescedEvents" in PointerEvent.prototype,
    width: 0,
    height: 0,
    dpr: 1,
    crosshair: { x: 0, y: 0 },
    target: null,
    targetShownAt: 0,
    currentPoints: [],
    currentPath: [],
    previousPath: [],
    lastEventTime: 0,
    lastMotionAt: performance.now(),
    pointerSpeed: 0,
    dwellStartedAt: 0,
    targetInside: false,
    schedule: [],
    scheduleIndex: 0,
    waitingForStable: false,
    hiddenUntil: 0,
    unavailable: false,
    renderHandle: 0,
    canvasVisible: false,
    resizeReady: false,
  };

  async function api(path, options = {}) {
    const client = window.ttbox && window.ttbox.api;
    if (!client || typeof client.request !== "function") throw new Error("TTBOX API Client 未加载");
    let body = options.body;
    if (typeof body === "string") {
      try { body = JSON.parse(body); } catch { body = undefined; }
    }
    const result = await client.request((options.method || "GET").toUpperCase(), path, body, { ...options, body: undefined });
    if (!result.ok) {
      const error = new Error(result.error || "请求失败");
      error.status = result.status;
      throw error;
    }
    return result.data || {};
  }

  function selectedProfile() {
    return state.profiles[0] || null;
  }

  function selectedMode() {
    return state.currentMode;
  }

  function selectedCompletion() {
    return "dwell";
  }

  function currentPresetName() {
    return document.documentElement.dataset.selectedPresetName || "";
  }

  function activationPayload() {
    const payload = {
      curve_blend: Number(ui.curve.value) / 100,
      speed_blend: Number(ui.speed.value) / 100,
      reaction_blend: Number(ui.reaction.value) / 100,
      max_reaction_delay_ms: Number(ui.delay.value),
    };
    const presetName = currentPresetName();
    if (presetName) payload.preset_name = presetName;
    return payload;
  }

  function deactivationOptions() {
    const presetName = currentPresetName();
    return presetName
      ? { method: "DELETE", body: JSON.stringify({ preset_name: presetName }) }
      : { method: "DELETE" };
  }

  function setMessage(message, error = false) {
    ui.message.textContent = message;
    ui.message.dataset.state = error ? "error" : "";
  }

  function setLease(active, text) {
    ui.lease.textContent = text;
    ui.lease.classList.toggle("live", active);
    ui.lease.classList.toggle("idle", !active);
  }

  function updateMixLabels() {
    ui.curveValue.value = `${ui.curve.value}%`;
    ui.speedValue.value = `${ui.speed.value}%`;
    ui.reactionValue.value = `${ui.reaction.value}%`;
    ui.delayValue.value = `${ui.delay.value}ms`;
  }

  function updateProfileUi() {
    const profile = selectedProfile();
    const reactionCount = Number(profile?.reaction_count || 0);
    const continuousCount = Number(profile?.continuous_count || 0);
    const total = Number(profile?.sample_count || 0);
    const completedReaction = Math.min(goals.reaction, Math.max(0, reactionCount));
    const completedContinuous = Math.min(goals.continuous, Math.max(0, continuousCount));
    const completedTotal = completedReaction + completedContinuous;
    const model = profile?.model || {};
    const coverage = model.coverage || {};
    const statistics = profile?.statistics || {};
    const quality = Number(model.quality || 0);
    const ready = Boolean(model.ready);
    const isActive = state.enabled && state.activeProfileId === profile?.id;

    ui.progress.textContent = `${completedTotal} / 168`;
    ui.reactionCount.textContent = `${completedReaction} / ${goals.reaction}`;
    ui.continuousCount.textContent = `${completedContinuous} / ${goals.continuous}`;
    ui.reactionMean.textContent = typeof statistics.reaction_mean_ms === "number" &&
      Number.isFinite(statistics.reaction_mean_ms)
      ? `${Math.round(statistics.reaction_mean_ms)} ms`
      : "--";
    ui.efficiency.textContent = typeof statistics.path_efficiency === "number" &&
      Number.isFinite(statistics.path_efficiency)
      ? `${Math.round(statistics.path_efficiency * 100)}%`
      : "--";
    ui.qualityBadge.textContent = model.exists ? `${quality} 分` : "未训练";
    ui.qualityBadge.classList.toggle("live", ready && quality >= 60);
    ui.qualityBadge.classList.toggle("idle", !ready || quality < 60);
    ui.qualityBar.style.width = `${quality}%`;
    ui.coverage.textContent = model.exists
      ? `反应 ${coverage.reaction || 0} · 连续 ${coverage.continuous || 0}`
      : "样本不足";
    ui.modelState.textContent = isActive ? "当前预设已启用" : "当前预设未启用";
    ui.activate.textContent = isActive ? "停用" : "启用";
    ui.activate.classList.toggle("ghost-button", !isActive);
    ui.activate.classList.toggle("danger-button", isActive);
    ui.activate.disabled = state.collecting || (!isActive && (!profile || !ready || quality < 60));
    ui.train.disabled = !profile || total === 0 || state.collecting;
    ui.clear.disabled = !profile || total === 0 || state.collecting;
    ui.start.disabled = state.unavailable || !profile || state.collecting || state.uploadFailed;
  }

  async function refreshProfiles() {
    const data = await api("/api/motion-profiles");
    state.profiles = Array.isArray(data.profiles) ? data.profiles : [];
    state.activeProfileId = data.active_profile_id || "";
    state.enabled = Boolean(data.enabled);
    const mix = data.mix || {};
    ui.curve.value = String(Math.round(100 * Number(mix.curve ?? 1)));
    ui.speed.value = String(Math.round(100 * Number(mix.speed ?? 1)));
    ui.reaction.value = String(Math.round(100 * Number(mix.reaction ?? 0.7)));
    ui.delay.value = String(Number(mix.max_reaction_delay_ms ?? 250));
    updateMixLabels();
    updateProfileUi();
  }

  function makeSchedule(mode) {
    const cycles = mode === "reaction" ? 3 : 4;
    const directions = [0, 4, 1, 5, 2, 6, 3, 7];
    const schedule = [];
    for (let cycle = 0; cycle < cycles; cycle += 1) {
      for (let index = 0; index < directions.length * 3; index += 1) {
        schedule.push({
          direction: directions[(index + cycle * 2) % directions.length],
          distance: (index + cycle) % 3,
        });
      }
    }
    return schedule;
  }

  function completedModeSamples(profile, mode) {
    const key = mode === "reaction" ? "reaction_count" : "continuous_count";
    const goal = goals[mode];
    const value = Math.floor(Number(profile?.[key] || 0));
    return Math.min(goal, Math.max(0, Number.isFinite(value) ? value : 0));
  }

  function beginContinuousMode() {
    const profile = selectedProfile();
    state.currentMode = "continuous";
    state.schedule = makeSchedule("continuous");
    state.scheduleIndex = completedModeSamples(profile, "continuous");
    state.waitingForStable = false;
    state.hiddenUntil = 0;
    setMessage("反应训练完成，开始连续切换");
    nextTarget();
  }

  function nextTarget(notice = "") {
    if (!state.collecting || state.uploadPending || state.uploadFailed) return;
    const mode = selectedMode();
    if (state.scheduleIndex >= state.schedule.length) {
      if (mode === "reaction") beginContinuousMode();
      else void finishTraining();
      return;
    }
    const radius = 22;
    const shortSide = Math.min(state.width, state.height);
    const ratios = [0.22, 0.36, 0.52];
    const margin = radius + 1;
    let selectedIndex = -1;
    let proposedX = 0;
    let proposedY = 0;
    for (let index = state.scheduleIndex; index < state.schedule.length && selectedIndex < 0; index += 1) {
      const candidate = state.schedule[index];
      for (let attempt = 0; attempt < 5; attempt += 1) {
        const jitterAngle = (Math.random() - 0.5) * Math.PI / 18;
        const angle = candidate.direction * Math.PI / 4 + jitterAngle;
        const distance = shortSide * ratios[candidate.distance] * (0.975 + Math.random() * 0.05);
        const x = state.crosshair.x + Math.cos(angle) * distance;
        const y = state.crosshair.y + Math.sin(angle) * distance;
        if (x >= margin && x <= state.width - margin && y >= margin && y <= state.height - margin) {
          selectedIndex = index;
          proposedX = x;
          proposedY = y;
          break;
        }
      }
    }
    if (selectedIndex < 0) {
      void endSession("当前准星位置无法生成均衡目标，请重新开始", true);
      return;
    }
    [state.schedule[state.scheduleIndex], state.schedule[selectedIndex]] =
      [state.schedule[selectedIndex], state.schedule[state.scheduleIndex]];
    const item = state.schedule[state.scheduleIndex++];
    state.target = {
      x: proposedX,
      y: proposedY,
      radius,
      direction: item.direction,
      distance: item.distance,
    };
    state.targetShownAt = performance.now();
    state.lastEventTime = state.targetShownAt;
    state.currentPoints = [];
    state.currentPath = [{ x: state.crosshair.x, y: state.crosshair.y }];
    state.dwellStartedAt = 0;
    state.targetInside = false;
    const progress = `${mode === "reaction" ? "反应训练" : "连续切换"} ${state.scheduleIndex} / ${state.schedule.length}`;
    setMessage(notice ? `${notice}；${progress}` : progress, Boolean(notice));
  }

  function prepareNextTarget(notice = "") {
    if (selectedMode() === "reaction" && state.scheduleIndex >= state.schedule.length) {
      beginContinuousMode();
      return;
    }
    if (selectedMode() === "continuous") {
      nextTarget(notice);
      return;
    }
    state.target = null;
    state.waitingForStable = true;
    state.hiddenUntil = 0;
    if (notice) setMessage(notice, true);
  }

  function samplePayload() {
    return {
      schema: "ttbox.motion-sample.v1",
      mode: selectedMode(),
      completion: selectedCompletion(),
      canvas: { width: state.width, height: state.height },
      start: state.currentPath[0],
      target: { x: state.target.x, y: state.target.y },
      radius: state.target.radius,
      browser: {
        pointer_lock: true,
        raw_update: state.movementEvent === "pointerrawupdate",
        coalesced_events: state.supportsCoalesced,
        user_agent: navigator.userAgent.slice(0, 256),
      },
      points: state.currentPoints,
    };
  }

  async function uploadSample(sample) {
    const sessionId = state.sessionId;
    state.uploadPending = true;
    state.pendingSample = sample;
    setMessage("正在保存样本");
    try {
      const data = await api(`/api/motion-training/sessions/${encodeURIComponent(sessionId)}/samples`, {
        method: "POST",
        body: JSON.stringify(sample),
      });
      if (!state.collecting || state.sessionId !== sessionId) return;
      state.uploadPending = false;
      state.uploadFailed = false;
      state.pausedForRetry = false;
      state.pendingSample = null;
      ui.retry.hidden = true;
      const profile = selectedProfile();
      if (profile) {
        profile.sample_count = Number(data.sample_count || profile.sample_count || 0);
        const key = sample.mode === "reaction" ? "reaction_count" : "continuous_count";
        profile[key] = Number(profile[key] || 0) + 1;
        if (data.profile_statistics && typeof data.profile_statistics === "object") {
          profile.statistics = data.profile_statistics;
        }
      }
      updateProfileUi();
      prepareNextTarget();
    } catch (error) {
      if (!state.collecting || state.sessionId !== sessionId) return;
      state.uploadPending = false;
      if (error.status === 400 || error.status === 413 || error.status === 422) {
        state.uploadFailed = false;
        state.pausedForRetry = false;
        state.pendingSample = null;
        state.scheduleIndex = Math.max(0, state.scheduleIndex - 1);
        ui.retry.hidden = true;
        setLease(true, "采集中");
        const detail = error.message.includes("path is implausible")
          ? "轨迹转折或路径过长"
          : "轨迹数据未通过校验";
        updateProfileUi();
        prepareNextTarget(`${detail}，已自动重画当前目标`);
        return;
      }
      if (error.status === 409) {
        await endSession("采集租约已结束，再次开始会从当前进度继续", true);
        return;
      }
      state.uploadFailed = true;
      state.pausedForRetry = true;
      ui.retry.hidden = false;
      setMessage(`样本上传失败：${error.message}`, true);
      setLease(true, "等待重试");
      if (document.pointerLockElement === canvas) {
        state.allowPointerUnlock = true;
        document.exitPointerLock();
      }
      updateProfileUi();
    }
  }

  function completeSample() {
    if (!state.target || state.uploadPending || state.uploadFailed) return;
    if (state.currentPoints.length < 2) {
      setMessage("轨迹点过少，请继续移动后完成目标", true);
      return;
    }
    const sample = samplePayload();
    state.previousPath = state.currentPath.slice();
    state.target = null;
    state.targetInside = false;
    void uploadSample(sample);
  }

  function processMovement(event) {
    const now = performance.now();
    const rawDx = Number(event.movementX || 0);
    const rawDy = Number(event.movementY || 0);
    if (!Number.isFinite(rawDx) || !Number.isFinite(rawDy)) return;
    const dt = Math.max(0, Math.min(2000, now - (state.lastEventTime || now)));
    state.lastEventTime = now;
    const rawDistance = Math.hypot(rawDx, rawDy);
    state.pointerSpeed = rawDistance / Math.max(dt, 1);
    if (rawDistance > 0.05) state.lastMotionAt = performance.now();
    const previousX = state.crosshair.x;
    const previousY = state.crosshair.y;
    state.crosshair.x = Math.max(0, Math.min(state.width, previousX + rawDx));
    state.crosshair.y = Math.max(0, Math.min(state.height, previousY + rawDy));
    const dx = state.crosshair.x - previousX;
    const dy = state.crosshair.y - previousY;

    if (!state.target || state.uploadPending || state.uploadFailed) return;
    let remainingDt = dt;
    if (state.currentPoints.length > 0) {
      while (remainingDt > 100) {
        if (state.currentPoints.length >= 2048) {
          void endSession("单条轨迹超过 2048 点，采集已停止", true);
          return;
        }
        state.currentPoints.push({ dt: 100, dx: 0, dy: 0 });
        state.currentPath.push({ x: previousX, y: previousY });
        remainingDt -= 100;
      }
    }
    if (state.currentPoints.length >= 2048) {
      void endSession("单条轨迹超过 2048 点，采集已停止", true);
      return;
    }
    state.currentPoints.push({ dt: remainingDt, dx, dy });
    state.currentPath.push({ x: state.crosshair.x, y: state.crosshair.y });
    const inside = Math.hypot(
      state.crosshair.x - state.target.x,
      state.crosshair.y - state.target.y,
    ) <= state.target.radius;
    state.targetInside = inside;
    if (selectedCompletion() === "dwell") {
      if (inside && state.pointerSpeed <= 0.8) {
        if (!state.dwellStartedAt) state.dwellStartedAt = performance.now();
      } else {
        state.dwellStartedAt = 0;
      }
    }
  }

  function onMovement(event) {
    if (!state.collecting || document.pointerLockElement !== canvas) return;
    const events = typeof event.getCoalescedEvents === "function" ? event.getCoalescedEvents() : [];
    const source = events.length ? events : [event];
    for (const item of source) processMovement(item);
  }

  function configureCanvas(resetCrosshair = false) {
    const rect = ui.shell.getBoundingClientRect();
    const width = Math.max(1, Math.round(rect.width));
    const height = Math.max(1, Math.round(rect.height));
    state.dpr = Math.max(1, Math.min(window.devicePixelRatio || 1, 3));
    canvas.width = Math.round(width * state.dpr);
    canvas.height = Math.round(height * state.dpr);
    state.width = width;
    state.height = height;
    if (resetCrosshair || !state.crosshair.x || !state.crosshair.y) {
      state.crosshair = { x: width / 2, y: height / 2 };
    } else {
      state.crosshair.x = Math.min(width, state.crosshair.x);
      state.crosshair.y = Math.min(height, state.crosshair.y);
    }
  }

  function drawPath(path, color, width) {
    if (path.length < 2) return;
    ctx.beginPath();
    ctx.moveTo(path[0].x, path[0].y);
    for (let index = 1; index < path.length; index += 1) ctx.lineTo(path[index].x, path[index].y);
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke();
  }

  function render() {
    state.renderHandle = 0;
    const now = performance.now();
    if (state.collecting && state.waitingForStable) {
      if (state.hiddenUntil && now - state.lastMotionAt < 120) {
        state.hiddenUntil = 0;
      } else if (!state.hiddenUntil && now - state.lastMotionAt >= 120) {
        state.hiddenUntil = now + 300 + Math.random() * 500;
      } else if (state.hiddenUntil && now >= state.hiddenUntil) {
        state.waitingForStable = false;
        state.hiddenUntil = 0;
        nextTarget();
      }
    }
    if (state.collecting && state.target && selectedCompletion() === "dwell" &&
        state.targetInside && !state.uploadPending && !state.uploadFailed) {
      const stableSince = Math.max(state.dwellStartedAt || 0, state.lastMotionAt || 0);
      if (stableSince > 0 && now - stableSince >= 60) completeSample();
    }

    ctx.setTransform(state.dpr, 0, 0, state.dpr, 0, 0);
    ctx.fillStyle = "#0a1014";
    ctx.fillRect(0, 0, state.width, state.height);
    ctx.strokeStyle = "rgba(150, 170, 180, .08)";
    ctx.lineWidth = 1;
    const grid = 40;
    for (let x = grid; x < state.width; x += grid) {
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, state.height); ctx.stroke();
    }
    for (let y = grid; y < state.height; y += grid) {
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(state.width, y); ctx.stroke();
    }
    drawPath(state.previousPath, "rgba(138, 155, 166, .38)", 1.5);
    drawPath(state.currentPath, "rgba(75, 220, 164, .82)", 2);

    if (state.target) {
      ctx.beginPath();
      ctx.arc(state.target.x, state.target.y, state.target.radius, 0, Math.PI * 2);
      ctx.fillStyle = "rgba(53, 211, 147, .22)";
      ctx.fill();
      ctx.strokeStyle = "#3fd39b";
      ctx.lineWidth = 2;
      ctx.stroke();
      ctx.beginPath();
      ctx.arc(state.target.x, state.target.y, 3, 0, Math.PI * 2);
      ctx.fillStyle = "#d9fff1";
      ctx.fill();
    }

    const cross = state.crosshair;
    ctx.strokeStyle = "#f4f7f9";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(cross.x - 10, cross.y); ctx.lineTo(cross.x - 3, cross.y);
    ctx.moveTo(cross.x + 3, cross.y); ctx.lineTo(cross.x + 10, cross.y);
    ctx.moveTo(cross.x, cross.y - 10); ctx.lineTo(cross.x, cross.y - 3);
    ctx.moveTo(cross.x, cross.y + 3); ctx.lineTo(cross.x, cross.y + 10);
    ctx.stroke();
    requestRender();
  }

  function requestRender() {
    if (!state.renderHandle && (state.canvasVisible || state.collecting)) {
      state.renderHandle = requestAnimationFrame(render);
    }
  }

  function setCollectingUi(active) {
    ui.start.disabled = active || !selectedProfile();
    ui.stop.disabled = !active;
    setLease(active, active ? "采集中" : "未采集");
    updateProfileUi();
  }

  async function requestLock() {
    try {
      await canvas.requestPointerLock({ unadjustedMovement: true });
    } catch (_error) {
      await canvas.requestPointerLock();
    }
  }

  async function startSession() {
    const profile = selectedProfile();
    if (!profile || state.collecting) return;
    const reactionCompleted = completedModeSamples(profile, "reaction");
    const continuousCompleted = completedModeSamples(profile, "continuous");
    if (reactionCompleted >= goals.reaction && continuousCompleted >= goals.continuous) {
      setMessage("有效样本已采集完成，正在生成个人模型");
      await finishTraining();
      return;
    }
    const resumeMode = reactionCompleted < goals.reaction ? "reaction" : "continuous";
    const resumeIndex = resumeMode === "reaction" ? reactionCompleted : continuousCompleted;
    setMessage("正在获取训练租约");
    try {
      const data = await api("/api/motion-training/sessions", {
        method: "POST",
        body: JSON.stringify({ profile_id: profile.id }),
      });
      state.sessionId = data.session_id;
      state.collecting = true;
      state.uploadPending = false;
      state.uploadFailed = false;
      state.pendingSample = null;
      state.pausedForRetry = false;
      state.allowPointerUnlock = false;
      state.finishing = false;
      state.currentMode = resumeMode;
      state.schedule = makeSchedule(resumeMode);
      state.scheduleIndex = resumeIndex;
      state.previousPath = [];
      state.currentPath = [];
      state.target = null;
      state.waitingForStable = resumeMode === "reaction";
      state.hiddenUntil = 0;
      state.lastMotionAt = performance.now();
      configureCanvas(true);
      setCollectingUi(true);
      requestRender();
      document.addEventListener(state.movementEvent, onMovement, { passive: true });
      state.heartbeat = window.setInterval(() => {
        void api(`/api/motion-training/sessions/${encodeURIComponent(state.sessionId)}/heartbeat`, { method: "PUT" })
          .catch((error) => endSession(`心跳失败：${error.message}`, true));
      }, 2000);
      await requestLock();
      if (resumeMode === "reaction") {
        setMessage(`继续反应训练 ${resumeIndex} / ${goals.reaction}，保持准星稳定`);
      } else {
        nextTarget();
      }
    } catch (error) {
      setMessage(error.message, true);
      await endSession("无法开始采集", false);
    }
  }

  async function endSession(reason = "采集已结束", releaseLock = true) {
    const sessionId = state.sessionId;
    const wasCollecting = state.collecting;
    state.collecting = false;
    state.sessionId = "";
    state.uploadPending = false;
    state.uploadFailed = false;
    state.pendingSample = null;
    state.pausedForRetry = false;
    state.allowPointerUnlock = false;
    state.target = null;
    state.targetInside = false;
    state.waitingForStable = false;
    state.hiddenUntil = 0;
    if (state.heartbeat) window.clearInterval(state.heartbeat);
    state.heartbeat = 0;
    document.removeEventListener(state.movementEvent, onMovement);
    ui.retry.hidden = true;
    if (releaseLock && document.pointerLockElement === canvas) document.exitPointerLock();
    setCollectingUi(false);
    setMessage(reason, reason.includes("失败") || reason.includes("超过"));
    if (wasCollecting && sessionId) {
      try {
        await api(`/api/motion-training/sessions/${encodeURIComponent(sessionId)}`, { method: "DELETE" });
      } catch (_error) {
        // The daemon lease expires automatically after eight seconds.
      }
    }
  }

  async function retryUpload() {
    if (!state.pendingSample || !state.collecting) return;
    const sample = state.pendingSample;
    try {
      await requestLock();
    } catch (error) {
      setMessage(`无法重新进入训练画布：${error.message}`, true);
      return;
    }
    if (!state.collecting) return;
    state.pausedForRetry = false;
    state.allowPointerUnlock = false;
    state.uploadFailed = false;
    ui.retry.hidden = true;
    setLease(true, "采集中");
    await uploadSample(sample);
  }

  async function finishTraining() {
    if (state.finishing) return;
    const profile = selectedProfile();
    if (!profile) return;
    state.finishing = true;
    await endSession("采集完成，正在生成个人模型");
    ui.start.disabled = true;
    ui.train.disabled = true;
    ui.activate.disabled = true;
    try {
      await api(`/api/motion-profiles/${encodeURIComponent(profile.id)}/train`, { method: "POST" });
      await api(`/api/motion-profiles/${encodeURIComponent(profile.id)}/activate`, {
        method: "POST",
        body: JSON.stringify(activationPayload()),
      });
      await refreshProfiles();
      setMessage("训练完成，个人模型已自动启用");
    } catch (error) {
      await refreshProfiles().catch(() => {});
      setMessage(`自动生成或启用失败：${error.message}`, true);
    } finally {
      state.finishing = false;
      updateProfileUi();
    }
  }

  async function saveMix() {
    if (!state.enabled) return;
    try {
      await api("/api/config", {
        method: "PUT",
        body: JSON.stringify({
          ai: { controller: {
            personal_motion_curve_blend: Number(ui.curve.value) / 100,
            personal_motion_speed_blend: Number(ui.speed.value) / 100,
            personal_motion_reaction_blend: Number(ui.reaction.value) / 100,
            personal_motion_max_reaction_delay_ms: Number(ui.delay.value),
          } },
        }),
      });
    } catch (error) {
      setMessage(`保存混合参数失败：${error.message}`, true);
    }
  }

  ui.start.addEventListener("click", () => void startSession());
  ui.stop.addEventListener("click", () => void endSession());
  ui.retry.addEventListener("click", () => void retryUpload());
  ui.train.addEventListener("click", async () => {
    const profile = selectedProfile();
    if (!profile) return;
    setMessage("正在生成个人模型");
    try {
      await api(`/api/motion-profiles/${encodeURIComponent(profile.id)}/train`, { method: "POST" });
      await refreshProfiles();
      setMessage("模型已生成，需要手动启用");
    } catch (error) { setMessage(error.message, true); }
  });
  ui.activate.addEventListener("click", async () => {
    const profile = selectedProfile();
    if (!profile) return;
    try {
      const isActive = state.enabled && state.activeProfileId === profile.id;
      if (isActive) {
        await api("/api/motion-profiles/active", deactivationOptions());
      } else {
        await api(`/api/motion-profiles/${encodeURIComponent(profile.id)}/activate`, {
          method: "POST",
          body: JSON.stringify(activationPayload()),
        });
      }
      await refreshProfiles();
      setMessage(isActive ? "个人模型已停用" : "个人模型已启用");
    } catch (error) { setMessage(error.message, true); }
  });
  ui.clear.addEventListener("click", async () => {
    const profile = selectedProfile();
    if (!profile || !window.confirm("清空全部训练样本和个人模型？")) return;
    try {
      await api(`/api/motion-profiles/${encodeURIComponent(profile.id)}/samples`, { method: "DELETE" });
      await refreshProfiles();
      setMessage("样本和模型已清空");
    } catch (error) { setMessage(error.message, true); }
  });

  for (const input of [ui.curve, ui.speed, ui.reaction, ui.delay]) {
    input.addEventListener("input", updateMixLabels);
    input.addEventListener("change", () => void saveMix());
  }

  document.addEventListener("pointerlockchange", () => {
    if (state.collecting && document.pointerLockElement !== canvas) {
      if (state.allowPointerUnlock && state.pausedForRetry) {
        state.allowPointerUnlock = false;
      } else {
        void endSession("Pointer Lock 已退出，采集结束", false);
      }
    }
  });
  document.addEventListener("visibilitychange", () => {
    if (state.collecting && document.hidden) void endSession("页面不可见，采集结束");
  });
  window.addEventListener("blur", () => {
    if (state.collecting) void endSession("窗口失去焦点，采集结束");
  });
  document.addEventListener("click", (event) => {
    const tab = event.target.closest("[data-control-section-target]");
    if (state.collecting && tab && tab.dataset.controlSectionTarget !== "control-section-motion-training") {
      void endSession("已切换页面，采集结束");
    }
    const opensTraining = tab?.dataset.controlSectionTarget === "control-section-motion-training";
    const pageTab = event.target.closest("[data-page-target]");
    const returnsToTraining = pageTab?.dataset.pageTarget === "control-page" && !ui.section.hidden;
    if (!state.collecting && (opensTraining || returnsToTraining)) {
      window.setTimeout(() => {
        refreshProfiles().catch((error) => setMessage(error.message, true));
      }, 0);
    }
  }, true);
  window.addEventListener("beforeunload", () => {
    if (state.collecting && state.sessionId) {
      fetch(`/api/motion-training/sessions/${encodeURIComponent(state.sessionId)}`, {
        method: "DELETE", keepalive: true,
      }).catch(() => {});
    }
  });

  const resizeObserver = new ResizeObserver(() => {
    const oldWidth = state.width;
    const oldHeight = state.height;
    configureCanvas(false);
    if (state.resizeReady && state.collecting &&
        (Math.abs(oldWidth - state.width) > 1 || Math.abs(oldHeight - state.height) > 1)) {
      void endSession("训练画布尺寸变化，采集结束");
    }
    state.resizeReady = true;
    requestRender();
  });
  resizeObserver.observe(ui.shell);

  const visibilityObserver = new IntersectionObserver((entries) => {
    state.canvasVisible = entries.some((entry) => entry.isIntersecting);
    if (!state.canvasVisible && !state.collecting && state.renderHandle) {
      cancelAnimationFrame(state.renderHandle);
      state.renderHandle = 0;
    }
    requestRender();
  });
  visibilityObserver.observe(ui.shell);

  const noFinePointer = window.matchMedia("(pointer: coarse)").matches &&
    !window.matchMedia("(any-pointer: fine)").matches;
  state.unavailable = noFinePointer || typeof canvas.requestPointerLock !== "function";
  ui.unavailable.hidden = !state.unavailable;
  ui.start.disabled = state.unavailable;
  configureCanvas(true);
  updateMixLabels();
  refreshProfiles().catch((error) => setMessage(error.message, true));
})();
