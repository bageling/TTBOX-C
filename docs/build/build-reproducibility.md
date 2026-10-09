# 出货构建可复现性 SOP（T1.15 交付件 · 2026-09-17）

> 本工作区路径 `build-reproducibility.md` = 仓库目标路径 `docs/build/build-reproducibility.md`。
> **依据**：`tasks.md` T1.15 · `t1.16-impl-spec.md` §6/§8 · `m1-acceptance-checklist.md` §1/§2 · `lib-scope-ruling.md` §11（随包 ELF 清单）。
> **目的**：**没有可复现的出货构建，就没有可交付的产物**——本 SOP 是 T1.07b「重建产物」与 T1.13「板端验收」的共同前提。

---

## §0 ★ 现实修正与纪律（先读，否则会照旧文档做错）

1. **真实构建目录已变更为 `build-aarch64-t114/`**（承载 T1.14 相关构建）。
   - `build-aarch64/` 是 **09-15 20:22 旧物**（产物 md5 `64b3226aab5798926b3872273d533bfd`），**仅作对照/废弃**，**不再是出货构建目录**。
   - 本 SOP 与验收清单里凡出现 `build-aarch64-t114/...` 均指**当前真实构建目录**。
2. **可复现性已实测 bit-for-bit 成立（同源、**同父目录下不同构建目录**）**：**同源（同 commit + 同工具链 + 同 sysroot）在不同构建目录 clean build，产物 `ttbox_core_main` 的 md5 与 BuildID 均一致**（**正样本**：`build-aarch64-t114`〔01:17:16〕与 `build-aarch64-t118`〔01:09:22〕**字节相同**，同 md5 `2494f8dd…` / sha256 `d20b25da…`）⇒ **无需** `-ffile-prefix-map` 等额外手段。**★ 判据前提：先证同源、再比字节；不同源哈希不得并列比较**（同清单 §0 规则 5(d)「计数必须同域」）。
   - ⚠️ 请**不要**再写"若工具链无法完全 bit-for-bit 则退而求其次（比符号表/段布局）"——该退路**不适用**（正样本已一致）。
   - **★ 口径边界（防把不同源哈希当"复现失败"）**：**只在"同源"前提下声称"字节可复现"**——`626a3b00…`（00:33）与 `c28ab675…` 是 **T1.18 之前**的产物、**现已不在盘上**；`d20b25da…` 是 **T1.18 之后**的产物（T1.18 加了 `target_compile_definitions(... HAS_JPEG=1 HAS_OPENCV=1)` ⇒ 编译命令变了）⇒ **三者不同源、根本不具可比性**，**不得并列称"复现失败"**。口径冻结见 **§13**（候选 A/B/C）；**判据以候选 C（同源闭包 + 形态 + NEEDED 闭集）为主**。
3. **★ 命名空间纪律（2026-09-17 补）**：本仓存在**两套 `A##` 编号、同形不同域**——**契约域** = `m1-acceptance-checklist.md` 的**全部条目**（`A#`，含 `A19a/A19b`、`A27a/A27b` 等子项；条目数随清单演进，以 §9/台账行为准）；**本地自测域** = `scripts/ttbox_release_selftest.sh` 的 `selftest A##`。**引用脚本编号一律写 `selftest A##`**（脚本头已声明、结尾已打印 `NAMESPACE:` 行）；**裁决 = 不重命名**（改名会让既有 QA 证据与脚本对不上）。详见 `m1-acceptance-checklist.md` §0。
4. **★ 跨仓引用纪律（2026-09-17 补 —— 答"`NAMESPACE:` 行该不该带路径"）**：**`NAMESPACE:` 行不承载文件系统路径**（它是**日志归属标签**、不是文档索引）。**禁止**用看似**本仓相对路径**的写法引用**另一仓**文件——脚本端 `ttbox_release_selftest.sh:260` 曾写 `docs/deploy/m1-acceptance-checklist.md`，该路径**在代码仓根本不存在 ⇒ 悬空**。**规范写法 = 带仓名前缀的逻辑路径**（如 `ttbox-vs-yu-program/m1-acceptance-checklist.md`），且**不带任何条数上界**（原 `A1–A32` 属硬编码陈旧计数，已去）。详见 `m1-acceptance-checklist.md` §0 规则 4。
5. **★ 计数锚定纪律（2026-09-17 补 —— 本轮第 4 处陈旧计数的根因收口）**：**禁止裸计数**（"26 个 CTest""14 项"这类**孤立数字 + 量词**）。每个计数**必须**为 **(a) 现势值**（含 `命令` + `时点` + `平台`）、**(b) 历史值**（含时点；**历史报告不得改写成现势值**）、**(c) 不写死**（`A##` / `<N 随…演进>` / `以 <权威来源> 为准`）三者之一。**CTest 条数权威锚 = `ctest -N`（Linux host）**。详见 `m1-acceptance-checklist.md` §0 规则 5。

---

## §1 环境前置

| 项 | 要求 |
|---|---|
| 宿主 | WSL 发行版（**版本钉死于 §6 工具链表**） |
| 交叉编译器 | `aarch64-linux-gnu-g++`（验证：`aarch64-linux-gnu-g++ --version`） |
| 构建器 | `ninja`（验证：`ninja --version`） |
| sysroot | `sysroot-aarch64/`（**来源须钉死并记录**，见 §5/§6） |
| 版本钉死 | WSL 发行版 + 交叉工具链版本 + sysroot 来源，三者**全部记入 §6 工具链表**，换机器按文档可重建 |

---

## §2 固定配置（强制，不得省略）

| 配置 | 值 | 说明 |
|---|---|---|
| `TTBOX_CORE_BUILD_AUTH` | **`OFF`** | **出货强制 AUTH=OFF**（M1 无在线客户端，NullClient fail-open） |
| 交叉工具链 | `aarch64-linux-gnu-g++` | 见 §1 |
| sysroot | `-DCMAKE_SYSROOT=${SYSROOT}`（指向 `sysroot-aarch64`） | 见 §5 |
| 构建目录 | `build-aarch64-t114/` | **见 §0-1** |
| 生成器 | `-G Ninja` | |

---

## §3 构建命令（可复现）

```sh
# 0) 变量
SRC=<仓库根>
SYSROOT="$SRC/sysroot-aarch64"
BUILD="$SRC/build-aarch64-t114"

# 1) clean（确保可复现：删净旧构建目录）
rm -rf "$BUILD"

# 2) configure（固定 AUTH=OFF + 交叉工具链 + sysroot + Ninja）
cmake -S "$SRC/core" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTTBOX_CORE_BUILD_AUTH=OFF \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DCMAKE_SYSROOT="$SYSROOT"

# 3) build
cmake --build "$BUILD" -j
```

> 一键脚本 `scripts/ttbox_build_release.sh`（**归工程师实现**）须把 §3–§9 全流程内置。

---

## §4 产物路径

| 产物 | 路径 |
|---|---|
| core 可执行 | `build-aarch64-t114/ttbox_core_main` |
| 留档 | `build-aarch64-t114/RELEASE_BUILD.md`（字段见 §10） |
| payload `bin/` 闭集 | **`{ ttbox_core_main }`**（**恰一个文件**，见 §8） |
| payload `lib/` | **release 作用域库**：**只有 `librknnrt.so`**（**源 = 链接期 sysroot 那份**，`d31fc19c…` / **2.3.2**；**甲案驳回**、**禁止出货仓库 `lib/librknnrt.so`**〔1.5.2 过期残留〕——见 `lib-scope-ruling.md` §6）；`librga`/`libjpeg`/**`libopencv*`** 属**基础镜像作用域**、**不打包**（见 §9 与 `lib-scope-ruling.md` §3/§11） |

---

## §5 md5 留档格式

每次出货构建写一行（供 `RELEASE_BUILD.md` 与仓库外备份共用）：

```
<commit_sha> | <toolchain_version> | <sysroot_source+version> | <product_path> | md5=<md5> | BuildID=<buildid> | <build_time_utc> | <build_host> | <strings_gate_result>
```

- 示例（示意）：
  ```
  abc1234 | aarch64-linux-gnu-g++ (GCC) X.Y.Z | sysroot-aarch64@<src> rev <n> | build-aarch64-t114/ttbox_core_main | md5=<32hex> | BuildID=<hex> | 2026-09-17T00:00:00Z | wsl-ubuntu | third=0/ip=0/cred=0
  ```
- **备份**：新产物 + md5 **备份到仓库外**，与旧 `64b3226a…` **隔离、不覆盖**。

---

## §6 工具链 / 依赖版本表（留档必录）

| 组件 | 版本 | 获取命令 |
|---|---|---|
| WSL 发行版 | `<填写>` | `cat /etc/os-release` |
| `aarch64-linux-gnu-g++` | `<填写>` | `aarch64-linux-gnu-g++ --version` |
| `ninja` | `<填写>` | `ninja --version` |
| CMake | `<填写>` | `cmake --version` |
| sysroot 来源 | `<填写>`（rK vendor / 板厂 SDK / 自建） | 记录来源与 rev |
| **sysroot `librknnrt.so`** | **`2.3.2`（2025-04-09），sha256 `d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8`** | `strings -a sysroot-aarch64/usr/lib/librknnrt.so \| grep -i 'librknnrt version'` + `sha256sum` |
| **sysroot `libopencv_{core,imgproc}.so`** | **`4.5d`**（板端硬 NEEDED，基础镜像作用域） | `ls sysroot-aarch64/usr/lib/aarch64-linux-gnu/libopencv_core.so*` |
| **sysroot `libjpeg.so`** | `<填写>`（基础镜像作用域） | `ls sysroot-aarch64/usr/lib/aarch64-linux-gnu/libjpeg.so*` |

> 本表由工程师在首次出货构建时**实际填入**，之后不得只改版本不改来源。
> **★ `librknnrt` 版本锚（`lib-scope-ruling.md` §6 改动点 5）**：`2.3.2` / `d31fc19c…` 是"链接期 = 模型 = 板上"三重一致的**基准值**；换机器若取到别的版本，即为**不可复现**，须停止出货。

---

## §7 字符串表硬门禁（三档 · 构建脚本内置、留档必录）

| 档 | 判据 | 处置 |
|---|---|---|
| **(a) 第三方域名** | `strings <产物> \| grep -Ec 'antszy\|blpro\|blpt'` **== 0** | **硬 FAIL** |
| **(b) 凭据默认值** | `grep -nE 'client_secret_[[:space:]]*=[[:space:]]*"[^"]+"' core/src/auth/TtboxLicenseClient.hpp` **== 0** | **硬 FAIL**（源码门禁；env **名**合法保留。**★ 2026-09-17 订正：原正则 `TTBOX_CLIENT_SECRET[^)]*"[^"]+` 对 `getenv("TTBOX_CLIENT_SECRET")` 恒假 FAIL，已替换，见 `m1-acceptance-checklist.md` A3 通则**） |
| **(c) 自有端点** | `strings <产物> \| grep -c '38\.127\.133\.6'` = **登记值** | **不判 FAIL** |

**★ (c) 档说明（写死防误设）**：`38.127.133.6:10039` 是**我方自有授权服务器、合法默认端点**，**不是缺陷**。**M1 期望 `0`**（T1.07a **只删第三方 `AiboxLicenseClient`、保留我方 `TtboxLicenseClient`**；M1 = NullClient 不接入 ⇒ IP 字面量随 ODR-use 消失）；**`>1` 告警**；**`==1` 只可能出现在 T2.x**（装回在线客户端后）⇒ **不得把 `==1` 设成 M1 门禁**。原"`38.127.133.6 ==0 为 FAIL`"判据**错误已废**；原"`TTBOX_LICENSE_SERVER|TTBOX_APP_KEY|TTBOX_CLIENT_SECRET` 命中==0"判据**错误已废**（三者是 env 变量名）。

> **★ (c) 档权威表述（2026-09-17 定；两式择一后选定"打明允许 1"式，理由在末）**——直接抄进脚本注释即可，**避免"跨文件推导"**：
> ```sh
> # 自有端点计数：M1 期望 0；T2.x 允许 ==1；>1 告警；★ 不判 FAIL（tasks.md:160 ⑤）
> ```
> **为何不选 `≡0` 首行断言式**：`≡0` 易被后人误读为"**永远 0**"（从而在 T2.x 装回在线客户端后**错误 FAIL**）；"打明允许 1"把「**M1 期望 0 / T2.x 允许 `==1` / `>1` 告警 / **不判 FAIL**」整句写进注释，与 `tasks.md:160 ⑤` 裁决**同源**、**抗腐**、无需再跨文件推导。

**任一 (a)/(b) 命中 ⇒ 构建脚本非零退出。**

---

## §8 payload 收集：白名单闭集（禁通配）

- **禁止 `build-aarch64*/` 通配**收集 payload（会把 `ttbox_core_main.bak-20260916` 等**发到板上**）。
- **payload `bin/` = 显式白名单闭集 = `{ ttbox_core_main }`**（**恰一个文件**）；排除 `*.bak-*`/`*_backup*`/`CMakeFiles/`/`*.o`/`*.a`/`*.cmake`。
- `imgdetect`/`ipc_ping`/`hardware_runner_main` **不随货**（开发/产线/验证工具）；T1.13 ⑨ 需要时**临时投放**（`scp` + `LD_LIBRARY_PATH=/opt/ttbox/current/lib`，见 `t1.16-impl-spec.md` §6.3）。
- **★ 随包 ELF 逐个登记（2026-09-17 补，见 `lib-scope-ruling.md` §11）**：`bin/` 闭集只声明"哪些文件进 payload"，**不声明每个 ELF 的运行期依赖闭集**。`payload` 内**每个随包 ELF（可执行或共享库）**（**已知三个**，来源 = QA **真实物化 payload + `readelf -d`**：`bin/ttbox_core_main`〔exec〕、`lib/librknnrt.so`〔shared obj〕、`usbproxy/usb-proxy`〔exec〕；payload 共 **153 文件**、可执行 ELF **恰 3 个**）须在 `lib-scope-ruling.md` §11「随包 ELF 清单」登记四元组（随包来源 / 期望 RUNPATH 形态 / NEEDED 作用域分类 / 板端确认项）；**新增随包 ELF 未登记 ⇒ 判 FAIL**。
- 建议形式：
  ```sh
  for b in ttbox_core_main; do install -m 0755 "$BUILD/$b" "$STAGE/bin/"; done   # 闭集，禁通配
  ```

---

## §9 构建后自检（host 侧；交叉产物 host 不能 `ldd`）

> **关键约束**：aarch64 产物在 x86 host 上**不能执行 `ldd`** ⇒ **host 自检只用 `readelf`（静态）**；**真正的 `ldd` 运行期解析归板端 T1.13**。

```sh
STAGE="$BUILD/.selfcheck"; rm -rf "$STAGE"; mkdir -p "$STAGE/bin" "$STAGE/lib"

# (1) 白名单拷可执行（禁通配）
for b in ttbox_core_main; do install -m 0755 "$BUILD/$b" "$STAGE/bin/"; done

# (2) 拷 release 作用域库（★ 只 librknnrt；librga/libjpeg/libopencv 属【基础镜像作用域】由板端系统提供，不拷）
#     ★ 铁律（lib-scope-ruling.md §6）：所拷那份必须与【链接期 RKNNRT_LIBRARY】逐字节同源（否则"新 bin 配错版库"）：
install -m 0644 "$SYSROOT/usr/lib/librknnrt.so" "$STAGE/lib/"   # 乙案（唯一可行；甲案已驳回）——须与链接期同源（2.3.2 / d31fc19c…）
LINKED="$(grep -aoE '/[^:"]*librknnrt\.so' "$BUILD/CMakeCache.txt" | head -1)"
[ -n "$LINKED" ] && { cmp -s "$LINKED" "$STAGE/lib/librknnrt.so" || { echo "SELFCHECK FAIL: librknnrt 与链接期不同源"; exit 1; }; }

# (3) 自检 A：RUNPATH 逐段（复用 T1.01 verify；非 ELF 自动跳过）
bash scripts/ttbox_release_verify.sh --bin-dir "$STAGE/bin" --lib-dir "$STAGE/lib" \
  || { echo "SELFCHECK FAIL: RUNPATH/闭集"; exit 1; }

# (4) 自检 B：NEEDED 覆盖（防漏拷库）
#     ★ librga/libjpeg/libopencv 属【基础镜像作用域】、不拷 ⇒ 必须列入白名单，否则误报 WARN（见 lib-scope-ruling.md）
for b in "$STAGE"/bin/*; do
  readelf -d "$b" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' | sort -u | while read -r need; do
    case "$need" in
      libc.so*|libm.so*|libstdc++.so*|libgcc_s.so*|ld-linux*) : ;;
      librga.so*|libjpeg.so*|libopencv_*.so*) : ;;    # ★ libopencv_* 属基础镜像作用域（4.5d）
      *) [ -e "$STAGE/lib/$need" ] || echo "SELFCHECK WARN: $b 需要 $need 但 lib/ 未打包" ;;
    esac
  done
done
```

- 自检失败 ⇒ **非零退出**（与 §7 字符串表门禁并列，发布前双重闸门）。
- **自检 C（连带给 T1.13 ⑨）**：`/opt/ttbox/current/lib/` 存在且含 `librknnrt.so`（**不含 `librga.so`/`libopencv*`**——基础镜像作用域，由系统提供；见 `m1-acceptance-checklist.md` A9 与 `lib-scope-ruling.md`）。

---

## §10 `build-aarch64-t114/RELEASE_BUILD.md` 字段定义

> 实际留档文件由**工程师产出**；本节只定义**字段**（构建脚本须按此写入）。

| 字段 | 必填 | 说明 |
|---|---|---|
| `commit` | ✅ | 构建所用 commit sha |
| `toolchain` | ✅ | 交叉编译器版本（§6） |
| `sysroot` | ✅ | 来源 + 版本/rev |
| `build_host` | ✅ | WSL 发行版 + 主机标识 |
| `build_time_utc` | ✅ | ISO8601 UTC |
| `configure_args` | ✅ | §3 完整 configure 命令 |
| `product_path` | ✅ | 产物路径 |
| `md5` | ✅ | 产物 md5 |
| `buildid` | ✅ | ELF BuildID（可复现佐证：与再次构建一致） |
| `strings_gate` | ✅ | §7 三档扫描结果（`third=<n>/ip=<n>/cred=<n>`） |
| `selfcheck` | ✅ | §9 结果（RUNPATH 逐段 / NEEDED 覆盖 PASS/FAIL） |
| `repro_verify` | ✅ | 两次 clean build 的 md5 + BuildID 一致性结论 |
| `librknnrt_version` | ✅ | 随包 `librknnrt.so` 版本（须 `2.3.2`）+ sha256 |
| `notes` | ○ | 差异/异常说明 |

---

## §11 usbproxy 预编译二进制「诚实性」（T1.15 · T1.06）

> **背景**：`usbproxy/usb-proxy`（预编译 ELF，**134160 B**，sha256 `bf8b98c6…`）**直接入 git**，**不随 core 构建**（独立 `usbproxy/Makefile`）。⇒ 它的"可复现性"必须**另行钉死**，否则入库二进制可能来自**未知源码版本**（板上跑的是没人能复现的代码——比"没有二进制"更危险）。**对应验收条目 = `m1-acceptance-checklist.md` A30。**

| 查 | 判据 | 期望 | FAIL |
|---|---|---|---|
| **① sha256** | `cd usbproxy && sha256sum -c usb-proxy.sha256` | `usb-proxy: OK`（= `bf8b98c6d4bbdd86bcb122d6bfef62d4734a06524c675a6c3409fb9e3efcff25`） | `FAILED` / `.sha256` 缺失 |
| **② 可重建** | `cd usbproxy && make CXX=aarch64-linux-gnu-g++ clean all checksum && sha256sum usb-proxy` | 重建成功 **且** sha256 == 入库值（理想） | **重建失败**（硬 FAIL）；sha256 不一致 → 见下方退路 |
| **③ 无旧路径** | `strings -a usbproxy/usb-proxy \| grep -E '/opt/ttbox/(src/)?usbproxy\|/opt/ttbox/usbproxy'` | **无输出** | 任何输出（二进制编译自旧目录） |

**★ ② 的退路（sha256 不一致时，不许只校 sha256 不验来源、也不许直接判 FAIL 了事）**：若因**工具链/依赖版本漂移**（`aarch64-linux-gnu-g++`、`libusb-1.0`/`lua5.4`/`jsoncpp`）导致重建 sha256 ≠ 入库值 ⇒ **P2 三件套**：① **重建必须成功**（硬要求，证明源码能产出该二进制）；② **功能等价**（`usbproxy/examples/*.lua` 注入用例或等价冒烟）；③ **重建记录留档**（工具链 + 依赖版本 + 重建 sha256 + 差异原因）写入本 SOP。**门槛澄清**：「可重建」是硬要求；「sha256 逐位一致」是理想。

**实测留档（2026-09-17）**：实算 sha256 = `bf8b98c6d4bbdd86bcb122d6bfef62d4734a06524c675a6c3409fb9e3efcff25`（与 `usb-proxy.sha256` **一致 ✅**）；大小 **134160 B ✅**；`/opt/ttbox/src/usbproxy`、`/opt/ttbox/usbproxy`、`/opt/ttbox`、`/home/`、`/mnt/c/` 各 **0 命中 ✅**（二进制内绝对路径仅 `/dev/raw-gadget`、`/lib/ld-linux-aarch64.so.1`、`/run/ttbox-mouse-passthrough/{cmd,event}.sock`，均运行期合法）。

---

## §13 ★ V4 可复现性裁决框架（候选 A/B/C；2026-09-17 架构师备 · **★ V4 判据已立＝下「V4 判据（冻结条）」，候选 A/B/C 为其证据分层**）

> **背景（同源可复现正样本）**：`build-aarch64-t114`(01:17:16) 与 `build-aarch64-t118`(01:09:22) **字节相同**（同 md5 `2494f8dd…` / sha256 `d20b25da…`，884736 B）⇒ **同源（同 commit + 同工具链 + 同 sysroot）跨构建目录可字节复现，有正样本**。**★ 判据前提：先证同源、再比字节；不同源哈希不得并列比较**——`626a3b00…`（T1.18 前）/`c28ab675…` 与 `d20b25da…`（T1.18 后）**不同源**（T1.18 加了 `target_compile_definitions(... HAS_JPEG=1 HAS_OPENCV=1)` ⇒ 编译命令已变），**不可比**。⇒ **"能否复现""复现到哪一层"仍须先定义清楚**，否则"可复现"是可被任意解释的空话。
> **为何先定口径**：M1 交接文档 §5 待办 1 = "重编证明可复现"——**这是 M1 门面**；口径含糊则无法判 PASS/FAIL、无法进门禁。

### 三候选对照

| 候选 | 判据 | 代价 | 可验性（怎么测） | 能否对外声称"可复现" | 风险 |
|---|---|---|---|---|---|
| **A. 同路径字节可复现** | **清空同一构建目录** → 重建 → `md5sum`/`sha256sum` **逐字节相同** | **低**（默认即满足） | `rm -rf $B && cmake … && md5sum`（同路径两次） | ✅ 可（**限定"同路径"**） | 换路径/换机器即可能不成立 ⇒ 声称范围须限定 |
| **B. 跨构建目录字节可复现（同父目录）** | **同父目录下不同构建目录** clean build → 产物**逐字节相同** | **低**（**正样本已证无需额外手段**；`-ffile-prefix-map` **仅在跨父目录/换机器时才可能必要**，见下「旧 B 降级注」） | 两个不同构建目录 → `cmp -s`（**已有正样本 `t114 ≡ t118`**） | ✅（"同一 checkout 下不同构建目录可复现"） | 若扩展到**跨父目录/跨机位**：路径进产物（`__FILE__`/`-I`）⇒ 需额外归一（**旧 B 口径，已降级**） |
| ~~**B(旧). 跨父目录/跨机位字节可复现**~~（**降级**） | 不同**父目录/绝对路径** clean build → 逐字节相同 | **中**（须 `-ffile-prefix-map` 归一 `__FILE__`/`-I` 绝对路径 + 钉死工具链/sysroot 时间戳） | 换父目录/换机位构建 → `cmp -s` | ⚠️ 仅当举证后才可 | 正样本（同父目录）**已证不需要**额外手段 ⇒ 降级为「可选加固」；若其它绝对路径源未全归一 ⇒ **假达标** |
| **C. 不以字节为判据** | 改判 **"同源闭包 + 形态 + NEEDED 闭集"**：同 `commit` + 同工具链/sysroot/configure ⇒ ① 依赖闭包相同 ② RUNPATH 形态 A ③ `readelf -d` NEEDED 集相同 | **低**（不追字节） | 三断言全绿（**与 §0 规则 6 同口径**：判据 = 形态/闭包，非哈希） | ⚠️ 有限（只能说"**同源可复现**"，**不能说"位相同"**） | 对外若被读成"bit-for-bit" ⇒ 过度声称；但**最抗腐**（不为字节差异反复返工） |

### 与 §0 规则 6 的关系（**信号：答案偏 C**）

`m1-acceptance-checklist.md §0 规则 6` 已立：**产物指纹锚（md5）必须带时点、且不得作判据，判据 = RUNPATH 形态**。⇒ **本 SOP 的"可复现"判据宜与规则 6 同口径 = 候选 C 为主**，**A/B 作为分层附加证据**。

### ★ V4 判据（冻结条 · `m1-acceptance-checklist.md §0` 规则 6 的**姊妹条**，2026-09-17 立）

> **与规则 6 的关系**：规则 6 管**产物指纹锚**（md5 必须带时点、**不得作判据**，判据 = RUNPATH 形态）；本条管**"可复现"本身的判据**。两者同构：**都不许拿"一个数字"当判据，都必须把条件写清**——故为**姊妹条**。

**V4 判据（正式 · 冻结）：**

> **同源码（同 commit）+ 同配置向量（`CMAKE_TOOLCHAIN_FILE` / `CMAKE_SYSROOT` / `CMAKE_BUILD_TYPE=Release` / **`-DTTBOX_PROJECT_ROOT`（必需、显式）** / `-DTTBOX_SHIP=ON` / 交叉开关，**逐项显式给出**）⇒ 产物逐位相同（`md5`/`sha256` 相同），且与构建目录路径无关。**
>
> **★ 为何判据是"配置向量"而非"同 CMake cache"（2026-09-17 订正）**：**"cache 相等"不足以判可比**——两份 cache **都缺** `TTBOX_PROJECT_ROOT` 时，**cache 记录是一致的（都是空）**，但产物**因源码检出路径不同而不同**（默认值 = `get_filename_component("${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)` 被编进 `kDefaultConfigPath`）。⇒ **按 cache 判可比，会把不该比的两个产物判成可比**（判据自身失效，即本轮 P0 陷阱）。**可比性判据只能是"逐项显式给出的配置向量"，不能是"cache 文件相等"。**

- **正样本（证"与构建目录路径无关"这一半）**：`build-aarch64-t114` ≡ `build-aarch64-t118`（**不同构建目录**、同父目录、同 commit/工具链/sysroot ⇒ 字节相同）。
- **★ 禁止声称**："**跨任意配置逐位相同**"——**不同 configure / 编译定义 / 检出路径 ⇒ 不同源 ⇒ 不可比**。**三个哈希的正确成因（2026-09-17 订正，勿再混列）**：
  - `626a3b00…` = **T1.18 前源码**（**无** `HAS_JPEG=1 HAS_OPENCV=1` 编译定义 ⇒ 与后两者**不同源、不可比**）；
  - `c28ab675…` = **同源（T1.18 后）+ 未显式固定 `-DTTBOX_PROJECT_ROOT`**（走默认值 ⇒ 检出路径被编进产物 ⇒ 同一源码、不同检出路径两产物不同）；
  - `d20b25da…` = **同源 + 固定 `-DTTBOX_PROJECT_ROOT=/opt/ttbox`**。
  ⇒ **`c28ab675…` 不属"T1.18 前"**；它与 `d20b25da…` **同源**，差异只因 **`TTBOX_PROJECT_ROOT` 未 pin**。**先证同源（同 commit + 同配置向量）、再比字节；不同源哈希不得并列比较。**
- **对外声称的边界**：可以说"**同源 + 同配置 ⇒ 逐位相同**"；**不得**说"**任意两次构建都相同**"（跨父目录/换机位仍可能因绝对路径注入而不同 ⇒ 那是候选 B 的**降级残留**，见上表与「旧 B(旧)」行）。**候选 C** 仍作为 M1 **门禁层**（对外可声称）判据。

### 配置向量（判据第 2 项的可执行化 · 两个后果）（2026-09-17 立）

**可复现单元 = （源码 commit, 配置向量）二元组**——不是"一个 commit"，也不是"一个 cache 文件"。**配置向量（逐项显式给出）**：

| 项 | 取值（M1 出货） | 不 pin 的后果 |
|---|---|---|
| `CMAKE_TOOLCHAIN_FILE` | 交叉工具链文件（钉版本） | 换工具链 ⇒ 不同源 |
| `CMAKE_SYSROOT` | `sysroot-aarch64`（钉来源） | 换 sysroot ⇒ 不同源 |
| `CMAKE_BUILD_TYPE` | `Release` | Debug ⇒ 不同源 |
| **`-DTTBOX_PROJECT_ROOT`** | **`/opt/ttbox`（必需、显式）** | **走默认值 = `CMAKE_CURRENT_SOURCE_DIR/..`（检出路径）⇒ 该宏被编进目标码（`kDefaultConfigPath`）⇒ 换检出路径即 `sha` 不同 + 产物内泄漏构建机路径** |
| `-DTTBOX_SHIP` | `ON`（出货） | 缺 ⇒ 门禁语义不同 |
| 交叉开关 | `-DTTBOX_CORE_BUILD_AUTH=OFF` 等 | 编译定义变 ⇒ 不同源 |

**两个后果（必写，防"复现假达标"）**：
- **甲 · 跨检出路径 ⇒ `sha` 不同**：同一 commit 在不同检出目录构建 ⇒ `c28ab675…` ≠ `d20b25da…`（**仅因 `TTBOX_PROJECT_ROOT` 未 pin**）。**"同 cache"判据会误判二者可比。**
- **乙 · 不 pin `TTBOX_PROJECT_ROOT` ⇒ 产物内默认配置路径 = 构建机路径**（`kDefaultConfigPath` 编译期定值）⇒ 板端回退到**不存在的构建机路径**；且违反 `m1-acceptance-checklist.md` **A4 附加断言（产物路径锚）**。

**★ 该宏被编入目标码（编译期定值，非运行期 `getenv`）** ⇒ **它对产物字节有直接影响**，故**必须列入配置向量、且必须显式固定**（落 `tasks.md` T1.15 待补 ⑥）。

### 建议的组合口径（待 V4 数据确认后冻结）

1. **M1 门禁层（对外可声称）**：**候选 C**（同源闭包 + 形态 A + NEEDED 闭集）——**必达、可判 PASS/FAIL、抗腐**。
2. **附加证据层（对内留档，不作门禁）**：**候选 A**（同路径字节一致；已有正样本）。
3. **候选 B（降级）**：**"同父目录下不同构建目录字节一致"已有正样本（`t114 ≡ t118`）⇒ 可作对内留档**；**跨父目录/跨机位（旧 B 口径，需 `-ffile-prefix-map`）已降级为"可选加固"**——正样本已证同一 checkout 下**无需**额外手段；**对外声称"跨机位相同"仍须待举证后才可写**（否则口径落空）。
4. **无论选谁**：`RELEASE_BUILD.md` 的 `repro_verify` 字段须**写清是哪一层**（"同路径字节一致" / "跨路径字节一致" / "同源闭包+形态+NEEDED 一致"），**不得只写"可复现 ✅"**（空话 = 假信号）。

### 待 V4 数据落定的三个具体问题

1. **不同源哈希的成因核验（2026-09-17 已订正）**：`626a3b00…`（**T1.18 前源码**，**无** `HAS_JPEG`/`HAS_OPENCV` 定义）与 `d20b25da…`（T1.18 后）**因编译定义变化而不同源**（⇒ 不可比）；**`c28ab675…` 与 `d20b25da…` 同源**，二者差异**只因 `-DTTBOX_PROJECT_ROOT` 未显式固定**（默认值 = 检出路径 ⇒ 被编进目标码）。**勿把 `c28ab675…` 混入"T1.18 前"**。并排除并发构建竞态。
2. **正样本（`t114 ≡ t118`，字节相同）能否在"换父目录/换机位"下复现**？⇒ 决定**降级后的跨机位口径**是否可达标。
3. **`-ffile-prefix-map` 若引入，是否改变 RUNPATH / NEEDED**？⇒ 决定**降级后的跨机位口径**代价是否可接受。

> **★ #2 / #3 保持"待换路径正/负控"，勿提前冻结**——"换父目录/换机位"是否真能字节复现，**须由一次真实的换路径正/负控实验**裁定，**不得凭"同父目录正样本"外推**。（此为本节"待数据"状态的显式保留，防被人误读成已冻结。）

（**数据一到即冻结口径**；本框架**不预设结论**，只把三条路的代价/可验性/可声称性摊开。）

---

## §12 关联

- `tasks.md` T1.15 · `t1.16-impl-spec.md` §6–§8 · `m1-acceptance-checklist.md` §1/§2 · `lib-scope-ruling.md` §6/§11
- `server-api-contract.md`（自有端点定性，§7-(c) 档依据）
- 交接文档 §4（P0-A/P0 链接阻断修复证据：`build-aarch64/ttbox_core_main` md5 `6449CECD…` + 备份 `.bak-20260916`）
