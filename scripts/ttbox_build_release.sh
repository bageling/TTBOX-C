#!/usr/bin/env bash
# ttbox_build_release.sh — 出货构建的【唯一入口】（T1.18 §5 发布门禁的执法点）
#
# ─────────────────────────────────────────────────────────────────────────────
# 为何存在（"唯一入口 + 为何存在"）：
#   libjpeg-linkage-ruling.md §5 规定"无 jpeg / 无 opencv **不许出货**"，但此前该判据
#   只写在文档里、且 scripts/ttbox_build_release.sh **并不存在**（全仓仅 core/CMakeLists.txt
#   一处注释引用） ⇒ T1.13/T1.15 未跑时，这道门禁**今天没有任何人执行**。
#   这与 P0-2b 同族：**守卫存在 = 假象**。故把门禁落地为可执行入口：
#   **出货构建一律经此**；绕开它 = 未执行门禁（脚本存在但无人调用 = 假护栏）。
#
# 双保险（两道都 fail-closed，互为补充）：
#   · 主保险（CMake，configure 期）：core/CMakeLists.txt 的 `TTBOX_SHIP=ON` 绊线
#     —— 缺 jpeg/opencv 时 configure 直接 FATAL_ERROR，**执行者不可能跳过**。
#   · 本脚本（build 期）：configure 后**复核** configure.log 的配置位；缺行亦拒。
#
# 语义：TTBOX_CORE_HAS_JPEG / TTBOX_CORE_HAS_OPENCV 非 TRUE ⇒ rc≠0、**拒绝出货**
#   （不是 WARNING）。与 §5.1 配套：`rc=0` 只证明**链接成功**、不证明**可出货**；
#   "不可出货"由本门禁拦下。
#
# ─────────────────────────────────────────────────────────────────────────────
# 门禁全景（一次执行全跑；任一不过 ⇒ rc≠0，且**不写留档**）：
#   0)  配置向量固定 `TTBOX_PROJECT_ROOT`（S1 / T1.15 待补 ⑥）
#   0b) 配置向量固定 `TTBOX_CORE_BUILD_AUTH=OFF`（T1.15 待补 ①，§2 强制配置）
#   1)  configure：强制 `TTBOX_SHIP=ON`（缺 jpeg/opencv ⇒ configure 期 FATAL）+ 强制上两项
#   1b) `CMakeCache.txt` fail-closed 复核（"传了"≠"生效了"；缺项即拒）
#   2)  依赖门禁：`TTBOX_CORE_HAS_JPEG` / `TTBOX_CORE_HAS_OPENCV` == TRUE
#   3)  build
#   4)  产物路径锚（A4 附加断言）：① 构建机绝对路径 == 0；② 含板端配置路径 >= 1
#   4b) 字符串表三档门禁（T1.15 待补 ③，§7）：
#         (a) 第三方域名 `antszy|blpro|blpt` == 0            → 硬 FAIL
#         (b) 凭据非空默认字面量 `client_secret_ = "..."` == 0 → 硬 FAIL（源码级）
#         (c) 自有端点 `cctv2.top` 登记值                     → ★不判 FAIL（详见 §7 (c) 档）
#   4c) usbproxy 预编译二进制「诚实性」（T1.15 验收⑧ / A30，§11）：
#         .sha256 一致 + 旧目录字面量 == 0 为硬门禁；「可重建」为可选（较重，opt-in），
#         且**必须在临时副本里重建**（Makefile 的 clean 会删掉入库件）
#   5)  payload 白名单闭集 + 构建后自检（T1.15 待补 ④/⑤，§8/§9）：
#         bin/ 恰 1 文件（禁 `.bak-*`/`*_backup*`/`*.o`/`*.a`/`*.cmake`/`CMakeFiles`）
#         lib/ 只 `librknnrt.so`，且与**链接期**那份逐字节同源（2.3.2 / d31fc19c…）
#         `readelf` 静态自检：RUNPATH 逐段 `$ORIGIN`（含空段拒绝）+ NEEDED 覆盖
#         （★ 交叉产物 host 不能 `ldd` ⇒ host 侧只用 `readelf`；运行期 `ldd` 归板端 T1.13）
#   6)  留档 `docs/build/release-records/RELEASE_BUILD-<UTC>-<commit7>.md`（字段见 §10）+ §5 单行记录
#       （T1.49：留档已由 ${BUILD_DIR}/ 迁出 ⇒ 不再需要"赶在 §3 clean 之前抢救留档"的时序，
#         且仓库根不会再因为留档白名单而冒出未被忽略的 build-* 路径）
#   7)  可复现性二次 clean build 对照（**可选**，`TTBOX_RELEASE_VERIFY_REPRO=1` 启用）
#
# 可选开关（默认全关；开启后的结论**一律写入留档**，不允许静默）：
#   TTBOX_RELEASE_VERIFY_REPRO=1              # 同源同向量、不同构建目录二次 clean build 逐字节对照
#   TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD=1   # usbproxy 从源码重建（§11②）
#   TTBOX_USBPROXY_INCLUDE / TTBOX_USBPROXY_LIBDIR   # 上者的依赖路径（默认 /usr/include、/usr/lib/aarch64-linux-gnu）
#
# 退出码：0 = 门禁通过 + 构建成功；非 0 = 拒绝出货（依赖缺失 / 留档缺项 / 自检失败 / 构建失败）。
#
# 用法（其余参数原样透传给 cmake；目录由 TTBOX_BUILD_DIR 指定，默认 build-aarch64-t114，
#       与 ttbox_fhs_init.sh 的 detect_build_dir 口径一致）：
#   bash scripts/ttbox_build_release.sh \
#     -DCMAKE_TOOLCHAIN_FILE=deploy/cmake/toolchain-aarch64.cmake \
#     -DCMAKE_SYSROOT=<sysroot> -DTTBOX_PROJECT_ROOT=/opt/ttbox \
#     -DTTBOX_CROSS_AARCH64=ON -DCMAKE_BUILD_TYPE=Release
#
# 打包/验证：本脚本**不重复实现**——门禁+构建通过后**委派**既有脚本：
#   ttbox_release_install.sh / ttbox_release_verify.sh / ttbox_release_selftest.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

die() { echo "FATAL: $*" >&2; exit 1; }

# ---- 仓库根定位：优先 TTBOX_REPO，否则由脚本自身位置推导 ----
if [ -n "${TTBOX_REPO:-}" ]; then
  REPO="$TTBOX_REPO"
else
  SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  REPO="$(cd "${SELF_DIR}/.." && pwd)"
fi

BUILD_DIR="${TTBOX_BUILD_DIR:-build-aarch64-t114}"
case "$BUILD_DIR" in
  /*) ;;
  *) BUILD_DIR="${REPO}/${BUILD_DIR}" ;;
esac

# ---- D01（2026-09-18 定案，P0 fail-closed）：BUILD_DIR 白名单守卫 ----
# 下面 §3 clean 会 `rm -rf "$BUILD_DIR"`；TTBOX_BUILD_DIR 是绝对路径时若被误传
# （如 /、/opt/ttbox、/var/lib/ttbox、$REPO 本身），一行命令就能删掉任意目录。
# 守卫：拒绝空串、拒绝根/发布/运行时/仓库本身等路径，命中即 die。
case "$BUILD_DIR" in
  ""|"/"|"/opt"|"/opt/ttbox"|"/var"|"/var/lib"|"/var/lib/ttbox"|"/etc"|"/etc/ttbox"|"/home"|"/root"|"/tmp"|"/usr"|"$REPO"|"$REPO"/)
    die "D01 守卫：拒绝危险构建目录 BUILD_DIR='${BUILD_DIR}'（可指定仓内相对目录，如 build-aarch64-t114）" ;;
  "$REPO"/*|"$REPO")
    case "$(basename "$BUILD_DIR")" in
      build-*|out-*) ;;   # 仓内 build-*/out-* 才是合法构建目录
      *) die "D01 守卫：BUILD_DIR 须为仓内 build-*/out-* 目录（实得 '${BUILD_DIR}'）" ;;
    esac ;;
  *) die "D01 守卫：BUILD_DIR 必须位于仓库内（'${REPO}' 下），实得 '${BUILD_DIR}'" ;;
esac
LOG="${BUILD_DIR}/configure.log"
mkdir -p "$BUILD_DIR"

echo "[release] 出货构建：强制 -DTTBOX_SHIP=ON（缺 libjpeg/OpenCV ⇒ configure 期即失败）"
echo "[release] 构建目录：${BUILD_DIR}"
echo "[release] configure 日志：${LOG}"

# ---- 0) 配置向量：TTBOX_PROJECT_ROOT 强制显式固定（S1 / T1.15 待补 ⑥）----
# 为何必须：该宏被**编进目标码**（core/src/app/Application.cpp 的 kDefaultConfigPath =
#   TTBOX_PROJECT_ROOT "/config/default.json"，编译期定值，非运行期 getenv）。
#   不固定 ⇒ CMake 回退到 `${CMAKE_CURRENT_SOURCE_DIR}/..`（**检出路径**）⇒ 两个后果：
#     甲 · 换检出路径即产物 sha 漂移（同源不再可比）；
#     乙 · 产物内泄漏构建机绝对路径 ⇒ 板端 kDefaultConfigPath 指向不存在的路径，
#          且违反 m1-acceptance-checklist.md A4 附加断言 ①。
#   （依据：build-reproducibility.md §13 配置向量表）
# 本脚本三道 fail-closed：① 强制传入（并**剥离调用方自带的同名参数**，防"我以为传了"）；
#                          ② configure 后复核 CMakeCache.txt；③ build 后产物字符串后置门禁。
PROJ_ROOT="${TTBOX_PROJECT_ROOT:-/opt/ttbox}"
case "$PROJ_ROOT" in
  /*) ;;
  *) die "TTBOX_PROJECT_ROOT 必须是绝对路径（板端运行根），实得 '${PROJ_ROOT}'。" ;;
esac

# ---- 0b) 配置向量：TTBOX_CORE_BUILD_AUTH 强制 OFF（T1.15 待补 ①）----
# 为何必须（build-reproducibility.md §2「固定配置（强制，不得省略）」）：
#   ★ M2 起（2026-09-17）：出货形态（AUTH=OFF）**已装离线授权**——OfflineCardClient
#   （Ed25519 自包含验签）+ LicenseDaemon + LicenseCard + ed25519_verify 属无条件
#   CORE_SOURCES，AUTH 门控**不再**决定"有无授权"，只决定"有无**在线**客户端"。
#   AUTH=ON 会把在线授权层（TtboxLicenseClient/HttpClient + OpenSSL）编进产物 ⇒ 三个后果：
#     甲 · 产物内出现自有端点字面量（cctv2.top）与在线授权代码路径 ⇒ §7 (c) 档
#          由 M1/M2 期望 0 变成 ==1，出货形态与声明不符；
#     乙 · 新增 NEEDED（libcrypto/libssl）⇒ 破坏 A32「随包 ELF 运行期依赖闭集」；
#     丙 · 编译定义变 ⇒ 不同源 ⇒ 与既有指纹锚（如 `d20b25da…`）**不可比**（§13 配置向量）。
#   M1 旧述「NullClient fail-open」已作废：fail-open 桩（DisabledAuth.cpp）已删除。
#   同 ROOT 一样三道 fail-closed：① 强制传入 + 剥离调用方同名参数；② CMakeCache 复核；③ (c) 档登记。
AUTH_FLAG=OFF
GEN="Ninja"   # §2「固定配置」：生成器钉死 Ninja（缺 -G 时 cmake 会退回 Unix Makefiles，
              #   随后因 CMAKE_MAKE_PROGRAM 未设而报一句**与真因无关**的错 ⇒ 诊断陷阱）

# 读取**上一次留档**（T1.49 起留档落 docs/build/release-records/，与构建目录解耦）
# 旧做法把留档写在 ${BUILD_DIR}/RELEASE_BUILD.md 内，而 §3 clean 会 rm -rf 该目录
#   ⇒ 只能靠"赶在 clean 之前读一次"的时序保命；更要命的是每轮构建都会在仓库根
#   新长出一个带 RELEASE_BUILD.md 的 build-* 目录，而 .gitignore 的白名单又把这类
#   文件**放行**（未被忽略≠已入库）⇒ 一次 `git add -A` 就把一堆构建路径带进版本库。
# 现按**文件名内的 UTC 时间戳**排序取最新一份（不用 `ls -t`/mtime：留档一旦被编辑器重新格式化
#   或复制，mtime 就不再等于构建时间 —— 实测 t142 的 mtime 是 22:49 而内部时间是 09:00:46）。
# 目录为空时 PREV_REC 为空，走"首次留档"分支。
RECORDS_DIR="${REPO}/docs/build/release-records"
PREV_MD="$(ls -1 "${RECORDS_DIR}"/RELEASE_BUILD-*.md 2>/dev/null | sort | tail -1 || true)"
PREV_REC="$(sed -n 's/^<!-- RELEASE_BUILD_RECORD: \(.*\) -->$/\1/p' "${PREV_MD}" 2>/dev/null | tail -1 || true)"

# ---- §3 clean（可复现前提：删净旧构建目录）----
# 为何强制：陈旧 CMakeCache/build.ninja 会让"我以为传了的向量"静默失效（正是本脚本
#   ①/⑥ 要防的那类假信号），且 §3 第 1 步逐字要求 `rm -rf "$BUILD"`。
echo "[release] §3 clean：rm -rf ${BUILD_DIR}"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"     # 重建空目录：下一行 `tee "$LOG"` 需要父目录已存在（tee 与 cmake 同时启动）

# 剥离调用方自带的 -DTTBOX_PROJECT_ROOT / -DTTBOX_CORE_BUILD_AUTH / -G（"-D...=v" 与 "-D ... v" 两形），
# 改用上面的显式值，避免 cmake "后者覆盖前者"的静默歧义。
PASS_ARGS=()
_skip_next=0
for _a in "$@"; do
  if [ "$_skip_next" = 1 ]; then _skip_next=0; continue; fi
  case "$_a" in
    -DTTBOX_PROJECT_ROOT=*|-DTTBOX_CORE_BUILD_AUTH=*) continue ;;
    -DTTBOX_PROJECT_ROOT|-DTTBOX_CORE_BUILD_AUTH)     _skip_next=1; continue ;;
    -G)                                              _skip_next=1; continue ;;
    -G*)                                             continue ;;
  esac
  PASS_ARGS+=("$_a")
done

# 归一化：相对路径参数按**仓库根**解析为绝对路径（fail-closed）。
# 为何必要：cmake 解析相对 toolchain 文件的基准不可靠（实测在脚本内直接报
#   `Could not find toolchain file: deploy/cmake/toolchain-aarch64.cmake` ⇒ 出货被误拒）。
#   文档里的用法示例用的就是相对路径 ⇒ 不归一 = 文档示例跑不通。
NORM_ARGS=()
for _a in "${PASS_ARGS[@]}"; do
  case "$_a" in
    -DCMAKE_TOOLCHAIN_FILE=*)
      _v="${_a#-DCMAKE_TOOLCHAIN_FILE=}"
      case "$_v" in /*) ;; *) _v="${REPO}/${_v}" ;; esac
      [ -f "$_v" ] || die "工具链文件不存在：'${_v}'（相对路径按仓库根 ${REPO} 解析）⇒ 拒绝出货。"
      _a="-DCMAKE_TOOLCHAIN_FILE=${_v}"
      ;;
    -DCMAKE_SYSROOT=*)
      _v="${_a#-DCMAKE_SYSROOT=}"
      case "$_v" in /*) ;; *) _v="${REPO}/${_v}" ;; esac
      _a="-DCMAKE_SYSROOT=${_v}"
      ;;
  esac
  NORM_ARGS+=("$_a")
done
PASS_ARGS=("${NORM_ARGS[@]}")

echo "[release] TTBOX_PROJECT_ROOT（强制固定）: ${PROJ_ROOT}"
echo "[release] TTBOX_CORE_BUILD_AUTH（强制固定）: ${AUTH_FLAG}"
echo "[release] 生成器（强制固定）: ${GEN}"

# ---- 1) configure（强制 TTBOX_SHIP=ON + 强制 ROOT/AUTH；stdout+stderr 落 configure.log）----
#   其余参数（工具链/sysroot/构建类型…）原样透传。configure 失败（含 TTBOX_SHIP 绊线触发）即中止。
if ! cmake -S "${REPO}/core" -B "${BUILD_DIR}" \
       -G "${GEN}" \
       -DTTBOX_SHIP=ON \
       -DTTBOX_PROJECT_ROOT="${PROJ_ROOT}" \
       -DTTBOX_CORE_BUILD_AUTH="${AUTH_FLAG}" \
       "${PASS_ARGS[@]}" 2>&1 | tee "$LOG"; then
  die "configure 失败（含 TTBOX_SHIP=ON 绊线）⇒ 拒绝出货。"
fi

# ---- 1b) 配置向量复核：CMakeCache.txt fail-closed（S1 ⑥ / T1.15 ①）----------------
#   为什么还要复核：脚本"传了"不等于"生效了"（调用方参数顺序/缓存残留都可能让显式值失效）。
#   缺项即拒（fail-closed）；值不符即拒。取证：tasks.md T1.15 ⑥ / ①。
CACHE_FILE="${BUILD_DIR}/CMakeCache.txt"
[ -f "$CACHE_FILE" ] || die "CMakeCache.txt 缺失（${CACHE_FILE}）⇒ 无法复核配置向量 ⇒ 拒绝出货。"

CACHED_ROOT="$(sed -n 's/^TTBOX_PROJECT_ROOT:[^=]*=\(.*\)$/\1/p' "$CACHE_FILE" | tail -n1)"
if [ -z "${CACHED_ROOT}" ]; then
  die "CMakeCache 缺 TTBOX_PROJECT_ROOT ⇒ 无法证明已固定（产物会泄漏构建机路径、板端配置路径错）⇒ 拒绝出货。"
fi
if [ "${CACHED_ROOT}" != "${PROJ_ROOT}" ]; then
  die "配置向量不符 —— CMakeCache 的 TTBOX_PROJECT_ROOT='${CACHED_ROOT}' != 期望 '${PROJ_ROOT}' ⇒ 拒绝出货。"
fi

CACHED_AUTH="$(sed -n 's/^TTBOX_CORE_BUILD_AUTH:[^=]*=\(.*\)$/\1/p' "$CACHE_FILE" | tail -n1)"
if [ -z "${CACHED_AUTH}" ]; then
  die "CMakeCache 缺 TTBOX_CORE_BUILD_AUTH ⇒ 无法证明在线授权层已关闭 ⇒ 拒绝出货。"
fi
if [ "${CACHED_AUTH}" != "${AUTH_FLAG}" ]; then
  die "配置向量不符 —— CMakeCache 的 TTBOX_CORE_BUILD_AUTH='${CACHED_AUTH}' != 期望 '${AUTH_FLAG}'（M1 无在线客户端）⇒ 拒绝出货。"
fi

CACHED_CROSS="$(sed -n 's/^TTBOX_CROSS_AARCH64:[^=]*=\(.*\)$/\1/p' "$CACHE_FILE" | tail -n1)"
CACHED_SYSROOT="$(sed -n 's/^CMAKE_SYSROOT:PATH=\(.*\)$/\1/p' "$CACHE_FILE" | tail -n1)"
CACHED_BUILDTYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=\(.*\)$/\1/p' "$CACHE_FILE" | tail -n1)"

# 工具链（留档必录，§10/§6）——★ 不能从 CMakeCache 取 CMAKE_CXX_COMPILER：
#   本仓的工具链在 toolchain-aarch64.cmake 里用 `set(CMAKE_CXX_COMPILER …)` 设的是
#   **普通变量**（不是 CACHE）⇒ 该键**根本不在 CMakeCache.txt 里**（实测：只有
#   CMAKE_CXX_COMPILER_AR/_RANLIB 两个派生键）。故改读 CMake **生成**的权威文件
#   `CMakeFiles/<ver>/CMakeCXXCompiler.cmake`（它由 CMake 亲自写入，含 path + version）。
CXXC_FILE="$(find "${BUILD_DIR}/CMakeFiles" -maxdepth 2 -name 'CMakeCXXCompiler.cmake' 2>/dev/null | head -1 || true)"
CACHED_CXX=""; CXX_VER=""
if [ -n "${CXXC_FILE}" ]; then
  CACHED_CXX="$(sed -n 's/^set(CMAKE_CXX_COMPILER "\(.*\)")$/\1/p' "$CXXC_FILE" | head -1 || true)"
  CXX_VER="$(sed -n 's/^set(CMAKE_CXX_COMPILER_VERSION "\(.*\)")$/\1/p' "$CXXC_FILE" | head -1 || true)"
fi
# §10 把 toolchain / sysroot 列为**必填** ⇒ 取不到即 fail-closed（防留档里写"<未识别>"过关）
[ -n "${CACHED_CXX}" ] || die "无法确定交叉工具链（读不到 ${CXXC_FILE:-CMakeFiles/*/CMakeCXXCompiler.cmake} 的 CMAKE_CXX_COMPILER）⇒ 留档必填项缺失 ⇒ 拒绝出货。"
[ -n "${CXX_VER}" ]    || die "无法确定交叉工具链版本 ⇒ 留档必填项缺失（§10 toolchain）⇒ 拒绝出货。"
[ -n "${CACHED_SYSROOT}" ] || die "CMakeCache 缺 CMAKE_SYSROOT ⇒ 留档必填项缺失（§10 sysroot）⇒ 拒绝出货。"
[ -n "${CACHED_BUILDTYPE}" ] || die "CMakeCache 缺 CMAKE_BUILD_TYPE ⇒ 配置向量不完整（§13）⇒ 拒绝出货。"

echo "[release] 配置向量复核通过：ROOT=${CACHED_ROOT} / AUTH=${CACHED_AUTH}"
echo "[release] 其余向量：CROSS_AARCH64=${CACHED_CROSS:-<未设>} / SYSROOT=${CACHED_SYSROOT:-<未设>} / CXX=${CACHED_CXX:-<未设>}"
if [ "${CACHED_CROSS}" != "ON" ]; then
  # 非 fatal：缺 CROSS_AARCH64=ON 时 RKNN/RGA 探测失败只打 WARNING，但随后产物会缺失，
  # 被下面 §4 的"产物缺失 ⇒ FATAL"兜住。此处只告警，避免对合法场景误拒。
  echo "[release][WARN] TTBOX_CROSS_AARCH64 != ON ⇒ RKNN/RGA 硬门禁未启用（出货形态应为 ON）"
fi

# ---- 2) 出货前置门禁（§5 逐字实现 + fail-closed）-----------------------------
#   依赖 CMake configure 期打出的两行 STATUS：
#     -- TTBOX_CORE_HAS_JPEG=TRUE|FALSE
#     -- TTBOX_CORE_HAS_OPENCV=TRUE|FALSE
#   ★ 两种"缺行"情形一律拒（fail-closed）：
#     (a) configure.log 不存在 ⇒ 无法核对 ⇒ 拒；
#     (b) `-- TTBOX_CORE_HAS_*=...` 行缺失 ⇒ flag() 返回空串 ⇒ != "TRUE" ⇒ 拒。
[ -f "$LOG" ] || die "configure 日志缺失（${LOG}）⇒ 无法核对配置位 ⇒ 拒绝出货。"
flag() { sed -n "s/^-- $1=\([A-Za-z]*\).*/\1/p" "$LOG" | tail -n1; }
JPEG_FLAG="$(flag TTBOX_CORE_HAS_JPEG)"
OCV_FLAG="$(flag TTBOX_CORE_HAS_OPENCV)"
if [ "$JPEG_FLAG" != "TRUE" ] || [ "$OCV_FLAG" != "TRUE" ]; then
  echo "FATAL: 出货构建缺依赖（TTBOX_CORE_HAS_JPEG=${JPEG_FLAG:-<缺行>}, TTBOX_CORE_HAS_OPENCV=${OCV_FLAG:-<缺行>}）" >&2
  echo "       → PreviewModule 走 stub ⇒ 预览/画框不可用 ⇒ 禁止出货。" >&2
  exit 1
fi
echo "[release] 门禁通过：TTBOX_CORE_HAS_JPEG=TRUE / TTBOX_CORE_HAS_OPENCV=TRUE"

# ---- 3) 构建（门禁通过后才编译）--------------------------------------------
cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 4)"

# ---- 4) 产物字符串后置门禁（A4 附加断言 · 产物路径锚）------------------------
#   「configure 传了」≠「产物对了」——本门禁直接查**产物本身**，是配置向量的最终出口证据。
#   ① 产物内不得含构建机绝对路径（检出根 / 用户目录）⇒ 期望 0
#   ② 产物必须含板端目标配置路径（打包默认值）        ⇒ 期望 >=1
#   取证：m1-acceptance-checklist.md「A4 附加断言 · 产物路径锚」。
#   注：① 的残留来源除 TTBOX_PROJECT_ROOT 外还有 `__FILE__`（core/src/common/Logger.hpp 的
#       TTBOX_LOGx 宏），已由 core/CMakeLists.txt 的 -ffile-prefix-map 归一；本门禁同时守住两者。
PRODUCT="${BUILD_DIR}/ttbox_core_main"
[ -f "$PRODUCT" ] || die "产物缺失（${PRODUCT}）⇒ 拒绝出货。"
command -v strings >/dev/null 2>&1 || die "缺 strings（binutils）⇒ 无法核对产物路径锚 ⇒ 拒绝出货。"

A4_HOSTPATH_N="$(strings -a "$PRODUCT" | grep -Ec '/home/|/mnt/|/Users/|/root/|build-aarch64' || true)"
A4_TARGET_N="$(strings -a "$PRODUCT" | grep -c "${PROJ_ROOT}/config/default.json" || true)"
if [ "${A4_HOSTPATH_N}" != "0" ]; then
  echo "FATAL: A4① 产物内残留构建机绝对路径 ${A4_HOSTPATH_N} 处（期望 0）⇒ 拒绝出货。样例：" >&2
  strings -a "$PRODUCT" | grep -E '/home/|/mnt/|/Users/|/root/|build-aarch64' | head -5 >&2
  echo "       → 检查 TTBOX_PROJECT_ROOT 是否显式固定；若为 __FILE__ 残留，检查 -ffile-prefix-map。" >&2
  exit 1
fi
if [ "${A4_TARGET_N}" -lt 1 ] 2>/dev/null; then
  die "A4② 产物内未含 '${PROJ_ROOT}/config/default.json'（期望 >=1）⇒ 拒绝出货。"
fi
echo "[release] 产物路径锚通过：A4① 构建机路径=${A4_HOSTPATH_N}（期望 0）/ A4② 板端配置路径=${A4_TARGET_N}（期望 >=1）"

# ---- 4b) 字符串表三档硬门禁（T1.15 待补 ③；build-reproducibility.md §7）-------
#   §7 三档：前两档硬 FAIL、第三档仅登记。任一 (a)/(b) 命中 ⇒ 非零退出。
#   ★ 计数口径：统一用 `strings -a | grep -c`（二进制直接 `grep -c` 会少算——对含
#     NUL/长串的 .rodata 按行统计不可靠；本轮实测 7 vs 实际 11 的教训）。
# (a) 第三方域名（硬 FAIL）：antszy / blpro / blpt 是我方**已删**的第三方授权/平台域名。
THIRD_N="$(strings -a "$PRODUCT" | grep -Ec 'antszy|blpro|blpt' || true)"
if [ "${THIRD_N}" != "0" ]; then
  echo "FATAL: §7(a) 产物内残留第三方域名 ${THIRD_N} 处（期望 0，硬 FAIL）⇒ 拒绝出货。样例：" >&2
  strings -a "$PRODUCT" | grep -E 'antszy|blpro|blpt' | head -5 >&2
  echo "       → T1.07a「core 清源」未彻底：第三方授权实体仍被 ODR-use 进产物。" >&2
  exit 1
fi

# (b) 凭据非空默认字面量（硬 FAIL，**源码级**）：env **变量名**合法保留，
#     `client_secret` **非空默认字面量**不得（对照 audit「空 secret 照签」缺陷）。
#     文件缺失 ⇒ fail-closed 拒绝（无法核对即拒）。
#     正则说明（2026-09-17 订正）：只匹配 `= "..."` 形式的**字面量赋值**；
#     原正则 `TTBOX_CLIENT_SECRET[^)]*"[^"]+` 对 `getenv("TTBOX_CLIENT_SECRET")` 恒假 FAIL，已废。
SECRET_SRC="${REPO}/core/src/auth/TtboxLicenseClient.hpp"
[ -f "$SECRET_SRC" ] || die "§7(b) 源码门禁无法执行：缺 ${SECRET_SRC} ⇒ 拒绝出货（fail-closed）。"
CRED_N="$(grep -cE 'client_secret_[[:space:]]*=[[:space:]]*"[^"]+"' "$SECRET_SRC" || true)"
if [ "${CRED_N}" != "0" ]; then
  echo "FATAL: §7(b) ${SECRET_SRC##*/} 内存在 client_secret 非空默认字面量 ${CRED_N} 处（期望 0，硬 FAIL）：" >&2
  grep -nE 'client_secret_[[:space:]]*=[[:space:]]*"[^"]+"' "$SECRET_SRC" >&2
  exit 1
fi

# (c) 自有端点计数：M1 期望 0；T2.x 允许 ==1；>1 告警；★ 不判 FAIL（tasks.md:160 ⑤）
#     ★ 2026-09-26：自有端点从旧 IP `38.127.133.6:10039`（七牛，已宕机）换成域名
#       `cctv2.top:10086`（阿里云，安全组直通）。扫描对象随之改为域名。
#       cctv2.top = **我方自有的**授权/OTA 服务器、合法默认端点，**不是缺陷**（不在 (a) 档第三方名单里）。
#     M1 = NullClient 不接入 ⇒ 端点字面量随 ODR-use 消失 ⇒ 期望 0。
#     ★ 不得把 `==1` 设成 M1 门禁（T2.x 装回在线客户端后会出现 ==1，届时会误 FAIL）。
ENDPOINT_N="$(strings -a "$PRODUCT" | grep -c 'cctv2\.top' || true)"
if [ "${ENDPOINT_N}" -gt 1 ] 2>/dev/null; then
  echo "[release][WARN] §7(c) 自有端点 cctv2.top 出现 ${ENDPOINT_N} 次（>1 告警；不判 FAIL）：" >&2
  strings -a "$PRODUCT" | grep 'cctv2\.top' | head -3 >&2
fi
STRINGS_GATE="third=${THIRD_N}/ip=${ENDPOINT_N}/cred=${CRED_N}"
echo "[release] 字符串表三档门禁通过：${STRINGS_GATE}（a)第三方=0 ✅ (b)凭据字面量=0 ✅ (c)自有端点=${ENDPOINT_N}〔M1 期望 0，登记不判 FAIL〕"

# ---- 4c) usbproxy 预编译二进制「诚实性」（T1.15 验收⑧ / A30；build-reproducibility.md §11）----
# 为何必须在**出货入口**把守：`usbproxy/usb-proxy` 是**预编译 ELF 直接入 git**、**不随 core 构建**
#   ⇒ 不在此处核对，板上就可能跑"没人能复现的源码产出的代码"——比"没有二进制"更危险。
#   与 T1.18 同族教训：**只写在文档里的判据 = 假护栏**（§11 此前只能靠人记得跑）。
# 三查（§11）：① `.sha256` 一致（硬 FAIL）② 可由 `usbproxy/Makefile` 重建（**硬要求**；sha 逐位一致是**理想**）
#              ③ 不含旧构建目录字面量（硬 FAIL）。
USBPROXY_DIR="${REPO}/usbproxy"
UBP="${USBPROXY_DIR}/usb-proxy"
UBP_SUM="${USBPROXY_DIR}/usb-proxy.sha256"
[ -f "$UBP" ] || die "§11/A30① 缺 usbproxy/usb-proxy ⇒ 无法证明随包二进制来源 ⇒ 拒绝出货。"
[ -f "$UBP_SUM" ] || die "§11/A30① 缺 usb-proxy.sha256 ⇒ 无法证明随包二进制来源（不许只校 sha 不验来源）⇒ 拒绝出货。"
UBP_SHA="$(sha256sum "$UBP" | cut -d' ' -f1)"
UBP_SHA_EXP="$(sed -n 's/^\([0-9a-fA-F]\{64\}\)[[:space:]].*/\1/p' "$UBP_SUM" | head -1)"
[ -n "$UBP_SHA_EXP" ] || die "usb-proxy.sha256 格式不可解析（期望 '<64hex>  usb-proxy'）⇒ 无法校对 ⇒ 拒绝出货。"
[ "$UBP_SHA" = "$UBP_SHA_EXP" ] \
  || die "§11/A30① usbproxy 入库二进制与 .sha256 不符（实得 ${UBP_SHA}，声明 ${UBP_SHA_EXP}）⇒ 拒绝出货。"
UBP_OLD_N="$(strings -a "$UBP" | grep -Ec '/opt/ttbox/(src/)?usbproxy|/opt/ttbox/usbproxy' || true)"
[ "$UBP_OLD_N" = "0" ] \
  || die "§11/A30③ usbproxy 内含旧构建目录字面量 ${UBP_OLD_N} 处（期望 0；说明它编译自旧目录）⇒ 拒绝出货。"

# ② 可重建（可选、较重 ⇒ 默认关；`TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD=1` 启用）。
#   ★ 铁律：**必须在临时副本里重建** —— Makefile 的 `clean` 会 `rm -f usb-proxy usb-proxy.sha256`，
#     就地跑会把**入库的那份二进制与校验和删掉**（已实测确认该 rm 行为）。
#   ★ 依赖路径**可覆盖**（默认值 = 本机 WSL 实测可用的一组）：
#     TTBOX_USBPROXY_INCLUDE / TTBOX_USBPROXY_LIBDIR。
#   ★ 不同源时的处置按 §11：**重建成功是硬要求**（失败 ⇒ 拒）；sha256 不一致 ⇒ 走 P2 三件套，
#     **不在本脚本判 FAIL**（§11 明写"sha256 逐位一致"是**理想**），但**必须留档**。
USBPROXY_REBUILD="未执行（本脚本默认不做 usbproxy 重建；设 TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD=1 启用，判据见 build-reproducibility.md §11②）"
if [ "${TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD:-0}" = "1" ]; then
  UBP_INC="${TTBOX_USBPROXY_INCLUDE:-/usr/include}"
  UBP_LIB="${TTBOX_USBPROXY_LIBDIR:-/usr/lib/aarch64-linux-gnu}"
  UBP_TMP="$(mktemp -d)"
  echo "[release] usbproxy 重建对照：副本 = ${UBP_TMP}/usbproxy（不触碰仓库内入库件）"
  cp -r "$USBPROXY_DIR" "${UBP_TMP}/usbproxy"
  # ★ 命令行 LDFLAGS 会**整个覆写** Makefile 里的 LDFLAGS（`+=` 全部失效），
  #   若不把 `-Wl,-rpath,'$ORIGIN/lib'` 一起带上，重建产物就**没有 RUNPATH**
  #   （T1.09 正是靠它找随包的 libjsoncpp.so.25），且与入库二进制的 sha 必然不同
  #   ⇒ 对照变成"苹果比橘子"，还可能把一份缺 RUNPATH 的二进制误当成可用产物。
  #   `$$` 是给 make 的转义（展开为 `$`），别改成单个 `$`。
  if ( cd "${UBP_TMP}/usbproxy" && \
       make CXX="${CACHED_CXX}" \
            CPPFLAGS="-I${UBP_INC} -I${UBP_INC}/lua5.4" \
            LDFLAGS="-pthread -L${UBP_LIB} -Wl,-rpath,'\$\$ORIGIN/lib'" \
            LDLIBS="-lusb-1.0 -llua5.4 -ljsoncpp" clean all ) > "${UBP_TMP}/rebuild.log" 2>&1; then
    UBP_NEW_SHA="$(sha256sum "${UBP_TMP}/usbproxy/usb-proxy" | cut -d' ' -f1)"
    if [ "$UBP_NEW_SHA" = "$UBP_SHA" ]; then
      USBPROXY_REBUILD="重建成功 + sha256 **逐位一致** ✅（理想达成；${UBP_NEW_SHA}）"
    else
      USBPROXY_REBUILD="重建成功但 sha256 不同（入库 ${UBP_SHA:0:16}… / 重建 ${UBP_NEW_SHA:0:16}…）⇒ 须走 §11② 的 P2 三件套（重建成功 + 功能等价 + 重建记录留档）；**本脚本不判 FAIL**（sha 一致是理想、可重建是硬要求）"
      echo "[release][WARN] ${USBPROXY_REBUILD}" >&2
    fi
    echo "[release] usbproxy 重建：${USBPROXY_REBUILD}"
  else
    tail -20 "${UBP_TMP}/rebuild.log" >&2 || true
    rm -rf "$UBP_TMP"
    die "§11/A30② usbproxy 重建**失败**（硬要求：源码必须能产出该二进制）⇒ 拒绝出货。"
  fi
  rm -rf "$UBP_TMP"
fi
echo "[release] usbproxy 诚实性：①.sha256 一致 ✅（${UBP_SHA:0:12}…）③旧路径字面量=0 ✅"

# ---- 5) payload 白名单闭集 + 构建后自检（T1.15 待补 ④/⑤；§8/§9）--------------
# ④ 白名单闭集：**禁 `build-aarch64*/` 通配**收 payload（会把 `ttbox_core_main.bak-20260916`
#    这类备份发到板上）。§8：`bin/` 闭集 == **恰两个文件** `ttbox_core_main` + `ttbox_web`。
# ⑤ 构建后自检（host 侧）：★ 关键约束 —— aarch64 产物在 x86 host **不能执行 `ldd`**
#    ⇒ host 自检**只用 `readelf`（静态）**；运行期 `ldd` 归板端 T1.13。
STAGE="${BUILD_DIR}/.selfcheck"
rm -rf "$STAGE"; mkdir -p "${STAGE}/bin" "${STAGE}/lib"

# (1) 闭集拷贝（逐名，禁通配）—— ★V1.0.56 起闭集 = { ttbox_core_main, ttbox_web, ttbox_edid, ttbox_ota }
#     （V1.0.47 加 ttbox_web；V1.0.53 把 EDID 应用入口由 shell 脚本换成 C++ 二进制）
for _b in ttbox_core_main ttbox_web ttbox_edid ttbox_ota; do
  install -m 0755 "${BUILD_DIR}/${_b}" "${STAGE}/bin/" || die "闭集拷贝失败：${_b}"
done
BIN_N="$(find "${STAGE}/bin" -maxdepth 1 -type f | wc -l)"
[ "${BIN_N}" = "4" ] || die "§8 payload bin/ 闭集应恰 4 文件（ttbox_core_main + ttbox_web + ttbox_edid + ttbox_ota），实得 ${BIN_N} ⇒ 拒绝出货。"
BAD_N="$(find "${STAGE}/bin" -maxdepth 1 -type f \
           \( -name '*.bak-*' -o -name '*.bak' -o -name '*_backup*' \
              -o -name '*.o' -o -name '*.a' -o -name '*.cmake' \) | wc -l)"
[ "${BAD_N}" = "0" ] || die "§8 payload bin/ 内混入禁用物 ${BAD_N} 项（.bak-*/*.o/*.a/*.cmake）⇒ 拒绝出货。"

# (2) release 作用域库：**只** librknnrt（librga/libjpeg/libopencv 属【基础镜像作用域】、不打包）
#     ★ 铁律（lib-scope-ruling.md §6）：所拷那份必须与【链接期 RKNNRT_LIBRARY】**逐字节同源**
#     （否则"新 bin 配错版库"）。来源 = 交叉 sysroot（链接期单一真源）。
RKNN_SRC="${CACHED_SYSROOT}/usr/lib/librknnrt.so"
[ -f "$RKNN_SRC" ] || die "sysroot 内缺 librknnrt.so（${RKNN_SRC}）⇒ 无法自包含随货 ⇒ 拒绝出货。"
install -m 0644 "$RKNN_SRC" "${STAGE}/lib/" || die "librknnrt.so 暂存失败"

LINKED="$(grep -aoE '/[^:"]*librknnrt\.so' "$CACHE_FILE" | head -1 || true)"
[ -n "$LINKED" ] || die "无法从 CMakeCache 解析链接期 librknnrt.so 路径 ⇒ 无法证明同源 ⇒ 拒绝出货。"
if ! cmp -s "$LINKED" "${STAGE}/lib/librknnrt.so"; then
  die "librknnrt 与链接期不同源（链接期 ${LINKED} ≠ sysroot 暂存份）⇒ 拒绝出货。"
fi
RKNN_SHA="$(sha256sum "${STAGE}/lib/librknnrt.so" | cut -d' ' -f1)"
RKNN_VER="$(strings -a "${STAGE}/lib/librknnrt.so" | grep -m1 -oE 'librknnrt version: [0-9][0-9.]*' | sed 's/.*: //' || true)"
[ "${RKNN_SHA}" = "d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8" ] \
  || die "随包 librknnrt.so sha256 不符基准（实得 ${RKNN_SHA}；基准 = 链接期=模型=板端 三重一致的 d31fc19c…）⇒ 拒绝出货。"
case "${RKNN_VER}" in
  2.3.2) : ;;
  *) die "随包 librknnrt.so 版本非 2.3.2（实得 '${RKNN_VER:-<未识别>}'）⇒ 换机器/换 sysroot 即不可复现 ⇒ 拒绝出货。" ;;
esac
echo "[release] librknnrt 同源校验通过：${RKNN_VER} / ${RKNN_SHA:0:8}…（与链接期逐字节相同）"

# (3) 自检 A：RUNPATH 逐段（语义与 ttbox_release_verify.sh §4 一致）
#     —— 为何不直接调 verify 脚本：其契约 = 对**已发布的 release 树**体检（读
#        `releases/<ver>/RELEASE_MANIFEST.json` + current 软链），出货前 stage 不是 release，
#        调用只会 `exit 2`（用法错误）。故此处**内联同语义断言**，真 release 树的权威校验
#        仍在 install 之后委派 `ttbox_release_verify.sh <ver>`（见脚本末尾）。
#     注：只对 bin/ 下 ELF 断言 RUNPATH；librknnrt.so 无 RUNPATH 段（它是被依赖方），
#        其"形态"由上面的同源+版本+sha256 三查把守（§9 亦只对 bin/* 做 RUNPATH 断言）。
command -v readelf >/dev/null 2>&1 || die "缺 readelf（binutils）⇒ 无法做 RUNPATH/NEEDED 自检 ⇒ 拒绝出货。"
command -v file >/dev/null 2>&1 || die "缺 file ⇒ 无法区分 ELF/非 ELF ⇒ 拒绝出货。"
SELFCHECK_RUNPATH_DETAIL=""
while IFS= read -r _f; do
  file -b -- "$_f" 2>/dev/null | grep -q ELF || continue        # 非 ELF 跳过（§7.2）
  _raw="$(readelf -d -- "$_f" 2>/dev/null | grep -E '\((RUNPATH|RPATH)\)' | head -1 || true)"
  [ -n "$_raw" ] || die "自检 A 失败：RUNPATH 缺失（要求 \$ORIGIN 自包含）: ${_f##*/}"
  _rp="$(printf '%s' "$_raw" | sed -n 's/.*\[\(.*\)\].*/\1/p')"
  [ -n "$_rp" ] || die "自检 A 失败：RUNPATH 段存在但为空串（[]）: ${_f##*/}"
  case "$_rp" in
    :*|*:|*::*) die "自检 A 失败：RUNPATH 含空段（首/尾/连续冒号；ld.so 语义下空段=回退 CWD）: ${_f##*/} => [${_rp}]" ;;
  esac
  _oldifs="$IFS"; IFS=':'
  for _seg in $_rp; do
    case "$_seg" in
      '$ORIGIN'|'$ORIGIN'/*|'${ORIGIN}'|'${ORIGIN}'/*) : ;;
      *) IFS="$_oldifs"; die "自检 A 失败：RUNPATH 段 '${_seg}' 非 \$ORIGIN 起头（跨版本/绝对路径）: ${_f##*/} => [${_rp}]" ;;
    esac
  done
  IFS="$_oldifs"
  SELFCHECK_RUNPATH_DETAIL="${SELFCHECK_RUNPATH_DETAIL}${_f##*/}=[${_rp}] "
done < <(find "${STAGE}/bin" -maxdepth 1 -type f | sort)
echo "[release] 自检 A 通过（RUNPATH 逐段 \$ORIGIN）：${SELFCHECK_RUNPATH_DETAIL}"

# (4) 自检 B：NEEDED 覆盖（防漏拷库）
#     ★ librga/libjpeg/libopencv 属【基础镜像作用域】、不拷 ⇒ 必须列入白名单，否则误报。
NEED_MISSING_N=0; NEED_MISSING_LIST=""
while IFS= read -r _f; do
  file -b -- "$_f" 2>/dev/null | grep -q ELF || continue
  while IFS= read -r _need; do
    [ -n "$_need" ] || continue
    case "$_need" in
      libc.so*|libm.so*|libstdc++.so*|libgcc_s.so*|ld-linux*) : ;;
      librga.so*|libjpeg.so*|libopencv_*.so*) : ;;      # 基础镜像作用域（板端系统提供）
      # ★ V1.0.49：httplib 开 SSL 后 ttbox_web 依赖 libssl/libcrypto。
      #   板端实测 /usr/lib/aarch64-linux-gnu/{libssl.so.3,libcrypto.so.3} 已由系统
      #   提供（2026-09-17 安装，非本仓交付物）⇒ 属基础镜像作用域，不随包。
      libssl.so*|libcrypto.so*) : ;;
      *)
        if [ ! -e "${STAGE}/lib/${_need}" ]; then
          NEED_MISSING_N=$(( NEED_MISSING_N + 1 ))
          NEED_MISSING_LIST="${NEED_MISSING_LIST}${_f##*/}→${_need} "
        fi
        ;;
    esac
  done < <(readelf -d -- "$_f" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' | sort -u)
done < <(find "${STAGE}/bin" -maxdepth 1 -type f | sort)
if [ "${NEED_MISSING_N}" != "0" ]; then
  die "自检 B 失败：${NEED_MISSING_N} 项 NEEDED 未随包且不在基础镜像白名单：${NEED_MISSING_LIST} ⇒ 拒绝出货。"
fi
NEEDED_N="$(readelf -d "${STAGE}/bin/ttbox_core_main" 2>/dev/null | grep -c NEEDED || true)"
SELFCHECK_NEEDED="PASS（${NEEDED_N} 项 NEEDED 全部覆盖：随包 1 + 基础镜像白名单）"
echo "[release] 自检 B 通过（NEEDED 覆盖）：${SELFCHECK_NEEDED}"

# (5) 自检 C（连带给 T1.13 ⑨）：**stage 内** lib/ 只含 librknnrt（不得混入 librga/libopencv*）
LIB_N="$(find "${STAGE}/lib" -maxdepth 1 -type f | wc -l)"
[ "${LIB_N}" = "1" ] || die "§8/§9 lib/ 应恰 1 文件（只 librknnrt）实得 ${LIB_N} ⇒ 拒绝出货。"
echo "[release] 自检 C 通过：stage lib/ 闭集 = $(basename -- "$(find "${STAGE}/lib" -maxdepth 1 -type f)")"

SELFCHECK_SUMMARY="RUNPATH 逐段 PASS / NEEDED 覆盖 ${SELFCHECK_NEEDED} / librknnrt 同源 PASS / lib 闭集 1"
echo "[release] 产物指纹：$(sha256sum "$PRODUCT" | cut -d' ' -f1)  (sha256, $(stat -c %s "$PRODUCT") B)"

# ---- 6) 可复现性二次 clean build 对照（可选；T1.15 验收① 的可执行落点）-------
#   默认**不执行**（省时）；`TTBOX_RELEASE_VERIFY_REPRO=1` 时在**同级不同构建目录**
#   用**同一配置向量** clean build 一次并逐字节比对（§3 clean + §13 V4 判据）。
#   不执行时留档字段必须写明"未执行"，**不得**只写"可复现 ✅"（空话 = 假信号，§13 建议 4）。
REPRO_RESULT="未执行（本脚本默认不做二次 clean build；设 TTBOX_RELEASE_VERIFY_REPRO=1 启用，判据见 build-reproducibility.md §13 V4）"
if [ "${TTBOX_RELEASE_VERIFY_REPRO:-0}" = "1" ]; then
  REPRO_DIR="${BUILD_DIR}-repro"
  echo "[release] 可复现性对照：二次 clean build → ${REPRO_DIR}"
  rm -rf "$REPRO_DIR"
  mkdir -p "$REPRO_DIR"
  cmake -S "${REPO}/core" -B "${REPRO_DIR}" \
    -G "${GEN}" \
    -DTTBOX_SHIP=ON -DTTBOX_PROJECT_ROOT="${PROJ_ROOT}" -DTTBOX_CORE_BUILD_AUTH="${AUTH_FLAG}" \
    "${PASS_ARGS[@]}" > "${REPRO_DIR}.configure.log" 2>&1 || die "可复现性对照：二次 configure 失败 ⇒ 拒绝出货。"
  cmake --build "${REPRO_DIR}" -j"$(nproc 2>/dev/null || echo 4)" > "${REPRO_DIR}.build.log" 2>&1 \
    || die "可复现性对照：二次 build 失败 ⇒ 拒绝出货。"
  if cmp -s "$PRODUCT" "${REPRO_DIR}/ttbox_core_main"; then
    REPRO_RESULT="同源 + 同配置向量、不同构建目录 ⇒ 逐字节相同 ✅（候选 B 层；md5 $(md5sum "$PRODUCT" | cut -d' ' -f1)）"
    echo "[release] ${REPRO_RESULT}"
  else
    REPRO_RESULT="❌ 逐字节不同（同源同向量却不一致 ⇒ 存在非确定性来源）"
    die "可复现性对照失败：两产物逐字节不同 ⇒ 拒绝出货（先用 cmp 定位差异段）。"
  fi
fi

# ---- 7) 留档 docs/build/release-records/（T1.49 由 BUILD_DIR 迁出；字段见 §10）+ §5 单行 ----
BUILD_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
GIT_COMMIT="$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
# ★ F11②（2026-09-17）：可复现锚完整性护栏。`commit` 记录的是**构建时 HEAD**，其语义前提是
#   "构建时无未提交的 core 源码改动"（见 §10 说明）。若 core 源码确有未提交改动，则记录的
#   commit **不包含**本次实际编译的源码 ⇒ 由该 commit 重建**不会**得到本产物（复现锚不自洽；
#   T4→T6 已各踩过一次）。此处显式告警（不阻断，避免误伤非 git / 特殊构建场景）。
if git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1; then
    _core_dirty="$(git -C "$REPO" status --porcelain -- core/src core/include 2>/dev/null || true)"
    if [ -n "${_core_dirty}" ]; then
        echo "  [!] 告警：core 源码存在**未提交**改动 ⇒ RELEASE_BUILD.md 的 commit=${GIT_COMMIT:0:7} 可能不含本次实际编译源码：" >&2
        printf '%s\n' "${_core_dirty}" | sed 's/^/        /' >&2
        echo "        （建议：先提交 core 源码再构建，使留档 commit 与实际源码自洽）" >&2
    fi
fi
TOOLCHAIN_DESC="${CACHED_CXX} (GCC ${CXX_VER})"
CXX_BANNER="$("${CACHED_CXX}" --version 2>/dev/null | head -1 || true)"
if [ -n "${CXX_BANNER}" ]; then TOOLCHAIN_DESC="${TOOLCHAIN_DESC} — ${CXX_BANNER}"; fi
HOST_DESC="$( { . /etc/os-release 2>/dev/null || true; printf '%s' "${PRETTY_NAME:-unknown}"; } ) @ $(uname -n 2>/dev/null || echo unknown)"
MD5="$(md5sum "$PRODUCT" | cut -d' ' -f1)"
SHA256="$(sha256sum "$PRODUCT" | cut -d' ' -f1)"
BUILDID="$(readelf -n "$PRODUCT" 2>/dev/null | sed -n 's/.*Build ID: //p' | head -1 || true)"
SIZE_B="$(stat -c %s "$PRODUCT")"
CONFIGURE_ARGS_STR="cmake -S ${REPO}/core -B ${BUILD_DIR} -G ${GEN} -DTTBOX_SHIP=ON -DTTBOX_PROJECT_ROOT=${PROJ_ROOT} -DTTBOX_CORE_BUILD_AUTH=${AUTH_FLAG} ${PASS_ARGS[*]:-}"

# 配置向量指纹（§13 V4 判据第 2 项的机器可比形式）：换任一向量项 ⇒ 指纹变 ⇒ 与上次留档不可比。
# ★ **不含 commit**：可复现单元 = (源码 commit, 配置向量) 二元组 ⇒ 指纹只覆盖**向量那一半**。
#   若把 commit 也拌进来，源码一推进就会让"配置向量相同"被判成"向量已变"（语义反转、文案误导）。
VECTOR_TMP="${BUILD_DIR}/.vector.canon"
{
  echo "build_type=${CACHED_BUILDTYPE}"
  # ★ 用**机器事实**（编译器绝对路径 + 版本号）而非展示用 banner 构造向量文本：
  #   否则脚本里一句文案调整就会让 vector_hash 变 ⇒ 把同源产物误判成"不可比"。
  echo "toolchain=${CACHED_CXX} ${CXX_VER}"
  echo "sysroot=${CACHED_SYSROOT}"
  echo "configure_args=${CONFIGURE_ARGS_STR}"
  echo "rknnrt_sha256=${RKNN_SHA}"
} > "$VECTOR_TMP"
VECTOR_HASH="$(sha256sum "$VECTOR_TMP" | cut -d' ' -f1)"

# 与上次留档比对（只作**留档对照**，不代判"可复现"——那由 §6 的可选二次 clean build 判）
#   ★ PREV_REC 在 §3 clean **之前**已读（留档就在 BUILD_DIR 内，clean 会把它删掉）。
#   ★ 三态分立（§0 规则 5(e) 四轴不得混列）：① 向量同 + commit 同 + md5 同 = 逐字节可复现；
#     ② 向量同 + commit 同 + md5 异 = **非确定性**（须查）；③ 向量同 + commit 异 = 源码推进，
#        md5 差异属**预期改变**，**不许**写成"复现失败"；④ 向量异 = 不同源、不可比。
PREV_CMP="无上次留档（首次）"
if [ -n "${PREV_REC}" ]; then
  PREV_VH="$(printf '%s' "$PREV_REC" | sed -n 's/.*vector_hash=\([0-9a-f]*\).*/\1/p')"
  PREV_MD5="$(printf '%s' "$PREV_REC" | sed -n 's/.*md5=\([0-9a-f]*\).*/\1/p')"
  PREV_COMMIT="$(printf '%s' "$PREV_REC" | sed -n 's/.*commit=\([0-9a-f]*\).*/\1/p')"
  if [ "${PREV_COMMIT}" = "${GIT_COMMIT}" ]; then SRC_SAME="源码 commit 相同"; else SRC_SAME="源码 commit 已变（${PREV_COMMIT:0:7}… → ${GIT_COMMIT:0:7}…）"; fi
  if [ "${PREV_VH}" = "${VECTOR_HASH}" ]; then
    if [ "${PREV_COMMIT}" = "${GIT_COMMIT}" ] && [ "${PREV_MD5}" = "${MD5}" ]; then
      PREV_CMP="配置向量相同 + ${SRC_SAME} + md5 相同（${MD5}）⇒ 同源逐字节可复现"
    elif [ "${PREV_COMMIT}" = "${GIT_COMMIT}" ]; then
      PREV_CMP="⚠️ 配置向量与源码 commit 都相同、但 md5 不同（上次 ${PREV_MD5} → 本次 ${MD5}）⇒ 存在非确定性，须查"
    else
      PREV_CMP="配置向量相同，但${SRC_SAME} ⇒ 本次 md5=${MD5} 与上次的差异属**预期改变**（非非确定性）"
    fi
  else
    PREV_CMP="配置向量已变（vector_hash ${PREV_VH:0:8}… → ${VECTOR_HASH:0:8}…）⇒ 与上次产物**不同源、不可比**；另：${SRC_SAME}"
  fi
fi

# 留档落 docs/build/release-records/（T1.49 口径：与构建目录解耦，理由见脚本头 §6 与 .gitignore 注释）
# 文件名带 UTC 时间戳 + commit 短号 ⇒ 历史留档可累积（几 KB/份），且不覆盖上一轮。
# ★ 不能直接用 ${BUILD_UTC}（形如 2026-09-23T02:23:25Z）做文件名：`:` 在 Windows 上是非法字符，
#   而本仓构建根在 /mnt/c（NTFS）下 ⇒ 必须先把 `:` 与 `-` 去掉。
mkdir -p "$RECORDS_DIR"
STAMP="$(printf '%s' "${BUILD_UTC}" | tr -d ':-')"
MD_FILE="${RECORDS_DIR}/RELEASE_BUILD-${STAMP}-${GIT_COMMIT:0:7}.md"
cat > "$MD_FILE" <<EOF
# RELEASE_BUILD.md — 出货构建留档（T1.15）

> 本文件由 \`scripts/ttbox_build_release.sh\` **自动生成**（门禁全绿后才写）。
> 字段定义：\`build-reproducibility.md\` §10；单行记录格式：§5。**手工改动无效**（下次构建覆盖）。
> \`commit\` 的语义 = **构建时被编译的源码 commit**（脚本不会有未提交的 core 源码改动）；
> 本留档自身、以及其后追加的提交都**不改变**该值 —— 故 HEAD 与它不一致属**正常**，不是错。
> \`vector_hash\` 只覆盖**配置向量那一半**（不含 commit），与 \`commit\` 合成 (源码, 向量) 二元组。

## 留档字段

| 字段 | 值 |
|---|---|
| \`commit\` | \`${GIT_COMMIT}\` |
| \`toolchain\` | ${TOOLCHAIN_DESC} |
| \`build_type\` | \`${CACHED_BUILDTYPE}\` |
| \`generator\` | \`${GEN}\` |
| \`sysroot\` | \`${CACHED_SYSROOT}\` |
| \`build_host\` | ${HOST_DESC} |
| \`build_time_utc\` | ${BUILD_UTC} |
| \`configure_args\` | \`${CONFIGURE_ARGS_STR}\` |
| \`product_path\` | \`${PRODUCT}\` |
| \`md5\` | \`${MD5}\` |
| \`sha256\` | \`${SHA256}\` |
| \`size\` | ${SIZE_B} B |
| \`buildid\` | \`${BUILDID}\` |
| \`strings_gate\` | \`${STRINGS_GATE}\`（(a)第三方域名=0 硬门禁 PASS / (b)凭据字面量=0 硬门禁 PASS / (c)自有端点=${ENDPOINT_N} 登记〔M1 期望 0，不判 FAIL〕） |
| \`selfcheck\` | ${SELFCHECK_SUMMARY}（host 侧 \`readelf\` 静态；★ 交叉产物 host **不能** \`ldd\`，运行期 \`ldd\` 归板端 T1.13） |
| \`usbproxy\` | sha256 \`${UBP_SHA}\`（= \`.sha256\` 声明值 ✅）；旧目录字面量=0 ✅；重建：${USBPROXY_REBUILD} |
| \`repro_verify\` | ${REPRO_RESULT} |
| \`librknnrt_version\` | \`${RKNN_VER}\` / sha256 \`${RKNN_SHA}\`（须 = 2.3.2 / \`d31fc19c…\`；基准 = 链接期=模型=板端 三重一致） |
| \`vector_hash\` | \`${VECTOR_HASH}\`（**配置向量**指纹，**不含 commit**——可复现单元 = (源码 commit, 配置向量) 二元组；换任一向量项即变 ⇒ 与旧留档**不可比**） |
| \`notes\` | 上次留档对照：${PREV_CMP}；A4①=${A4_HOSTPATH_N}（期望 0）/ A4②=${A4_TARGET_N}（期望 >=1）；CROSS_AARCH64=${CACHED_CROSS:-<未设>} |

## §5 单行记录（供仓库外备份共用）

\`\`\`
${GIT_COMMIT} | ${TOOLCHAIN_DESC} | ${CACHED_SYSROOT} | ${PRODUCT} | md5=${MD5} | BuildID=${BUILDID} | ${BUILD_UTC} | ${HOST_DESC} | ${STRINGS_GATE}
\`\`\`

<!-- RELEASE_BUILD_RECORD: commit=${GIT_COMMIT} md5=${MD5} sha256=${SHA256} buildid=${BUILDID} vector_hash=${VECTOR_HASH} -->
EOF
echo "[release] 留档已写：${MD_FILE}"
echo "[release] 留档摘要：commit=${GIT_COMMIT:0:7} md5=${MD5} buildid=${BUILDID:0:12}… vector_hash=${VECTOR_HASH:0:12}…"

echo "[release] 出货构建完成：${BUILD_DIR}"
echo "[release] 后续（委派既有脚本，本脚本不重复实现）："
echo "          bash ${REPO}/scripts/ttbox_release_install.sh    # 浇筑到 /opt/ttbox"
echo "          bash ${REPO}/scripts/ttbox_release_verify.sh     # 校验 RUNPATH 闭集/sha（真 release 树权威）"
echo "          bash ${REPO}/scripts/ttbox_release_selftest.sh   # 发布布局自测"
