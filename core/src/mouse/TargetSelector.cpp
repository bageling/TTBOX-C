// TargetSelector.cpp — A10 目标选择器实现（多目标追踪 + 分层选择）
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
 *   TargetSelector 根据规则选一个：
 *   - 离瞄准点最近的目标
 *   - 连续出现多帧的目标（更稳定）
 *   - 跟踪已有目标（不会突然跳走）
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "mouse/TargetSelector.hpp"

#include <algorithm>
#include <cmath>

namespace ttbox::core::aim {

namespace {
float box_center_x(const DetectionBox& b) { return (b.x1 + b.x2) * 0.5f; }
float box_center_y(const DetectionBox& b) { return (b.y1 + b.y2) * 0.5f; }
float box_diag(const DetectionBox& b) {
    return std::hypot(b.x2 - b.x1, b.y2 - b.y1);
}
}  // namespace

std::vector<TargetSelector::Candidate> TargetSelector::collect_candidates(
    const std::vector<DetectionBox>& dets, const TargetSelectorConfig& cfg, float cx, float cy,
    float radius_sq) const {
    std::vector<Candidate> out;
    for (const auto& b : dets) {
        if (b.score < cfg.confidence) continue;
        if (!cfg.class_filter.empty()) {
            const bool in = std::find(cfg.class_filter.begin(), cfg.class_filter.end(),
                                      b.class_id) != cfg.class_filter.end();
            if (!in) continue;
        }
        const float bdx = b.x1 + (b.x2 - b.x1) * cfg.aim_ratio_x - cx;
        const float bdy = b.y1 + (b.y2 - b.y1) * cfg.aim_ratio_y - cy;
        const float d_sq = bdx * bdx + bdy * bdy;
        if (d_sq > radius_sq) continue;  // FOV 范围外
        out.push_back({b, box_center_x(b), box_center_y(b), d_sq,
                       class_priority(cfg, b.class_id)});
    }
    // 排序：priority 高者优先，同 priority 按距离近者优先。
    // （priority 用于"同距离竞争"时的目标优先级，不影响距离本身。）
    std::sort(out.begin(), out.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.priority != b.priority) return a.priority > b.priority;
                      return a.dist_sq < b.dist_sq;
                  });
        return out;
    }

    // ---- ByteTrack 增强（第4项）实现 ----
    // 卡尔曼匀速模型：状态 [cx,cy,w,h,vx,vy,vw,vh]。
    // 关联参考点 = 上次平滑位置 + 速度（clamp 到 kalman_max_speed_px），
    // 对慢速/匀速目标预测≈当前 → 与原裸框心关联行为兼容（109 用例不受影响）。
    void TargetSelector::kalman_predict(TrackEntry& t, const TargetSelectorConfig& cfg) const {
        const float maxv = cfg.kalman_max_speed_px > 0.0f ? cfg.kalman_max_speed_px : 25.0f;
        auto cv = [maxv](float v) { return v > maxv ? maxv : (v < -maxv ? -maxv : v); };
        t.pred_cx = t.kx + cv(t.vx);
        t.pred_cy = t.ky + cv(t.vy);
    }

    void TargetSelector::kalman_update(TrackEntry& t, const DetectionBox& obs,
                                       const TargetSelectorConfig& cfg, uint32_t now_ms) {
        const float g = cfg.kalman_velocity_gain;  // 速度学习增益
        const float ncx = box_center_x(obs);
        const float ncy = box_center_y(obs);
        const float nw = obs.x2 - obs.x1;
        const float nh = obs.y2 - obs.y1;
        if (t.hits == 0) {
            // 首次命中：直接赋观测（速度=0）
            t.kx = ncx; t.ky = ncy; t.kw = nw; t.kh = nh;
            t.vx = t.vy = t.vw = t.vh = 0.0f;
        } else {
            // 用"预更新前的 innovation"学习速度（旧代码先纠正再取差导致速度为 0）
            t.vx += (ncx - t.kx) * g;
            t.vy += (ncy - t.ky) * g;
            t.vw += (nw - t.kw) * g;
            t.vh += (nh - t.kh) * g;
            // 位置/尺寸向观测收缩（朴素增益，保留历史平滑）
            float pos = g + 0.55f; if (pos > 0.9f) pos = 0.9f;
            t.kx += (ncx - t.kx) * pos;
            t.ky += (ncy - t.ky) * pos;
            t.kw = nw;  // 尺寸直接跟随观测（框尺寸更可信）
            t.kh = nh;
        }
        t.hits++;
        if (t.hits >= 3) t.confirmed = true;
        t.last_seen_ms = now_ms;
        kalman_predict(t, cfg);
    }

    void TargetSelector::trim_tracks(const TargetSelectorConfig& cfg) {
        // 1) 删除丢失帧数超过 buffer 的轨迹（修"只增不删"隐患：轨迹有生有死）
        const uint32_t buf = cfg.track_buffer_frames > 0 ? cfg.track_buffer_frames : 30u;
        if (!tracks_.empty()) {
            tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                         [&](const TrackEntry& t) {
                                             return !t.active && t.lost_frames > buf;
                                         }),
                          tracks_.end());
        }
        // 2) 总轨迹数超上限：裁剪最长未命中（last_seen 最旧）的非激活轨迹
        if (tracks_.size() > cfg.max_tracks) {
            // 活动轨迹永远保留；只对非激活轨迹按 last_seen_ms 升序淘汰
            std::stable_sort(tracks_.begin(), tracks_.end(),
                             [](const TrackEntry& a, const TrackEntry& b) {
                                 if (a.active != b.active) return a.active;  // 活动优先
                                 if (a.lost_frames != b.lost_frames) return a.lost_frames > b.lost_frames;  // 丢失多的先删
                                 return a.last_seen_ms < b.last_seen_ms;      // 更早未见先删
                             });
            while (tracks_.size() > cfg.max_tracks) tracks_.pop_back();
        }
    }

    TargetSelection TargetSelector::select(const std::vector<DetectionBox>& dets,
                                           const TargetSelectorConfig& cfg, uint32_t now_ms) {
    TargetSelection out;
    if (dets.empty() || cfg.roi_w == 0 || cfg.roi_h == 0) {
        // 无检测：激活 track 丢失计数 + 宽限判定
        // 注意：空检测帧立即返回 invalid（安全红线：不允许凭旧坐标产生移动）。
        // "短暂消失保持 target_id"由 AimStateMachine 的 LOST_GRACE 层实现
        // （track 在宽限内不删除，恢复检测后同 id 延续），见 AimStateMachine。
        for (auto& t : tracks_) {
            if (t.active) {
                            t.lost_frames++;
                            // 宽限耗尽 → 放弃激活（不立即删 track，允许后续重建）
                            // ★ 2026-09-26：按真实时间判定（last_seen_ms 距今），
                            //   旧实现 lost_frames*7 隐含 143fps 假设 —— 60fps 时
                            //   实际宽限放大 2.4 倍、30fps 放大 4.7 倍，"打幽灵"
                            //   的时长完全不受 lost_grace_ms 控制。回绕安全比较。
                            if (static_cast<uint32_t>(now_ms - t.last_seen_ms) >=
                                static_cast<uint32_t>(cfg.lost_grace_ms)) {
                                t.active = false;
                                active_track_ = -1;
                            }
                        }
                    }
                    // 轨迹生命周期：删除丢失超 buffer 的失效轨迹（修"只增不删"）
                    trim_tracks(cfg);
                    last_reason_ = TargetSelection::kNone;
                    return out;
                }

    // 选择器每一帧（无论是否有检测）都执行轨迹上限裁剪：
    // 旧实现只在“无检测”分支调用，目标持续存在时轨迹数会越过 max_tracks。
    trim_tracks(cfg);

                const float cx = static_cast<float>(cfg.roi_w) * cfg.center_x;
                const float cy = static_cast<float>(cfg.roi_h) * cfg.center_y;
                // 半径基准优先用 search_radius_px（= 截取尺寸内划最大圆的半径，
                // 由 AimThread 从 capture.width/height 填）；未填时回退旧口径
                // min(roi_w, roi_h)/2（整帧），保证未接线的调用方行为不变。
                const float base_radius = cfg.search_radius_px > 0.0f
                    ? cfg.search_radius_px
                    : std::min(cfg.roi_w, cfg.roi_h) * 0.5f;
                const float radius = base_radius * cfg.fov_range;
                const float radius_sq = radius * radius;

                // ByteTrack：每帧先对现有轨迹做卡尔曼预测（写入 pred_cx/pred_cy 供关联参考）
                for (auto& t : tracks_) kalman_predict(t, cfg);

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
                auto cands = collect_candidates(*use_dets, cfg, cx, cy, radius_sq);
    if (cands.empty()) {
            // 有检测但全被过滤/出范围：同上宽限判定
            for (auto& t : tracks_) {
                if (t.active) {
                    t.lost_frames++;
                    // 同上：按真实时间判定宽限（回绕安全），不再用 帧数×7ms
                    if (static_cast<uint32_t>(now_ms - t.last_seen_ms) >=
                        static_cast<uint32_t>(cfg.lost_grace_ms)) {
                        t.active = false;
                        active_track_ = -1;
                    }
                }
            }
            trim_tracks(cfg);
            last_reason_ = TargetSelection::kNone;
            return out;
        }

    // ---- 第 1 层：track_lock（候选与激活 track 相同 id）----
    // 激活 track 的匹配范围：自身对角 × 2（容忍检测抖动）
    if (active_track_ >= 0) {
        TrackEntry* at = nullptr;
        for (auto& t : tracks_) {
            if (t.id == active_track_) { at = &t; break; }
        }
        if (at) {
                    // track_lock 匹配半径：目标对角 × 1.0 + 8px（同目标连续帧小位移；
                    // 大位移/重编号由第 2 层 rect_lock 兜底，防误匹配邻近目标）
                    const float match_r = box_diag(at->box) * 1.0f + 8.0f;
                    const float match_r_sq = match_r * match_r;
                    // 关联参考点：ByteTrack 用卡尔曼预测中心（pred_cx/pred_cy）；
                                        // 默认关：用裸框心 → 保持传统最近邻行为（109 用例兼容）。
                                        const float ref_cx = (cfg.use_kalman_predict && at->pred_cx != 0.0f) ? at->pred_cx : at->cx;
                                        const float ref_cy = (cfg.use_kalman_predict && at->pred_cy != 0.0f) ? at->pred_cy : at->cy;
                    // 找离锁定框中心最近的候选
                    const Candidate* best = nullptr;
                    float best_d = match_r_sq;
                    for (const auto& c : cands) {
                        const float d = (c.cx - ref_cx) * (c.cx - ref_cx) +
                                        (c.cy - ref_cy) * (c.cy - ref_cy);
                        if (d < best_d) { best_d = d; best = &c; }
                    }
                    if (best) {
                        // 更新 track（框体 + 卡尔曼状态）
                        at->box = best->box;
                        at->cx = best->cx;
                        at->cy = best->cy;
                        at->lost_frames = 0;
                        kalman_update(*at, best->box, cfg, now_ms);
                        out.valid = true;
                out.box = best->box;
                out.target_id = at->id;
                out.distance = std::sqrt(best->dist_sq);
                out.lock_radius = std::max(1.0f, 0.06f * (best->box.x2 - best->box.x1));
                out.reason = TargetSelection::kTrackLock;
                last_reason_ = out.reason;
                last_locked_dist_sq_ = best->dist_sq;  // 供丢失后切靶滞后比较用
                // BB 对标：记录本帧选中点，供打分制的 stick（粘滞）项使用
                last_target_x_ = best->box.x1 + (best->box.x2 - best->box.x1) * cfg.aim_ratio_x;
                last_target_y_ = best->box.y1 + (best->box.y2 - best->box.y1) * cfg.aim_ratio_y;
                has_last_target_ = true;
                return out;
            }
            // 激活 track 未匹配：丢失宽限
            at->lost_frames++;
            // ★ 2026-09-26：同上按真实时间判定（回绕安全），帧率偏离 143fps 时
            //   旧口径的宽限失真最大 8.6 倍。
            const bool grace_exhausted =
                static_cast<uint32_t>(now_ms - at->last_seen_ms) >=
                static_cast<uint32_t>(cfg.lost_grace_ms);
            if (grace_exhausted) {
                at->active = false;
                active_track_ = -1;
                // 继续走第 2/3 层
            } else {
                // 宽限内：保持原目标（用锁定框），不切换
                out.valid = true;
                out.box = at->box;
                out.target_id = at->id;
                const float ddx = at->cx - cx, ddy = at->cy - cy;
                out.distance = std::sqrt(ddx * ddx + ddy * ddy);
                out.lock_radius = std::max(1.0f, 0.06f * (at->box.x2 - at->box.x1));
                out.reason = TargetSelection::kTrackLock;
                last_reason_ = out.reason;
                return out;
            }
        } else {
            active_track_ = -1;  // 激活 track 不存在（被清理）
        }
    }

    // ---- 切靶防抖守卫（对齐 BB：target_switch_hysteresis + switch_cooldown）----
    // 只作用在「锁定已丢失、正要另选目标」的第 2/3 层；
    // 第 1 层 track_lock 是保持锁定，不经过这里（所以不影响正常跟枪）。
    // 两个阈值任一为 0 ⇒ 对应机制关闭，行为与加入前一致。
    auto switch_blocked = [&](float new_dist_sq) -> bool {
        // ★ 只有一个候选 ⇒ 不存在"在多个目标间切换"（目标高速移动/瞬移同样走这条路径），
        //   此时若还拦，会把正常跟踪一起挡死 —— 一律放行。
        if (cands.size() < 2) return false;
        if (cfg.switch_cooldown_ms > 0.0f && has_switch_) {
            // ★ 2026-09-26：无符号回绕安全比较。now_ms 是 uint32 时基（V4L2 单调钟，
            //   约 49.7 天回绕），旧实现有符号相减在回绕后恒为巨大负数 ⇒ 冷却永不解除，
            //   而 last_switch_ms_ 只在第 3 层成功时更新（封锁本身阻止更新）⇒ 不可
            //   自愈死锁，只有重启能救。无符号差值对回绕天然正确。
            const uint32_t since = now_ms - last_switch_ms_;
            if (since < static_cast<uint32_t>(cfg.switch_cooldown_ms)) return true;
        }
        if (cfg.switch_hysteresis > 0.0f && last_locked_dist_sq_ > 0.0f) {
            const float k = 1.0f + cfg.switch_hysteresis;
            // 新目标必须**明显更近**才允许切：new_dist_sq × (1+h)² < old_dist_sq
            // （用距离平方比较，与候选排序一致，省一次 sqrt）
            if (!(new_dist_sq * k * k < last_locked_dist_sq_)) return true;
        }
        return false;
    };

    // ---- 第 2 层：rect_lock / continuity（候选与任一 track 位置匹配）----
    // 遍历非激活 track（含宽限内旧目标），按位置匹配
    {
        float best_d = 1e30f;
        const Candidate* best_c = nullptr;
        TrackEntry* best_t = nullptr;
        for (auto& t : tracks_) {
                    if (t.active) continue;  // 已有激活走第 1 层
                    const float match_r = box_diag(t.box) * cfg.switch_match_ratio + 24.0f;
                    const float match_r_sq = match_r * match_r;
                    const float ref_cx = (cfg.use_kalman_predict && t.pred_cx != 0.0f) ? t.pred_cx : t.cx;
                                        const float ref_cy = (cfg.use_kalman_predict && t.pred_cy != 0.0f) ? t.pred_cy : t.cy;
                    for (const auto& c : cands) {
                        const float d = (c.cx - ref_cx) * (c.cx - ref_cx) + (c.cy - ref_cy) * (c.cy - ref_cy);
                        if (d < match_r_sq && d < best_d) { best_d = d; best_c = &c; best_t = &t; }
                    }
                }
    if (best_c && best_t) {
                    // 切换到另一个轨迹前，先释放旧的激活轨迹，避免多个 track 同时 active。
                    for (auto& t : tracks_) {
                        if (t.id != best_t->id) t.active = false;
                    }
                    best_t->box = best_c->box;
                    best_t->cx = best_c->cx;
                    best_t->cy = best_c->cy;
                    best_t->lost_frames = 0;
                    best_t->active = true;
                    kalman_update(*best_t, best_c->box, cfg, now_ms);
                    active_track_ = best_t->id;
                    // ★ 第 2 层是「位置匹配 ⇒ 大概率同一目标」，不算切换、不启用冷却，
                    //   否则会误伤"短暂丢失后重新获取"（那是 lost_grace_ms 的职责）。
                    //   只刷新滞后比较基准；真正的新目标切换由第 3 层守卫把关。
                    last_locked_dist_sq_ = best_c->dist_sq;
                    out.valid = true;
                    out.box = best_c->box;
                    out.target_id = best_t->id;
                    out.distance = std::sqrt(best_c->dist_sq);
                    out.lock_radius = std::max(1.0f, 0.06f * (best_c->box.x2 - best_c->box.x1));
                    out.reason = TargetSelection::kRectLock;
            last_reason_ = out.reason;
            return out;
        }
    }

    // ---- 第 3 层：score（无锁定，新建 track 或复用最近 track）----
        {
            // ---- lock_hold：锁定保持窗内维持现锁，不给新目标（对齐 BB lock_hold_time=1500ms）----
            // 只作用在「锁定已丢失、正要另选」的第 3 层；默认 0 = 关闭，行为与加入前一致。
            // 窗口内的处理是"只刷新位置"——但本帧没有匹配到该轨迹的新检测框，故沿用其历史框。
            if (cfg.lock_hold_ms > 0.0f && lock_track_id_ >= 0) {
                const long long held =
                    static_cast<long long>(now_ms) - static_cast<long long>(lock_start_ms_);
                if (held >= 0 && held < static_cast<long long>(cfg.lock_hold_ms)) {
                    for (auto& t : tracks_) {
                        if (t.id != lock_track_id_) continue;
                        out.valid = true;
                        out.box = t.box;
                        out.target_id = t.id;
                        const float hdx = t.cx - cx, hdy = t.cy - cy;
                        out.distance = std::sqrt(hdx * hdx + hdy * hdy);
                        out.lock_radius = std::max(1.0f, 0.06f * (t.box.x2 - t.box.x1));
                        out.reason = TargetSelection::kTrackLock;
                        last_reason_ = out.reason;
                        return out;
                    }
                    // 锁定轨迹已被裁剪 ⇒ 放弃保持，继续走正常选靶
                }
            }
            // ---- 候选优选：打分制（BB calcPriority）或"最近优先"（默认，现行为）----
            const Candidate* chosen = &cands.front();  // 已按（优先级, 距离）排序
            if (cfg.priority_scoring) {
                // raw = distScore×w_dist + sizeScore×w_size + stick×0.5
                //   distScore = 1/(1 + (dist/M)/dist_ref)      —— M = 本档倍镜真实倍率
                //   sizeScore = min(1, sqrt((w/M)×(h/M))/size_ref)
                //   stick = distLast<threshold ? (1-distLast/threshold)×stickiness : 0
                // ★ V3 阶段 4（2026-09-28）：两项都先除以 M，归一到**腰射等效量纲**。
                //   尺寸项同时由 面积/10000 改为 sqrt(面积)/参考边长 —— 原式在 640 窗口
                //   里 2 倍镜起就全部撞顶（实测 70×243=17086 ≫ 10000），等于没配。
                //   M=1.0（腰射/未配倍率）时 distScore 与旧式逐位相同。
                const float m = (cfg.zoom_scale > 0.0f) ? cfg.zoom_scale : 1.0f;
                const float dref = (cfg.dist_ref_px > 0.0f) ? cfg.dist_ref_px : 100.0f;
                const float sref = (cfg.size_ref_px > 0.0f) ? cfg.size_ref_px : 320.0f;
                float best_score = -1e30f;
                for (const auto& cand : cands) {
                    const float dist = std::sqrt(cand.dist_sq) / m;
                    const float dist_score = dist > 0.0f ? 1.0f / (1.0f + dist / dref) : 0.0f;
                    const float bw = (cand.box.x2 - cand.box.x1) / m;
                    const float bh = (cand.box.y2 - cand.box.y1) / m;
                    // fmax(0,·)：异常框（坐标倒置）会让 sqrt 吃到负数 ⇒ NaN 污染整轮打分
                    const float size_score = std::fmin(1.0f, std::sqrt(std::fmax(0.0f, bw * bh)) / sref);
                    float stick = 0.0f;
                    if (has_last_target_ && cfg.switch_threshold_px > 0.0f) {
                        const float sdx = cand.cx - last_target_x_;
                        const float sdy = cand.cy - last_target_y_;
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
            }
            const Candidate& c = *chosen;
            // 切靶防抖：冷却未过 / 新目标不够近 ⇒ 本帧不选新目标（返回无效）。
            // 宁可短暂无目标，也不在两个目标之间来回拉锯（对齐 BB 的 cooldown + hysteresis）。
            if (switch_blocked(c.dist_sq)) {
                last_reason_ = TargetSelection::kNone;
                return out;
            }
            // 与第 2 层一致：切换激活轨迹前先释放旧锁，防止 active 轨迹无限累积。
            if (active_track_ >= 0) {
                for (auto& t : tracks_) {
                    if (t.id == active_track_) {
                        t.active = false;
                        break;
                    }
                }
            }
            // 复用已存在但未激活且距离近的 track（防同目标重复建 track）
            TrackEntry* reuse = nullptr;
            for (auto& t : tracks_) {
                if (t.active) continue;
                const float d = (c.cx - t.cx) * (c.cx - t.cx) + (c.cy - t.cy) * (c.cy - t.cy);
                if (d < 1600.0f) { reuse = &t; break; }  // <40px 复用
            }
            if (reuse) {
                reuse->box = c.box;
                reuse->cx = c.cx;
                reuse->cy = c.cy;
                reuse->lost_frames = 0;
                reuse->active = true;
                kalman_update(*reuse, c.box, cfg, now_ms);
                active_track_ = reuse->id;
                out.box = c.box;
                out.target_id = reuse->id;
            } else {
                // 新建 track（初始化卡尔曼状态）
                TrackEntry nt;
                nt.id = next_id_++;
                nt.box = c.box;
                nt.cx = c.cx;
                nt.cy = c.cy;
                nt.lost_frames = 0;
                nt.active = true;
                nt.kx = c.cx; nt.ky = c.cy;
                nt.kw = c.box.x2 - c.box.x1; nt.kh = c.box.y2 - c.box.y1;
                nt.vx = nt.vy = nt.vw = nt.vh = 0.0f;
                nt.hits = 0;
                nt.created_ms = now_ms;
                kalman_predict(nt, cfg);
                nt.last_seen_ms = now_ms;
                tracks_.push_back(nt);
                active_track_ = nt.id;
                out.box = c.box;
                out.target_id = nt.id;
            }
            out.valid = true;
            out.distance = std::sqrt(c.dist_sq);
            out.lock_radius = std::max(1.0f, 0.06f * (c.box.x2 - c.box.x1));
            last_switch_ms_ = now_ms;
            has_switch_ = true;
            last_locked_dist_sq_ = c.dist_sq;
            // BB 对标：本次选择建立/延续锁定 ⇒ 记锁定轨迹与起点（lock_hold 窗口判据），
            // 并留下上帧选中瞄准点（打分制 stick 项用）。
            if (out.target_id != lock_track_id_) {
                lock_track_id_ = out.target_id;
                lock_start_ms_ = now_ms;
            }
            last_target_x_ = c.box.x1 + (c.box.x2 - c.box.x1) * cfg.aim_ratio_x;
            last_target_y_ = c.box.y1 + (c.box.y2 - c.box.y1) * cfg.aim_ratio_y;
            has_last_target_ = true;
            out.reason = TargetSelection::kScore;
            last_reason_ = out.reason;
            return out;
        }
    }

}  // namespace ttbox::core::aim
