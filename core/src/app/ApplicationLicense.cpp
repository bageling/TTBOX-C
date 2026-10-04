// ApplicationLicense.cpp — 授权状态查询（4 个）
//
// ★ 由 Application.cpp 按职责拆分而来（2026-10-04 core 结构治理 S1）。
//   拆分方式 = **只搬定义，不改任何逻辑**：成员函数仍是 Application 的同一个类的成员，
//   只是定义落在别的编译单元（类外定义可跨 TU 分散，这是 C++ 标准允许的）。
//   ⇒ 外部行为、符号名、ABI 全部不变；唯一变化是 .o 的组织方式。
//
// 本文件负责：授权状态查询（4 个）
// ★ 这一组与其它三组**零调用耦合**（机械枚举验证：4 个函数只被外部/IPC 调用，
//   组内 4 个互不调用，也不调任何 Application 工具）⇒ 独立成 TU 最干净。
#include "app/Application.hpp"
#include "app/ApplicationInternal.hpp"   // strip（inline 共享）
#include "auth/LicenseGate.hpp"
#include "auth/OfflineCardClient.hpp"    // M2.01/M2.02：离线签名卡验证（fail-closed）
#include "common/Logger.hpp"
#include "common/Paths.hpp"        // A-PATH-5：系统 license 文件路径唯一真源
#include <fstream>

using ttbox::core::app_internal::strip;

namespace ttbox::core {

bool Application::license_allow_run() const {
    return license_daemon_ && license_daemon_->allow_run();
}
bool Application::license_is_pro() const {
    return license_daemon_ && license_daemon_->is_pro();
}
auth::LicenseStatus Application::license_status_snapshot() const {
    return license_daemon_ ? license_daemon_->status_snapshot()
                           : auth::LicenseStatus{};
}
std::string Application::resolve_license_card(const std::string& cli_license) const {
    if (!cli_license.empty()) return cli_license;

    // 系统路径 /etc/ttbox/license.key：与原 aibox /etc/aibox/ 1:1 对齐语义
    {
        std::ifstream f(paths::kSystemLicenseFile);
        if (f) {
            std::string s;
            std::getline(f, s);
            s = strip(s);
            if (!s.empty()) return s;
        }
    }

    // 配置 fallback（开发期）
    const std::string cfg = config_.get_string("license_card_key", "");
    if (!cfg.empty()) return cfg;

    // M2.02：激活过的卡持久化在 LicenseStore（license.json = 信封原文）。
    // 重启后的恢复链：--license > /etc/ttbox/license.key > config > store。
    // （不解析、不验签——读原文交给 LicenseDaemon.verify_once。）
    // ★ M2.07（D-D）：**cloud 形文档不是离线卡信封** —— 不得作为"卡"交给 OfflineCardClient
    //   （否则 Application::run()→verify_now_blocking() 会把它离线验签并抹掉 LicenseDaemon
    //   刚由 restore_cloud_doc_locked() 恢复的云态 ⇒ 重启后云激活丢失）。云态恢复归
    //   LicenseDaemon 负责，故对 cloud 形文档返回空。判定唯一真源 = StoreLoadResult::doc_is_cloud。
    {
        auth::LicenseStore store;
        const auth::StoreLoadResult lr = store.load();
        // 判定唯一真源 = auth::offline_card_doc()（含 D-D 守卫：cloud 形文档不得当离线卡）。
        const std::string doc = auth::offline_card_doc(lr);
        if (!doc.empty()) return doc;
    }
    return {};
}

}  // namespace ttbox::core
