// ClipHeightRatio.hpp — V1.0.09：框底被裁剪区截断时的「身高/框宽」比追踪器（纯状态、无依赖）。
//
// 为什么需要它：检测只吃画面中心一块 crop（板端 416x416），近身目标的下半身落在 crop
// 之外 ⇒ 可见框高 h_obs = crop 下边 - y1 比真实身高小 ⇒ ty = y1 + oy*h_obs 相对人体上飘，
// 且越近截得越多（所以症状是「走进目标才飘」而不是「一直偏」）。
// 修法是用**框宽**反推身高（宽度不随纵向裁剪失真），但「身高/框宽」这个比值不能写死：
// 本模型 cls5 的框宽高比实测在 0.29~0.52 之间漂（远距离时常只框上半身 ⇒ 框更宽更短），
// 写死一个值必然在某一头过度修正。
//
// 做法：记住**同一目标最近一次「未截断」帧**的 h/w。比值在等比缩放下不变（人走近时
// 框宽、框高按同一比例放大），所以可以直接乘到当前框宽上反推身高。目标一换（track id
// 变了）就作废，退回跨目标 EMA 兜底；连 EMA 都没有 ⇒ 返回 0，由调用方退回配置兜底值。
#pragma once

namespace ttbox::core::aim {

class ClipHeightRatioTracker {
public:
    // h/w 的合理区间：挡退化框（近方形框、把两人并一框的超宽框）被拿去外推身高。
    static constexpr float kMinRatio = 0.8f;
    static constexpr float kMaxRatio = 12.0f;

    // 换模型世代时清空（旧目标的比值与新目标无关，禁止跨世代继承）。
    void reset() { per_target_ = 0.0f; per_target_id_ = -1; ema_ = 0.0f; }

    // 每帧喂一次。bottom_clipped = 本帧框底是否贴到裁剪区下边界。
    void observe(float box_w, float box_h, bool bottom_clipped, int track_id) {
        if (!(box_w > 1.0f) || !(box_h > 1.0f)) return;
        const float ratio = box_h / box_w;
        if (!(ratio >= kMinRatio && ratio <= kMaxRatio)) return;
        if (bottom_clipped) return;   // 被截断帧的比例本身就偏小，不能拿来当基准
        per_target_ = per_target_ > 0.0f ? per_target_ * 0.8f + ratio * 0.2f : ratio;
        per_target_id_ = track_id;
        ema_ = ema_ > 0.0f ? ema_ * 0.95f + ratio * 0.05f : ratio;
    }

    // 截断帧取用：优先本目标缓存，其次跨目标 EMA；都没有 ⇒ 0（调用方退回配置兜底）。
    float ratio_for(int track_id) const {
        if (per_target_id_ == track_id && per_target_ > 0.0f) return per_target_;
        return ema_;
    }

    float per_target() const { return per_target_; }
    int per_target_id() const { return per_target_id_; }
    float ema() const { return ema_; }

private:
    float per_target_ = 0.0f;    // 本目标最近一次未截断帧的 h/w
    int per_target_id_ = -1;     // 上者所属目标
    float ema_ = 0.0f;           // 跨目标 EMA（目标刚锁上就已被截断时的兜底）
};

}  // namespace ttbox::core::aim
