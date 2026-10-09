(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const section = $("control-section-motion-training");
  if (!section || $("motionTrainingCanvas")) return;

  const ui = {
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
  const goals = { reaction: 72, continuous: 96 };
  const state = {
    profiles: [],
    activeProfileId: "",
    enabled: false,
    busy: false,
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

  function setMessage(message, error = false) {
    ui.message.textContent = message;
    ui.message.dataset.state = error ? "error" : "";
  }

  function updateMixLabels() {
    ui.curveValue.value = `${ui.curve.value}%`;
    ui.speedValue.value = `${ui.speed.value}%`;
    ui.reactionValue.value = `${ui.reaction.value}%`;
    ui.delayValue.value = `${ui.delay.value}ms`;
  }

  function mixPayload() {
    const payload = {
      curve_blend: Number(ui.curve.value) / 100,
      speed_blend: Number(ui.speed.value) / 100,
      reaction_blend: Number(ui.reaction.value) / 100,
      max_reaction_delay_ms: Number(ui.delay.value),
    };
    const presetName = document.documentElement.dataset.selectedPresetName || "";
    if (presetName) payload.preset_name = presetName;
    return payload;
  }

  function deactivationOptions() {
    const presetName = document.documentElement.dataset.selectedPresetName || "";
    return presetName
      ? { method: "DELETE", body: JSON.stringify({ preset_name: presetName }) }
      : { method: "DELETE" };
  }

  function updateUi() {
    const profile = selectedProfile();
    const reactionCount = Number(profile?.reaction_count || 0);
    const continuousCount = Number(profile?.continuous_count || 0);
    const reactionCompleted = Math.min(goals.reaction, Math.max(0, reactionCount));
    const continuousCompleted = Math.min(goals.continuous, Math.max(0, continuousCount));
    const total = Number(profile?.sample_count || 0);
    const model = profile?.model || {};
    const coverage = model.coverage || {};
    const statistics = profile?.statistics || {};
    const quality = Math.min(100, Math.max(0, Number(model.quality || 0)));
    const ready = Boolean(model.ready);
    const isActive = state.enabled && state.activeProfileId === profile?.id;

    ui.progress.textContent = `${reactionCompleted + continuousCompleted} / 168`;
    ui.reactionCount.textContent = `${reactionCompleted} / ${goals.reaction}`;
    ui.continuousCount.textContent = `${continuousCompleted} / ${goals.continuous}`;
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
    ui.activate.disabled = state.busy || (!isActive && (!profile || !ready || quality < 60));
    ui.train.disabled = state.busy || !profile || total === 0;
    ui.clear.disabled = state.busy || !profile || total === 0;
  }

  async function refresh() {
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
    updateUi();
  }

  async function runAction(message, action, success) {
    if (state.busy) return;
    state.busy = true;
    updateUi();
    setMessage(message);
    try {
      await action();
      await refresh();
      setMessage(success);
    } catch (error) {
      setMessage(error.message, true);
    } finally {
      state.busy = false;
      updateUi();
    }
  }

  ui.train.addEventListener("click", () => {
    const profile = selectedProfile();
    if (!profile) return;
    void runAction(
      "正在生成个人模型",
      () => api(`/api/motion-profiles/${encodeURIComponent(profile.id)}/train`, { method: "POST" }),
      "个人模型已生成",
    );
  });

  ui.activate.addEventListener("click", () => {
    const profile = selectedProfile();
    if (!profile) return;
    const isActive = state.enabled && state.activeProfileId === profile.id;
    void runAction(
      isActive ? "正在停用个人模型" : "正在启用个人模型",
      () => isActive
        ? api("/api/motion-profiles/active", deactivationOptions())
        : api(`/api/motion-profiles/${encodeURIComponent(profile.id)}/activate`, {
          method: "POST",
          body: JSON.stringify(mixPayload()),
        }),
      isActive ? "个人模型已停用" : "个人模型已启用",
    );
  });

  ui.clear.addEventListener("click", () => {
    const profile = selectedProfile();
    if (!profile || !window.confirm("清空全部训练样本和个人模型？")) return;
    void runAction(
      "正在清空训练数据",
      () => api(`/api/motion-profiles/${encodeURIComponent(profile.id)}/samples`, { method: "DELETE" }),
      "样本和模型已清空",
    );
  });

  for (const input of [ui.curve, ui.speed, ui.reaction, ui.delay]) {
    input.addEventListener("input", updateMixLabels);
    input.addEventListener("change", async () => {
      if (!state.enabled) return;
      try {
        const mix = mixPayload();
        await api("/api/config", {
          method: "PUT",
          body: JSON.stringify({
            ai: { controller: {
              personal_motion_curve_blend: mix.curve_blend,
              personal_motion_speed_blend: mix.speed_blend,
              personal_motion_reaction_blend: mix.reaction_blend,
              personal_motion_max_reaction_delay_ms: mix.max_reaction_delay_ms,
            } },
          }),
        });
      } catch (error) {
        setMessage(`保存混合参数失败：${error.message}`, true);
      }
    });
  }

  document.addEventListener("click", (event) => {
    const tab = event.target.closest("[data-control-section-target]");
    const opensTraining = tab?.dataset.controlSectionTarget === "control-section-motion-training";
    const pageTab = event.target.closest("[data-page-target]");
    const returnsToTraining = pageTab?.dataset.pageTarget === "control-page" && !section.hidden;
    if (opensTraining || returnsToTraining) {
      window.setTimeout(() => refresh().catch((error) => setMessage(error.message, true)), 0);
    }
  }, true);

  updateMixLabels();
  updateUi();
  refresh().catch((error) => setMessage(error.message, true));
})();
