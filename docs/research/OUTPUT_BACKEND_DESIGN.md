# TTBOX OutputBackend 设计

- 日期：2026-08-30
- 纪律：AimThread 零改动、不破坏 PID1/Hotkey Gate、不降低 147 FPS

---

## 一、设计目标

```text
AimThread
    ↓
OutputAction
    ↓
OutputBackend
    ↓
LocalHidBackend（写 /dev/hidg0）
```

1. **AimThread 只产 OutputAction（dx/dy/buttons），不判断设备类型** —— 现有 send() 调用点保持。
2. **OutputBackend 是设备选择器**：统一 connect/send/health 语义。
3. **Hotkey Gate / mouse.enabled 保持在后端内**（与现 TtboxHidOutput 相同的实时判定逻辑）。
4. **性能要求**：send() 路径零阻塞、零分配（不改变 AimThread 4ms 周期节拍）。

---

## 二、接口设计

### 2.1 后端基类（IOutputBackend）

```cpp
// output/OutputBackend.hpp
namespace ttbox::core::output {

enum class BackendState {
    kDisconnected,
    kConnecting,
    kConnected,
    kError,
};

struct BackendHealth {
    BackendState state = BackendState::kDisconnected;
    std::string detail;
    uint64_t send_ok = 0;
    uint64_t send_fail = 0;
    uint64_t reconnect_count = 0;
    int64_t last_send_ok_us = 0;
};

class IOutputBackend {
public:
    virtual ~IOutputBackend() = default;
    virtual bool connect(std::string* error = nullptr) = 0;
    virtual void disconnect() = 0;
    virtual bool reconnect(std::string* error = nullptr) = 0;
    virtual BackendHealth health() const = 0;
    virtual bool mouse_move(int32_t dx, int32_t dy, int32_t wheel = 0) = 0;
    virtual bool mouse_button(uint8_t button, uint8_t action) = 0;
    virtual bool mouse_click(uint8_t button) = 0;
    virtual const char* name() const = 0;

    void set_enabled(bool enabled) { enabled_ = enabled; }
    void set_button_source(std::atomic<uint16_t>* source) { button_source_ = source; }
    void set_config_source(RuntimeConfig* config) { config_source_ = config; }

protected:
    bool gate_allows() const;
    std::atomic<uint16_t>* button_source_ = nullptr;
    RuntimeConfig* config_source_ = nullptr;
    bool enabled_ = false;
};

}  // namespace ttbox::core::output
```

### 2.2 设备选择器（OutputBackend）

```cpp
class OutputBackend final : public IHidOutput {
public:
    struct Params {
        std::string kind = "local_hid";
        std::string hidg_path = "/dev/hidg1";
        RuntimeConfig* runtime_config = nullptr;
        std::atomic<uint16_t>* button_source = nullptr;
        bool enabled = false;
    };

    OutputBackend() = default;
    ~OutputBackend();
    bool configure(const Params& p, std::string* error = nullptr);
    IOutputBackend* backend() { return backend_.get(); }
    bool send(const OutputAction& action) override;
    void set_enabled(bool enabled);
    void set_button_source(std::atomic<uint16_t>* source);
    void set_config_source(RuntimeConfig* config);

private:
    std::unique_ptr<IOutputBackend> backend_;
    Params params_;
};
```

---

## 三、Hotkey Gate 与总闸（保持现状）

现 TtboxHidOutput 的 Gate 逻辑（原样迁移到基类）：

```text
send 入口
  ├─ enabled_ == false            → 拦截（静态总闸）
  ├─ config_source_ == null       → 拦截（fail-closed）
  ├─ !mouse.enabled               → 拦截（运行时总开关）
  ├─ mask = aim_hotkey|aim_hotkey2; mask==0 → 拦截（配置缺失）
  ├─ button_source_ & mask == 0   → 拦截（热键未按下）
  └─ 通过 → 设备发送
```

---

## 四、性能纪律

- `send()` 热路径：无锁、无堆分配、无 JSON、无日志（成功时）
- 连接/health 为慢路径：独立于发送线程
- 回归门槛：Capture ≈147 / Detection ≈147 / E2E P50 ≈11.5ms 不下降

---

## 五、实现状态

- ✅ OutputBackend.hpp + IOutputBackend + 选择器
- ✅ LocalHidBackend（迁移 TtboxHidOutput，不改 report/协议/行为）
- ✅ OutputBackend 包装接入（AimThread 零改动）、本地回归
- ✅ 测试：backend switch / invalid / unavailable / fallback / hotkey gate / zero movement / target lost
- ⬜ 真机验证 + 性能回归（板端 192.168.0.53 当前断连）