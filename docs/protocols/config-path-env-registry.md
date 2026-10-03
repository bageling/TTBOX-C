# 配置 · 常量 · 路径 口径登记表（唯一真源）

> **归属**：软件团队·工程师（寇豆码），Task #2（整改方案见
> [`../handover/2026-09-17/配置常量路径口径-基线与整改方案-2026-09-17.md`](../handover/2026-09-17/配置常量路径口径-基线与整改方案-2026-09-17.md)）。
> **性质**：本文件是 **RUNTIME 环境变量的 allowlist** 与 **跨语言同值常量的真源清单**。
> 凡引擎（C++/Python/shell）在**进程启动后**读取的 `TTBOX_*` / `USB_PROXY_*`，**必须**在下表登记；
> 未登记即违规，由 [`scripts/ttbox_conventions_gate.sh`](../../scripts/ttbox_conventions_gate.sh) 断言。
> **口径基线（可判定规则）** 见 [`docs/CONVENTIONS.md`](../CONVENTIONS.md)「六、配置·常量·路径口径基线」。
> **本表登记的是"目标值"，不是"现状描述"** —— 现状与目标冲突时以本表为准，代码照本表整改。

---

## 一、作用域分类（D-ENV-1）

| 作用域 | 含义 | 生命周期 | 是否需登记 allowlist |
|---|---|---|---|
| **BUILD** | CMake / toolchain / 发布门禁在**编译期**消费 | 构建一次即消失 | 否（但不允许进运行期代码） |
| **RUNTIME** | 进程**启动后**读取，影响运行行为 | 进程存活期 | **是（本表 §二）** |
| **TEST** | 仅验收/自测脚本消费 | 仅测试进程 | 否（但须标 `TEST` 域） |

命名前缀：**RUNTIME 一律 `TTBOX_`**（历史遗留的 `USB_PROXY_*` 例外见 §四）。

---

## 二、RUNTIME 环境变量 allowlist（D-ENV-2）

> 覆盖语义列「覆盖谁」= 该变量覆盖的编译期/模块默认值。**真源**列 = 唯一权威默认值的定义点。

### 2.1 Core（C++，`core/src/**`）

| 变量名 | 类型 | 默认 | 真源（默认值定义点） | 覆盖语义 | 作用域 | 消费点 |
|---|---|---|---|---|---|---|
| `TTBOX_CONFIG` | 路径/目录 | `TTBOX_PROJECT_ROOT "/config/default.json"` | `Application.cpp::kDefaultConfigPath` | 覆盖 `--config` 之后的编译期默认（**仅本机开发兜底**） | RUNTIME | `Application::initialize` |
| `TTBOX_IPC_SOCKET` | 路径 | `/run/ttbox/core.sock` | `core/src/common/Paths.hpp::kIpcSocketDefault` | 覆盖 IPC socket 默认（CLI `--ipc` 优先） | RUNTIME | `Application::initialize`、`ipc_ping`、web/preview/tools |
| `TTBOX_MODELS_ROOT` | 路径 | `TTBOX_PROJECT_ROOT "/models"` | `ModelRegistry` 构造兜底 | 覆盖配置 `model_registry_root` | RUNTIME | `ModelRegistry`、`Application`、`ttbox-model.py`、web、`scripts/a9_ai_isolation.sh`（默认 `/opt/ttbox/models` = FHS 化前旧布局落点） |
| `TTBOX_HID_ROOT` | 路径 | `TTBOX_PROJECT_ROOT "/hid"` | `HidPackageRegistry` / `HidRuntime` 兜底 | 覆盖 HID 包根 | RUNTIME | `HidPackageRegistry.cpp`、`HidRuntime.cpp` |
| `TTBOX_LICENSE_SERVER` | URL | 内置默认（云端授权基址） | `TtboxLicenseClient.hpp` | 覆盖授权服务基址 | RUNTIME | `TtboxLicenseClient.hpp` |
| `TTBOX_APP_KEY` | 字符串 | 空 | `TtboxLicenseClient.hpp` | 云端授权 app_key（**TEST/运维注入**，非产品默认） | RUNTIME | `TtboxLicenseClient.hpp` |
| `TTBOX_CLIENT_SECRET` | 字符串 | 空 | `TtboxLicenseClient.hpp` | 云端授权 client_secret（同上） | RUNTIME | `TtboxLicenseClient.hpp` |
| `TTBOX_CORE_MLOCK` | 开关 | `1`（`0`=关） | `core/src/common/RtSched.cpp`（`mlockall`，采集/推理路径防换页） | 覆盖进程级锁页开关 | RUNTIME | `Application::initialize` |
| `TTBOX_CORE_CAPTURE_RT_PRIORITY` | SCHED_FIFO 优先级 | `60`（`0`=不启用，上限 `70`） | `core/src/capture/V4L2Capture.cpp`（**在采集线程内部**调用 `RtSched::apply_fifo`） | 覆盖采集线程实时优先级；失败降级为普通调度 | RUNTIME | `V4L2Capture::capture_loop` |
| `TTBOX_CORE_CAPTURE_CPU` | CPU 编号 | `-1`（不绑，沿用大核掩码 CPU4~7） | `RtSched::apply_fifo` | 覆盖采集线程单核绑定 | RUNTIME | `V4L2Capture::capture_loop` |
| `TTBOX_IPC_DEBUG` | 开关 | `0`（`1`=开；空串亦视为关） | `core/src/ipc/IpcServer.cpp::ipc_debug_enabled`（`IPCDBG` 宏） | 打开 IPC **服务端**逐事件日志（监听 / accept / 读到多少字节 / 是否回包 / recv 错误码），排查「客户端连接成功却读不到响应」；生产默认关闭、零开销 | RUNTIME | `IpcServer.cpp` 内 `IPCDBG` 各调用点 |
| `TTBOX_LOG_DIR` | 路径 | `/var/log/ttbox` | `core/src/common/Paths.hpp::kLogDirDefault` | 覆盖日志目录（批次 1.4）。core 的 `Logger`/`FileSink` 在此写 `ttbox.log`（全量流水，WARN/INFO 批量落盘）/ `ttbox-error.log`（只 FATAL/ERROR，内容永不过滤）/ `operation.log`（客户操作，同步）；web 侧写 `web.log`。目录由 `scripts/ttbox_fhs_init.sh` 创建（ttbox:ttbox 0755）；不可写时 sink 自降级为不可用，**不影响启动** | RUNTIME | `Application::initialize` |
| `TTBOX_LOG_LEVEL` | 字符串 | 空（=INFO） | `plugins/web/lib/logging_setup.py::resolve_level` | 覆盖 web 侧日志级别（批次 1.4）。取值 **`debug`/`info`/`warn`/`error`/`fatal`/`off`** —— 与 core 的 `--log-level` **同一套取值**（§5.1「级别必须可在配置里调，不用重编译」）。web 侧无 CLI，故走环境变量（systemd unit 可写 `Environment=`） | RUNTIME | `logging_setup.setup_logging` |

> **已删除（不得复活）**：`TTBOX_CONFIG_PATH`（同义异名，V-05），`TTBOX_MODEL_ROOT`（同义异名，V-04）。
> 二者**不保留兼容读**（D-ENV-3 / D-ENV-5）。

### 2.2 Web 插件（Python，`plugins/web/**`）

| 变量名 | 类型 | 默认 | 真源 | 覆盖语义 | 作用域 |
|---|---|---|---|---|---|
| `TTBOX_IPC_SOCKET` | 路径 | `/run/ttbox/core.sock` | `plugins/web/lib/paths.py::IPC_SOCKET_DEFAULT` | 覆盖 IPC socket | RUNTIME |
| `TTBOX_ROOT` | 路径 | `<树根>/plugins/web`（根锚派生） | `ttbox-web.py` | 覆盖 web 静态根 | RUNTIME |
| `TTBOX_PREFIX` | 路径 | `/opt/ttbox` | `plugins/web/lib/paths.py::ttbox_prefix` | 覆盖运行根前缀（派生 scripts/presets/config 子路径） | RUNTIME |
| `TTBOX_SCRIPTS_DIR` | 路径 | `<TTBOX_PREFIX>/scripts` | `plugins/web/lib/paths.py::scripts_dir` | 覆盖 scripts 目录（`wifi_manager` / `edid` 工具链所在） | RUNTIME |
| `TTBOX_PRESETS_DIR` | 路径 | `<TTBOX_PREFIX>/presets` | `plugins/web/lib/paths.py::presets_dir` | 覆盖预设目录 | RUNTIME |
| `TTBOX_HDMIRX_EDID` | 路径 | `<TTBOX_SCRIPTS_DIR>/edid/hdmirx_edid.py` | `plugins/web/lib/paths.py::hdmirx_edid_tool` | 覆盖 EDID 工具路径 | RUNTIME |
| `TTBOX_MOTION_PROFILES_DIR` | 路径 | `<TTBOX_PREFIX>/config/motion-profiles` | `plugins/web/lib/paths.py::motion_profiles_dir` | 覆盖动作曲线目录 | RUNTIME |
| `TTBOX_PLUGINS_ROOT` | 路径 | `<树根>/plugins` | `plugins/web/lib/paths.py::plugins_dir`（消费方 `framework_api.py`） | 覆盖插件根 | RUNTIME |
| `TTBOX_PLUGIN_REPOSITORY_ROOT` | 路径 | `<TTBOX_PLUGINS_ROOT>/repository` | `framework_api.py` | 覆盖插件仓库根 | RUNTIME |
| `TTBOX_ENABLE_DESIGNER` | 开关 | 未设=关（fail-closed） | `api_v1.py` | 置 `1` 才注册设计器接口（安全闸门） | RUNTIME |
| `TTBOX_DISPLAY_CONFIG` | 路径 | `/opt/ttbox/config/hardware_display.json` | `scripts/edid/edid_apply.sh` | 覆盖显示配置路径 | RUNTIME |
| `TTBOX_WIFI_*` | 见下 | 见下 | `scripts/wifi_manager.py` | 配网引导参数 | RUNTIME |
| `TTBOX_IPC_TCP` | 地址 | 空（用 Unix socket） | `ttbox-web.py` / `api_v1.py` | 覆盖 IPC 传输为 `host:port`（Windows 本机联调） | RUNTIME |
| `TTBOX_CONFIG_DIR` | 路径 | `<TTBOX_PREFIX>/config` | `plugins/web/lib/paths.py::config_dir` | 覆盖 web 配置目录（凭据/hardware_display 派生） | RUNTIME |
| `TTBOX_WEB_CREDENTIALS` | 路径 | `<TTBOX_CONFIG_DIR>/default.json` | `plugins/web/lib/paths.py::web_credentials_file` | 覆盖 web 云端凭据文件（**web 自有，非运行期配置真源**） | RUNTIME |
| `TTBOX_UI_CUSTOM_CSS` | 路径 | `/opt/ttbox/config/ui-custom.css` | `api_v1.py` | 覆盖自定义 UI CSS 路径 | RUNTIME |
| `TTBOX_CONVERT_WORKDIR` | 路径 | `/tmp/ttbox_onnx_convert` | `ttbox-web.py` | ONNX→RKNN 转换临时目录 | RUNTIME |
| `TTBOX_CONVERTER_SCRIPT` | 路径 | 转换脚本默认路径 | `ttbox-web.py` | 覆盖模型转换脚本 | RUNTIME |
| `TTBOX_CONVERTER_PYTHON` | 路径 | 默认 python3 | `ttbox-web.py` | 覆盖转换用解释器 | RUNTIME |
| `TTBOX_CONVERT_CALIB_DIR` | 路径 | 转换标定目录默认 | `ttbox-web.py` | 覆盖标定数据目录 | RUNTIME |
| `TTBOX_ALLOW_ONNX` | 开关 | 未设=关（fail-closed） | `ttbox-web.py` | 置 `1` 才允许上传 ONNX（安全闸门） | RUNTIME |
| `TTBOX_CLOUD_SESSION` | 路径 | 本地会话文件默认 | `plugins/web/lib/cloud_session.py` | 覆盖云激活会话文件路径 | RUNTIME |

> **已删除（不得复活）**：`TTBOX_WEB_HOST` / `TTBOX_WEB_PORT`（V-20：装饰性 env，代码里端口已定死，
> 「改它们不会生效」的注释属误导性补丁）。端口真源 = `ttbox-web.py::LISTEN_PORT`（V-06）。
> 同义异名 `TTBOX_DEFAULT_WEB_PORT` / `TTBOX_PORT` 亦**删除**（D-ENV-3）。

### 2.3 Preview 插件 / usbproxy（RUNTIME）

| 变量名 | 类型 | 默认 | 真源 | 作用域 |
|---|---|---|---|---|
| `TTBOX_IPC_SOCKET` | 路径 | `/run/ttbox/core.sock` | 同上（跨语言同值） | RUNTIME |
| `TTBOX_PREVIEW_HOST` | 地址 | `127.0.0.1` | `ttbox-preview.service` | RUNTIME |
| `TTBOX_PREVIEW_PORT` | 端口 | `8001` | `ttbox-preview.service` | RUNTIME |
| `TTBOX_PREVIEW_URL` | URL | `http://127.0.0.1:8001` | `ttbox-web.service` | RUNTIME |
| `USB_PROXY_DEVICE` | 设备名 | 空 = 自动取 `/sys/class/udc` 排序首个（P8 起不再写死硬件编号）；板端由 `ttbox-usbproxy.service` 显式钉为 `fc000000.usb` | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_DRIVER` | 驱动名 | `dwc3-gadget` | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_SOCKET_DIR` | 路径 | `/run/ttbox-mouse-passthrough` | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_WAIT_SECONDS` | 秒 | `1` | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_BIN` | 路径 | `<usbproxy 目录>/usb-proxy` | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_EXTRA_ARGS` | 字符串 | 空 | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_LIBDIR` | 路径 | `<usbproxy 目录>/lib` | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_UDC_WAIT_SECONDS` | 秒 | `60` | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_MOUSE_WAIT_SECONDS` | 秒 | `0`=无限等待（1.5.62 起；合成模式已删除，找不到物理鼠标就一直等） | `run-ttbox-usb-proxy.sh` | RUNTIME |
| `USB_PROXY_MLOCK` | 开关 | `1`（`0`=关） | `usbproxy/misc.cpp`（`mlockall`，转发路径防换页） | RUNTIME |
| `USB_PROXY_ENDPOINT_RT_PRIORITY` | SCHED_FIFO 优先级 | `60`（`0`=不启用） | `usbproxy/proxy.cpp` 端点读/写线程 | RUNTIME |
| `USB_PROXY_EP0_RT_PRIORITY` | SCHED_FIFO 优先级 | `65`（`0`=不启用） | `usbproxy/proxy.cpp` `ep0_loop` | RUNTIME |
| `USB_PROXY_THREAD_RT_PRIORITY` | SCHED_FIFO 优先级 | `55`（`0`=不启用） | `usbproxy/device-libusb.cpp` 事件泵 | RUNTIME |
| `USB_PROXY_MOUSE_CONTROL_RT_PRIORITY` | SCHED_FIFO 优先级 | `98`（`0`=不启用） | `usbproxy/mouse_control.cpp` | RUNTIME |
| `USB_PROXY_CPU_AFFINITY` | CPU 编号 | `-1`（不绑） | `usbproxy/proxy.cpp` / `device-libusb.cpp` | RUNTIME |
| `USB_PROXY_MOUSE_CONTROL_CPU_AFFINITY` | CPU 编号 | `<核数-1>` | `usbproxy/mouse_control.cpp` | RUNTIME |

> **§6-6 裁定**：`USB_PROXY_*` 归属**脚本 `usbproxy/board/run-ttbox-usb-proxy.sh`**（RUNTIME），
> 由 `deploy/systemd/ttbox-usbproxy.service` 的 `Environment=` 提供；前缀沿用既有 `USB_PROXY_`
> （不改为 `TTBOX_`）以避免破坏 USB gadget 既有运维习惯与外部脚本依赖。

### 2.4 运维脚本（RUNTIME，shell）

| 变量名 | 类型 | 默认 | 真源 | 作用域 |
|---|---|---|---|---|
| `TTBOX_PREFIX` | 路径 | `/opt/ttbox` | 各脚本头部 `TTBOX_PREFIX="${TTBOX_PREFIX:-/opt/ttbox}"` | RUNTIME |
| `TTBOX_RUN` | 路径 | `/run/ttbox` | 同 | RUNTIME |
| `TTBOX_KEEP_VERSIONS` | 整数 | 部署脚本内置 | `ttbox_release_install.sh` | RUNTIME |
| `TTBOX_EDID_REHANDSHAKE` | 开关 | `1` | `scripts/edid/edid_apply.sh` | RUNTIME |
| `TTBOX_EDID_REHANDSHAKE_ATTEMPTS` | 整数 | **2**（单一真源，V-09；★ 2026-09-28 由 12 下调——每轮切一次 HPD = 源端黑屏一次，12 轮最坏 12 次黑屏） | `scripts/edid/edid_apply.sh` | RUNTIME |
| `TTBOX_EDID_HPD_SETTLE_SEC` | 秒 | `0.5` | `scripts/edid/edid_apply.sh` | RUNTIME |
| `TTBOX_EDID_LOCK_TIMEOUT_SEC` | 秒 | `10`（★ 2026-09-28 由 14 下调；板端实测锁定需 7~8s） | `scripts/edid/edid_apply.sh` | RUNTIME |
| `TTBOX_CURRENT` | 路径 | `/opt/ttbox/current` | `scripts/ttbox.sh` | 覆盖 current 软链根（运维入口/doctor 定位 scripts 与 web） | RUNTIME |
| `TTBOX_STATE` | 路径 | `/opt/ttbox/state` | `scripts/ttbox.sh` + `core/src/common/Paths.hpp::kStateDirDefault` | 覆盖状态目录（version 留档、OTA 状态、`runtime_intent.json` 用户启停意愿等） | RUNTIME |

> **2026-09-23 补登记（T1.49）**：下方 4 个是**出厂镜像烘焙链**（`image/**`）的注入参数。
> 该目录此前整目录在门禁 `SKIP_TOP` 豁免里（0 跟踪），补入库后首次纳管才暴露 ——
> 属**补登记**，不是新引入。其中两个为凭据类，纪律与 §2.5 的 TEST 域一致：明文只留
> 项目外目录，不入镜像、不入文档；`TTBOX_ROOT_PASS` 自 V5 起**禁止回落固定弱口令**
> （未提供即构建中止），判据见 `image/steps/04_board_config.sh` 开头注释。

| 变量名 | 类型 | 默认 | 真源 | 作用 | 作用域 |
|---|---|---|---|---|---|
| `TTBOX_STAGE_DIR` | 路径 | 无（必填） | `image/prepare_stage.sh`、`run_in_img.sh`、`90_finalize_host.sh`、`image/v5_bake.sh` | 覆盖烘焙 stage 暂存目录（灌装树落点） | RUNTIME |
| `TTBOX_VER` | 版本号 | 无（必填） | `image/prepare_stage.sh` | 覆盖烘焙出的镜像版本号 | RUNTIME |
| `TTBOX_HOSTKEY_DIR` | 路径 | 无（必填） | `image/v5_bake.sh`、`steps/04_board_config.sh`、`90_finalize_host.sh` | 注入镜像内 ssh host key 的来源目录（**凭据类**） | RUNTIME |
| `TTBOX_ROOT_PASS` | 口令 | 无（必填，**禁回落到固定弱口令**） | `image/steps/04_board_config.sh` | 烘焙时设置镜像 root 口令（**凭据类**） | RUNTIME |
| `TTBOX_EXPECT_VER` | 版本号 | 空（不钉版本） | `image/90_finalize_host.sh`、`image/v5_bake.sh` | 收尾门禁可选开关：钉死「镜像内浇筑版本必须等于此值」（不传则只验 current 指向真 release） | RUNTIME |

### 2.5 显式登记为 TEST 域（不进 RUNTIME allowlist）

| 变量/常量 | 位置 | 域 | 说明 |
|---|---|---|---|
| `TTBOX_M2_CARDS` | `scripts/ttbox_m207_*` | TEST | 验收脚本夹具路径 |
| `TTBOX_CLIENT_SECRET` / `TTBOX_APP_KEY` / `TTBOX_LICENSE_SERVER` | `scripts/ttbox_m207_*`、`scripts/legacy/*` | TEST | 验收脚本注入的云端凭据（**非产品端口/路径**） |
| `CLOUD_PORT = 10015` | `scripts/ttbox_m207_b21_expire.py:35` | TEST | **云端回调端口常量**（验收脚本夹具），非产品端口，**登记即可、不改值** |
| `TTBOX_RESTART_UNITS` / `TTBOX_HEALTH_TIMEOUT` / `TTBOX_RELEASE_SELFTEST` / `TTBOX_RELEASE_VERIFY_REPRO` / `TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD` / `TTBOX_REAL_CORE_MAIN` | `scripts/*_selftest.sh` / `*_verify.sh` | TEST | 自测/体检脚本开关 |
| `TTBOX_WEB` | `scripts/ttbox_m207_accept.py`、`scripts/ttbox_m2xx_console_accept.py` 的 `--base-url` 默认值 | TEST | 验收脚本覆盖 web 基址（默认 http://127.0.0.1:8000）；非产品变量，不进 RUNTIME allowlist |
| `TTBOX_OTA_PRIV_PASSWORD` | `tools/ota/fake_ota_server.py` | TEST | 本地联调夹具注入签名私钥口令（正式签名口令只走本机 `C:\ttbox-ota-keys\PASSPHRASE.txt` / 运维手工输入，永不进环境或代码） |
| `TTBOX_DTB_SRC` / `TTBOX_DTB_FIX_TEST` | `scripts/ttbox_dtb_fix.sh` | TEST | DTB 修复脚本的测试钩子：前者覆盖源 DTB 路径（默认真源 `deploy/dtb/…`）、后者放行非 root 并跳过重启安排（仅离线夹具用）。正式运行两值均不设 ⇒ 走真源 + 真实重启；已同步登记进门禁 `ENV_ALLOW` |
| `TTBOX_DTB_GOOD_SHA` / `TTBOX_DTB_BAD_SHA` / `TTBOX_DTB_REPORT` | `scripts/ttbox_dtb_fix.sh` | TEST | 2026-09-22 从板端回流修复脚本时新增：前两者覆盖「修复后/损坏后 DTB 期望哈希」（默认值即真源 `277d9de8…` / `7b8cc892…`），后者覆盖报告落点（默认 `/opt/ttbox/presets/_dtbfix.json`）。生产不设、走默认；已同步登记进门禁 `ENV_ALLOW` |
| `TTBOX_USB_MODE_TEST` | `scripts/ttbox_usb_mode.sh` | TEST | 2026-09-22 新增 USB 透传模式运维入口时登记：放行非 root 写 drop-in（`TTBOX_SYSTEMD=0` 一并跳过 daemon-reload/restart），仅供离线夹具。正式运行不设 ⇒ 强制 root + 真实重启；已同步登记进门禁 `ENV_ALLOW` |
| `TTBOX_HW_TEST_DIR` | `scripts/a9_ai_isolation.sh` | TEST | 2026-10-01 登记（P8）：板端**手动** HW 并发隔离诊断所需的测试二进制目录（须含 `test_worker_hw` / `test_hid_load_sim`，由板端 `cmake -DTTBOX_CORE_BUILD_HW_TESTS=ON` 构建产出）。**刻意无默认值**：板端发布树 `/opt/ttbox` 只有 `releases/V1.0.xx` + `current`，不落构建产物，猜任何路径都错 ⇒ 不指定即报错讲清要什么。★ 不复用 `TTBOX_BUILD_DIR`（后者是 §3.3 BUILD 域 = 交叉编译产物目录，借用即同名异义）。生产不设、该脚本不参与自动链路 ⇒ 零影响；已同步登记进门禁 `ENV_ALLOW` |
| `TTBOX_PYTHON` | `scripts/ttbox_phase1_gate.sh` | TEST | 2026-10-01 登记（P8 复查）：显式指定门禁脚本用的 Python 解释器。**本机解释器位置属本机事实**，不该硬编码进版本化脚本（原实现写死 `C:/Users/Administrator/...` 当兜底，换台机器即废，且违反口径门禁⑪）。不设则按 `python3` → `python` → `$HOME/.workbuddy/binaries/python/envs/default/Scripts/python.exe` 顺序做**能力探测** —— G6 要的是"能 `import pytest`"的那个，不是单纯"存在"的那个。已同步登记进门禁 `ENV_ALLOW` |

> **§6-5 裁定**：`10015` 属 **TEST** 域（验收脚本常量），登记后**不改值**。

---

## 三、跨语言同值常量（B-CONST-2）

> 无法 `#include` 打通时（C++ / Python / shell 三类引擎），**唯一承诺** = 本表登记 + 门禁同值断言。
> 下表任一取值变更**必须**同步改本节 + 门禁脚本，否则 `ttbox_conventions_gate.sh` FAIL。
>
> **另（V-13）**：Core IPC **客户端**的唯一实现 = `plugins/web/lib/ipc.py::request`；
> `ttbox-web.py::ipc_request` 与 `framework_api.py::_ipc_request` 均为其**薄封装**（不得再各写一份）。

| 常量 | 值 | C++ 定义（唯一真源） | Python 镜像 | shell 镜像 | 门禁断言 |
|---|---|---|---|---|---|
| **IPC socket** | `/run/ttbox/core.sock` | `core/src/common/Paths.hpp::kIpcSocketDefault` | `plugins/web/lib/paths.py::IPC_SOCKET_DEFAULT` | `ttbox/core.service` 的 `TTBOX_IPC_SOCKET` | 三处逐字符相等 |
| **mouse cmd sock** | `/run/ttbox-mouse-passthrough/cmd.sock` | `Paths.hpp::kMouseCmdSocketDefault` | `paths.py::MOUSE_CMD_SOCK_DEFAULT` | `usb-proxy.cpp::mouse_cmd_socket` | 逐字符相等 |
| **mouse event sock** | `/run/ttbox-mouse-passthrough/event.sock` | `Paths.hpp::kMouseEventSocketDefault` | `paths.py::MOUSE_EVENT_SOCK_DEFAULT` | `usb-proxy.cpp::mouse_event_socket` | 逐字符相等 |
| **license 文件** | `/etc/ttbox/license.key` | `Paths.hpp::kSystemLicenseFile` | — | — | — |
| **心跳间隔** | `60` 秒 | `auth/LicenseConstants.hpp::kHeartbeatIntervalSecDefault` | — | — | 各处引用同一常量（Daemon/Gate/StateMachine/Client） |
| **心跳超时** | `180` 秒 | `auth/LicenseConstants.hpp::kHeartbeatTimeoutSecDefault` | — | — | 各处引用同一常量 |
| **Web 监听端口** | `8000` | — | `paths.py::WEB_PORT_DEFAULT`（`ttbox-web.py::LISTEN_PORT` 派生）、`wifi_manager.py::WEB_PORT_DEFAULT` | `ttbox_release_install.sh::WEB_PORT` | 四处同值（门禁断言） |
| **预览端口** | `8001` | — | `ttbox-preview.service` env | — | — |
| **EDID 重试次数** | `12` | — | — | `edid_apply.sh::ATTEMPTS` | 登记一致（V-09） |

### 3.1 版本号三名（B-CONST-3 / V-10）

| 语义 | 名 | 真源 | 可见性 |
|---|---|---|---|
| **Core 内部版本** | `kCoreVersion` | `core/include/ttbox/core/version.hpp` | 仅日志/诊断 |
| **面板对外版本** | `kAppVersion` | `plugins/web/bin/ttbox-web.py` | 面板 `/api/license`、云激活 `client_version` |
| **出货留档版本** | `TTBOX_RELEASE_VERSION` | `scripts/ttbox_fhs_init.sh` / 发布树 | 部署/回滚锚 |

> 三者是**三个不同事实**，**禁止**共用同一字符串、**禁止**多处硬编码。
> 门禁断言 `version.hpp::kCoreVersion == core/CMakeLists.txt project VERSION`（同一事实的 CMake 镜像）。

### 3.3 BUILD 域常量（编译期消费，不得进运行期业务路径）

`scripts/ttbox_build_release.sh` / `ttbox_fhs_init.sh` 消费、**仅供构建/安装**：`TTBOX_PROJECT_ROOT`、
`TTBOX_BUILD_DIR`、`TTBOX_USBPROXY_INCLUDE`、`TTBOX_USBPROXY_LIBDIR`、`TTBOX_RKNNRT_SO`、`TTBOX_GIT_SHA`、
`TTBOX_RELEASE_VERIFY_REPRO`、`TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD`。门禁按**名 allowlist**放行，
但不允许在 `core/src`、`plugins/**`（运行期）里读取。

---

## 四、路径口径基线（A-PATH，摘要）

| 规则 | 说明 |
|---|---|
| **A-PATH-1** | C++ 运行根唯一来源 = 编译期 `-DTTBOX_PROJECT_ROOT`；派生资源写 `std::string(TTBOX_PROJECT_ROOT) + "/<sub>"`。禁止在 C++ 另写 `/opt/ttbox` 当运行根。 |
| **A-PATH-2** | 运行期可变路径取「CLI > env > 编译期/头文件默认」；默认值只来自单一头（`Paths.hpp`）。 |
| **A-PATH-3** | Python 自身位置用 `Path(__file__).resolve().parents[N]`；跨包注入只在插件启动头部一次性 `append`（**不用 `insert(0)`**，防遮蔽 stdlib）；业务路径不得二次改 `sys.path`。 |
| **A-PATH-4** | shell 根前缀参数化：`TTBOX_PREFIX` / `TTBOX_ETC` / `TTBOX_RUN` / `TTBOX_REPO`（仓库根由 `dirname $BASH_SOURCE/..` 反推）。 |
| **A-PATH-5** | 同一路径字面量全仓单点定义（`Paths.hpp` / `paths.py` / 脚本头部）；跨语言打不通 include 时以本节 §三 + 门禁保证同值。 |
| **A-PATH-6** | 禁止注释/文档声称某路径"仍生效"而代码已删（效仿 `ttbox-core.service` 对已删 `LD_LIBRARY_PATH` 的【已删除】正例）；禁止构建机绝对路径进产物。 |

---

## 五、配置口径基线（C-CFG，摘要）

| 规则 | 说明 |
|---|---|
| **C-CFG-1** | 运行期配置真源 = 板端 `/etc/ttbox/config.d/`（`00-factory.json` 只读基线 ← `10-device.json` 客户层，按文件名升序深合并）。写回目标唯一 = `config.d/10-device.json`。 |
| **C-CFG-2** | 读取优先级链：`--config <path>` > `TTBOX_CONFIG` > 编译期默认（`kDefaultConfigPath`，**仅本机开发兜底**）。生效路径必须在服务启动时显式给出。 |
| **C-CFG-3** | 运行时配置**单一写入者 = Core**（`SET_CONFIG` IPC → `ConfigManager::persist`）。**Web/任何 Python 侧禁止直接读写配置文件**；读配置一律走 Core IPC（`GET_CONFIG`）。 |
| **C-CFG-4** | 默认值单一真源 = `deploy/config/00-factory.json`。C++ `ConfigManager::get_*(key, def)` 的 `def` 不得成为第二真源。 |
| **C-CFG-5** | `config/default.json` 仅定位为"本机开发样例"，其**共享键**必须与 `deploy/config/00-factory.json` **同值**（门禁断言防漂移）；出厂基线唯一 = `deploy/config/`。 |

---

## 六、门禁自检（一条命令判定）

```bash
bash scripts/ttbox_conventions_gate.sh              # 门禁（PASS ⇒ 退出码 0）
bash scripts/ttbox_conventions_gate.sh --selftest   # 门禁 + 负向控制（证明检测器真能捕获篡改）
# 断言：① 无散落路径字面量（仅 Paths.hpp / paths.py / usb-proxy.cpp / systemd / deploy config 可出现）
#       ② 无未登记 RUNTIME/BUILD env
#       ③ 无同义异名 env（TTBOX_CONFIG_PATH / TTBOX_MODEL_ROOT / TTBOX_DEFAULT_WEB_PORT / TTBOX_PORT / TTBOX_WEB_HOST）
#       ④ V-03 共享键同值（config/default.json 与 deploy/config/default.json.prod ↔ 00-factory.json）
#       ⑤ 跨语言同值常量（socket×3 / 端口 8000 / EDID attempts 2 / 心跳 60·180）
#       ⑥ 版本同值（version.hpp::kCoreVersion **去掉字母前缀** == CMakeLists project VERSION；
#          产品版本走 V 线时为 "V1.0.01"，CMake 只解析数字 ⇒ 那边写数字镜像 "1.0.01"）
#       ⑦ 无补丁残迹（hardware_display.json 单点；systemd_units.py / runner.py 已删）
#       ⑧ 出货 Python 无绝对路径 sys.path.insert
#       ⑨ 无 TTBOX_PROJECT_ROOT 兜底 #define（强制由 CMake -D 注入）
# 退出码 0 = PASS
```

体检入口已接入 [`scripts/ttbox_release_verify.sh`](../../scripts/ttbox_release_verify.sh)（源码树在场时执行；`TTBOX_SKIP_CONVENTIONS_GATE=1` 可跳过）。
