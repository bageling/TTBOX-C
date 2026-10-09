// OutputBackend.hpp — 统一输出后端抽象（自研外设后端）
//
// 目标：AimThread 只产生 OutputAction（dx/dy/buttons），不判断设备类型。
//       OutputBackend 作为 IHidOutput 的兼容实现，当前只含 LocalHidBackend。
//
// 设计依据：docs/research/OUTPUT_BACKEND_RESEARCH.md（真机实证）。
// 纪律：
//   - Hotkey Gate / mouse.enabled 实时判定逻辑保持与现 AiboxHidOutput 完全一致；
//   - send() 热路径零分配、无锁、无日志；
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "common/Paths.hpp"   // A-PATH-5：mouse cmd.sock 默认单点真源
#include "output/IHidOutput.hpp"

namespace ttbox::core { class RuntimeConfig; }

namespace ttbox::core::output {

struct OutputAction;  // 来自 IHidOutput.hpp

// ---------------------------------------------------------------------------
// 后端状态 / 健康
// ---------------------------------------------------------------------------
// 后端连接状态。
enum class BackendState {
    kDisconnected,
    kConnecting,
    kConnected,
    kError,
};

// 后端健康/遥测快照（供 Web 展示与排障）。
struct BackendHealth {
    BackendState state = BackendState::kDisconnected;
    std::string detail;           // 人类可读（Web 展示）
    uint64_t send_ok = 0;
    uint64_t send_fail = 0;
    uint64_t reconnect_count = 0;
    int64_t last_send_ok_us = 0;  // 最近成功发送时刻（steady，us）
    uint64_t socket_write_ok = 0;
    uint64_t socket_write_fail = 0;
    uint64_t send_count = 0;
    int32_t last_dx = 0;
    int32_t last_dy = 0;
    int32_t last_wheel = 0;
    uint64_t last_timestamp_us = 0;
};

// ---------------------------------------------------------------------------
// 按钮/动作编码（自研；各后端映射到自己协议）
// ---------------------------------------------------------------------------
// 按钮编号（1 左 … 5 前）与动作编码（down/up/click）；各后端映射到自己协议。
constexpr uint8_t kBtnLeft = 1;
constexpr uint8_t kBtnRight = 2;
constexpr uint8_t kBtnMiddle = 3;
constexpr uint8_t kBtnBack = 4;
constexpr uint8_t kBtnForward = 5;
constexpr uint8_t kActDown = 1;
constexpr uint8_t kActUp = 2;
constexpr uint8_t kActClick = 3;

// ---------------------------------------------------------------------------
// IOutputBackend：一种物理设备协议
// ---------------------------------------------------------------------------
class IOutputBackend {
public:
    virtual ~IOutputBackend() = default;

    // 生命周期
    virtual bool connect(std::string* error = nullptr) = 0;
    virtual void disconnect() = 0;
    virtual bool reconnect(std::string* error = nullptr) = 0;
    virtual BackendHealth health() const = 0;

    // 输出
    virtual bool mouse_move(int32_t dx, int32_t dy, int32_t wheel = 0) = 0;
    virtual bool mouse_button(uint8_t button, uint8_t action) = 0;
    virtual bool mouse_click(uint8_t button) = 0;

    // 后端名字（诊断用）。
    virtual const char* name() const = 0;

    // ---- Hotkey Gate / 总闸（基类实现）----
    //   判定顺序对齐 AiboxHidOutput::send；但"无配置源"分支已收紧为 fail-closed（详见 gate_allows）。
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }
    void set_button_source(std::atomic<uint16_t>* source) { button_source_ = source; }
    void set_config_source(RuntimeConfig* config) { config_source_ = config; }

protected:
    // 发送前调用：false = 被 Gate 拦截（不发送）。
    // 判定顺序：1) 静态总闸；2) config 缺失；3) mouse.enabled；4) 热键 mask 缺失；5) 热键未按下。
    // 安全语义（fail-closed）——无配置源时一律拒绝注入：
    //   - 有按键源、无配置源 → 拒绝（与 AiboxHidOutput::send 一致）；
    //   - 无按键源、无配置源 → 同样拒绝（E-07 收紧）；AiboxHidOutput 在该分支未同步、仍会放行，
    //     二者在此分叉。生产默认走本类（kind=usb_proxy → gate_allows），AiboxHidOutput 仅作回退路径；
    //     如需两者对齐属另一批次决策，本批次未改动 AiboxHidOutput 行为。
    // 实现位于 OutputBackend.cpp（需 RuntimeConfig 完整定义）。
    bool gate_allows() const;

    std::atomic<uint16_t>* button_source_ = nullptr;
    RuntimeConfig* config_source_ = nullptr;
    bool enabled_ = false;
};

// ---------------------------------------------------------------------------
// OutputBackend：设备选择器（IHidOutput 兼容实现，AimThread 零改动）
// ---------------------------------------------------------------------------
class OutputBackend final : public IHidOutput {
public:
    // 后端选择与 Gate 参数（configure() 传入）。
    struct Params {
        std::string kind = "local_hid";   // local_hid
        std::string hidg_path = "/dev/hidg1";
        std::string proxy_socket_path = paths::kMouseCmdSocketDefault;
        // Gate / 运行时
        RuntimeConfig* runtime_config = nullptr;
        std::atomic<uint16_t>* button_source = nullptr;
        bool enabled = false;
    };

    OutputBackend() = default;
    // 析构：unique_ptr 自动释放后端。
    ~OutputBackend();

    // 按 kind 创建具体后端、绑定 Gate 源并预连接。
    bool configure(const Params& p, std::string* error = nullptr);

    // 当前后端指针（可空）。
    IOutputBackend* backend() { return backend_.get(); }
    const IOutputBackend* backend() const { return backend_.get(); }
    // 已保存参数。
    const Params& params() const { return params_; }

    // IHidOutput::send：转发为鼠标移动（按键不经 send）。
    bool send(const OutputAction& action) override;
    // ---- 按键注入（自动扳机用；button 是**位掩码**，见 IHidOutput.hpp）----
    // 语义：只做转发，Gate 判定在各后端内部（与 mouse_move 一致）。
    // 掩码 → 协议编号的转换在这里做，后端实现只认编号（与 usb-proxy BUTTON_CMD 对齐）。
    bool mouse_button(uint8_t button, uint8_t action) override;
    // click = button(action=click)。
    bool mouse_click(uint8_t button) override;

    // 后端健康快照。
    BackendHealth health() const;
    // 后端名字（无后端返回 "none"）。
    const char* backend_name() const;
    // 静态总闸实际生效值（output_enabled）。与 AimThread 的 injection_allowed 是
    // 两个独立闸门，排障时必须一起看：任一为 false 都不会有任何注入。
    bool enabled() const { return backend_ ? backend_->enabled() : false; }

    // 转发总闸 / 按键源 / 配置源到当前后端。
    void set_enabled(bool enabled);
    void set_button_source(std::atomic<uint16_t>* source);
    void set_config_source(RuntimeConfig* config);

private:
    // 当前后端与已保存参数。
    std::unique_ptr<IOutputBackend> backend_;
    Params params_;
};

}  // namespace ttbox::core::output
