// OfflineCardClient.cpp — M2.01/M2.02：离线签名卡验证（fail-closed，无 OpenSSL）
#include "auth/OfflineCardClient.hpp"

#include <chrono>
#include <cstring>

#include "auth/LicenseKeys.hpp"
#include "auth/LicenseStateMachine.hpp"
#include "auth/ed25519/ed25519_verify.hpp"

namespace ttbox::core::auth {

namespace {
// 取当前墙钟 Unix 秒。
int64_t wall_now_unix_s() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
}  // namespace

// 离线卡验证：解析 → key_id → Ed25519 验签 → 设备绑定 → 过期；reject 即写 out_status。
// 恒返回 true（本地确定性判定，无网络类失败 ⇒ 状态机 fail-open 结构性不可达）。
bool OfflineCardClient::verify_once(const std::string& card_envelope,
                                    const std::string& bind_device,
                                    LicenseStatus& out_status,
                                    std::string* err_message) {
    out_status = LicenseStatus{};  // 权威否定前先归零（不残留调用方旧字段）

    const auto fail = [&](LicenseState st, const std::string& why) {
        out_status.state = st;
        out_status.last_error = why;
        out_status.bind_device = bind_device;
        if (err_message) *err_message = why;
        return true;  // ★ req_ok 恒 true：本地确定性判定，无网络类失败
    };

    // 空卡（未激活）：LicenseDaemon 对空卡根本不会调 verify_once
    // （thread_loop 先拦），此处防御性处理为 kInvalidCard。
    if (card_envelope.empty()) {
        return fail(LicenseState::kInvalidCard, "offline card: empty");
    }

    // 1) 解析 + 归一化（fail-closed；闭集/字符集唯一拦截点在 parse_license_card）
    LicenseCard card;
    std::string perr;
    if (!parse_license_card(card_envelope, &card, &perr)) {
        return fail(LicenseState::kInvalidCard, "offline card: " + perr);
    }

    // 2) key_id 必须命中内嵌公钥族（换钥即旧卡全废，fail-closed）
    if (card.key_id != kLicenseKeyId) {
        return fail(LicenseState::kInvalidCard,
                    "offline card: unknown key_id '" + card.key_id + "'");
    }

    // 3) Ed25519 验签（canonical 串 = 签名原文）
    std::array<uint8_t, 32> pk{};
    if (!license_public_key(&pk)) {
        return fail(LicenseState::kInvalidCard, "offline card: bad embedded key");
    }
    std::vector<uint8_t> sig;
    if (!license_base64_decode(card.signature_b64, &sig) || sig.size() != 64) {
        return fail(LicenseState::kInvalidCard, "offline card: bad signature encoding");
    }
    const std::string canonical = license_canonical(card);
    if (!ed25519::verify(sig.data(),
                         reinterpret_cast<const uint8_t*>(canonical.data()),
                         canonical.size(), pk.data())) {
        return fail(LicenseState::kInvalidCard, "offline card: signature mismatch");
    }

    // 4) 设备绑定（卡载 device 必须等于本板 bind_string() = cpu_serial）
    if (card.device != bind_device) {
        return fail(LicenseState::kBoundElsewhere,
                    "offline card: bound to another device");
    }

    // 5) 过期（expires_at == 0 ⇒ 永久）
    const int64_t now_s = now_unix_s_ >= 0 ? now_unix_s_ : wall_now_unix_s();
    if (card.expires_at != 0 && now_s >= card.expires_at) {
        out_status.expire_unix_ms = card.expires_at * 1000;
        return fail(LicenseState::kExpired, "offline card: expired");
    }

    // 6) 全过 ⇒ kValid + 签名卡内容投影（字段已归一化，零二次过滤）
    out_status.state = LicenseState::kValid;
    out_status.is_pro = card.is_pro;
    out_status.plan = card.plan;
    out_status.features = card.features;
    out_status.ui_brand = card.ui_brand;
    // M2.05：内部字段 license_id（唯一用途 = to_snapshot() 派生只读短码；不直接投影）。
    out_status.license_id = card.license_id;
    out_status.expire_unix_ms = card.expires_at == 0 ? 0 : card.expires_at * 1000;
    out_status.bind_device = bind_device;
    // 脱敏卡号（license_id 前 8 位）
    {
        std::string s = card.license_id;
        if (s.size() > 8) s = s.substr(0, 8) + "******";
        out_status.card = s;
    }
    if (err_message) err_message->clear();
    return true;
}

}  // namespace ttbox::core::auth
