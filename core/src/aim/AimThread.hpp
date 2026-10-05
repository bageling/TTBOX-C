// AimThread.hpp — 独立瞄准控制线程骨架。
// 当前阶段只验证 Worker -> Mailbox -> AimThread 的数据链路，不改变现有采集/推理行为。
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include "pipeline/AimTargetMailbox.hpp"
#include "output/IHidOutput.hpp"
#include "mouse/AimStateMachine.hpp"
#include "mouse/ClipHeightRatio.hpp"
#include "mouse/FrozenRect.hpp"     // V1.0.10：冻结落点（腿被切就停更新，yu holding_previous 思路）
#include "mouse/TargetSelector.hpp"
#include "model/RuntimeProfile.hpp"
#include "aim/Pid1Controller.hpp"
#include "aim/FittsAimController.hpp"
#include "aim/PipelineDebug.hpp"
#include "aim/PidTrace.hpp"
#include "mouse/AimTracker.hpp"
#include "mouse/OneEuroFilter.hpp"
#include "mouse/PullCurve.hpp"
#include "mouse/ContinuousLead.hpp"
#include "mouse/PersonalTrajectoryShader.hpp"
#include "mouse/RecoilController.hpp"
#include "mouse/TriggerController.hpp"
#include "mouse/JitterFeedforward.hpp"
namespace ttbox::core::aim {
class AimThread {
public:
    struct Status {
        bool running = false;
        bool has_task = false;
        bool has_target = false;
        int target_id = -1;
        int target_class_id = -1;
        float target_score = 0.0f;   // 选中目标的模型置信度（0~1），预览框角标显示
        float target_width = 0.0f;
        float target_height = 0.0f;
        float target_x1 = 0.0f;
        float target_y1 = 0.0f;
        float target_x2 = 0.0f;
        float target_y2 = 0.0f;
        std::vector<DetectionBox> detection_boxes;
        float target_point_x = 0.0f;
        float target_point_y = 0.0f;
        float reference_x = 0.0f;
        float reference_y = 0.0f;
        float error_x = 0.0f;
        float error_y = 0.0f;
        float pid_output_x = 0.0f;
        float pid_output_y = 0.0f;
        float scheduler_input_x = 0.0f;
        float scheduler_input_y = 0.0f;
        int16_t move_x = 0;
        int16_t move_y = 0;
        uint64_t last_frame = 0;
        uint64_t consumed = 0;
        uint64_t stale = 0;
        uint64_t target_frames = 0;
        uint64_t no_target_frames = 0;
        // V1.0.11：选靶量测门控遥测 —— 累计「本帧量测被判为坏 ⇒ 沿用上一帧框」的帧数。
        // 用途：上板后直接从记录器核"门控有没有在工作、拒了多少"，不必让业主配合做实验。
        uint64_t selector_hold_frames = 0;
        uint32_t tracks = 0;           // 当前跟踪中的目标数（detections/tracks 显示）
        float predicted_x = 0.0f;
        float predicted_y = 0.0f;
        float control_x = 0.0f;
        float control_y = 0.0f;
        float smith_dx = 0.0f;
        float smith_dy = 0.0f;
        int16_t min_move_x = 0;
        int16_t max_move_x = 0;
        int16_t min_move_y = 0;
        int16_t max_move_y = 0;
        // 累计**请求投递**的 HID count（有符号和，自 start() 起累加，不随 consumed 重置）。
        // 为什么需要：自动标定要的是「每 count 对应多少 px」的物理增益，而闭环里有恒等式
        //   画面上目标的位移(px) = gain(px/count) × Σcounts
        // （相机位移由 count 积分而来，与控制器参数无关）⇒ gain = Δpx / ΔΣcounts。
        // 旧实现拿 bias 的 px 当分母，量纲就不对（px/px）⇒ 比值恒 ≈1.0、与游戏灵敏度无关，
        // 标定"成功"也只能写出与真实无关的参数。
        // 口径：Gate 之后的最终 move_x/y（未放行的帧上面已归零 ⇒ 天然不进累计）。
        // 与真正落到 usbproxy 的 count 一致的前提是发送不失败；标定时应同时核对
        // mouse_control_socket_write_ok 是否在涨（见 CoreRuntime 的遥测映射）。
        int64_t out_counts_x = 0;
        int64_t out_counts_y = 0;
        uint64_t clipped_frames = 0;
        uint64_t gated_frames = 0;      // Hotkey Gate 拦截的周期数（热键未按）
        uint16_t last_hotkey_bits = 0;  // 最近一次采样的物理按键位图（遥测）
        bool last_injection_allowed = false;  // 最近一次 Gate 判定结果
        // 热键保护是否处于「全部挂起」（hotkey_guard.enabled 时由 toggle_hotkey 翻转）。
        // 挂起期间热键位图被清零 ⇒ last_injection_allowed 恒 false、gated_frames 持续增长。
        bool hotkeys_suspended = false;
        // 本周期生效的瞄准档索引（mouse.aim_profiles 的下标）；-1 = 无档命中。
        // 「任意两档键位互斥」的面板约束保证最多一档命中 ⇒ 这个值没有歧义。
        // 标定模式（mouse.calibrating）恒为 0。面板用它显示"当前第 N 档生效"。
        int active_profile = -1;
        uint64_t last_timestamp_us = 0;
        // ---- 自动扳机遥测（2026-09-25 接线后新增）----
        uint64_t trigger_fire_count = 0;  // 自 start() 起累计开火次数（按下命令成功投递才算）
        bool trigger_active = false;      // 任一扳机处于激活态（面板 pill 用）
        uint8_t trigger_button = 0;       // 最近一次开火使用的键位掩码（0 = 未开火）
        // ---- 压枪遥测（2026-09-30 对照 yu 重做后收敛为 3 个字段）----
        // 旧版有 7 个闭环字段（add_y/integral/state/obs_frames/p_term/i_term/baseline），
        // 随闭环引擎一并删除。现在压枪只有一条 yu 式速率链路，观测面就三样：
        // 本帧注入量、本次开火累计下压量、当前拉力。
        float recoil_add_y = 0.0f;     // 本帧压枪注入的下压量（count）
        float recoil_acc_px = 0.0f;    // 本次开火累计下压量（px，受 roi_h 钳制）
        float recoil_rate_px_s = 0.0f; // 当前拉力 = 3×strength×speed（px/s；未激活为 0）
    };
    AimThread() = default;
    ~AimThread() { stop(); }
    bool start(AimTargetMailbox* mailbox, std::shared_ptr<output::IHidOutput> output, int interval_us = 4000, RuntimeConfig* runtime_config = nullptr, std::atomic<uint16_t>* physical_buttons = nullptr);
    void stop();

    // 第13阶段：链路诊断开关（config 控制；默认关闭，不影响实时链路）
    void set_pipeline_debug(bool enabled, uint32_t sample_interval = 60) {
        pipeline_debug_.enabled = enabled;
        pipeline_debug_.sample_interval = sample_interval > 0 ? sample_interval : 60;
    }

    // 第13阶段：PID Trace 采集开关（config 控制；默认关闭，只记录不改变行为）
    void set_pid_trace(bool enabled, const std::string& path = "") {
        if (enabled) {
            if (!pid_trace_.open(path)) {
                std::fprintf(stderr, "[PidTrace] 打开 Trace 文件失败: %s\n",
                             path.empty() ? "/tmp/pid_trace.csv" : path.c_str());
                return;
            }
        } else {
            pid_trace_.close();
        }
    }

    // 第15阶段：预测时域（秒；0=关闭预测，保持原行为）
    void set_prediction_time(float seconds) {
        prediction_time_s_ = seconds > 0.0f ? seconds : 0.0f;
    }
    Status status() const;

private:
    // 每次 start 都代表一个全新的采集/模型运行世代。必须清除旧模型的目标锁定、
    // 跟踪、PID、亚像素余数、显示滤波和压枪/拟人状态，禁止 A 模型状态泄漏到 B。
    void reset_runtime_state();
    void loop();
    AimTargetMailbox* mailbox_ = nullptr;
    std::shared_ptr<output::IHidOutput> output_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int interval_us_ = 4000;
    RuntimeConfig* runtime_config_ = nullptr;
    std::atomic<uint16_t>* physical_buttons_ = nullptr;
    TargetSelector selector_;
    Pid1Controller pid_x_;   // P_PID：X 轴（predict=3.0）
    Pid1Controller pid_y_;   // P_PID：Y 轴（predict=0.0）
    // ★ V1.0.43：Fitts 定律控制器（dat58 思路，治 pid1「追着怪/停不住」）。
    //   与 pid_x_/pid_y_ 并存，由 mouse.controller_type 二选一（默认 fitts）。
    FittsAimController fitts_x_;
    FittsAimController fitts_y_;
    AimStateMachine state_machine_;
    PipelineDebug pipeline_debug_;  // 第13阶段：链路诊断采样器（默认关闭）
    PidTrace pid_trace_;            // 第13阶段：PID 逐帧 Trace 采集（默认关闭）
    AimTracker tracker_;            // 第15阶段：目标跟踪器（速度估计+预测）
    PullCurve pull_curve_;          // 拉枪曲线：远距离拉枪时附加弧线/抖动（deadzone 前生效）
    ContinuousLead continuous_lead_;  // 持续提前量：AI 输出持续同向后附加 X 偏置（pull_curve 后、recoil 前注入 scaled_x）
    PersonalTrajectoryShader personal_shader_;  // 拟人化整形引擎：Fitts 时长+包络+垂直抖动（Gate 前生效）
    RecoilController recoil_;       // 压枪引擎（2026-09-30 对照 yu 重做：单套速率模型）
    // 自动扳机（BB 两套状态机）。此前全仓无人 include ⇒ 面板开关是死的、点击发不出去。
    // 决策在这里产出，注入走 output_->mouse_button（按下/抬起两条命令，不在控制线程里 sleep）。
    TriggerController trigger_;
    uint8_t trigger_release_btn_ = 0;      // 待抬起的键位掩码（0 = 无）
    uint32_t trigger_release_at_ms_ = 0;   // 抬起时刻（now_ms 时基）
    uint64_t trigger_fire_count_ = 0;
    // 扳机开火联动（此前面板有开关、core 不读 = 假开关）：
    //   移动节流（trigger2.move_throttle_frames）：开火后这么多帧内不发鼠标位移，
    //   位移退回 remainder（不丢量），用于压掉开火瞬间的抖动。
    //   ★ 2026-09-29：另一件「压枪联动偏移（trigger.y_offset）」已随 v7.26 一并删除。
    int trigger_throttle_frames_ = 0;
    // 热键边沿（对齐 BB「按下 shuwuResetPid、松开完全重置」）。
    // 下降沿 = 本帧未放行（含 mouse.enabled=false / 热键松开 / 挂起）。
    bool last_injection_allowed_ = false;
    // ---- V1.0.12（2026-09-30）：active_zoom_scale_（PID 误差分母）已删 —— 不区分倍镜。----
    // ---- V1.0.08：裁剪区下边界（与检测框/准星同一坐标系，全帧像素）----
    // = 帧高/2 + search_radius（V1.0.13 起 capture.offset_y 已删，裁剪区恒居中；
    //   板端实测 1440/2 + 320 = 1040）。
    // 只给「框底被裁剪区截断 ⇒ 落点外推」用；<0 = 未知（不做外推）。
    float crop_bottom_px_ = -1.0f;
    // ---- V1.0.09：被截断时的身高反推比（h/w），按目标自校准 ----
    // 状态机抽在 ClipHeightRatio.hpp（纯状态、可单测）。为什么不能写死一个「人体宽高比」：
    // 本模型 cls5 的框宽高比在 0.29~0.52 之间漂（远距离常只框上半身），写死会过度修正。
    ClipHeightRatioTracker clip_ratio_tracker_;
    // ---- V1.0.10：冻结落点（腿被切 ⇒ 停止更新框，用上一次能看全的框算落点）----
    // 优先于 V1.0.09 的外推；没有可冻结的框（拐角撞脸）时才退回外推。零新配置项。
    FrozenRectTracker frozen_rect_;
    // ---- 压枪观测的「短暂丢目标容忍」：2026-09-30 已删 ----
    // 旧版这里有一条 kClObsHoldUs=150ms 的"沿用上次有效观测"补丁，是为已删除的闭环引擎
    // 续积分用的。对照 yu 后不需要了：yu 的做法是**开火门控 + target_lost_release_ms
    // 渐出**（压枪在释放窗内自己继续跑，不依赖"沿用旧误差"）。同一个坑换了个正确解法。
    // ---- V3 阶段 5：拟人化抖动前馈扣除（2026-09-28）----
    // 两条拟人化链（humanize / personal_trajectory）都只往输出里"加"抖动，
    // 但抖动会在 response_delay_ms（实测 51ms）之后出现在采集画面里，被 PID 当成
    // "目标动了"反向追 ⇒ 抖动被自己抵消，闭环还多一串多余修正。
    // 这里按延迟把注入量**加回**控制误差 ⇒ PID 看不见自己发的抖动。
    // ★ 挂在"抖动分量"上（两条链各自上报），**不挂整条整形量**：速度包络/制动是
    //   故意要走的一段位移，扣掉会让 PID 以为还没到 ⇒ 过冲。
    JitterFeedforward jitter_ff_;
    // 本帧实际加回的像素量（诊断用，供后续观测字段）
    float jitter_ff_x_px_ = 0.0f;
    float jitter_ff_y_px_ = 0.0f;
    // 热键保护：toggle_hotkey 的**上升沿**翻转挂起状态。用上一周期的原始位图判边沿，
    // 与瞄准热键的"按住才生效"语义区分开（这里是按一下切换一次，按住不会连续翻转）。
    uint16_t last_raw_buttons_ = 0;         // 上一周期采样到的原始物理按键位图
    bool hotkeys_suspended_ = false;        // 当前是否全部挂起（跨周期保持，直到再按一次）
    // 显示框 One-Euro 平滑（第15阶段）：检测框上边缘 y1 帧间跳变 ±18px（模型头顶边界），
    // 平滑后预览框稳定、标定稳定检测可过。仅影响显示/标定观测，不影响瞄准控制链。
    OneEuroFilter display_smooth_x1_{0.8f, 0.10f, 1.0f};
    OneEuroFilter display_smooth_y1_{0.8f, 0.10f, 1.0f};
    OneEuroFilter display_smooth_x2_{0.8f, 0.10f, 1.0f};
    OneEuroFilter display_smooth_y2_{0.8f, 0.10f, 1.0f};
    int last_display_target_id_ = -1;
    uint64_t last_display_ts_us_ = 0;  // 显示框平滑用的上一帧时间戳（display 块独立于控制 dt）
    float target_age_ms_ = 0.0f;                    // 当前选中目标年龄（ms，拟人化整形用）
    float prediction_time_s_ = 0.0f;  // 第15阶段：预测时域（秒；0=关闭预测，保持原行为）
    uint64_t last_timestamp_us_ = 0;
    float remainder_x_ = 0.0f;
    float remainder_y_ = 0.0f;
    int last_target_id_ = -1;
    mutable std::mutex status_mutex_;
    Status status_{};
};
}  // namespace ttbox::core::aim
