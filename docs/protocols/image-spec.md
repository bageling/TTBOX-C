# ttbox2 Ubuntu RK3588 母版规格 (image-spec)

> 阶段 4 产物 · 定义最终 IMG 的完整组成
> 状态: SPEC (未生成 IMG, 未硬件验证)
> 标记: 本地有真实文件并已算 SHA256 的为 **PINNED**; 尚无真实文件的为 **UNRESOLVED**

---

## 1. Board

| 项 | 值 |
|---|---|
| 目标板 | OrangePi 5 Plus **或** OrangePi 5 Ultra (RK3588) |
| 构建参数 | `BOARD=orange-pi-5-plus` / `BOARD=orange-pi-5-ultra` (config.env) |
| 规则 | 两板 DTB/Kernel 禁止混入同一镜像; 每板独立构建, 各选对应 DTB |

当前测试机型号已确认 (阶段 5): **BOARD=orange-pi-5-plus**

## 2. Ubuntu version

- **Ubuntu 22.04 LTS (jammy) ARM64**
- 不升级 mainline / 不升级新 Ubuntu 内核

## 3. Base image

| 项 | 值 | 状态 |
|---|---|---|
| 来源入口 | OrangePi 官方下载页 (5 Plus): `http://www.orangepi.cn/html/hardWare/computerAndMicrocontrollers/service-and-support/Orange-Pi-5-plus.html` | 下载页 URL |
| 归档位置 | `resources/base/` (下载解压后放入, 填 `config.env:BASE_IMAGE_FILE`) | 目录已建 |
| 文件 | 官方 Ubuntu 22.04 arm64 镜像 (百度网盘分发, 无稳定直链) | **UNRESOLVED** |
| SHA256 | 下载后计算并回填 (构建脚本强制校验) | **UNRESOLVED** |

构建输入必须为**唯一指定文件**: 该 .img 的 SHA256 一旦回填, 即成为可复现基线; 未回填前构建中止。

## 4. Kernel (PINNED_KERNEL)

| 项 | 值 |
|---|---|
| 文件 | `linux-image-current-rockchip-rk3588_1.2.2_arm64.deb` |
| 版本 | 1.2.2 (Source: `linux-6.1.99-rockchip-rk3588`) |
| 架构 | arm64 |
| 来源 | 构建资源归档 `resources/kernel/` (已归档, manifest.sha256 校验) |
| SHA256 | `72CABCFF33ACFB79E27F391A02A730C770360313D00ACC4D0C887435342B11CC` |
| 大小 | 23,055,992 B |
| 状态 | **PINNED** |

原始上游发布 URL 未记录 (**UNRESOLVED**, 但构建不依赖它——文件已内置于项目并 SHA256 校验, 可重复)。

## 5. DTB

| 项 | 值 |
|---|---|
| 文件 | `linux-dtb-current-rockchip-rk3588_1.2.2_arm64.deb` |
| 版本 | 1.2.2 (Source: `linux-6.1.99-rockchip-rk3588`) |
| 架构 | arm64 |
| 来源 | 构建资源归档 `resources/kernel/` (已归档, manifest.sha256 校验) |
| SHA256 | `7250E927388869B8CFFF851E02F72A18463B0A75156D0D78C9CF48C2EA80B09E` |
| 大小 | 99,716 B |
| 内含 (已解包实检) | `boot/dtb-6.1.99-rockchip-rk3588/rockchip/rk3588-orangepi-5-plus.dtb`、`rk3588-orangepi-5-ultra.dtb` 等; 按 `BOARD` 只启用对应 DTB |
| 状态 | **PINNED** |

## 6. HDMI RX

| 项 | 值 |
|---|---|
| Overlay 文件 | `boot/dtb-6.1.99-rockchip-rk3588/rockchip/overlay/rk3588-hdmirx.dtbo` (位于第 5 节 dtb deb 内, 已实包验证存在) |
| 启用方式 | 引导环境 `/boot/orangepiEnv.txt` 写入 `overlays=hdmirx` |
| 驱动 | 内核 6.1.99-rockchip-rk3588 内 `rk_hdmirx` (V4L2 mplane) |
| 状态 | **PINNED** (配置来源确定) |

## 7. RKNN Runtime

| 项 | 值 | 状态 |
|---|---|---|
| `librknnrt.so` 版本 | `2.3.2 (429f97ae6b@2025-04-09T09:09:27)` (二进制内字符串实取) | PINNED |
| `librknnrt.so` 来源 | 构建资源归档 `resources/rknn/librknnrt.so` → 安装至 `/usr/lib/librknnrt.so` | PINNED |
| `librknnrt.so` SHA256 | `D31FC19C85B85F6091B2BD0F6AF9D962D5264A4E410BFB536402EC92BAC738E8` | PINNED |
| `librknnrt.so` 大小 | 7,726,232 B | PINNED |
| `rknn_server` (可选) | 版本 `2.3.2` (build 1842325@2025-03-30); SHA256 `EEA12FE4270FAD8AFF015056319705B2EB871563EBD001EFF8D8788BDD1C0CFA`; 归档于 `resources/rknn/`; **默认不进 Production Image** (非 AI 正常运行必需, 仅 DEV 调试) | PINNED |
| `rknn-toolkit-lite2` | 固定 `==2.3.2`, wheel `rknn_toolkit_lite2-2.3.2-cp311-cp311-linux_aarch64.whl` | 版本 PINNED |
| wheel 文件/URL | 归档位置 `resources/rknn/` (待人工下载放入); 来源 = Rockchip 官方发布渠道 (airockchip/rknn-toolkit2 发布物) | **UNRESOLVED** |
| wheel SHA256 | 下载后回填 (`config.env:RKNN_WHEEL_PATH` + 构建校验) | **UNRESOLVED** |

禁止: `latest` 下载、自动找新版、自动升级 RKNN。

## 8. Python

| 项 | 值 | 状态 |
|---|---|---|
| 版本 | Python **3.11** | PINNED (大版本) |
| 来源 | Ubuntu 22.04 默认 3.10 不满足; 用 `deadsnakes/ppa` 安装 `python3.11` | 来源固定 |
| 精确小版本 | 需首次构建后锁定 (PPA 内容会演进) | **UNRESOLVED** (构建时以 apt 安装结果回填) |

## 9. Python dependencies (venv 内固定版本)

| 包 | 固定版本 | SHA256 | 状态 |
|---|---|---|---|
| numpy | `==1.26.4` (最后 1.x, cp311 aarch64 wheel) | 待回填 | 版本 PINNED / SHA256 UNRESOLVED; 归档 `resources/python/` |
| evdev | `==1.7.1` (纯 Python) | 待回填 | 版本 PINNED / SHA256 UNRESOLVED; 归档 `resources/python/` |
| rknn-toolkit-lite2 | `==2.3.2` | 见第 7 节 | UNRESOLVED (wheel) |

依赖安装走**本地 wheel 缓存** (构建时 `pip download` 一次并锁定), 首次构建后回填 SHA256。

## 10. ttbox2

| 项 | 值 |
|---|---|
| 版本 | 0.0.1 |
| 内嵌方式 | 构建时 rsync 项目 `ttbox/` + `pyproject.toml` → `/opt/ttbox2/app/`; 排除 tests/vendor/scripts/__pycache__/.pytest_cache/开发报告等 |
| 运行 | venv `/opt/ttbox2/venv/bin/python -m ttbox`, `PYTHONPATH=/opt/ttbox2/app` |
| 不散落 | 源码不进 /usr/local /home /tmp /etc; 系统服务只引用 `/opt/ttbox2` |

## 11. Model

| 项 | 值 |
|---|---|
| 文件 | `models/yolo261n-rk3588.rknn` (归档: `resources/models/`) |
| SHA256 | `2B178F3DC5013A101242E988672BE9199CEB492B7EABA4C6271231A00CB770AA` |
| 大小 | 7,445,558 B |
| 内嵌位置 | `/opt/ttbox2/models/` |
| 状态 | **PINNED** (独立资产, 可替换模型不重建系统) |

## 12. Config

| 文件 | SHA256 |
|---|---|
| `config/default.json` | `3A474D6CD6B2A70B4A0E3406E38A16644712FA1A22797A87B68EE11C330EF66A` |
| `config/hdmirx_edid_identity.json` | `1003FE5E3C14868770C6928F0233745C91F9D856727264E3AF7CC160CA149E11` |
| `config/yolo261n-rk3588.json` | `08962F8DCF85D79E8D61CD1FC5C0E3801BEA720B60FE9AF64A0FC882C2F3C847` |

内嵌至 `/opt/ttbox2/config/`; 运行时修改写回 `/opt/ttbox2/config/` (可写数据区), 不写系统只读区。

## 13. systemd

镜像预置 5 个单元 (`scripts/systemd/`), 全部 `Restart=on-failure`、日志进 journal:

```
ttbox-firstboot.service  首次初始化 (oneshot, 一次性)
   ↓
ttbox-gadget.service     HID Gadget (oneshot)
   ↓
ttbox-passthrough.service 键鼠透传
   ↓
ttbox-ai.service         AI 采集/推理/自瞄
ttbox-web.service        Web 控制台 :8000 (独立)
```

业务进程不合并; 启动不依赖手动执行 python。

## 14. firstboot

只做初始化, **不安装任何软件** (软件全部镜像构建期内嵌):

1. 生成机器唯一信息 (machine-id / SSH host keys 由系统首次启动自动生成, 镜像内已清空)
2. 创建 `/opt/ttbox2/{data,logs,runtime}` 目录
3. 检查 Python / numpy / evdev / rknnlite
4. 检查模型存在
5. 检查 `/dev/video0`、`/dev/input`
6. 初始化 HID Gadget
7. 执行一次 `ttbox doctor`
8. 写入 `.firstboot-done` 标记后退出 (有标记则跳过)

## 15. filesystem layout

```
/ (Ubuntu 22.04 arm64 rootfs)
├── boot/
│   ├── orangepiEnv.txt            # overlays=hdmirx
│   └── dtb-6.1.99-rockchip-rk3588/rockchip/   # 由 dtb deb 提供 (仅对应 BOARD 的 dtb+overlay)
├── usr/lib/librknnrt.so           # RKNN runtime (2.3.2)
├── usr/bin/rknn_server            # 可选
├── etc/systemd/system/ttbox-*.service
└── opt/ttbox2/
    ├── app/                       # ttbox 包 + pyproject (+ models/config 软链)
    ├── config/                    # default.json 等
    ├── models/                    # yolo261n-rk3588.rknn
    ├── data/                      # .ai_state.json / .ai_models.json / 标记等
    ├── logs/                      # ai.log 等
    ├── runtime/                   # ttbox-firstboot.sh / hid-gadget.sh
    └── venv/                      # Python 3.11 + numpy/evdev/rknnlite
```

## 16. 构建主机要求

| 项 | 要求 |
|---|---|
| OS | Linux (Debian/Ubuntu 均可) |
| 架构 | **AMD64 或 ARM64** (AMD64 需 `qemu-user-static`) |
| 权限 | root (loop/chroot) |
| 工具 | curl / losetup / rsync / xz / 7z(可选) / parted(可选) |
| 网络 | 仅构建期需要 (下载固定版本资源); **镜像内嵌全部依赖, 运行时不依赖互联网** |
| Windows 开发机 | 只负责源码/配置/构建脚本/资源整理, 不承担 loop/chroot 构建 |

## 17. 外部资源清单 (版本 + SHA256)

| 资源 | 版本 | SHA256 | 状态 |
|---|---|---|---|
| linux-image-current-rockchip-rk3588 deb | 1.2.2 (6.1.99) | `72CABCFF…B11CC` | **PINNED** ✅ |
| linux-dtb-current-rockchip-rk3588 deb | 1.2.2 (6.1.99) | `7250E927…80B09E` | **PINNED** ✅ |
| rk3588-hdmirx.dtbo | 6.1.99 (内含于 dtb deb) | (随 deb 校验) | **PINNED** ✅ |
| librknnrt.so | 2.3.2 | `D31FC19C…8BAC738E8` | **PINNED** ✅ |
| rknn_server | 2.3.2 | `EEA12FE4…D1C0CFA` | **PINNED** ✅ |
| yolo261n-rk3588.rknn | — | `2B178F3D…0CB770AA` | **PINNED** ✅ |
| config/default.json 等 3 件 | — | 见第 12 节 | **PINNED** ✅ |
| Ubuntu 22.04 arm64 base .img | 22.04 | 待回填 | **UNRESOLVED** |
| rknn_toolkit_lite2-2.3.2-cp311-…-aarch64.whl | 2.3.2 | 待回填 | **UNRESOLVED** |
| numpy==1.26.4 wheel (cp311) | 1.26.4 | 待回填 | UNRESOLVED (版本已锁) |
| evdev==1.7.1 wheel | 1.7.1 | 待回填 | UNRESOLVED (版本已锁) |
| python3.11 (deadsnakes) 精确小版本 | 3.11.x | 待首次构建回填 | UNRESOLVED |

构建强制: 所有 PINNED 文件在构建时校验 SHA256; 任何 UNRESOLVED 项缺失即中止, 不使用 `latest` / 未固定版本。

---

## 资源归档 (阶段 5)

```
resources/
├── base/     # Ubuntu 22.04 arm64 BSP .img      (待下载, UNRESOLVED)
├── kernel/   # linux-image/dtb-current-rockchip-rk3588_1.2.2 deb  (PINNED ✅)
├── rknn/     # librknnrt.so 2.3.2 + rknn_server 2.3.2 (PINNED ✅); wheel 待下载 (UNRESOLVED)
├── python/   # numpy 1.26.4 / evdev 1.7.1 wheels (待下载, UNRESOLVED)
├── models/   # yolo261n-rk3588.rknn              (PINNED ✅)
└── manifest.sha256   # 全部归档文件 SHA256 (已归档 5 项校验通过)
```

### 构建前检查 (阶段 5)

- 工具: `scripts/build/verify_resources.sh`（Linux 构建机权威执行；本机 Windows 以 PowerShell 等价校验，git bash sha256sum 受 MSYS 管道限制）
- 已归档 5 文件 SHA256 与 manifest 一致 ✅
- 缺失 4 项：`base/*.img`、`rknn_toolkit_lite2-2.3.2-cp311-cp311-linux_aarch64.whl`、`numpy-1.26.4-…-aarch64.whl`、`evdev-1.7.1-py3-none-any.whl`
- **RESOURCE_CHECK=FAIL**（补齐缺失资源并回填 SHA256 后方可进入镜像构建）

---

## 附: DEV / PRODUCTION 镜像定义

| | DEV IMAGE | PRODUCTION IMAGE |
|---|---|---|
| SSH | 允许 | 关闭 (默认) |
| 调试工具 / journal | 保留 | 保留 journal 与日志 |
| Python 调试 | 允许 | 不包含 |
| 开发缓存 (pyc/pytest_cache/git) | — | 一律排除 |
| 业务进程 | 同 PRODUCTION | 默认启动 passthrough/ai/web |
| Web 管理 | 保留 | 保留 |
| 只读文件系统 | 不做 | 本阶段不做 |

构建入口 `config.env:IMAGE_FLAVOR=dev|prod` (构建脚本扩展, 本阶段仅定义)。

---

## 禁止与声明

- 本规格仅定义母版组成; 未生成 IMG、未连接测试机、未烧录、未验证 HDMI/RKNN/HID
- 未使用任何 `latest` / 未固定版本的外部依赖
- 所有 SHA256 均为本地真实文件计算值; 标 UNRESOLVED 的项无真实文件, 未猜测
