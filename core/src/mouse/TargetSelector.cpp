// TargetSelector.cpp — A10 目标选择器实现（单层打分 + 单一“还是他吗”判定 + 锁定保持）
/*
 * TTBOX 文件说明
 *
 * 文件：TargetSelector.cpp
 *
 * 作用：
 *   从多个检测结果中选择一个最佳目标进行瞄准。
 *
 * 小白理解：
 *   AI 可能检测出 5 个目标，但一次只能瞄准一个。
 *   TargetSelector 根据一套打分规则选一个：
 *   - 距离近的目标分高
 *   - 框大（近处/可信）的目标分高
 *   - 上一帧选中的目标有“粘滞”加成
 *   选中的目标会“锁定”：只有“同类别 + 框重叠够多（IoU）”才算还是他；
 *   他短暂消失时，先把上一帧位置保持住（锁定保持），超时了才换人。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "mouse/TargetSelector.hpp"

#include <algorithm>
#include <cmath>

namespace ttbox::core::aim {

namespace {
// 框中心 X（crop 系）。
float box_center_x(const DetectionBox& b) { return (b.x1 + b.x2) * 0.5f; }
// 框中心 Y（crop 系）。
float box_center_y(const DetectionBox& b) { return (b.y1 + b.y2) * 0.5f; }

// ---- V1.0.11：量测门控 / 单一“还是他吗”判定 的阈值 ----------------------------
// 全部**写死在算法里、不进面板**（对齐 yu 口径：精细量只遥测、不暴露成旋钮）。
// 定障依据（板端 2026-09-29 完整录制 341,859 帧，见
// .workbuddy/artifacts/yu-选靶与落点抖动方案-2026-09-30.md）：
//   · 稳态（已对准）误差每秒过零 3.67 次、帧间只动 0.13px ⇒ 幅度小、频率高，就是眼睛看到的"晃"；
//   · corr(框高极差, 落点极差)=0.744，落点对 y1 敏感度 0.69（对 y2 只 0.31）⇒ 是框顶在摆；
//   · 落点尖峰（>20px，占 1.26% 帧）里 88.6% 是**突跳** ⇒ 可用连续性判据拦，不误杀跟枪；
//   · 离线回放：本门控让 p90 落点抖动 2.759 → 1.706（−38.2%），互相关滞后 0 帧。
namespace selgate {
// 绝对下限：位移小于它一律当正常（正常头顶边界抖动 ~±1px、目标慢移 ~6px/帧@144fps）
constexpr float kMeasJumpAbsPx = 8.0f;
// 相对倍数：位移要超过「局部速度」的这么多倍才算突跳
constexpr float kMeasJumpRel = 3.0f;
// 连续 hold 到这么多帧就认输放行（说明目标真的换了或真的在快移）—— 防锁死
constexpr uint32_t kMeasMaxHold = 10;
// 框高比硬上限：配置只能调得更严、不能放宽（要挡的正是"同一个目标头顶边界抖动"）
constexpr float kSizeRatioCap = 1.35f;
// 单一“还是他吗”判定（照 yey lock_target_iou_threshold=0.6）：
// 同类别 且 IoU ≥ 0.6 才算同一个人，否则视为新目标。
constexpr float kSamePersonIou = 0.6f;

// 局部速度 = 最近 4 次**被接受**位移的中位数。
// ★ 用中位数而不是均值：单帧尖峰抬不高基准（均值会被污染）；真运动时基准随之抬升，
//   阈值自动放宽 ⇒ 不需要额外的"目标在不在动"判断。
inline float local_speed(const TrackEntry& t) {
    if (t.d_hist_n == 0) return 0.0f;
    const int n = t.d_hist_n < 4 ? t.d_hist_n : 4;
    float v[4];
    for (int i = 0; i < n; ++i) v[i] = t.d_hist[i];
    for (int i = 1; i < n; ++i) {   // n<=4 的插入排序
        const float key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
        v[j + 1] = key;
    }
    return v[n / 2];
}

inline void push_speed(TrackEntry& t, float d) {
    if (t.d_hist_n < 4) {
        t.d_hist[t.d_hist_n++] = d;
    } else {
        t.d_hist[0] = t.d_hist[1];
        t.d_hist[1] = t.d_hist[2];
        t.d_hist[2] = t.d_hist[3];
        t.d_hist[3] = d;
    }
}

// 生效的框高比上限：0 保持"关"的语义；非 0 一律钳到 kSizeRatioCap 以内。
inline float effective_size_ratio(float configured) {
    if (configured <= 0.0f) return 0.0f;
    return configured < kSizeRatioCap ? configured : kSizeRatioCap;
}

// 两框交并比（IoU），用于单一“还是他吗”判定（照 yey calculateIOU）。
inline float box_iou(const DetectionBox& a, const DetectionBox& b) {
    const float ix1 = a.x1 > b.x1 ? a.x1 : b.x1;
    const float iy1 = a.y1 > b.y1 ? a.y1 : b.y1;
    const float ix2 = a.x2 < b.x2 ? a.x2 : b.x2;
    const float iy2 = a.y2 < b.y2 ? a.y2 : b.y2;
    const float iw = ix2 - ix1, ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    const float inter = iw * ih;
    const float uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

// 锁定保持窗口（ms）。cfg.lock_hold_ms ≤ 0 ⇒ 0（关，丢失即放弃）。
// ★ 无符号回绕安全：now_ms - last_seen_ms 用 uint32 相减，回绕后差值依然正确。
inline uint32_t lock_hold_window_ms(const TargetSelectorConfig& cfg) {
    return cfg.lock_hold_ms > 0.0f ? static_cast<uint32_t>(cfg.lock_hold_ms) : 0u;
}

// 框的瞄准点到选择中心的距离（px），与 collect_candidates 的 dist_sq 同一口径。
inline float aim_distance(const DetectionBox& b, const TargetSelectorConfig& cfg,
                          float cx, float cy) {
    const float ax = b.x1 + (b.x2 - b.x1) * cfg.aim_ratio_x;
    const float ay = b.y1 + (b.y2 - b.y1) * cfg.aim_ratio_y;
    return std::hypot(ax - cx, ay - cy);
}
}  // namespace selgate
}  // namespace

// ★ V1.0.31 相对几何判据：同一位置有多个框时，"明显更竖长的那个"更像人体。
//
// 为什么不用绝对高宽比阈值：实测（2026-10-04 板端训练场）球 h/w=0.88、人 h/w=1.68~3.55，
//   看着能分开，但**远处/小目标的人框同样偏扁** —— 绝对阈值一刀切会误杀正常人形
//   （本机回归当场打回：test_target_selector / test_pipeline / test_selector_clip_guard）。
//   所以改成**只在"多个框挤在一起"时比高宽比**，单框场景完全不干预。
namespace {

// 框的高宽比 h/w（宽 ≤1px 时回退 1.0，防除零）。
float aspect_h_over_w(const DetectionBox& b) {
    const float w = b.x2 - b.x1;
    const float h = b.y2 - b.y1;
    return (w > 1.0f) ? (h / w) : 1.0f;
}

// 同位置判据：中心距 ≤ 较短边的一半（局部重叠 ⇒ 多半是同一目标的不同部位）
bool near_same_spot(const DetectionBox& a, const DetectionBox& b) {
    const float acx = (a.x1 + a.x2) * 0.5f, acy = (a.y1 + a.y2) * 0.5f;
    const float bcx = (b.x1 + b.x2) * 0.5f, bcy = (b.y1 + b.y2) * 0.5f;
    const float dx = acx - bcx, dy = acy - bcy;
    const float min_side = std::min(std::min(a.x2 - a.x1, a.y2 - a.y1),
                                    std::min(b.x2 - b.x1, b.y2 - b.y1));
    const float r = min_side * 0.5f;
    return (dx * dx + dy * dy) <= (r * r);
}

}  // namespace

// 存在"同位置且明显更竖长（≥1.5×）"的框 ⇒ 自己就偏圆/方块，标 1 后排。
// （成员函数：Candidate 是 private 类型，匿名 namespace 里的自由函数碰不到它。）
bool TargetSelector::looks_like_round_object(const std::vector<Candidate>& cands,
                                            size_t self) const {
    const float mine = aspect_h_over_w(cands[self].box);
    for (size_t i = 0; i < cands.size(); ++i) {
        if (i == self) continue;
        if (!near_same_spot(cands[self].box, cands[i].box)) continue;
        if (aspect_h_over_w(cands[i].box) > mine * 1.5f) return true;
    }
    return false;
}

// ★ V1.0.40（2026-10-07）移植 BB-828 :4576-4582 calculateDynamicRangeValue：
//   **按目标框面积线性插值出本帧的有效选靶范围**。
// 为什么（物理依据）：纯视觉拿不到视角/FOV，但**框面积 = 距离的代理量**——
//   远处目标框小（且抖动大、落点易偏）⇒ 缩小选靶范围，只锁很准的；
//   近处目标框大 ⇒ 放开全范围。这在像素域内近似补偿了 3D 透视的影响。
// 判据（照 BB 原式，min/max 用其默认 40/200/1200）：
//   area ≤ box_min ⇒ min_range；area ≥ box_max ⇒ 全范围(search_radius_px)；中间线性插值。
// 返回：有效半径（px）。enabled=false 或参数非法时原样返回 search_radius_px（行为不变）。
float TargetSelector::dynamic_range_px(const DetectionBox& box,
                                       const TargetSelectorConfig& cfg,
                                       float full_range_px) const {
    if (!cfg.dynamic_range_enabled) return full_range_px;
    const float w = box.x2 - box.x1;
    const float h = box.y2 - box.y1;
    if (w <= 0.0f || h <= 0.0f) return full_range_px;
    const float area = w * h;
    const float lo = cfg.dynamic_range_box_min_px2;
    const float hi = cfg.dynamic_range_box_max_px2;
    const float min_range = std::max(1.0f, cfg.dynamic_range_min_px);
    // 参数非法（lo>=hi）时退回全范围，绝不因配置错误把选靶打死。
    if (!(hi > lo) || !(min_range <= full_range_px)) return full_range_px;
    if (area <= lo) return min_range;
    if (area >= hi) return full_range_px;
    const float t = (area - lo) / (hi - lo);
    return min_range + (full_range_px - min_range) * t;
}

std::vector<TargetSelector::Candidate> TargetSelector::collect_candidates(
    const std::vector<DetectionBox>& dets, const TargetSelectorConfig& cfg, float cx, float cy,
    float base_radius) const {
    std::vector<Candidate> out;
    // 全范围 = base_radius × fov_range（动态范围以它为"上限"，见 dynamic_range_px）。
    const float full_radius = base_radius * cfg.fov_range;
    // ---- V1.0.07：贴裁剪区边界的候选剔除（详见 TargetSelector.hpp）----
    // 裁剪区半宽 = search_radius_px（capture 与画面同中心；0 = 未知 ⇒ 不做该判定）。
    const float crop_half = cfg.search_radius_px > 0.0f ? cfg.search_radius_px : 0.0f;
    auto clipped_by_crop = [&](const DetectionBox& b) -> bool {
        if (crop_half <= 0.0f) return false;
        if (!cfg.reject_clip_horizontal && !cfg.reject_clip_top) return false;
        // 离准星近的框不判：近身目标的腿本来就常被裁剪区下边截掉，那是正常情况。
        if (std::hypot(box_center_x(b) - cx, box_center_y(b) - cy) <= cfg.clip_center_max_px) {
            return false;
        }
        const float m = cfg.clip_margin_px;
        if (cfg.reject_clip_horizontal &&
            (b.x1 <= cx - crop_half + m || b.x2 >= cx + crop_half - m)) {
            return true;
        }
        if (cfg.reject_clip_top && b.y1 <= cy - crop_half + m) return true;
        return false;
    };
    for (const auto& b : dets) {
        if (b.score < cfg.confidence) continue;
        if (!cfg.class_filter.empty()) {
            const bool in = std::find(cfg.class_filter.begin(), cfg.class_filter.end(),
                                      b.class_id) != cfg.class_filter.end();
            if (!in) continue;
        }
        // ★★ V1.0.31：这里**不做**「按绝对高宽比剔除非人」——试过，被自己的回归打回：
        //   绝对阈值（h/w < 1.15 判为球）在"远处/小目标"上不成立，一刀切会误杀正常人形
        //   （test_target_selector / test_pipeline / test_selector_clip_guard 集体变红）。
        //   改为**相对判据**，见下方 prefer_humanoid_candidate()：只在「多个框挤在同一位置」
        //   时用高宽比打破平局（球 0.88 vs 人 1.68 ⇒ 选人）；单个框一律不干预（原行为）。
        //   背景：2026-10-04 板端实测靶人站着不动、双手张开，AI 选中的却是罩在头盔位置的
        //   35×31 框（h/w=0.88）—— 按业界标准（sunone_aimbot：6=训练场的球）那就是球，
        //   而板端 class_filter 是 [0..6] 全选 ⇒ 挑中了球。详见 prefer_humanoid_candidate。
        if (clipped_by_crop(b)) continue;  // 被裁剪区切线切断的框：瞄准点不可信
        const float bdx = b.x1 + (b.x2 - b.x1) * cfg.aim_ratio_x - cx;
        const float bdy = b.y1 + (b.y2 - b.y1) * cfg.aim_ratio_y - cy;
        // ★ V1.0.40：按**本框面积**算有效范围（移植 BB-828 :4576）。
        //   逐候选判定（不是全局一个值）——大框放宽、小框收紧，与 BB 语义一致。
        const float eff_radius = dynamic_range_px(b, cfg, full_radius);
        const float eff_radius_sq = eff_radius * eff_radius;
        const float d_sq = bdx * bdx + bdy * bdy;
        if (d_sq > eff_radius_sq) continue;  // 有效范围外（框面积小 ⇒ 范围更小）
        out.push_back({b, box_center_x(b), box_center_y(b), d_sq,
                       class_priority(cfg, b.class_id)});
    }
    // ★ V1.0.31 相对几何判据打标：同位置群里有"明显更像人"的框时，把"圆/方块状"的
    //   整体后排（**只加一个排序键**，不改 priority、不改距离语义）。
    //   单框场景不产生任何影响 ⇒ 既有行为与既有测试逐字节不变。
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].prefer_humanoid = looks_like_round_object(out, i) ? 1 : 0;
    }
    // 排序：① 几何判据（prefer_humanoid 小的优先）② priority 高者 ③ 距离近者
    std::sort(out.begin(), out.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.prefer_humanoid != b.prefer_humanoid) {
                          return a.prefer_humanoid < b.prefer_humanoid;
                      }
                      if (a.priority != b.priority) return a.priority > b.priority;
                      return a.dist_sq < b.dist_sq;
                  });
        return out;
    }

    // ---- 轨迹生命周期实现 ----
    // ★ 2026-10-07 清理：原「卡尔曼速度预测」（kalman_predict / kalman_update）已删除。
    //   实测：名为卡尔曼实为 alpha-beta（无协方差 P、无增益 K、无矩阵求逆）；
    //   use_kalman_predict 恒 false ⇒ pred_cx/pred_cy 零生产消费。关联参考点恒用 cx/cy。
    //   原 kalman_update 里唯一活的是 last_seen_ms 刷新（被 trim_tracks 淘汰逻辑读），
    //   收进 track_seen。hits/confirmed 原为卡尔曼时代残留，删卡尔曼后零消费，一并删。
    void TargetSelector::track_seen(TrackEntry& t, uint32_t now_ms) {
        t.last_seen_ms = now_ms;
    }

    void TargetSelector::trim_tracks(const TargetSelectorConfig& cfg, uint32_t now_ms) {
        // 1) 删除丢失帧数超过 buffer 的轨迹（修"只增不删"隐患：轨迹有生有死）
        const uint32_t buf = cfg.track_buffer_frames > 0 ? cfg.track_buffer_frames : 30u;
        // ★ 2026-10-08 修 B-2：lost_frames 计数冻结 ⇒ 帧数判据永不成立，补时间判据。
        //   根因：cpp 里三处 `lost_frames++`（:308 / :374 / :477）全在 `if (t.active)` 内，
        //   track 一旦 active=false（宽限耗尽）计数就永久冻结在那一刻的值，再也涨不到 buf。
        //   ⇒ 原删除条件 `!active && lost_frames > buf` 是死条件，僵尸轨迹永不回收。
        //   这里用「距上次命中的真实时长」兜底，与 :315 的宽限判定同一口径（回绕安全）。
        //   buf_ms 取 buf × 7ms（一帧 ≈7ms，与本文件历史 nominal 帧周期一致）：
        //   ★ 刻意**不**用 lost_grace_ms×2 —— 两者职责不同（宽限管"还能不能锁"，
        //     缓冲管"track 留多久"），且 lost_grace_ms 允许配 0（见 test_selector_stress），
        //     若用 2×lost_grace_ms 会让"宽限=0"变成"下一帧就删 track"，把 track 复用
        //     （保 target_id 延续）整条机制打死 ⇒ 那是改选靶语义，超出本次修复范围。
        const uint32_t buf_ms = (buf > (UINT32_MAX / 7u)) ? UINT32_MAX : (buf * 7u);
        if (!tracks_.empty()) {
            tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                         [&](const TrackEntry& t) {
                                             if (t.active) return false;  // 激活中永不删
                                             if (t.lost_frames > buf) return true;
                                             // 无符号相减天然回绕安全；now==last_seen 时为 0，判据不成立。
                                             return static_cast<uint32_t>(now_ms - t.last_seen_ms) > buf_ms;
                                         }),
                          tracks_.end());
        }
        // 2) 总轨迹数超上限：裁剪最长未命中（last_seen 最旧）的非激活轨迹
        if (tracks_.size() > cfg.max_tracks) {
            // ★ 2026-10-08 修 B-1：淘汰方向 previously 反了。
            //   排序约定 = 「队首 = 最该淘汰」，删除端必须与排序端同向（erase(begin())）。
            //   原实现排序把 active 排在队首、lost_frames 多/last_seen 旧的排在队首，
            //   却用 pop_back() 删队尾 ⇒ 删掉的恰是**最健康**的非激活轨迹（丢帧少、
            //   最近还命中过），僵尸全留着。注释写的"裁剪最旧的非激活轨迹"与实现相反。
            //   现改为：非激活排前（最该淘汰者在队首），激活 track 沉到队尾 ——
            //   只有当非激活 track 全部耗尽仍超上限时，才会动到激活轨迹（硬上限兜底）。
            std::stable_sort(tracks_.begin(), tracks_.end(),
                             [](const TrackEntry& a, const TrackEntry& b) {
                                 if (a.active != b.active) return !a.active;  // 非激活在前（先淘汰）
                                 if (a.lost_frames != b.lost_frames) return a.lost_frames > b.lost_frames;  // 丢失多的先删
                                 return a.last_seen_ms < b.last_seen_ms;      // 更早未见先删
                             });
            // erase(begin()) 是 O(n)；n ≤ max_tracks+新增数（默认 64 量级）且仅超限时触发，可接受。
            while (tracks_.size() > cfg.max_tracks) tracks_.erase(tracks_.begin());
        }
    }

    TargetSelection TargetSelector::select(const std::vector<DetectionBox>& dets,
                                           const TargetSelectorConfig& cfg, uint32_t now_ms) {
    TargetSelection out;

    // ---- 安全红线：空检测帧 / 未接线 ROI → 立即返回 invalid ----
    // 不允许凭旧坐标产生移动。空帧只做轨迹生命周期维护（丢失计数 + 锁定保持超时）。
    if (dets.empty() || cfg.roi_w == 0 || cfg.roi_h == 0) {
        const uint32_t hold_ms = selgate::lock_hold_window_ms(cfg);
        for (auto& t : tracks_) {
            if (!t.active) continue;
            t.lost_frames++;
            // 锁定保持窗口耗尽才放弃激活（回绕安全）。hold_ms=0 ⇒ 丢失即放弃。
            if (static_cast<uint32_t>(now_ms - t.last_seen_ms) >= hold_ms) {
                t.active = false;
                if (active_track_ == t.id) active_track_ = -1;
            }
        }
        trim_tracks(cfg, now_ms);
        last_reason_ = TargetSelection::Reason::kNone;
        return out;
    }

    // 选择器每一帧（无论是否有检测）都执行轨迹上限裁剪：
    // 旧实现只在“无检测”分支调用，目标持续存在时轨迹数会越过 max_tracks。
    trim_tracks(cfg, now_ms);

    const float cx = static_cast<float>(cfg.roi_w) * cfg.center_x;
    const float cy = static_cast<float>(cfg.roi_h) * cfg.center_y;
    // 半径基准优先用 search_radius_px（= 截取尺寸内划最大圆的半径，
    // 由 AimThread 从 capture.width/height 填）；未填时回退旧口径
    // min(roi_w, roi_h)/2（整帧），保证未接线的调用方行为不变。
    const float base_radius = cfg.search_radius_px > 0.0f
        ? cfg.search_radius_px
        : std::min(cfg.roi_w, cfg.roi_h) * 0.5f;
    const float radius = base_radius * cfg.fov_range;
    last_fov_radius_px_ = radius;  // ★ 记录本帧实际用的选靶半径（像素）。V1.0.37 起
                                   //   预览蓝圆已移前端，此值现由 test_target_selector
                                   //   观察选靶半径数学（防止 base_radius × fov_range 被改坏）

    // 头身稳定过滤（对齐 BB applyHeadBodyStable）：同一帧同时出现 (bodyN + headN)
    // 时删掉 headN 框 —— 头身同框时头部框容易把瞄准点抢走，只留身体框更稳。
    // 默认 head_body_stable=false ⇒ dets 原样传入，行为与本参数加入前一致。
    std::vector<DetectionBox> stable_dets;
    const std::vector<DetectionBox>* use_dets = &dets;
    if (cfg.head_body_stable) {
        auto has_class = [&dets](int c) {
            if (c < 0) return false;
            for (const auto& b : dets) {
                if (b.class_id == c) return true;
            }
            return false;
        };
        const bool combo1 = has_class(cfg.hb_body1) && has_class(cfg.hb_head1);
        const bool combo2 = has_class(cfg.hb_body2) && has_class(cfg.hb_head2);
        if (combo1 || combo2) {
            stable_dets.reserve(dets.size());
            for (const auto& b : dets) {
                if (combo1 && b.class_id == cfg.hb_head1) continue;
                if (combo2 && b.class_id == cfg.hb_head2) continue;
                stable_dets.push_back(b);
            }
            use_dets = &stable_dets;
        }
    }
    auto cands = collect_candidates(*use_dets, cfg, cx, cy, base_radius);

    // ---- 单一“还是他吗”判定 + 锁定保持 ----
    // 只认一个判据（照 yey）：同类别 且 IoU ≥ 0.6 才算同一个人。
    // 是 → 继续瞄（量测门控 + 平滑更新）；不是 → 锁定保持窗口内输出最后位置；
    // 窗口耗尽才真正放弃、交给单层打分选新目标。
    const bool had_active_lock = (active_track_ >= 0);
    if (active_track_ >= 0) {
        TrackEntry* at = nullptr;
        for (auto& t : tracks_) {
            if (t.id == active_track_) { at = &t; break; }
        }
        if (at) {
            // 在所有候选里找“还是他”：同类别 + IoU 最高者（≥ 0.6）。
            // ★ 身份判定用 ref_box（每帧更新的参考框），不用 at->box：
            //   at->box 会被量测门控冻结（reject 时沿用上一帧、防落点跳），若拿它算 IoU，
            //   快移目标累计位移逐帧增大 ⇒ IoU 单调下降到 0.6 以下 ⇒ 误判“换了人”⇒ 掉锁。
            const Candidate* same = nullptr;
            float best_iou = -1.0f;
            for (const auto& c : cands) {
                if (c.box.class_id != at->ref_box.class_id) continue;
                const float iou = selgate::box_iou(c.box, at->ref_box);
                if (iou >= selgate::kSamePersonIou && iou > best_iou) {
                    best_iou = iou;
                    same = &c;
                }
            }

            if (same) {
                // ★ 每帧都更新参考框（不经过门控冻结）：身份判定始终跟着真实目标走。
                //   门控 reject 时 at->box 仍冻结（输出防跳），但 ref_box 照常跟进 ——
                //   这就是"输出框冻结"与"是不是同一个人"两条职责的解耦点。
                at->ref_box = same->box;

                // ---- V1.0.11：量测跳变门控（照 yu 的 jump_rejected_holding_previous）----
                // 板端定障：同一 track 下框顶 y1 帧间跳 ±17px，噪声经落点 y1+0.31h
                // 直接进控制环；实测稳态误差每秒过零 3.67 次 = 眼睛看到的"晃"。
                // 尖峰里 88.6% 是突跳（跳变量 ÷ 邻帧位移中位 ≥2）⇒ 连续性判据能拦，
                // 且不误杀正常跟枪（离线回放：p90 抖动 −38.2%、滞后 0 帧）。
                const float mv = std::hypot(same->cx - at->cx, same->cy - at->cy);
                const float thr = std::fmax(selgate::kMeasJumpAbsPx,
                                            selgate::kMeasJumpRel * selgate::local_speed(*at));
                const float ratio_eff = selgate::effective_size_ratio(cfg.track_size_ratio);
                const bool reject = (ratio_eff > 0.0f) && mv > thr &&
                                    at->hold_frames < selgate::kMeasMaxHold;
                if (reject) {
                    // holding_previous：不更新几何，沿用上一帧的框。
                    // ★ last_seen_ms 必须刷新 —— 目标确实还在（有候选匹配上了），
                    //   不刷新会被锁定保持/轨迹回收判成"丢失"，把刚稳住的锁定放掉。
                    at->hold_frames++;
                    at->rejected_total++;
                    selector_holds_total_++;
                    at->lost_frames = 0;
                    at->last_seen_ms = now_ms;
                    out.held = true;
                } else {
                    // 认输放行这一帧：把这一跳记进历史 ⇒ 局部速度基准抬升，
                    // 阈值随之放宽（真快移的目标被 hold 满 10 帧后自然恢复跟随）。
                    if (mv > 0.0f) selgate::push_speed(*at, mv);
                    at->hold_frames = 0;
                    // 更新 track（框体 + 中心）
                    at->box = same->box;
                    at->cx = same->cx;
                    at->cy = same->cy;
                    at->lost_frames = 0;
                    track_seen(*at, now_ms);
                }
                out.valid = true;
                out.box = at->box;              // reject ⇒ 本帧输出沿用上一帧的框
                out.target_id = at->id;
                // distance 必须从**实际输出的框**算（reject 时 out.box 是旧框，不是 same->box）。
                out.distance = selgate::aim_distance(out.box, cfg, cx, cy);
                out.lock_radius = std::max(1.0f, 0.06f * (at->box.x2 - at->box.x1));
                out.reason = TargetSelection::Reason::kTrackLock;
                out.continuity = false;  // id 未变 ⇒ 上游无需“延续”提示
                last_reason_ = out.reason;
                last_target_x_ = at->box.x1 + (at->box.x2 - at->box.x1) * cfg.aim_ratio_x;
                last_target_y_ = at->box.y1 + (at->box.y2 - at->box.y1) * cfg.aim_ratio_y;
                has_last_target_ = true;
                return out;
            }

            // 没找到“还是他” ⇒ 锁定保持：窗口内继续输出最后位置（valid，不返回 invalid）。
            at->lost_frames++;
            if (static_cast<uint32_t>(now_ms - at->last_seen_ms) <
                selgate::lock_hold_window_ms(cfg)) {
                out.valid = true;
                out.box = at->box;
                out.target_id = at->id;
                out.distance = selgate::aim_distance(at->box, cfg, cx, cy);
                out.lock_radius = std::max(1.0f, 0.06f * (at->box.x2 - at->box.x1));
                out.reason = TargetSelection::Reason::kTrackLock;
                out.held = true;   // 本帧几何未更新（目标不在候选里）
                out.continuity = false;
                last_reason_ = out.reason;
                return out;
            }

            // 锁定保持超时 ⇒ 真正放弃锁定，交给下面的单层打分。
            at->active = false;
            active_track_ = -1;
        } else {
            active_track_ = -1;  // 激活 track 不存在（被清理）
        }
    }

    // ---- 开火期禁切靶（照 yu 的 fire_switch_guarded，保留其意图）----
    // 走到这里说明锁定保持已耗尽、正要另选新目标。开火中不允许另选：
    // 宁可本帧无目标，也不在压枪时把准星从正压着的目标甩到别人身上。
    if (cfg.fire_active && had_active_lock) {
        last_reason_ = TargetSelection::Reason::kNone;
        return out;
    }

    // ---- 单层打分（照 BB-828 calcPriority）----
    // rawPriority = distScore×weight_dist + sizeScore×weight_size + stick×0.5
    //   distScore = 1/(1 + dist/dist_ref_px)
    //   sizeScore = min(1, w×h / size_ref_px²)   （BB 原式 area/10000 = area/100²）
    //   stick = distLast < threshold ? (1 - distLast/threshold)×stickiness : 0
    // ★ 打分恒开启（不再由 priority_scoring 开关）；候选已按 prefer_humanoid/priority/距离
    //   排序，用严格 > 比较 ⇒ 同分时保持排序靠前者（几何判据→高优先→近）。
    if (cands.empty()) {
        last_reason_ = TargetSelection::Reason::kNone;
        return out;
    }
    const float dref = (cfg.dist_ref_px > 0.0f) ? cfg.dist_ref_px : 100.0f;
    const float sref = (cfg.size_ref_px > 0.0f) ? cfg.size_ref_px : 100.0f;
    const float size_ref_area = sref * sref;
    const Candidate* chosen = nullptr;
    float best_score = -1e30f;
    for (const auto& cand : cands) {
        const float dist = std::sqrt(cand.dist_sq);
        const float dist_score = 1.0f / (1.0f + dist / dref);
        const float bw = cand.box.x2 - cand.box.x1;
        const float bh = cand.box.y2 - cand.box.y1;
        // fmax(0,·)：异常框（坐标倒置）会让面积变负 ⇒ NaN 污染整轮打分
        const float size_score = std::fmin(1.0f, std::fmax(0.0f, bw * bh) / size_ref_area);
        float stick = 0.0f;
        if (has_last_target_ && cfg.switch_threshold_px > 0.0f) {
            const float ax = cand.box.x1 + bw * cfg.aim_ratio_x;
            const float ay = cand.box.y1 + bh * cfg.aim_ratio_y;
            const float sdx = ax - last_target_x_;
            const float sdy = ay - last_target_y_;
            const float dist_last = std::sqrt(sdx * sdx + sdy * sdy);
            if (dist_last < cfg.switch_threshold_px) {
                stick = (1.0f - dist_last / cfg.switch_threshold_px) * cfg.stickiness;
            }
        }
        const float score = dist_score * cfg.weight_dist +
                            size_score * cfg.weight_size + stick * 0.5f;
        if (score > best_score) {
            best_score = score;
            chosen = &cand;
        }
    }
    if (!chosen) {
        last_reason_ = TargetSelection::Reason::kNone;
        return out;
    }

    // ---- 获取/复用轨迹 ----
    // 优先按“同一个人”复用非激活轨迹（同类别 + IoU ≥ 0.6）⇒ target_id 延续、上游不 reset；
    // 退而求其次按距离复用（<40px，兼容类别被重编号的场景）。
    TrackEntry* reuse = nullptr;
    for (auto& t : tracks_) {
        if (t.active) continue;
        if (chosen->box.class_id == t.box.class_id &&
            selgate::box_iou(chosen->box, t.box) >= selgate::kSamePersonIou) {
            reuse = &t;
            break;
        }
    }
    if (!reuse) {
        for (auto& t : tracks_) {
            if (t.active) continue;
            const float d = (chosen->cx - t.cx) * (chosen->cx - t.cx) +
                            (chosen->cy - t.cy) * (chosen->cy - t.cy);
            if (d < 1600.0f) { reuse = &t; break; }  // <40px 复用
        }
    }
    if (reuse) {
        reuse->box = chosen->box;
        reuse->ref_box = chosen->box;   // 掉 id 重获时正确初始化参考框（身份判定基线）
        reuse->cx = chosen->cx;
        reuse->cy = chosen->cy;
        reuse->lost_frames = 0;
        reuse->active = true;
        // 新一次锁定 ⇒ 清掉旧的量测门控速度基准（避免吃上一段的 stale 历史）。
        reuse->d_hist_n = 0;
        reuse->hold_frames = 0;
        track_seen(*reuse, now_ms);
        active_track_ = reuse->id;
        out.target_id = reuse->id;
    } else {
        // 新建 track
        TrackEntry nt;
        nt.id = next_id_++;
        nt.box = chosen->box;
        nt.ref_box = chosen->box;   // 首次锁定：参考框 = 输出框
        nt.cx = chosen->cx;
        nt.cy = chosen->cy;
        nt.lost_frames = 0;
        nt.active = true;
        nt.last_seen_ms = now_ms;
        tracks_.push_back(nt);
        active_track_ = nt.id;
        out.target_id = nt.id;
    }
    out.valid = true;
    out.box = chosen->box;
    out.distance = std::sqrt(chosen->dist_sq);
    out.lock_radius = std::max(1.0f, 0.06f * (chosen->box.x2 - chosen->box.x1));
    out.reason = TargetSelection::Reason::kScore;
    out.continuity = false;
    last_target_x_ = chosen->box.x1 + (chosen->box.x2 - chosen->box.x1) * cfg.aim_ratio_x;
    last_target_y_ = chosen->box.y1 + (chosen->box.y2 - chosen->box.y1) * cfg.aim_ratio_y;
    has_last_target_ = true;
    last_reason_ = out.reason;
    return out;
    }

}  // namespace ttbox::core::aim
