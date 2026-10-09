#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "auth/DeviceFingerprint.hpp"
// LicenseState / LicenseStatus 及降级状态机纯函数（apply_check_result）
// 定义于此头文件（T1.08 抽出，便于在无 OpenSSL 的默认构建下单测）。
#include "auth/LicenseStateMachine.hpp"
// T1.09：授权持久化（原子写 / 掉电自愈 / 防降级）。**无 OpenSSL 依赖**，故
// AUTH=ON/OFF 两种构建都要编（LicenseStore.cpp 在 CORE_SOURCES 基础列表；
// 见 t1.09-t1.10-impl-spec.md §2.8）。
// ★ M2：AUTH=OFF 的 DisabledAuth fail-open 桩已删——LicenseDaemon.cpp 本就无
//   OpenSSL 依赖，现无条件编译（真验签 = OfflineCardClient，fail-closed）。
#include "auth/LicenseStore.hpp"
// M2.01：离线卡结构（activate() 的结构校验用；实现见 LicenseCard.cpp）。
#include "auth/LicenseCard.hpp"

namespace ttbox::core::auth {

// 授权客户端接口（ILicenseClient）。
// 在线实现见 TtboxLicenseClient；协议见 server-api-contract.md
// （/api/client/*，JSON + HMAC-SHA256）。
class ILicenseClient {
public:
    virtual ~ILicenseClient() = default;

    // 发送一次在线校验请求；返回 true 表示请求本身成功（不代表卡有效）。
    // 我方在线协议见 server-api-contract.md（/api/client/*，JSON + HMAC-SHA256）。
    virtual bool verify_once(const std::string& card,
                             const std::string& bind_device,
                             LicenseStatus& out_status,
                             std::string* err_message = nullptr) = 0;
};

// 授权守护：后台线程，周期 heartbeat 与原 cardVerifyThreadFunc 等价
//  - 启动：立即 verify_once；成功后按心跳（默认 1h）
//  - 失败：按指数退避（30s → 2m → 10m → 30m max）
//  - 本地 Fallback：若上次已通过且当前时间 < expire_time，允许运行
class LicenseDaemon {
public:
    explicit LicenseDaemon(ILicenseClient& client);
    ~LicenseDaemon();

    // 卡号来源：配置 + 命令行 --license；优先使用命令行
    void set_card(const std::string& card_plain);

    // 读取当前内存授权态的快照拷贝（加锁）。
    LicenseStatus status_snapshot() const;
    bool allow_run() const;   // 是否允许 AI 主功能（Valid | Fallback）
    bool is_pro() const;      // Pro 功能：武器扩展模型、高级 FOV 选项

    // 启动/停止后台线程；不会阻塞调用者
    bool start();
    void stop();

    // 同步触发一次立即验卡（用于 --verify-only CLI 选项）
    bool verify_now_blocking(std::string* err_message = nullptr);

    // ---- M2.02：离线卡激活（ACTIVATE_LICENSE 的 daemon 侧原子序）----
    // ① 结构校验（parse_license_card，fail-closed，不碰内存态）
    // ② set_card + 立即 verify（OfflineCardClient：Ed25519 + 设备绑定 + 过期）
    // ③ **kValid 才落盘**（store_->save_doc 写 license.json；失败卡不落盘 ⇒ 防半激活）
    // 返回 false + error（拒绝原因）。成功后调用方负责把快照 publish 到 LicenseGate。
    bool activate(const std::string& card_envelope, std::string* error = nullptr);

    // ---- M2.07：云端卡密激活（ACTIVATE_CLOUD 的 daemon 侧原子序，D2 裁决）----
    // 云端 card-login 由 web 进程完成（HMAC 签名归 web 层）；core 只负责：
    //   ① expire_unix_ms > now 校验（fail-closed）
    //   ② features 经 normalize_features 收窄（闭集唯一权威 = LicenseStateMachine）
    //   ③ status 置 kValid（verified_at=now、heartbeat_interval=60）
    //   ④ store_->save_doc 落盘 cloud 形 license.json（source:"cloud"）
    // 返回 false + error。成功后调用方负责把快照 publish 到 LicenseGate。
    // card_mask/max_devices 仅随 doc 落盘与投影展示，不参与授权语义。
    bool activate_cloud(int64_t expire_unix_ms,
                        const std::vector<std::string>& features,
                        const std::string& plan,
                        const std::string& card_mask,
                        std::string* error = nullptr);

    // ---- M2.07：云端否定（到期/禁用）⇒ 立即锁定（D4 快路径）----
    // 置 kExpired + last_error=reason；**不落盘**（本地 license.json 保留供对账）。
    // publish 由调用方做（Application::handle_license_activate_cloud）。
    bool deactivate_cloud(const std::string& reason);

    // ---- 便于测试 / mock 的注入点 ----
    void set_heartbeat_interval_ms(int64_t ms);
    void set_backoff_base_ms(int64_t ms);

    // T1.09/T1.10：最近一次磁盘恢复结果（start() 时由 store_->load() 缓存）。
    // 供 LicenseGate 投影取 grace_until 等（无副作用、不加锁更安全故返回值拷贝）。
    StoreLoadResult store_load_result() const;

private:
    void thread_loop();
    void do_check_cycle_locked(const std::string& card_plain,
                                const std::string& bind_device);
    // M2.07：启动时从磁盘 cloud 形 license.json（source:"cloud"）恢复内存态。
    // 非 cloud 文档 / 解析失败 ⇒ 不动内存态（离线卡既有 resolve 链不受影响）。
    void restore_cloud_doc_locked();

    ILicenseClient& client_;
    mutable std::mutex mu_;
    LicenseStatus status_;
    std::string card_plain_;      // 完整卡号（内存中，不进入日志/JSON dump）
    DeviceFingerprint fp_;

    // ★ M2.07：当前授权来源是否为云端卡密（source:"cloud"）。云端授权无本地卡号，
    //   thread_loop 的空卡分支据此**跳过** kInvalidCard 归一（否则每次空卡周期都会
    //   冲掉云激活态）。activate_cloud()/restore_cloud_doc_locked() 置 true；
    //   离线卡 activate() 接管时置 false（两者互斥，见 impl-spec §0 D2/D7）。
    bool cloud_license_ = false;

    // T1.09：授权持久化（唯一落盘入口）。构造即分配；测试可注入目录（未来扩展点）。
    std::unique_ptr<LicenseStore> store_;
    // 最近一次 store_->load() 结果；供 store_load_result() 返回（Gate 投影用）。
    StoreLoadResult last_load_;

    int64_t heartbeat_ms_ = 3600 * 1000;   // 1h 默认
    int64_t backoff_base_ms_ = 30 * 1000;  // 30s
    int64_t backoff_cap_ms_ = 1800 * 1000; // 30m max
    int64_t cur_backoff_ms_ = 0;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace ttbox::core::auth
