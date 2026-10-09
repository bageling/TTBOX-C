# TTBOX 依赖清单（依赖管理与部署说明）

> 本文档列出 TTBOX 在 RK3588 目标板（Orange Pi 5 Plus / Ubuntu 22.04 aarch64）上的**全部运行依赖**，
> 以及仓库内已随附的依赖文件。任何新板部署按此清单准备即可，无需联网下载大文件。

## 一、系统包（apt，需 root）

| 包 | 用途 | 安装命令 |
|---|---|---|
| `cmake` `g++` `make` `pkg-config` | 编译工具链 | `apt-get install -y cmake g++ make pkg-config` |
| `librga-dev` `librga2` | RGA 硬件缩放（头文件在 `/usr/include/rga/`） | `apt-get install -y librga-dev librga2` |
| `libjpeg-dev` | JPEG 编解码（预览流） | `apt-get install -y libjpeg-dev` |
| `v4l-utils` | V4L2 调试工具（v4l2-ctl） | `apt-get install -y v4l-utils` |
| `libopencv-dev` | OpenCV 4.x（预览检测框绘制：身体框+头部小框+标签） | `apt-get install -y libopencv-dev` |
| `python3-flask` | Web 后端（ttbox-web 插件） | `apt-get install -y python3-flask` |
| `python3-waitress` | WSGI 服务器（ttbox-web 生产运行） | `apt-get install -y python3-waitress` |
| `python3-numpy` | 数值计算（插件/测试脚本） | `apt-get install -y python3-numpy` |

> 说明：Ubuntu 22.04 (jammy) 的 apt 源自带上述全部包，无需 pip3。OpenCV 版本 4.5.4。

## 二、仓库随附依赖（本仓库已入库，部署时直接复制）

| 文件 | 仓库路径 | 部署目标 | 用途 |
|---|---|---|---|
| librknnrt.so (7.73MB, 2.3.2) | ⛔ **不在仓库**（作用域裁决已删 1.5.2 残留；来源 = 链接期 sysroot，见 `lib-scope-ruling.md` §6） | payload `releases/<ver>/lib/librknnrt.so`（由 `scripts/ttbox_fhs_init.sh` 从链接期 sysroot **同源**拷入，经 `rknnrt_gate` 校验） | RKNN NPU 运行时（同时支持 /dev/rknpu 与 /dev/dri/renderD DRM 模式）；由 payload 内 `$ORIGIN/../lib` RUNPATH 就近解析 |
| rknn_api.h | `third_party/rknn/rknn_api.h` | `/usr/local/include/rknn/` | RKNN 推理 API 头文件 |
| rknn_matmul_api.h | `third_party/rknn/rknn_matmul_api.h` | `/usr/local/include/rknn/` | RKNN 矩阵乘 API 头文件 |
| 检测模型 | `models/installed/jwdl_sjzv11/model.rknn` | `/var/lib/ttbox/models/installed/jwdl_sjzv11/model.rknn` | 256x256 INT8 7类 6输出 YOLO 模型（FHS 客户数据目录） |

安装命令（板端）：
```bash
# librknnrt.so：**不**手工从仓库拷（仓库已无此文件）。它随发布树交付于 releases/<ver>/lib/，
#   由 scripts/ttbox_fhs_init.sh 从【链接期 sysroot】同源拷入，并经 rknnrt_gate 校验 =
#   2.3.2 / 7726232 B / sha256 d31fc19c…；运行期由 payload 内 $ORIGIN/../lib RUNPATH 就近解析，
#   无需写入 /usr/local/lib，也无需 ldconfig。
#   ⛔ 严禁把 1.5.2 过期残留（5241144 B / sha256 9f53d7b1…）拷上设备（lib-scope-ruling.md §6）。
sudo mkdir -p /usr/local/include/rknn
sudo cp third_party/rknn/rknn_api.h third_party/rknn/rknn_matmul_api.h /usr/local/include/rknn/
# 模型复制到 FHS 客户数据目录（/var/lib/ttbox/models —— 升级/回滚绝不触碰）
sudo mkdir -p /var/lib/ttbox/models/installed/jwdl_sjzv11
sudo cp models/installed/jwdl_sjzv11/model.rknn /var/lib/ttbox/models/installed/jwdl_sjzv11/
```

## 三、HDMI RX 链路（内核/设备树）

| 项 | 说明 |
|---|---|
| HDMI RX overlay | `/boot/extlinux/extlinux.conf` 追加 `fdtoverlays rk3588-hdmirx.dtbo`，重启后出现 `/dev/video0` |
| 内核模块 | `CONFIG_VIDEO_ROCKCHIP_HDMIRX=y`（已编译进内核，Ubuntu 官方内核自带） |
| NPU 设备 | DRM 模式：`/dev/dri/renderD129`（librknnrt.so 兼容） |
| EDID 注入 | `deploy/scripts/edid/edid_apply.sh（已取代孤儿脚本 deploy/inject_edid.sh，2026-09-18 J-35 处置）`（写 sysfs 节点切换 EDID 版本：1=340M/1080p，2=600M/4K） |
| 生产配置模板 | `deploy/config/default.json.prod`（rgb/0.25/640/安全基线，部署时复制到 /opt/ttbox/config/） |

EDID 注入（新板首次部署必须执行，否则 PC 端虚拟屏只能枚举 1080p）：
```bash
sudo bash deploy/scripts/edid/edid_apply.sh（已取代孤儿脚本 deploy/inject_edid.sh，2026-09-18 J-35 处置） 2   # 注入 600M EDID，PC 切换 4K 输出
cat /sys/devices/platform/fdee0000.hdmirx-controller/hdmirx/hdmirx/edid  # 应输出 2
```

## 四、systemd 服务（deploy/systemd/）

> **唯一权威源 = `deploy/systemd/`**（DEP-07 / T1.05 收敛）：仓库内每 unit 有且仅有一份。
> 历史副本 `plugins/web/config/ttbox-web.service`、`plugins/preview/config/ttbox-preview.service`、
> `usbproxy/systemd/ttbox-usbproxy.service` **已删除**，各目录只留一份 `README.md` 指向本表。
> 收敛动作不是"挑一份对的留下"——两份现存副本各有不同的病；记录该病历的
> `usbproxy/systemd/README.md` 已于 2026-09-19 文档清理中移除，
> 需追溯用 `git show HEAD:usbproxy/systemd/README.md`。
>
> 部署时 unit 是**版本产物的一部分**：随发布树交付于 `/opt/ttbox/current/deploy/systemd/`，
> 由 `scripts/ttbox_release_install.sh`（原子切换/回切）与 `scripts/ttbox_ensure_services.sh`
> （幂等自愈，配 `ttbox-ensure.timer` 每 10 分钟巡检）安装到 `/etc/systemd/system/`。
> **请勿再从旧副本路径拷贝 `*.service`。**

| 服务文件 | 服务名 | 说明 |
|---|---|---|
| ttbox-core.service | ttbox-core | C++ 核心（V4L2 Capture + RGA + RKNN + DecodeNMS + AimThread） |
| ttbox-web.service | ttbox-web | Web 控制台 0.0.0.0:**8000**（源码定死，见 plugins/web/bin/ttbox-web.py 的 LISTEN_PORT） |
| ttbox-preview.service | ttbox-preview | 预览流 127.0.0.1:8001 |
| ttbox-edid.service | ttbox-edid | 开机 EDID 注入（oneshot） |
| ttbox-usbproxy.service | ttbox-usbproxy | USB 鼠标代理（HID） |

部署命令：
```bash
sudo cp deploy/systemd/*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ttbox-core ttbox-web ttbox-preview ttbox-edid
# usbproxy 按需启用
```

## 五、目录部署结构（/opt/ttbox —— releases/\<ver\> + current 软链）

> 旧版本文档描述的是"直铺 /opt/ttbox"的旧布局，与实测不符（板端 `current` 手工软链、`releases/` 单版本）。现按 DEP-06 的 releases/current 布局订正。

```
/opt/ttbox/
├── releases/
│   ├── 1.2.0/                     # 完整运行树（原子整体，随版本交付）
│   │   ├── bin/{ttbox_core_main, ipc_ping}
│   │   ├── lib/{librknnrt.so, …}  # 设备专有库随版本走，由 $ORIGIN RUNPATH 就近解析
│   │   ├── plugins/{web,preview}/ # Web(8000) / 预览(8001)
│   │   ├── framework/  ttbox_motion/  platform/
│   │   ├── usbproxy/{board/*.sh, usb-proxy, config…}
│   │   ├── scripts/{edid/, wifi_manager.py}
│   │   ├── deploy/systemd/*.service   # unit 的唯一事实源随版本走
│   │   └── RELEASE_MANIFEST.json      # {version, built_at, git_sha, files_sha256}
│   └── 1.1.0/                     # 保留前 2 版（回滚用）
├── current -> releases/1.2.0      # 原子切换的唯一指针
├── plugins -> current/plugins     # 迁移期过渡软链①（插件发现 + 旧 web unit 路径兼容）
└── scripts -> current/scripts     # 迁移期过渡软链②（wifi_manager/EDID/LAN-blocklist 工具链 import）
```

> **`current` 是五个 unit 路径的共同地基**（core/web/preview/usbproxy/edid 共 7 处引用）；
> 不存在时这些服务全部 `203/EXEC`。切换走"同分区 rename"，中途掉电只留完整版本或 `.staging` 残留。
>
> **过渡软链清单**（由 `ttbox_release_install.sh --activate` 浇筑，`ttbox_release_verify.sh` 逐条断言；
> 只收录随版本交付的代码/工具目录，**客户数据绝不入列**——指进 release 树会在下次发布时被整体替换）：
> | 旧布局硬编码路径 | 指向 | 兜住的运行期引用 |
> |---|---|---|
> | `/opt/ttbox/plugins` | `current/plugins` | `framework_api.py:60` 插件发现、旧 web unit 停血路径 |
> | `/opt/ttbox/scripts` | `current/scripts` | `ttbox-web.py:53,1190,3374` 等 wifi/EDID/LAN 工具链、`scripts/edid/hdmirx_edid.py:9`、`edid_apply.sh:17` |
>
> 其余 `/opt/ttbox/*` 硬编码（config/models/presets/tools/runtime 等）属客户数据或内部工具，
> **不靠软链兜**，须由源码侧改为 FHS 路径（详见 DEP-06 硬编码路径普查报告，归 T1.02/T1.05/后续 lane）。

发布与切换（DEP-06）：
```bash
# 发布 + 原子切换（payload 校验 → staging → 全量 sha256 → 原子发布 → 换链 → 健康检查 → 失败自动回切）
scripts/ttbox_release_install.sh <ver> <payload_dir> --activate
# 回滚（换指针，无需备份目录——上一版本本身就是完整运行树）
scripts/ttbox_release_install.sh --rollback
# 独立体检（manifest sha256 / ExecStart -x / WorkingDirectory -d / RUNPATH 自包含 / StartLimit 段位）
scripts/ttbox_release_verify.sh [<ver>]
```

> 客户数据（模型/HID/授权）在 `/var/lib/ttbox/**`，配置在 `/etc/ttbox/**`，**均不在 `/opt/ttbox` 内**，升级/回滚绝不触碰（见 §一、§二与 design §B2.2）。

## 五·附、测试钩子（生产不可达）

发布/校验脚本内置两个钩子，**生产环境完全不可达**，仅用于本机（WSL）与 CI 自测：

| 钩子 | 触发条件（缺一不可） | 作用 | 默认值 |
|---|---|---|---|
| `--selftest-stall <秒>` | 命令行显式传该参数 **且** 环境变量 `TTBOX_RELEASE_SELFTEST=1` | staging 完成后停顿 N 秒，用于验证"切换中途 kill 不留半截版本"（`.staging` 残留隔离） | 未传 = 无停顿；只传参数不设环境变量 → 脚本报错退出（防从 profile/systemd `Environment=`/CI 泄漏导致生产发布静默卡住） |
| `TTBOX_SYSTEMD` | 环境变量 `auto`(默认)/`1`/`0` | `0`=强制跳过 daemon-reload/restart 与健康检查（WSL、容器里无 ttbox unit 时用，**会打印 SKIP，绝不假装跑过**）；`1`=强制使用；`auto`=探测 systemd 是否 PID1 | `auto` |

## 六、编译（core）

```bash
cd core
mkdir -p build && cd build
cmake .. && make -j4
# 产物: ttbox_core_main
```

关键 CMake 依赖探测：
- RGA：`find_path(RGA_INCLUDE_DIR im2d.h PATHS /usr/local/include /usr/include /usr/include/rga)`
- RKNN：`find_library(RKNNRT_LIBRARY rknnrt …)`（**走 sysroot re-root**；★ T1.18 已删除
  `if(EXISTS /opt/ttbox/lib/librknnrt.so)` host 绝对路径旁路，见 `lib-scope-ruling.md` §6）
- libjpeg / OpenCV：`find_package(JPEG)` + `find_package(OpenCV COMPONENTS core imgproc)`

### 六·1 预览源闭包（★ T1.18）

`core/src/preview/PreviewModule.cpp` **同时**依赖 libjpeg 与 OpenCV（无条件 `#include <jpeglib.h>`
+ `<opencv2/core.hpp>` + `<opencv2/imgproc.hpp>`）。故其**源选择与链接注入条件统一为
`JPEG_FOUND AND OpenCV_FOUND`**：

- **两者都齐** ⇒ 编译真实现（预览 + 检测框绘制可用）；
- **缺 libjpeg、或 缺 OpenCV、或 双缺** ⇒ 改链 `core/src/preview/PreviewModule_stub.cpp`
  （空实现：`start()` 回 `false`、`snapshot()` 回 `false`、`stop()` 空）⇒ `ttbox_core` /
  `ttbox_core_main` **仍可编译链接**（消费端 `Application.cpp` / `CoreRuntime.cpp` 无需改动），
  但**预览功能整体禁用**。

configure 期会打印两行**配置位**（供发布门禁解析）：

```text
-- TTBOX_CORE_HAS_JPEG=TRUE|FALSE
-- TTBOX_CORE_HAS_OPENCV=TRUE|FALSE
```

> ⛔ **发布门禁**：任一 ≠ `TRUE` 的构建**禁止出货**（发布链据此非零退出）——
> "无 libjpeg/OpenCV 构建 = 可编、可测、**不可发**"。板端**必须**带 libjpeg + OpenCV 4.5.4
> （见 §一、§二）。
> 权威裁决：`libjpeg-linkage-ruling.md`（架构师 T1.18 裁决）。

## 七、安全基线（部署后必须确认）

```bash
python3 -c "import json;c=json.load(open('/opt/ttbox/config/default.json'));print(c['output_enabled'],c['mouse'])"
# output_enabled=False  mouse.enabled=False  calibrating=False
```
