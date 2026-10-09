# TTBOX 文档中心

这里是 TTBOX 模块版**现存**的中文文档。分类规则与命名约定见 [`CONVENTIONS.md`](CONVENTIONS.md)。

> **2026-09-19 清理**：按业主指示（"不能影响程序源码运行，清理没用的，README.md 要重新写"），
> 移除与代码无关的过程报告、历史交接与介绍类文档共 **100 份 / 763 KB**，只保留
> **被源码/脚本/测试/部署描述引用**的文件，以及必要的规约、协议与索引。
> 处置清单见 `.workbuddy/artifacts/待清理文档清单-2026-09-19.md`。
> 被移除的文件仍在 git 历史里，`git restore --source=HEAD -- <路径>` 即可取回（本次未提交）。

---

## 一、唯一真源表（★ 只信这一张表）

| 主题 | 唯一真源 | 说明 |
|---|---|---|
| 项目总入口 | [`../README.md`](../README.md) | 接线、服务、目录结构、链路、构建、测试 |
| 文档分类规则 | 本文件 + [`CONVENTIONS.md`](CONVENTIONS.md) | 放哪判定树 + 命名约定 |
| IPC 协议 | [`protocols/ipc-protocol.md`](protocols/ipc-protocol.md) | 网页 ↔ 核心通信（真值以源码为准） |
| 旧路径指针桩 | [`ipc-protocol.md`](ipc-protocol.md) | `core/src/ipc/IpcServer.hpp` 按旧路径引用，故保留 |
| 母版规格 | [`protocols/image-spec.md`](protocols/image-spec.md) | Ubuntu RK3588 母版规格 |
| **口径登记表** | [`protocols/config-path-env-registry.md`](protocols/config-path-env-registry.md) | 配置·常量·路径唯一真源：RUNTIME env allowlist + 跨语言同值常量 |
| OTA 服务端契约 | [`protocols/ota-server-contract.md`](protocols/ota-server-contract.md) | 分发服务器查询契约 |
| 输出后端设计 | [`research/OUTPUT_BACKEND_DESIGN.md`](research/OUTPUT_BACKEND_DESIGN.md) | `core/src/output/OutputBackend.hpp` 的设计依据 |
| 构建可复现 | [`build/build-reproducibility.md`](build/build-reproducibility.md) | 可复现锚（CMake / 发布脚本按 §N 引用） |
| 板端依赖 | [`../deploy/DEPENDENCIES.md`](../deploy/DEPENDENCIES.md) | 板端依赖清单 |
| 服务用户/组约定 | [`../scripts/ttbox_fhs_init.sh`](../scripts/ttbox_fhs_init.sh) | ★V1.0.52：原记录于 `ttbox_platform/supervisor/README.md`，该目录已随「去 Python」批次 1 移除；现约定即该脚本自身（建号处） |

### 交付前过程报告（被源码或测试引用，故保留）

| 文档 | 内容 |
|---|---|
| [`交付前更新功能定案-2026-09-18.md`](交付前更新功能定案-2026-09-18.md) | OTA 线 35 条实施清单与定案 |
| [`交付前Web实测报告-2026-09-19.md`](交付前Web实测报告-2026-09-19.md) | 面板真机实测结论与复现要点 |
| [`交付前Web按钮落实审计-2026-09-19.md`](交付前Web按钮落实审计-2026-09-19.md) | 95 控件端到端追线，A/B/C/D 四档 |
| [`面板功能补齐实施计划-2026-09-19.md`](面板功能补齐实施计划-2026-09-19.md) | T2 系列功能补齐计划与状态 |

### 历史交接（只读，禁改写）

| 文档 | 内容 |
|---|---|
| [`handover/2026-09-17/RELEASE.md`](handover/2026-09-17/RELEASE.md) | 发布记录 |
| [`handover/2026-09-17/控制台布局冻结基线-2026-09-18.md`](handover/2026-09-17/控制台布局冻结基线-2026-09-18.md) | 控制台布局冻结基线 |
| [`handover/2026-09-17/控制台1比1采用-架构设计-2026-09-18.md`](handover/2026-09-17/控制台1比1采用-架构设计-2026-09-18.md) | 控制台 1:1 采用架构设计 |
| [`handover/2026-09-17/配置常量路径口径-基线与整改方案-2026-09-17.md`](handover/2026-09-17/配置常量路径口径-基线与整改方案-2026-09-17.md) | 口径门禁的判定依据 |

---

## 二、这些文档谁在引用（改名或删除前先看这张表）

`docs/check_links.py` 只查文档之间的相对链接，**查不到代码里的引用**。下表是代码侧的引用关系，
从全仓逐个字面量扫出来后核对过；动这些文件前请先确认引用方。

| 文档 | 引用方 |
|---|---|
| `CONVENTIONS.md` | `scripts/ttbox_m207_accept.py:991` |
| `ipc-protocol.md` | `core/src/ipc/IpcServer.hpp:3,24`、`plugins/web/api_v1.py` |
| `protocols/config-path-env-registry.md` | `core/src/common/Paths.hpp:7`、`plugins/web/bin/ttbox-web.py`、`plugins/web/lib/paths.py`、`scripts/edid/edid_apply.sh:14`、`scripts/ttbox_conventions_gate.sh:11,291` |
| `protocols/ota-server-contract.md` | `plugins/web/bin/ttbox-web.py`、`tools/ota/fake_ota_server.py` |
| `交付前更新功能定案-2026-09-18.md` | `deploy/systemd/ttbox-ota.path` |
| `交付前Web实测报告-2026-09-19.md` | `plugins/web/bin/ttbox-web.py`、`plugins/web/tests/test_web_config_crop_normalize.py`、`test_web_storage_expand_poll.py` |
| `交付前Web按钮落实审计-2026-09-19.md` | `plugins/web/tests/` 下 4 个测试文件 |
| `面板功能补齐实施计划-2026-09-19.md` | `plugins/web/tests/test_web_hotkey_guard.py`、`test_web_presets_roundtrip.py` |
| `build/build-reproducibility.md` | `core/CMakeLists.txt`（2 处）、`scripts/ttbox_build_release.sh`（7 处） |
| `research/OUTPUT_BACKEND_DESIGN.md` | `core/src/output/OutputBackend.hpp:6` |
| `handover/2026-09-17/RELEASE.md` | `scripts/ttbox_m207_accept.py:498` |
| `handover/2026-09-17/配置常量路径口径-基线与整改方案-2026-09-17.md` | `scripts/ttbox_conventions_gate.sh:4` |
| `handover/2026-09-17/控制台布局冻结基线-2026-09-18.md` | `plugins/web/tests/test_web_console_parity.py` |
| `handover/2026-09-17/控制台1比1采用-架构设计-2026-09-18.md` | `scripts/ttbox_m2xx_console_accept.py:5` |
| `../deploy/DEPENDENCIES.md` | `deploy/systemd/ttbox-edid.service` |

> 注：`core/src/output/OutputBackend.hpp:6` 与 `scripts/ttbox_m2xx_console_accept.py:5` 里写的文件名
> 与磁盘上的实际名有出入（`OUTPUT_BACKEND_RESEARCH.md` / `控制台1:1采用`），按精确路径匹配抓不到，
> 是靠人工核对补上的。**修注释比删文档更合适**，但那属于改源码，本次未动。

---

## 三、链接自检（改完文档后必跑）

```bash
python docs/check_links.py
# 递归覆盖 docs/** + modules/** + 根 README.md；期望 broken_count=0
```
