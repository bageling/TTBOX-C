// Application.hpp — C++ Core 应用生命周期（initialize / run / shutdown）
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "auth/LicenseDaemon.hpp"        // 提供 ILicenseClient（成员 unique_ptr 只需接口声明）
#include "common/Metrics.hpp"
#include "common/Paths.hpp"            // A-PATH-5：IPC socket 默认单点真源
#include "config/ConfigManager.hpp"
#include "ipc/IpcServer.hpp"
#include "model/ModelManagement.hpp"
#include "model/RuntimeProfile.hpp"
#include "output/IHidOutput.hpp"
#include "runtime/CoreRuntime.hpp"

namespace ttbox::core {

// ---- 进程级 shutdown 标志（S 拆分前提）--------------------------------
// ★ 为什么从 Application.cpp 的匿名命名空间提到这里：
//   原来 `g_shutdown_requested` + `shutdown_flag()` 定义在 Application.cpp 的
//   **匿名命名空间**里。Application.cpp 拆成 4 个编译单元后，`request_shutdown()`
//   （写）与 `run()`（读）**分处不同 TU** —— 若标志仍留在匿名命名空间，
//   两个 TU 各得一份独立静态副本 ⇒ **request_shutdown() 置的位 run() 永远读不到**
//   ⇒ 关机信号静默丢失，进程只能被 SIGKILL。
//   （这类"拆分后各拿一份静态状态"的错**不会编译报错、不会崩**，只是功能静默失效。）
// 修法：声明进头文件（类外 inline 变量，C++17 起每个 TU 共享同一实体）。
inline std::atomic<bool> g_shutdown_requested{false};

inline std::atomic<bool>& shutdown_flag() { return g_shutdown_requested; }

// Application — 应用主类："总入口"。
// 职责：解析命令行/配置 → 授权校验 → 组装 CoreRuntime → 启动 IPC 服务
//       → 事件循环（自动重试/心跳）→ 优雅退出。
// 输入：命令行参数（--config 路径 / --ipc socket 路径）
// 输出：运行中的完整系统（核心 + IPC + 授权）
class Application {
public:
    Application() = default;
    ~Application();

    // 解析 CLI + 初始化 Logger/Config/IPC/CoreRuntime/授权。
    // 成功返回 0；失败返回非 0（含明确错误日志）。
    int initialize(int argc, char** argv);

    // 事件循环：启动 CoreRuntime，阻塞直到收到 shutdown 请求。
    // 每 tick 执行状态同步 + IPC 心跳；退出时按序停止 CoreRuntime。
    void run();

    // 请求退出（线程安全；signal handler 可直接调用，仅置原子标志）。
    static void request_shutdown();

    // 清理：停止 CoreRuntime → 停止 IPC → 停止 授权 → 写日志。
    void shutdown();

    bool running() const { return running_.load(); }
    const ConfigManager& config() const { return config_; }
    SystemStatus status() const;

    // 对外查询接口（授权状态 / 卡号 / Pro 功能）
    auth::LicenseStatus license_status_snapshot() const;
    bool license_allow_run() const;
    bool license_is_pro() const;

private:
    // 供 IpcServer providers 使用
    SystemStatus status_provider() const;
    JsonValue config_provider() const;

    // SET_CONFIG 原子更新（解析→validate→RuntimeConfig.update→落盘，任一失败不污染现配置）。
    // 返回 false 时 error 说明原因；persisted 表示是否写入配置文件。
    bool handle_config_update(const JsonValue& profile_json, std::string* error, bool* persisted);
    // RUNTIME_CONTROL 启停（复用 CoreRuntime start/stop，不改状态机）
    bool handle_runtime_control(const std::string& action, std::string* error);
    // V1.0.50 硬件写入：APPLY_EDID（写 config + 调 edid_apply.sh）/ SET_USB_MODE
    //（改 systemd 单元 USB_PROXY_MODE + 重载）。core 跑 User=root，是唯一有权限的层。
    bool handle_hardware_action(const std::string& action, const JsonValue& params,
                                JsonValue* data, std::string* error);
    // R5 用户启停意愿持久化：把显式 start/stop 落盘，供 core 重启后还原（OTA 更新会
    // systemctl restart ttbox-core，不还原就会出现"没点启动却自己跑起来"）。
    // 无记录时不动默认值（保持历史语义 want=true）。
    void load_runtime_intent();
    bool persist_runtime_intent(bool want_running);
    // R6 启动意图裁决（唯一入口）：先判"刚更新过"（版本更替 ⇒ 强制停止并落盘），
    // 否则按 R5 还原用户显式意愿。state_dir 由 initialize() 解析（TTBOX_STATE > 默认）。
    void apply_startup_runtime_intent(const std::string& state_dir);
    // M2.02：ACTIVATE_LICENSE（离线卡激活 + Gate publish + wire 投影；fail-closed）
    bool handle_license_activate(const std::string& card_envelope,
                                 JsonValue* data, std::string* error);
    // M2.07：ACTIVATE_CLOUD（云端卡密激活 + Gate publish + wire 投影；契约 §3.2）
    bool handle_license_activate_cloud(const JsonValue& params,
                                       JsonValue* data, std::string* error);
    // 模型管理（v0.3）：桥接 ModelRegistry（import/validate/install/activate/remove/list）
    bool handle_model_import(const std::string& src_path, const std::string& model_id,
                             const std::string& label, const std::string& source_format,
                             const std::string& sha256, std::string* error);
    JsonValue handle_model_list();
    bool handle_model_validate(const std::string& model_id, std::string* error);
    bool handle_model_install(const std::string& model_id, std::string* error);
    bool handle_model_activate(const std::string& model_id, std::string* error);
    bool handle_model_set_concurrency(const std::string& model_id, int count, std::string* error);

    // 模型热切换：stop → 用新 active 模型重建 Worker 参数 → start。
    // 返回 true = 新模型已加载且完成首次真实推理（running_model_id 已提交）。
    // 返回 false = 新模型加载/首帧失败，已回滚 active 到旧模型并恢复其运行。
    // capture_missing（可选出参）：true 表示失败原因是**采集侧一帧都没进来**
    //   （HDMI 无信号 / 信号源未输出），而不是模型本身有问题。调用方据此给可操作文案，
    //   并且回滚时不再把"旧模型同样没画面可验证"误报成回滚失败。
    bool switch_active_model_runtime(const std::string& new_model_id, std::string* error,
                                     bool* capture_missing = nullptr);

    // 无画面预检：采集在跑却在 probe_ms 内一帧都收不到 ⇒ 判定没有画面输入。
    // 采集未启动（capture 为空或未 running）时无判据 ⇒ 返回 true 不拦截。
    bool has_capture_signal(int probe_ms = 200) const;

    // T02 启动死锁修复：从"待配置"降级态重建 CoreRuntime（调用方须持生命周期锁）。
    // 未选模型/参数缺失时进程不退出，由本方法在 MODEL_ACTIVATE 或主循环重试时拉起。
    bool try_resume_from_degraded(std::string* error);

    bool handle_model_remove(const std::string& model_id, std::string* error);

    // 激活成功后同步 RuntimeProfile.model_id 与 active.json 一致（先落盘、后发布内存）。
    // 返回 false 时调用方必须将模型切换事务回滚，禁止留下配置与实际运行模型脱节。
    bool sync_model_id_to_profile(const std::string& model_id, std::string* error = nullptr);

    // RuntimeProfile 唯一持久化入口：将 canonical profile 合入宿主配置，
    // 通过同目录临时文件 + 原子 rename 发布，避免断电/崩溃留下半截 JSON。
    bool persist_runtime_profile(const RuntimeProfile& profile, std::string* error = nullptr);

    // 输出总闸（output_enabled）一次性自愈：出厂基线曾把它固定为 false 且无人覆盖，
    // 等价出厂即封死注入。仅当设备层未显式设置时才补写 true（只补不盖）。
    // 必须在读取 output_enabled 之前调用。返回 true 表示发生了迁移或尝试过迁移。
    bool migrate_output_enabled(std::string* note = nullptr);

    // 从配置构造 CoreRuntime 参数。
    // ★ M2.03：增 `gates`（特性级启停）—— `gates.inference==false` 时**跳过**模型解析/校验，
    //   使"仅采集"（受限卡 features=[capture]）在**未选模型**时也能起（否则 B6 必 FAIL）。
    //   `gates` 由 Application 在会话边界读一次 LicenseGate 快照后下传（§0.2）。
    bool build_runtime_params(CoreRuntime::Params& out_params,
                              const CoreRuntime::FeatureGates& gates,
                              std::string* error);

    // ★ M2.03：会话边界读一次 LicenseGate 快照 → 三项特性 gate（唯一读点，不每帧查）。
    CoreRuntime::FeatureGates current_feature_gates() const;
    // ★ M2.03：卡内 ui_brand 的大写形（水印文本用；不可信态回落 "TTBOX"）。只读快照，不推导授权。
    std::string brand_upper() const;
    // ★ M2.03：诊断文案助手 —— 点名当前缺失的 feature（capture/inference/aim），供 WARN/错误文案用。
    std::string gate_missing_summary() const;
    // ★ M2.03：按 gates 计算预览降级参数（全功能 ⇔ capture∧inference∧aim；否则 ≤5fps + 水印）。
    //   文本 = 卡内 ui_brand 大写形 + " - LIMITED"（全 ASCII，内嵌位图字体可绘）。
    void apply_preview_degrade(CoreRuntime::Params* params,
                               const CoreRuntime::FeatureGates& gates) const;

    // 加载卡号：优先级 --license > /etc/ttbox/license.key > 配置 license_card_key
    std::string resolve_license_card(const std::string& cli_license) const;

    ConfigManager config_;
    mutable std::mutex config_persist_mutex_;
    // CoreRuntime 生命周期事务锁：保护 MODEL_ACTIVATE、RUNTIME_CONTROL、主循环自动重试
    // 与状态采集，禁止多个 IPC 线程同时 stop/init/start/collect。
    mutable std::mutex runtime_lifecycle_mutex_;
    IpcServer ipc_;
    // IPC socket 路径优先级链（T01）：--ipc 参数 > TTBOX_IPC_SOCKET 环境变量 > 此默认值。
    // 默认迁入 /run/ttbox/（FHS tmpfs）：/tmp 为 1777 全局可写，任意本地用户可抢先
    // 创建同名 socket 劫持控制通道；/run/ttbox 由 systemd RuntimeDirectory 建管。
    std::string ipc_path_ = paths::kIpcSocketDefault;
    std::string config_path_;
    std::atomic<bool> running_{false};
    double start_time_ms_ = 0.0;
    bool initialized_ = false;
    bool verify_only_ = false;  // --verify-only：授权完立即退出，不启推理

    // ---- 待配置降级态（T02 启动死锁修复）----
    // "等待用户配置"绝不能是致命错误：模型未选择/参数缺失时进程必须存活并暴露 IPC，
    // 由 Web 引导用户补全后经 MODEL_ACTIVATE / 自动重试拉起 runtime。
    // true = core_runtime_ 已分配但未完成 initialize（run() 会周期性尝试重建）。
    bool runtime_waiting_config_ = false;
    std::string degraded_reason_;  // 降级原因（如 MODEL_NOT_SELECTED），恢复时清空


    // ---- 核心链路（接入 Application 生命周期）----
    RuntimeConfig runtime_config_;
    std::shared_ptr<output::IHidOutput> hid_output_;
    std::unique_ptr<CoreRuntime> core_runtime_;
    bool runtime_started_ = false;
    // 期望运行标志（自动启停核心）：true=应保持 runtime 运行，false=用户手动停止。
    // 开机/start 时置 true；用户 /api/control/stop 置 false。主循环据此自动重试/自恢复。
    // R5：进程启动时由 load_runtime_intent() 用上次显式意愿覆盖（无记录则保持 true）。
    std::atomic<bool> want_runtime_running_{true};
    // 用户启停意愿文件路径（R5），initialize() 解析：TTBOX_STATE > paths::kStateDirDefault
    // 目录 + paths::kRuntimeIntentFileName。
    std::string runtime_intent_path_;
    // 更新冒烟自检（2026-09-20 方案B）：命中"刚更新过"时先把 AI 流水线跑起来满足旧版
    // 更新器的健康门禁（要求 IPC current_model_id 非空 = 模型真跑过首帧），等更新器把
    // ota_status.json 落成 SUCCESS/FAILED 后再停回停止态（业主约定：更新后不点启动不跑）。
    std::atomic<bool> post_update_smoke_{false};
    std::string post_update_smoke_expected_version_;
    double post_update_smoke_deadline_ms_ = 0.0;
    // ota_status.json 路径（与 runtime_intent_path_ 同目录），供主循环自检收尾读取。
    std::string ota_status_path_;
    // 模型仓库（v0.3）：root = 配置 model_registry_root 或 <项目>/models
    std::unique_ptr<ModelManagement> model_management_;
    std::string running_model_id_;
    std::string model_failure_code_;
    std::string model_failure_message_;

    // ---- 授权（等价原 aibox-bl cardVerifyThreadFunc）----
    std::unique_ptr<auth::ILicenseClient> license_client_;
    std::unique_ptr<auth::LicenseDaemon> license_daemon_;
    std::string license_override_pro_endpoint_;   // --debug-license-pro-endpoint
    std::string license_override_normal_endpoint_; // --debug-license-normal-endpoint
    std::string license_server_secret_;           // ACCESS_KEY 签名密钥（仅开发环境显式传入）
};

}  // namespace ttbox::core
