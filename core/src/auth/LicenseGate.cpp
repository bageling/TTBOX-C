// LicenseGate.cpp — T1.10 授权执法快照实现（施工图：t1.09-t1.10-impl-spec.md §2.4 / §3.2 / §3.3）
//
// 语义红线（§0.2 fail-open / fail-closed 分明）：
//   · 网络类失败 → 由状态机（apply_check_result）已 fail-open（kFallback）；本层不额外改写。
//   · 权威否定（kExpired / kInvalidCard / kBoundElsewhere）→ fail-closed：
//     kExpired 仅宽限窗口内允许；其余一律不允许（ai_allowed=false）。
//   · 未激活（kUnknown）/ 未验证（kChecking）→ fail-closed（不允许），但**不阻断**透传/Web 管理。
#include "auth/LicenseGate.hpp"
#include "auth/LicenseConstants.hpp"   // B-CONST-4：心跳 60/180 单点真源（直接引用）

#include <utility>

#include "auth/LicenseShortCode.hpp"  // M2.05：short_code 派生（纯逻辑）

namespace ttbox::core::auth {

// 是否允许 AI：kValid/kFallback 放行，kExpired 仅宽限窗口内放行，其余拒绝（fail-closed）。
bool LicenseSnapshot::ai_allowed() const {
    switch (state) {
        case LicenseState::kValid:
        case LicenseState::kFallback:
            // 有效 / 离线 fail-open（本地缓存仍在有效期，状态机已保证）⇒ 允许 AI。
            return true;
        case LicenseState::kExpired:
            // 权威否定：仅宽限窗口内允许。to_snapshot 已按 now 把"到期未超宽限"写成
            // grace_until_ms > 0；超宽限则归零 ⇒ 此处即 fail-closed。
            return grace_until_ms > 0;
        default:
            // kUnknown / kChecking / kInvalidCard / kBoundElsewhere / kNetworkError(未 fail-open) ⇒ 拒绝
            return false;
    }
}

// 功能位查询：features 内含该名字即 true；空集/含非法名字一律 false。
bool LicenseSnapshot::feature_enabled(const char* f) const {
    if (f == nullptr) return false;
    for (const auto& x : features) {
        if (x == f) return true;
    }
    return false;  // 空集 ⇒ 一律 false
}

// 把内部状态枚举映射为对外的 wire 状态名（小写，见 §3.2）。
std::string wire_state_name(const LicenseSnapshot& s) {
    switch (s.state) {
        case LicenseState::kUnknown:
        case LicenseState::kInvalidCard:
            return "unactivated";
        case LicenseState::kChecking:
            // 不外泄：启动期首次为 unactivated；"保持上次 wire 值"在 publish() 内处理。
            return "unactivated";
        case LicenseState::kValid:
        case LicenseState::kFallback:
            return (s.plan == "trial") ? "trial_active" : "active";
        case LicenseState::kExpired:
            return "restricted";
        case LicenseState::kBoundElsewhere:
            return "restricted_hard";
        case LicenseState::kNetworkError:
            return "net_unreachable";
    }
    return "unactivated";  // 不可达（枚举穷尽）
}

// wire 口径的"已激活"判定：active / expiring_soon / trial_active 三者为真。
bool wire_activated(const LicenseSnapshot& s) {
    const std::string w = wire_state_name(s);
    return w == "active" || w == "expiring_soon" || w == "trial_active";
}

// 由 daemon 状态 + store 恢复结果构造执法快照：仅可信态投影 features/plan/ui_brand/短码。
LicenseSnapshot to_snapshot(const LicenseStatus& s, const StoreLoadResult& lr, int64_t now_ms) {
    LicenseSnapshot snap;
    snap.state = s.state;
    snap.is_pro = s.is_pro;
    snap.last_error = s.last_error;
    snap.heartbeat_interval_s = (s.heartbeat_interval > 0) ? s.heartbeat_interval
                                                           : kHeartbeatIntervalSecDefault;
    snap.expire_unix_ms = s.expire_unix_ms;

    // grace：仅 kExpired 且 now < grace 时保留（fail-closed，除非宽限内）。
    // ★ 必须在下面的 trusted 判定之前算完 —— trusted 要读 grace_until_ms。
    snap.grace_until_ms = 0;
    if (snap.state == LicenseState::kExpired && lr.grace_until > now_ms) {
        snap.grace_until_ms = lr.grace_until;
    }

    // ---- 可信态（签名卡内容的唯一放行闸门）----
    // 只有「授权被认可」的三种态才允许把卡下发的 features / plan / ui_brand 投影出去：
    //   kValid / kFallback（离线 fail-open，此时卡内容来自上一次成功验证，可信）
    //   kExpired 且仍在宽限窗口内（宽限是服务端给的延长期，卡内容仍有效）
    // 未激活（kUnknown / kChecking / kInvalidCard）与 kBoundElsewhere 一律 fail-closed：
    // 既不给功能位，也不透卡里的品牌串 —— 降级设备必须回到默认品牌，不得留下上次的痕迹。
    const bool trusted = (snap.state == LicenseState::kValid || snap.state == LicenseState::kFallback ||
                          (snap.state == LicenseState::kExpired && snap.grace_until_ms > 0));

    if (!trusted) {
        snap.plan = "none";
        snap.features.clear();
        snap.ui_brand = default_ui_brand();
        return snap;
    }

    // plan：卡下发的优先；缺失则按 is_pro 粗分（M1 兼容路径，字段语义不变）。
    snap.plan = s.plan.empty() ? (s.is_pro ? "subscription" : "permanent") : s.plan;

    // features：归一化（去空 / 去重 / 闭集过滤）。闭集外名字在此被丢弃 ⇒
    // 协议漂移或被篡改的名字不会变成可用功能位（fail-closed）。
    snap.features = normalize_features(s.features);

    // ui_brand：卡下发的优先，但必须过字符集校验（会被前端渲染）；不合法即回落默认。
    snap.ui_brand = sanitize_ui_brand(s.ui_brand);

    // M2.05：短码 = license_id 的单向派生（**仅可信态**；非法/空 id ⇒ ""）。
    // 与 ui_brand 同一闸门：降级设备不得留下上次卡的短码痕迹。
    snap.short_code = license_short_code_derive(s.license_id);

    return snap;
}

// 进程内单例访问点（函数局部 static，线程安全初始化）。
LicenseGate& LicenseGate::instance() {
    static LicenseGate g;  // 进程内单例（线程安全初始化）
    return g;
}

// 唯一写入口：锁内整体替换快照；checking/network 态不外泄（保留上次 wire 态）。
void LicenseGate::publish(const LicenseSnapshot& s) {
    std::lock_guard<std::mutex> lk(mu_);
    LicenseSnapshot ns = s;
    // §3.2/§3.3：kChecking / kNetworkError **不外泄、不单独成为 wire state**
    //   —— 保留上次非（checking|network）态用于 wire（activated 随上次）；
    //   本次的 last_error 仍保留供诊断。M1 无真网络层，此分支只在单测/未来启用。
    if ((s.state == LicenseState::kNetworkError || s.state == LicenseState::kChecking) && cur_ &&
        cur_->state != LicenseState::kNetworkError && cur_->state != LicenseState::kChecking) {
        ns.state = cur_->state;
    }
    cur_ = std::make_shared<const LicenseSnapshot>(std::move(ns));
}

// 无锁读快照：锁内拷贝 shared_ptr 后解引用；从未 publish 过则返回默认（kUnknown）。
LicenseSnapshot LicenseGate::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_) return *cur_;
    return LicenseSnapshot{};  // 默认 = kUnknown ⇒ unactivated（A23 期望）
}

}  // namespace ttbox::core::auth
