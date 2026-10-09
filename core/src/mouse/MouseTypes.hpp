// MouseTypes.hpp — A10 AI 鼠标注入基础类型
//
// 物理透传 + AI 注入双路径。本文件只定义类型/配置，不含逻辑。
// 命名空间 ttbox::core::aim（避免与 hid/HidTypes.hpp 的 MouseState/CoordinateTransform 冲突）。
#pragma once

#include <cstdint>
#include <cstring>

#include "common/Types.hpp"

namespace ttbox::core::aim {

// 瞄准状态机状态（AimStateMachine）
enum class AimState : int {
    kIdle = 0,       // 无目标 / 未激活
    kSelecting = 1,  // 目标检测命中，待选中
    kAiming = 2,     // 目标选中 + 热键有效，持续输出
    kLostGrace = 3,  // 目标丢失宽限期（默认 78ms）
};

// ★ 2026-10-04 代码体检：本函数**生产代码零引用**（全仓只有这一定义处）。
//   属预留便捷函数，保留不删（删除是业主的决定）；标注以免下次重复排查。
inline const char* aim_state_name(AimState s) {
    switch (s) {
        case AimState::kSelecting: return "SELECTING";
        case AimState::kAiming: return "AIMING";
        case AimState::kLostGrace: return "LOST_GRACE";
        default: return "IDLE";
    }
}

// 瞄准热键触发方式：0=任一按键(any) 1=同时按下(all)
inline const char* mouse_hotkey_mode_name(int mode) {
    return mode == 1 ? "all" : "any";
}
inline int mouse_hotkey_mode_from_string(const char* s) {
    return (s && strcmp(s, "all") == 0) ? 1 : 0;
}

// 物理鼠标相对移动（HID 层解析结果，int16 保真）
struct PhysicalMotion {
    int16_t dx = 0;
    int16_t dy = 0;
    uint16_t buttons = 0;  // bit0=left bit1=right bit2=middle bit3=back bit4=forward
    int8_t wheel = 0;
    uint64_t timestamp_us = 0;
};

// AI 注入移动（经 P → scale → deadzone → smooth → clamp 后）
struct AiMove {
    int16_t dx = 0;
    int16_t dy = 0;
};

// 合并后最终移动
struct MergedMove {
    int16_t dx = 0;
    int16_t dy = 0;
    uint16_t buttons = 0;
    int8_t wheel = 0;
};

// 类别级瞄准点偏移（class_offsets[]：按 class_id + priority 覆盖默认 offset）
struct ClassOffset {
    int class_id = 0;
    float offset_x = 0.5f;  // 框内比例 0~1（0=左/上 1=右/下）
    float offset_y = 0.5f;
    int priority = 0;       // 优先级（同类别多个 offset 时取 priority 最高）
};

// 头部瞄准约束（瞄准点限定在头框内部安全区）
// 把瞄准点限制在"头框内部安全区"，防止瞄准点飘出头部（锁头稳定）。在 body 框上估算头区：
//   head_top = y1 + head_offset_top_fraction × h（默认 0.04）
//   head_bottom = y1 + (head_offset_top_fraction + head_height_fraction) × h（默认 0.04+0.28=0.32 → 上 32% 为头区）
// 约束：aim point 必须落在头区内部（带安全内缩 fraction），且相对锚点单帧滞后 ≤ max_lag。
// 只对"瞄头"生效（offset_y < 0.5）；瞄身体时不做头约束（保留身体偏移自由）。
struct HeadAimConfig {
    bool enabled = false;                 // 是否启用头区约束（默认关，保持现有行为）
    float head_offset_top_fraction = 0.04f;  // 头区顶在 body 框内的 y 比例
    float head_height_fraction = 0.28f;      // 头区高度占 body 框比例（0.28 ≈ 头占上 1/3）
    float safe_inset_fraction = 0.12f;       // 头区内安全内缩比例（0~0.45）
    float max_lag_fraction = 0.18f;          // 锚点滞后上限（相对头区尺寸比例）
    float max_lag_px = 1.25f;                // 锚点滞后上限（px，取两者较小）
};

// 瞄准点配置（AimPointProfile）：
//   默认瞄准点 = 框中心 + offset × 框尺寸；class_offsets 按类别覆盖。
//   ★ V1.0.13（2026-09-30）：aim_offset_x/y（准星在 crop 系的像素偏移）已删 ——
//     业主口径「功能太复杂、参考物太多，落点与设置的瞄点对不上」。
//     准星定义改为裁剪区正中心（CoordinateTransform::reference_point 不再加偏移），
//     于是"落点"只剩 offset_x/offset_y **一个**入口。旧配置里出现这两个键一律
//     静默忽略（等价 0 ⇒ 与删除前行为一致；板端实测一直是 0）。
struct AimPointProfile {
    float offset_x = 0.5f;
    float offset_y = 0.5f;
    std::vector<ClassOffset> class_offsets;
    // ★ switch_delay_ms 已删（2026-09-26）：全仓只有序列化在搬它，没有任何消费者，
    //   面板也没有对应控件 —— 属于"看着像配置、其实没人读"的尸体字段。
    HeadAimConfig head_aim;     // 头部瞄准约束（第3项）
    // ---- V1.0.08：框底被裁剪区下边界截断时的落点修正 ----
    // 机理：crop 只取画面中心 640x640（半径 320px），近身目标的下半身落在 crop 之外
    //   ⇒ 可见框高 h_obs = crop 下边 - y1 比真实身高小 ⇒ ty = y1 + oy*h_obs 相对人体上飘。
    //   越近截得越多：板端实测被截帧落点比真身位置高 p50=59px、极端 210px（约身高 30%，
    //   正好是「从胸口一路走到头顶」）。
    // 修法：框底贴到裁剪区下边界时，用**框宽**反推身高（宽度不随纵向裁剪失真）。
    //   ★ 身高宽比优先取「同一目标最近一次未截断帧的 h/w」（AimThread 自校准，见成员
    //     clip_ratio_）—— 本模型 cls5 的框宽高比实测在 0.29~0.52 之间漂（远距离常只框
    //     上半身），写死会过度修正。没有自校准值时退回下面的 body_w_over_h 兜底。
    // ★ 默认开 = 缺陷修复；设 false 可回到 V1.0.07 行为做 A/B。
    bool clip_bottom_extrapolate = true;
    float body_w_over_h = 0.32f;          // 兜底「人体框 宽/高」（仅无自校准值时用）
    float clip_bottom_margin_px = 12.0f;  // 框底离裁剪区下边界多近算「贴边」
};

// 瞄准档位（2026-09-24）：面板「热键与类别」页每张卡片 = 一个档位。
// 按下的鼠标键命中哪一档，本次瞄准就用那一档的瞄准点 / 类别 / 移动倍率 / FOV 倍率。
//
// ★★ 硬约束：**任意两档的键位并集（主 ∪ 副）必须互斥**（按位与 == 0）。
//    它挡掉的是"同一个键给两档"——那种配置连单键按下都判不出该用哪档，纯属配错。
//    校验由面板保存时（前端 + 后端）共同执行；Core 侧只消费，不报错。
//    代价（业主已接受）：不支持「档1=右键、档2=右键+侧键」这种嵌套。
//    只约束档位之间；挂起键（hotkey_guard.toggle_hotkey）与压枪键不参与 ——
//    压枪与瞄准共用右键本来就有意为之。
//
// ★★★ 但互斥**不足以**让选档唯一，这点必须说清：同时按下两档各自的键
//    （例如左键开火 + 右键瞄准同时按住）会让两档都命中。位图能同时置多位，
//    物理上禁不掉，穷举 32 种按键组合就能看到多档同时命中。
//    所以选档还需要一条兜底：**取面板顺序里第一个命中的档**（见 aim_profile_match）。
//    「单键按下 → 互斥保证唯一」+「多键同按 → 顺序优先」合起来才是确定的选档。
//
// ★ 默认值 (0x02 / 0 / any) 与老平铺字段 aim_hotkey / aim_hotkey2 / aim_hotkey_mode 的默认值
//   逐位相同 ⇒ 结构体自带一档即可让 aim_profiles 非空，是老配置的行为等价基线。
struct AimHotkeyProfile {
    uint8_t hotkey = 0x02;                    // 主热键位掩码：1=left 2=right 4=middle 8=back 16=forward
    uint8_t hotkey2 = 0x00;                   // 副热键位掩码（0=不使用）
    int hotkey_mode = 0;                      // 触发方式：0=任一按键(any) 1=同时按下(all)
    float offset_x = 0.5f;                    // 本档瞄准点 X（框内比例 0~1）
    float offset_y = 0.5f;                    // 本档瞄准点 Y
    std::vector<ClassOffset> class_offsets;   // 本档类别专属偏移（空=沿用 mouse.aim_point 的全局表）
    std::vector<int> class_filter;            // 本档目标类别（空=不限）
    float sensitivity = 1.0f;                 // 本档移动倍率（乘在全局 mouse.sensitivity 之后）
    float fov_scale = 1.0f;                   // 本档 FOV 倍率（0.1~1.0，乘在全局 fov.radius 之后）
    // V1.0.12（2026-09-30）：本档「倍镜真实放大倍率」zoom_scale 与「本档 px/count」
    // gain_px_per_count 已删除 —— 业主口径「不区分倍镜，靠压枪和自瞄把准星拉回目标身上」。
    // 旧配置里出现这两个键一律**静默忽略**（等价 1.0 / 0 ⇒ 与删除前行为一致）。
};

// 单档命中判定：当前按键位图是否落进这一档。
// mode=0(any)：主/副键任一按下即命中（副键为 0 时只看主键）；
// mode=1(all)：主键与副键必须同时按下（任一侧为 0 则永不命中 —— 面板已校验，此处 fail-closed）。
inline bool aim_hotkey_profile_hit(const AimHotkeyProfile& ap, uint16_t buttons) {
    const bool a = ap.hotkey != 0 && (buttons & ap.hotkey) != 0;
    const bool b = ap.hotkey2 != 0 && (buttons & ap.hotkey2) != 0;
    return ap.hotkey_mode == 1 ? (a && b) : (a || b);
}

// 两档键位是否重叠（主 ∪ 副 按位相交）。面板保存校验用同一个判定，避免两处口径漂移。
inline bool aim_profiles_overlap(const AimHotkeyProfile& a, const AimHotkeyProfile& b) {
    const uint16_t ma = static_cast<uint16_t>(a.hotkey) | static_cast<uint16_t>(a.hotkey2);
    const uint16_t mb = static_cast<uint16_t>(b.hotkey) | static_cast<uint16_t>(b.hotkey2);
    return (ma & mb) != 0;
}

// 拉枪曲线（pull_curve：目标距离 ≥ min_distance 时在拉枪方向附加弧线）
struct PullCurveConfig {
    bool enabled = true;
    float strength = 0.8f;       // 弧线强度（0.8）
    // ★ 2026-10-07 清理：jitter_px 已删。抖动于 2026-09-29 删除（PullCurve.hpp 不再消费），
    //   字段此前仅由序列化层（RuntimeProfile）搬运，属死字段。
    float min_distance = 80.0f;  // 激活距离（crop 系 px）
};

// 持续提前量（continuous_lead：AI 输出同向累计超 enter 后附加 X 偏置，渐入渐出）
struct ContinuousLeadConfig {
    bool enabled = false;
    float enter_distance = 150.0f;      // 触发累计距离
    float scale = 0.5f;                 // 偏置比例
    float fade_in_ms = 300.0f;
    float fade_out_ms = 300.0f;
    float near_disable_ratio = 0.66f;   // 目标接近时衰减比例（保留字段）
};



// 拟人化整形引擎（personal_trajectory_shader：Fitts 时长 + 速度包络 + 垂直抖动 + 自适应抑制 + 安全守卫）
// TTBOX 拟人化整形：Fitts 时长 + 速度包络 + 垂直抖动 + 自适应抑制 + 安全守卫。
// 作用在 AimThread 输出链 move_x/move_y（int16 HID count）上、热键 Gate 之前：
//   把"恒定 PID 输出"整形为"接近真人手部动作"的移动包络（加速→减速 + 垂直随机抖动 + 安全钳制）。
// 不绕过 PID / 死区 / 热键安全门（整形后仍被 Gate 归零）。
// 区分于 personal_motion（倍率曲线，只改输出倍率）：本引擎是"完整移动轨迹整形"。
struct PersonalTrajectoryConfig {
    bool enabled = false;          // 总开关（默认关，保持现有行为）
    // -- Fitts 时长模型：目标距离 → 一次移动的期望时长（接近真人）
    float fitts_intercept_ms = 120.0f;      // 拦截常数（ms）
    float fitts_slope_ms_per_bit = 85.0f;   // 每位斜率（ms）、Fitts law 对数距离
    // -- 速度包络（transport 阶段加速/减速）
    float speed_scale = 1.0f;               // 整体速度系数（0.75~1.25，>1 更快 = 时长更短）
    float stability_scale = 1.0f;           // 稳定性系数（0.70~1.35，>1 更稳 = 时长更长）
    // -- 抖动 / 曲线（垂直向随机游走，模拟人手曲线）
    float variation_scale = 1.0f;           // 抖动幅度系数（0.40~1.80）
    float max_extra_px = 2.0f;              // 单轴最大附加量（count）
    float max_visual_variation_px = 1.5f;   // 视觉抖动上限（px）
    float curve_time_constant_ms = 32.0f;   // 抖动自相关时间常数（ms）
    float curve_rms_px = 0.8f;              // 抖动 RMS 基准（px）
    float jitter_amp_px = 0.20f;            // 抖动幅度（px）
    // -- 自适应抑制（大误差/目标快/目标老/方向突变 → 自动降强度或停用，防乱晃）
    bool adaptive_enabled = true;           // 是否启用自适应抑制
    float min_error_px = 18.0f;             // 小于此误差不抖动（close to target）
    float urgent_error_px = 72.0f;          // 超过此误差视为大误差 → 停用整形（直出）
    float urgent_speed_px_s = 520.0f;       // 目标移动速度超过此 → 停用整形
    float max_target_age_ms = 18.0f;        // 目标年龄超过此 → 停用整形
    float capture_priority_ms = 5.0f;       // 捕获前几 ms 停用整形（等镜头稳定）
    float transport_gain = 0.16f;           // 中间段增益（加速峰，0~0.2）
    float direction_change_cosine = 0.15f;  // 方向突变检测余弦阈值
    // -- 响应参数（视觉抖动预算换算：target_radius / response_px_per_count）
    float response_px_per_count = 0.65f;    // 每 count 对应 px（来自 gain_x/y_px_per_count 标定）
};

// 压枪（recoil：按住开火键期间持续下压，补偿后坐力）
// 压枪模块行为设计（12 参数语义），输出链完全基于 TTBOX 自身：
//   压枪量在 AimThread 输出链 pull_curve 之后、deadzone 之前注入 scaled_y，
//   与 PID 输出融合后统一走 deadzone → remainder → int16 → 拟人化整形 → 热键 Gate。
// 不照搬独立 recoil 链路；默认全关，保持旧行为。
// ---------------------------------------------------------------------------
// 压枪（recoil assist）—— 2026-09-30 按 yu（yuai v2）重做后的**唯一**配置面
//
// yu 的压枪引擎（aiassistance_daemon，汇编级还原见
// .workbuddy/artifacts/yu-压枪深挖与TTBOX方案-2026-09-30.md §1）：
//   每帧拉量 = 3 · strength · speed · ramp · dt    （纯 Y，X 恒 0，无枪械表）
//   ramp     = 1 − curve_strength · smoothstep(clamp(t/80ms,0,1))，t = 释放后时长
//   钳制     = 累计补偿量夹在 roi_h 内（yu 的 roi_w/roi_h）
//   门控     = 开火按住 + 目标量测有效；目标丢失后 target_lost_release_ms 内继续跑
//
// 与旧版的差别（旧版臃肿在哪）：
//   · 删掉 RecoilBbConfig 三段查表 / VerticalCorrectionConfig 三档渐变 /
//     RecoilClConfig 开火期闭环 —— 三套并存互相打架，且 yu 一套都没有；
//   · 删掉 humanize_jitter_*（yu 的抖动上限只有 2px，且我们 2026-09-29 已判定为
//     重复抖动机制、零消费）；
//   · 删掉缓入（旧版开火后 ramp 从 0 爬到 1，等于前 ~200ms 压不住）——
//     yu 是**开火即全量**，只在释放段做渐出。
// ---------------------------------------------------------------------------
struct RecoilConfig {
    bool enabled = false;            // 总开关
    int hotkey = 0x01;               // 开火热键位掩码（1=left）
    int hotkey2 = 0x00;              // 副开火热键位掩码（0=不使用）
    int hotkey_mode = 1;             // 触发方式：1=any 任一命中 2=all 同时按下
    bool only_when_target_visible = true;  // 仅有目标量测时才压（防空压）
    float target_lost_release_ms = 300.0f; // 目标丢失后仍压时长（ms），0=立即释放（yu [0,3000]）
    bool trigger_delay_enabled = false;    // 延迟触发开关（防单点误触）
    float trigger_delay_ms = 120.0f;       // 按住超过该时长才开始压（松开重新计时）
    float strength = 100.0f;         // 拉力基准 [0,300]；拉速 = 3×strength×speed px/s
    float speed = 1.0f;              // 拉力倍率 [0.1,3.0]
    float curve_strength = 0.6f;     // 释放渐出深度 [0,1]；0=硬停，1=80ms 内衰减到 0
    float roi_h = 300.0f;            // 累计下压钳制（px，≤0=不限）；对齐 yu 的 roi_h
};

// 2026-09-29：自动扳机 v7.26（TriggerConfig）已整段删除 —— 业主裁定「自动开火只留 2.0」。
//   它的字段（key1/key2/key3/rifle_mode/click_count…）与 2.0 高度重合，两套并在同一次
//   发布里只会让用户选错。旧配置里残留的 `mouse.trigger` 段会被 RuntimeProfile 忽略。
//   2.0 的定义见下方 Trigger2Config。

// ---------------------------------------------------------------------------
// BB 扳机 2.0（对齐 BB `auto_trigger2_*`，见 bb-port/01-选靶与扳机.md §3）
//
// 与已删除的 v7.26 的核心差异：
//   · 组合键**按住即连发**（无点按激活态）；
//   · **首枪门**：需 dtt < first_err（可选 precision 需先稳定 N 帧）；
//     首枪之后**不再校验距离**，只要有锁定就按间隔继续打；
//   · 目标丢失超过 retarget_reset_ms 才重置首枪态（重新走误差+延迟）；
//   · 可选「急停检测」：中心出现指定准星颜色才允许开枪（打狙急停用）；
//   · 随枪压枪：`with_simple_recoil` 一个开关（2026-09-30 收敛：原来简易/进阶/准星三路，
//     后两路挂的是已删除的 BB 查表与准星找色引擎，一并删除。yu 也只有一路
//     `auto_trigger_spray_assist`）。
// ---------------------------------------------------------------------------
struct Trigger2Config {
    bool enabled = false;            // 总开关
    uint8_t key1 = 0x10;             // 组合键1（BB 默认 6=侧键2 → 0x10）
    uint8_t key2 = 0x00;             // 组合键2（0 = 常满足）
    uint8_t fire_button = 0x01;      // 开火键位掩码（BB 默认 1=左键）
    bool with_aim = true;            // 附带自瞄
    bool with_simple_recoil = false; // 随枪压枪：扳机连发期间自动附带压枪（对齐 yu auto_trigger_spray_assist）
    float confidence = 0.5f;         // 本扳机专用检测置信
    float first_err = 30.0f;         // 首枪允许误差上限（px）
    float first_delay = 0.0f;        // 首枪延迟（ms，从首次进入误差圈起算）
    float fire_interval = 1.0f;      // 连发间隔（帧；按 600Hz 折算为 ms = 帧 × 1000/600）
    float fire_random = 0.0f;        // 连发间隔随机 ±（帧）
    int fire_count = 1;              // 每次连发点击次数
    float press_duration = 50.0f;    // 单次点击的按下保持时长（ms）
    int move_throttle_frames = 2;    // 连发期移动合帧发送间隔（帧）
    bool precision_enabled = false;  // 精确模式：首枪前需连续 N 帧进入 precision_range
    float precision_range = 10.0f;   // 精确判定距离阈（px）
    int precision_frames = 5;        // 精确稳定帧数
    float retarget_reset_ms = 1000.0f; // 目标丢失超过该时长即重置首枪态（0 = 不重置）
    bool lite_mode = false;          // 精简模式（跳过拟人/抗过冲，仅压枪）
    bool stop_detect_enabled = false; // 急停检测开关
    int stop_detect_color_id = 2;    // 目标准星颜色（1=红 2=绿 3=蓝 4=黄 5=青 6=品红）
    int stop_detect_tolerance = 60;  // 颜色容差（0~255）
    int stop_detect_range = 80;      // 中心检测半径（px）
    int stop_detect_interval = 10;   // 检测周期（帧）
};



// 热键保护（hotkey_guard）：按一次 toggle_hotkey 在「热键生效 / 全部挂起」之间切换。
//
// 挂起的实现方式是**把物理按键位图在本控制周期内清零**（AimThread 里做），
// 于是所有以位图为判据的链路一并失效：瞄准热键 Gate、压枪开火键，
// 以及后续在同一个位图上挂的连点 / 自动开火。
// 物理鼠标自身的透传不受影响 —— 那条路不经过这个位图（见 usbproxy 的透传分支）。
//
// ★ 默认 enabled=false ⇒ 未显式开启时 AimThread 不做任何翻转、位图原样透传，
//   输出链与本参数加入前逐字节一致（照 ContinuousLeadConfig 的先例）。
struct HotkeyGuardConfig {
    bool enabled = false;          // 总开关（关掉即恢复"未挂起"，不保留幽灵挂起）
    uint8_t toggle_hotkey = 0x04;  // 切换键位掩码：1=left 2=right 4=middle 8=back 16=forward
};

// 目标锁定确认配置（ENTER/HOLD 双阈值 + 确认帧 + instant-enter，第2项）
// control_gate 目标确认：新目标需更高置信度连续确认，已锁目标用较低阈值保持（防闪烁），
// 近距离高置信目标跳过确认窗（快瞄）。
struct LockConfirmConfig {
    int confirmation_frames = 1;      // 新目标连续确认帧数（默认 1=首帧即锁，兼容旧行为）
    float enter_conf = 0.0f;          // 进入（新锁）置信度阈值
    float hold_conf = 0.0f;           // 保持（已锁）置信度阈值（<= enter）
    bool instant_enter_enabled = true; // 近距离高置信目标跳过确认窗
    float instant_enter_dist = 105.0f; // instant-enter 距离阈值（px）
    float instant_enter_conf = 0.50f;  // instant-enter 置信度阈值
};

// TTBOX 个人移动曲线模型：只保存已训练模型的安全运行参数。
// 原始训练样本留在 Gateway 的独立 profile.json，Core 热路径只读 knots。
struct PersonalMotionConfig {
    bool enabled = false;
    float curve_blend = 1.0f;
    // ★ speed_blend / reaction_blend / max_reaction_delay_ms 已删（2026-09-26）：
    //   PersonalMotion::scale 从头到尾只读 enabled / curve_blend / knots，
    //   那三个参数既不显示也不参与计算，留着只会让人以为调了有用。
    std::vector<float> knots;  // 空 ⇒ 用 PersonalMotion::default_knots() 的内置曲线
};

// V3 阶段 3a（2026-09-28）：滤波强度按目标框高自适应。
//
// 动机（训练场实测，docs/calib/range-measure-2026-09-28.json）：
//   同一个腰射档下，20m 处 box_h=84.55px 的抖动 stdev 只有 0.04px（相对 0.05%），
//   30m 处 box_h=50.69px 的抖动 stdev 涨到 0.18px（相对 0.36%）⇒ **相对抖动差 7.6 倍**。
//   远处小框每帧抖的绝对像素虽小、但占框比例大 ⇒ 瞄准点跟着抖 ⇒ 稳态残留变大。
//
// 做法：One-Euro 的 min_cutoff 由「框高 EMA」驱动 —— 框越小，截止频率越低（滤得越狠）；
//   框大到参考尺寸以上就回到现有基准值 0.8Hz，行为与加此机制前一致。
//
// ★ 方向性矛盾（V3 已写明，必须留在注释里，别指望"越强越好"）：
//   加强滤波 ⇒ 相位滞后 ⇒ 移动目标落点偏后。min_cutoff 一个旋钮同时管平滑与滞后，
//   所以**默认关闭**，只在实测确认远处确实抖得难受时才开，且 cutoff 上下限都可配。
struct BoxAdaptiveFilterConfig {
    bool enabled = false;            // 默认关 ⇒ 逐字节保持现有行为（1.5.66 及更早）
    float ref_box_h_px = 100.0f;     // 参考框高：≥ 此值用 max_cutoff_hz（近处/大框）
    float max_cutoff_hz = 0.8f;      // 大框截止频率 = 现行 kPosMinCutoffHz（0.8Hz）
    float min_cutoff_hz = 0.15f;     // 框高趋 0 时的截止频率（滤得更狠）
    float box_h_ema_alpha = 0.10f;   // 框高 EMA 系数（慢变量，防单帧框跳把 cutoff 拉飞）
};

// 鼠标配置（RuntimeProfile.mouse，与模型彻底分离）
struct MouseProfile {
    bool enabled = false;                       // AI 注入总开关（false = 纯物理透传，与 A9 一致）
    // ---- 瞄准档位（2026-09-24）：热键的唯一真源 ----
    // ★ 结构体里**不再**保留平铺的 aim_hotkey / aim_hotkey2 / aim_hotkey_mode，
    //   免得出现"改了平铺字段却不生效"的第二条路。
    // ★ 不变量：**非空**。默认自带一档（0x02 / 0 / any = 老默认值）；
    //   RuntimeProfile 解析时若 JSON 没有 aim_profiles（老配置），会用平铺 JSON key 合成第 0 档。
    //   所以运行期（AimThread / 输出闸门）可以直接遍历，零分配。
    std::vector<AimHotkeyProfile> aim_profiles{AimHotkeyProfile{}};
    float fov_range = 1.0f;                     // 目标选择范围（0~1，仅影响目标选择）
    float confidence = 0.25f;                   // 目标置信度阈值（目标选择）
    // ---- V1.0.40（2026-10-07，移植 BB-828 :4576-4582）动态选靶范围配置 ----
    // 纯视觉拿不到角度/FOV，但**框面积 = 距离的代理量**：小框（远）抖动大 ⇒ 收紧范围只锁准的；
    // 大框（近）⇒ 放开全范围。**默认关**（enabled=false）⇒ 与加此参数前逐字节一致。
    bool dynamic_range_enabled = false;
    float dynamic_range_min_px = 40.0f;         // 远处小目标用的范围（BB 用 40）
    float dynamic_range_box_min_px2 = 200.0f;   // 面积下界（BB 用 200）
    float dynamic_range_box_max_px2 = 1200.0f;  // 面积上界（BB 用 1200）
    // ★★★ V1.0.38（2026-10-05）：**回归 pid1 原文移植**（业主令「pid 以 pid1 为准完全移植，
    //   不许有自己的变动」）。V1.0.13 曾把 kp/kd 语义改成"真实有效值"、删掉 smooth 字段，
    //   本次**全部撤回**，kp/kd/smooth 三者恢复 pid1.cpp main() 的原始参数与原始关系：
    //     runAxis("X", kp=25.0, kd=25.0, predict=3.0, rate=0.3, smooth=9900.0)
    //     runAxis("Y", kp=25.0, kd=25.0, predict=0.0, rate=0.3, smooth=9900.0)
    //   kp/kd 回到「未折算的原始比例增益」；soft-limit 重新由 smooth 字段控制（不再恒 0）。
    //   ⚠ 口径变化：输出量级比 V1.0.13~V1.0.37 大（smooth 不再折进 kp），
    //     出厂/旧配置需重跑标定（业主 2026-10-05 决定：不写迁移，全员重标）。
    float kp_x = 25.0f;                         // X 比例增益（pid1 原始值，未折算）
    float kp_y = 25.0f;
    float kd_x = 25.0f;                         // 微分增益（pid1 刹车，防过冲）
    float kd_y = 25.0f;
    // ★ FOV 角度换算模式（fov_mode/hfov/vfov/move_speed）已删除（2026-10-07）。
    //   依据：纯视觉拿不到真实 FOV ⇒ 焦距是假参数，atan 换算不代表更正确；
    //   业主对标 BB-828 / yey（各5000+ 行源码）确认两者都是**纯像素域**闭环、
    //   fov 命中 0 处。换游戏重调系数的问题改由「像素域闭环标定」解决
    //   （见 docs/architecture/自瞄算链路改造-像素域标定方案-2026-10-07.md）。
    float rate_x = 0.3f;                        // 输出速率（X 独立；pid1 kp_gain_rate=0.3）
        float rate_y = 0.3f;
    float sensitivity = 1.0f;                   // 灵敏度
    float output_scale = 1.0f;                  // 输出缩放（与 fov_range 严格分离）
    // 标定产物：游戏灵敏度（px/count）——鼠标 1 count = 画面多少 px。
    // 输出换算：count = kp×err / gain（px → count 正确换算，防单位错乱过冲）。
    float gain_x_px_per_count = 0.65f;          // X 轴（标定测得；默认 0.65 近似）
    float gain_y_px_per_count = 0.65f;          // Y 轴
    // V3 阶段 5 前置（2026-09-28）：实测**回路延迟**（注入 count → 画面真的动了的毫秒数）。
    // 板端实测 51ms（≈7.3 帧 @144fps）。0 = 还没标过。
    // 为什么必须进配置：拟人化抖动是"自己发出的扰动"，要按这个延迟把它从误差里
    // 扣回去；扣错帧数（比如按"下一帧"扣）前馈自己就变成高频扰动，反而更抖。
    // ★ 此前只活在标定记录里（面板 mouse_response_delay_ms），core 运行时拿不到。
    float response_delay_ms = 0.0f;
    float deadzone_x = 1.0f;                    // X 死区（count，|v|<dz → 0）
    float deadzone_y = 1.0f;
    // controller 公式（kp×rate×err + predict×vel）与输出链参数
    // ★★★ V1.0.44（2026-10-08，业主令「pid1 作为最终控制器，完全照搬 pid1 原文」）：
    //   predict_x 改回 **3.0**，即 `pid1.cpp` main() 的 `runAxis("X", 25.0, 25.0, 3.0, ...)`
    //   原值。这是本项目第二次把这个值设为出厂默认（V1.0.38 曾抄过一次）。
    //   ⚠⚠ **历史教训存档**：V1.0.42 考古定案过——pid1 原文 3.0 是 `main()` 跑分程序里的
    //   演示值（只喂固定误差列表打印输出，非生产配置）；BB927（shuwu PID 原发行方）实战值是
    //   0.5（`pid_shuwu.lua` 注释：「predict X=3 是过期信息，以 cfg 为准 0.5」）；
    //   且 V1.0.38 抄了 3.0 后**出现过「乱飞」**（业主原始反馈「准星直接飞出自瞄范围停在那里」）。
    //   本次是业主在知情上述历史后主动拍板的结果，**不是我们自行推断**。
    //   ⇒ 若日后再现「飞出去 / 冲过头」，第一嫌疑就是这一行，直接调回 0.5 即可验证。
    //   本地仿真参考（gain=0.65 + 51ms + 噪声±2px）：p0.5 移动50px/s 落后2.9px稳、
    //   移动120px/s 落后6.0px稳；p3.0 未做同等条件的稳定性仿真（回路增益接近/超过 1）。
    //   Y 轴：pid1 原文 `runAxis("Y", 25.0, 25.0, 0.0, ...)` 亦为 0，保持不变（一致）。
    float predict_x = 3.0f;                   // X 前馈（2026-10-08 业主令：完全照搬 pid1 原文 main()）
    float predict_y = 0.0f;                   // Y 前馈（BB927 与 pid1 原文均为 0，不打提前量）
    // ★★★ V1.0.38：**smooth 字段恢复**（V1.0.13 曾删除并折算进 kp/kd）。
    //   它是 pid1 的 soft-limit 强度：9900 = 把 P/D 压到 1%（原厂值），0 = 关闭软限幅。
    //   Pid1Controller::update() 里 `if (smooth)` 的第 5 参就是它 —— 之前恒传 0.0
    //   等于把 pid1 最重要的一道大误差保护整个关掉了。
    float smooth_x = 9900.0f;                 // pid1 soft-limit（原始值）
    float smooth_y = 9900.0f;
    // ★★★ pid1 是唯一控制器。V1.0.43（2026-10-05）曾引入 Fitts 定律控制器作为默认
    //   （MT = A + B×log2(2|err|/W+1)，治「追着怪/停不住」），但面板下拉框 HTML 破损
    //   且 core 实现已移除（git 6a868c9），已于 2026-10-07 按业主令整体移除，
    //   面板上的假开关同步清理。个人轨迹时长模型里的 fitts_intercept_ms /
    //   fitts_slope_ms_per_bit 与控制器无关，保留。
    float output_deadzone = 1.0f;               // output_deadzone（自适应死区基准）
    // 插件配置（pull_curve / continuous_lead / recoil / personal_motion / personal_trajectory）
        PullCurveConfig pull_curve;
        PersonalMotionConfig personal_motion;
        PersonalTrajectoryConfig personal_trajectory;  // 拟人化整形引擎（Fitts 时长+包络+垂直抖动+自适应抑制+安全守卫）
        // 持续提前量：AI 输出持续同向累计超 enter 后附加 X 偏置（渐入渐出）。
        // ★ 默认 enabled=false ⇒ 未显式开启时输出链与本参数加入前逐字节一致（行为零变化）。
        // 此前 ContinuousLeadConfig 与 ContinuousLead.hpp 早已存在且有单测，但
        // **本结构体缺该成员、AimThread 从未调用** ⇒ 签名/面板都无从配置（M2 补齐）。
        ContinuousLeadConfig continuous_lead;
    RecoilConfig recoil;                    // 压枪（输出链 pull_curve 后、deadzone 前注入 scaled_y）
    // ---- 自动扳机（BB 对标，2026-09-24 移植）----
    // 两套状态机互相独立，可分别开启；都只产出"要开火"的决策（TriggerCmd），
    // 真正的点击由 AimThread 拿到决策后调 output->mouse_click 注入 —— 决策与注入分离。
    // 默认 enabled=false ⇒ 不开时 AimThread 不跑扳机，输出链与加入前逐字节一致。
    Trigger2Config trigger2;                // BB 扳机 2.0（v7.26 已删，2026-09-29）
    HotkeyGuardConfig hotkey_guard;         // 热键保护（按 toggle_hotkey 切换「热键挂起」）
    AimPointProfile aim_point;
    float lost_grace_ms = 78.0f;                // 目标丢失宽限期
    // ---- 切靶防抖（对齐 BB：target_switch_hysteresis / switch_cooldown）----
    // 只在「刚失去锁定、正要另选新目标」时生效；目标仍被锁定时不触发
    //（第 1 层 track_lock 本来就保持锁定，不会走到这里）。
    //  hysteresis：新目标必须比刚失去的锁定目标**明显更近**才允许切
    //              —— 判定 `new_dist_sq × (1+h)² < old_dist_sq`（用平方比，省 sqrt）。
    //              BB 取 50%，即新目标要近 1/1.5 以上才切。
    //  cooldown ：一次切换发生后，这段时间内不再切换，防来回拉锯。
    //  两者都为 0/0 时行为与加入前逐字节一致（旧用例兼容）。
    float switch_hysteresis = 0.5f;             // 切换滞后比例（0.5 = 需近 50%）
    float switch_cooldown_ms = 600.0f;          // 切换冷却（ms）
    // ---- V1.0.07：贴裁剪区边界剔除 + 锁定尺寸一致性（详见 TargetSelector.hpp）----
    // 前四项是缺陷修复（出厂即生效，不是可选特性）：
    //   ① 贴裁剪区左右边界的框（画面外的人被切成瘦长条）一旦被选中会把准星猛拉 300px；
    //   ② 第 1 层 track_lock 不看框尺寸 ⇒ cls5↔cls0 两套人体框来回换，落点乱跳。
    // 每项都可单独关（设 false / 0）回到加此参数前的行为，便于 A/B 定位。
    bool reject_clip_horizontal = true;   // 左/右贴裁剪区边界 ⇒ 剔除候选
    bool reject_clip_top = false;         // 上边贴裁剪区边界 ⇒ 剔除（默认关，近身仰角目标易误伤）
    float clip_margin_px = 6.0f;          // 框边距裁剪区边界多远算"被切断"
    float clip_center_max_px = 105.0f;    // 离准星超过这个距离才判贴边
    float track_size_ratio = 1.35f;       // 锁定换块的框高比上限（0 = 关）
                                          // ★ V1.0.11：2.0 → 1.35。2.0 太宽（实测尖峰帧
                                          //   框高比 p90=1.86 ⇒ 92.3% 的坏量测被放行）；
                                          //   实际生效值另有硬上限 kSizeRatioCap=1.35
                                          //   （见 TargetSelector.cpp）—— 配置只能更严。
    // ---- 选靶四项机制（对齐 BB 目标选择/锁定，见 bb-port/01 §1）----
    // ★ lock_hold_ms 默认 1500（对齐 BB lock_hold_time，新骨架出厂即生效）；
    //   其余默认 0/false ⇒ 不开时选靶行为与本参数加入前逐字节一致（1.5.46 兼容）。
    //   对应 TargetSelectorConfig 里的同名字段，由 AimThread 逐帧灌进 scfg。
    //   此前这四项只在 TargetSelector 里实现了算法、**没有配置通路**（AimThread 未赋值、
    //   JSON 无键）⇒ 永远吃默认值、面板也开不了。2026-09-24 补齐通路。
    float lock_hold_ms = 1500.0f;       // 锁定保持窗：期内只刷新位置、不换目标（对齐 BB lock_hold_time=1500；0=关）
    bool priority_scoring = false;      // true=打分制选靶（距离+尺寸+粘滞），false=最近优先
    float weight_dist = 1.0f;           // 打分制：距离项权重
    float weight_size = 0.3f;           // 打分制：尺寸项权重
    float stickiness = 1.0f;            // 打分制：粘滞权重（进公式时 ×0.5）
    float switch_threshold_px = 60.0f;  // stick 判定半径（px）
    bool head_body_stable = false;      // 头身稳定过滤总开关（头身同框时删头框，只留身体）
    int hb_body1 = 0;                   // 组合1：身体类
    int hb_head1 = 1;                   // 组合1：头类（命中即删）
    int hb_body2 = -1;                  // 组合2：身体类（-1 = 该组不启用）
    int hb_head2 = -1;                  // 组合2：头类
    // V3 阶段 3a：滤波强度按框高自适应（默认关 ⇒ 与加入前逐字节一致）。
    BoxAdaptiveFilterConfig box_adaptive;
    LockConfirmConfig lock_confirm;                 // 目标锁定确认（ENTER/HOLD + instant-enter，第2项）
    // A11 标定闭环：calibrating 强制 AIMING；calibration_bias_* 把准星带到偏置位再拉回
    bool calibrating = false;                   // 标定模式（自瞄全程输出，用偏置测闭环响应）
    float calibration_bias_x = 0.0f;            // 标定偏置 px（加在参考点上，自瞄自动拉到该点）
    float calibration_bias_y = 0.0f;
};

// ---- 瞄准档位查询 ----
// MouseProfile.aim_profiles 非空是**结构体不变量**（默认自带一档；解析时若 JSON 无数组
// 则用平铺老 key 合成第 0 档）。下面几个查询仍各自做一次兜底，防手工构造的 profile 越界。

// 生效档数。数组意外为空时按 1 算（= 用默认档）。
// ★ 2026-10-04 代码体检：本函数**生产代码零引用**（全仓只有这一定义处）。
//   属预留便捷函数，保留不删（删除是业主的决定）；标注以免下次重复排查。
inline size_t aim_profile_count(const MouseProfile& p) {
    return p.aim_profiles.empty() ? 1u : p.aim_profiles.size();
}

// 按索引取档。数组为空时给静态默认档，越界时给最后一份 —— 都是 fail-safe 读取，不崩。
inline const AimHotkeyProfile& aim_profile_at(const MouseProfile& p, size_t idx) {
    static const AimHotkeyProfile kFallback{};
    if (p.aim_profiles.empty()) return kFallback;
    if (idx >= p.aim_profiles.size()) return p.aim_profiles.back();
    return p.aim_profiles[idx];
}

// 所有档位的键位并集 —— 输出放行闸门用。
// ★ 闸门必须用并集：选档只认命中的那一档，但 usb 报告什么时候来取决于玩家按了哪个键。
//   闸门若只看某一档，其它档的键位会被整条拦掉，表现为「换个键就不瞄了」。
inline uint16_t aim_hotkey_mask(const MouseProfile& p) {
    uint16_t m = 0;
    for (const auto& ap : p.aim_profiles) {
        m |= static_cast<uint16_t>(ap.hotkey) | static_cast<uint16_t>(ap.hotkey2);
    }
    return m;
}

// 选档：返回命中的档索引，无档命中返回 -1（等价于老代码里的「没按热键」）。
// ★ 命中即返回 ⇒ **面板顺序优先**。单键按下时「键位互斥」保证只有一个档命中，顺序无所谓；
//   多键同按（左键+右键）可能多档都命中，这时取数组中靠前的那一档 ——
//   见 AimHotkeyProfile 处的说明：互斥挡不住多键同按，必须靠顺序兜底。
inline int aim_profile_match(const MouseProfile& p, uint16_t buttons) {
    for (size_t i = 0; i < p.aim_profiles.size(); ++i) {
        if (aim_hotkey_profile_hit(p.aim_profiles[i], buttons)) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace ttbox::core::aim
