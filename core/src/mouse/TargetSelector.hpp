// TargetSelector.hpp — A10 目标选择器（多目标追踪 + 分层选择）
//
// 全部在 ROI/crop 坐标系内选择目标（不恢复到全帧）。
// 目标选择层级：
//   track_lock → rect_lock/continuity → score
//   （1）selector_track_lock：候选存在与上一帧相同 track_id 的目标
//   （2）selector_rect_lock：track id 变但候选矩形与锁定目标位置匹配（同一目标被重编号）
//   （3）selector_score：无既有锁定，从候选集按评分选最优（距离+尺寸综合）
// 丢失宽限：锁定目标短时丢失（≤ lost_grace_ms）保持 track 不切换，宽限耗尽才切/放弃。
// 自适应锁定半径：lock_radius = max(1.0, 0.06 × 框宽)（对齐参考公式）。
/*
 * TTBOX 文件说明
 *
 * 文件：TargetSelector.hpp
 *
 * 作用：
 *   目标选择器的定义。
 *
 * 小白理解：
 *   从多个检测结果中选择一个最佳目标。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#pragma once

#include <cstdint>
#include <vector>

#include "common/Types.hpp"
#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

// 目标选择配置（由 MouseProfile + ROI 尺寸派生，运行时组装）
struct TargetSelectorConfig {
    float fov_range = 1.0f;          // 0~1；搜索半径 = min(roi_w, roi_h) / 2 × fov_range
    float confidence = 0.25f;        // 置信度阈值
    std::vector<int> class_filter;   // 空 = 全部保留
    uint32_t roi_w = 0;              // ROI/crop 宽（DetectionBox 所在坐标系）
    uint32_t roi_h = 0;
    // 搜索半径基准（px，与 DetectionBox 同坐标系；0 = 回退 min(roi_w, roi_h) / 2）。
    // 为什么单独留一个字段：roi_w/roi_h 同时用于计算 FOV 中心（cx = roi_w × center_x），
    // 而框坐标是**整帧**坐标系（AimThread 传 task.frame_width/height）——把 roi_w/roi_h
    // 换成截取尺寸会让中心错位到帧的左上角。
    // 而业主口径要求瞄准范围 = **截取尺寸内划最大的圆形**，半径必须取截取尺寸：
    // 整帧 min(2560,1440)/2 = 720px 已经大于检测区半宽（640/2 = 320px），
    // 圆比检测区还大 ⇒ 这条范围约束形同没写。故半径单独走这个字段。
    float search_radius_px = 0.0f;
    float center_x = 0.5f;           // 选择中心（crop 系归一化）
    float center_y = 0.5f;
    float lost_grace_ms = 30.0f;     // 目标丢失宽限（对齐参考 selector_lost_grace_ms=30）
    // ---- 切靶防抖（对齐 BB target_switch_hysteresis=50 / switch_cooldown=600ms）----
    // 只在「失去锁定、正要另选目标」时生效（第 1 层 track_lock 仍保持锁定，不经过这里）。
    // 两者为 0 时行为与加入前一致（旧用例兼容）。
    float switch_hysteresis = 0.5f;   // 新目标需近 (1+h) 倍才允许切（平方比较，省 sqrt）
    float switch_cooldown_ms = 600.0f; // 切换后冷却，期间不再切
    float aim_ratio_x = 0.5f;
    float aim_ratio_y = 0.2f;
    float switch_match_ratio = 0.4f; // rect_lock 匹配距离 = 目标对角 × 此比例

    // ---- V1.0.07：贴裁剪区边界的候选剔除 ----
    // 背景（2026-09-29 实机定障）：模型只看得到 capture 裁剪区（板端 640×640 居中），
    // 画面外的人只有一小条落进裁剪区 ⇒ 检测框被"切"成瘦长条，且框边正压在裁剪区边界上
    //（实测 t=3484.41：x1=1577 x2=1600，宽 23px 高 145px，右边界 1600 = 裁剪区右缘）。
    // 这种框的瞄准点算出来离准星 300px，被选中就"猛拉一把"。
    // 量化（业主实测段 t=3180~3610，瞄准中单帧鼠标位移）：
    //   贴裁剪边界的帧 n=15  p50=15 / p90=27 / max=28
    //   非贴边的帧     n=2066 p50=0  / p90=3
    //   ⇒ 贴边帧只占 0.7%，却贡献了全部位移尖峰。
    // 只判**左右两条竖边 + 上边**，不判下边：近身目标的腿被裁剪区下边截掉是常态
    //（实测 9 帧这种大框位移只有 -1~-6 count），判了会误伤近战。
    // 裁剪区左右边界由 search_radius_px 推出：capture 与画面同中心，半宽 = 该半径
    //（AimThread 用 min(capture.w, capture.h)/2 填）⇒ 不引入新的配置通路。
    bool reject_clip_horizontal = true;  // 左/右贴裁剪区边界 ⇒ 剔除该候选
    bool reject_clip_top = false;        // 上边贴裁剪区边界 ⇒ 剔除（默认关，近身仰角目标易误伤）
    float clip_margin_px = 6.0f;         // 框边距裁剪区边界多远算"被切断"
    float clip_center_max_px = 105.0f;   // 离准星超过这个距离才判贴边（近处的框豁免）

    // ---- V1.0.07：锁定跟踪的尺寸一致性 ----
    // 背景：同一目标的框高相邻帧不会突变，但模型有 cls5 / cls0 两套"人体"输出，互相切换时
    // 框高差 3~14 倍 ⇒ 落点按 offset_y×Δh 跳 60~200px。
    // 实测跨类别 |Δtgt_y| p90=64 / p99=159，同类别 p90=10（类别跳变 94% 都是 cls5↔cls0）。
    // 第 1 层 track_lock 此前只比"谁离上帧锁定框中心近"，不看尺寸 ⇒ 整块换掉。
    // 框高比 max(新/旧, 旧/新) > 此值的候选不参与竞争。0 = 关闭（回到加此参数前行为）。
    // ★ V1.0.11：默认 2.0 → 1.35。2.0 太宽 —— 实测（板端 34 万帧）落点尖峰帧的框高比
    //   p50=1.43 / p90=1.86，**92.3% 落在 2.0 以内 ⇒ 全被放行**。
    //   另外 .cpp 里对生效值加了硬上限 kSizeRatioCap=1.35（配置只能调得更严，不能放宽），
    //   这样板端存量配置里写的 2 会被自动钳到 1.35，不必改板端 config.d。
    float track_size_ratio = 1.35f;

    // ---- V1.0.11：开火期禁切靶（照 yu 的 fire_switch_guarded）----
    // 开火中若第 1 层没保住锁定，**不再去第 2/3 层另选目标** ⇒ 本帧无目标。
    // 理由：压枪时切靶 = 准星从压着的目标甩到别人身上，是最难受的一种"晃"。
    // 由 AimThread 每帧填（扳机激活状态）。
    bool fire_active = false;

    // ---- V1.0.11：切靶确认窗（照 yu 的 pending_switch_frames + selector_acquire_delay_ms）----
    // 只在「本来锁着一个目标 → 掉了 → 正要另选」**且候选不止一个**时生效：
    // 新目标要在同一处连续出现这么多帧才真正切过去（首次锁定、单候选瞬移都不延迟）。
    // 用帧数而不是毫秒 —— 照 yu 的 pending_switch_frames，且不受调用方时钟口径影响。
    // 0 或 1 = 关（回到加此机制前行为，供测试 / A-B）。
    // ★ 不进面板：这是算法内部状态，不是用户旋钮（对齐 yu 口径）。
    uint32_t switch_confirm_frames = 4;

        // ---- ByteTrack 增强（多轨迹跟踪与长时间静默裁剪）----
        // 在保持原有 track_lock/rect_lock/score 三层选择语义不变的前提下，
        // 为轨迹增加"卡尔曼速度预测 + 轨迹生命周期"：
        //   (1) association 参考点用 Kalman 预测框（非裸框心），
        //       慢速/匀速目标在测试中预测≈当前 → 行为兼容旧用例。
        //   (2) track 超 lost_frames 达 track_buffer_frames 即删除（修"只增不删"隐患）。
        //   (3) 总轨迹数超过 max_tracks 时优先裁剪最早未命中（最长静默）轨迹。
        float kalman_velocity_gain = 0.30f;  // 速度学习增益（0.22+q*8≈0.30）
            float kalman_max_speed_px = 25.0f;   // 预测速度上限（px/帧），防抖预测过度
            bool use_kalman_predict = false;      // 是否用卡尔曼预测中心做关联参考点
                                                  // 默认关：保持"裸框心最近邻"传统行为（109 用例兼容）。
                                                  // 开启时：匀速/快速目标用预测中心，抗遮挡/快速移动更稳，
                                                  // 但会改变关联参考点（需真机调参验证后再启用）。
            uint32_t track_buffer_frames = 30;   // 丢失缓冲帧数，超过即删除轨迹
            uint32_t max_tracks = 64;            // 轨迹总上限（防内存无限增长）

        // ---- 选择器行为（第13阶段确认）----
    // 选择排序：距离FOV中心排序后，若启用 priority，则同距离段内按优先级（越大越优先）。
    // 优先级（priority）用于"同距离竞争"时优先生成新 track / 参与 score 层。
    // 未启用（默认 0）时完全保持原有"距离最近优先"行为，兼容旧测试。
    bool priority_enabled = false;   // 是否启用优先级排序
    std::vector<int> priority_classes;      // 优先类别（优先级=1，列表内优先）
    std::vector<int> priority_classes_high; // 高优先类别（优先级=2，最优先）

    // ---- BB 对标（2026-09-24，见 bb-port/01-选靶与扳机.md §1）----
    // ★ 全部默认 0/false ⇒ 不开启时行为与本参数加入前**逐字节一致**（1.5.46 兼容）。
    //   铁律：新机制一律出厂关，要在面板上显式打开才生效。
    float lock_hold_ms = 0.0f;          // 锁定保持窗：期内只刷新位置、不换目标（0=关；对齐 BB lock_hold_time=1500）
    bool priority_scoring = false;      // true=打分制选靶（dist+size+stick），false=最近优先（现行为）
    float weight_dist = 1.0f;           // 打分制：距离项权重（对齐 BB priority_weight_dist=1）
    float weight_size = 0.3f;           // 打分制：尺寸项权重（对齐 BB priority_weight_size=0.3）
    float stickiness = 1.0f;            // 打分制：粘滞权重（对齐 BB target_stickiness=1，进公式时 ×0.5）
    float switch_threshold_px = 60.0f;  // stick 判定半径（对齐 BB target_switch_threshold=60px）
    // ---- V3 阶段 4（2026-09-28）：打分去量纲 ----
    // ① 尺寸项由 面积/10000 改为 sqrt(面积)/参考边长 —— 原式在 640 窗口里
    //    2 倍镜起就全部撞顶（实测 70×243=17086 ≫ 10000），等于没配。
    // ② V1.0.12（2026-09-30）：原先「两项都除以本档真实倍率 M」已删除
    //    （业主口径不区分倍镜）。打分回像素原值，所有档位共用一套权重。
    float size_ref_px = 320.0f;     // 尺寸参考边长（默认 = 640 截取窗口的一半）
    float dist_ref_px = 100.0f;     // 距离尺度（腰射等效 px，原式里的 100）
    // 头身稳定过滤：同一帧里同时出现 (bodyN + headN) 时删掉 headN 框，
    // 理由——头身同框时"头部框"容易把瞄准点抢走，只留身体框更稳（对齐 BB applyHeadBodyStable）。
    bool head_body_stable = false;      // 总开关（默认关）
    int hb_body1 = 0;                   // 组合1：身体类（BB head_body_body1=0）
    int hb_head1 = 1;                   // 组合1：头类（BB head_body_head1=1，命中即删）
    int hb_body2 = -1;                  // 组合2：身体类（-1 = 该组不启用）
    int hb_head2 = -1;                  // 组合2：头类
};

// 选择结果
struct TargetSelection {
    bool valid = false;
    DetectionBox box;
    int target_id = -1;              // 稳定追踪 id（track_id）
    float distance = 0.0f;           // 到选择中心的距离（px）
    float lock_radius = 0.0f;        // 自适应锁定半径（px）
    // 选择层级 reason（对齐参考 trace reason）
    enum Reason { kNone = 0, kTrackLock, kRectLock, kScore } reason = kNone;
    // ---- V1.0.11 ----
    // held：本帧几何**没更新**，用的是上一帧的框（照 yu 的 *_holding_previous）。
    //   上游据此可以知道"这一帧的量测被判为坏"，用于遥测与调试。
    bool held = false;
    // continuity：本帧是「同一个目标的延续」而不是新目标（id 变了但框重叠 + 尺寸一致）。
    //   上游据此**不重置**平滑器/PID（避免落点跳与重新起步）。
    bool continuity = false;
};

// 单个追踪轨迹
struct TrackEntry {
    int id = -1;
    DetectionBox box;
    float cx = 0.0f;                 // 框中心
    float cy = 0.0f;
    uint32_t last_seen_ms = 0;       // 最后出现（外部时钟 ms）
    uint32_t lost_frames = 0;        // 连续丢失帧数
    bool active = false;             // 是否激活（锁定目标）

    // ---- ByteTrack 卡尔曼状态（第4项）----
    // 匀速模型 [cx, cy, w, h, vx, vy, vw, vh]，关联时用预测中心 (px, py)。
    float kx = 0.0f, ky = 0.0f;      // 卡尔曼平滑位置
    float kw = 0.0f, kh = 0.0f;      // 卡尔曼平滑尺寸
    float vx = 0.0f, vy = 0.0f;      // 速度（px/帧）
    float vw = 0.0f, vh = 0.0f;      // 尺寸变化率（px/帧）
    float pred_cx = 0.0f, pred_cy = 0.0f;  // 预测中心（关联参考点）
    uint32_t hits = 0;               // 累计命中帧数
    bool confirmed = false;          // 是否已确认（hits 达标）
    uint32_t created_ms = 0;         // 创建时间（用于存在时长排序，裁剪最旧）

    // ---- V1.0.11：量测跳变门控状态（照 yu 的 jump_rejected_holding_previous）----
    // d_hist：最近 4 次**被接受**的帧间位移（px）。用中位数当"局部速度"基准 ——
    //   单帧噪声不会抬高基准（均值会被尖峰污染），真运动则基准随之上抬、阈值自动放宽。
    // hold_frames：连续 holding_previous 的帧数；到 kMeasMaxHold 就必须认输放行（防锁死）。
    // rejected_total：累计拒绝帧数（遥测用，看门控有没有在工作）。
    float d_hist[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8_t d_hist_n = 0;
    uint32_t hold_frames = 0;
    uint32_t rejected_total = 0;
};

// TargetSelector — 目标选择器：从多个检测框(DetectionBox)中挑出唯一要跟踪的目标。
// 输入：检测框列表 + 当前时钟(now_ms)
// 输出：TargetSelection（选中的目标：类别/位置/锁定状态）
// 规则：多帧稳定防跳变 + 类别过滤 + 丢失宽限；被 AimThread 每帧调用
class TargetSelector {
public:
    // 有状态选择：内部维护多目标 track 表。
    // now_ms = 当前毫秒时钟（用于丢失宽限判定）。
    // 返回选择结果（valid=false 表示无目标）。
    TargetSelection select(const std::vector<DetectionBox>& dets,
                           const TargetSelectorConfig& cfg, uint32_t now_ms = 0);

    // 最近一次选择的 reason（供外部观测）
    TargetSelection::Reason last_reason() const { return last_reason_; }

    // 重置所有 track（目标切换/模型切换/瞄准退出时）。
    // 新运行世代的 track id 从 1 重新开始，禁止继承旧模型身份空间。
    void reset() {
        tracks_.clear();
        active_track_ = -1;
        last_reason_ = TargetSelection::kNone;
        next_id_ = 1;
        has_switch_ = false;
        last_switch_ms_ = 0;
        last_locked_dist_sq_ = -1.0f;
        lock_track_id_ = -1;
        lock_start_ms_ = 0;
        has_last_target_ = false;
        last_target_x_ = 0.0f;
        last_target_y_ = 0.0f;
        // V1.0.11：切靶确认窗 + 上一帧输出框 + 门控遥测一并清零
        has_pending_ = false;
        pending_cx_ = 0.0f;
        pending_cy_ = 0.0f;
        pending_frames_ = 0;
        has_last_out_box_ = false;
        selector_holds_total_ = 0;
    }

    const std::vector<TrackEntry>& tracks() const { return tracks_; }

    // V1.0.11：累计「量测被判坏 ⇒ 沿用上一帧」的帧数（遥测用，看门控在不在工作）。
    uint64_t selector_holds_total() const { return selector_holds_total_; }

private:
    // 从检测框列表匹配候选（过滤 + 距离排序）
    struct Candidate {
        DetectionBox box;
        float cx, cy, dist_sq;
        int priority = 0;  // 类别优先级（0=普通 1=优先 2=高优先）
    };
    // 计算类别优先级：命中 high 列表=2，命中普通列表=1，否则=0
    int class_priority(const TargetSelectorConfig& cfg, int class_id) const {
        if (!cfg.priority_enabled) return 0;
        for (int c : cfg.priority_classes_high) if (c == class_id) return 2;
        for (int c : cfg.priority_classes) if (c == class_id) return 1;
        return 0;
    }
    std::vector<Candidate> collect_candidates(const std::vector<DetectionBox>& dets,
                                                  const TargetSelectorConfig& cfg, float cx, float cy,
                                                  float radius_sq) const;

        // ---- ByteTrack 辅助（第4项）----
        // 用卡尔曼匀速模型预测轨迹下一帧中心（写入 pred_cx/pred_cy），关联参考点。
        void kalman_predict(TrackEntry& t, const TargetSelectorConfig& cfg) const;
        // 用观测框更新卡尔曼状态（位置平滑 + 速度学习），并重算预测中心。
        void kalman_update(TrackEntry& t, const DetectionBox& obs, const TargetSelectorConfig& cfg,
                           uint32_t now_ms);
        // 轨迹生命周期：删除丢失超 buffer 的轨迹，并裁剪总轨迹数到 max_tracks 上限。
        void trim_tracks(const TargetSelectorConfig& cfg);

        std::vector<TrackEntry> tracks_;
                    int active_track_ = -1;          // 当前激活锁定 track id
                    TargetSelection::Reason last_reason_ = TargetSelection::kNone;
                    uint32_t next_id_ = 1;
                    // ---- 切靶防抖状态 ----
                    // has_switch_ 独立于 last_switch_ms_：后者为 0 是合法时刻（now_ms 可能就是 0），
                    // 用 ">0" 判断"是否发生过切换"会漏掉这一种情况。
                    bool has_switch_ = false;
                    uint32_t last_switch_ms_ = 0;       // 上次切换（选中新目标）的时刻
                    float last_locked_dist_sq_ = -1.0f; // 刚失去的锁定目标的距离平方（<0 表示无）
                    // ---- BB 对标状态（2026-09-24）----
                    int lock_track_id_ = -1;        // 当前锁定的 track id（lock_hold 判据）
                    uint32_t lock_start_ms_ = 0;    // 该锁定建立时刻（用于 lock_hold_ms 窗口）
                    bool has_last_target_ = false;  // last_target_x_/y_ 是否有效
                    float last_target_x_ = 0.0f;    // 上帧选中瞄准点（打分制 stick 项用）
                    float last_target_y_ = 0.0f;
                    // ---- V1.0.11：切靶确认窗（照 yu 的 pending_switch_frames）----
                    // 只在「本来锁着一个目标、掉了、正要另选」时生效；首次锁定不延迟。
                    // 同一位置连续待够 kSwitchConfirmMs 才真切，避免切到一闪而过的错框上。
                    bool has_pending_ = false;
                    float pending_cx_ = 0.0f;
                    float pending_cy_ = 0.0f;
                    uint32_t pending_frames_ = 0;
                    // ---- V1.0.11：上一帧输出框（判 continuity：id 变了但是不是同一个目标）----
                    bool has_last_out_box_ = false;
                    DetectionBox last_out_box_;
                    uint32_t last_out_box_ms_ = 0;
                    // 门控遥测：累计 holding_previous 帧数（供 status 暴露，看门控有没有工作）
                    uint64_t selector_holds_total_ = 0;
        };

}  // namespace ttbox::core::aim
