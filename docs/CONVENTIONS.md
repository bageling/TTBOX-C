# 文档与代码分类约定（CONVENTIONS）

> **归属**：代码梳理批次 0（2026-09-17）
> **依据**：`docs/handover/2026-09-17/代码梳理方案-2026-09-17.md` §2 / §3 / §6
> **用途**：**新增文件时"放哪"的判定表** + **命名约定**。命中即停，不必纠结。
> 文档总入口见 [`README.md`](README.md)；出货硬约束以 `scripts/ttbox_fhs_init.sh` 的 payload 白名单闭集为准。

---

## 一、核心维度：按「消费方 + 生命周期」而非「运行时分层」

本仓库真正的"架构"不是代码分层，而是**出货物边界**（payload 白名单、FHS release 树、编译期 `TTBOX_PROJECT_ROOT`、`scripts/` 路径锚）。
因此**顶层按「谁消费、何时消费、是否进 payload」划分**；运行时分层维度**下沉到 `core/src/` 内部**使用。

---

## 二、新增文件"放哪"判定表（★ 按顺序问，命中即停）

| 序 | 自问 | 命中 → 落点 |
|---|---|---|
| 1 | 它是 **C++ 源码** 吗？ | `core/src/<层>/`（跨模块接口进 `core/include/ttbox/core/`） |
| 2 | 它是 **C++ 的测试/工具** 吗？ | 单测 `core/tests/`；板端/仿真工具 `core/tools/` |
| 3 | 它是 **随包运行的 Python** 吗（被 web/core/fhs 运行期加载）？ | 插件 `plugins/<name>/`；运动 `ttbox_motion/`（★V1.0.52 起框架 `framework/` 已移除） |
| 4 | 它是 **构建/发布/运维脚本**，且被 FHS 或发布门禁引用？ | `scripts/`（★**不得**外移；被 fhs 引用还要同步改 `ttbox_fhs_init.sh`） |
| 5 | 它是 **离线/开发期工具**（不在板端跑）？ | `tools/`（模型转换/签发类） |
| 6 | 它是 **部署描述**（unit/toolchain/出厂配置）？ | `deploy/{systemd,cmake,config}/` |
| 7 | 它是 **配置模板** 吗？ | 开发模板 `config/`；出厂基线 `deploy/config/`；**运行期真值不在仓库** |
| 8 | 它是 **测试** 吗？ | C++→`core/tests/`；pytest→**就近包内 `tests/`**；跨模块/板端→`tests/` |
| 9 | 它是 **文档** 吗？ | `docs/<类别>/`（类别见 §四） |
| 10 | 以上都不是（一次性探针/抓取物/构建产物） | **不进库**：构建产物→build 目录（`.gitignore` 已覆盖）；探针→**不要落在仓库根**，用临时目录 |

### 决策树（Mermaid）

```mermaid
flowchart TD
  A[新文件] --> B{C++ 源码/测试?}
  B -- 源码 --> B1[core/src/层/]
  B -- 测试 --> B2[core/tests/]
  B -- 工具 --> B3[core/tools/]
  B -- 否 --> C{随包运行?}
  C -- 插件 --> C1[plugins/name/]
  C -- 框架/领域包 --> C2[ttbox_motion/ 领域包]
  C -- 否 --> D{被 FHS/发布门禁引用?}
  D -- 构建/发布/运维脚本 --> D1[scripts/  不可外移]
  D -- 部署描述 --> D2[deploy/  systemd|cmake|config]
  D -- 否 --> E{离线工具?}
  E -- 是 --> E1[tools/]
  E -- 否 --> F{测试?}
  F -- pytest --> F1[就近包内 tests/]
  F -- 跨模块/板端 --> F2[tests/]
  F -- 否 --> G{文档?}
  G -- 是 --> G1[docs/类别/]
  G -- 否 --> H[不入库: 构建产物/pycache/探针/凭据]
```

---

## 三、命名约定（**强制**）

| 对象 | 规则 | 现状示例 |
|---|---|---|
| 目录名（代码包） | 小写、`snake_case`、单数；包必有 `__init__.py` | ✅ `ttbox_motion/`、`plugins/` |
| 目录名（文档） | 小写英文 `kebab-case` 主题名（`architecture/`、`ops/`、`protocols/`、`performance/`…） | ✅ 本轮已建；旧 `架构/` 已迁移 |
| C++ 文件 | 类名同 `PascalCase.hpp/.cpp`；实现类与接口分离（`Xxx.hpp` / `Xxx_stub.cpp`） | ✅ `ICapture.hpp` / `V4L2Capture.hpp`、`PreviewModule_stub.cpp` |
| 插件 | `plugins/<name>/{bin/{ttbox-<name>, ttbox-<name>.py},config/,…}`；`bin/ttbox-<name>` 为 **bash launcher** | ✅ `plugins/web/bin/ttbox-web` |
| pytest 文件 | `test_*.py`，就近包内 `tests/` | ✅ `core/tests/`、就近包内 `tests/` |
| C++ 测试 | `test_*.cpp`（CTest 注册名 = 去掉 `test_`） | ✅ `core/tests/test_pipeline.cpp` |
| shell 测试 | `test_*.sh`；集成脚本按域分目录 `tests/<域>/` | ✅ `tests/api/`、`tests/monitor/` |
| 文档命名 | **活真源**：`主题.md`；**一次性报告**：`主题-YYYY-MM-DD.md`；**版本化**：`主题-vX.Y.Z.md` | ✅ `交付前Web实测报告-2026-09-19.md` |
| 日期格式 | `YYYY-MM-DD`（ISO，本地日） | ✅ |
| 私有/临时 | 一律不入库（`.testkeys/`、`_*`、`*.local.json`、`*.log`） | ✅ `.gitignore` 已覆盖 |

---

## 四、`docs/` 分类（目标树）

```text
docs/
├── README.md            # 文档总入口 + 唯一真源表 + 代码侧引用关系表
├── CONVENTIONS.md       # 本文件
├── ipc-protocol.md      # 旧路径指针桩（被 core/src/ipc/IpcServer.hpp 引用，保留）
├── protocols/           # 活·协议与规格（ipc-protocol / image-spec / config-path-env-registry / ota-server-contract）
├── research/            # 研究（OUTPUT_BACKEND_DESIGN.md）
├── build/               # 构建可复现（build-reproducibility.md）
├── handover/YYYY-MM-DD/ # ★历史交接（只追加，禁改写）
└── 主题-YYYY-MM-DD.md    # 一次性过程报告（仅当被源码/脚本/测试引用时入库）
```

**文档放哪的判定**：消费方是"协议实现者 / 构建维护者 / 运维" → 对应 `protocols|research|build/`；
历史交接 → `handover/`（**禁改写，只追加**）；被代码引用的过程报告 → `docs/` 根。

### 入库硬线（2026-09-19 起）

**一份文档是否入库，取决于有没有"非文档的引用方"** —— 源码、脚本、测试、systemd/CMake/HTML 都算。
没有任何代码引用的介绍类、盘点类、过程类文档一律不入库。

现状（2026-09-19 清理后）：`docs/` 下 17 份 + `deploy/DEPENDENCIES.md`
+ 根 `README.md`，共 **18 份**。谁引用谁见 [`README.md`](README.md) §二。
> **2026-10-01（P3）**：平台层包 `platform/` 已改名 `ttbox_platform/`（目录名 = Python 包名，
> 且不再遮蔽标准库 `platform`）；上文那份 README 随之改路径。
> **★ 2026-10-06（V1.0.52）**：`ttbox_platform/supervisor/README.md` 已随「去 Python」批次 1
> 与 `ttbox_platform/` 整个目录一起删除 ⇒ 服务用户/组约定的实际载体改为
> `scripts/ttbox_fhs_init.sh` 的建号段本身（`docs/README.md` 索引已同步）。

> **2026-09-18 清理**：`docs/archive/`（历史快照 / 旧性能报告 / 阶段报告，41 个文件）与四个旧路径指针目录
> （`架构/` `开发/` `规划/` `验证/`）已删除。
> **2026-09-19 清理**：按业主「不能影响程序源码运行，清理没用的」口径再清 **100 份 / 763 KB**，
> 覆盖 `architecture/`、`ops/`、`guide/`、`product/`、`web/`、`AI/`、`verification/` 的全部文档、
> `handover/2026-09-17/` 的 12 份、各模块与包的 21 份 README、`docs/` 根 10 份过程报告，
> 以及仓库根未跟踪的 `SOURCE_SNAPSHOT.md`。被清文件仍在 git 历史中。

### 旧路径指针规则

若某文档被**在库源码注释**按**旧路径**引用而**无法同步修改引用方**（如 `core/**` 不许改），
则在**旧路径留一个指针文件**（内容=一句"已迁移至新路径"+映射表），保证历史引用仍可解析。示例：`docs/ipc-protocol.md`。

### 链接自检（改名/移动文档后必跑）

```bash
python docs/check_links.py
# 递归覆盖 docs/** 全层（含 archive/）+ modules/** + 根 README.md；
# 输出 scanned_files / links_checked（仅仓库相对链接）/ absolute_file_uris（绝对 file:// URI，不计断链）/ broken_count
# 期望：broken_count=0
```

> `absolute_file_uris` 是历史快照里指向**旧工作区**（`G:\工作区\…`）的绝对 URI，属档案内容，不修不改写。

---

## 五、五档处置语义

| 档位 | 含义 |
|---|---|
| **保留** | 位置与职责都不动 |
| **移动** | 换位置但保留内容（`git mv`，保历史） |
| **合并** | 内容并入他处 |
| **归档** | 移入只读归档区（本机产物 = 仓库根 `.archive-2026-09-17/`），不再维护 |
| **删除** | 直接移除（2026-09-18 起：业主确认后，被取代的旧文档 / 旧路径桩 / 历史归档可直接删除，git 历史可追溯） |

> 早期纪律为"判删除一律改归档、供业主复核"；**2026-09-18 业主已复核并确认删除**，故 `docs/archive/` 与旧路径指针目录已移除。
> 本机产物归档区 = 仓库根 `.archive-2026-09-17/`（已在 `.gitignore`）。

---

## 六、配置 · 常量 · 路径口径基线（**无补丁红线**）

> **归属**：软件团队·工程师（寇豆码），Task #2。全量基线见
> [`handover/2026-09-17/配置常量路径口径-基线与整改方案-2026-09-17.md`](handover/2026-09-17/配置常量路径口径-基线与整改方案-2026-09-17.md)；
> **机器可校验登记表**（RUNTIME env allowlist + 跨语言同值常量）见
> [`protocols/config-path-env-registry.md`](protocols/config-path-env-registry.md)。
> **一条命令判定**：`bash scripts/ttbox_conventions_gate.sh`（退出码 0 = PASS；`--selftest` 含负向控制）。

四个维度、**单一真源（SSOT）** 原则：**一处定义，处处引用/派生**；跨语言（C++/Py/shell）打不通 include 时，
以**登记表 + 门禁同值断言**保证不漂移。**禁止**以注释/兼容读/双份默认值等"补丁"手法续命。

| 维度 | 真源 | 逐条规则（摘要） |
|---|---|---|
| **A·路径** | C++ `core/src/common/Paths.hpp`；Python `plugins/web/lib/paths.py`；shell 头部 `TTBOX_PREFIX` | `A-PATH-1` C++ 运行根唯一 = 编译期 `-DTTBOX_PROJECT_ROOT`；`A-PATH-2` 取值链 CLI > env > 头文件默认；`A-PATH-3` Python 用 `Path(__file__).parents[N]` 相对派生 + `append`（禁 `insert(0)`）；`A-PATH-4` shell 前缀参数化；`A-PATH-5` 同一路径字面量全仓单点定义；`A-PATH-6` 禁"注释声称仍生效"与构建机绝对路径进产物 |
| **B·常量** | 模块头/顶层常量 | `B-CONST-1` 单点定义；`B-CONST-2` 跨语言同值常量 = 唯一 C++ 头 + Py/shell 镜像 + 登记 + 门禁；`B-CONST-3` 版本三名分离（`kCoreVersion` / `kAppVersion` / `TTBOX_RELEASE_VERSION`）；`B-CONST-4` 时间/阈值单点；`B-CONST-5` 禁 `x or DEFAULT` / 吞错兜底 / 复制粘贴常量 |
| **C·配置** | 运行期真源 = 板端 `/etc/ttbox/config.d/`（`00-factory.json` ← `10-device.json` 深合并） | `C-CFG-1` 运行期配置真源唯一；写回目标唯一 = `config.d/10-device.json`；`C-CFG-2` 读取链 `--config` > `TTBOX_CONFIG` > 编译期默认；`C-CFG-3` **单一写入者 = Core**（Web 禁直读直写配置，一律走 IPC）；`C-CFG-4` 默认值唯一真源 = `deploy/config/00-factory.json`；`C-CFG-5` `config/default.json` 仅"本机开发样例"，其共享键必须与 00-factory **同值**（门禁断言） |
| **D·env** | `docs/protocols/config-path-env-registry.md` | `D-ENV-1` 前缀 `TTBOX_`，按 BUILD/RUNTIME/TEST 分类；`D-ENV-2` RUNTIME 必须登记 allowlist；`D-ENV-3` 同义异名归一（保留 1 名）；`D-ENV-4` 启动期归一为内部变量；`D-ENV-5` 禁 `X or Y` 双名兼容读 |

**新增/改动的硬性要求**：新增路径/常量/配置键/env **先登记**（`protocols/config-path-env-registry.md`）**再改码**；
`bash scripts/ttbox_conventions_gate.sh` 必须仍 PASS（`scripts/ttbox_release_verify.sh` 已接入）。
