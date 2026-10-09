#pragma once
// LicenseStore.hpp — T1.09 授权持久化（施工图：t1.09-t1.10-impl-spec.md §2.1）
//
// 职责：把"授权文档原文（license.json）+ 派生状态（state.json）"以**原子写**落盘，
//       并在读取时做**自愈**（state.json 损坏时从 license.json 的 issued_at 重建防回滚基线）。
// 依赖：纯 POSIX 文件 I-O + 仓库内置 Json；**无 OpenSSL / 无网络 / 无锁**。故 AUTH=ON/OFF
//       两种构建都必须编译（CORE_SOURCES 无条件列表）。
// ★ M2.07：doc 顶层容忍新增字段 `source`（"cloud"|"offline"）与 `card_mask`（云端卡密
//   激活写入，见 LicenseDaemon::activate_cloud）。load() 对未知字段宽容（向后兼容旧
//   license.json）；**但自 M2.07(D-D) 起额外解析顶层 `source` 以登记 `doc_is_cloud`**
//   （见下方 StoreLoadResult），供上层判定"该文档不是离线卡信封"，避免把它当离线卡验签。
//   is_downgrade 防降级逻辑不变（cloud 激活不参与 issued_at 基线）。
#include <cstdint>
#include <string>

namespace ttbox::core::auth {

// 磁盘恢复结果（load() 输出）。字段语义见 design.md §A7.1。
struct StoreLoadResult {
    bool     state_ok        = false; // state.json 存在且可解析
    bool     doc_ok          = false; // license.json 存在且非空
    bool     recovered_fresh = false; // state.json 损坏 → 基线从 doc 重建（自愈）
    std::string doc_json;             // license.json 原文（不解析，交给验证层）
    // ★ M2.07（D-D）：doc 是否为 **cloud 形**（顶层 source == "cloud"）。由 load() 解析得出，
    //   是"该文档不是离线卡信封"这一判定的**唯一真源**：Application::resolve_license_card()
    //   与 LicenseDaemon::restore_cloud_doc_locked() 都读此字段，禁止各自手搓字符串匹配。
    //   语义：cloud 形文档由 activate_cloud() 落盘，只能走云态恢复/维护，**绝不能**当离线卡验签。
    bool     doc_is_cloud    = false;
    int64_t  last_seen_issued_at   = 0; // 防降级单调基准（LIC-17）
    int64_t  server_time_floor_unix = 0; // 防回拨下界（§A7.3）
    int64_t  grant_expires_at      = 0; // trial online_grant 到期
    int64_t  grace_until           = 0; // 会话宽限到期
    std::string error;                  // 诊断
};

// resolve_license_card() 的 store 分支判定 —— **唯一真源**（M2.07.1 T3 可测性抽取）。
// 仅当文档可用（doc_ok）、**非 cloud 形**（!doc_is_cloud）、且非空时，才可作为
// “离线卡信封”交出（返回 doc_json）；否则返回空串。
// 语义要点（D-D）：cloud 形文档必须交云态恢复链（LicenseDaemon）处理，**绝不能**在
// 恢复链里当离线卡交出——否则 Application::run()→verify_now_blocking() 会把它离线验签
// 并抹掉 restore_cloud_doc_locked() 刚恢复的云态 ⇒ 重启后云激活丢失。
// 抽为独立纯函数以便宿主单测直接命中该判定（Application 与测试共用**同一实现**，
// 不留第二份逻辑）；不改变任何运行语义。
inline std::string offline_card_doc(const StoreLoadResult& r) {
    if (r.doc_ok && !r.doc_is_cloud && !r.doc_json.empty()) return r.doc_json;
    return {};
}

class LicenseStore {
public:
    // 目录默认 /var/lib/ttbox/license（0700，T1.01 fhs_init 已建）；文件 0600。
    explicit LicenseStore(std::string dir = "/var/lib/ttbox/license");

    // 读 license.json + state.json。**不抛**：任一损坏走自愈语义（见 .cpp / 规格 §2.2 / §6）。
    StoreLoadResult load() const;

    // 原子保存文档原文（保持 license.json 与内存一致）。失败不破坏旧文件。
    bool save_doc(const std::string& doc_json, std::string* error);

    // 原子保存派生状态（防回滚基准/时间下界/grant/grace）。未给出的项保持既有值。
    bool save_state(int64_t last_seen_issued_at,
                    int64_t server_time_floor_unix,
                    int64_t grant_expires_at,
                    int64_t grace_until,
                    std::string* error);

    // 纯逻辑：同 license_id 下 issued_at 不得回退（LIC-17 防降级）。
    static bool is_downgrade(int64_t doc_issued_at, int64_t last_seen_issued_at);

    const std::string& dir() const { return dir_; }

private:
    // 同目录 tmp → write+fsync → 回读自证 → rename（原子）→ fsync(父目录)。
    bool write_file_atomic_(const std::string& path,
                            const std::string& content,
                            std::string* error) const;
    std::string dir_;
};

}  // namespace ttbox::core::auth
