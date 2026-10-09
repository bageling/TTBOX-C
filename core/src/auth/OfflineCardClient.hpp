#pragma once
// OfflineCardClient.hpp — M2.01/M2.02：离线签名卡验证客户端（ILicenseClient 实现）
//
// 语义（与在线客户端 TtboxLicenseClient 对照）：
//   · verify_once(card, bind_device, out, err) **恒 req_ok=true**（本地确定性计算，
//     不存在"网络类失败"）⇒ LicenseStateMachine 的 fail-open（kFallback）对离线卡
//     **结构性不可达**——一切拒绝（签名/绑定/过期）都是权威否定 ⇒ fail-closed。
//     这正是商业化门控要的语义：无卡/坏卡/他板卡 ⇒ AI 不跑。
//   · 状态映射：签名/格式/key_id 错 ⇒ kInvalidCard；device != bind_device ⇒
//     kBoundElsewhere；expires_at != 0 且 now >= expires_at ⇒ kExpired；全过 ⇒ kValid。
//   · features/plan/ui_brand/is_pro/expire_unix_ms 从卡内容投影（归一化由
//     parse_license_card 完成，闭集/字符集拦截点唯一）。
//
// 依赖：LicenseCard（纯）+ ed25519_verify（自包含 TweetNaCl 路径，无 OpenSSL）
//       ⇒ AUTH=ON/OFF 两种构建都编译；出货向量（AUTH=OFF）从本类获得真验签。
#include "auth/LicenseCard.hpp"
#include "auth/LicenseDaemon.hpp"  // ILicenseClient / LicenseStatus

#include <cstdint>
#include <string>

namespace ttbox::core::auth {

class OfflineCardClient : public ILicenseClient {
public:
    OfflineCardClient() = default;
    ~OfflineCardClient() override = default;

    // now_unix_s 注入口（默认墙钟；单测注入固定时间测过期边界）。
    void set_now_unix_s(int64_t s) { now_unix_s_ = s; }

    // 验卡：解析→key_id→Ed25519→绑卡→过期；任一不过即按类置状态，恒返回 true（无网络类失败）。
    bool verify_once(const std::string& card_envelope,
                     const std::string& bind_device,
                     LicenseStatus& out_status,
                     std::string* err_message = nullptr) override;

private:
    int64_t now_unix_s_ = -1;  // <0 ⇒ 用系统墙钟
};

}  // namespace ttbox::core::auth
