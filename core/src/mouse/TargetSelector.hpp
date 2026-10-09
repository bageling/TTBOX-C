// TargetSelector.hpp — A10 目标选择器（单层打分 + 单一“还是他吗”判定 + 锁定保持）
//
// 全部在 ROI/crop 坐标系内选择目标（不恢复到全帧）。
// 选择流程（2026-10-08 起替换旧的三层瀑布骨架）：
//   1) collect_candidates：置信度/类别过滤 + FOV/贴边剔除 + prefer_humanoid 排序；
//   2) 单一“还是他吗”判定（照 yey）：同类别 且 IoU ≥ 0.6 ⇒ 还是同一个目标，
//      继续瞄（量测跳变门控 + 平滑更新）；
//   3) 不是 ⇒ 锁定保持：窗口内输出最后位置（不返回 invalid），超时才放弃；
//   4) 单层打分（照 BB-828 calcPriority）：dist + size + stick，取分最高者。
// 丢失宽限：目标短时丢失（≤ lock_hold_ms）保持 track 与 target_id 不切换。
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
    float fov_range = 1.0f;          // 0~1；搜索半径 = 基准半径 × fov_range（基准=截取区最大圆半径）
    // ★ V1.0.31 几何兜底：**高宽比下限**，低于它判为「球/道具/烟雾」不参与选靶。
    //   实测（2026-10-04 板端训练场）：球 h/w=0.88，人 h/w=1.68~3.55 ⇒ 界取 1.15。
    //   这是**兜底**，主判据仍是 class_filter（业界 sunone 默认 [0]=player）。
    //   设 <=0 关闭该兜底。
    //   ★ 实际已改为**相对判据**（prefer_humanoid，见 collect_candidates）；本字段保留
    //     供配置层兼容，select() 不再消费它。
    float min_aspect_h_over_w = 1.15f;
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
    // ---- V1.0.40（2026-10-07，移植 BB-828 :4576-4582 calculateDynamicRangeValue）----
    // **框面积 → 选靶范围动态缩放**。纯视觉拿不到角度/FOV，但**框面积是距离的代理量**：
    // 远处小目标（面积小）抖动大、落点易偏 ⇒ 用**小范围**只锁很准的；
    // 近处大目标 ⇒ 用**全范围**放开。这就是 BB 用来近似 3D 透视影响的手段。
    // 判据：area ≤ box_min_px2 ⇒ min_range_px；area ≥ box_max_px2 ⇒ search_radius_px（全范围）；
    //       中间线性插值。**默认关**（enabled=false）⇒ 行为与加此参数前逐字节一致。
    bool dynamic_range_enabled = false;
    float dynamic_range_min_px = 40.0f;    // 远处小目标用的最小范围（BB 用 40）
    float dynamic_range_box_min_px2 = 200.0f;   // 面积下界（BB 用 200）
    float dynamic_range_box_max_px2 = 1200.0f;  // 面积上界（BB 用 1200）
    float center_x = 0.5f;           // 选择中心（crop 系归一化）
    float center_y = 0.5f;
    float lost_grace_ms = 30.0f;     // 目标丢失宽限（AimStateMachine 的 LOST_GRACE 用；
                                     // select() 的锁定保持改由 lock_hold_ms 管）
    // ---- 切靶防抖（旧三层瀑布骨架，已退役）----
    // 新骨架用「单一 IoU 判定 + 锁定保持」取代了冷却/滞后/确认窗；以下字段保留供
    // 配置层兼容，select() 不再消费。
    float switch_hysteresis = 0.5f;   // 新目标需近 (1+h) 倍才允许切（旧骨架）
    float switch_cooldown_ms = 600.0f; // 切换后冷却，期间不再切（旧骨架）
    float aim_ratio_x = 0.5f;
    float aim_ratio_y = 0.2f;
    float switch_match_ratio = 0.4f; // rect_lock 匹配距离 = 目标对角 × 此比例（旧骨架）

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
    // ★ 新骨架：该“同一目标”判据已由「同类别 + IoU ≥ 0.6」取代（照 yey），
    //   size_ratio_ok 不再过滤候选。本字段仍被**量测跳变门控**当作开关读取：
    //   0 = 关门控，非 0 = 开门控（生效值仍被钳到 kSizeRatioCap=1.35）。
    float track_size_ratio = 1.35f;

    // ---- V1.0.11：开火期禁切靶（照 yu 的 fire_switch_guarded）----
    // 开火中若锁定保持已耗尽、正要另选新目标，**不再另选** ⇒ 本帧无目标。
    // 理由：压枪时切靶 = 准星从压着的目标甩到别人身上，是最难受的一种"晃"。
    // 由 AimThread 每帧填（扳机激活状态）。
    bool fire_active = false;

    // ---- V1.0.11：切靶确认窗（旧骨架，已退役）----
    // 新骨架用锁定保持取代确认窗；本字段保留供配置层兼容，select() 不再消费。
    uint32_t switch_confirm_frames = 4;

        // ---- 轨迹生命周期（多轨迹跟踪与长时间静默裁剪）----
        //   (1) track 静默超 track_buffer_frames×7ms 即删除（修"只增不删"隐患）。
        //   (2) 总轨迹数超过 max_tracks 时优先裁剪最早未命中（最长静默）轨迹。
        //   ★ 2026-10-08 修 B-2：(1) 的删除判据原为 `lost_frames > track_buffer_frames`，
        //   而 lost_frames 只在 `if (t.active)` 内自增 ⇒ track 一旦非激活就永久冻结，
        //   该判据恒不成立、僵尸轨迹永不回收。现补「距 last_seen_ms 真实时长 > buf×7ms」
        //   作为等价兜底（语义仍是"缓冲约 buf 帧"），不新增配置项、不改格式。
        //   ★ 2026-10-07 清理：删除了原「卡尔曼速度预测」三字段
        //     （kalman_velocity_gain / kalman_max_speed_px / use_kalman_predict）——
        //     实为 alpha-beta 简化滤波而非卡尔曼，且 use_kalman_predict 恒 false、
        //     预测中心 pred_cx/pred_cy 零生产消费。关联参考点恒用裸框心 cx/cy。
        uint32_t track_buffer_frames = 30;   // 丢失缓冲帧数，超过即删除轨迹
        uint32_t max_tracks = 64;            // 轨迹总上限（防内存无限增长）

        // ---- 选择器行为（第13阶段确认）----
    // 选择排序：距离FOV中心排序后，若启用 priority，则同距离段内按优先级（越大越优先）。
    // 优先级（priority）用于"同距离竞争"时优先生成新 track / 参与 score 层。
    // 新骨架里优先级只作为**同分/排序 tie-break**（打分本身不含类别优先）。
    bool priority_enabled = false;   // 是否启用优先级排序
    std::vector<int> priority_classes;      // 优先类别（优先级=1，列表内优先）
    std::vector<int> priority_classes_high; // 高优先类别（优先级=2，最优先）

    // ---- BB 对标（2026-09-24，见 bb-port/01-选靶与扳机.md §1）----
    // ★ 锁定保持窗：期内只刷新位置、不换目标。0=关（丢失即放弃）。
    //   ★ 注意：MouseTypes.hpp / RuntimeProfile 的默认仍是 0（关）——要让新骨架在板端
    //     默认开启 1500ms（对齐 BB lock_hold_time），需要另派配置层任务把默认改过来。
    float lock_hold_ms = 0.0f;          // 锁定保持窗：期内只刷新位置、不换目标（0=关；对齐 BB lock_hold_time=1500）
    // ★ 新骨架：打分恒开启，本字段仅保留供配置层兼容、不再作为开关。
    bool priority_scoring = false;      // 兼容保留（旧：true=打分制，false=最近优先）
    float weight_dist = 1.0f;           // 打分制：距离项权重（对齐 BB priority_weight_dist=1）
    float weight_size = 0.3f;           // 打分制：尺寸项权重（对齐 BB priority_weight_size=0.3）
    float stickiness = 1.0f;            // 打分制：粘滞权重（对齐 BB target_stickiness=1，进公式时 ×0.5）
    float switch_threshold_px = 60.0f;  // stick 判定半径（对齐 BB target_switch_threshold=60px）
    // ---- 打分去量纲（对齐 BB calcPriority）----
    // ① 尺寸项照 BB 原式 min(1, w×h / size_ref_px²)，size_ref_px=100 ⇒ 参考面积 10000px²。
    //    （此前 V1.0.12 的 sqrt(面积)/size_ref 与 320 参考边长已按业主口径回退为 BB 原式。）
    // ② 距离项 1/(1 + dist/dist_ref_px)，dist_ref_px=100 即 BB 原式里的 /100。
    float size_ref_px = 100.0f;     // 尺寸参考边长（px；BB 原式面积 = size_ref_px² = 10000）
    float dist_ref_px = 100.0f;     // 距离尺度（px；BB 原式里的 100）
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
    // enum class（2026-10-07 整改）：原为裸枚举，名字会泄漏到外围作用域且可隐式转 int。
    // ★ kRectLock 已随旧第 2 层退役、不再产出，但枚举值保留（test_selector_gating 等旧用例
    //   仍引用它，删了会破坏编译）。
    enum class Reason : uint8_t { kNone = 0, kTrackLock, kRectLock, kScore };
    Reason reason = Reason::kNone;
    // ---- V1.0.11 ----
    // held：本帧几何**没更新**，用的是上一帧的框（照 yu 的 *_holding_previous）。
    //   新骨架两处会置 true：① 量测跳变门控拒绝本帧量测；② 锁定保持期目标不在候选里。
    bool held = false;
    // continuity：本帧是「同一个目标的延续」而不是新目标（id 变了但框重叠 + 尺寸一致）。
    //   ★ 新骨架恒为 false：同一个人由 IoU 判定复用同一条 track ⇒ target_id 不变，
    //     上游据此无需“重新编号仍同一目标”的提示。字段保留供接口兼容。
    bool continuity = false;
};

// 单个追踪轨迹
struct TrackEntry {
    int id = -1;
    DetectionBox box;                // 输出框（量测门控冻结语义：reject 时沿用上一帧，防落点跳）
    // ★ 修复「量测门控冻结框 × IoU 判定」掉 id 隐患（fix-selector-dropid）：
    //   ref_box 是**每帧更新的参考框**，只参与「还是他吗」的 IoU 判定，不经过门控冻结。
    //   职责分离：box = 输出（可被门控 hold，保持落点稳定）；ref_box = 身份（跟着真实目标走，
    //   判断"是不是同一个人"）。若 IoU 判定也拿被冻结的 box 当参考，快移目标会因累计位移
    //   单调掉 IoU 到 0.6 以下 ⇒ 误判"换了人"⇒ 掉锁重建 id + 上游 PID reset。
    //   单帧尖峰（如 y1 跳 ±17px）对 145px 高框 IoU≈0.79 仍 >0.6，不会污染身份判定。
    DetectionBox ref_box;
    float cx = 0.0f;                 // 框中心
    float cy = 0.0f;
    uint32_t last_seen_ms = 0;       // 最后出现（外部时钟 ms）
    uint32_t lost_frames = 0;        // 连续丢失帧数
    bool active = false;             // 是否激活（锁定目标）

    // ★ 2026-10-07 清理：删除原「ByteTrack 卡尔曼状态」8 字段（kx/ky/kw/kh/vx/vy/vw/vh）
    //   与 pred_cx/pred_cy（预测中心）、hits/confirmed（命中计数/确认态）、created_ms（只写不读）。
    //   关联参考点恒用 cx/cy（裸框心），无任何速度预测。

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
// 规则：单层打分 + 单一“还是他吗”判定 + 锁定保持；被 AimThread 每帧调用
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
        last_reason_ = TargetSelection::Reason::kNone;
        next_id_ = 1;
        has_last_target_ = false;
        last_target_x_ = 0.0f;
        last_target_y_ = 0.0f;
        selector_holds_total_ = 0;
    }

    // 当前 track 表（供遥测/预览读取，只读）。
    const std::vector<TrackEntry>& tracks() const { return tracks_; }

    // V1.0.11：累计「量测被判坏 ⇒ 沿用上一帧」的帧数（遥测用，看门控在不在工作）。
    uint64_t selector_holds_total() const { return selector_holds_total_; }

    // ★ V1.0.31：本帧实际使用的 FOV 半径（像素）。预览用它画那个圆，
    //   免得"画出来的圆"和"真正约束选靶的圆"对不上（这是调试画面最常见的骗人点）。
    float last_fov_radius_px() const { return last_fov_radius_px_; }

private:
    // 从检测框列表匹配候选（过滤 + 距离排序）
    struct Candidate {
        DetectionBox box;
        float cx, cy, dist_sq;
        int priority = 0;  // 类别优先级（0=普通 1=优先 2=高优先）
        // ★ V1.0.31 相对几何判据：0=不像圆/方块（优先）1=像（后排）。
        //   只在「同一位置有多个框、且其中明显更竖长」时才置 1（见 TargetSelector.cpp）。
        //   用相对比较而非绝对 h/w 阈值：绝对阈值会误杀远处/小目标的正常人框
        //   （2026-10-04 本机回归：绝对阈值让 3 个既有测试集体变红）。
        int prefer_humanoid = 0;
    };
    // 计算类别优先级：命中 high 列表=2，命中普通列表=1，否则=0
    int class_priority(const TargetSelectorConfig& cfg, int class_id) const {
        if (!cfg.priority_enabled) return 0;
        for (int c : cfg.priority_classes_high) if (c == class_id) return 2;
        for (int c : cfg.priority_classes) if (c == class_id) return 1;
        return 0;
    }
    // 从检测框列表筛选出落在选靶范围内的候选，并打上优先级/几何标记后排序。
    std::vector<Candidate> collect_candidates(const std::vector<DetectionBox>& dets,
                                                  const TargetSelectorConfig& cfg, float cx, float cy,
                                                  float base_radius) const;
    // ★ V1.0.31 相对几何判据（Candidate 是 private 类型，故必须是成员函数）
    bool looks_like_round_object(const std::vector<Candidate>& cands,
                                  size_t self) const;
    // ★ V1.0.40（移植 BB-828 :4576）：按框面积算本帧有效选靶半径（详见 cpp 内注释）。
    float dynamic_range_px(const DetectionBox& box, const TargetSelectorConfig& cfg,
                           float full_range_px) const;

        // 轨迹生命周期：删除丢失超 buffer 的轨迹，并裁剪总轨迹数到 max_tracks 上限。
        // ★ 2026-10-08 修 B-2：新增 now_ms —— 删除判据补了「距上次命中真实时长」，
        //   原来只靠 lost_frames（该计数在 active=false 后冻结 ⇒ 判据恒不成立）。
        void trim_tracks(const TargetSelectorConfig& cfg, uint32_t now_ms);
        // 轨迹命中刷新：更新 last_seen_ms（被 trim_tracks 淘汰逻辑读）。
        void track_seen(TrackEntry& t, uint32_t now_ms);

        std::vector<TrackEntry> tracks_;
                    // ★ V1.0.31：本帧实际使用的 FOV 半径（像素）。预览画圆时直接取它，
                    //   保证"画出来的圆"与"真正约束选靶的圆"永远是同一个。
                    float last_fov_radius_px_ = 0.0f;
                    int active_track_ = -1;          // 当前激活锁定 track id
                    TargetSelection::Reason last_reason_ = TargetSelection::Reason::kNone;
                    uint32_t next_id_ = 1;
                    // ---- BB 对标状态（2026-09-24）----
                    bool has_last_target_ = false;  // last_target_x_/y_ 是否有效
                    float last_target_x_ = 0.0f;    // 上帧选中瞄准点（打分制 stick 项用）
                    float last_target_y_ = 0.0f;
                    // 门控遥测：累计 holding_previous 帧数（供 status 暴露，看门控有没有工作）
                    uint64_t selector_holds_total_ = 0;
        };

}  // namespace ttbox::core::aim
