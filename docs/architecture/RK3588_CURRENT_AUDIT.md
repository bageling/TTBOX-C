# RK3588 商业化项目完整现状审计

> **执行状态注记（2026-10-01，P1–P6 之后追加）** —— 本文件的以下判断**已作废**，正文保留作历史记录：
> · 平台层包**不是** `platform/`：已改名 `ttbox_platform/`（P3，目录名 = 包名，且不再遮蔽标准库 `platform`）；
> · `plugins/web/api_v1.py`、`framework_api.py`：**源码保留且随出货包交付**（P1 复核；原"S1 交付减法"
>   已被业主整体回滚）。二者在 `bin/ttbox-web.py` 里目前**没有注册点**（随包但不注册），是否恢复注册待业主定；
> · 出货清单已**双列化** `<源> -> <目标>`；出货树 = 清单映射结果 + `bin/`、`lib/` 两个构建产物，
>   脚本不再做任何增删（P1）；
> · 测试的根定位统一为 `plugins/web/lib/paths.py::repo_root`（根锚发现）；全仓 `parents[N]` 与
>   `"../.."` 相对跳目录已清零，并由 `scripts/ttbox_conventions_gate.sh` ⑩ 断住（P5/P6）；
> · `pyproject.toml` 的 `testpaths` 已含三套件 ⇒ 裸 `python -m pytest` 即跑全量（P6）。
>
> 现行口径以这三处为准：`deploy/pack_manifest.txt`、
> `docs/protocols/config-path-env-registry.md`、`docs/CONVENTIONS.md`。

> 只读审计。本阶段**未修改任何代码**、未构建、未改板端配置、未启停服务。
> 目的：为下一阶段重新设计商业化程序框架提供事实依据。

- 本地仓库：`G:\WORKBUDDY工作区\TTBOX-最终源码-2026-09-18`，HEAD = **`6ec0fd9`**（V1.0.17 整体回滚后的基线）
- 板端：Orange Pi 5 Plus / RK3588 / Ubuntu 22.04.5 LTS / kernel `5.10.0-1012-rockchip` / aarch64 / `192.168.0.120`
- 板端版本：**V1.0.17**（`/opt/ttbox/current -> releases/V1.0.17`），流水线按设计停在停止态
- 审计时间：2026-09-30 22:30–23:40（本地，UTC+8）

**证据标记**：`[实测]` 本地/板端命令实测输出；`[源码]` 读源码确认；`[推断]` 由前两者推导、未经直接验证。

---

## 1. 项目现状

| 维度 | 现状 | 证据 |
|---|---|---|
| 仓库 HEAD | `6ec0fd9`（revert 回滚 V1.0.17 清理，内容等同 `3d91b80`） | `[实测]` `git log -1` |
| 云端 latest | **V1.0.16** | 工作区记忆基线 |
| 板端运行 | **V1.0.17**（与云端/源码不一致，按铁律 3 未回退） | `[实测]` `readlink current` |
| 工作树 | tracked 变更 0 行；仅 `?? docs/architecture/` 与 `?? docs/资产盘点-重构前基线-2026-09-30.md` | `[实测]` `git status --porcelain` |
| 代码总量 | 约 76,400 行（不含构建产物） | `[实测]` `find ... \| xargs wc -l` |
| 语言 | C++17（核心/usbproxy）、Python3（框架/插件/脚本）、Shell、HTML/JS 单文件、Lua、CMake、JSON | `[源码]` |
| 测试基线 | CTest 41/41（host 默认）；pytest framework 39 + platform 61 + web 476 | 工作区记忆基线 |
| 当前运行态 | `running=true` / `runtime_running=false` / `current_model_id=""` / metrics 全 0 | `[实测]` IPC `GET_STATUS` |

**项目性质**：单机 AI 瞄准盒子。链路 = HDMI 采集 → RGA 缩放 → RKNN 推理 → 检测解码 → 选靶 → PID → 鼠标注入（USB gadget 透传）。商业化形态 = RK3588 整机 + 离线授权卡 + 云端 OTA。

**当前最大结构性事实**（影响重构判断）：
1. 核心链路（采集/RGA/RKNN/解码/控制/注入）**成立且已实测跑通**，可作新框架基底。
2. 治理层（Web 面板 + 约 78 条路由 + 配置读写）**单文件 6,087 行**，是重构主战场。
3. 存在**整包死代码**（`api_v1.py`、`framework_api.py`、`core/src/hid/` 全部实现、`platform/`、`framework/` 随包但运行期零引用）。
4. **无认证 / 无签名校验 / 无 capability 收敛**，安全面在"激活后局域网任意主机"级别不成立。
5. **性能无任何基线数据**（流水线长期停止），§21 全部标 `未测量`。

---

## 2. 本地代码结构

### 2.1 顶层目录

| 目录 | 体积 | 文件数 | Git 跟踪 | 性质 |
|---|---|---|---|---|
| `.archive-2026-09-23/` | 349M | — | 否 | 历史构建归档 |
| `dist/` | 188M | 125 | 否 | OTA 出包产物（两套版本谱系混放） |
| `core/` | 76M | 1248 | 288 | C++ 推理核心 + 测试 + 构建产物 |
| `build-aarch64-t148/` | 27M | 195 | 否 | 交叉构建产物（根级） |
| `build-aarch64-t148-repro/` | 20M | 199 | 否 | 复现性构建产物 |
| `deploy/` | 17M | 29 | 19 | 部署定义 + 旧 OTA 包 |
| `image/` | 4.7M | 59 | 49 | 出厂镜像烘焙 |
| `plugins/` | 1.7M | 93 | 91 | 插件（含 Web 控制台） |
| `usbproxy/` | 1.3M | 36 | 34 | 鼠标注入代理（C++） |
| `docs/` | 971K | 119 | 107 | 文档与留档 |
| `scripts/` | 718K | 49 | 46 | 运维/构建/发布脚本 |
| `framework/` | 141K | 27 | 27 | 插件框架（Python） |
| `platform/` | 125K | 32 | 31 | 平台服务层（Python） |
| `tools/` | 126K | 8 | 8 | 离线工具（授权/OTA/模型转换） |
| `tests/` | 57K | 10 | 10 | 板端接口/监控脚本 |
| `config/` | 10K | 4 | 4 | 编译期默认配置 |
| `ttbox_motion/` | 45K | 4 | 3 | 运动校准/训练 |

源码分布：`core/src` 28,737 / `plugins` 30,434 / `core/tests` 18,844 / `scripts` 11,142 / `usbproxy` 7,856 / `image` 3,046 / `tools` 2,796 / `framework` 1,700 / `platform` 1,104 / `tests` 710 / `ttbox_motion` 614。

### 2.2 模块与调用关系

| 模块 | 路径 | 作用 | 谁调用 | 使用 |
|---|---|---|---|---|
| C++ 核心入口 | `core/src/main.cpp` | 注册信号 → `Application` | systemd | 是 |
| 应用总装 | `core/src/app/Application.{hpp,cpp}`（2,265 行） | CLI/配置/授权/IPC/运行时装配 | `main()` | 是 |
| 运行时编排 | `core/src/runtime/CoreRuntime.*` | 采集/推理/瞄准/预览生命周期 | `Application` | 是 |
| 采集 | `core/src/capture/V4L2Capture.*` | V4L2 + mmap + dma-buf | `CoreRuntime` | 是 |
| RGA | `core/src/rga/RgaProcessor.*` | librga/im2d crop+resize | `Preprocess` | 是 |
| 预处理 | `core/src/rknn/Preprocess.*` | RGA 主路径 + CPU 回退 | `WorkerPool` | 是 |
| RKNN 引擎 | `core/src/rknn/RKNNEngine.*` | init/run/零拷贝绑定 | `InferenceWorker` | 是 |
| 解码 NMS | `core/src/rknn/DecodeNMS.*`（899 行） | 多布局解码 + NMS | `InferenceWorker` | 是 |
| 工作池 | `core/src/rknn/WorkerPool.*` | N worker 并行推理 | `CoreRuntime` | 是 |
| 瞄准线程 | `core/src/aim/AimThread.*` | 选靶 + PID + 输出 | `CoreRuntime` | 是 |
| 目标选择 | `core/src/mouse/TargetSelector.*` | 选靶/切换/迟滞 | `AimThread` | 是 |
| 输出层 | `core/src/output/*` | HID 后端抽象 + 总闸 | `AimThread` | 是 |
| IPC 服务 | `core/src/ipc/IpcServer.*`（1,212 行） | Unix socket NDJSON，15 命令 | `Application` | 是 |
| 授权 | `core/src/auth/*`（25 文件） | 离线卡 + Ed25519 + 状态机 | `Application` | 是 |
| 模型管理 | `core/src/model/*`（17 文件） | 注册/适配/后端/元数据 | `Application` | 是 |
| 预览 | `core/src/preview/PreviewModule.*` | MJPEG 独立线程 | `CoreRuntime` | 是 |
| 物理鼠标读取 | `core/src/input/PhysicalMouseReader.*` | 接 usb-proxy event.sock | `CoreRuntime` | 是 |
| HID 包 | `core/src/hid/*`（17 文件） | 包注册/转发/解析 | **无人** | **否**（见 §22） |
| 插件框架 | `framework/`（27 文件） | 插件生命周期/安装/完整性 | **无人** | **否**（随包但零引用） |
| 平台服务层 | `platform/`（32 文件） | supervisor/health/model/runtime | 仅测试 | **否** |
| Web 插件 | `plugins/web/`（`bin/ttbox-web.py` 6,087 行） | 面板 + 全部业务 API | systemd `ttbox-web` | 是 |
| v1 旧 API | `plugins/web/api_v1.py`（882 行） | `/api/v1/*` 蓝图 | **无调用点** | **否（死）** |
| 框架桥接 API | `plugins/web/framework_api.py` | `/api/plugins/*` | **无调用点** | **否（死）** |
| 其余插件 | `plugins/{model,preview,system,fan,log,monitor,network,wifi,upgrade}/` | 各自 plugin.json + entry | 无 unit | **否**（见 §22） |
| usbproxy | `usbproxy/*.cpp` | raw-gadget 鼠标透传 | systemd `ttbox-usbproxy` | 是 |
| 运动校准 | `ttbox_motion/` | 曲线训练/校准 | `plugins/web` import | 是 |
| 运维脚本 | `scripts/`（46 文件） | 构建/发布/OTA/FHS/诊断 | 人 / systemd / web | 部分 |
| 离线工具 | `tools/`（8 文件） | 授权生成、OTA 签名、模型转换 | 人（开发期） | 是（开发期） |
| 板端测试 | `tests/`（10 文件） | 板端 API 验收 + 监控 | 人（手工） | 是（手工） |
| 部署定义 | `deploy/` | systemd 单元、配置、公钥、dtb | 安装脚本 | 是 |
| 镜像烘焙 | `image/`（59 文件） | WSL loop 挂载 → chroot → 交付 | 人（出厂） | 是（出厂期） |

**核心模块风险排序**（单文件体量）：`ttbox-web.py` 6,087 > `Application.cpp` 2,265 > `index.html` 13,406（含 CSS 5,188 + JS 7,200）> `IpcServer.cpp` 1,212 > `DecodeNMS.cpp` 899。

### 2.3 第三方代码

`core/third_party/`（658K，**全为头文件、无二进制**）：`onnxruntime/include/`（8 头，622K）、`rknn/rknn_api.h`（36K）。`[源码]` `core/CMakeLists.txt:381-389`：ONNX Runtime **仅 Windows** 链接；Linux/aarch64 走 RKNN。

---

## 3. 板端目录结构

### 3.1 FHS 布局

| 板端路径 | 类型 | 用途 | 运行时必须 |
|---|---|---|---|
| `/opt/ttbox/` | 目录（35M） | 程序根（FHS 私有前缀） | 是 |
| `/opt/ttbox/current` | **软链** → `releases/V1.0.17` | 版本切换指针（原子） | 是 |
| `/opt/ttbox/releases/V1.0.16` | 目录 12M | 上一版完整运行树（回滚用） | 否（保留） |
| `/opt/ttbox/releases/V1.0.17` | 目录 12M | 当前版本完整运行树 | 是 |
| `/opt/ttbox/releases/V1.0.17.ota.staging` | 目录 12M | **OTA 展开残留** | **否（垃圾）** |
| `/opt/ttbox/plugins`、`/opt/ttbox/scripts` | 软链 → `current/*` | 过渡期兼容路径 | 否（兼容） |
| `/opt/ttbox/config/` | 目录 24K | 运行期配置 | 是 |
| `/opt/ttbox/config/cloud_session.json.tmp.*` ×37 | 文件 | **临时文件泄漏** | **否（垃圾）** |
| `/opt/ttbox/presets/` | 目录 24K | 面板预设 | 否 |
| `/opt/ttbox/state/` | 目录 16K | 运行期状态（升级不清理） | 是 |
| `/opt/ttbox/state/{runtime_intent.json,core_boot_version,ota_status.json}` | 文件 | 启停意愿/上次启动版本/更新结果 | 是 |
| `/opt/ttbox/runtime/edid/current.bin` | 文件 | 当前 EDID blob | 否 |
| `/etc/ttbox/config.d/00-factory.json` | 文件 root:root 0644 | 出厂基线，**升级整体替换** | 是 |
| `/etc/ttbox/config.d/10-device.json` | 文件 root:ttbox 0664 | 设备层，**升级不覆盖**，Core 唯一写回目标 | 是 |
| `/var/lib/ttbox/` | 目录 60M | 客户数据（升级不触碰） | 是 |
| `/var/lib/ttbox/models/installed/{sjzv11___1,320dawan0907,EP}/` | 目录 | 已装模型 | 是 |
| `/var/lib/ttbox/models/registry/active.json` | 文件 | 当前激活模型 id | 是 |
| `/var/lib/ttbox/models/_incoming/` | 目录 | 上传中转（**5 个残留 rknn**） | 否（垃圾） |
| `/var/lib/ttbox/license/license.json` | 目录 0700 | 授权落盘 | 是 |
| `/var/lib/ttbox/activation/` | 目录 0700 | 激活身份 | 是 |
| `/var/lib/ttbox/ota/jobs/` | 目录 0770 root:ttbox | **OTA 特权通道入口** | 是 |
| `/var/lib/ttbox/ota/jobs/.probe-*` ×2 | 文件 | **探针残留** | **否（垃圾）** |
| `/var/lib/ttbox/{hid,motion-profiles,presets,update}/` | 空目录 | 预留 | 否 |
| `/run/ttbox/core.sock` | socket 0660 root:ttbox | **核心控制通道** | 是 |
| `/run/ttbox-mouse-passthrough/{cmd,event}.sock` | socket 0660 | 鼠标控制下行 / 物理鼠标上行 | 是 |
| `/etc/systemd/system/ttbox-*.{service,path,timer}` | 文件（10 个） | 服务定义 | 是 |
| `/usr/local/sbin/ttbox-time-guard.sh` | 脚本 1050B | 开机时钟下限守卫 | **是（本地仓库无此文件）** |
| `/var/log/ttbox/` | 目录 ttbox:ttbox | 预期日志目录（**当前为空**） | 否（未使用） |

### 3.2 版本目录差异

`[实测]` `diff -rq releases/V1.0.16 releases/V1.0.17` 差异 11 个文件：`RELEASE_MANIFEST.json`、`bin/ttbox_core_main`、`plugins/web/bin/ttbox-web.py`、`plugins/web/lib/__pycache__/*.pyc`（2 个）、`static/motion_training{,_mobile}.js`、`templates/index.html`、`scripts/ttbox_ota_updater.py`、`scripts/ttbox_release_install.sh`、`ttbox_motion/calibration.py`。

release 树内含构建期未清理的 `__pycache__/*.pyc`。

### 3.3 数据目录分裂（重要）

客户数据实际**分两处**：预设/曲线/凭据仍在 `/opt/ttbox/{config,presets}`；而 `/var/lib/ttbox/{presets,motion-profiles}` **已建目录但无人写入**。→ OTA 升级只保证 `/var/lib/ttbox` 不被触碰，`/opt/ttbox/config` 与 `/opt/ttbox/presets` 属"程序根内数据"，**升级语义模糊**。

---

## 4. 启动 / 进程 / 服务

### 4.1 启动链

```text
内核 → systemd(pid1)
 ├ [sysinit]      ttbox-time-guard.service → /usr/local/sbin/ttbox-time-guard.sh   （抬时钟；必在 web/core 之前）
 ├ [multi-user]   ttbox-core.service     root:ttbox   ExecStart=current/bin/ttbox_core_main --config /etc/ttbox/config.d --ipc /run/ttbox/core.sock
 ├ [multi-user]   ttbox-web.service      ttbox:ttbox  ExecStart=current/plugins/web/bin/ttbox-web        → 0.0.0.0:8000
 ├ [multi-user]   ttbox-preview.service  ttbox:ttbox  ExecStart=current/plugins/preview/bin/ttbox-preview → 127.0.0.1:8001
 ├ [multi-user]   ttbox-usbproxy.service root:ttbox   ExecStartPre=stop 5 冲突服务(可失败) ; modprobe raw_gadget(不可失败)
 │                                        ExecStart=current/usbproxy/board/run-ttbox-usb-proxy.sh → exec usb-proxy
 ├ [multi-user]   ttbox-edid.service     oneshot,RemainAfterExit  timeout 45s edid_apply.sh || echo WARN（非致命）
 ├ [multi-user]   ttbox-ota.path         waiting  PathExistsGlob=/var/lib/ttbox/ota/jobs/*.json → ttbox-ota.service(root oneshot)
 └ [timers]       ttbox-ensure.timer     OnBootSec=2min / OnCalendar=*:0/10 → ttbox-ensure.service(oneshot, **disabled**)
```

**Core 内部初始化顺序** `[源码]` `Application.cpp:495 initialize()`：
① 解析 CLI → ② **风扇设转**（失败仅 WARN）→ ③ 路径裁决 `--config` > `TTBOX_CONFIG` > 编译期 → ④ `config_.load()`（目录分层深合并，**失败即拒绝启动**）→ ⑤ `apply_startup_runtime_intent()`（**R6：版本变则强制停机**）→ ⑥ CPU 调频（governor→schedutil）→ ⑦ 授权（`OfflineCardClient`→`LicenseDaemon::start()`）→ ⑧ 加载 `runtime_profile`（校验失败自愈写回）→ ⑨ `ModelManagement::init()` → ⑩ 组装 `CoreRuntime::Params` + `IpcServer`。

### 4.2 服务运行态 `[实测]`

| Unit | 状态 | enabled | 身份 |
|---|---|---|---|
| `ttbox-core.service` | active running | enabled | root:ttbox |
| `ttbox-web.service` | active running | enabled | ttbox:ttbox |
| `ttbox-preview.service` | active running | enabled | ttbox:ttbox |
| `ttbox-usbproxy.service` | active running | enabled | root:ttbox |
| `ttbox-edid.service` | active (exited, oneshot) | enabled | root |
| `ttbox-time-guard.service` | active (exited, oneshot) | enabled | root |
| `ttbox-ota.path` | active (waiting) | enabled | — |
| `ttbox-ota.service` | inactive dead | **static** | root（触发时） |
| `ttbox-ensure.service` | inactive dead | **disabled** | — |
| `ttbox-ensure.timer` | active (waiting) | enabled | — |
| `usb-proxy.service` / `usb-proxy-test.service` | **not-found** | — | — |

### 4.3 进程 / 端口 `[实测]`

| PID | PPID | User | %CPU | %MEM | 命令 |
|---|---|---|---|---|---|
| 3121501 | 1 | root | 16.0 | 1.4 | `usb-proxy --device=fc000000.usb --driver=dwc3-gadget --mouse_control_cmd_socket=…/cmd.sock --mouse_control_event_socket=…/event.sock --vendor_id=373b --product_id=10c9 --hid_passthrough_compat --enable_mouse_control` |
| 3121481 | 1 | root | 4.0 | 0.9 | `ttbox_core_main --config /etc/ttbox/config.d --ipc /run/ttbox/core.sock` |
| 3121490 | 1 | ttbox | 1.4 | 0.2 | `ttbox-preview.py` |
| 3121484 | 1 | ttbox | 0.9 | 0.6 | `ttbox-web.py` |

四进程 **PPID 全 = 1**（systemd 直管；`run-ttbox-usb-proxy.sh` 用 `exec`，脚本不驻留）。

| 端口 | 绑定 | 进程 |
|---|---|---|
| `0.0.0.0:8000` | **全网可达** | `ttbox-web` |
| `127.0.0.1:8001` | 仅本机 | `ttbox-preview` |
| `0.0.0.0:22` | 全网可达 | sshd |
| `127.0.0.53:53` | 本机 | systemd-resolve |

### 4.4 当前运行状态 `[实测]` IPC `GET_STATUS`

```json
{"running": true, "runtime_running": false, "current_model_id": "",
 "version": "V1.0.17", "uptime_ms": 2201592,
 "license": {"activated": true, "state": "active", "plan": "subscription",
             "features": ["capture","inference","aim","ota"],
             "capabilities": {"capture":true,"inference":true,"aim":true,"ota":true}},
 "metrics": {"capture_fps": 0, "frames_total": 0, "model_workers_total": 0}}
```

journal 佐证（UTC 13:56:40 起跑 → 13:57:11 停回）：`真实模型已就绪: running_model_id=sjzv11___1 inference+decode=PASS` → `capture 已停止（STREAMOFF + 归还全部 buffer）`。
⇒ **不是故障**：更新冒烟自检跑通后按 `runtime_intent.json={"want_runtime_running":false}` 停回停止态。

---

## 5. 脚本与注入位置

### 5.1 脚本清单（按调用者）

| 脚本 | 位置 | 调用者 | 时机 | 作用 |
|---|---|---|---|---|
| `run-ttbox-usb-proxy.sh` | `usbproxy/board/` | `ttbox-usbproxy.service` | 启动 | 等 UDC(60s)→探测鼠标→`exec usb-proxy`；前置 `LD_LIBRARY_PATH` |
| `ttbox_ensure_services.sh` | `scripts/` | `ttbox-ensure.timer`（每10min） | 周期 | 幂等自愈 7 unit |
| `edid_apply.sh` | `scripts/edid/` | `ttbox-edid.service` / Web `/api/hardware/display` | 开机 + 用户点"应用" | 注入 EDID（HPD 重协商） |
| `edid_patch_boot_image.sh` | `scripts/edid/` | **Web** `ttbox-web.py:5449` | 用户改 EDID | 打补丁进 boot 分区 |
| `ttbox-time-guard.sh` | **板端独有** `/usr/local/sbin/` | `ttbox-time-guard.service` | sysinit | 抬系统时钟到证书有效期 |
| `ttbox_ota_updater.py` | `scripts/` | `ttbox-ota.service`（root oneshot） | `ota.path` 触发 | 7 步 OTA + 回滚 |
| `ttbox_release_install.sh` | `scripts/` | 更新器 / 手工 | 安装/回滚 | 铺 release 树、切 current、建目录、装 unit |
| `ttbox_fhs_init.sh` | `scripts/` | 镜像烘焙 / 首装 | 出厂 | 建 FHS 目录、铺 `config.d` |
| `ttbox_build_release.sh` | `scripts/` | 人（开发机） | 出包前 | 构建 + 门禁 + 留档 |
| `ttbox_pack_ota.sh` / `ttbox_pack_delta.sh` | `scripts/` | 人 | 打包 | 装配 payload + 签名 |
| `ttbox.sh`/`ttbox_doctor.sh`/`ttbox_backup.sh`/`ttbox_restore.sh`/`ttbox_dtb_fix.sh`/`ttbox_usb_mode.sh`/`ttbox-web-reset-credentials.sh` | `scripts/` | 人（排障） | 手工 | 运维 |
| `a9_*.sh`（12 个） | `scripts/` | 人（硬件调试） | 手工 | HID gadget/CPU 亲和/压力测试 |
| `ttbox_m207_accept.py` 等（3 个） | `scripts/` | 人（验收） | 手工 | 板端验收断言（只读） |
| `wifi_manager.py` | `scripts/` | `plugins/wifi` | 运行期 | WiFi 管理 |
| `image/*.sh`（36 个） | `image/` | 人（出厂） | 出厂 | WSL loop 挂载 → chroot → 交付 |
| `tests/api/*.sh`、`tests/monitor/*` | `tests/` | 人（板端） | 手工 | API 验收 / 长时监控 |

### 5.2 注入位置（启动时自动执行/覆盖/替换）

| # | 类型 | 位置 | 行为 | 风险 |
|---|---|---|---|---|
| **I1** | 环境变量注入（web） | `deploy/systemd/ttbox-web.service:24,33,36,39,44` | `TTBOX_ROOT`/`PYTHONPATH`/`TTBOX_IPC_SOCKET`/`TTBOX_MODELS_ROOT`/`TTBOX_PREVIEW_URL` | 改 `PYTHONPATH` 可劫持模块解析；注释警告**严禁**把 `/opt/ttbox` 放进 PYTHONPATH（撞 stdlib `platform`） |
| **I2** | 环境变量注入（core/preview/usbproxy） | `ttbox-core.service:40`；`ttbox-preview.service:21-23`；`ttbox-usbproxy.service:17-19` | `TTBOX_IPC_SOCKET`/`TTBOX_PREVIEW_HOST/PORT`/`USB_PROXY_*` | 低 |
| **I3** | `LD_LIBRARY_PATH` 注入（usbproxy） | `run-ttbox-usb-proxy.sh:59-60` | 前置 `$PROJECT_DIR/lib` | **中**：运行时库搜索路径改写；`usbproxy/lib/` 被替换即劫持 `libjsoncpp` |
| **I4** | ExecStartPre 停他人服务 | `ttbox-usbproxy.service:28` | `timeout 20s systemctl stop usb-proxy{,-test}.service mouse-passthrough.service opi-mouse-gadget.service usbdevice.service` | 会停镜像里第三方 USB 透传；板端实测 `not-found`，当前无影响 |
| **I5** | ExecStartPre 加载内核模块 | `ttbox-usbproxy.service:33` | `modprobe raw_gadget`（不可失败） | 低 |
| **I6** | 开机 root 执行脚本 | `ttbox-edid.service` ExecStart | `timeout 45s edid_apply.sh`（含 HPD 重协商） | 中：改 HDMI-RX 寄存器/sysfs，历史黑屏故障源 |
| **I7** | 开机修改系统时钟 | `ttbox-time-guard.sh`（`date -s`） | 时钟早于证书期→抬到 2026-09-20 12:00 UTC | 中：**直接改系统时间**；该脚本不在仓库 |
| **I8** | 开机修改 CPU 调频 | `Application.cpp` init 120-135 | 写 governor + `scaling_min_freq` | 中：改 sysfs，需 root |
| **I9** | 开机风扇设转 | `Application.cpp` init 66-70 | 写 hwmon pwm | 低（失败仅 WARN） |
| **I10** | Web 写 root 特权任务 | `ttbox-web.py:2496/2612` → `/var/lib/ttbox/ota/jobs/*.json` → `ttbox-ota.service`(root) | web(ttbox) 驱动 root 更新器 | **高**（设计如此；缓解 = Ed25519 验签 + 0770 目录） |
| **I11** | Web 直接 exec 外部命令 | `hostnamectl`(2459)、`os.system('systemctl reboot\|poweroff')`(2755)、`bash edid_apply.sh`(5442)、`bash edid_patch_boot_image.sh`(5449)、`v4l2-ctl`(5248)、`systemctl`(多处)、`busctl`(2733) | Web 可改主机名/重启/改 boot 镜像/改 EDID | **高**（无 `shell=True`，参数走 argv，未见注入面） |
| **I12** | 模型路径注入 | `TTBOX_MODELS_ROOT` > 配置 `model_registry_root` > `Application.cpp:708` 宏兜底 | 三级裁决 | 低 |
| **I13** | 配置文件生成 | `ttbox_fhs_init.sh:78-91` | `00-factory.json` **每次安装整体覆盖**；`10-device.json` **仅当不存在时创建** | 中 |
| **I14** | ExecStartPre 冲突服务 | 同 I4 | — | — |

**未发现的注入**：全仓 `grep LD_PRELOAD` **0 命中**（仅 `ttbox-core.service:37` 一条**已删除**的 LD_LIBRARY_PATH 注释）；**无** `EnvironmentFile=`；**无** `ExecStartPost`；**无** docker-entrypoint 类钩子。

---

## 6. 硬件资源

`[实测]` `/proc/device-tree/model` = `Orange Pi 5 Plus`，`compatible` = `rockchip,rk3588-orangepi-5-plus`。

| 资源 | 配置 | 实测值 |
|---|---|---|
| SoC | RK3588（8nm，4×A76 + 4×A55 + 3 核 NPU + Mali-G610） | — |
| CPU | 8 核：cpu0-3 = **Cortex-A55 @1.8GHz**，cpu4-7 = **Cortex-A76 @2.352GHz**（min 408MHz） | `lscpu` / `cpuinfo_max_freq` |
| 内存 | 7,924 MB total；已用 478 MB；available 7,338 MB；buff/cache 1,530 MB | `free -m` |
| **Swap** | **0（无交换）** | `free -m` |
| NPU | 3 核，可用频点 300/400/500/600/700/800/900/**1000 MHz**，当前 **1,000 MHz** | `/sys/class/devfreq/fdab0000.npu/` |
| GPU | Mali-G610，当前 **1,000 MHz** | `/sys/class/devfreq/fb000000.gpu/` |
| DMC（内存控制器） | 当前 2,112 MHz | `/sys/class/devfreq/dmc/` |
| 温度 | soc 51C / bigcore0 51C / bigcore1 52C / littlecore 52C / center 51C / gpu 51C / npu 51C（**空闲态**） | `/sys/class/thermal/` |
| 风扇 | `pwmfan` pwm1 = **50**（0–255） | `/sys/class/hwmon/hwmon*/pwm1` |
| RTC | **有** `/dev/rtc0`（符号链 `/dev/rtc`） | `ls -la /dev/rtc*` |

**存储**（关键）：

| 设备 | 容量 | 挂载 | 说明 |
|---|---|---|---|
| `mmcblk1`（SD 卡） | 28.9G | `mmcblk1p2`(4.6G) → `/` | **根分区**：4.5G，已用 3.2G，可用 1.1G，**75%** |
| `mmcblk1p1` | 4M | — | 引导 |
| `mmcblk0`（eMMC） | 58.3G | `p1`1G / `p2`57.3G — **均未挂载** | **大容量 eMMC 闲置** |
| `mtd0` "loader" | 16M | — | SPI loader |

**设备节点**：

| 节点 | 权限 | 谁打开 | 说明 |
|---|---|---|---|
| `/dev/video0` | `crw-rw---- root:video` | core | HDMI-RX 采集 |
| `/dev/video-dec0`、`/dev/video-enc0` | `crw-rw---- root:video` | **无人** | 硬编解码**未使用** |
| `/dev/rga` | `crw-rw---- root:video` | core | RGA 硬件 |
| `/dev/dri/card0` | `root:video` | **无人** | 显示 |
| `/dev/dri/card1` | `root:video` | **core** | ⚠ core 打开的是 **card1**（NPU/DRM 渲染节点，非显示卡） |
| `/dev/dri/renderD128/129` | `root:render` | — | 渲染节点 |
| `/dev/raw-gadget` | `crw------- root:root 0600` | **usbproxy** | USB gadget 注入 |
| `/dev/uinput`、`/dev/uhid` | `crw------- root:root 0600` | **无人** | ⚠ 全仓无引用，纯残留 |
| `/dev/input/event0-9` | `crw-rw---- root:input 0660` | — | 10 个输入设备 |
| `/dev/bus/usb/003/003` | — | usbproxy | 物理鼠标设备 |

**网络**：`enP3p49s0` **UP** `192.168.0.120/24`（业务口）；`enP4p65s0` **DOWN**（第二网口未用）；`lo`。
**USB**：`Bus 003 Device 003: ID 373b:10c9 Compx Nearlink Mouse Dongle`（被 usbproxy 接管的物理鼠标）；另有 Genesys Logic Hub ×2。

**资源占用缺口（未测量）**：运行态 CPU 占用/内存峰值/NPU 利用率/NPU 温度（负载下）/带宽（DMC 压力）——流水线长期停止，**未测量**，见 §21。

---

## 7. 动态库与运行时

`[实测]` `ldd /opt/ttbox/current/bin/ttbox_core_main`：

| 库 | 解析路径 | 来源 |
|---|---|---|
| **`librknnrt.so`** | `/opt/ttbox/current/bin/../lib/librknnrt.so`（7,726,232 B，**2.3.2**） | **随版本自带**（RUNPATH `$ORIGIN/../lib`） |
| `librga.so.2` | `/lib/aarch64-linux-gnu/librga.so.2`（实体 `librga.so.2.1.0`） | **系统** |
| `libjpeg.so.8` | 系统 | 系统 |
| `libopencv_core.so.4.5d`、`libopencv_imgproc.so.4.5d`、`libtbb.so.2` | 系统 | 系统 |
| `libstdc++`/`libm`/`libgcc_s`/`libc`/`libpthread`/`libdl`/`libz` | 系统 | 系统 |

`[源码]` usbproxy 侧：`run-ttbox-usb-proxy.sh:53-58` 自陈**预编译无 RPATH**，依赖 `libjsoncpp.so.25`，随包自带于 `usbproxy/lib/` 并由脚本**显式前置** `LD_LIBRARY_PATH`。

**Runtime 版本门禁**：`core/CMakeLists.txt` 内嵌 `librknnrt.so` sha256 基准（`d31fc19c…`），`scripts/ttbox_build_release.sh:405-509` 做 payload 闭集自检（Library 只允许 `librknnrt.so` 且 sha256 同源）。
**系统是否另装 rknn**：`find / -name 'librknn*.so*'` **无系统副本** ⇒ 单一来源。

**未测量**：运行期常驻内存（库映射总量）、动态加载耗时分布。

---

## 8. 视频采集链路

| 段 | 实现 | 实际参数 | 证据 |
|---|---|---|---|
| HDMI 源 | 外部 PC → HDMI-RX | 驱动 `rk_hdmirx`，`fdee0000.hdmirx-controller`，当前 **2560×1440**，总尺寸 2720×1497，pixelclock 586.336MHz（**144 fps**），progressive | `[实测]` `v4l2-ctl --all/--query-dv-timing` |
| V4L2 采集 | `V4L2Capture.cpp` | `/dev/video0`，**Video Capture Multiplanar**（`fmt.pix_mp`），`VIDIOC_G_FMT` 读实际格式（**不 S_FMT 改写**），REQBUFS **8 buffer**，QUERYBUF + `mmap` + `VIDIOC_EXPBUF`（dma-buf fd） | `[源码]` `:232,334,364,377`；`[实测]` 日志 8 buffers mmap len=11059200 dma_fd=… |
| **采集层硬件裁剪** | `V4L2Capture.cpp:285` `VIDIOC_S_SELECTION` | ⚠ **实际失败**：`V4L2 Selection/Crop 不可用，回退完整帧` → `Preprocess/RGA: Inappropriate ioctl for device`。配置里"采集层硬件裁剪已启用 640×640（居中）"只是**意图**，链路实吃**全帧 2560×1440** | `[实测]` journal WARN；`[源码]` `:280-300` |
| Buffer 管理 | 同上（RAII） | mmap 虚拟地址 + EXPBUF dma-buf fd 并存；STREAMOFF 时归还全部 | `[源码]` `:92,353-377,505` |
| DMA-BUF | 同上 | 采集 plane 带 dma_fd；RGA 侧**必须有 dma_fd**，无 fd 直接拒绝（`RgaProcessor.cpp:217-218`："本阶段要求 dma_fd 路径，禁止 CPU 拷贝输入"） | `[源码]` |
| 线程调度 | `RtSched.cpp:145` | 采集线程 **SCHED_FIFO 60**（读回确认），绑定 cpu4-7 | `[实测]` journal |

**内核侧异常** `[实测]` `journalctl -p err`（与采集直接相关，全部来自 `hdmirx` driver）：
```
fdee0000.hdmirx-controller: Err, timing is invalid
fdee0000.hdmirx-controller: hdmirx_wait_lock_and_get_timing signal not lock, tmds_clk_ratio:0
fdee0000.hdmirx-controller: hdmirx_stop_streaming wait last irq timeout, return bufs!
```
这是 HDMI 源断流/未锁定时的内核报错（13:57 停机后源端拔线即触发），**不是软件栈故障**，但反映"无信号时 driver 会刷 err 日志"。

**配置说了但没生效**：`capture_width/height`（`00-factory.json:65-66` = 1920/1080）在 `core/src` **0 命中**（仅 Web `ttbox-web.py:4600` 映射自 `crop_size` 供显示）；`crop_size=640` core 侧 0 命中。

---

## 9. RGA 链路

| 项 | 值 | 证据 |
|---|---|---|
| 实现 | `core/src/rga/RgaProcessor.cpp`（librga / im2d） | `[源码]` |
| 输入导入 | `importbuffer_fd`（缓存 `input_handles`），**只接 dma_fd** | `[源码]` `:217-218` |
| 缩放 | **单段 `improcess`（crop+resize）** 优先，失败回退 `imcrop`+`imresize` 两段 | `[源码]` `:231-235,277-296,349-373` |
| 输出 | **256×256**，`stride_px=256`，`center_crop=on`，`RGB888`，每帧换新 `dma_fd` | `[实测]` journal `RgaProcessor init: out 256x256 stride_px=256 dma_fd=44 center_crop=on color=RGB888` |
| 编排 | `core/src/rknn/Preprocess.cpp`：主路径 RGA；`TTBOX_CORE_HAS_RGA` 未定义时 CPU fallback（仅 BGR888/RGB888）；FLOAT16 模型透传 RGA 原始 BGR888 不做归一化 | `[源码]` `:81-130,143-166` |
| memcpy | ⚠ **主链路无 memcpy**（RGA→RKNN 走 dma-buf 直绑）；仅 CPU fallback 有 `memcpy`（`:166`） | `[源码]` |
| 每 worker 独立 | 3 个 worker 各有一个 `RgaProcessor`（日志 3 条不同 `dma_fd=44/40/54`） | `[实测]` |

**实际作用**：RGA 承担**全部**空间缩放（2560×1440 → 256×256，面积比 56.25×），因为采集层硬件裁剪不生效。这是 `resize_ms` 的主要来源（**具体耗时未测量**，见 §21）。

---

## 10. RKNN / 模型链路

### 10.1 Runtime 与模型

| 项 | 实测值 | 证据 |
|---|---|---|
| Runtime 版本 | **2.3.2**（7,726,232 B） | `[源码]` CMake 门禁 + `[实测]` ldd |
| 位置 | 随版本自带 `releases/<ver>/lib/librknnrt.so`（RUNPATH `$ORIGIN/../lib`） | `[实测]` ldd |
| 模型格式 | `.rknn`（YOLO 系列，INT8） | `[实测]` |
| 模型库根 | `/var/lib/ttbox/models` | `[实测]` unit ENV + `10-device.json` |
| 已装模型 | `sjzv11___1`(10,980,940 B，**已激活**)、`320dawan0907`(4,115,913 B)、`EP`(4,191,494 B)；各含 `manifest.json`+`metadata.json`+`validation/` | `[实测]` |
| 激活状态 | `registry/active.json` = `{"activated_at":1790691604956,"model_id":"sjzv11___1"}` | `[实测]` |
| 加载 | `rknn_init(&ctx, "<model_path>", 0, nullptr)` | `[源码]` `:111-123` |
| NPU core_mask | **来自 `worker_cores`**：worker0=`1`、worker1=`2`、worker2=`4`（3 核用满） | `[实测]` + `[源码]` `:194-201` |

### 10.2 输入输出 `[实测]` journal

```
rknn_init OK: /var/lib/ttbox/models/installed/sjzv11___1/model.rknn
模型信息: 输入 1 输出 6 | 输入 256x256 INT8 NHWC size=196608B qnt=AFFINE scale=0.003922 zp=-128 | 加载 85.9–94.3ms
```
- 输入：1 个，256×256 INT8 NHWC，196,608 B，AFFINE scale=1/255，zp=-128
- 输出：**6 个**（对应 6 输出张量 → 走 DecodeNMS 多输出 DFL 分支）

### 10.3 零拷贝绑定 `[实测]`

```
RKNN 零拷贝 I/O 已绑定: input=196608 bytes, outputs=6, external_dma=不可用（非 UINT8 原生输入，直绑恒被拒）
```
- 输入走 `rknn_set_io_mem` 绑定（**无每帧 `rknn_inputs_set` 拷贝**）
- **`external_dma=不可用`**：外部 dma 直绑被拒 ⇒ 当前是"**内部零拷贝**"，非全链路 dma 直通
- `[源码]` `:259-270` 关键约束：`rknn_set_io_mem` 成功后**无解绑 API**，半绑态再走 `rknn_inputs_set` 会让该 worker **永久 100% 占用**

### 10.4 Worker 与并发 `[实测]`+`[源码]`

```
worker[0] 就绪: core_mask=1 模型加载 94.315ms → 已绑定独立大核 cpu4
worker[1] 就绪: core_mask=2 模型加载 85.915ms → 已绑定独立大核 cpu5
worker[2] 就绪: core_mask=4 模型加载 87.921ms → 已绑定独立大核 cpu6
RKNNEngine 预热完成: 3 次 / 25–28 ms（已清空统计，不计入 infer_ms）
```
- **3 worker 真并行**：每 worker 独立 `rknn_init` + 独立预热 3 次；`WorkerPool.cpp:61` 每 worker 独立 context；**无 mutex 包 `rknn_run`**
- 取帧用**事件唤醒** `latest->wait_new(last_seq_, 400)`（替代 400µs 轮询），固定轮转避免重复处理同帧
- 并发数可由 `MODEL_SET_CONCURRENCY` / 模型 `manifest.worker_cores` 覆盖（`Application.cpp:363-373`）
- **CPU 亲和硬编码** `1ULL << (4 + id_)`（`WorkerPool.cpp:280-282`）⇒ worker0/1/2 → cpu4/5/6，**与 `worker_cores` 值无关**（该值只作 NPU core_mask）

### 10.5 全生命周期

| 操作 | IPC | 实现 |
|---|---|---|
| 列表 | `MODEL_LIST` | `Application.cpp:1862` |
| 导入 | `MODEL_IMPORT` | `:1821`（Web 先传文件到 `_incoming/`） |
| 校验 | `MODEL_VALIDATE` | `:1896` |
| 安装 | `MODEL_INSTALL` | `:1905` |
| 激活（热切换） | `MODEL_ACTIVATE` | `:1914` → `switch_active_model_runtime()`(`:2133`)：stop → 重建 worker → start → 首帧真推理验证 → **失败回滚旧模型** |
| 删除 | `MODEL_REMOVE` | `:2226` |
| 并发 | `MODEL_SET_CONCURRENCY` | `:2235` |

`rknn_destroy` 唯一调用点：`RKNNEngine.cpp:476`（`WorkerPool::stop()` 路径释放）。
**模型完整性**：仅校验 sha256，**无签名校验**（见 §17）。

---

## 11. Detection / Selector / Control / HID

### 11.1 Decode + NMS（`core/src/rknn/DecodeNMS.cpp`，899 行）

| 项 | 实现 | 证据 |
|---|---|---|
| 分发 | `:150-213` 按输出数量选择布局：**kSingle**（单输出，yolo261n / v26m e2e）、**kDfl**、**kDflDist**、**kDflPairDist**、**kE2e** | `[源码]` |
| 实际命中 | 当前 `sjzv11___1`（6 输出）命中 **多输出 DFL** 分支；`EP` 命中 `process_dfl` | `[推断]`（布局名对应） |
| 类别分数 | **已是 sigmoid 概率**，直接取 max/argmax；注释警告二次 sigmoid 会把 0.9 压到 0.71 造成**假阳性 flood** | `[源码]` `:163,192-209,229-267,287-303` |
| NMS | `:110-148` 按类别分组，IoU 阈值来自配置 | `[源码]` |
| 坐标映射 | `map_coords:837-864` | `[源码]` |

### 11.2 TargetSelector（`core/src/mouse/TargetSelector.*`）

选靶 / 迟滞 / 切换冷却 / 丢帧宽限；配置项 **20+**（`[源码]` `AimThread.cpp:90-115,188-222`）。
`[源码]` `AimThread.cpp:222` 自陈注释："此前算法已实现但这里没赋值" ⇒ 存在**选择参数装配分散**（见 §22 重复项 R7）。

### 11.3 Control / PID

| 项 | 值 | 证据 |
|---|---|---|
| 线程周期 | `interval_us` 默认 **4000 µs（250 Hz）** | `[源码]` `AimThread.cpp:20-25,85` |
| 控制器 | `Pid1Controller` = P/I/D + **predict** + **双 Kalman** | `[源码]` `Pid1Controller.hpp` |
| 初始参数 | `pid_x_.init(25,25,3,0.3,9900)` / `pid_y_.init(25,25,0,0.3,9900)`（pid1.cpp 原始参数） | `[源码]` `:140-164` |
| 运行期覆盖 | 由 `frame_profile->mouse.{kp,kd,predict}` 经 `configure()` 覆盖 | `[源码]` |
| 热键 | **上升沿**重置 PID | `[源码]` `:264-266` |
| 前馈/偏移补偿 | **无** | `[源码]` |

**未接线（仅测试引用，生产零调用）**：`core/src/control/RateLimit.hpp`、`Deadzone.hpp`、`OutputScale.hpp`。

### 11.4 HID 注入（关键事实）

| 项 | 结论 | 证据 |
|---|---|---|
| 注入路径 | `AimThread` → `OutputGate` → `IHidOutput` → `MouseControlClient` → `cmd.sock` → usbproxy → `/dev/raw-gadget` | `[源码]` `MouseControlClient.cpp:22-33` |
| MOVE 报文 | magic `0x4F50` + version + type + request_id + **dx/dy/wheel** = **20 字节**；`SOCK_SEQPACKET` | `[源码]` |
| 后端选择 | 配置 `output_backend=usb_proxy` | `[实测]` `10-device.json` |
| **`/dev/uinput` / `/dev/uhid`** | ⚠ **全仓零引用**；设备节点存在但**无进程打开** | `[实测]`+`[源码]` |
| **`core/src/hid/*`（17 文件）** | ⚠ **随包编译但运行期零实例化** | `[源码]` |
| **`HidForwarder` rx/tx** | ⚠ **默认不启动**（生产代码零实例化） | `[源码]` `HidForwarder.cpp:96-97` |
| 输出总闸 | `output_gate_allows()` = 静态总闸 + 运行态（`OutputGate.hpp:39`） | `[源码]` |

⇒ **唯一真实注入路径 = cmd.sock → usbproxy → raw-gadget**。`hid/` 整包与 uinput/uhid 是**历史遗留**。

---

## 12. 线程与资源

### 12.1 线程清单 `[源码]`

| 线程 | 创建位置 | 数量 | 停止方式 |
|---|---|---|---|
| Capture | `V4L2Capture.cpp:468` | 1 | `running_=false` + join |
| InferenceWorker | `WorkerPool.cpp:193/207` | **3** | `running_=false` + 事件唤醒 400µs + join |
| NpuMonitor | `NpuMonitor.cpp:58` | 0–1 | 间隔轮询 |
| AimThread | `AimThread.cpp:26` | 1 | `running_=false` + join |
| PreviewModule | `PreviewModule.cpp:380` | 1 | `running_=false` + join |
| IpcServer accept | `IpcServer.cpp:357` | 1 | `running_=false` + join |
| IpcServer 客户端 | `IpcServer.cpp:487` | **每连接 1（detached）** | 自结束 + fd 上限 |
| LicenseDaemon | `LicenseDaemon.cpp:80` | 1 | `stop()` |
| PhysicalMouseReader 主/事件 | `PhysicalMouseReader.cpp:108/85` | 2 | `running_=false` |
| HidForwarder rx/tx | `HidForwarder.cpp:96-97` | **0**（默认不启动） | 按需 |
| （usbproxy）libusb hotplug | `device-libusb.cpp:249` | 1 | 进程退出 |
| （usbproxy）mouse_control 连接 | `mouse_control.cpp:444` | 1+ | 进程退出 |

`[实测]` 停止态：core = **3 线程**（主 + 2 常驻）；usbproxy = **14**（13 `usb-proxy` + 1 `libusb_event`）。
`[推断]` 运行态 core ≈ **11 线程**（不含 IPC 每连接线程）。

### 12.2 同步原语

| 机制 | 位置 | 用途 |
|---|---|---|
| `std::atomic` | 各模块 `running_`/统计 | 无锁计数/启停 |
| `std::mutex` | `Application::runtime_lifecycle_mutex_` | **生命周期事务锁**（禁并发 stop/init/start） |
| `std::mutex` | `Application::config_persist_mutex_` | 配置落盘串行化 |
| `std::mutex` + `condition_variable` | `IpcServer`（`conn_fds_mutex_`/`cv_`） | 连接 fd 管理 + 退出等待 |
| `std::mutex` | `AimThread::status_mutex_` | 状态快照 |
| **无锁** | `pipeline/AimTargetMailbox.hpp`、`rknn/SpscQueue.hpp` | 跨线程帧/目标（`take_latest`/`wait_new`） |
| **SCHED_FIFO** | `RtSched.cpp` | 采集线程 FIFO 60（实测）+ worker 独立大核绑定 |

### 12.3 资源生命周期

| 资源 | 释放 | 风险 |
|---|---|---|
| V4L2 mmap + dma-buf | `close()`：STREAMOFF + munmap + 关 dma fd（实测日志确认） | **无泄漏**（注释 `:196` 专门处理上次残留） |
| RGA `importbuffer_fd` | 析构 | 缓存按 dma_fd 键有界；异常路径未逐一验证 `[待验证]` |
| RKNN context | `WorkerPool::stop()` | 实测停止后线程归 3、fd 归 7 ⇒ 已释放 |
| NPU `rknn_set_io_mem` | **框架无解绑 API** | **已知风险**（半绑态禁用回退） |
| IPC 客户端 fd | 线程结束关闭 | 有 `active_connections_` 管理；上限未逐一验证 `[待验证]` |
| **`cloud_session.json.tmp.*`** | **未清理** | ⚠ 实测残留 **37 个** |
| **OTA staging** | `finally`（`:519-525`） | ⚠ 板端仍留 `V1.0.17.ota.staging` 12M ⇒ 该路径未走到 finally `[推断]` |

### 12.4 内存 `[实测]` 停止态 core

`VmSize 857,364 kB` / `VmRSS 79,004 kB` / `Threads 3` / `fd 7`。
fd 明细：2 个 IPC socket、1 个 socket、1 个 `/dev/rga`、1 个 `/dev/dri/card1`、1 个 `/dev/null`。
**运行态内存/句柄峰值：未测量**（见 §21）。

---

## 13. Web / API

### 13.1 总量

| 应用 | 文件 | 路由数 | 状态 |
|---|---|---|---|
| 主面板 Flask app | `plugins/web/bin/ttbox-web.py`（**6,087 行**） | 约 **78 条** | **真实挂载** |
| v1 蓝图 `api_v1` | `plugins/web/api_v1.py`（882 行） | 20 条 + 25 条 not-implemented | **全部未注册（死代码）** |
| 框架桥接 `install_framework_api` | `plugins/web/framework_api.py` | 15+ 条 | **全部未注册（死代码）** |

未注册证据：无 `register_blueprint`；`bin/ttbox-web.py:2158-2159` 明写注册块已摘除；`plugins/web/__init__.py:1-10` 记为已移出出货包。

### 13.2 真实生效的核心 API（分类）

页面 `GET /`、`/desktop`、`/mobile`、`/activate`｜状态 `GET /api/{state,system,system/storage}`｜**配置** `GET/PUT /api/config`（PUT 走 IPC `SET_CONFIG`，**Web 不落盘**）｜控制 `POST /api/control/{start,stop}`｜标定 `GET/PUT/DELETE /api/control/calibration` + `POST .../start|cancel`｜模型 `GET /api/models` + `POST /api/models/{import,import-onnx,delete,select,rknn-concurrency,class-names,bind-preset}`｜预设 `GET/POST /api/presets` + `POST .../load|import`｜硬件 `GET/PUT /api/hardware/{mouse,display}`｜授权 `GET /api/license` + `POST /api/license/activate`｜OTA `POST /api/ota/install`、`/api/update/{install,check}`、`GET /api/update/status`｜动作曲线 `/api/motion-profiles*`、`/api/motion-training/sessions*`、`.../train`｜电源 `POST /api/system/{reboot,poweroff}`、`PUT /api/system/hostname`｜诊断 `POST /api/diagnostics/aim-trace`、`GET /api/diagnostics/usb-proxy.zip`（后者缺 `glob` 导入致 `hidg_devices.txt` 恒缺）｜预览 `GET /api/preview.mjpg`。

### 13.3 装饰性 / 未生效 API（约 25 条）

`GET /api/announcement`(2261 恒 503)、`POST /api/system/storage/expand`(2434 恒 501)、`PUT /api/system/web-port`(2466 恒 400)、`POST /api/system/reactivate`(2484 恒 400/409)、`POST /api/models/remote-frame-format`(3567 回显)、`POST /api/models/hailo-pipeline-depth`(3617 回显，**RK3588 无 Hailo**)、`GET /api/events`(4809 **恒空壳**)、`PUT /api/hardware/mouse/mode`(5203 恒 `ok:false`)、`POST /api/activation/reset-local-identity`(5651)、`GET/POST /api/activation/full-recovery`(5663/5688)、`/api/themes*`(5699–5759 硬编码单一 default)、`/api/branding/background*`(5769–5791 恒拒绝)、`POST /api/remote/{connect,import,delete}`+`GET /api/remote/models`(5956–5971 恒 `_remote_not_ready()`)、`POST /api/motion-profiles`(5837 只认内置 default)。

### 13.4 Web 直接写盘（绕过 Core）

| 路径 | 内容 | 证据 |
|---|---|---|
| `<TTBOX_PRESETS_DIR>`（默认 `/opt/ttbox/presets`） | 预设 JSON | `:3124,3683,3697,3788` |
| `<TTBOX_MODELS_ROOT>/installed/<id>/ui_meta.json` | UI 字段（tmp+`os.replace`） | `:3002-3005` |
| `<TTBOX_MODELS_ROOT>/_incoming/` | 上传中转 | `:3450-3454,3272-3275` |
| `/tmp/ttbox_onnx_convert` | ONNX 转换工作区 | `:3194,3241-3357` |
| `/var/lib/ttbox/ota/jobs/*.json` | **OTA 特权任务文件** | `:2515,2592-2606` |
| `/opt/ttbox/config/calibration.json` | 标定（**硬编码绝对路径**） | `:3824,3906-3920` |
| `/opt/ttbox/run/aim_trace.json` | 轨迹（**硬编码**，该目录不在 FHS 表内） | `:4764-4766` |
| `<TTBOX_CONFIG_DIR>/hardware_display.json` | 显示配置 | `:5266,5433` |
| `<TTBOX_CONFIG_DIR>/cloud_session.json` | 云会话（0600，tmp+fsync+replace）→ **tmp 泄漏源** | `:2073-2075`、`lib/cloud_session.py:57-87` |
| `<TTBOX_MOTION_PROFILES_DIR>` | 曲线档案 | `ttbox_motion/training.py:112-115,216,350` |
| `/etc/ttbox/web_credentials.json` → **改名 `.retired`** | 一次性 | `:1952-1959` |

**反例（重要）**：运行期配置（`runtime_profile` 等）Web **一律不落盘**，只走 IPC `SET_CONFIG`（`:2817,3743,4020,4106,4145,4183,4373,4504,4689,4821,5823`）。

### 13.5 Web 执行外部命令（无 `shell=True`，参数走 argv）

`hostname -I`、`hostnamectl set-hostname`、`os.system('systemctl reboot|poweroff')`、`systemctl {is-active,is-enabled,status,show}`、`busctl call … CanReboot/CanPowerOff`、`findmnt`/`lsblk`、`v4l2-ctl --query-dv-timing`、`bash edid_apply.sh`、`bash edid_patch_boot_image.sh`、`convert_onnx_to_rknn.py`。

### 13.6 假成功（实测确认 2 处）

| # | 位置 | 行为 |
|---|---|---|
| S1 | `ttbox-web.py:3487-3492` | 模型导入时元数据写失败被吞 ⇒ 仍返回"**导入成功**" |
| S2 | `ttbox-web.py:214-226` | `/api/system` 读盘失败返 0 ⇒ 仍返回 `ok:true` |

---

## 14. 配置 / 状态

### 14.1 配置分层 `[源码]` `ConfigManager.cpp:122-188`

| 层 | 文件 | 权限 | 升级行为 | 写回 |
|---|---|---|---|---|
| 出厂基线 | `/etc/ttbox/config.d/00-factory.json`（源 `deploy/config/00-factory.json`） | root:root 0644 | **每次安装整体替换** | **永不写** |
| 设备层 | `/etc/ttbox/config.d/10-device.json`（源 `deploy/config/10-device.json`） | root:ttbox 0664 | **仅当不存在时创建** | **唯一写回目标** |
| 编译期默认 | `config/default.json` | — | — | 仅开发机（无 `--config`/`TTBOX_CONFIG`） |

**合并规则**：目录内 `*.json` 按**文件名升序** `deep_merge`；对象递归、标量/数组整体替换；空目录 ⇒ **明确失败**。
**写回**：只写"相对基线的差异"（`layer_diff`）；原子性 = `tmp` + `fsync` + `rename` + 目录 `fsync`。
**优先级链**：`--config` > `TTBOX_CONFIG` > 编译期默认。
**三重同值断言**：`config/default.json`、`deploy/config/default.json.prod`、`deploy/config/00-factory.json` 共享键由 `scripts/ttbox_conventions_gate.sh:13,34,369` 断言一致。

### 14.2 环境变量（全量）

**Core**：`TTBOX_IPC_SOCKET`(`/run/ttbox/core.sock`)、`TTBOX_CONFIG`、`TTBOX_STATE`(`/opt/ttbox/state`)、`TTBOX_MODELS_ROOT`、`TTBOX_HID_ROOT`、`TTBOX_IPC_DEBUG`、`TTBOX_LICENSE_SERVER`/`TTBOX_APP_KEY`/`TTBOX_CLIENT_SECRET`。
**Web/插件**：`TTBOX_ROOT`、`TTBOX_IPC_SOCKET`、`TTBOX_IPC_TCP`、`TTBOX_PREFIX`、`TTBOX_SCRIPTS_DIR`、`TTBOX_MODELS_ROOT`、`TTBOX_CONFIG_DIR`、`TTBOX_PRESETS_DIR`、`TTBOX_WEB_CREDENTIALS`、`TTBOX_HDMIRX_EDID`、`TTBOX_MOTION_PROFILES_DIR`、`TTBOX_CLOUD_SESSION`、`TTBOX_CONVERT_WORKDIR`、`TTBOX_CONVERTER_SCRIPT`、`TTBOX_CONVERTER_PYTHON`、`TTBOX_CONVERT_CALIB_DIR`、`TTBOX_ALLOW_ONNX`、`TTBOX_PREVIEW_URL`、`TTBOX_PREVIEW_HOST/PORT`、`USB_PROXY_*`、`PYTHONPATH`。
**已删除（勿再当有效）**：`TTBOX_WEB_HOST`/`TTBOX_WEB_PORT`、core 的 `LD_LIBRARY_PATH`。

### 14.3 同一配置多来源 / 实际生效值

| 键 | 来源数 | 实际生效 | 备注 |
|---|---|---|---|
| IPC socket | 4 | `/run/ttbox/core.sock` | 跨语言门禁断言同值 |
| 模型库根 | 3 | `/var/lib/ttbox/models` | ⚠ Web unit 设 env、core 读配置，**两条路径独立** |
| 裁剪尺寸 | 2 | 640×640 | 采集层 SELECTION 失败 → RGA 承担 |
| 预览帧率 | 2 | 15 | 受限态取 `min(fps,5)` |
| 输出总闸 | 2 | **true** | `migrate_output_enabled` 一次性自愈 |
| worker 数量与 core_mask | 2 | `{1,2,4}` | 值 = NPU core_mask |
| CPU 亲和 | 0（**硬编码**） | worker→cpu4/5/6，capture→cpu4-7 | 与配置无关 |
| `capture_width/height` | 1（仅配置） | **无消费** | 死配置 |
| `crop_size` | 1（配置/面板） | **core 无消费** | Web 侧字段 |

### 14.4 状态语义（易混）

| 词 | 含义 | 真源 |
|---|---|---|
| `running` | Application 事件循环存活 | `GET_STATUS.data.running` |
| `runtime_running` | AI 流水线是否在跑 | `GET_STATUS.data.runtime_running` |
| `current_model_id` | **正在运行**的模型（非"激活"） | `GET_STATUS`（实测 `""`） |
| 激活模型 | — | `registry/active.json` = `sjzv11___1` |
| `enabled` | `mouse.enabled`/`output_enabled` | `10-device.json` |
| `activated` | 卡是否有效 | `GET_STATUS.data.license.activated` = true |
| `ready` | 至少一帧真 RKNN 推理成功进 Decode | `CoreRuntime::model_ready()` |
| `SUCCESS` | 更新器安装结果 | `state/ota_status.json` |
| `want_runtime_running` | 用户上次显式启停意愿 | `state/runtime_intent.json` |

---

## 15. 日志 / 错误 / 恢复

### 15.1 日志

| 项 | 现状 | 证据 |
|---|---|---|
| Core | 仅 **stdout/stderr 同步输出**（无文件 sink），经 systemd 进 journald | `[源码]` `common/Logger.*` |
| unit 重定向 | **无任何 `StandardOutput=`/`StandardError=`**（走 journald 默认） | `[源码]` `grep deploy/systemd` |
| Web/插件 | Python logging 到 stderr | `[源码]` |
| 预期日志目录 | `/var/log/ttbox/` **存在但为空** | `[实测]` |
| 日志轮转 | 无应用侧轮转（依赖 systemd-journald） | `[源码]` |
| **异步/落盘诊断包** | **无**（无 file sink、无归档导出） | `[源码]` |

### 15.2 吞异常 / 静默失败

| 类型 | 数量 | 位置 |
|---|---|---|
| C++ 空 catch 吞异常 | **4 处** | core/src 内 |
| Python `except: pass` | **12 处** | plugins/scripts |
| **真实假成功** | **2 处** | S1 模型导入(§13.6)、S2 `/api/system`(§13.6) |

### 15.3 Core dump / 崩溃取证

⚠ **无 core dump 机制**：`systemd` unit 未设 `LimitCORE`，`/proc/sys/kernel/core_pattern` 未配置应用侧落盘 ⇒ 崩溃后**无 stack 可查**。

### 15.4 看门狗与故障恢复

| # | 故障场景 | 是否有处理 | 机制 / 位置 |
|---|---|---|---|
| F1 | 采集卡死（**3s 无帧**） | ✅ 有 | `Application.cpp:1064-1261` 看门狗 |
| F2 | 推理卡死（**5s 无进展**） | ✅ 有 | 同上 |
| F3 | 启动失败 | ✅ 有 | **2s 重试** + `StartLimitBurst=5/300s` |
| F4 | 更新失败 | ✅ 有 | 更新冒烟自检（**200s 上限**）+ `rollback()` |
| F5 | HDMI 无信号 | ⚠ 部分 | `capture_missing` 出参区分；但 13:57 后 kernel 持续刷 `hdmirx` err（**未抑制**） |
| F6 | 模型激活失败 | ✅ 有 | `switch_active_model_runtime` 失败回滚 + 恢复运行 |
| F7 | 配置损坏 | ✅ 有 | `load()` 失败拒绝启动；`runtime_profile` 校验失败自愈写回 |
| F8 | 授权过期/失效 | ✅ 有 | `LicenseDaemon` 状态机 + 受限态降级（预览降 5fps） |
| F9 | OTA 包降级攻击 | ⚠ 仅 updater | `ttbox_ota_updater.py:493-498` 拒绝降级；**`release_install.sh` 可绕过** |
| F10 | 磁盘满 | ❌ 无 | 无阈值告警/自清（根分区已 75%） |
| F11 | USB gadget 失效 | ⚠ 部分 | `run-ttbox-usb-proxy.sh` 启动期等 UDC/鼠标；**运行期无重连**（`[推断]`） |

### 15.5 服务自愈

`ttbox-ensure.timer`（每 10min）→ `ttbox_ensure_services.sh` 幂等 `enable/start` 7 个 unit。**但 `ttbox-ensure.service` 本身 `disabled`**（timer 可直接激活 oneshot，不影响）。

---

## 16. 网络 / IPC

### 16.1 监听面

| 端口/socket | 绑定/权限 | 使用者 |
|---|---|---|
| `0.0.0.0:8000` | 全网可达，裸 HTTP | `ttbox-web` |
| `127.0.0.1:8001` | 仅回环 | `ttbox-preview` |
| `0.0.0.0:22` | 全网可达 | sshd |
| `/run/ttbox/core.sock` | 0660 root:ttbox | core ↔ web/preview/工具 |
| `/run/ttbox-mouse-passthrough/cmd.sock` | 0660 | core/web → usbproxy |
| `/run/ttbox-mouse-passthrough/event.sock` | 0660 | usbproxy → core |

### 16.2 Core IPC 协议

`core/src/ipc/IpcServer.cpp`：Unix socket，**NDJSON 行协议**（JSON + 末尾换行），**一连接一请求**，15 命令：
`PING`、`GET_STATUS`、`GET_CONFIG`、`SET_CONFIG`、`RUNTIME_CONTROL`、`MODEL_{LIST,IMPORT,VALIDATE,INSTALL,ACTIVATE,REMOVE,SET_CONCURRENCY}`、`ACTIVATE_LICENSE`、`ACTIVATE_CLOUD`、`GET_PREVIEW`。
帧尺寸从 `data.metrics.input_width/height` 读。
Python 侧唯一客户端：`plugins/web/lib/ipc.py`（socket 路径 = `TTBOX_IPC_SOCKET` > `paths.py::IPC_SOCKET_DEFAULT`）。

### 16.3 鼠标透传协议

`cmd.sock`/`event.sock`：`SOCK_SEQPACKET`，magic `0x4F50`，MOVE 报文 20 字节（`MouseControlClient.cpp:22-33`）。Web 侧 USB gadget 身份下发走同通道（`ttbox-web.py:5010-5050`）。

### 16.4 云 / OTA 网络

- OTA 下载：**仅 https**（scheme 白名单），旁车 `<url>.sign.json`
- 云授权：`TTBOX_LICENSE_SERVER` 端点 + `TTBOX_APP_KEY`/`TTBOX_CLIENT_SECRET`
- **无 WebSocket / 无 SSE**：`GET /api/events`(4809) **恒空壳** ⇒ 前端只能轮询
- **离线可用**：离线卡本地 Ed25519 验签 ⇒ 断网不失效

### 16.5 未测量

- 实际出网带宽/延迟、OTA 包下载速率、云端接口 P95 —— **未测量**
- IPC 单请求往返延迟、并发连接上限 —— **未测量**

---

## 17. 权限 / 安全

### 17.1 运行身份

| 进程 | 身份 | 权限缺口 |
|---|---|---|
| `ttbox-core` | **root:ttbox** | root（为写 sysfs：调频/风扇/时钟相关） |
| `ttbox-usbproxy` | **root:ttbox** | root（`/dev/raw-gadget` 0600） |
| `ttbox-web` | ttbox:ttbox | 非 root，但可 exec systemctl/edid 脚本 |
| `ttbox-preview` | ttbox:ttbox | 非 root |
| `ttbox-ota.service` | **root**（oneshot） | root |
| `ttbox-edid.service` | root | root |

⚠ **无 systemd `CapabilityBoundingSet=` / `NoNewPrivileges=` / `ProtectSystem=` / `PrivateTmp=`**（`[源码]` grep `deploy/systemd` 全 0 命中）⇒ **全靠 root 兜底，无 capability 收敛**。
⚠ **无 udev 规则**（`/etc/udev/rules.d/` 无 ttbox 相关）⇒ 设备节点权限来自内核默认。

### 17.2 Web 认证

⚠⚠ **Web 完全无认证** `[源码]` `ttbox-web.py:1944-1948`：激活后 **局域网任意主机** 可调用全部 API（含 `reboot`/`poweroff`/改主机名/改 boot 镜像/改 EDID/写 OTA 特权任务）。面板监听 `0.0.0.0:8000`。
历史 `web_credentials.json` 已**改名 `.retired`**（`:1952-1959`），即认证机制**已移除**。

### 17.3 完整性 / 签名

| 对象 | 校验 | 缺口 |
|---|---|---|
| OTA 包 | sha256 + **Ed25519 双因子验签**（4 签名域） | ✅ |
| 模型 `.rknn` | **仅 sha256** | ❌ **无签名**（可被同名替换） |
| 配置 | 无 | ❌ 无防篡改 |
| 授权卡 | Ed25519（离线）/ 云端 | ✅ |

### 17.4 权限 / 目录

| 路径 | 权限 | 备注 |
|---|---|---|
| `/var/lib/ttbox/license/`、`activation/` | 0700 | 授权数据 |
| `/var/lib/ttbox/ota/jobs/` | 0770 root:ttbox | 特权通道入口 |
| `/etc/ttbox/config.d/00-factory.json` | 0644 root:root | 基线 |
| `/etc/ttbox/config.d/10-device.json` | 0664 root:ttbox | 设备层 |
| `/opt/ttbox/config/cloud_session.json` | 0600 | 云会话 |
| `/dev/raw-gadget`、`/dev/uinput`、`/dev/uhid` | **0600 root:root** | uinput/uhid 未被使用 |

### 17.5 风险小结

1. **无认证 + 全网监听**（§17.2）：单机模式可接受，**组网/云管场景不可接受**。
2. **模型无签名**（§17.3）：供应链攻击面。
3. **无 capability 收敛**（§17.1）：一旦 core/web 被拿下即 root。
4. **无 udev 规则**：设备权限依赖内核默认，不可控。
5. **无 core dump**（§15.3）：崩溃不可取证。

---

## 18. 编译 / 部署

### 18.1 编译

| 项 | 值 | 证据 |
|---|---|---|
| C++ 标准 | **C++17**（REQUIRED ON，EXTENSIONS OFF） | `core/CMakeLists.txt:60-62` |
| 项目版本 | `project(ttbox_core VERSION 1.0.16)`（**硬编码**，与运行期 `kCoreVersion=V1.0.17` **不同源**） | `:47` |
| 默认构建型 | Release | `:64-66` |
| 警告 | `-Wall -Wextra -Wpedantic` | `:97-102` |
| 路径消除 | `-ffile-prefix-map=${TTBOX_SOURCE_ROOT}=.` | `:114-117` |
| **RPATH/RUNPATH** | `BUILD_RPATH` = `INSTALL_RPATH` = **`$ORIGIN/../lib`** | `:506-511` |
| `LD_LIBRARY_PATH` | CMake 内**无任何设置** | grep 0 命中 |
| 链接库 | OpenSSL、librga、RKNN、libjpeg、OpenCV（Windows 另加 ONNX Runtime、ws2_32） | `:370-418` |
| 交叉工具链 | `aarch64-linux-gnu-{gcc,g++,ld,ar,objcopy,objdump}` | `deploy/cmake/toolchain-aarch64.cmake:17-25` |
| sysroot | 空占位，由 `-DCMAKE_SYSROOT=` 传入 | `:28` |

⚠ **宿主 ctest 必须在 ASCII 路径跑**（中文路径恒少 2 例假失败）。

### 18.2 构建 → 打包 → 部署链

- `scripts/ttbox_build_release.sh`（出货唯一入口）：固定配置向量（强制 `TTBOX_PROJECT_ROOT=/opt/ttbox`、**`TTBOX_CORE_BUILD_AUTH=OFF`**、`-G Ninja`）→ clean → configure → **CMakeCache 复核** → 依赖门禁 → build → **产物后置门禁**（路径锚、字符串表、usbproxy 诚实性）→ **payload 闭集自检**（bin/ 恰 1 文件、lib/ 只 `librknnrt.so` 且 sha256 同源、`readelf` RUNPATH/NEEDED）→ 可选二次 clean build 复现对照 → 留档 `docs/build/release-records/RELEASE_BUILD-<UTC>-<commit7>.md`。
- `ttbox_pack_ota.sh`：版本真源 = `core/include/ttbox/core/version.hpp::kCoreVersion`；白名单 `deploy/pack_manifest.txt`；断言 0–6；`--priv <pem>` + `--key-id`（默认 `ttbox-ota-2026b`）签名。**仓库内只有公钥**。
- `ttbox_pack_delta.sh`：断言 d0–d3；**不签名**（分发服务器统一签）。
- `ttbox_release_install.sh`：写 `releases/<ver>/` + 原子切 `current`；建 `/var/lib/ttbox/ota/jobs`(0770)、`state`、模型库 6 子目录；建过渡软链；装 unit；`--rollback [ver]`。
- 可复现约束 `docs/build/build-reproducibility.md`：**同 commit + 同配置向量 ⇒ 产物逐位相同**（不得声称"任意两次相同"）。`vector_hash` **不含 commit**。
- ⚠ **构建向量顺序敏感**：`-DCMAKE_SYSROOT` 必须在 `-DCMAKE_TOOLCHAIN_FILE` **之前**（顺序反了 vector_hash 会变）。

### 18.3 构建产物现状（本地）

4 个构建目录：`build-aarch64-t148`(27M)、`build-aarch64-t148-repro`(20M)、`core/build-aarch64-t148`(24M)、`core/build-ascii`(27M)；另有 2 个日志。**`core/` 内有两个构建目录**（非标准位置）。

---

## 19. 安装 / OTA

### 19.1 安装（`ttbox_release_install.sh` + `ttbox_fhs_init.sh`）

| 步骤 | 行为 |
|---|---|
| 铺 release | `${TTBOX_PREFIX}/releases/<ver>/` |
| 切版本 | 原子替换 `current` 软链 |
| 建目录 | `/var/lib/ttbox/ota/jobs`(root:ttbox 0770)、`/opt/ttbox/state`、模型库 6 子目录(ttbox:ttbox 0775) |
| 兼容软链 | `/opt/ttbox/{plugins,scripts}` → `current/*` |
| 装 unit | 铺 `deploy/systemd/*` 到 `/etc/systemd/system/` |
| 重载 | `systemctl daemon-reload` |
| 配置 | `00-factory.json` **整体覆盖**；`10-device.json` **仅不存在时创建** |

### 19.2 OTA 7 步（`scripts/ttbox_ota_updater.py`）

① URL scheme 白名单（**仅 https**）→ ② 下载包 + **旁车** `<url>.sign.json` → ③ sha256 比对 → ④ **Ed25519 验签**（canonical JSON，4 签名域 `sha256/version/built_at/key_id`）→ ⑤ 展开到 `releases/<ver>.ota.staging/`（增量在此合并）→ ⑥ 全量 sha256 复验（对照 `RELEASE_MANIFEST.json`） + **⑥b 降级拒绝**（`:493-498`）→ ⑦ 原子发布（`release_install.sh --activate`）+ 健康检查（三服务 active + core IPC 返回非空 version）；失败 → `rollback()`；`finally` 清临时目录与 staging。

**触发通道**：web(ttbox) 写 `/var/lib/ttbox/ota/jobs/*.json` → `ttbox-ota.path`(PathExistsGlob) → `ttbox-ota.service`(root oneshot)。

### 19.3 OTA 会 / 不会碰到什么（文件级）

| 对象 | OTA 行为 | 证据 |
|---|---|---|
| `/opt/ttbox/releases/<ver>` | 新增/替换 | `[源码]` |
| `current` 软链 | 原子切换 | `[源码]` |
| `/etc/systemd/system/ttbox-*` | **重装 + daemon-reload** | `[源码]` |
| `/etc/ttbox/config.d/00-factory.json` | **整体替换** | `[源码]` |
| `/etc/ttbox/config.d/10-device.json` | **不覆盖** | `[源码]` |
| `/var/lib/ttbox/**` 数据内容 | **不触碰**（模型/授权/激活/OTA 归档） | `[源码]` |
| 版本保留 | **保留 2 版**（`TTBOX_KEEP_VERSIONS=2`） | `[源码]` |
| **配置迁移器** | ❌ **不存在**（配置键变更无迁移机制） | `[源码]` |
| **降级拒绝** | ⚠ **只在 updater**，`release_install.sh` 手工调用可绕过 | `[源码]` |
| **启动期完整性校验** | ❌ **不存在**（不验 release 树签名） | `[源码]` |

**OTA 残留**：板端 `releases/V1.0.17.ota.staging` 12M 未清（§12.3）。

---

## 20. 测试

### 20.1 总量 `[实测]`

| 套件 | 数量 | 说明 |
|---|---|---|
| **CTest** | `add_test` **47 条**注册；**host 默认实际 41**；板端 `TTBOX_CORE_BUILD_HW_TESTS=ON` 时 47 | 主目标 `ttbox_core_tests` 内含 145/142 个 C++ 用例（平台差） |
| pytest `framework/tests` | **39** | `--collect-only` |
| pytest `platform/tests` | **61** | 同上 |
| pytest `plugins/web/tests` | **476** | 同上 |
| 板端 API 脚本 | 4（`tests/api/*.sh`） | 需板端 web 在跑 |
| 板端监控脚本 | 6（`tests/monitor/*`） | 长时采样/EDID 矩阵 |

Python 合计 **576**。

### 20.2 PASS / FAIL / SKIP

- CTest：`core/CMakeLists.txt:610-887` 注册 47 条；**本机历史实测 41/41 PASS**（记忆基线）；**本次未重跑**（避免构建）。
- pytest：本次**只 collect，未执行**；记忆基线 **framework 39 + platform 61 + web 476 全绿**。
- **已知排除**：`test_web_calibration_apply.py`、`test_web_recoil_translation.py` 为脚本风格（import 期 `sys.exit()`），被 `conftest.py:13-16` `collect_ignore`，**不计入 476**。
- ⚠ **未实测项**：本次审计**没有重新执行任何测试**，上述 PASS 来自历史基线。

### 20.3 真实覆盖 vs 桩

| 套件 | 真实/桩 | 证据 |
|---|---|---|
| C++ | **真实逻辑**（链接真实 `ttbox_core` 静态库） | `core/CMakeLists.txt:609` |
| framework | **真实逻辑，零 mock** | `grep -i mock` 0 命中 |
| platform | **真实逻辑**（进程/服务边界用**注入替身**） | `MockProcessAdapter`/`MockServiceAdapter` 是注入双，SUT 真实 |
| web | **真实 web 源码，core IPC 是桩** | 22 个测试文件 `spec_from_file_location` 加载**真实** `ttbox-web.py`；仅 monkeypatch `ipc_request` |
| 板端脚本 | **真实副作用** | `tests/monitor/real_function_test.sh:2-4` 自陈 |

**覆盖盲区**：
- `V4L2Capture` 真实采帧、`RgaProcessor` 真实缩放 → 仅 HW 测试（默认不注册）
- Web 的 **core IPC 协议本身**（`IpcServer.cpp`）在 web 测试中是桩
- `usbproxy` 仅 `test_hid_layout`/`test_inject_clock`（预编译 exe，**未纳入 CTest**）
- `image/` 镜像烘焙、`scripts/` 运维脚本 → **无自动测试**

---

## 21. 性能

> **未测量。** 流水线（采集/推理/瞄准）长期处于停止态（`runtime_running=false`），且本阶段禁止启停服务，因此**无法观测任何负载指标**。以下逐项标注"未测量"，禁止推断。

| 指标 | 状态 | 说明 |
|---|---|---|
| 采集 FPS | **未测量** | 需运行态 |
| 端到端延迟（HDMI → 鼠标位移） | **未测量** | 需运行态 + 真源信号 |
| 推理耗时 infer_ms | **未测量** | 运行态统计项（停止态为 0） |
| RGA resize_ms | **未测量** | 运行态统计项 |
| Decode/NMS 耗时 | **未测量** | 运行态统计项 |
| NPU 利用率（3 核） | **未测量** | `/sys/kernel/debug/rknpu/load` 当前 0% |
| CPU 占用（负载下） | **未测量** | 停止态 idle 值见下 |
| 内存 / 句柄峰值（负载下） | **未测量** | 停止态 RSS 79MB |
| NPU/SoC 温度（负载下） | **未测量** | 当前 51–52C 为空闲 |
| 丢帧率 / 队列深度 | **未测量** | 运行态统计项 |
| OTA 下载/安装耗时 | **未测量** | 依赖网络 |
| IPC 往返延迟 | **未测量** | — |

**仅有可用作"基线锚点"的停止态实测值**（非性能结论，仅为上下文）：
- 模型加载：**85.9–94.3 ms/worker**（3 worker 串行加载）
- RKNN 预热：**25–28 ms/3 次**（`RKNNEngine.cpp:525`）
- 停止态 CPU：usbproxy 16.0% / core 4.0% / preview 1.4% / web 0.9%（`ps %CPU`，为进程生命周期均值）
- 停止态 RSS：core 79 MB / web 0.6% / preview 0.2%

**结论**：性能基线**必须**在业主授权进入运行态后补测，否则新框架无回归对照。

---

## 22. 重复 / 遗留

### 22.1 重复实现

| # | 重复项 | 位置 | 说明 |
|---|---|---|---|
| R1 | **两套 IPC 客户端** | `plugins/web/lib/ipc.py`（唯一走货）vs `api_v1.py:10-18,95-153`（同构重复，未注册） | `[源码]` |
| R2 | **两套 API 路由表** | `ttbox-web.py`（真实挂载 78 条）vs `api_v1.py`（20+25 条，全未注册） | `[源码]` |
| R3 | **两处 OTA 签名公钥** | `deploy/keys/ttbox-ota-2026b.pub` 与 `tools/ota/keys/ttbox-ota-2026b.pub` | `[源码]` |
| R4 | **两份 dtb** | `deploy/dtb/rk3588-orangepi-5-plus.dtb` 与 `image/factory/rk3588-orangepi-5-plus.dtb` | `[源码]` |
| R5 | **两套 IPC 文档** | `docs/ipc-protocol.md` 与 `docs/protocols/ipc-protocol.md` | `[源码]` |
| R6 | **两套"默认配置"** | `config/default.json`（编译期）与 `deploy/config/{default.json.prod,00-factory.json}`（出货） | 门禁断言同值 |
| R7 | **两套目标选择实现** | `mouse/TargetSelector.*` 与 `AimThread.cpp` 内选择参数装配（`:222` 自陈"此前算法已实现但这里没赋值"） | `[源码]` |

### 22.2 遗留 / 死资产（**引用关系已确认**）

> 用户要求：不能因名字叫 old 就认定废弃。下表每项都给出**引用关系确认结论**。

| 资产 | 引用关系确认 | 结论 |
|---|---|---|
| `plugins/web/api_v1.py`（882 行） | 全仓 grep `register_blueprint` **0 命中**；`ttbox-web.py:2158-2159` 明写注册块已摘除；`plugins/web/__init__.py:4-6` 记为"已移出出货包" | **确认死** |
| `plugins/web/framework_api.py` | 无任何 `install_framework_api()` 调用点 | **确认死** |
| `scripts/legacy/ttbox_m2_license_accept_v1_authflow.py` | 无调用点；目录名 legacy | **确认死** |
| `platform/`（32 文件） | **不入出货包**；板端 release 树**无** `platform/`；仅 `platform/tests` 引用自身；无 unit 引用 | **确认死**（仅测试自服务） |
| `framework/`（27 文件） | **仍随出货包**（板端 release 树含 `framework/`）；`ttbox-web.py` 有 `sys.path.append`，但**全仓无 `FrameworkRuntime` 实例化调用点** | ⚠ **随包但运行期零引用** |
| `core/src/hid/*`（17 文件） | 随包编译；`HidForwarder` 生产代码**零实例化**；全仓无 `/dev/uinput`/`/dev/uhid` | **确认运行期死** |
| `plugins/{model,preview,system,fan,log,monitor,network,wifi,upgrade}/`（8 个非 web 插件） | **无对应 systemd unit**；入口是 `signal.pause()` 占位；**仍随包** | ⚠ **随包但无启动路径** |
| `core/src/controller/`、`core/src/coordinate/` | `find` **0 文件** | **空目录** |
| `core/src/detector/IDetector.hpp` | 17 行空接口壳，实现已迁 `rknn/` | **死接口** |
| `core/src/preview/PreviewModule_stub.cpp` | 与真实实现并存，编译开关未确认 | `[待验证]` |
| `deploy/ota-out/1.5.30~34/`（10 文件 16M） | 属旧 `1.5.x` 谱系 | **旧谱系残留** |
| `usbproxy/tests/*.exe`（2 个预编译） | 二进制入库 | 遗留 |
| `core/third_party/onnxruntime/`（Windows 专用） | aarch64 出货不链接 | 平台专用 |
| `core/src/control/{RateLimit,Deadzone,OutputScale}.hpp` | 仅测试引用，生产零调用 | **未接线** |
| `ttbox-web.py` 约 25 条装饰性路由 | 各恒返回固定值 | 装饰性 |

### 22.3 数据目录分裂

预设/曲线/凭据仍在 `/opt/ttbox/{config,presets}`；`/var/lib/ttbox/{presets,motion-profiles}` **已建但无人写** ⇒ OTA"不触碰客户数据"的保证**只覆盖 `/var/lib/ttbox`**，覆盖不到 `/opt/ttbox` 内数据。

### 22.4 板端垃圾清单 `[实测]`

`config/cloud_session.json.tmp.*` **×37**｜`releases/V1.0.17.ota.staging` 12M｜`models/_incoming/*.rknn` **×5**｜`ota/jobs/.probe-*` ×2｜`/tmp/aim_probe.{py,b64}` + `cal.json` + `cfg.json` + `st.json`。

---

## 23. 商业化缺口

| # | 缺口 | 现状 | 影响 |
|---|---|---|---|
| G1 | **解绑 / 换机流程** | **无**（`/api/activation/reset-local-identity` 恒 400） | 客户换机即需人工介入 |
| G2 | **试用发放** | **无**（无试用卡/时限机制） | 无法做转化漏斗 |
| G3 | **恢复出厂** | **桩**（`/api/activation/full-recovery` 恒拒绝） | 返修/二次销售无路径 |
| G4 | **批量管理 / 多机管控** | **无**（单机 Web，无云端批量接口） | 无法做 B 端 |
| G5 | **诊断包导出** | **部分**（`/api/diagnostics/usb-proxy.zip` 有，但 `hidg_devices.txt` 恒缺；无整机诊断包） | 售后取证弱 |
| G6 | **OTA 断点续传** | **无**（下载失败即重来） | 弱网升级失败率高 |
| G7 | **配置迁移器** | **无**（配置键变更无迁移） | 升级后行为漂移 |
| G8 | **启动期完整性校验** | **无**（不验 release 树签名） | 落盘篡改不可检测 |
| G9 | **模型签名校验** | **无**（仅 sha256） | 供应链攻击面 |
| G10 | **Web 认证 / 权限分级** | **无**（全开） | 不可组网 |
| G11 | **授权在线校验/回收** | **有但弱**（离线卡本地验签可离线跑） | 无法在线吊销 |
| G12 | **崩溃取证** | **无 core dump** | 现场问题不可复现 |
| G13 | **压力/性能基线** | **未测量**（§21） | 无回归对照 |
| G14 | **quarantine 面板入口** | **无**（`models/quarantine/` 目录存在但 UI 无入口） | 隔离模型不可管理 |
| G15 | **多型号适配** | **有骨架**（模型 manifest + adapter），但仅 1 款机型 | 扩展成本未知 |

---

## 24. 保留 / 封装 / 重写 / 废弃 / 待验证

### 保留（可直接作新框架基础）

| 资产 | 理由 |
|---|---|
| `core/src/{capture,rga,rknn,model,ipc,common,config}` | 职责单一、接口清晰、有测试、实测链路成立 |
| `core/src/output/*`（`IHidOutput` 抽象 + OutputGate） | 后端可插拔，总闸语义明确 |
| `core/src/common/Paths.hpp` | 跨语言路径单点真源，设计正确 |
| `usbproxy/`（C++ 透传代理） | 独立进程、协议清晰、`lib/` 自包含 |
| `deploy/systemd/*` | 单元写得极详细，历史故障有注释留档 |
| `scripts/ttbox_ota_updater.py` + `ttbox_release_install.sh` + `ttbox_pack_*.sh` | 7 步 OTA + 验签 + 回滚，工程化程度高 |
| `tools/{license,ota,converter}` | 离线工具链完整 |
| `plugins/web/lib/{ipc,paths}.py` | 边界清晰；`paths.py` 是单点真源 |
| `ttbox_motion/` | 独立算法模块 |
| 全部测试（C++ 47 / Python 576） | 真实逻辑为主，是重构安全网 |

### 封装（功能可用，需重新隔离）

| 资产 | 需隔离的点 |
|---|---|
| `core/src/aim/AimThread.cpp` + `mouse/*` | 瞄准算法与配置读取耦合；`TargetSelector` 参数装配分散 |
| `core/src/app/Application.cpp`（2,265 行） | 总装职责过载（CLI/配置/授权/IPC/模型/生命周期/冒烟全在一类） |
| `core/src/ipc/IpcServer.cpp`（1,212 行） | 15 命令分发在单文件 |
| `plugins/web/bin/ttbox-web.py`（6,087 行 / 78 路由） | 应拆 Blueprint（config/models/license/motion/update） |
| `plugins/web/templates/index.html`（**13,406 行**） | CSS 5,188 + HTML 1,000 + JS 7,200 同文件；301 个 id |
| `core/src/auth/*`（25 文件） | 授权子域可再分层；`OfflineCardClient`/`TtboxLicenseClient` 双实现 |
| `plugins/web/lib/cloud_session.py` | **先修 tmp 泄漏**再封装 |

### 重写（结构不适合商业化）

| 资产 | 理由 |
|---|---|
| Web 前端 `index.html` 单文件 | 无模块化、无构建、无组件边界 |
| Web 配置读写路径治理 | 硬编码绝对路径 + 多来源；应统一到 `paths.py` |
| `core/src/hid/*`（17 文件） | 文件数偏多、运行期零实例化 |
| `image/`（36 脚本 + 17 steps） | 无版本、无幂等声明、无自动测试 |

### 废弃（引用关系已确认无使用）

`plugins/web/api_v1.py`｜`plugins/web/framework_api.py`｜`scripts/legacy/*`｜`platform/`｜`core/src/hid/*`｜`core/src/controller/`+`coordinate/`（空）｜`core/src/detector/IDetector.hpp`｜`core/src/control/{RateLimit,Deadzone,OutputScale}.hpp`（未接线）｜`deploy/ota-out/1.5.30~34/`｜`usbproxy/tests/*.exe`｜`core/third_party/onnxruntime/`（Windows 专用）｜约 25 条装饰性路由。

### 待验证（证据不足）

| 项 | 缺什么证据 |
|---|---|
| `framework/` 随包的实际用途 | 有 `sys.path.append`，但无 `FrameworkRuntime` 实例化点 |
| 8 个非 web 插件是否曾被 unit 拉起 | 无 unit；入口 `signal.pause()` |
| `HidForwarder` rx/tx 是否可能被配置启用 | 运行态未观察；需带日志复跑 |
| `PreviewModule_stub.cpp` 编译条件 | 需查 CMake 分支 |
| `aim_trace.json` 目录 `/opt/ttbox/run/` 是否预建 | 板端未见该目录 |
| `/api/v1/*` 是否被外部客户端依赖 | 需查分发服务器/客户端日志 |
| `process_width/height`、`resize_method` 消费点 | grep 未明确定位 |
| 运行态线程数/资源/性能 | 需业主授权进运行态（§21） |

---

## 25. 问题总表

| # | 问题 | 位置 | 证据 | 影响 | 优先级 | 后续建议 |
|---|---|---|---|---|---|---|
| Q1 | `cloud_session.json.tmp.*` 泄漏 **37 个** | 板端 `/opt/ttbox/config/` | `ls \| grep -c tmp`=37 | 目录污染、可耗尽 inode | **高** | 写失败即删 tmp；加启动期清扫 |
| Q2 | OTA staging 残留 12M | 板端 `releases/V1.0.17.ota.staging` | `du -sh`=12M | 占盘（根 75%）、语义混乱 | **高** | 修 `finally` 未覆盖路径；启动期清理 `*.staging` |
| Q3 | Web **完全无认证** + `0.0.0.0:8000` | `ttbox-web.py:1944-1948` | 源码 + `ss -tulpn` | 局域网任意主机可 reboot/改 boot 镜像 | **高** | 恢复认证或收敛到回环/反代 |
| Q4 | 采集层硬件裁剪不生效 | `V4L2Capture.cpp:285` | journal WARN `Inappropriate ioctl` | RGA 承担 56× 缩放，性能损失 | **高** | 改 RGA 前置 crop 或换支持 SELECTION 的采集路径 |
| Q5 | 模型无签名校验 | `core/src/model/*` | 仅 sha256 | 供应链攻击面 | **高** | 引入模型签名 + 校验 |
| Q6 | 无 core dump 机制 | systemd unit / core_pattern | 无 `LimitCORE` | 崩溃不可取证 | **高** | 配 `LimitCORE` + `coredumpctl` |
| Q7 | 无 capability 收敛 / 无 udev 规则 | `deploy/systemd/*` | grep 0 命中 | root 全权，攻击面大 | **中** | 加 `CapabilityBoundingSet` 等 |
| Q8 | 配置无迁移器 | `ttbox_release_install.sh` | 无迁移逻辑 | 升级后行为漂移 | **中** | 加配置版本号 + 迁移器 |
| Q9 | 启动期无完整性校验 | 同上 | 无 release 树验签 | 落盘篡改不可检测 | **中** | 启动校验 manifest 签名 |
| Q10 | 降级拒绝可被绕过 | `release_install.sh` | 只在 updater | 回滚/降级攻击 | **中** | 把降级检查下沉到 install |
| Q11 | 死代码整包 `api_v1.py`+`framework_api.py` | `plugins/web/` | 无 `register_blueprint` | 1,000+ 行维护负担 | **中** | 删除（引用关系已确认） |
| Q12 | `core/src/hid/*` 17 文件运行期零实例化 | `core/src/hid/` | 无 `/dev/uinput` 引用 | 随包体积、误解风险 | **中** | 确认后删除 |
| Q13 | `framework/`+8 插件随包但无启动路径 | `framework/`、`plugins/*` | 无 unit、无实例化 | 包体虚增、审计噪声 | **中** | 移出出货包或补齐 unit |
| Q14 | `platform/` 死代码 | `platform/` | 不入包、零引用 | 维护负担 | **中** | 移入测试目录 |
| Q15 | 约 25 条装饰性 API | `ttbox-web.py` | 恒返回固定值 | UI 假象、维护负担 | **中** | 删除或标注 501 |
| Q16 | 死配置 `capture_width/height` | `deploy/config/00-factory.json:65-66` | core/src 0 命中 | 排障误导 | **中** | 删或接线 |
| Q17 | `worker_cores` 命名误导 | `00-factory.json` | 实为 NPU core_mask | 误读为 CPU 核号 | **中** | 改名 `npu_core_masks` |
| Q18 | `project VERSION` 与 `kCoreVersion` 不同源 | `core/CMakeLists.txt:47` | 1.0.16 vs V1.0.17 | 版本双轨 | **中** | 单一真源 |
| Q19 | 无统一 Storage 层 | 分散各处 | §8 | 路径散落 | **中** | 引入 Storage 抽象 |
| Q20 | 硬编码绝对路径绕过 `paths.py` | `ttbox-web.py:3824/3854/4765` | 源码 | 违反自家纪律 | **中** | 统一到 `paths.py` |
| Q21 | 无 WebSocket/SSE，`/api/events` 空壳 | `ttbox-web.py:4809` | 源码 | 前端只能轮询 | **中** | 实现或删除 |
| Q22 | 2 处真实假成功 | `:3487-3492`、`:214-226` | 源码 | 用户被误导 | **中** | 修错误传播 |
| Q23 | 数据目录分裂 | `/opt/ttbox` vs `/var/lib/ttbox` | §22.3 | OTA 数据保护覆盖不全 | **中** | 统一数据根 |
| Q24 | `ttbox-time-guard.sh` 不在仓库 | 板端 `/usr/local/sbin/` | 本地 grep 无 | 仓库/镜像不同源 | **中** | 纳入仓库 |
| Q25 | 两套版本谱系并存 | `dist/`、`deploy/ota-out/` | `ls` | 出包选错 | **中** | 归档旧谱系 |
| Q26 | `/dev/uinput`+`/dev/uhid` 残留 | 板端 `/dev/` | 无进程打开 | 无用节点 | **低** | 确认后不加载 |
| Q27 | HDMI 无信号刷 kernel err | `hdmirx` driver | journal | 日志噪声 | **低** | 应用侧抑制或设阈值 |
| Q28 | `usb-proxy.service` 冲突列表指向不存在服务 | `ttbox-usbproxy.service:28` | `not-found` | 每次启动 5 次无效 systemctl | **低** | 清理列表 |
| Q29 | `/var/log/ttbox/` 空目录 | 板端 | `ls` | 设计残留 | **低** | 删或接线 |
| Q30 | release 树含 `__pycache__` | `releases/V1.0.1{6,7}` | `diff -rq` | 内嵌构建机信息 | **低** | 打包前清 pyc |
| Q31 | 模型 `_incoming/` 残留 5 个 rknn | 板端 | `find` | 占空间 | **低** | 清残留 |
| Q32 | OTA 探针 `.probe-*` 残留 | 板端 | `find` | 垃圾 | **低** | 清残留 |
| Q33 | `/tmp` 排障残留 | 板端 `/tmp` | `ls` | 垃圾 | **低** | 清残留 |
| Q34 | README 版本过期 | `README.md:5,95` | 写 V1.0.01 | 新人误导 | **低** | 更新 |
| Q35 | `LicenseStore STATE_MISSING` 每次启动 WARN | `LicenseStore.cpp:286` | journal | 噪声 | **低** | 区分首启 |
| Q36 | `docs/build/release-records/` 66 份线性增长 | 仓库 | `find` | 仓库膨胀 | **低** | 定期归档 |
| Q37 | `PreviewModule_stub.cpp` 并存 | `core/src/preview/` | 源码 | 编译分支不明 | **待验证** | 查 CMake |
| Q38 | `process_width/height`、`resize_method` 消费点不明 | 配置 | grep 未定位 | 可能死配置 | **待验证** | 定位消费点 |
| Q39 | `aim_trace.json` 目录未预建 | `/opt/ttbox/run/` | 板端未见 | 写入可能静默失败 | **待验证** | 确认目录 |
| Q40 | 性能基线缺失 | 全链路 | §21 | 无回归对照 | **待验证** | 授权后补测 |

---

## 附：28 项验收清单（本次审计自检）

| # | 验收项 | 状态 | 证据位置 |
|---|---|---|---|
| 1 | 确认仓库 HEAD 与工作树状态 | ✅ | §1，`6ec0fd9`，tracked 变更 0 |
| 2 | 盘点本地顶层目录与体积 | ✅ | §2.1 |
| 3 | 建立本地模块 → 调用者 → 使用状态表 | ✅ | §2.2 |
| 4 | 标出重复实现 | ✅ | §22.1（R1–R7） |
| 5 | 标出死代码/遗留（**含引用关系确认**） | ✅ | §22.2 |
| 6 | 盘点第三方代码与运行时库 | ✅ | §2.3、§7 |
| 7 | 实测板端 FHS 目录布局 | ✅ | §3.1 |
| 8 | 对比版本目录差异 | ✅ | §3.2 |
| 9 | 还原完整启动链 | ✅ | §4.1 |
| 10 | 实测服务运行态 / 进程 / 端口 | ✅ | §4.2–4.3 |
| 11 | 实测当前运行状态（IPC 只读） | ✅ | §4.4 |
| 12 | 建立脚本清单（按调用者） | ✅ | §5.1 |
| 13 | 逐一列出注入位置（启动期自动执行） | ✅ | §5.2（I1–I14） |
| 14 | 盘点硬件资源（CPU/NPU/内存/存储/设备） | ✅ | §6 |
| 15 | 还原视频采集链路 + 标出失效配置 | ✅ | §8 |
| 16 | 还原 RGA 链路 | ✅ | §9 |
| 17 | 还原 RKNN/模型链路（含零拷贝/并发） | ✅ | §10 |
| 18 | 还原 Detection/Selector/Control/HID | ✅ | §11 |
| 19 | 盘点线程/同步/资源/内存 | ✅ | §12 |
| 20 | 盘点 Web/API（含未生效路由、写盘、假成功） | ✅ | §13 |
| 21 | 盘点配置分层/环境变量/多来源/状态语义 | ✅ | §14 |
| 22 | 盘点日志/错误/看门狗/故障恢复矩阵 | ✅ | §15 |
| 23 | 盘点网络/IPC/协议/离线能力 | ✅ | §16 |
| 24 | 盘点权限/安全（认证/签名/能力收敛） | ✅ | §17 |
| 25 | 还原编译/部署链 | ✅ | §18 |
| 26 | 还原安装/OTA（含文件级影响） | ✅ | §19 |
| 27 | 盘点测试总量/真实性/盲区 | ✅ | §20 |
| 28 | 性能数据（**无则写未测量**） | ✅ 标未测量 | §21 |

**范围声明**：本次审计**只读**。未重构、未改 Bug、未优化、未删除/移动文件、未构建、未改板端配置、未启停服务。板端仅执行只读命令（`ls/find/du/cat/readlink/systemctl status/journalctl/ss/ps/v4l2-ctl --query/lscpu/free/lsblk/lsusb/ip`）＋经 `core.sock` 发只读 `GET_STATUS`。

---

## 附：git 状态对照

```text
[调查前]  git status --porcelain
?? "docs/资产盘点-重构前基线-2026-09-30.md"

[调查后]  git status --porcelain
?? "docs/资产盘点-重构前基线-2026-09-30.md"
?? docs/architecture/          ← 本报告，唯一新增
```

`HEAD` 调查前 = 调查后 = `6ec0fd9`。除本报告与上一轮盘点文档外，**未新增/修改/删除任何被跟踪文件**。
