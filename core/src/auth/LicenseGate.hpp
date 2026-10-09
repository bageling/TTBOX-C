#pragma once
// LicenseGate.hpp — T1.10 授权执法快照（施工图：t1.09-t1.10-impl-spec.md §2.4 / §3）
//
// 单一真相源（§0.1）：进程内唯一授权执法态。写入 = publish()；读取 = 无锁快照拷贝。
// 三层（会话边界 / IPC 投影 / 板端诊断）只是**投影**，不得各自推导授权语义。
// 依赖：仅 LicenseStateMachine（纯头）+ LicenseStore（纯文件 I-O）⇒ 无 OpenSSL / 无网络，
//       AUTH=ON/OFF 两种构建都必须编译（CORE_SOURCES 无条件列表）。
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "auth/LicenseConstants.hpp"     // B-CONST-4：心跳 60/180 单点真源
#include "auth/LicenseStateMachine.hpp"  // LicenseState / LicenseStatus（纯头）
#include "auth/LicenseStore.hpp"         // StoreLoadResult

namespace ttbox::core::auth {

// ---- feature 名闭集常量（M2.03）----
// ★ 唯一权威 = LicenseStateMachine.hpp 的 known_features()（本处**不**另定义取值集合，
//   只把闭集里的四个名字提成常量，供 C++ 消费点避免散落字符串字面量）。
//   任何新增 feature 名**必须**先进 known_features()，否则被 fail-closed 丢弃、永不生效。
namespace feature_name {
inline constexpr const char* kCapture   = "capture";
inline constexpr const char* kInference = "inference";
inline constexpr const char* kAim       = "aim";
inline constexpr const char* kOta       = "ota";
}  // namespace feature_name

// 执法快照（不可变；publish 时整体替换，读侧只拷贝 shared_ptr）。
struct LicenseSnapshot {
    LicenseState state = LicenseState::kUnknown;
    std::string  plan = "none";          // none|trial|subscription|permanent
    bool         is_pro = false;
    // 功能位：唯一权威闭集见 LicenseStateMachine.hpp 的 known_features()。
    // 未激活 / 权威否定 / 闭集外名字 ⇒ 一律为空（fail-closed），feature_enabled 随之 false。
    std::vector<std::string> features;
    // UI 品牌：已被 sanitize_ui_brand() 过滤（[A-Za-z0-9_-]，≤32）；不可信态回落默认品牌。
    // 前端只消费本字段，**不得**自行拼接或信任其它来源的品牌字符串。
    std::string  ui_brand = "ttbox";
    // M2.05：卡号可读短码（由 license_id 单向派生；**仅可信态非空**，与 ui_brand 同闸门）。
    // 形如 "TTB-XXXX-XXXX"；不可信态 = ""（默认值）。仅供人念/对账，**非安全边界**。
    std::string  short_code;
    int64_t      expire_unix_ms = 0;     // 0 = 无期限/未知
    int64_t      grace_until_ms = 0;     // 会话宽限到期（仅 kExpired 且窗口内 > 0）
    std::string  last_error;             // 仅诊断
    int          heartbeat_interval_s = kHeartbeatIntervalSecDefault;

    // 决策函数（§A9.3 等价表 M1 子集）：kValid/kFallback → true；kExpired → 仅 grace 内 true；其余 false。
    bool ai_allowed() const;
    // features 空集 ⇒ 一律 false（§2.4）。
    bool feature_enabled(const char* f) const;
    // ★ M2.03：会话级 gate —— 整链能否启动。定义 = feature_enabled("capture")
    //   （无采集 = 无帧源 ⇒ 起整链无意义）。是 ai_allowed() 的**等价收窄**替代：
    //   只有可信态才填 features，故本函数已隐含 state 门（含 kExpired 宽限语义，与 ai_allowed 一致）。
    bool pipeline_allowed() const { return feature_enabled(feature_name::kCapture); }
    // capability 投影助手：语义 ≡ feature_enabled（同名一义，命名用于 IPC/Web 的"能力位"投影）。
    bool capability(const char* f) const { return feature_enabled(f); }
};

// wire 状态名（§3.2，小写）：由 daemon 态 + plan 派生（纯函数，可单测）。
std::string wire_state_name(const LicenseSnapshot& s);
// §3.3：activated ==（wire ∈ {active, expiring_soon, trial_active}）。
bool wire_activated(const LicenseSnapshot& s);

// 由 daemon 快照 + Store 恢复结果构造执法快照（自由函数，便于单测）。
LicenseSnapshot to_snapshot(const LicenseStatus& s, const StoreLoadResult& lr, int64_t now_ms);

// 进程内单例：唯一写入口 publish()；读路径仅锁一次取指针拷贝（禁在读路径做 IO/网络）。
class LicenseGate {
public:
    static LicenseGate& instance();

    void publish(const LicenseSnapshot& s);  // 唯一写入口（心跳/验证线程）
    LicenseSnapshot snapshot() const;        // 无锁读（拷贝 shared_ptr 后解引用）

    bool ai_allowed() const { return snapshot().ai_allowed(); }
    bool feature_enabled(const char* f) const { return snapshot().feature_enabled(f); }
    // ★ M2.03：会话级 gate（定义见 LicenseSnapshot::pipeline_allowed）。
    bool pipeline_allowed() const { return snapshot().pipeline_allowed(); }

private:
    LicenseGate() = default;
    mutable std::mutex mu_;
    std::shared_ptr<const LicenseSnapshot> cur_;  // 写：整体替换；读：锁内拷贝指针
};

}  // namespace ttbox::core::auth
