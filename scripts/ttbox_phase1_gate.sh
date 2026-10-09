#!/usr/bin/env bash
# ttbox_phase1_gate.sh — TTBOX 第一阶段门禁（老板拍板 23 项的可机械校验子集）
#
# 用法：
#   bash scripts/ttbox_phase1_gate.sh                  # 快检（G1/G2/G3/G4/G5/G7）
#   bash scripts/ttbox_phase1_gate.sh --with-tests     # 追加 G6（ctest + pytest，慢）
#   bash scripts/ttbox_phase1_gate.sh --emit-longfile  # 打印存量 >300 行文件清单（生成登记表用）
#
# 依赖：git bash（find/grep/awk）、python3（复用 ttbox_conventions_gate.sh 的口径门禁）。
# 退出码：0 = 全过；1 = 有 FAIL。WARN 不影响退出码。
#
# 口径依据：docs/architecture/代码书写规矩-技术版-2026-10-01.md §4/§6/§7/§8/§11。

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 2

FAIL=0
ok()  { echo "[gate][PASS] $*"; }
bad() { echo "[gate][FAIL] $*"; FAIL=1; }
warn(){ echo "[gate][WARN] $*"; }

SRC_DIRS="core/src core/include usbproxy plugins scripts tools tests"
find_src() {
  for d in $SRC_DIRS; do
    find "$d" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \
      -o -name '*.py' -o -name '*.sh' -o -name '*.lua' \) 2>/dev/null
  done
}

# ---------------------------------------------------------------- G1 行数
# 规则（《代码书写规矩·技术版》§4.2）：
#   ① >300 行的源文件必须登记在 scripts/phase1_longfile_registry.txt；
#      登记表 = 存量快照；任何新出现的不在表内的超长文件 = FAIL（新增代码硬卡 300 行）。
#   ② 棘轮：当前行数不得跨过「登记行数所在的下一个 100 整数位」。
#      ★ 为什么补 ②（2026-10-01）：此前只判 ① ⇒ 已登记的超长文件涨到任意行数都没人拦。
#      本轮 ttbox-web.py 涨过 6100 是**人工比对**才发现的，门禁全程 PASS —— 假护栏。
#      跨位了只有两条路：压回去，或拆文件；**不许改登记表糊过去**（改了就是自己给自己发豁免）。
G1_LONGFILE_REG="scripts/phase1_longfile_registry.txt"

if [ "${1:-}" = "--emit-longfile" ]; then
  find_src | while read -r f; do
    n=$(awk 'END{print NR}' "$f")
    [ "$n" -gt 300 ] && echo "$n|$f"
  done | sort -t'|' -k1,1rn
  exit 0
fi

if [ ! -f "$G1_LONGFILE_REG" ]; then
  bad "G1 缺登记表 $G1_LONGFILE_REG（用 --emit-longfile 生成后人工确认入库）"
else
  # 登记表格式（2026-10-01 起）：<登记行数>|<路径>，与 --emit-longfile 输出同构。
  tmp_g1=$(mktemp)
  find_src | while read -r f; do
    n=$(awk 'END{print NR}' "$f")
    [ "$n" -gt 300 ] && echo "$n|$f"
  done > "$tmp_g1"
  miss=0
  while IFS='|' read -r n f; do
    # 用 awk 精确比对路径（不用 grep -F "|$f"：`a/b.cpp` 会误匹配 `xa/b.cpp`）。
    reg_n=$(awk -F'|' -v p="$f" '$2==p {print $1; exit}' "$G1_LONGFILE_REG")
    if [ -z "$reg_n" ]; then
      bad "G1 超长文件未登记: $f ($n 行) —— 用 --emit-longfile 生成并人工确认后入库"
      miss=1
      continue
    fi
    lim=$(( (reg_n / 100 + 1) * 100 - 1 ))
    if [ "$n" -gt "$lim" ]; then
      bad "G1 超长文件跨过 100 行位: $f 现 $n 行 > 上限 $lim（登记 $reg_n）—— 压回去或拆文件，禁改登记表"
      miss=1
    fi
  done < "$tmp_g1"
  rm -f "$tmp_g1"
  [ "$miss" -eq 0 ] && ok "G1 超长文件全部已登记且未跨 100 行位（$(wc -l < "$G1_LONGFILE_REG" | tr -d ' ') 条）"
fi

# ---------------------------------------------------------------- G2 空处理
# C++ 空 catch；Python except:pass（同行或下一行）/ 裸 except:。
# 存量已知项走基线棘轮（scripts/phase1_gate_baseline.txt）；实际 > 基线 = FAIL，< 基线 = WARN 提示收缩。

# Python 解释器：优先 python3，缺失时回退本机托管 venv（不引入环境变量，避免口径门禁②登记负担）
# Python 解释器：**按能力挑，不写死本机路径**。
#   为什么不能只判"存在"：本机 PATH 里的 python3 完全可能指向一个没装 pytest 的裸解释器，
#   挑到它 G6 就会误判 FAIL。所以候选逐个试 `import <模块>`，谁能跑用谁。
#   候选顺序：TTBOX_PYTHON（显式指定，登记于 docs/protocols/config-path-env-registry.md）
#             → python3 → python → $HOME/.workbuddy/binaries/python/envs/default/Scripts/python.exe
#   最后一个用 $HOME 而不是写死用户名：家目录是标准概念，不属于"某个人的机器"。
#   ★ 原实现把开发机托管 venv 的 "C:/Users/Administrator/..." 直接当兜底 —— 换台机器就废，
#     而且违反口径门禁⑪（禁开发机绝对路径）。
pick_python() {   # $1 = 必须能 import 的模块名；空串 = 只要解释器能起
    local mod="${1:-}" c
    for c in ${TTBOX_PYTHON:-} python3 python \
             "$HOME/.workbuddy/binaries/python/envs/default/Scripts/python.exe"; do
        command -v "$c" >/dev/null 2>&1 || continue
        if [ -z "$mod" ]; then printf '%s' "$c"; return 0; fi
        "$c" -c "import $mod" >/dev/null 2>&1 && { printf '%s' "$c"; return 0; }
    done
    return 1
}

PYEXE="$(pick_python '')"
if [ -z "$PYEXE" ]; then
    echo "[gate][ERROR] 找不到可用的 Python 解释器（G2、G4 的扫描需要）；可用 TTBOX_PYTHON=<路径> 指定" >&2
    exit 2
fi
cpp_empty=$(grep -rnE --include='*.cpp' --include='*.hpp' --include='*.h' \
  -e 'catch[[:space:]]*\([^)]*\)[[:space:]]*\{[[:space:]]*\}' \
  core/src core/include usbproxy 2>/dev/null | wc -l | tr -d ' ')

py_empty=$("$PYEXE" - <<'PYEOF'
import os, re, sys
root = os.getcwd()
dirs = ["plugins", "framework", "ttbox_platform", "scripts", "tools", "tests"]
pat_open = re.compile(r'^\s*except\b[^:]*:\s*(#.*)?$')
hits = []
for d in dirs:
    for dp, _, fns in os.walk(d):
        for fn in fns:
            if not fn.endswith(".py"):
                continue
            p = os.path.join(dp, fn)
            try:
                lines = open(p, encoding="utf-8", errors="replace").read().splitlines()
            except OSError:
                continue
            for i, ln in enumerate(lines):
                if re.match(r'^\s*except\b[^:]*:\s*pass(\s|#|$)', ln):
                    hits.append(p); break
                if pat_open.match(ln):
                    for nxt in lines[i+1:i+3]:
                        s = nxt.strip()
                        if not s or s.startswith("#"):
                            continue
                        if s == "pass":
                            hits.append(p); break
                        break
print(len(hits))
PYEOF
)

# ---------------------------------------------------------------- G3 硬编码板端路径
# 白名单：core/src/common/Paths.hpp、plugins/web/lib/paths.py（路径单点真源）。
hw_paths=$(grep -rnE --include='*.py' --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.sh' --include='*.lua' \
  -e '/opt/ttbox' -e '/var/lib/ttbox' -e '/etc/ttbox' -e '/run/ttbox' \
  core/src core/include plugins usbproxy framework ttbox_platform ttbox_motion 2>/dev/null \
  | grep -vE 'common/Paths\.hpp|web/lib/paths\.py' | wc -l | tr -d ' ')

# ---------------------------------------------------------------- 基线棘轮
BASELINE="scripts/phase1_gate_baseline.txt"
read_base() { # $1=key → count（缺省 0）
  awk -v k="$1" '$1==k{print $2; found=1} END{if(!found) print 0}' "$BASELINE" 2>/dev/null
}
ratchet() { # $1=key $2=actual $3=label
  b=$(read_base "$1")
  if [ "$2" -gt "$b" ]; then
    bad "$3 实际 $2 > 基线 $b（禁止新增，存量须只减不增）"
  elif [ "$2" -lt "$b" ]; then
    warn "$3 实际 $2 < 基线 $b —— 有存量已清零，请同步收缩 scripts/phase1_gate_baseline.txt"
    ok "$3（较基线净减 $((b - 2 > 0 ? b - 2 : 0)) 由 WARN 提示跟踪）"
  else
    ok "$3 = $2（与基线持平）"
  fi
}

if [ ! -f "$BASELINE" ]; then
  bad "G2/G3 缺基线文件 $BASELINE"
else
  ratchet "G2cpp" "$cpp_empty" "G2 C++空catch"
  ratchet "G2py"  "$py_empty"  "G2 Python吞异常"
  ratchet "G3"    "$hw_paths"  "G3 硬编码板端路径"
fi

# ---------------------------------------------------------------- G4 预留接口登记
RESERVED_REG="docs/architecture/预留接口登记表-2026-10-01.md"
res_markers=$(grep -rnE 'RESERVED\(' core/src core/include usbproxy plugins framework ttbox_platform ttbox_motion scripts tools 2>/dev/null | wc -l | tr -d ' ')
res_no_expiry=$(grep -rnE 'RESERVED\(' core/src core/include usbproxy plugins framework ttbox_platform ttbox_motion scripts tools 2>/dev/null \
  | grep -vE 'expiry=[0-9]{4}-[0-9]{2}-[0-9]{2}' | wc -l | tr -d ' ')
res_rows=0
if [ -f "$RESERVED_REG" ]; then
  res_rows=$(grep -cE '^\| R-' "$RESERVED_REG" 2>/dev/null || true)
  res_rows=$(printf '%s' "$res_rows" | head -n1)
  res_rows=${res_rows:-0}
fi
if [ "$res_no_expiry" -gt 0 ]; then
  bad "G4 有 RESERVED 标记缺 expiry=$res_no_expiry 处"
elif [ "$res_markers" -ne "$res_rows" ]; then
  bad "G4 代码 RESERVED 标记 $res_markers 处 ≠ 登记表 $res_rows 行（两边必须一致）"
else
  ok "G4 预留接口登记一致（$res_markers 处）"
fi

# ---------------------------------------------------------------- G5 口径（含版本真源）
if bash scripts/ttbox_conventions_gate.sh >/tmp/ttbox_g5.log 2>&1; then
  ok "G5 口径门禁（7 组，含⑥版本同值）"
else
  bad "G5 口径门禁失败（详见 /tmp/ttbox_g5.log）"; sed -n '1,10p' /tmp/ttbox_g5.log
fi

# ---------------------------------------------------------------- G7 制品
pyc_tracked=$(git ls-files 2>/dev/null | grep -E '__pycache__/|\.pyc$' | wc -l | tr -d ' ')
exe_tracked=$(git ls-files 2>/dev/null | grep -E '\.exe$' | wc -l | tr -d ' ')
[ "$pyc_tracked" -eq 0 ] && ok "G7 无 pycache/pyc 入库" || bad "G7 pycache/pyc 入库 $pyc_tracked 个"
ratchet "G7exe" "$exe_tracked" "G7 .exe 入库"

# ---------------------------------------------------------------- G6 测试（可选）
if [ "${1:-}" = "--with-tests" ]; then
  # 宿主构建目录：可覆盖；默认沿用约定路径（本机随时重建得出来，不是私有数据）。
  HB="${HB:-C:/ttbox-hostbuild}"
  if [ -d "$HB" ] && command -v ctest >/dev/null 2>&1; then
    # 判据**直接用 ctest 的退出码**（全过 = 0）。
    #   ★ 原实现 grep 的是 `ctest 2>&1 | tail -1` —— 但 ctest 输出最后一行是
    #     "Total Test time (real) = …"，汇总行 "100% tests passed" 在**其上一行**
    #     ⇒ 该判据恒假，G6 ctest 永远 FAIL（一直没暴露是因为平日都跑不带 --with-tests 的快检）。
    if (cd "$HB" && ctest >/dev/null 2>&1); then
      ok "G6 ctest 全绿"
    else
      bad "G6 ctest 未全绿（复现：cd $HB && ctest）"
    fi
  else
    warn "G6 未跑（缺 $HB 或 ctest）"
  fi
  # pytest 同样用**能力探测**挑解释器（本机 python3 很可能是没装 pytest 的裸解释器）。
  PYV="$(pick_python pytest)"
  if [ -n "$PYV" ]; then
    # ★ --basetemp 必须给：不给的话 pytest 收尾会去清 pytest-of-*/garbage-*（数百个文件），
    #   被安全删除策略拦下 ⇒ 退出码 1 且汇总行不打印 —— 测试其实全绿，却会被这里判成 FAIL。
    BT="$(mktemp -d)"
    pfail=0
    for t in framework/tests ttbox_platform/tests plugins/web/tests; do
      "$PYV" -m pytest "$t" -q --import-mode=importlib --basetemp="$BT/$(echo "$t" | tr / _)" \
        >/dev/null 2>&1 || { bad "G6 pytest $t 未全绿"; pfail=1; }
    done
    rm -rf "$BT" 2>/dev/null || true
    [ "$pfail" -eq 0 ] && ok "G6 pytest 三套件全绿"
  else
    warn "G6 未跑（找不到带 pytest 的解释器；可用 TTBOX_PYTHON=<路径> 指定）"
  fi
else
  echo "[gate][SKIP] G6 测试未跑（加 --with-tests 启用）"
fi

# ----------------------------------------------------------------
echo "----------------------------------------"
if [ "$FAIL" -eq 0 ]; then
  echo "[gate] 全部通过（G8 目录归属为人工审查项，不在本脚本）"
  exit 0
else
  echo "[gate] 存在 FAIL，禁止进入下一批次"
  exit 1
fi
