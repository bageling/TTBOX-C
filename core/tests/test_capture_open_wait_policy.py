# -*- coding: utf-8 -*-
"""锁定 V4L2Capture::open() 里「格式稳定等待」的判据 —— 2026-10-04。

★ 为什么需要这条**源码级**测试（而不是一条能跑的 C++ 用例）：
  `V4L2Capture.cpp` 在 host 构建里**根本不参与编译**（本机没有 V4L2 硬件，
  CMake 只在 Unix 分支编它）⇒ 本机 ctest 对这 800ms 的逻辑**零覆盖**。
  这正是技能里记的「第 2 步假绿盲区」：改动落在这 5 个文件上时，
  本机全绿说明不了任何事。
  但"该不该等 800ms"这件事**在源码里就能判对错** —— 它是一段有明确结构的
  分支选择逻辑。所以这里用源码断言把它钉住：改坏了本机立刻红，
  而不是等出包上板之后由业主"感觉卡"来发现。

★ 它守的是什么（业主反馈"首次点击启动会卡顿"的服务端那一半）：
  板端实测 —— 冷路径 `POST /api/control/start` = 1.07s，热路径 = 0.25s，
  差的正是这段 800ms 盲等。而"冷路径"在真实使用里几乎每次都命中
  （点了停止、隔十几秒再点启动，必然超过 5s 的热重开窗口）。
  精确证据：盒子 uptime 4.6 天时连续 40 次**全新 open** 读到的格式
  全部是 2560/1440 BGR3，一次都没变 ⇒ 驱动早已过渡完毕，等待纯属浪费。

★ 判据口径（踩过的坑）：
  - **必须按 `open()` 函数体内的定位切片**，不能全文找 `sleep_for(800)` ——
    全文匹配一旦有人在别处也写 800ms 就会误判。
  - 断言用**结构**（这个 sleep 是不是落在最后一个兜底 else 里），
    不用行号 —— 行号一改就假绿。
"""
from __future__ import annotations

import re
from pathlib import Path

from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))
CAP_CPP = REPO / 'core' / 'src' / 'capture' / 'V4L2Capture.cpp'
CAP_HPP = REPO / 'core' / 'src' / 'capture' / 'V4L2Capture.hpp'

# open() 里第 4 步的边界：从这行注释到 fmt2 重读之前，就是整段等待逻辑。
STEP4_START = '// ---- 4. 等待格式稳定'
STEP4_END = 'struct v4l2_format fmt2'

SLEEP_LINE = 'std::this_thread::sleep_for(std::chrono::milliseconds(800));'


def _step4() -> str:
    src = CAP_CPP.read_text(encoding='utf-8')
    i = src.index(STEP4_START)
    j = src.index(STEP4_END, i)
    return src[i:j]


def _cpp() -> str:
    return CAP_CPP.read_text(encoding='utf-8')


def _hpp() -> str:
    return CAP_HPP.read_text(encoding='utf-8')


def _uptime_body() -> str:
    """boot_uptime_sec() 的函数体。

    ★ 用**花括号配平**取函数体，不用 `index('\\n}')`：后者会在函数体里
      第一个遇到的、缩进为 0 的 `}` 处截断 —— 本函数体内全是缩进花括号，
      恰好能用，但只要有人加一个同级的 lambda/struct 就会静默截短，
      断言于是变成在半截函数上做检查（假绿）。
    """
    cpp = _cpp()
    i = cpp.index('double boot_uptime_sec()')
    j = cpp.index('{', i)
    depth = 0
    for k in range(j, len(cpp)):
        if cpp[k] == '{':
            depth += 1
        elif cpp[k] == '}':
            depth -= 1
            if depth == 0:
                return cpp[i:k + 1]
    raise AssertionError('boot_uptime_sec() 花括号不配平')


# ---------------------------------------------------------------------------
# 1. 800ms 只许出现在「兜底」那一条分支里
# ---------------------------------------------------------------------------

def test_800ms_sleep_appears_exactly_once_in_the_whole_file():
    """多一处都意味着又有人加了一条"无条件等"。"""
    n = _cpp().count(SLEEP_LINE)
    assert n == 1, '整个文件里有 %d 处 800ms 盲等，应当只有 1 处（兜底分支）' % n


def test_800ms_sleep_lives_in_the_fallback_else():
    """★ 核心断言：这 800ms 必须是**最后一条兜底分支**，不能被提到前面无条件执行。

    怎么才会红（写断言先问这句）：
      · 把这个 sleep 从 `} else {` 挪到 `if (warm_reopen) {` 之前 ⇒ 红；
      · 把兜底 else 删掉、让 sleep 无条件跑 ⇒ `} else {` 不在它前面 ⇒ 红；
      · 把 sleep 移到别的分支（比如 boot_settled 那支）⇒ 前面的 `} else {` 变成
        `} else if (...) {` 的后续，前缀不再是 `} else {` ⇒ 红。
    """
    block = _step4()
    i = block.index(SLEEP_LINE)
    prefix = block[:i]
    assert '} else {' in prefix[-400:], (
        '这段 800ms 不在最后一条兜底 else 里 —— 它又被无条件执行了')


def test_step4_has_exactly_one_else_if():
    """结构 = ① 热重开 ② 盒子已启动够久 ⇒ 一条 else if + 一个兜底 else。

    多一条或少一条都说明判据被动过，必须回来看这一整段注释再改。
    """
    n = _step4().count('} else if (')
    assert n == 1, '第 4 步的分支数变了（现在 %d 个 else if），请复核判据' % n


# ---------------------------------------------------------------------------
# 2. 跳过等待的两条判据必须都在
# ---------------------------------------------------------------------------

def test_step4_checks_warm_reopen():
    block = _step4()
    assert 'warm_reopen' in block
    assert 'kWarmReopenWindowMs' in block, '热重开窗口常量被拿掉了'


def test_step4_checks_box_uptime():
    block = _step4()
    assert 'boot_uptime_sec()' in block, (
        '少了「盒子已启动多久」这条判据 ⇒ 冷路径又会白等 800ms（业主说的"首次点击卡顿"）')
    assert 'kBootSettleSec' in block


def test_warm_reopen_does_not_treat_never_closed_as_zero_gap():
    """`last_close_ms_ == 0` 表示「本进程从没关过设备」，**不是**「刚刚关过」。

    ★ 若写成 `now_ms - last_close_ms_ < kWarmReopenWindowMs` 且不加 0 的排除，
      开机后的第一次打开会被误判成热重开 ⇒ 恰好跳过"真正需要等"的那一次
      （这是"冷启动第一次可能拿到过渡中的格式"的成因）。
    """
    block = _step4()
    assert 'last_close_ms_ > 0.0' in block, (
        '又拿 0 当"刚刚关闭"用了 —— 开机后第一次打开会被误判成热重开')


# ---------------------------------------------------------------------------
# 3. uptime 读不到时必须走保守分支（慢可以，错不行）
# ---------------------------------------------------------------------------

def test_uptime_helper_fails_closed_to_unknown():
    """两条失败路径都必须落到 -1。

    ★ 写法坑：两条路径的形态**不一样** ——
      打不开是 `if (fp == nullptr) { return -1.0; }`（提前 return），
      读失败是 `if (fscanf(...) != 1) { up = -1.0; }`（改值后落到函数末尾的 return）。
      只数 `return -1.0;` 会数成 1 条然后误报失败（本文件第一版就这么错的）。
    """
    body = _uptime_body()
    assert 'return -1.0;' in body, 'fopen 失败路径没有返回 -1'
    assert 'up = -1.0;' in body, 'fscanf 读失败路径没有把结果置成 -1'
    assert '>= kBootSettleSec' in _step4(), (
        '判据不是 `>= kBootSettleSec` ⇒ -1（未知）可能被当成"已稳定"')


def test_uptime_helper_never_throws_or_aborts():
    """读 /proc 失败绝不能崩 —— 这段代码在采集初始化路径上，崩了设备就起不来。"""
    body = _uptime_body()
    for bad in ('throw', 'abort', 'assert(', 'exit('):
        assert bad not in body, 'boot_uptime_sec() 里出现了 %s ⇒ 失败会崩' % bad
    assert 'fclose(fp)' in body, 'fopen 成功路径没关文件描述符'


def test_boot_settle_threshold_is_documented_in_header():
    hpp = _hpp()
    m = re.search(r'kBootSettleSec\s*=\s*([0-9.]+)', hpp)
    assert m, 'header 里没有 kBootSettleSec'
    assert float(m.group(1)) == 60.0, (
        '阈值变成 %s 了：驱动注释说 hdmirx 过渡发生在"开机后 1~2 秒"，'
        '60s 是 30x 余量；往下调会削掉「开机后立刻点启动」这条真实路径的保护'
        % m.group(1))


# ---------------------------------------------------------------------------
# 4. 跳过后原有的「格式变了就重试」防线不许被删
# ---------------------------------------------------------------------------

def test_format_change_retry_is_still_there():
    """跳过等待**不等于**放弃校验：跳过后仍要重读 G_FMT 比对并重试。"""
    cpp = _cpp()
    assert 'V4L2 格式不稳定' in cpp, '格式变化的重试告警被删了'
    assert 'kMaxRetries' in cpp
    block = _step4()
    assert 'kMaxRetries' not in block  # 重试循环在 open() 更外层，不该被搬进第 4 步
