// AimThread.cpp — 独立瞄准线程最小可验证实现。
#include "aim/AimThread.hpp"
#include "aim/PipelineDebug.hpp"
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>
#include "aim/AimError.hpp"
#include "mouse/FovAngle.hpp"
#include "mouse/CoordinateTransform.hpp"
#include "mouse/AimPointProfile.hpp"
#include "mouse/PersonalMotion.hpp"
#include "mouse/PersonalTrajectoryShader.hpp"
#include "common/CpuAffinity.hpp"
#include "common/Logger.hpp"
#include "output/OutputBackend.hpp"   // kActDown/kActUp（自动扳机注入动作）
namespace ttbox::core::aim {
bool AimThread::start(AimTargetMailbox* mailbox, std::shared_ptr<output::IHidOutput> output, int interval_us, RuntimeConfig* runtime_config, std::atomic<uint16_t>* physical_buttons) {
    if (!mailbox || !output || running_.exchange(true)) return false;
    mailbox_ = mailbox; output_ = std::move(output); interval_us_ = interval_us > 0 ? interval_us : 4000; runtime_config_ = runtime_config; physical_buttons_ = physical_buttons;
    reset_runtime_state();
    { std::lock_guard<std::mutex> lk(status_mutex_); status_ = {}; status_.running = true; }
    // BB927 实战参数：X predict=0.5（pid1.cpp 的 3.0 是演示值），Y predict=0.0。
    pid_x_.init(25.0, 25.0, 0.5, 0.3, 9900.0);
    pid_y_.init(25.0, 25.0, 0.0, 0.3, 9900.0);
    thread_ = std::thread(&AimThread::loop, this);
    return true;
}

void AimThread::reset_runtime_state() {
    selector_.reset();
    state_machine_.reset();
    tracker_.reset();
    pid_x_.reset();
    pid_y_.reset();
    pull_curve_.reset();
    continuous_lead_.reset();  // 持续提前量累计/方向/渐入电平清零（destroy→init 重建一致）
    personal_shader_.reset();
    recoil_.reset();
    clip_ratio_tracker_.reset();  // V1.0.10：身高反推比随世代清零（禁止跨世代继承旧目标比例）
    frozen_rect_.reset();        // V1.0.10：冻结框随世代清零（换模型后旧框位置已不可信）
    jitter_ff_.reset();      // V3 阶段 5：跨世代的抖动欠账不能留给新模型去扣
    // 自动扳机：换世代必须清状态，且**先把按住的键抬起**（否则换模型时鼠标键卡在按下态）。
    trigger_.reset();
    trigger_release_btn_ = 0;
    trigger_release_at_ms_ = 0;
    trigger_throttle_frames_ = 0;
    last_injection_allowed_ = false;
    display_smooth_x1_.reset();
    display_smooth_y1_.reset();
    display_smooth_x2_.reset();
    display_smooth_y2_.reset();
    last_display_target_id_ = -1;
    last_display_ts_us_ = 0;
    target_age_ms_ = 0.0f;
    last_timestamp_us_ = 0;
    remainder_x_ = 0.0f;
    remainder_y_ = 0.0f;
    last_target_id_ = -1;
    // 热键保护：挂起状态**跨世代保留** —— 它是用户按出来的意图，换个模型不该被悄悄解除。
    // 但边沿判据要重新对齐到当前物理位图：否则 start 那一刻会把"键仍按着"误读成一次
    // 新的上升沿，刚挂起就被自己解掉（start 后第一周期就会翻转一次）。
    last_raw_buttons_ = physical_buttons_ ? physical_buttons_->load(std::memory_order_acquire) : 0;
}
void AimThread::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(status_mutex_); status_.running = false;
}
AimThread::Status AimThread::status() const {
    std::lock_guard<std::mutex> lk(status_mutex_);
    return status_;
}
void AimThread::loop() {
    // 瞄准控制链对延迟敏感，固定在大核运行，避免被调度到小核产生抖动。
    {
        std::string aerr;
        if (!CpuAffinity::set_thread_affinity(CpuAffinity::kBigCoreMask, &aerr)) {
            TTBOX_LOG_WARN("AimThread 绑定大核失败: " + aerr);
        }
    }
    uint64_t last_frame = 0;
    while (running_.load(std::memory_order_acquire)) {
        AimTargetTask task;
        if (mailbox_->take_latest(&task, last_frame)) {
            last_frame = task.frame_number;
            const uint64_t previous_timestamp_us = last_timestamp_us_;
            const int target_id_before = last_target_id_;  // 本帧切换判定基准
            // 新控制链：目标选择 → 误差 → 纯 PID/P 控制 → OutputAction。
            TargetSelectorConfig scfg;
            scfg.roi_w = task.frame_width; scfg.roi_h = task.frame_height;
            // 用户置信度阈值（RuntimeProfile.mouse.confidence）：0 = 用模型默认（选 0.25 底线）。
            // 修复点：此前写死 0.0f 导致 Web 置信度阈值参数无效（中看不中用）。
            float out_sensitivity = 1.0f, out_scale = 1.0f;
            float out_deadzone = 1.0f;
            float recoil_px_per_count = 0.65f;  // 压枪 px→count 换算（默认 0.65 px/count，标定可覆盖）
            PersonalMotionConfig personal_motion;
            PullCurveConfig pull_curve_cfg;  // 拉枪曲线配置（默认 enabled=true, min_distance=80, strength=0.8）
            ContinuousLeadConfig lead_cfg;  // 持续提前量配置（默认 enabled=false ⇒ 不动输出）
            PersonalTrajectoryConfig personal_traj_cfg;  // 拟人化整形引擎配置（默认 enabled=false，保持现有行为）
            RecoilConfig recoil_cfg;  // 压枪配置（默认 enabled=false，保持现有行为）
            float kp_x = 0.0f, kp_y = 0.0f, kd_x = 0.0f, kd_y = 0.0f;
            // ★ V1.0.43：Fitts 控制器（dat58 思路，治 pid1「追着怪/停不住」）。
            //   与 pid1 并存，controller_type 二选一（默认 fitts）。
            bool use_fitts = false;
            float gain_x_ppc = 0.65f, gain_y_ppc = 0.65f;  // 标定增益（Fitts px→count 换算）
            AimPointProfile aim_point;
            LockConfirmConfig lock_confirm_cfg;  // 目标锁定确认（ENTER/HOLD，第2项）
            std::shared_ptr<const RuntimeProfile> frame_profile;
            if (runtime_config_) {
                frame_profile = runtime_config_->snapshot();
            }
            // ---- 热键解析与选档（2026-09-24）----
            // ★ 必须放在选靶之前：选靶要用**本档**的 FOV 半径 / 瞄准点 / 目标类别，
            //   这几项都是 TargetSelectorConfig 的入参，晚一步算就等于用了上一帧的档。
            // 顺序：物理按键 → 热键保护挂起 → 逐档命中 → 生效档索引。
            const uint16_t raw_buttons =
                physical_buttons_ ? physical_buttons_->load(std::memory_order_acquire) : 0;
            // ---- 热键保护（hotkey_guard）：toggle 键**上升沿**翻转「全部挂起」----
            // 挂起 = 本控制周期把热键位图清零 ⇒ 瞄准 Gate 与压枪一并失效（两者都读这个位图）。
            // 物理鼠标透传不受影响：那条路在 usbproxy 侧，不经过本变量。
            // guard 关掉（或 toggle 键未配）时立即恢复"未挂起"，不保留幽灵挂起状态。
            uint8_t guard_toggle = 0;
            if (frame_profile) {
                const auto& guard = frame_profile->mouse.hotkey_guard;
                if (guard.enabled) guard_toggle = guard.toggle_hotkey;
            }
            if (guard_toggle != 0) {
                const bool now_down = (raw_buttons & guard_toggle) != 0;
                const bool was_down = (last_raw_buttons_ & guard_toggle) != 0;
                if (now_down && !was_down) hotkeys_suspended_ = !hotkeys_suspended_;
            } else {
                hotkeys_suspended_ = false;
            }
            last_raw_buttons_ = raw_buttons;
            const uint16_t hotkey_bits = hotkeys_suspended_ ? 0u : raw_buttons;
            // 鼠标五键统一位图：左1、右2、中4、侧1 8、侧2 16。
            // 热键位全部来自用户配置快照（每周期重读 → 改配置即时生效，无需重启）。
            // ★ 「任意两档键位互斥」（面板保存时校验）⇒ 任何按键组合最多命中一档，
            //   所以命中即返回，不需要优先级，结果也与遍历顺序无关。
            // A11 标定模式：无视物理热键强制放行（标定线程注入运动帧，物理鼠标不参与），
            //   并强制用第 0 档参数 —— 标定要的是一份确定的瞄准点/FOV，不是"哪档被按下"。
            int active_profile = -1;
            bool injection_allowed = false;
            if (frame_profile) {
                active_profile = frame_profile->mouse.calibrating
                                     ? 0
                                     : aim::aim_profile_match(frame_profile->mouse, hotkey_bits);
                injection_allowed = frame_profile->mouse.calibrating ||
                                    (frame_profile->mouse.enabled && active_profile >= 0);
            }
            // ---- BB 热键边沿（对齐「按下 shuwuResetPid、松开完全重置」）----
            // 上升沿：热键刚按下 → 清 PID 在途量，本次瞄准从零起算（BB 的原生行为）。
            // 下降沿：热键松开 → 连提前量/拟人化/抗过冲/速度Kp 一并复位，
            //   下次按下是全新一轮（否则上一轮的收帧窗/积分/衰减帧数会带过来）。
            if (injection_allowed && !last_injection_allowed_) {
                pid_x_.reset();
                pid_y_.reset();
                remainder_x_ = 0.0f;
                remainder_y_ = 0.0f;
            } else if (!injection_allowed && last_injection_allowed_) {
                pid_x_.reset();
                pid_y_.reset();
                remainder_x_ = 0.0f;
                remainder_y_ = 0.0f;
                jitter_ff_.reset();  // V3 阶段 5：热键松开那一帧的抖动欠账一并作废
            }
            last_injection_allowed_ = injection_allowed;
            if (frame_profile) {
                // 本周期生效档；无档命中（未按热键 / 全部挂起）时退回全局量，
                // 此时输出本来就被 Gate 拦着，选靶仍用全局参数 —— 与加档位前逐位一致。
                const aim::AimHotkeyProfile* ap =
                    active_profile >= 0
                        ? &aim::aim_profile_at(frame_profile->mouse,
                                               static_cast<size_t>(active_profile))
                        : nullptr;
                // FOV：**全局基准半径 × 本档倍率**。倍率 1.0 ⇒ 等于总览滑块原值。
                // 拆开是因为总览半径是全局的、倍率是按档的（面板「热键 FOV 缩放」文案
                // 就是"在总览 FOV 半径上再乘这个倍率"）。
                // ★★ V1.0.31 修 FOV 形同虚设：原来多乘了一个 2.0
                //   （把"半径"当"直径"算了）⇒ 板端 fov.radius=0.5 算成 1.0
                //   ⇒ 半径 = 320（= 640 画面的半宽）⇒ 圆等于整个画面
                //   ⇒ **任何框都在圆内，FOV 从来没有约束作用**（业主调 FOV 基本没用）。
                //   现在 fov.radius 就是**相对画面半宽的比例**：0.5 ⇒ 半径 160（真排除远处）。
                //   对照业界（VIper/各类 aimbot）：FOV 半径是用户自定值，通常**小于**半画。
                scfg.fov_range = (frame_profile->fov.enabled ? frame_profile->fov.radius : 1.0f) *
                                 (ap ? ap->fov_scale : 1.0f);
                // 本档目标类别（瞄准侧窄化）。推理侧收的是**全档并集**，见 web_body_to_profile。
                // ★ 此前 scfg.class_filter 从未被赋值 ⇒ 瞄准侧类别过滤一直是关的，
                //   全靠推理侧把非本类别框丢掉。多档位必须把这个字段接上。
                if (ap) scfg.class_filter = ap->class_filter;
                // 瞄准范围 = **截取尺寸内划最大的圆形**（业主口径）：
                // 半径基准取 capture（中心截取尺寸，板端 640×640）⇒ 320px。
                // 之前用整帧 task.frame_width/height（2560×1440）⇒ min/2 = 720px，
                // 比检测区半宽（320px）还大 ⇒ 圆从未真正约束过选靶。
                // 框坐标与 capture 是同一套 1:1 像素（中心裁剪不缩放，见 fov_map_no_roi_full_frame
                // / fov_map_with_roi 用例），故半径可直接用；capture 为 0（全帧）时留 0，
                // TargetSelector 会回退旧口径。
                {
                    const auto& cap = frame_profile->capture;
                    if (cap.width > 0 && cap.height > 0) {
                        scfg.search_radius_px =
                            static_cast<float>(cap.width < cap.height ? cap.width : cap.height) * 0.5f;
                        // V1.0.08：裁剪区下边界（框/准星同一坐标系）= 帧高/2 + 裁剪偏移 + 半宽。
                        // 板端实测（2026-09-29）：帧 2560x1440、capture 416x416 居中
                        // ⇒ 1440/2 + 0 + 208 = **928**，与 cls5 原始框 y2 在 928 的巨峰吻合。
                        // 框底贴到它 ⇒ 框高被截断 ⇒ 落点相对人体上飘。
                        // V1.0.13：cap.offset_y 已删 ⇒ 下边界 = 帧高/2 + 半宽（恒居中）。
                        crop_bottom_px_ =
                            task.frame_height > 0
                                ? static_cast<float>(task.frame_height) * 0.5f +
                                      scfg.search_radius_px
                                : -1.0f;
                    } else {
                        crop_bottom_px_ = -1.0f;
                    }
                }
                scfg.lost_grace_ms = frame_profile->mouse.lost_grace_ms;
                // 切靶防抖（对齐 BB 的 hysteresis / cooldown）：配置缺省即取 MouseProfile 默认
                // 0.5 / 600ms；两者置 0 可完全关闭，回到加此机制前的行为。
                scfg.switch_hysteresis = frame_profile->mouse.switch_hysteresis;
                scfg.switch_cooldown_ms = frame_profile->mouse.switch_cooldown_ms;
                // V1.0.07：贴裁剪区边界剔除 + 锁定尺寸一致性（缺陷修复，出厂即生效）。
                // 两者的定障依据见 TargetSelector.hpp；前者靠 search_radius_px 推出裁剪区
                // 左右边界（capture 与画面同中心），不需要新的几何通路。
                scfg.reject_clip_horizontal = frame_profile->mouse.reject_clip_horizontal;
                scfg.reject_clip_top = frame_profile->mouse.reject_clip_top;
                scfg.clip_margin_px = frame_profile->mouse.clip_margin_px;
                scfg.clip_center_max_px = frame_profile->mouse.clip_center_max_px;
                scfg.track_size_ratio = frame_profile->mouse.track_size_ratio;
                // 选靶四项机制（2026-09-24 补通路）：锁定期 / 打分制 / 粘滞 / 头身稳定。
                // ★ 此前 TargetSelector 里算法已实现但这里没赋值 ⇒ 永远吃结构体默认（全关），
                //   面板也开不了。默认值仍是 0/false ⇒ 不开时选靶行为逐字节不变。
                scfg.lock_hold_ms = frame_profile->mouse.lock_hold_ms;
                scfg.priority_scoring = frame_profile->mouse.priority_scoring;
                scfg.weight_dist = frame_profile->mouse.weight_dist;
                scfg.weight_size = frame_profile->mouse.weight_size;
                // V1.0.12（2026-09-30）：此处原两处 scfg.zoom_scale 赋值已删（不区分倍镜）。
                scfg.stickiness = frame_profile->mouse.stickiness;
                scfg.switch_threshold_px = frame_profile->mouse.switch_threshold_px;
                scfg.head_body_stable = frame_profile->mouse.head_body_stable;
                scfg.hb_body1 = frame_profile->mouse.hb_body1;
                scfg.hb_head1 = frame_profile->mouse.hb_head1;
                scfg.hb_body2 = frame_profile->mouse.hb_body2;
                scfg.hb_head2 = frame_profile->mouse.hb_head2;
                scfg.confidence = frame_profile->mouse.confidence > 0.0f
                                      ? frame_profile->mouse.confidence : 0.25f;
                // 瞄准点：先取全局基底（含 aim_offset_* / head_aim / switch_delay 这些不按档的量），
                // 再用本档覆盖 offset 与类别偏移。
                scfg.aim_ratio_x = ap ? ap->offset_x : frame_profile->mouse.aim_point.offset_x;
                scfg.aim_ratio_y = ap ? ap->offset_y : frame_profile->mouse.aim_point.offset_y;
                kp_x = frame_profile->mouse.kp_x; kp_y = frame_profile->mouse.kp_y;
                kd_x = frame_profile->mouse.kd_x; kd_y = frame_profile->mouse.kd_y;
                aim_point = frame_profile->mouse.aim_point;
                if (ap) {
                    aim_point.offset_x = ap->offset_x;
                    aim_point.offset_y = ap->offset_y;
                    // 本档类别偏移为空 = 沿用全局类别偏移表（老配置合成的第 0 档就是这种情况）
                    if (!ap->class_offsets.empty()) aim_point.class_offsets = ap->class_offsets;
                }
                // 输出链参数：sens 全局缩放 ×（本档移动倍率）× output_scale × output_deadzone
                out_sensitivity = frame_profile->mouse.sensitivity * (ap ? ap->sensitivity : 1.0f);
                out_scale = frame_profile->mouse.output_scale;
                out_deadzone = frame_profile->mouse.output_deadzone;
                recoil_px_per_count = frame_profile->mouse.gain_y_px_per_count > 0.05f
                                          ? frame_profile->mouse.gain_y_px_per_count : 0.65f;
                personal_motion = frame_profile->mouse.personal_motion;
                pull_curve_cfg = frame_profile->mouse.pull_curve;
                lead_cfg = frame_profile->mouse.continuous_lead;
                personal_traj_cfg = frame_profile->mouse.personal_trajectory;
                lock_confirm_cfg = frame_profile->mouse.lock_confirm;
                recoil_cfg = frame_profile->mouse.recoil;
                // ★★★ V1.0.38：**smooth 接回 Pid1Controller 第 5 参**（pid1 完全移植）。
                //   V1.0.13 起这里恒传 0.0 ⇒ `if (smooth)` 恒假 ⇒ pid1 的 soft-limit
                //   （smoothTerm：把大误差下的 P/D 输出压成亚线性，防止一帧打飞）
                //   被整体关掉，只剩裸 K_p + K_i + K_d。
                //   现按 pid1 原文 main() 的 runAxis(..., smooth=9900.0) 接回原值。
                pid_x_.configure(kp_x, kd_x, frame_profile->mouse.predict_x,
                                 frame_profile->mouse.rate_x, frame_profile->mouse.smooth_x);
                pid_y_.configure(kp_y, kd_y, frame_profile->mouse.predict_y,
                                 frame_profile->mouse.rate_y, frame_profile->mouse.smooth_y);
                // ★★★ V1.0.39：把标定实测的 px/count 喂给速度观测器。
                //   pid1 原文 `tv = error_diff + last_u` 隐含假设 gain=1（1count=1px），
                //   我们板端实测 gain≈0.65 ⇒ 不换算就会系统性低估目标速度 35%，
                //   前馈力度恒不足 ⇒ 只能靠调低 predict 补偿（V1.0.13 的 3.0→1.0 就是为此）。
                //   接入后 predict 恢复作者原意量级，换游戏灵敏度不必再动 predict。
                //   放在同一个「配置刷新块」内 ⇒ 标定写回 gain 后热更新即生效。
                //   X/Y 各用自己的 gain（两轴灵敏度可能不同）。
                pid_x_.set_gain(frame_profile->mouse.gain_x_px_per_count);
                pid_y_.set_gain(frame_profile->mouse.gain_y_px_per_count);
                // ★★★ V1.0.43：Fitts 控制器配置 + 控制器选择（与 pid 并存，controller_type 二选一）。
                //   gain 复用标定产物（gain_x/y_px_per_count，与 pid set_gain 同源）。
                use_fitts = (frame_profile->mouse.controller_type == "fitts");
                fitts_x_.configure(frame_profile->mouse.fitts_a_ms,
                                   frame_profile->mouse.fitts_b_ms,
                                   frame_profile->mouse.fitts_deadzone_px,
                                   0.05f, frame_profile->mouse.fitts_ff_gain);
                fitts_y_.configure(frame_profile->mouse.fitts_a_ms,
                                   frame_profile->mouse.fitts_b_ms,
                                   frame_profile->mouse.fitts_deadzone_px,
                                   0.05f, frame_profile->mouse.fitts_ff_gain);
                gain_x_ppc = frame_profile->mouse.gain_x_px_per_count > 1e-4f
                                 ? frame_profile->mouse.gain_x_px_per_count : 0.65f;
                gain_y_ppc = frame_profile->mouse.gain_y_px_per_count > 1e-4f
                                 ? frame_profile->mouse.gain_y_px_per_count : 0.65f;
            }
            // V1.0.11：开火期禁切靶（照 yu 的 fire_switch_guarded）——
            // 扳机激活中若丢掉锁定，不去第 2/3 层另选目标（宁可本帧不瞄，
            // 也不在压枪时把准星从正压着的目标甩到别人身上）。
            scfg.fire_active = trigger_.auto_trigger2().activated();
            const auto selected = selector_.select(task.detections, scfg,
                static_cast<uint32_t>(task.timestamp_us / 1000ULL));
            const uint32_t now_ms32 = static_cast<uint32_t>(task.timestamp_us / 1000ULL);
            // 热键解析/挂起/选档/边沿复位已在选靶之前完成（见上面「热键解析与选档」段）——
            // 这里只消费 injection_allowed 组装状态机事件，不再重复判决。
            AimStateEvent event; event.has_target = selected.valid;
            event.hotkey_active = injection_allowed;
            event.now_ms = task.timestamp_us / 1000ULL;
            event.target_confidence = selected.valid ? selected.box.score : 0.0f;
            event.target_distance = selected.distance;
            if (state_machine_.update(event, scfg.lost_grace_ms, lock_confirm_cfg)) { pid_x_.reset(); pid_y_.reset(); remainder_x_=0.0f; remainder_y_=0.0f; last_target_id_=-1; }
            int16_t move_x = 0, move_y = 0; float ex = 0.0f, ey = 0.0f;
            float pred_ex = 0.0f, pred_ey = 0.0f;  // 第15阶段：预测误差
            float tx = 0.0f, ty = 0.0f, ref_x = 0.0f, ref_y = 0.0f;
            float smooth_tx = 0.0f, smooth_ty = 0.0f;  // 第15阶段：平滑后瞄准点（滤检测框抖动，遥测/控制共用）
            // 控制域误差（像素域）。提到块外：无目标时保持 0，供输出尾链读取。
            float control_x = 0.0f, control_y = 0.0f;
            float aibox_x = 0.0f, aibox_y = 0.0f, scaled_x = 0.0f, scaled_y = 0.0f;
            bool fov_mode_active = false;  // FOV 模式：fov_move 已是 count 域最终移动量，旁路 PID 的 kp×err
            float fov_out_x = 0.0f, fov_out_y = 0.0f;  // FOV 模式输出（count 域）
            float trace_control_x = 0.0f, trace_control_y = 0.0f;
            float trace_smith_dx = 0.0f, trace_smith_dy = 0.0f;
            // ---- Hotkey Gate：最终输出安全边界 ----
            // 热键未按下时：无论目标/误差/PID 状态如何，
            // 本周期一律跳过移动计算（不积分、不累计余数），
            // 只保留误差遥测；最终发送的 OutputAction 强制 dx=dy=0。
            // AI 链路（mailbox→selector→误差遥测）不受热键影响，始终运行。
            if (!injection_allowed) {
                pid_x_.reset();      // 清在途量/PID 状态，防止旧状态绕过 Gate
                pid_y_.reset();
                remainder_x_ = 0.0f;
                remainder_y_ = 0.0f;
            }
            // ---- 本帧目标是否可用于误差控制 ----
            // ★ 压枪**不能**挂在这个条件里：它必须能在无目标时继续下压，否则面板的
            //   「无目标时也压枪」(no_target_always) 与「目标丢失保持窗」
            //   (target_lost_release_ms) 两个语义在集成层永远走不到
            //   （RecoilController.hpp 要求 !has_target 才进这些分支，
            //    而此前传给它的 target_visible 就是 selected.valid ⇒ 恒为 true）。
            const bool target_ok = selected.valid && task.frame_width > 0 && task.frame_height > 0;
            // ---- ★ V1.0.10：压枪的「有效量测」门控（对照 yu，2026-09-30）----
            // yu 压枪稳的根因不是公式，而是它的输入端永远拿不到坏框：跟踪器量测门控
            // （框突变/贴边/尺寸非法 ⇒ 拒绝量测 + 保持上一帧）先跑，压枪只吃有效量测。
            // 我们这一侧对应的信号就是「框底被裁剪区截断 ⇒ 冻结落点（FrozenRect）」，
            // 即 yu 的 target_near_edge / holding_previous。压枪拿它当门控，
            // 无效量测按"目标短暂丢失"处理（走 target_lost_release_ms 渐出窗口）。
            bool measurement_valid = false;
            // 准星到目标距离（px）。无目标时无意义 ⇒ 保持 0（下游据此跳过距离相关项）。
            float dtt_px = 0.0f;
            // 帧间时间基准：**不依赖目标** ⇒ 提到块外，压枪与拉枪曲线都要用。
            const float dt = previous_timestamp_us > 0 && task.timestamp_us > previous_timestamp_us
                ? static_cast<float>(task.timestamp_us - previous_timestamp_us) / 1000000.0f : 0.004f;
            const float dt_ms = dt * 1000.0f;  // 拉枪曲线抖动需要毫秒级时间基准
            // ★ V1.0.31：框裁小已退役（业主口径「框完整，只偏移落点」）。
            //   控制链与显示框都用**模型给的原框**；落点在框内由 offset_y 定位。
            //   排除「非人目标」（球/武器/烟雾）改由选靶层做：类别筛选 + 几何兜底。
            const DetectionBox& aim_box_src = selected.box;
            const AimPointProfile& prof_ub = aim_point;
            if (target_ok) {
                // ---- V1.0.09：框底被裁剪区下边界截断时的身高反推比（按目标自校准）----
                // V1.0.10 起降级为**兜底**：只在「目标一出现就被截、没有可冻结的框」时使用。
                // 未截断帧：h/w 就是真实比例 ⇒ 记下来（本目标 EMA + 跨目标 EMA）。
                // 截断帧：用记住的比例 × 当前框宽反推身高（宽度不随纵向裁剪失真）。
                float clipped_h_over_w = 0.0f;
                DetectionBox frozen_box;
                bool have_frozen = false;
                {
                    // ★ V1.0.31：框不再裁剪，这里判的就是**模型给的原框**。
                    //   近身时框底会被 640×640 画面切掉 ⇒ 触发外推/冻结（既有行为）。
                    const bool box_bottom_clipped =
                        crop_bottom_px_ > 0.0f &&
                        aim_box_src.y2 >= crop_bottom_px_ - prof_ub.clip_bottom_margin_px;
                    // 有效量测 = 框没被裁剪区截断（见上面 measurement_valid 的说明）
                    measurement_valid = !box_bottom_clipped;
                    // 自校准比（h/w）记录的也是收缩域的比 —— 与外推消费端同域，自洽。
                    clip_ratio_tracker_.observe(aim_box_src.x2 - aim_box_src.x1,
                                                aim_box_src.y2 - aim_box_src.y1,
                                                box_bottom_clipped, selected.target_id);
                    // ---- V1.0.10：冻结落点 ----
                    // 腿被切 ⇒ 停止更新框，落点保持上一次能看全的那一帧（yu 的
                    // holding_previous 思路）。走近时框顶上升与身高变大互相抵消，
                    // 那个点在屏幕上本来就不该动 ⇒ 冻整个 rect 比外推更简单也更准。
                    frozen_rect_.observe(aim_box_src, box_bottom_clipped, selected.target_id);
                    if (box_bottom_clipped) {
                        have_frozen = frozen_rect_.frozen_for(selected.target_id, &frozen_box);
                        // 兜底比：只在 have_frozen=false（拐角撞脸，没看过全框）时被消费。
                        clipped_h_over_w = clip_ratio_tracker_.ratio_for(selected.target_id);
                    }
                }
                // 冻结生效 ⇒ 用冻结框算落点；crop_bottom 传 -1 明确关闭外推路径
                // （冻结框本身在裁剪区内部，不该再被外推改写）。
                // 没有冻结框 ⇒ 走 V1.0.09 外推（行为不变，留 A/B 通路）。
                // ★ V1.0.23：aim_box 的未冻结分支取收缩框 aim_box_src（frozen_rect
                //   观察的也是收缩框 ⇒ 冻结域与实时域同域，切换不跳）。
                const DetectionBox& aim_box = have_frozen ? frozen_box : aim_box_src;
                // ★ V1.0.23：落点用收缩框 + 换算后的 prof_ub（offset 等比例量已 ÷k，
                //   ty 与不收缩时同一像素）。aim_point_at / constrain 内部不改。
                if (!aim_point_at(aim_box, selected.box.class_id, prof_ub, &tx, &ty,
                                  have_frozen ? -1.0f : crop_bottom_px_,
                                  have_frozen ? 0.0f : clipped_h_over_w)) {
                    tx = (aim_box.x1 + aim_box.x2) * 0.5f;
                    ty = aim_box.y1 + (aim_box.y2 - aim_box.y1) * 0.15f;
                }
                // 第3项：头部瞄准约束（默认关）。若启用且瞄头，把瞄准点钳进头区安全区
                // 并限制单帧滞后，防止锁头时瞄准点飘出头部。约束在 AimPointProfile.cpp。
                if (prof_ub.head_aim.enabled) {
                    constrain_aim_point_to_head(aim_box, prof_ub, &tx, &ty);
                }
                // 第15阶段：目标跟踪器（速度估计 + 预测）。
                // 目标切换（target_id 变化）→ tracker 内部 Reset（速度清零）。
                // prediction_time_s_>0 时用预测点做控制误差；=0 保持原行为（直接用瞄准点）。
                // ★ V1.0.11：continuity（同一个目标被重新编号）时**不重置** ——
                //   照 yu 的 continuity_reference_rect：id 变了但参考框重叠+尺寸一致就是
                //   同一目标，重置 One-Euro 反而让落点跳一下、跟随重新起步。
                if (tracker_.target_switched(selected.target_id) && !selected.continuity) {
                    tracker_.reset();
                }
                // V3 阶段 3a：滤波强度按框高自适应（默认关 ⇒ 与加此机制前逐字节一致）。
                // 配置每帧重读（改配置即时生效）；框高喂在 update **之后** —— 切靶时
                // update 会把框高 EMA 清零，放之后才能立刻用新目标的框高重建。
                tracker_.configure(frame_profile->mouse.box_adaptive);
                tracker_.update(tx, ty, selected.target_id, task.timestamp_us);
                // ★ V1.0.23：框高喂收缩框（滤波自适应/遥测与控制域一致，域不混）。
                tracker_.set_box_h(aim_box_src.y2 - aim_box_src.y1);
                // 第15阶段：控制误差必须用「平滑后瞄准点」。
                // 此前 prediction_time_s_=0 时 control 直接用原始 ex/ey，
                // 检测框上边缘 y1 帧间跳变（±18px，模型头顶边界）直接进 PID：
                //   K_p 输出 ±2 count + K_d 反向 ±4 count → 输出在 deadzone(1.0)
                //   反复穿越 → 「停顿（阻塞）+ 锁定后上下抖动」。
                // 平滑位置来自 AimTracker 内置 One-Euro（min_cutoff=0.8Hz），
                // 预测关闭时也用它做控制误差（不平滑只做速度估计）。
                smooth_tx = tracker_.state().x;
                smooth_ty = tracker_.state().y;
                float pred_tx = smooth_tx, pred_ty = smooth_ty;
                if (prediction_time_s_ > 0.0f) {
                    tracker_.predict(prediction_time_s_, &pred_tx, &pred_ty);
                }
                // ★ V1.0.11：continuity 时**不重置**整条链（照 yu 的 continuity 语义）——
                //   同一个目标被重新编号不需要清 PID/拉枪/压枪计时，清了反而制造一次落点跳。
                if (last_target_id_ != -1 && selected.target_id != last_target_id_ &&
                    !selected.continuity) {
                    // 目标切换：速度/加速度来自旧目标，必须清除预测状态。
                    pid_x_.reset(); pid_y_.reset(); remainder_x_ = remainder_y_ = 0.0f;
                    pull_curve_.reset();  // 拉枪曲线时间基准清零（新目标重新拉枪）
                    continuous_lead_.reset();  // 持续提前量累计清零（新目标重新累计"同向距离"）
                    personal_shader_.reset();  // 拟人化整形重置（新目标重新整形）
                    recoil_.reset();  // 压枪：换目标 ⇒ 计时/累计量/残差作废（yu 同口径）
                    jitter_ff_.reset();       // V3 阶段 5：换目标 ⇒ 旧目标的抖动欠账作废
                }
                last_target_id_ = selected.target_id;
                // AIBOX 对标：不做位置外推；误差直接来自本帧检测结果。
                // 速度信息只进入 P_PID 的前馈/Kalman，不在目标坐标层 coast。
                CoordinateTransform::reference_point(static_cast<float>(task.frame_width),
                                                     static_cast<float>(task.frame_height),
                                                     aim_point, &ref_x, &ref_y);
                // 第15阶段：原始误差用瞄准点，控制误差用预测点（若启用）。
                ex = tx - ref_x;
                ey = ty - ref_y;
                pred_ex = pred_tx - ref_x;
                pred_ey = pred_ty - ref_y;
                control_x = (prediction_time_s_ > 0.0f) ? pred_ex : (smooth_tx - ref_x);
                control_y = (prediction_time_s_ > 0.0f) ? pred_ey : (smooth_ty - ref_y);
                dtt_px = std::hypot(ex, ey);
                if (frame_profile) {
                    // 自动标定偏置进入同一控制误差域，复用正式 PID/输出链测量响应。
                    if (frame_profile->mouse.calibrating) {
                        control_x += frame_profile->mouse.calibration_bias_x;
                        control_y += frame_profile->mouse.calibration_bias_y;
                    }
                    if (frame_profile->mouse.fov_mode) {
                        // FOV 模式：像素误差 → 角度 → HID count（fov_move 输出已是最终移动量）。
                        // 修复点：此前把 count 域输出替换 control_x 再进 PID（kp=25×count）双重缩放。
                        // 现在 control_x 保持像素域（个人曲线/拉枪距离判定需要像素域），
                        // fov 输出存入 fov_out，在 PID 调用处直接旁路（见 L191-193）。
                        fov_out_x = fov_move_x(ex, static_cast<float>(task.frame_width),
                                               frame_profile->mouse.hfov, frame_profile->mouse.move_speed_x);
                        fov_out_y = fov_move_y(ey, static_cast<float>(task.frame_height),
                                               frame_profile->mouse.vfov, frame_profile->mouse.move_speed_y);
                        fov_mode_active = true;
                    }
                }
                // 2026-09-29：扳机压枪联动偏移（trigger.y_offset）已随 v7.26 一并删除。
                // ---- V3 阶段 5：拟人化抖动前馈扣除（默认关 ⇒ 与本机制加入前逐字节一致）----
                // 把"到期的抖动 count"换算成像素、加回控制误差 ⇒ PID 看不见自己发的抖动。
                // 量纲：jitter(count) × px_per_count = 画面像素位移。
                // 顺序：先 advance（取到期量），再走 PID；本帧新注入的抖动在帧尾才 push，
                //       ⇒ 本帧注入的样本下一帧才开始计时，不会"提前一帧扣"。
                jitter_ff_x_px_ = 0.0f;
                jitter_ff_y_px_ = 0.0f;
                if (frame_profile && frame_profile->mouse.jitter_feedforward.enabled) {
                    const auto& ff = frame_profile->mouse.jitter_feedforward;
                    float delay_ms = ff.delay_ms;
                    if (!(delay_ms > 0.0f)) delay_ms = frame_profile->mouse.response_delay_ms;
                    float g = ff.gain_px_per_count;
                    if (!(g > 0.0f)) g = frame_profile->mouse.gain_y_px_per_count;
                    if (delay_ms > 0.0f && g > 0.0f) {
                        float jx = 0.0f, jy = 0.0f;
                        jitter_ff_.advance(dt_ms, delay_ms, &jx, &jy);
                        const float sc = (ff.scale > 0.0f) ? ff.scale : 0.0f;
                        float ax = jx * g * sc;
                        float ay = jy * g * sc;
                        // 单帧上限：防异常配置把误差顶飞（默认 40px）
                        const float lim = (ff.max_px > 0.0f) ? ff.max_px : 0.0f;
                        if (lim > 0.0f) {
                            ax = std::clamp(ax, -lim, lim);
                            ay = std::clamp(ay, -lim, lim);
                        }
                        control_x += ax;
                        control_y += ay;
                        jitter_ff_x_px_ = ax;
                        jitter_ff_y_px_ = ay;
                    } else {
                        // 没标过延迟 / 没标过 gain ⇒ 前馈无从换算，清空缓冲防止陈旧样本积压
                        jitter_ff_.reset();
                    }
                } else if (jitter_ff_.pending() > 0) {
                    jitter_ff_.reset();  // 开关刚关：别留一队过期样本等下次开启时集中释放
                }
                // pid1.cpp P_PID 直接消费控制域误差（像素域）。
                // FOV 模式：fov_out 已是 count 域最终移动量，直接作为控制器输出（旁路 kp×err）。
                trace_smith_dx = 0.0f; trace_smith_dy = 0.0f;
                trace_control_x = control_x; trace_control_y = control_y;
                if (fov_mode_active) {
                    aibox_x = fov_out_x;
                    aibox_y = fov_out_y;
                } else if (use_fitts) {
                    // ★★★ V1.0.43：Fitts 定律控制器（近慢远快、只追误差不追变化率，
                    //   治 pid1「追着怪/停不住」）。死区用目标框高做尺寸自适应
                    //   （远目标框小死区小→精确，近目标框大死区大→不抖）。
                    const float box_h = (selected.valid && selected.box.y2 > selected.box.y1)
                                            ? (selected.box.y2 - selected.box.y1) : 0.0f;
                    // ★ 速度前馈用 AimTracker 的 vx/vy（One-Euro + 框高自适应滤波后的速度估计，
                    //   噪声已抑制）。补偿 Fitts 只对误差做比例响应带来的固有滞后。
                    aibox_x = fitts_x_.update(control_x, box_h, gain_x_ppc,
                                              tracker_.state().vx, dt_ms);
                    aibox_y = fitts_y_.update(control_y, box_h, gain_y_ppc,
                                              tracker_.state().vy, dt_ms);
                } else {
                    // pid1.cpp P_PID：X predict=3.0，Y predict=0（main() 原始参数）。
                    // ★ V1.0.12（2026-09-30）：原先误差先除以本档倍镜倍率再进 PID，已按
                    //   业主口径删除（不区分倍镜，所有档位共用一套 kp）。
                    aibox_x = static_cast<float>(pid_x_.update(control_x));
                    aibox_y = static_cast<float>(pid_y_.update(control_y));
                }
                // 输出链：P_PID 输出 × sens（全局灵敏度） × output_scale。
                // rate_x/y 已在 Pid1 内部作为 kp_gain_rate 消费，此处不再重复。
                const float out_gain = out_sensitivity * out_scale;
                scaled_x = aibox_x * out_gain;
                scaled_y = aibox_y * out_gain;
                // 个人曲线只改变输出倍率，不绕过 PID、死区和热键安全门。
                const float personal_distance = std::hypot(control_x, control_y);
                const float personal_gain = PersonalMotion::scale(personal_distance, personal_motion);
                scaled_x *= personal_gain;
                scaled_y *= personal_gain;
                // 拉枪曲线：目标误差 ≥ min_distance 时，在拉枪方向附加弧线/抖动（Y 轴附加量）。
                // 位置在 personal_gain 之后、deadzone 之前（PullCurve.hpp 设计输出链顺序）。
                // err 用控制域误差（control_x/y），out 用当前缩放输出（scaled_x/y）。
                scaled_y += pull_curve_.apply(control_x, control_y, scaled_x, scaled_y,
                                             pull_curve_cfg, dt_ms);
                // 持续提前量（continuous_lead）：AI 输出**持续同向**累计超过 enter 阈值后，
                // 在 X 轴附加偏置（渐入渐出），用于跟住横向持续移动的目标。
                // 位置在 pull_curve 之后、recoil 之前（同为输出链附加项）；
                // 依然受 deadzone → remainder → int16 → 拟人化整形 → 热键安全门约束，
                // 热键放开时最终输出照样被归零（不绕过任何安全门）。
                //
                // ★ 量纲（关键）：喂入的是 PID 输出 aibox_x/aibox_y（count 域的"AI 输出"），
                //   **不是** scaled_x/scaled_y。原因：enter 阈值必须与用户灵敏度解耦 ——
                //   若喂 scaled_*（= aibox × sens × output_scale × personal_gain），
                //   用户一调高灵敏度就会让提前量**静默地更早触发**，行为不可预期。
                //   截断成 int32 只丢 <1 count 的残差，对"累计同向距离"判定无实质影响。
                //
                scaled_x += continuous_lead_.apply(static_cast<int32_t>(aibox_x),
                                                   static_cast<int32_t>(aibox_y),
                                                   dt_ms, lead_cfg);
            }  // ← 结束 target_ok 块（以下压枪与输出尾链在无目标时也要跑）

            // ---- 自动扳机（BB 两套状态机 v7.26 / 2.0）----
            // ★ 2026-09-25 接线：此前 TriggerController **全仓无人 include**，AimThread 也不跑它
            //   ⇒ 面板上的「自动扳机」开关是死的：决策没人算、点击没人发
            //   （OutputAction.button_mask 恒 0，后端 mouse_button 也一个字节都没往外发）。
            // 本模块只产出"要不要开火"的决策，注入在这里做（决策与注入分离的原设计）。
            // 两套都关（默认）时 update() 立刻返回空命令 ⇒ 输出链与接线前逐字节一致。
            TriggerCmd trig_cmd;
            if (frame_profile) {
                TriggerInput tin;
                tin.now_ms = now_ms32;
                tin.dt_ms = dt_ms;
                tin.has_target = target_ok;
                tin.dtt_px = dtt_px;
                tin.target_conf = selected.valid ? selected.box.score : 0.0f;
                tin.hotkey_bits = hotkey_bits;   // 挂起时位图已清零 ⇒ 扳机一并停火
                // 准星中心是否被任一检测框覆盖（v7.26 的 crosshair_check 门）
                {
                    float chx = 0.0f, chy = 0.0f;
                    CoordinateTransform::reference_point(static_cast<float>(task.frame_width),
                                                         static_cast<float>(task.frame_height),
                                                         aim_point, &chx, &chy);
                    tin.center_covered = false;
                    for (const auto& tb : task.detections) {
                        if (chx >= tb.x1 && chx <= tb.x2 && chy >= tb.y1 && chy <= tb.y2) {
                            tin.center_covered = true;
                            break;
                        }
                    }
                }
                // 急停检测（准星颜色）：判色在**有帧的那一侧**（推理 worker，CrosshairProbe），
                // 结果经 AimTargetTask.stop_detect_hit 带过来 —— 任务只含小型检测结果、不传图像，
                // 本线程拿不到像素，只能消费这个 bool。
                // 语义（TriggerController 第 269 行）：found=true 才允许开火（找到准星=可以打）。
                // 未开启时该门不生效，沿用原来的恒真。
                tin.stop_detect_found = frame_profile->mouse.trigger2.stop_detect_enabled
                                            ? task.stop_detect_hit
                                            : true;
                trig_cmd = trigger_.update(frame_profile->mouse, tin);

                // ---- 按下/抬起：拆成两条命令，绝不在控制线程里 sleep ----
                // 按压时长（press_duration，默认 50ms）用时间戳跨周期保持：本帧按下、
                // 到点的那一帧抬起。控制线程阻塞 sleep 会直接吃掉几帧瞄准输出。
                if (trigger_release_btn_ != 0 && now_ms32 >= trigger_release_at_ms_) {
                    output_->mouse_button(trigger_release_btn_, output::kActUp);
                    trigger_release_btn_ = 0;
                }
                if (trig_cmd.fire && trig_cmd.button != 0 &&
                    output_->mouse_button(trig_cmd.button, output::kActDown)) {
                    ++trigger_fire_count_;
                    trigger_release_btn_ = trig_cmd.button;
                    const float hold_ms = trig_cmd.press_duration_ms > 1.0f
                                              ? trig_cmd.press_duration_ms : 10.0f;
                    trigger_release_at_ms_ = now_ms32 + static_cast<uint32_t>(hold_ms);
                    // 移动节流：只有 2.0 那套有这个参数，开火后若干帧不发位移（防开火抖动）
                    if (trig_cmd.fired_by_trigger2 &&
                        frame_profile->mouse.trigger2.move_throttle_frames > 0) {
                        trigger_throttle_frames_ =
                            frame_profile->mouse.trigger2.move_throttle_frames;
                    }
                }
            } else {
                trigger_.reset();
            }

            // ---- 压枪（recoil）：开火期间持续下压补偿后坐力 ----
            // ★★ 2026-09-30 对照 yu 重做（业主令：「压枪太臃肿、参数太多，算法逻辑也不对」）。
            //   旧版这里并着三套引擎（速率模型 / BB 三段查表 / 开火期闭环）互相打架，
            //   现在只剩**一套 yu 式速率模型**（yu 的完整逻辑见
            //   .workbuddy/artifacts/yu-压枪深挖与TTBOX方案-2026-09-30.md §1）：
            //     每帧下压 = 3 × strength × speed × ramp × dt     （纯 Y，X 恒 0，无枪械表）
            //     开火中 ramp = 1（yu 没有缓入）；释放后
            //       ramp = 1 − curve_strength · smoothstep(t/80ms)，t = 释放后时长
            //     累计下压量夹在 roi_h 内（yu 的 roi 上限保护）
            //   门控改用 measurement_valid（本帧量测有效），不是裸 target_ok —— 这是 yu
            //   压枪稳的根因：框被裁剪/冻结 ⇒ 拒绝量测 ⇒ 压枪走 target_lost_release_ms
            //   渐出窗口，永远吃不到坏框。
            //   ★ 仍在目标块外计算：无目标/量测无效时压枪靠释放窗口继续跑，正是 yu 的语义。
            float recoil_add_x = 0.0f;
            float recoil_add_y = 0.0f;
            float recoil_rate_px_s = 0.0f;
            {
                // 随枪压枪（trigger2.with_simple_recoil，对齐 yu auto_trigger_spray_assist）：
                // 扳机连发期间自动附带压枪 —— 等价于"帮它按住开火键"，复用同一 hotkey 语义，
                // 不另立一套判定（该开关在旧版是死的，没有任何消费点）。
                uint16_t fire_bits = hotkey_bits;
                if (trig_cmd.recoil_simple && recoil_cfg.hotkey != 0) {
                    fire_bits = static_cast<uint16_t>(fire_bits |
                                                      static_cast<uint16_t>(recoil_cfg.hotkey));
                }
                const auto rd = recoil_.update(fire_bits, measurement_valid, recoil_cfg,
                                               dt_ms, recoil_px_per_count);
                recoil_add_x = rd.x;   // yu 压枪无横向（恒 0），接口位保留
                recoil_add_y = rd.y;   // 下压为正
                if (recoil_.active()) {
                    float sp = recoil_cfg.speed;
                    if (!(sp >= 0.1f)) sp = 0.1f;
                    if (sp > 3.0f) sp = 3.0f;
                    recoil_rate_px_s = 3.0f * recoil_cfg.strength * sp;
                }
            }

            // ---- 输出尾链 ----
            // 有目标 ⇒ PID 已算出 scaled；无目标但压枪在压 ⇒ 也走同一条尾链
            // （deadzone → remainder → int16 → 拟人化 → 热键安全门），
            // 保证压枪量与正常瞄准一样受同样的安全门与量化约束，不绕过任何一道。
            if (target_ok || recoil_add_x != 0.0f || recoil_add_y != 0.0f) {
                // ★★ 2026-09-28：尾链顺序**照搬 BB 927 原版**（main.lua:5943 / :6010-6015）：
                //     速度波动 → 拟人化 → 抗过冲 → 压枪 → 全局正弦波 → deadzone
                //   此前我们是 压枪 → 抗过冲 → 正弦波 → deadzone → **拟人化放最后**，
                //   与 BB 差三处：
                //     ① 拟人化在抗过冲**之后** ⇒ 噪声/过冲不再被抗过冲收一遍；
                //     ② 压枪在抗过冲**之前** ⇒ 压枪量会被抗过冲按"离目标近"衰减掉，
                //        只能靠 target_ok 补丁绕开（原版顺序天然没这问题）；
                //     ③ 拟人化挪到了整数截断**之后** ⇒ 噪声不足 1 count 就被吃掉，
                //        原版是在浮点位移域做的。
                //   ★ 默认这几个模块全关 ⇒ 老配置行为零变化；只有开了的人才有差别。


                // ---- 深度融合（2026-09-28）：两条拟人化链合成一条 ----
                // 职责分工（不再二选一，互补叠加）：
                //   ① 自研 personal_trajectory = **这一段路怎么走**
                //      （Fitts 时长、起步→加速→收尾的速度包络、垂直于移动方向的手抖曲线、
                //        大误差/快目标/老目标自动停手的抑制）
                //   ② BB humanize = **人的生理特征**
                //      （低通、反应延迟、冲过头、快到了收力、手不稳的噪声）
                // 顺序：先定轨迹、再叠人的特征 —— BB 的过冲/噪声才有"超出轨迹"的自由。
                // ★ 两者都开时不会互相打架：包络管"沿路快慢"，噪声/过冲管"末端手感"；
                //   BB 的速度波动只在锁定首帧作用一次，与自研全程包络天然错开。
                float fused_raw_x = scaled_x, fused_raw_y = scaled_y;
                if (personal_traj_cfg.enabled && injection_allowed) {
                    if (!personal_shader_.active()) {
                        personal_shader_.activate(std::hypot(control_x, control_y), personal_traj_cfg);
                        target_age_ms_ = 0.0f;
                    }
                    const auto& ts = tracker_.state();
                    personal_shader_.set_error_speed_px_s(ts.valid ? std::hypot(ts.vx, ts.vy) : 0.0f);
                    personal_shader_.set_target_age_ms(target_age_ms_);
                    target_age_ms_ += dt_ms;
                    personal_shader_.set_target_radius_px(
                        (aim_box_src.y2 - aim_box_src.y1) * 0.5f);  // ★ V1.0.23：同收缩域
                    personal_shader_.shape_f(&scaled_x, &scaled_y, control_x, control_y, dt_ms,
                                             personal_traj_cfg);
                    // V3 阶段 5：只登记**垂直随机抖动**。transport 增益是故意要走的一段。
                    if (frame_profile && frame_profile->mouse.jitter_feedforward.enabled) {
                        jitter_ff_.push(personal_shader_.last_jitter_x(),
                                        personal_shader_.last_jitter_y());
                    }
                }


                // ---- 融合链末端守卫 ----
                // 自研那套安全约束（幅度上限 / 与误差同向 / 不许反向 / 能量不增）
                // 罩住**整条**拟人化输出 —— 深度融合后 BB 的过冲和噪声同样不许
                // 把准星推反、不许凭空加力。自研链没开时这条守卫不生效（保持 BB 原味）。
                if (personal_traj_cfg.enabled && injection_allowed) {
                    personal_shader_.guard_fused(&scaled_x, &scaled_y,
                                                 fused_raw_x, fused_raw_y,
                                                 control_x, control_y, personal_traj_cfg);
                }

                scaled_y += recoil_add_y;
                scaled_x += recoil_add_x;
                // ---- V3 阶段 3b：输出门限挪到余数累加**之后**（2026-09-28）----
                //
                // 旧行为（已废弃）：这里先把 |scaled| < out_deadzone 归零，再进 remainder。
                //   ⇒ 不足门槛的输出被**永久丢弃**，remainder 根本收不到它 ⇒
                //     稳态误差被钉在"输出首次跌破门槛那一刻的剩余误差"，
                //     动态场景下这部分欠账永远补不回来。
                //
                // 新行为：先累加，再由"累积量是否够 out_deadzone"决定本帧发不发，
                //   **余数始终保留**，下一帧继续攒。死区从此只决定"多久发一次"，
                //   不再决定稳态误差。
                //
                // 闭环 replay 实测（core/tools/pid_sim/aim_replay.py，Gain=0.686 / 延迟 51ms / 144fps）：
                //   腰射 移动 60px/s：稳态 7.05px → 5.15px（-27%）
                //   腰射 框跳 ±18px：稳态 0.47px → 0.21px（-55%）
                //   腰射 静止靶：    稳态 0.21px → 0.21px（静止时旧链路靠"冻结"亦可，收益在动态）
                //   ★ 结论：收益集中在**动态场景**，静止靶场景两者相当。
                //
                // out_deadzone 沿用同一个配置键（面板「抖动忽略门槛」，index.html:5393），
                // 量纲仍是 count，语义变为"累积到这么多才发一次"。
                remainder_x_ += scaled_x; remainder_y_ += scaled_y;
                const float emit_x = (std::abs(remainder_x_) >= out_deadzone) ? remainder_x_ : 0.0f;
                const float emit_y = (std::abs(remainder_y_) >= out_deadzone) ? remainder_y_ : 0.0f;
                // int16 截断保护：单帧输出 clamp 到 HID count 范围（-32768..32767），
                // 防异常大值 static_cast 产生实现定义行为（乱飞）。
                constexpr float kHidMax = 32767.0f;
                constexpr float kHidMin = -32768.0f;
                const float cx_f = std::clamp(emit_x, kHidMin, kHidMax);
                const float cy_f = std::clamp(emit_y, kHidMin, kHidMax);
                move_x = static_cast<int16_t>(cx_f);
                move_y = static_cast<int16_t>(cy_f);
                // 只把真正发出去的部分从余数里扣掉；未发的部分继续留着攒（不丢弃）。
                remainder_x_ -= static_cast<float>(move_x); remainder_y_ -= static_cast<float>(move_y);
                // 兜底：若余数已非有限（理论上 validate 已挡），立即清零防持续乱飞
                if (!std::isfinite(remainder_x_) || !std::isfinite(remainder_y_)) {
                    remainder_x_ = 0.0f; remainder_y_ = 0.0f;
                    pid_x_.reset(); pid_y_.reset();
                }
                // ---- 拟人化整形引擎（第 1 项落地）：对 move_x/move_y 做 Fitts 时长+速度包络+垂直抖动 ----
                // 只作用于热键 Gate 之前；Gate 关闭时输出仍被归零（安全边界不变）。
                // 输入：已量化 count(dx,dy) + 控制误差 px(ex,ey)；按需激活（新目标首次有效帧）。
                // ---- BB 拟人化链（humanize.enabled 时替掉旧 personal_shader_）----
                // ★ 2026-09-28 深度融合：两条拟人化链都已**上移**到浮点位移域
                //   （自研整形 → BB 拟人化 → 融合守卫 → 抗过冲，见上面尾链段），
                //   这里不再有第二次整形。两条链不再互斥，互补叠加。
            }
            // ---- Hotkey Gate 兜底（安全边界最后一行）----
            // 无论前面算出什么，热键未按下时最终动作强制归零。
            if (!injection_allowed) {
                move_x = 0;
                move_y = 0;
            }
            // ---- 扳机移动节流（trigger2.move_throttle_frames，2026-09-26 接线）----
            // 开火后这几帧不送位移，压掉扣扳机那一下的抖动。★★ 位移不是丢掉：
            // 退回 remainder，节流过后再顺着发出来（否则每次开火都吃掉一截位移）。
            if (trigger_throttle_frames_ > 0 && (move_x != 0 || move_y != 0)) {
                remainder_x_ += static_cast<float>(move_x);
                remainder_y_ += static_cast<float>(move_y);
                move_x = 0;
                move_y = 0;
                --trigger_throttle_frames_;
            }
            output_->send(output::OutputAction{move_x, move_y, 0, 0, task.frame_number, task.timestamp_us});
            // ---- 第13阶段：链路诊断采样（默认关闭；开启后每 N 帧输出一次完整链路）----
            {
                PipelineDebug::Snapshot ds;
                ds.frame_number = task.frame_number;
                ds.frame_w = task.frame_width;
                ds.frame_h = task.frame_height;
                ds.detections = task.detections.size();
                ds.target.valid = selected.valid;
                ds.target.class_id = selected.valid ? selected.box.class_id : 0;
                ds.target.confidence = selected.valid ? selected.box.score : 0.0f;
                ds.target.box = selected.valid ? selected.box : DetectionBox{};
                ds.target.center_x = selected.valid ? (selected.box.x1 + selected.box.x2) * 0.5f : 0.0f;
                ds.target.center_y = selected.valid ? (selected.box.y1 + selected.box.y2) * 0.5f : 0.0f;
                ds.target.target_id = selected.valid ? selected.target_id : -1;
                ds.point.valid = selected.valid;
                ds.point.x = tx;
                ds.point.y = ty;
                ds.error_x = ex;
                ds.error_y = ey;
                ds.command.dx = move_x;
                ds.command.dy = move_y;
                ds.command.valid = true;
                ds.command.frame_number = task.frame_number;
                ds.command.timestamp_us = task.timestamp_us;
                ds.mouse_disabled = !injection_allowed;  // 本阶段恒 true（禁止注入）
                pipeline_debug_.sample(ds);
            }
            // ---- 第13阶段：PID 逐帧 Trace 采集（默认关闭；只记录不改变控制行为）----
            if (pid_trace_.enabled()) {
                PidTrace::Entry e;
                e.timestamp_us = task.timestamp_us;
                e.frame_number = task.frame_number;
                e.target_id = selected.valid ? selected.target_id : -1;
                e.target_x = selected.valid ? tx : 0.0f;
                e.target_y = selected.valid ? ty : 0.0f;
                e.reference_x = selected.valid ? ref_x : 0.0f;
                e.reference_y = selected.valid ? ref_y : 0.0f;
                e.error_x = ex;
                e.error_y = ey;
                // 第15阶段：预测点/预测误差（AimTracker 输出；未启用预测时=瞄准点/原始误差）
                e.predicted_x = selected.valid ? (prediction_time_s_ > 0.0f ? pred_ex + ref_x : tx) : 0.0f;
                e.predicted_y = selected.valid ? (prediction_time_s_ > 0.0f ? pred_ey + ref_y : ty) : 0.0f;
                e.pred_error_x = selected.valid ? pred_ex : 0.0f;
                e.pred_error_y = selected.valid ? pred_ey : 0.0f;
                e.controller_raw_x = selected.valid ? aibox_x : 0.0f;
                e.controller_raw_y = selected.valid ? aibox_y : 0.0f;
                // 第15阶段：输出链中间值（死区前原始缩放输出 / 死区后 / 余数）
                e.deadzone_out_x = selected.valid ? scaled_x : 0.0f;
                e.deadzone_out_y = selected.valid ? scaled_y : 0.0f;
                e.rate_limited_x = selected.valid ? scaled_x : 0.0f;
                e.rate_limited_y = selected.valid ? scaled_y : 0.0f;
                e.remainder_x = remainder_x_;
                e.remainder_y = remainder_y_;
                e.final_command_x = move_x;
                e.final_command_y = move_y;
                e.target_switch = (selected.valid && target_id_before != -1 &&
                                   selected.target_id != target_id_before) ? 1 : 0;
                e.target_lost = selected.valid ? 0 : 1;
                e.confidence = selected.valid ? selected.box.score : 0.0f;
                pid_trace_.record(e);
            }
            last_timestamp_us_ = task.timestamp_us;
            std::lock_guard<std::mutex> lk(status_mutex_);
            status_.has_task = true;
            status_.detection_boxes = task.detections;
            status_.has_target = selected.valid;
            status_.target_id = selected.valid ? selected.target_id : -1;
            status_.target_class_id = selected.valid ? selected.box.class_id : -1;
            status_.target_score = selected.valid ? selected.box.score : 0.0f;
            // ★ V1.0.24：显示框 = **控制链正在用的那个上半身框**，不再做多框并集。
            //   旧实现把同一目标身上的所有框并起来（身体框 + 头框 → 画面上那个
            //   "又大又含头"的框），业主看到的"框是全身加头部"就是它造成的 ——
            //   显示与控制不是同一个几何，参考物失真。
            //   现在两者同一来源：面板画的就是算法瞄的那块。
            //   比例恒 = upper_body_ratio()，无条件生效。
            if (selected.valid) {
                float display_x1 = aim_box_src.x1;
                float display_y1 = aim_box_src.y1;
                float display_x2 = aim_box_src.x2;
                float display_y2 = aim_box_src.y2;
                // 显示框 One-Euro 平滑：检测框上边缘 y1 帧间跳变 ±18px（模型头顶边界），
                // 平滑后预览框稳定、标定稳定检测（aim_pos_x/y + width/height 变化 <5%）可过。
                // 目标切换时重建平滑状态（新目标坐标完全不同，避免旧轨迹拖尾）。
                if (last_display_target_id_ != selected.target_id) {
                    display_smooth_x1_.reset();
                    display_smooth_y1_.reset();
                    display_smooth_x2_.reset();
                    display_smooth_y2_.reset();
                    last_display_target_id_ = selected.target_id;
                }
                const float display_dt = last_display_ts_us_ > 0 &&
                                                 task.timestamp_us > last_display_ts_us_
                                             ? static_cast<float>(task.timestamp_us - last_display_ts_us_) /
                                                   1000000.0f
                                             : 0.004f;
                last_display_ts_us_ = task.timestamp_us;
                display_x1 = display_smooth_x1_.update(display_x1, display_dt);
                display_y1 = display_smooth_y1_.update(display_y1, display_dt);
                display_x2 = display_smooth_x2_.update(display_x2, display_dt);
                display_y2 = display_smooth_y2_.update(display_y2, display_dt);
                status_.target_x1 = display_x1;
                status_.target_y1 = display_y1;
                status_.target_x2 = display_x2;
                status_.target_y2 = display_y2;
                // 尺寸用平滑后的显示框：标定稳定检测（_calib_target）读 width/height，
                // 原始 h 因 y1 抖动 7.5% > 5% 阈值会导致「目标稳定检测超时」。
                status_.target_width = display_x2 - display_x1;
                status_.target_height = display_y2 - display_y1;
            } else {
                status_.target_x1 = 0.0f;
                status_.target_y1 = 0.0f;
                status_.target_x2 = 0.0f;
                status_.target_y2 = 0.0f;
                status_.target_width = 0.0f;
                status_.target_height = 0.0f;
            }
            if (selected.valid) ++status_.target_frames; else ++status_.no_target_frames;
            status_.trigger_fire_count = trigger_fire_count_;
            status_.trigger_active = trigger_.auto_trigger2().activated();
            status_.trigger_button = trig_cmd.button;
            uint32_t active_tracks = 0;
            for (const auto& te : selector_.tracks()) {
                if (te.active) ++active_tracks;
            }
            status_.tracks = active_tracks;
            // 遥测：平滑后瞄准点（滤检测框 y1 抖动）。
            // CoreRuntime 把 predicted_x/y 映射为 metrics.aim_pos_x/y，
            // 标定稳定检测（_calib_target 的 center jitter <1px）读的就是它，
            // 必须用平滑位置，否则 18px 级框抖动直接导致「目标稳定检测超时」。
            status_.predicted_x = selected.valid ? smooth_tx : 0.0f;
            status_.predicted_y = selected.valid ? smooth_ty : 0.0f;
            status_.target_point_x = selected.valid ? tx : 0.0f;
            status_.target_point_y = selected.valid ? ty : 0.0f;
            status_.reference_x = selected.valid ? ref_x : 0.0f;
            status_.reference_y = selected.valid ? ref_y : 0.0f;
            status_.error_x = ex;
            status_.error_y = ey;
            status_.pid_output_x = selected.valid ? aibox_x : 0.0f;
            status_.pid_output_y = selected.valid ? aibox_y : 0.0f;
            status_.scheduler_input_x = selected.valid ? scaled_x : 0.0f;
            status_.scheduler_input_y = selected.valid ? scaled_y : 0.0f;
            // 压枪遥测（2026-09-30 收敛为 3 项，对齐 yu 的 recoil_* 观测面）
            status_.recoil_add_y = recoil_add_y;
            status_.recoil_acc_px = recoil_.acc_px();
            status_.recoil_rate_px_s = recoil_rate_px_s;
            // V1.0.11：选靶量测门控遥测 —— 累计「判为坏量测 ⇒ 沿用上一帧」的帧数。
            // 上板后直接从记录器核门控有没有在工作（不需要业主配合做实验）。
            status_.selector_hold_frames = selector_.selector_holds_total();
            status_.control_x = trace_control_x;
            status_.control_y = trace_control_y;
            status_.smith_dx = trace_smith_dx;
            status_.smith_dy = trace_smith_dy;
            status_.move_x = move_x;
            status_.move_y = move_y;
            // 累计请求投递的 count（自动标定的分母真源，见 AimStatus::out_counts_x 的说明）。
            // 位置：Gate 之后（未放行的帧 move 已归零），与下面 min/max 统计同一份 move 值。
            status_.out_counts_x += move_x;
            status_.out_counts_y += move_y;
            if (status_.consumed == 0) {
                status_.min_move_x = status_.max_move_x = move_x;
                status_.min_move_y = status_.max_move_y = move_y;
            } else {
                status_.min_move_x = std::min(status_.min_move_x, move_x);
                status_.max_move_x = std::max(status_.max_move_x, move_x);
                status_.min_move_y = std::min(status_.min_move_y, move_y);
                status_.max_move_y = std::max(status_.max_move_y, move_y);
            }
            // move_x/move_y 已是 int16 HID count（见上文 clamp 到 ±32767），
            // 截断阈值必须用 int16 极值判定；旧的 ±127 是 8-bit 时代阈值，
            // 会把正常小幅移动误计为"截断"（E-09，仅遥测）。
            constexpr int16_t kMoveClampMin = std::numeric_limits<int16_t>::min();
            constexpr int16_t kMoveClampMax = std::numeric_limits<int16_t>::max();
            if (move_x <= kMoveClampMin || move_x >= kMoveClampMax ||
                move_y <= kMoveClampMin || move_y >= kMoveClampMax) {
                ++status_.clipped_frames;
            }
            if (!injection_allowed) ++status_.gated_frames;
            // 遥测口径：last_hotkey_bits 保留**原始物理**按键位图（挂起时也如实反映手上真按了什么），
            // 挂起本身单独用一个布尔字段表达，避免"挂起"与"没按键"在遥测里混成同一件事。
            status_.last_hotkey_bits = raw_buttons;
            status_.hotkeys_suspended = hotkeys_suspended_;
            // 本周期生效档索引（-1 = 无档命中）。标定模式恒为 0。
            status_.active_profile = active_profile;
            status_.last_injection_allowed = injection_allowed;
            status_.last_timestamp_us = task.timestamp_us;
            status_.last_frame = task.frame_number;
            ++status_.consumed;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(interval_us_));
    }
}
}  // namespace ttbox::core::aim
