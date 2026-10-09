# TTBOX 仓库编程语言占比报告（最终口径）

- **统计时间**：2026-10-06
- **统计范围**：`git ls-files` 跟踪的 903 个文件，剔除二进制/数据/资产
- **统计脚本**：`tools/lang_stats_final.py`（**唯一版本**，口径最严谨；仓库根从脚本自身位置派生，不硬编码路径，符合 `ttbox_conventions_gate.sh` 第⑪条）
- **代码行定义**：物理行，剔除空行与注释行；注释行单独统计
- **原始数据**：`docs/audit/lang_stats_final.json`

> 本文档经过三次口径修正才可靠。初版漏了三处：`core/third_party` 混入、`CMakeLists.txt` 扩展名误判、`core/tests` 2.7 万行测试代码未单独识别。详见「口径演进」。
>
> **配套审查**：`code-review-honor-code.md`（370 行）— 按八荣八耻对 16 个目录做的只读审查，总体评级「良好」，含 1 项高危安全问题与 2 项 P0 建议。

---

## 一、总览

| 分类 | 代码行 | 占比 | 文件数 |
|------|--------|------|--------|
| **产品代码** | 74,813 | 61.9% | 540 |
| **测试代码** | 27,035 | 22.4% | 186 |
| 第三方代码 | 12,921 | 10.7% | 10（onnxruntime） |
| 文档正文 | 5,496 | 4.6% | 95 |
| **总计** | **120,265** | 100% | |

**测试/产品比 = 36.1%** — 高于嵌入式 C++ 项目常见的 10~30% 区间，说明测试投入是充足的（这一点与初版「测试极薄」的判断相反，特此更正）。

---

## 二、产品代码语言占比（分母 = 74,813 行）

| 排名 | 语言 | 文件数 | 代码行 | 注释行 | 空行 | 占产品代码 |
|------|------|--------|--------|--------|------|-----------|
| 1 | **C++** (.cpp) | 199 | 29,472 | 6,575 | 3,382 | **39.4%** |
| 2 | **Python** | 149 | 18,908 | 1,986 | 3,356 | **25.3%** |
| 3 | JavaScript | 15 | 8,133 | 554 | 613 | **10.9%** |
| 4 | Shell | 86 | 7,175 | 2,084 | 891 | **9.6%** |
| 5 | CSS | 3 | 4,894 | 0 | 811 | **6.5%** |
| 6 | Text | 9 | 2,078 | 0 | 58 | 2.8% |
| 7 | HTML | 4 | 1,447 | 7 | 117 | 1.9% |
| 8 | JSON | 50 | 1,259 | 0 | 6 | 1.7% |
| 9 | CMake | 2 | 597 | 684 | 102 | 0.8% |
| 10 | C/C++ 头文件 (.h) | 4 | 279 | 0 | 49 | 0.4% |
| 11 | Systemd | 8 | 124 | 125 | 16 | 0.2% |
| 12 | Nginx配置 | 1 | 102 | 0 | 0 | 0.1% |
| 13 | Makefile | 1 | 100 | 0 | 16 | 0.1% |
| 14 | C (.c) | 1 | 97 | 16 | 8 | 0.1% |
| 15 | Lua | 6 | 76 | 105 | 23 | 0.1% |
| 16 | PowerShell | 1 | 52 | 23 | 4 | 0.1% |
| 17 | TOML | 1 | 20 | 6 | 4 | 0.0% |

### 分层视角

- **系统核心**（C++ + C + 头 + CMake）：**约 41%**
- **脚本编排**（Python + Shell + PowerShell）：**约 35%**
- **前端三件套**（JS + CSS + HTML）：**约 19%**
- **配置与其他**（JSON/Text/Systemd/Nginx/Makefile/TOML）：**约 5%**

---

## 三、测试代码语言占比（分母 = 27,035 行）

| 语言 | 文件数 | 代码行 | 占测试代码 |
|------|--------|--------|-----------|
| **C++** | 96 | 16,430 | **60.8%** |
| **Python** | 77 | 9,726 | **36.0%** |
| JSON | 4 | 441 | 1.6% |
| Shell | 9 | 438 | 1.6% |

测试代码主要语言是 C++（60.8%），与产品主力语言一致 —— **测试在测真正的产品代码，不是另起一套**。这是健康信号。

---

## 四、按目录分布

### 产品代码

| 目录 | 代码行 | 主导语言 | 性质 |
|------|--------|----------|------|
| `core/` | 26,690 | C++ 24,376 | 产品核心（推理/采集/瞄准/IPC/授权） |
| `plugins/` | 23,816 | Python 9,057 / JS 8,133 / CSS 4,894 | 插件层（web/ai/fan/model/log…） |
| `scripts/` | 9,124 | Python 4,585 / Shell 4,379 | 运维与构建脚本 |
| `usbproxy/` | 5,942 | C++ 5,096 | USB HID 代理（独立 Makefile） |
| `image/` | 4,228 | Shell 2,393 | 整机镜像烘焙 |
| `tools/` | 1,740 | Python 1,740 | 转换工具（ONNX→RKNN 等） |
| `framework/` | 977 | Python 977 | 框架层 |
| `docs/` | 628 | JSON 229 / Python 223 | 文档配套脚本 |
| `ttbox_motion/` | 519 | Python 519 | 动作模块 |
| `ttbox_platform/` | 494 | Python 494 | 平台抽象 |
| `deploy/` | 478 | JSON 137 / Systemd 124 | systemd + Nginx + CMake |
| `config/` | 137 | JSON 137 | 配置 |

### 测试代码

| 目录 | 代码行 | 构成 |
|------|--------|------|
| `core/tests/` | 17,000 | C++ 15,920 / Python 639 / JSON 441 |
| `plugins/web/tests/` | 7,427 | Python 7,427 |
| `tools/` | 581 | Python 581 |
| `tests/`（顶层） | 541 | Shell 438 / Python 103 |
| `usbproxy/tests/` | 510 | C++ 510 |
| `framework/tests/` | 509 | Python 509 |
| `ttbox_platform/tests/` | 467 | Python 467 |

---

## 五、代码量最大的源文件（产品代码 Top 15）

| 代码行 | 语言 | 文件 |
|--------|------|------|
| 5,470 | JavaScript | `plugins/web/static/panel/10-flow.js` |
| 4,529 | CSS | `plugins/web/static/panel.css` |
| 1,564 | C++ | `usbproxy/proxy.cpp` |
| 1,175 | Python | `tools/converter/convert_onnx_to_rknn.py` |
| 1,047 | C++ | `core/src/ipc/IpcServer.cpp` |
| 1,001 | HTML | `plugins/web/templates/index.html` |
| 941 | Python | `scripts/ttbox_m207_accept.py` |
| 940 | C++ | `core/src/model/ModelRegistry.cpp` |
| 855 | C++ | `usbproxy/mouse_control.cpp` |
| 841 | C++ | `core/src/app/Application.cpp` |
| 817 | C++ | `core/src/model/RuntimeProfile.cpp` |
| 784 | JavaScript | `plugins/web/static/motion_training.js` |
| 729 | C++ | `core/src/rknn/DecodeNMS.cpp` |
| 719 | C++ | `core/src/aim/AimThread.cpp` |

`core/src/` 下超 500 行的 .cpp 共 16 个，最大 `IpcServer.cpp` 1,240 行。

---

## 六、口径演进（三次修正，供复现参考）

| 版本 | 脚本 | 修正内容 | 产品代码行数 |
|------|------|----------|-------------|
| v1 | `tools/lang_stats.py` | 朴素扩展名统计 | 113,824（不可用） |
| v2 | `tools/lang_stats_refined.py` | 剥离 `core/third_party`；`CMakeLists.txt` 归CMake；无扩展名脚本按 shebang 判定 | 101,848（仍含测试） |
| **v3** | `tools/lang_stats_final.py` | **测试代码单独成桶**（`core/tests` 2.03 万行此前被当成产品代码） | **74,813** |

三处最容易算错的地方：

1. **`core/third_party/onnxruntime`**：10 个文件、12,921 行，主要是 ORT API 头文件。不剥离会严重虚高 C++ 占比（该目录注释密度极高）。
2. **`CMakeLists.txt` 没有 `.cmake` 扩展名**：本仓库 `core/` 和 `deploy/` 各有一个，朴素按扩展名统计会漏掉。
3. **测试代码藏在子目录**：`core/tests/`（2.03 万行）、`plugins/web/tests/`（7,427 行）等 6 个测试目录，若只看顶层 `tests/`（541 行）会低估测试规模 50 倍。

未计入：`.onnx` 模型、`.dtb` 设备树、`.jpg`/`.bgr` 素材、`.hex` 固件、`.out` 构建产物。

---

## 七、客观质量指标（本次一并核验）

| 指标 | 数值 | 判断 |
|------|------|------|
| C++ `core/src/` 裸 `new`/`delete` | **0 处** | ✅ 无手动内存管理 |
| 智能指针使用（`unique_ptr`/`shared_ptr`） | 117 处 | ✅ RAII 已普及 |
| 互斥锁（`std::mutex`/`lock_guard`） | 78 处 | ✅ 并发有保护 |
| 线程创建（`std::thread`/`pthread_create`） | 17 处 | — |
| `malloc`/`calloc`/`free` | 13 处 | ⚠️ 需逐个确认配对 |
| Shell 脚本含 `set -e` | 30 / 85（35%） | ⚠️ **55 个脚本无失败即停** |
| Shell 中 `rm -rf` | 48 处 | ⚠️ 需确认路径安全性 |
| 前端 `eval(`/`innerHTML` | 28 处 | ⚠️ 需甄别 XSS 风险 |
| CSS `!important` | 5 处 | ✅ 可接受 |

---

## 八、对照「代码全 C++」目标

最近提交 `d246609`（V1.0.46）声明「代码全 C++，Python 只做脚本编排」。

| 判据 | 数据 | 结论 |
|------|------|------|
| C++ 是否第一语言 | 产品代码 39.4%，测试代码 60.8%，均居首 | ✅ 达成 |
| Python 定位 | 产品 25.3%（18,908 行），测试 36.0% | ⚠️ 需确认是否符合"脚本编排"定位 |
| 是否存在双轨 | `plugins/` 下Python 9,057 行 + JS 8,133 行；`core/` 下 Python 1,293 行 | ⚠️ 存在 C++/Python 混合区|

**待用户确认**（不臆想业务，需你给判据）：
1. `plugins/web/bin/ttbox-web.py`（1,133 行）、`api_v1.py`（882 行）这类 Web 后端，算「脚本编排」还是「产品代码」？
2. `core/` 内 1,293 行 Python（pid_sim 工具、usb_diag 等）的定位？
3. 若目标是"C++ 为主"，当前 39.4% 是否达标，还是要求 80%+？

这个判定会直接改变 C++ 占比的量级，建议先对齐再动手改。

---

*复现命令：`python tools/lang_stats_final.py`*

---

## 附录A：口径对照（净代码行 vs 物理行）

本报告统一用**净代码行**（剔除空行与注释）。若他人用 `wc -l`（物理行）对照，数字会偏大，尤其测试目录注释密度高：

| 位置 | 物理行（`wc -l`） | 净代码行（本报告） | 折算 |
|------|-----------------|------------------|------|
| `core/tests/*.cpp` | 20,332 | 15,920 | 78% |
| `plugins/web/tests/*.py` | 10,515 | 7,427 | 71% |
| Python 测试合计 | ~13,400 | 9,726 | 73% |
| `10-flow.js` | 6,195 | 5,470 | 88% |
| `panel.css` | 5,284 | 4,529 | 86% |
| `IpcServer.cpp` | 1,240 | 1,047 | 84% |

对照时的差异来源：**注释行 + 空行**。C++ 测试注释占2,889 行（注释率约 14%），比产品代码高。

## 附录 B：测试规模详表（物理行口径）

审查中确认的测试资产规模，此处用物理行以便与 CI 输出一致：

| 位置 | 规模 | 备注 |
|------|------|------|
| `core/tests/*.cpp` | 20,332 行 / 92 文件 / 417 个 `TEST()` / 51 个 `add_test` | C++ 单测主力 |
| `plugins/web/tests/` | 10,515 行 / 49 文件 / 505 个测试函数 | Web 面板测试 |
| `ttbox_platform/tests` + `framework/tests` + `tools/ota` | 1,933 行 | 平台/框架 |
| **Python 测试合计** | **约 13,400 行 / 78 文件** | — |

HEAD 提交自报 `ctest 44/44；pytest 789 passed`。仓库有 **6 道架构门禁** 防退化，其中 `test_tools_python_is_orchestration_only.py` 有 10 条断言，明确禁止 Python 影子控制器 —— 这是"代码全 C++"目标被**自动化守卫**而非人工纪律维护的证据。