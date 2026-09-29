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
//   aim_offset_x/y = 瞄准参考点（准星）偏移，crop 系像素。
struct AimPointProfile {
    float offset_x = 0.5f;
    float offset_y = 0.5f;
    float aim_offset_x = 0.0f;  // crop 系 px（crop 中心 + 偏移 = 准星）
    float aim_offset_y = 0.0f;
    std::vector<ClassOffset> class_offsets;
    // ★ switch_delay_ms 已删（2026-09-26）：全仓只有序列化在搬它，没有任何消费者，
    //   面板也没有对应控件 —— 属于"看着像配置、其实没人读"的尸体字段。
    HeadAimConfig head_aim;     // 头部瞄准约束（第3项）
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
    // V3 阶段 2（2026-09-28）：本档**倍镜真实放大倍率**（误差角度化的分母）。
    // 语义：开这档时，同一物理偏差在画面上被放大几倍。填 1.0 = 腰射（不改变行为）。
    // ★ 训练场实测（docs/calib/range-measure-2026-09-28.json）：
    //   真实倍率 ≈ 1.44 × 镜上标称 ⇒ 2 倍填 2.87、4 倍填 5.79、6 倍填 8.67。
    //   **不能按镜子标称填**（标称 2 实际 2.87，按标称调会偏 44%）。
    // 用途：PID 输入的像素误差除以它，等价于把误差换成角度域（差一个常数 1/f_hip，已并入 kp）
    //   ⇒ kp 保持腰射标定值即可通吃各倍镜，**不需要知道绝对焦距、也不需要知道靶子高度**。
    float zoom_scale = 1.0f;
    // V3 阶段 5（2026-09-28）：本档实测 px/count（鼠标 1 count = 画面多少 px）。
    // ★ 必须按倍镜各测一次：px/count 随 f × ADS 系数变，腰射的 0.686 在 6 倍镜下不成立。
    //   填 0 = 还没测过 ⇒ 回退到 mouse.gain_y_px_per_count（腰射值）。
    //   测法同 gain 标定：训练场里固定发 N count，量画面位移 px，取 px/N。
    float gain_px_per_count = 0.0f;
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

// 拉枪曲线（pull_curve：目标距离 ≥ min_distance 时在拉枪方向附加弧线/抖动）
struct PullCurveConfig {
    bool enabled = true;
    float strength = 0.8f;       // 弧线强度（0.8）
    float jitter_px = 3.0f;      // 抖动幅度 px
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

// 拟人化（humanize：目标输出附加抖动 + 曲线平滑，用于压枪与瞄准共用）
struct HumanizeConfig {
    bool enabled = true;
    float curve_strength = 0.45f;  // 曲线混合强度
    float jitter_px = 0.25f;       // 抖动幅度 px
    float jitter_frequency = 8.0f; // 抖动频率 Hz
};

// 贝塞尔轨迹（BezierTrajectory）：把一次位移拆成多帧子位移点列
enum class BezierDirection : int {
    kRight = 0,
    kLeft = 1,
    kUp = 2,
    kDown = 3,
};

struct BezierTrajectoryConfig {
    bool enabled = false;          // 总开关（默认关，保持现有行为）
    int generation = 1;            // 1=一代固定控制点；2=二代随机弧线
    // -- 一代（path1）
    float segments = 10.0f;        // 分段基数：seg = max(5, floor(segments × min(1.5, dist/100)))
    // -- 二代（path2）
    float linear_threshold = 45.0f;  // 距离 ≤ 它就直走（省开销）
    float curvature = 0.2f;          // 曲率：弓高 = min(60, dist × curvature × 0.5)
    float peak_min = 0.2f;           // 弓高比例抽签下界
    float peak_max = 0.6f;           // 弓高比例抽签上界
    bool dir_up = true;              // 允许的弧线方向（BB 默认 up/down 开、left/right 关）
    bool dir_down = true;
    bool dir_left = false;
    bool dir_right = false;
    // -- 公共
    float min_move = 0.1f;         // 单段最小位移，小于它的段被丢弃（位移被下一段吸收）
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

// ---------------------------------------------------------------------------
// V3 阶段 5（2026-09-28）：拟人化抖动**前馈扣除**。
//
// 问题：拟人化注入的抖动是"自己发的扰动"，它会在 response_delay_ms 之后真的
//       出现在采集画面里 ⇒ PID 把它当成"目标动了"，于是反向去追 ⇒ 抖动被自己
//       抵消掉（拟人化消失），且闭环里多出一串本不该有的修正。
//
// 做法：拟人化链每帧报出"这帧注入了多少抖动（count）"，存进环形缓冲；
//       按逐帧 dt 累计到 response_delay_ms 之后，才把它乘 px/count 换算回像素、
//       **加回**控制误差（control_x/y）。PID 因此看不到自己发的抖动。
//
// ★ 为什么只扣抖动、不扣整条整形量：速度包络/制动是"故意要走的那一段位移"，
//   扣掉会让 PID 以为还没到、继续加力 ⇒ 过冲。只有随机抖动该被扣。
//
// ★ 为什么按 dt 累计而不是"固定 N 帧"：帧率会抖（板端 144fps 实测在 130~150 之间
//   漂），按帧数对齐会累积错位，前馈本身就变成高频扰动。
//
// ★ 默认 enabled=false ⇒ 与本参数加入前逐字节一致。
// ---------------------------------------------------------------------------
struct JitterFeedforwardConfig {
    bool enabled = false;           // 总开关
    float delay_ms = 0.0f;          // 落帧延迟；0 = 用 mouse.response_delay_ms（实测 51ms）
    float gain_px_per_count = 0.0f; // 本档 px/count；0 = 用热键档 gain_px_per_count，再兜底 gain_y
    float scale = 1.0f;             // 扣除比例（1.0=全额；0.5=只扣一半，留一点人味残差）
    float max_px = 40.0f;           // 单帧加回量的绝对值上限（px），防异常值把误差顶飞
};

// 压枪（recoil：按住开火键期间持续下压，补偿后坐力）
// 压枪模块行为设计（12 参数语义），输出链完全基于 TTBOX 自身：
//   压枪量在 AimThread 输出链 pull_curve 之后、deadzone 之前注入 scaled_y，
//   与 PID 输出融合后统一走 deadzone → remainder → int16 → 拟人化整形 → 热键 Gate。
// 不照搬独立 recoil 链路；默认全关，保持旧行为。
struct RecoilConfig {
    bool enabled = false;            // 总开关
    int hotkey = 0x01;               // 开火热键位掩码（1=left，复用 y_axis_fire_hotkey 语义）
    int hotkey2 = 0x00;              // 副开火热键位掩码（0=不使用）
    int hotkey_mode = 1;             // 触发方式：1=any 任一命中 2=all 同时按下
    bool only_when_target_visible = true;  // 仅有目标时才压（防空压）
    float target_lost_release_ms = 200.0f; // 目标丢失后仍压时长（ms），0=立即释放
    bool trigger_delay_enabled = false;    // 延迟触发开关（防单点误触）
    float trigger_delay_ms = 120.0f;       // 按住超过该时长才开始压（松开重新计时）
    float strength = 0.0f;           // 下压速率基准（px/s，0=不输出）
    float speed = 1.0f;              // 下压倍率（乘在 strength 上）
    bool humanize_enabled = true;    // 拟人化开关（缓入缓出 + X 轴微动）
    float humanize_curve_strength = 0.45f;  // 下压拆步缓入缓出比例
    float humanize_jitter_px = 0.25f;       // 压枪时附加 X 轴微动幅度（px）
    float humanize_jitter_frequency = 8.0f; // X 轴微动变化频率（Hz）
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
//   · 支持随枪压枪（简易 / 进阶 / 准星三路触发）。
// ---------------------------------------------------------------------------
struct Trigger2Config {
    bool enabled = false;            // 总开关
    uint8_t key1 = 0x10;             // 组合键1（BB 默认 6=侧键2 → 0x10）
    uint8_t key2 = 0x00;             // 组合键2（0 = 常满足）
    uint8_t fire_button = 0x01;      // 开火键位掩码（BB 默认 1=左键）
    bool with_aim = true;            // 附带自瞄
    bool with_crosshair = false;     // 附带准星压枪（组合键按下即生效）
    bool with_simple_recoil = false; // 附带简易压枪
    bool with_adv_recoil = false;    // 附带进阶压枪
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

// ===========================================================================
// BB 对标第二批（2026-09-24）：压枪升级 / 两代提前量 / 拟人化链 / 三个小件
//   提取来源：bb-port/02-压枪与小件.md、03-提前量与拟人化.md（只取结构与标定值，
//   实现全部 C++ 自写）。
//   ★ 统一约定：全部 enabled 默认 false。不开时 AimThread 不跑该模块，
//     输出链与本批加入前**逐字节一致**（照 ContinuousLeadConfig 的先例）。
// ===========================================================================

// ---------------------------------------------------------------------------
// 压枪三段查表引擎（BB `recoil_presets` / `getRecoilMove`，见 02 号 §1、§2.3）
//
// 与原有 RecoilConfig 的关系：
//   · RecoilConfig 是 TTBOX 原有的「速率模型」：下压速率 = strength × speed（px/s）× dt。
//   · RecoilBbConfig 是 BB 的「三段查表模型」：开火时长按 total_time 三等分，
//     每段查一组 (vert, horiz) **相对像素/帧**，再乘全局倍率，末尾叠漂移正弦与一阶平滑。
//   · 两者以 RecoilBbConfig::enabled 互斥：false（默认）⇒ 完全走原速率模型，行为零变化。
// ---------------------------------------------------------------------------
struct RecoilBbConfig {
    bool enabled = false;            // 三段查表引擎开关（false ⇒ 走原速率模型）
    int preset = 1;                  // 当前预设编号 1..3
    float preset_total_time_ms[3] = {1500.0f, 1500.0f, 1500.0f};  // 各预设总时长（ms）
    // 每预设 3 段的垂直/水平量（px/帧，vert 正=向下压，horiz 正=向右修正）。
    // 段序号按开火时长三等分：1=[0,t/3) 2=[t/3,2t/3) 3=[2t/3,∞)。
    float preset_vert[3][3] = {{1.5f, 1.5f, 1.5f}, {1.5f, 1.5f, 1.5f}, {1.0f, 1.3f, 1.6f}};
    float preset_horiz[3][3] = {{0.0f, 0.0f, 0.0f}, {-2.0f, -2.0f, -2.0f}, {0.0f, 0.0f, 0.0f}};
    float global_vert = 0.5f;        // 垂直全局倍率
    float global_horiz = 0.5f;       // 水平全局倍率
    float delay_ms = 50.0f;          // 开火后延迟多久开始压（ms）
    float smooth = 0.90f;            // 一阶平滑系数 s（0 = 不平滑）
    float distance_limit = 80.0f;    // 目标距中心超过它不压（px，≤0 = 不限）
    bool no_target_always = false;   // 无目标时也压
    bool drift_enabled = false;      // 水平漂移正弦开关
    float drift_amplitude = 0.20f;   // 漂移幅度（px）
    float drift_freq = 1.0f;         // 漂移频率（Hz）
    bool y_suppress_enabled = false; // 垂直修正 Y 路屏蔽（进阶压枪）
    float y_suppress_strength = 0.0f; // 屏蔽乘子（0 = 全屏蔽）
    float max_down_distance = 0.0f;  // 最大下压距离（px，0 = 不限）
    float adv_mult = 0.9f;           // BB 扳机开火后整体压枪倍率（默认 0.9）
};

// ---------------------------------------------------------------------------
// 垂直修正 + 力度渐变（BB `getVerticalCorrection`，见 02 号 §3.3）
// 输出是**直接叠加**进最终位移的像素量（不是乘子），可与压枪同时生效。
// ---------------------------------------------------------------------------
struct VerticalCorrectionConfig {
    bool enabled = true;             // 垂直修正开关（BB 默认 true；但外层 RecoilBbConfig.enabled 未开则整段不跑）
    bool no_target = false;          // 无目标也修正
    float strength = 1.0f;           // 垂直修正基准强度（px/帧）
    float horiz = 0.0f;              // 水平修正量（px/帧）
    float delay_ms = 0.0f;           // 起始延迟（ms，从热键按下起算）
    float max_down_distance = 0.0f;  // 最大下压距离（px，0 = 不限）
    bool y_suppress_enabled = false; // Y 路屏蔽开关（BB `auto_recoil_y_suppress_*`）
    float y_suppress_strength = 0.0f; // Y 路屏蔽乘子（0 = 全屏蔽）
    // 三档渐变互斥（都开则顺序靠前者生效）
    bool ramp1_enabled = false;      // 渐变 1
    float ramp1_duration_ms = 1300.0f;
    float ramp1_start = 1.4f, ramp1_middle = 1.6f, ramp1_end = 0.01f;
    bool ramp2_enabled = false;      // 渐变 2
    float ramp2_duration_ms = 2000.0f;
    float ramp2_start = 1.0f, ramp2_middle = 0.5f, ramp2_end = 0.1f;
    bool ramp3_enabled = false;      // 渐变 3
    float ramp3_duration_ms = 2000.0f;
    float ramp3_start = 1.0f, ramp3_middle = 0.5f, ramp3_end = 0.1f;
};

// ---------------------------------------------------------------------------
// 开火期闭环纠偏（压枪 v1，2026-09-29 业主裁定方案 A）
//
// 为什么需要（代码事实，不是设计偏好）：
//   · core/src/aim/AimThread.cpp:25 `pid_y_.init(25.0, 25.0, 0.0, 0.3, 9900.0)`；
//     第 3 个实参是 predict，而它只乘在积分通道上（Pid1Controller.hpp:80-90
//     `ki_raw = ((error_diff + last_u) * predict) * integral_gain`）
//     ⇒ predict_y = 0 把 K_i 整个乘成 0 ⇒ **默认配置下 Y 轴只有 P+D、没有 I**。
//   · 枪口上抬 ⇒ 准星不动、画面整体上移 ⇒ 目标框在画面里匀速下移（斜坡输入）。
//     斜坡只有 I 项吃得掉：P 必留稳态误差、D 在稳态下不干活
//     ⇒ **PID 结构上补不了后坐力**，这才是压枪模块存在的全部理由。
//   本模块 = 把这一环补回来：一个**只在开火期生效的闭环积分项**。
//
// 设计约束（业主 2026-09-29 定案，实现不得偏离）：
//   ① 「有实时观测就压，没有实时观测就不猜」—— 观测不成立时立即清零，
//      不留跨开火记忆（换枪/换倍镜/换节奏最怕的就是拿上次的经验去猜）；
//   ② 不需要选枪/录枪/预采数据 —— 观测量是**实时实测**的画面偏移（error_y），
//      不含任何枪械先验，枪械差异一律由实测自己表达；
//   ③ 只下压（单向）—— 积分下限钳到 0，永远不会把准星往上推。
//
// 与既有两套压枪引擎的关系：**完全独立**。RecoilConfig（老速率模型）与
// RecoilBbConfig（BB 三段查表）各自照旧；本模块默认 enabled=false ⇒ 不开时
// 输出链与加入前逐字节一致。
// ---------------------------------------------------------------------------
struct RecoilClConfig {
    bool enabled = false;            // 总开关（默认关；不开时行为零变化）
    float gain = 2.0f;               // 闭环积分增益（count / (px·s)）—— ★待实测整定
    float integral_max = 100.0f;     // 积分限幅（px·s）；实际下压量 = gain × clamp(积分)
    int start_frames = 6;            // 起压前最少连续有效观测帧数（前几发不压 = 设计内代价）
    float press_max_count = 20.0f;   // 单帧最大下压（count，安全阀）；0 = 不限
};

// 2026-09-29：提前量一代（Lead1Config，帧窗口投票法）已整段删除 —— 业主裁定
//   「提前量只留 2.0」。一代与二代原本是**相加**关系（不是替代），删掉之后二代自己
//   照常工作，只是不再有"投票法"那一份额外偏移。旧配置残留的 `mouse.lead1` 段会被忽略。

// ---------------------------------------------------------------------------
// 提前量二代：积分累积法（BB `lead2_*`，见 03 号 §2）
// integral += errorX × gain × yScale²（注意是平方）；死区内乘 decay 衰减；
// Y 轴抑制由「上一帧纵向输出」驱动（纵向输出越大，横向提前量越小 → 防斜拉抛物线）。
// ---------------------------------------------------------------------------
struct Lead2Config {
    bool enabled = false;            // 总开关
    float gain = 0.05f;              // 积分增益（1/帧）
    float max_offset = 25.0f;        // 偏移上限（±px）
    float decay = 0.95f;             // 死区内积分衰减系数（每帧 ×该值）
    float activation_distance = 100.0f; // 激活距离（px）
    float dead_zone = 1.0f;          // 误差死区（px）
    float hold_ms = 10.0f;           // 保持窗（ms）
    float cooldown_ms = 250.0f;      // 进入距离后的冷却（ms）
    bool y_suppress_enabled = true;  // Y 轴抑制开关
    float y_suppress_min = 0.5f;     // 纵向输出小于它不抑制（px/帧）
    float y_suppress_max = 2.0f;     // 纵向输出大于它完全抑制（px/帧）
};

// ---------------------------------------------------------------------------
// BB 拟人化整形链（BB `applyHumanize`，见 03 号 §3）
// 顺序固定：一阶低通 → 反应延迟 → 过冲 → 制动 → 高斯噪声。
// ★ smooth_factor 在 BB 里「无开关、>0 即生效」，本实现把默认改成 0.0f
//   （=关闭），保证"默认零变化"这条底线；要用再把值调上去。
// ★ human_rest_* 是 BB 的死功能（无人读取），按文档要求**不实现**。
// ---------------------------------------------------------------------------
struct HumanizeShaperConfig {
    bool enabled = false;            // 总开关（低通/延迟/过冲/制动/噪声全部受它控）
    float smooth_factor = 0.0f;      // 一阶低通系数（0~0.99，0 = 关闭）
    float overshoot = 0.0f;          // 过冲强度（factor = 1 + overshoot×min(1, dtt/200)）
    float brake_distance = 0.0f;     // 制动触发距离（px，0 = 关闭）
    float noise_sigma = 0.2f;        // 高斯噪声标准差（count）—— BB 原版加在位移 mx/my 上，量纲是 count
    float delay_ms = 0.0f;           // 反应延迟基准（ms）
    float delay_random_ms = 0.0f;    // 反应延迟随机幅度（±ms）
    // ★ 2026-09-28 照搬 BB 927 原版：speed_fluctuation / accuracy_sim 在 BB 里是
    //   **独立于 humanize_enabled 的开关**（main.lua:5943 / :6445 各自独立判断），
    //   不属于本结构。此前塞在这里且未接线 ⇒ 面板勾了没反应（假开关）。
    //   现已拆成 MouseProfile.speed_fluctuation / .accuracy_sim，按原版口径独立生效。
};

// ---------------------------------------------------------------------------
// BB 移动速度波动（BB `applySpeedFluctuation`，main.lua:5265 / 调用点 :5943）
// 照搬 BB 927 原版：让一次"拉过去"的动作先慢、再快、最后减速 —— 速度倍率乘在
// 位移上。progress = 1 - dtt/total_distance；起步段 / 收尾段各占一个比例。
//
// ★ 原版口径（照抄，不自己发挥）：
//   1. 它是**独立开关**，不受 humanize_enabled 管（原版 :5266 只看 speed_fluctuation_enabled）。
//   2. 只在**刚锁定目标的第一帧**生效一次（原版 :5943 传 st.speed_fluctuation_first_lock，
//      用完立刻置 false）。之后同一目标上不再作用。
//   3. 全程参考距离 td 用的是 `sqrt(Centre²+Centre²)`（瞄准范围对角线，原版 :5943）。
//
// ★ 默认 enabled=false ⇒ 与本模块加入前逐字节一致。
// ---------------------------------------------------------------------------
struct SpeedFluctuationConfig {
    bool enabled = false;                 // 独立开关（不归 humanize_enabled 管）
    float start_speed = 0.80f;            // 起步速度倍率（0.1~1.0）
    float accel_ratio = 0.20f;            // 起步段占总距离比例（0.1~0.5）
    float decel_ratio = 0.20f;            // 收尾段占总距离比例（0.1~0.5）
    float intensity = 0.15f;              // 随机波动幅度（0~0.5）
    float total_distance_px = 452.5f;     // 全程参考距离（px）= sqrt(Centre²+Centre²)，Centre=320
};

// ---------------------------------------------------------------------------
// BB 命中率随机（BB `applyAccuracySim`，main.lua:5274 / 调用点 :6445）
// 照搬 BB 927 原版：按概率把**瞄准点**推到目标框四角/边缘，复现"打不中"的手感。
//
// ★ 原版口径（照抄）：
//   1. 它是**独立开关**，不受 humanize_enabled 管。
//   2. 作用在**瞄准点**（选靶之后、进 PID 之前），不是在输出位移上加抖动
//      —— 原版 :6445 `at = applyAccuracySim(at, lp)`，改的是 locked_target。
//   3. 命中"完美"概率内不动；不完美时按方向策略取角度，偏移量 = 框半径 × 强度。
//
// ★ 默认 enabled=false ⇒ 与本模块加入前逐字节一致。
// ---------------------------------------------------------------------------
struct AccuracySimConfig {
    bool enabled = false;                 // 独立开关（不归 humanize_enabled 管）
    float perfect_rate = 90.0f;           // 完美命中概率（%，50~100）
    float offset_strength = 0.50f;        // 偏移强度（占框半径比例，0.1~1.0）
    int direction = 0;                    // 0=四角优先 1=边缘随机 2=全随机（BB 字符串同义）
};

// ---------------------------------------------------------------------------
// 抗过冲（BB `applyAntiOvershoot`，见 02 号 §5）
// 靠近中心时按百分比衰减位移；内/外圈各自"最多衰减 N 帧"，跑满即本轮不再干预；
// 准星飘到外圈以外并持续超 reset_cooldown 则整轮复位（可再来一次）。
// ---------------------------------------------------------------------------
struct AntiOvershootConfig {
    bool enabled = false;            // 总开关
    float outer_distance = 20.0f;    // 外圈半径（px）
    float outer_strength = 50.0f;    // 外圈每帧衰减强度（%）
    float inner_distance = 10.0f;    // 内圈半径（px）
    float inner_strength = 90.0f;    // 内圈每帧衰减强度（%）
    int outer_frames = 11;           // 外圈最多衰减帧数
    int inner_frames = 6;            // 内圈最多衰减帧数
    float reset_cooldown_ms = 500.0f; // 持续越界复位冷却（ms）
};

// ---------------------------------------------------------------------------
// 速度自适应 Kp（BB `getSpeedAdaptiveKpMultiplier`，见 02 号 §6）
// 滑动窗口估平均帧间位移：动目标加大 Kp（跟得紧），静目标减小 Kp（防抖）。
// ★ 输出是**乘子**，由 AimThread 在每帧 PID 计算前临时乘到 kp 上、算完还原。
// ---------------------------------------------------------------------------
struct SpeedAdaptiveKpConfig {
    bool enabled = false;            // 总开关
    float move_mult = 1.5f;          // 移动时 Kp 乘子
    float static_mult = 0.8f;        // 静止时 Kp 乘子
    float threshold = 3.0f;          // 平均速度阈值（px/帧）
    int frames = 5;                  // 滑动窗口帧数
};

// ---------------------------------------------------------------------------
// 全局正弦扰动（BB `applyGlobalWave`，见 02 号 §4）
// 同一相位驱动 X/Y，各自乘振幅并做一阶平滑，叠加到每帧最终位移上。
// ---------------------------------------------------------------------------
struct GlobalWaveConfig {
    bool enabled = false;            // 总开关
    float amp_x = 0.10f;             // X 振幅（px）
    float amp_y = 0.10f;             // Y 振幅（px）
    float freq = 1.0f;               // 频率（Hz）
    float smooth = 0.50f;            // 一阶平滑系数（0 = 不平滑）
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
    // PID 默认值以用户提供的 pid1.cpp 权威参数为准（X: kp=25 kd=25 predict=3 rate=0.3；Y: predict=0）
        float kp_x = 25.0f;                         // X 比例增益（P 控制）
        float kp_y = 25.0f;
        float kd_x = 25.0f;                         // 微分增益（pid1 刹车，防过冲）
        float kd_y = 25.0f;
    // A10.1：FOV 角度换算模式（参考 PD Aim fov 算法，可选）
    bool fov_mode = false;                      // true = 角度换算输出（替代 kp×err）
    float hfov = 83.105f;                       // 水平视场角（度）
    float vfov = 53.0f;                         // 垂直视场角（度）
    float move_speed_x = 500.0f;                // X 每整圈移动像素（角度换算）
    float move_speed_y = 500.0f;                // Y 每整圈移动像素（角度换算）
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
    float predict_x = 3.0f;                   // pid1 X 前馈 3.0（追左右移动目标）
        float predict_y = 0.0f;                   // pid1 Y 不带前馈（仅位置纠正）
    float smooth_x = 9900.0f;                   // smooth 参考值（9900≈不过滤；TTBox 用 smooth 0~1 兼容）
    float smooth_y = 9900.0f;
    float output_deadzone = 1.0f;               // output_deadzone（自适应死区基准）
    // 插件配置（pull_curve / continuous_lead / recoil / personal_motion / personal_trajectory）
        PullCurveConfig pull_curve;
        PersonalMotionConfig personal_motion;
        PersonalTrajectoryConfig personal_trajectory;  // 拟人化整形引擎（Fitts 时长+包络+垂直抖动+自适应抑制+安全守卫）
        JitterFeedforwardConfig jitter_feedforward;  // V3 阶段 5：拟人化抖动前馈扣除（默认关）
        // 持续提前量：AI 输出持续同向累计超 enter 后附加 X 偏置（渐入渐出）。
        // ★ 默认 enabled=false ⇒ 未显式开启时输出链与本参数加入前逐字节一致（行为零变化）。
        // 此前 ContinuousLeadConfig 与 ContinuousLead.hpp 早已存在且有单测，但
        // **本结构体缺该成员、AimThread 从未调用** ⇒ 签名/面板都无从配置（M2 补齐）。
        ContinuousLeadConfig continuous_lead;
    RecoilConfig recoil;                    // 压枪（输出链 pull_curve 后、deadzone 前注入 scaled_y）
    // ---- BB 对标第二批（2026-09-24）----
    // 全部默认 false ⇒ 不开时 AimThread 不跑这些模块，输出链与本批加入前逐字节一致。
    RecoilBbConfig recoil_bb;               // 压枪三段查表引擎（recoil.enabled 且 recoil_bb.enabled 才走）
    VerticalCorrectionConfig vertical_correction;  // 垂直修正 + 力度渐变（叠加进最终位移）
    RecoilClConfig recoil_cl;               // 开火期闭环纠偏（压枪 v1，2026-09-29；默认 enabled=false）
    Lead2Config lead2;                      // 提前量二代（积分累积，X 轴）；一代 2026-09-29 已删
    HumanizeShaperConfig humanize;          // BB 拟人化整形链（替换 personal_shader 调用点）
    // BB 927 原版里这两个是**独立开关**，不归 humanize.enabled 管（照搬，2026-09-28）。
    SpeedFluctuationConfig speed_fluctuation;  // 移动速度波动（作用在位移上，拟人化之前）
    AccuracySimConfig accuracy_sim;            // 命中率随机（作用在瞄准点上，选靶阶段）
    AntiOvershootConfig anti_overshoot;     // 抗过冲状态机
    SpeedAdaptiveKpConfig speed_adaptive_kp; // 速度自适应 Kp（临时乘子）
    GlobalWaveConfig global_wave;           // 全局正弦扰动
    // ---- 自动扳机（BB 对标，2026-09-24 移植）----
    // 两套状态机互相独立，可分别开启；都只产出"要开火"的决策（TriggerCmd），
    // 真正的点击由 AimThread 拿到决策后调 output->mouse_click 注入 —— 决策与注入分离。
    // 默认 enabled=false ⇒ 不开时 AimThread 不跑扳机，输出链与加入前逐字节一致。
    Trigger2Config trigger2;                // BB 扳机 2.0（v7.26 已删，2026-09-29）
    // ---- 贝塞尔弧线（2026-09-26 接线）----
    // 此前 `BezierTrajectory` 与它的配置**三层全死**：无人 include、MouseProfile 无成员、
    // 配置不解析。接线走 HEX(`safety.lua`) 验证过的 **warp 用法**：在**误差域**加一个
    // 垂直于误差方向的弧线偏移（逐帧生效、不跨帧拆队列）⇒ 不破坏 PID 闭环。
    // （模块里 path1/path2 的"拆点列逐帧发"是 BB 原版用法，留给开环拉枪场景，本次不接。）
    BezierTrajectoryConfig bezier;
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
    // ---- 选靶四项机制（对齐 BB 目标选择/锁定，见 bb-port/01 §1）----
    // ★ 默认 0/false ⇒ 不开时选靶行为与本参数加入前**逐字节一致**（1.5.46 兼容）。
    //   对应 TargetSelectorConfig 里的同名字段，由 AimThread 逐帧灌进 scfg。
    //   此前这四项只在 TargetSelector 里实现了算法、**没有配置通路**（AimThread 未赋值、
    //   JSON 无键）⇒ 永远吃默认值、面板也开不了。2026-09-24 补齐通路。
    float lock_hold_ms = 0.0f;          // 锁定保持窗：期内只刷新位置、不换目标（0=关）
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
