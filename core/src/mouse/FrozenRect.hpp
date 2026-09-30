// FrozenRect.hpp — V1.0.10：「冻结落点」的状态机（纯状态、无依赖）。
//
// 为什么需要它：检测只吃画面中心一块 crop（板端 416x416），近身目标的下半身落在
// crop 之外 ⇒ 可见框被下边界截断，h_obs 偏小，落点相对人体上飘（V1.0.08/09 的病灶）。
// V1.0.09 的修法是「按框宽外推身高」，但那是在**修一个坏量测**。
//
// V1.0.10 改为「拒绝坏量测」（对齐 yu 的 holding_previous 思路，2026-09-29 定调）：
//   腿一被切，就**停止更新框**，落点保持上一次能看全的那一帧。
//   数学上站得住：走近时框顶上升与身高变大互相抵消——「你正在瞄的那个点，
//   屏幕上本来就不该动」。现场数据复算：贴边那一刻冻结 ty≈669，继续走近后
//   真胸口≈672，误差仅 3px（外推法在同一场景 p50 误差约 14px）。
//
// 与 V1.0.09 外推的关系：**兜底**。目标一出现就已经被截（拐角撞脸）时没有
// 「能看全的上一帧」可冻 ⇒ 冻结器返回 false，调用方退回 V1.0.09 外推路径。
// 冻结不引入任何新配置项（业主定调：少加参数，用户看不懂的东西不加）。
//
// 语义细节：
//   · 冻结的是**整个 rect**（不是只冻高度）——只冻高度会让落点越走越偏上。
//   · 换目标（track id 变）⇒ 旧冻结框立即作废；新目标在第一次未截断帧重建。
//   · 目标退回可看全（远去）⇒ 恢复正常更新，冻结值被覆盖。
#pragma once

#include "common/Types.hpp"

namespace ttbox::core::aim {

class FrozenRectTracker {
public:
    void reset() {
        valid_ = false;
        id_ = -1;
    }

    // 每帧喂一次（仅在有可用目标的帧调用）。
    // bottom_clipped = 本帧框底是否贴到裁剪区下边界（判据与 V1.0.09 相同）。
    void observe(const DetectionBox& box, bool bottom_clipped, int track_id) {
        if (track_id != id_) {
            // 换目标：旧目标的冻结框对新目标是错的位置，立即作废。
            valid_ = false;
            id_ = track_id;
        }
        if (bottom_clipped) return;  // 被截断帧：保持上一帧（这正是「冻结」本身）
        if (!((box.x2 - box.x1) > 1.0f) || !((box.y2 - box.y1) > 1.0f)) return;  // 退化框不冻
        frozen_ = box;
        valid_ = true;
    }

    // 截断帧取用：有本目标的冻结框 ⇒ 拷出并返回 true；否则 false（调用方退回外推兜底）。
    bool frozen_for(int track_id, DetectionBox* out) const {
        if (!valid_ || track_id != id_) return false;
        *out = frozen_;
        return true;
    }

    bool valid() const { return valid_; }
    int id() const { return id_; }

private:
    DetectionBox frozen_{};  // 同一目标最近一次「未截断」帧的完整框
    int id_ = -1;            // 冻结框所属目标
    bool valid_ = false;
};

}  // namespace ttbox::core::aim
