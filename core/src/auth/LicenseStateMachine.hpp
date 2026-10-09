#pragma once

// LicenseStateMachine.hpp — 授权降级状态机的纯逻辑单元（T1.08）
//
// 背景（design.md §A8.0「前置缺陷修复」）：
//   LicenseDaemon::do_check_cycle_locked() 曾用 `status_ = out;` 整体覆盖状态，
//   把"上一次成功验证"留下的 verified_at_ms / expire_unix_ms 一并清零，导致
//   紧随其后的 kFallback 判定（要求这两个字段 > 0）**永远不可达**。
//   后果：一旦接上运行期执法点，网络一抖状态就停在错误态、无法回退 ⇒「断网就停」，
//   直接违反"网络错误必须 fail-open"的降级红线。
//
// 本单元把该状态迁移抽成**不依赖 OpenSSL / 不依赖网络 / 不依赖锁**的纯函数，
// 使它能被默认构建（TTBOX_CORE_BUILD_AUTH=OFF）直接单测。
// 状态枚举与 LicenseStatus 亦移入此处，作为状态机的单一权威定义，
// LicenseDaemon.hpp 通过包含本头文件复用（不改变字段/取值语义）。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// B-CONST-4：心跳间隔/超时单点真源（kHeartbeatIntervalSecDefault /
// kHeartbeatTimeoutSecDefault），本头及各授权单元一律引用，不再各写 60/180。
#include "auth/LicenseConstants.hpp"

namespace ttbox::core::auth {

// 授权状态枚举（原定义于 LicenseDaemon.hpp，语义与取值完全保持）
enum class LicenseState : int {
    kUnknown = 0,       // 启动时未验证
    kChecking = 1,      // 正在与服务端握手
    kValid = 2,         // 授权有效（Pro或普通）
    kExpired = 3,       // 卡已过期
    kInvalidCard = 4,   // 卡号非法
    kBoundElsewhere = 5,// 卡已绑定到其他 cpu_serial
    kNetworkError = 6,  // 无法访问授权服务器
    kFallback = 7,      // 本地缓存通过（无法连网时）
};

// 授权状态快照（原定义于 LicenseDaemon.hpp，字段与默认值完全保持）
struct LicenseStatus {
    LicenseState state = LicenseState::kUnknown;
    bool is_pro = false;        // Pro 版 / 普通版
    int64_t expire_unix_ms = 0; // 过期时间点（Unix ms，0 = 未知）
    std::string card;           // 当前卡号（前 8 位 + ****** 脱敏公开）
    std::string bind_device;    // 当前绑定 cpu_serial
    std::string last_error;     // 上次失败原因（仅调试用，不对外公开敏感）
    int64_t verified_at_ms = 0; // 上次成功验卡时间
    int64_t next_check_ms = 0;  // 下次必查时间（心跳周期）
    std::string cached_token;   // 本地缓存 token（Fallback 时校验签名）
    int heartbeat_interval = 60;  // 心跳间隔秒（来自 app-info / 心跳响应）
    int heartbeat_timeout = 180;  // 心跳超时秒

    // ---- 以下三项为「签名卡内容」（M2）：由服务端下发、随卡一起被签名保护，----
    //      由到 LicenseGate 快照做投影与门控（唯一投影点 = to_snapshot）。
    //      本结构**不**做合法性过滤，过滤在 to_snapshot（闭集/字符集），避免两个过滤点。
    std::vector<std::string> features;  // 功能位（闭集子集，见 known_features()）
    std::string plan;                   // 套餐：none|trial|subscription|permanent
    std::string ui_brand;               // UI 品牌标识（空 ⇒ 回落 default_ui_brand()）
    // M2.05：卡号（**内部字段**，唯一用途 = 供 to_snapshot() 派生只读短码）。
    // ★ 不直接投影给前端（只投 short_code）：避免无谓暴露卡序列号。
    // 由 OfflineCardClient 在 kValid 时按卡内 license_id 填充；其余态为空。
    std::string license_id;
};

// ---- 签名卡内容：闭集与归一化（纯函数；无 OpenSSL / 无网络 / 无文件 I-O）----
//
// 授权卡内容虽经签名通道送达，仍**不得**直接穿透到运行期与前端：
//   · features 只认闭集内的名字 ⇒ 协议漂移、拼写错误、被篡改的名字一律丢弃（fail-closed）。
//   · ui_brand 会被前端渲染 ⇒ 只放行 [A-Za-z0-9_-] 且 ≤ 32 字符，其余回落默认品牌。
//     本处是「卡内容不可信字符」的唯一拦截点，前端不得再做二次拼接假设。
//
// 闭集是**唯一权威定义**：Metrics.hpp 的 LicenseStatusBlock::features 注释、
// LicenseGate.hpp 的 LicenseSnapshot::features 注释均与此对齐，勿各写一份。
inline const std::vector<std::string>& known_features() {
    static const std::vector<std::string> k = {"capture", "inference", "aim", "ota"};
    return k;
}

// 判断某名字是否属闭集 known_features()。
inline bool is_known_feature(const std::string& f) {
    const auto& k = known_features();
    return std::find(k.begin(), k.end(), f) != k.end();
}

// 归一化：丢弃空串 → 丢弃闭集外名字 → 去重（保留首次出现顺序）。
inline std::vector<std::string> normalize_features(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (const auto& f : in) {
        if (f.empty()) continue;
        if (!is_known_feature(f)) continue;
        if (std::find(out.begin(), out.end(), f) != out.end()) continue;
        out.push_back(f);
    }
    return out;
}

// 卡未下发 / 不可信时的默认 UI 品牌（前端据此回落，故必须有确定值，不能是空串）。
inline const char* default_ui_brand() { return "ttbox"; }

// UI 品牌字符集校验：非空 ∧ ≤32 字符 ∧ 仅 [A-Za-z0-9_-] ∧ 首字符为字母或数字。
// 不合法 ⇒ 返回默认品牌（绝不返回空串，前端无需自行兜底）。
inline std::string sanitize_ui_brand(const std::string& in) {
    if (in.empty() || in.size() > 32) return default_ui_brand();
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z');
        if (i == 0 && !alnum) return default_ui_brand();
        if (!alnum && c != '_' && c != '-') return default_ui_brand();
    }
    return in;
}

// 判定"网络类失败"——这是 fail-open（进入 kFallback）的唯一入口。
//
//   req_ok == false            → 请求本身未成功（连不上 / 超时 / TLS 失败）
//   state  == kNetworkError    → 客户端明确回报网络错误
//
// 明确的服务端权威否定（kExpired / kInvalidCard / kBoundElsewhere）**不属于**网络类，
// 因此绝不 fail-open —— 这正是验收③（服务器明确 expired → 停 AI）成立的结构保证。
inline bool is_network_class_failure(bool req_ok, LicenseState out_state) {
    if (!req_ok) return true;
    return out_state == LicenseState::kNetworkError;
}

// 单次检查周期的状态迁移（纯函数；除对 status / cur_backoff_ms 的写入外无副作用）。
//
// 参数：
//   status         [in/out] 累积的授权状态（进入前持有上次成功验证的时间戳）
//   out            [in]     本次 client_.verify_once() 填充的原始结果
//   req_ok         [in]     请求本身是否成功（true=拿到响应，不代表卡有效）
//   now_ms         [in]     当前时间（Unix ms）
//   heartbeat_ms   [in]     默认心跳间隔（ms，服务端未下发时使用）
//   backoff_base_ms/in 指数退避基数（首次失败值）
//   backoff_cap_ms [in]     退避上限
//   cur_backoff_ms [in/out] 当前退避值（成功清零，失败按 2 倍递增并封顶）
//
// 迁移规则：
//   ① 成功（req_ok && out.state == kValid）：
//        采纳本次结论，刷新 verified_at_ms = now_ms 与 next_check_ms，退避清零。
//   ② 失败：
//        采纳本次结论（state / last_error / expire / 心跳参数…），
//        **但不抹掉**上一次成功验证留下的时间戳：
//          verified_at_ms  ← 上次成功时间（保留历史）
//          expire_unix_ms  ← 本次下发 > 0 则用本次，否则沿用历史
//        签名卡内容 features / plan / ui_brand 同族处理：**仅网络类失败**回落历史值
//        （否则网络一抖就清空功能位，违反 fail-open）；权威否定一律清空（fail-closed）。
//        仅当"网络类失败"且历史仍在有效期内时，置 kFallback（fail-open）。
//        （与 design.md §A8.0 的 `if (/* 网络类失败 */ ...) derive_offline_state(...)` 一致）
//
// 健壮性保证（T1.08 追加）：
//   ① 请求本身失败（req_ok == false）时，客户端回报的任何"成功"都不可信——
//      绝不采纳 kValid；统一归一化为 kNetworkError，交由 fail-open 逻辑处理。
//   ② expire_unix_ms == 0 视为**无期限**（永久授权）→ 亦应 fail-open：
//      "历史仍在有效期内"的判据改为 (expire == 0 || now < expire)。
inline void apply_check_result(LicenseStatus& status,
                               const LicenseStatus& out,
                               bool req_ok,
                               int64_t now_ms,
                               int64_t heartbeat_ms,
                               int64_t backoff_base_ms,
                               int64_t backoff_cap_ms,
                               int64_t& cur_backoff_ms) {
    const int64_t prev_verified_at = status.verified_at_ms;
    const int64_t prev_expire = status.expire_unix_ms;
    // 签名卡内容与时间戳同族：网络类失败必须原样保留（见下方失败路径说明）。
    const std::vector<std::string> prev_features = status.features;
    const std::string prev_plan = status.plan;
    const std::string prev_ui_brand = status.ui_brand;
    const std::string prev_license_id = status.license_id;  // M2.05：短码派生源，同族保留

    // 采纳本次检查真正产生的字段
    status = out;

    // 健壮性①：请求未成功 → 客户端回报的结论不可信，绝不采纳 kValid。
    // （否则 out.state==kValid 且 req_ok==false 时会把 kValid 泄漏出去。）
    if (!req_ok) {
        status.state = LicenseState::kNetworkError;
    }

    if (req_ok && out.state == LicenseState::kValid) {
        // 成功：刷新验证时间与下一次心跳
        status.verified_at_ms = now_ms;
        const int64_t hi_ms = out.heartbeat_interval > 0
                                  ? static_cast<int64_t>(out.heartbeat_interval) * 1000
                                  : heartbeat_ms;
        status.next_check_ms = now_ms + hi_ms;
        cur_backoff_ms = 0;
        return;
    }

    // 失败：保留上一次成功验证留下的时间戳（本任务修复的核心）
    status.verified_at_ms = prev_verified_at;
    status.expire_unix_ms = (out.expire_unix_ms > 0) ? out.expire_unix_ms : prev_expire;

    // 签名卡内容（features / plan / ui_brand）与时间戳**同理**：
    //   · 网络类失败 → 服务端本轮没说话，out 的这三项必为空（客户端解析失败路径不填）。
    //     不回落 ⇒ 一次网络抖动就把已授权的功能位清空 ⇒ 违反 fail-open 红线（"断网就停"）。
    //   · 权威否定（kExpired / kInvalidCard / kBoundElsewhere）→ 服务端明确拒绝，
    //     必须清空（fail-closed）。此时**不回落**即为清空，正是所需行为。
    // 故回落只在网络类失败分支内做，与 fail-open 的判据同一来源，不另立条件。
    if (is_network_class_failure(req_ok, out.state)) {
        if (status.features.empty()) status.features = prev_features;
        if (status.plan.empty()) status.plan = prev_plan;
        if (status.ui_brand.empty()) status.ui_brand = prev_ui_brand;
        if (status.license_id.empty()) status.license_id = prev_license_id;  // M2.05
    }

    // 仅网络类失败 fail-open → kFallback（本地缓存仍在有效期内）
    // 健壮性②：expire == 0 视为无期限（永久卡）→ 不因"无到期时间"而拒绝 fail-open。
    //
    // ★ 2026-09-23 复核：这条**是设计，不是 bug**——`LicenseCard.hpp` 的契约写明
    //   `"expires_at": 0 // unix 秒；0 = 永久`，OfflineCardClient 与 LicenseDaemon
    //   的到期判据都按同一口径写，且有用例 `license_sm_zero_expire_is_permanent_and_
    //   falls_back` 钉着。删掉它会让**永久卡一断网就停功能**，比原来更糟。
    //   真正的隐患在客户端：expireAt **存在但解析失败**时字段静默留 0，一张订阅卡
    //   就可能被当成永久卡。已在 TtboxLicenseClient 两处解析点修掉（解析失败即判响应无效）。
    const bool never_expires = (status.expire_unix_ms == 0);
    if (is_network_class_failure(req_ok, out.state) &&
        status.verified_at_ms > 0 &&
        (never_expires || now_ms < status.expire_unix_ms)) {
        status.state = LicenseState::kFallback;
    }

    // 指数退避：backoff_base → 2x → 4x → … 封顶 backoff_cap
    if (cur_backoff_ms <= 0) {
        cur_backoff_ms = backoff_base_ms;
    } else {
        cur_backoff_ms = std::min(cur_backoff_ms * 2, backoff_cap_ms);
    }
}

}  // namespace ttbox::core::auth
