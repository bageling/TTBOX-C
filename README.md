# TTBOX 模块版

一台用 **AI 看画面、自动动鼠标** 的边缘计算盒子。硬件是 OrangePi 5 Plus（瑞芯微 RK3588，带 3 核 NPU）。

```text
电脑画面 ──HDMI 线──▶ TTBOX 盒子（RK3588）
                          │
                          ▼
                  AI 识别画面里的目标
                          │
                          ▼
                  USB 线模拟真实鼠标移动
```

简单说：**盒子只用 HDMI 线看电脑屏幕，用 AI 找到目标，再用 USB 线把鼠标指令送进电脑。**
它不读内存、不改游戏数据，只处理外部画面，接入电脑时不需要安装任何软件。

当前出货版本 **`V1.0.43`**。版本号的写法有讲究（不能改成纯数字），原因见[第八节](#八版本与交付)。

---

## 一、快速上手（5 分钟）

### 1. 接线

| 线 | 方向 | 作用 |
|---|---|---|
| HDMI 输入线 | 电脑 → 盒子 | 盒子的眼睛，看电脑画面 |
| USB 输出线 | 盒子 → 电脑 | 盒子的手，模拟鼠标移动 |

### 2. 通电并打开网页

盒子通电等约 30 秒，在电脑浏览器打开 `http://<盒子IP>:8000`（例如 `http://192.168.0.53:8000`）。

### 3. 检查画面是否进来

网页「总览」页：采集 FPS > 0 说明 HDMI 画面进来了；检测数量 > 0 说明 AI 认出了目标；预览有画面说明盒子在推流。

### 4. 激活模型

到「模型库」页选一个模型点激活。

### 5. 打开注入并按住热键测试

到「鼠标控制」页：打开「鼠标注入」开关（`mouse.enabled=true`），设置热键（默认左/右键）。**按住热键 AI 才注入鼠标，松开立即停止**，不是开机一直控制。

---

## 二、仓库目录结构

```text
├── core/               AI 核心：C++ 源码 + 单元测试（唯一构建源树）
│   ├── include/        跨模块接口头
│   ├── src/            生产源码（采集/推理/瞄准/输出）
│   ├── tests/          C++ 单测 + 真机调试脚本
│   ├── tools/          本机/板端调试工具（IPC 探针、HID 工具）
│   ├── third_party/    第三方内嵌源码
│   └── CMakeLists.txt  构建脚本
├── plugins/            网页控制台(8000) / 预览(8001) 等插件
├── framework/          Python 插件管理框架
├── ttbox_motion/       运动校准 / 训练
├── usbproxy/           自研鼠标注入代理源码（含预编译 ELF）
├── ttbox_platform/     平台服务层（supervisor/health/model/runtime）
├── image/              出厂整机镜像烘焙链（不进包）
├── tools/              离线工具（模型转换 / 许可 / OTA 签发）
├── scripts/            构建 / 发布 / 运维脚本 + edid 工具链
├── config/             开发侧配置模板
├── deploy/             systemd 单元 / 出厂配置 / 依赖说明
├── docs/               文档中心（见 docs/README.md）
├── tests/              板端集成 / 监控 / API 验收脚本
├── preview_server.py   预览服务的独立入口（调试用）
└── pyproject.toml      Python 侧工程配置（pytest 口径）
```

各顶层目录职责与「是否进 payload（release 树）」：

| 目录 | 中文功能 | 是否进 payload |
|---|---|---|
| `core/` | 核心引擎（C++ 唯一构建源树） | 仅 `bin/ttbox_core_main` |
| `plugins/` | 网页控制台 / 预览 / model / fan / wifi / network / monitor / log / system / upgrade | ✅ 整包 |
| `framework/` | 插件发现 / 安装 / 生命周期 / 权限 | ✅ 整包 |
| `ttbox_motion/` | 运动校准 / 训练 | ✅ 整包 |
| `usbproxy/` | Raw Gadget 鼠标注入代理（含预编译 ELF） | ✅ 整包 |
| `ttbox_platform/` | 平台服务层 supervisor / health / model / runtime | ✅ 整包 |
| `scripts/` | 构建/发布/运维 + edid 工具链 | 白名单点名 |
| `deploy/` | systemd 单元 / 出厂配置 / OTA 公钥 / DTB | 白名单点名 |
| `config/` | 开发侧模板 | ❌ |
| `image/` / `tools/` / `tests/` / `docs/` | 镜像烘焙 / 离线工具 / 集成测试 / 文档 | ❌ |

> 「是否进 payload」的唯一真源是 `scripts/ttbox_fhs_init.sh` 的 `sync_tree` 白名单闭集，本表只是说明。

`core/src/` 各目录：`capture/`（V4L2 采集）、`rga/`（硬件缩放）、`rknn/`（NPU 推理/解码）、`model/`（模型库）、`mouse/`（选目标/坐标换算）、`aim/`（瞄准线程 + PID）、`output/`（输出后端连 usbproxy）、`ipc/`（进程通信）、`preview/`（预览图）、`input/`（物理鼠标按键）、`app/`/`runtime/`（装配与运行时）、`auth/`（授权）、`common/`/`config/`（公共工具配置）。

---

## 三、板端运行环境

| 组件 | 说明 |
|---|---|
| 硬件 | OrangePi 5 Plus，RK3588，3 核 NPU |
| 系统 | Armbian / Ubuntu Linux（aarch64） |
| 视频输入 | HDMI RX，板端 `/dev/video0` |
| AI 核心 | C++ 程序 `ttbox_core_main` |
| 模型 | `.rknn` 格式，YOLO 系列 |
| 网页 / 预览 | Python 服务，8000 / 8001 端口 |
| 鼠标注入 | 自研 `usbproxy` |

板端服务：`ttbox-core`（AI 核心）、`ttbox-web`（8000）、`ttbox-preview`（8001）、`ttbox-usbproxy`（鼠标注入）、`ttbox-edid`（HDMI 身份，默认关闭）。

板端安装路径（`current` 是版本化运行树的**原子切换点**，OTA 与回滚对整棵树生效）：

```text
/opt/ttbox/
├── current -> releases/<版本>   当前生效运行树（禁止直写）
│   ├── bin/ttbox_core_main      AI 核心主程序
│   ├── lib/librknnrt.so         NPU 运行库（随包）
│   ├── plugins/  framework/  ttbox_motion/  ttbox_platform/
│   ├── usbproxy/usb-proxy       鼠标注入代理
│   ├── scripts/  deploy/        运维脚本 + systemd/出厂配置
├── releases/<版本>/            各版本运行树（历史留档，可回滚）
└── models/                      已安装模型
```

> 板端**不编译源码**。换 core 二进制走正规发布链（`ttbox_build_release.sh` → `ttbox_pack_ota.sh` → OTA），不要手工往 `/opt/ttbox/current/` 拷。

---

## 四、完整数据链路

```text
HDMI 画面 → V4L2Capture（采集，DMA-BUF）→ RgaProcessor（RGA 缩放）
  → RKNNEngine（NPU 推理）→ DecodeNMS（输出变检测框）→ TargetSelector（选目标）
  → Pid1Controller（算位移）→ AimThread（热键门控）→ MouseControlClient
  → usbproxy（Raw Gadget）→ Windows 鼠标真实移动
```

预览是另一条独立小路：`采集帧 → OpenCV 画框 → JPEG → 8001 → 网页`。
`PreviewModule` 同时依赖 libjpeg 与 OpenCV，两者齐才编译真实现，缺一则自动改链空实现（预览禁用、不得出货）。详见 `deploy/DEPENDENCIES.md` §六·1。

---

## 五、配置怎么改

板端运行参数**真源**是 `/etc/ttbox/config.d/`：

```text
/etc/ttbox/config.d/00-factory.json   出厂基线（只读）
/etc/ttbox/config.d/10-device.json    客户层（升序深合并，唯一写回目标）
```

改配置推荐走网页 `PUT /api/config`（深合并，只改传的字段）。运行期配置的**单一写入者是 Core**（`SET_CONFIG` IPC），Web 读配置走 `GET_CONFIG`。

> 别改错地方：`/opt/ttbox/config/` 是随包携带的其它配置（非写回目标）；仓库 `config/default.json` 只是本机开发样例；出厂基线唯一 = `deploy/config/`。

路径 / 常量 / 配置键 / 环境变量唯一真源见 `docs/protocols/config-path-env-registry.md`，由 `bash scripts/ttbox_conventions_gate.sh` 断言（退出码 0 = PASS）。

---

## 六、开发与构建

**强制流程（不可裁剪）**：本机开发 → 本机测试 → 修 Bug → 本机全 PASS → 交叉编译 → 打包 → 上板 → 真机验证。本机未全绿不许交叉编译、不许上板；板端发现 Bug 必须回本机改。

### Windows 本机（开发 + 单元测试）

```bash
cd core
cmake -B build-win -G "MinGW Makefiles" -DTTBOX_CORE_WITH_ONNX=ON
cmake --build build-win -j8
ctest --test-dir build-win --output-on-failure
```

Windows 需先准备 MSYS2 工具链，编译时把 `C:\msys64\ucrt64\bin` 加进 `PATH`。

### 交叉编译到 aarch64（出货）

板端不编译源码，用 WSL 里的交叉工具链出包再走发布链：

```bash
bash scripts/ttbox_build_release.sh   # 出货构建（先提交，否则留档 commit 不自洽）
bash scripts/ttbox_pack_ota.sh        # 打包 + 签名
```

> 出货构建必须显式带 `TTBOX_BUILD_DIR=build-aarch64-* -DCMAKE_SYSROOT=/root/sysroot -DCMAKE_TOOLCHAIN_FILE=deploy/cmake/toolchain-aarch64.cmake`。留档写进 `docs/build/release-records/`。

---

## 七、测试

```bash
# C++ 侧
cd core
cmake --build build-ascii -j8
ctest --test-dir build-ascii --output-on-failure

# Python 侧（裸入口跑全量，testpaths 已含 framework / ttbox_platform / plugins/web 三套件）
python -m pytest -q

# 口径门禁 + 文档链接
bash scripts/ttbox_conventions_gate.sh
python docs/check_links.py
```

判据一律 **0 failed**，别钉条数（用例会增删）。C++ 单测是一个单体二进制 `ttbox_core_tests`，不带参数跑全部。

板端常用验证：`systemctl is-active ttbox-core ttbox-web ttbox-preview ttbox-usbproxy`。

---

## 八、版本与交付

### 版本号为什么必须带字母前缀

当前产品版本 **`V1.0.43`**，真源是 `core/include/ttbox/core/version.hpp::kCoreVersion`。

**不要把它改成纯数字 `1.0.43`。** 盒子判「云端这个包能不能升」只看版本串排序，规则在三个地方各写一份（板端 `scripts/ttbox_ota_updater.py`、面板 `plugins/web/bin/ttbox-web.py`、云端 bridge `ota.js`），且**字母段排在数字段之前**：

| 比较 | 结果 |
|---|---|
| `V1.0.43` vs `1.5.70` | `V1.0.43` 更新，可以升 ✅ |
| `1.0.43` vs `1.5.70` | `1.0.43` 更旧，判降级、直接拒装 ❌ |

关键：这套判定跑在**盒子已装好的旧版本**里，改仓库源码救不了现网。只要跨这条线，新版本就必须带字母前缀。回归锁在 `plugins/web/tests/test_web_ota_version_scheme.py`。

CMake 的 `project(VERSION)` 只认数字，所以 `core/CMakeLists.txt` 写**去掉前缀的数字镜像** `1.0.43`，门禁第 ⑥ 项断言「`kCoreVersion` 去前导字母 == CMake VERSION」。

### 出货链路

```text
改代码 → 提交 → 本机全绿 → WSL 交叉构建（留档）→ 打包签名 → 云端入库 → 切 latest → 板端点「检查更新」
```

板子**不会自己拉云端**，OTA 只由面板「检查更新」按钮触发。云端版本管理页按 `versionCode` 倒序，`V1.0.43`（versionCode 100043）比 `1.5.xx`（105xxx）小、排列表最后一行，不影响功能（盒子只认 `/ota/latest`）。

---

## 九、常见问题

**网页打不开？** 查 `ttbox-web` 是否 active、防火墙、地址是否是 `http://<盒子IP>:8000`。

**预览黑屏？** 先看「总览」采集 FPS，为 0 说明 HDMI 没进盒子（换线 / 检查电脑是否复制输出）。

**鼠标不动？** 依次查：`output_enabled`、`mouse.enabled`、热键是否按住、`ttbox-usbproxy` 是否 active、`/run/ttbox-mouse-passthrough/cmd.sock` 是否存在。

**面板点「检查更新」没反应？** 板子不自动拉云端，只有点按钮才查；云端比板端新才出升级入口。

**改配置没生效？** 确认改的是 `/etc/ttbox/config.d/10-device.json`（唯一写回目标），改别处不生效。

---

## 十、文档

文档中心只保留**与代码有引用关系**的文档，入口见 [`docs/README.md`](docs/README.md)，分类规则见 [`docs/CONVENTIONS.md`](docs/CONVENTIONS.md)。

| 想了解 | 看哪里 |
|---|---|
| 协议与规格 | `docs/protocols/` |
| 配置/常量/路径口径 | `docs/protocols/config-path-env-registry.md` |
| 构建可复现 | `docs/build/build-reproducibility.md` |
| 出货构建留档 | `docs/build/release-records/` |
| 板端依赖与出货约束 | `deploy/DEPENDENCIES.md` |
| 服务账号约定 | `ttbox_platform/supervisor/README.md` |

---

许可证：MIT，详见 [LICENSE](LICENSE)。
