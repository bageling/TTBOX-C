# 平台层：服务编排与健康检查（实验骨架）

这一层是 TTBOX 平台 V1 的实验骨架，**当前没有接入正式运行链路**。

## 它负责什么

用 `ServiceCatalog` 描述 TTBOX 有哪些系统服务，用 `SystemdServiceAdapter` 实际控制 systemd：

| 服务 | 预期行为 |
|---|---|
| AI 核心（Core） | `Restart=always`、`RestartSec=5`，运行用户 `ttbox`，数据目录 `/var/lib/ttbox`，配置目录 `/etc/ttbox` |
| HID 透传 | 启动前 `modprobe libcomposite`；不同供应商版本用 `Restart=on-failure` 或 `no` |
| 网页服务 | 独立 systemd 单元，依赖网络在线，带预检和重启限制 |
| 控制面 | root 后端，依赖网络在线，`Restart=always`、`RestartSec=1`，目录 `/opt/autobl` |

## 和正式链路的关系

- `Supervisor` 只负责服务编排顺序，实际逻辑仍调用现有的 `RuntimeController`，**不重新实现核心**。
- `HealthMonitor` 汇总运行时状态和服务状态。
- 在 Linux / RK3588 板端，systemd 调用是真实执行的。
- 在 Windows 本机测试时，只使用 `MockServiceAdapter`，测试结果不能当板端证据。

> 结论：这个目录是设计骨架，正式服务编排以 `deploy/systemd/` 下的文件为准。
