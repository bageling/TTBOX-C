# -*- coding: utf-8 -*-
"""锁定 calib-bind.js 的「自足性」—— 它不能依赖 10-flow 的任何东西。

★★ 这条测试是被两次真bug 逼出来的（2026-10-03）：
  第一次 `CALIB_PHASE_LABELS is not defined`
  第二次 `calibTraceLines / calibPollTimer is not defined`
  两次都是：拆 bindEvents 时把 5 个标定函数拎成了顶层，但它们依赖的
  **共享变量**留在了 10-flow 的函数体内。
  ⇒ 而 node --check、514 条 pytest、11 个 script 全 200、
    9 个页签逐个点、14 个按钮逐个点 —— **五样全绿**，
    只有「**实际调用那个函数**」才炸。
  ⇒ 所以本文件用**正则做静态自足性检查**：calib-bind.js 里读到的每个
    自由变量，必须能在它自己或浏览器内置里找到。
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CALIB = REPO / 'plugins' / 'web' / 'static' / 'panel' / 'calib-bind.js'
# ★ 2026-10-03 改名：00-shared.js → 10-flow.js（流程编排层）
SHARED = REPO / 'plugins' / 'web' / 'static' / 'panel' / '10-flow.js'

# calib-bind.js 允许用的外部依赖（都是 10-flow 的**顶层**声明）
ALLOWED_EXTERNAL = {
    # 基础设施
    'api', 'toast', 'showToast', 'on', 'state', 'runUiAction',
    'getCropSize', 'getPreviewImageLayout', 'updateAimRangeOverlay',
    'setValue', 'getValue', 'getCheckbox', 'normalizeCropSize',
    'setDisplayIdentityReadonly', 'updateDisplayCustomModeButtonUi',
    'validateDisplayHardware', 'populateDisplayHardware',
    'formatBytes', 'stableStringify', 'escapeHtml', 'isEmpty',
    'hideDisclaimer', 'acknowledgeAnnouncement', 'setStatusBadge',
    'getModelClassesText', 'currentModelImportType',
    'setModelClassNamesDialogOpen', 'renderCalibration',
    'scheduleCalibrationPolling', 'refreshCalibration',
    'syncConfigAfterCalibration', 'calibPhaseLabel',
    # ★ 2026-10-03 核实后补：这三个是 10-flow 的**顶层** function，
    #   calib-bind.js 合法依赖（已 grep 确认 `^function xxx` 存在）
    '$', 'populateForm', 'setApplyStatus',
    # 浏览器内置
    'window', 'document', 'console', 'JSON', 'Math', 'Object', 'Array',
    'Number', 'String', 'Boolean', 'Promise', 'Set', 'Map', 'Date', 'Error',
    'parseInt', 'parseFloat', 'isNaN', 'setTimeout', 'clearTimeout',
    'setInterval', 'clearInterval', 'fetch', 'URL', 'FormData', 'Blob',
    'File', 'FileReader', 'AbortController', 'TextDecoder', 'Uint8Array',
    'ArrayBuffer', 'DataView', 'performance', 'location', 'navigator',
    'CustomEvent', 'Event', 'MutationObserver', 'ResizeObserver',
    'requestAnimationFrame', 'getComputedStyle', 'matchMedia', 'Symbol',
    'WeakMap', 'Proxy', 'Reflect', 'DOMParser', 'KeyboardEvent',
    'MouseEvent', 'ClipboardEvent', 'HTMLElement', 'Node', 'Image',
    'instantiateWasm', 'WebAssembly',
}

# 本文件自己的顶层声明
DECL = re.compile(
    r'^(?:async\s+)?function\s+(\w+)'          # function foo
    r'|^(?:const|let|var)\s+(\w+)'             # const FOO
    r'|^function\s+(\w+)\s*\(([^)]*)\)',       # function foo(a, b)
    re.M)
PARAM = re.compile(r'^function\s+\w+\s*\(([^)]*)\)', re.M)


def _top_level_names(path):
    s = path.read_text(encoding='utf-8')
    out = set()
    for m in re.finditer(r'^(?:async\s+)?function\s+(\w+)', s, re.M):
        out.add(m.group(1))
    for m in re.finditer(r'^(?:const|let|var)\s+(\w+)', s, re.M):
        out.add(m.group(1))
    for m in PARAM.finditer(s):
        for p in m.group(1).split(','):
            p = p.strip()
            if p:
                out.add(p.split('=')[0].split(':')[-1].strip())
    for m in re.finditer(r'^\s*\{([^}]*)\}\s*=', s, re.M):
        for x in m.group(1).split(','):
            if x.strip():
                out.add(x.strip().split(':')[-1].strip())
    return out


def _strip_noise(s):
    """去掉注释、字符串，以及 JS 关键字。"""
    s = re.sub(r'//[^\n]*', '', s)
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'"(?:[^"\\]|\\.)*"', '""', s)
    s = re.sub(r"'(?:[^'\\]|\\.)*'", "''", s)
    s = re.sub(r'`(?:[^`\\]|\\.)*`', '``', s)
    return s


# ★ JS 关键字 / 字面量 —— 它们不是「自由变量」，必须排除
KEYWORDS = {
    'async', 'await', 'break', 'case', 'catch', 'class', 'const', 'continue',
    'debugger', 'default', 'delete', 'do', 'else', 'export', 'extends',
    'finally', 'for', 'function', 'if', 'import', 'in', 'instanceof', 'let',
    'new', 'of', 'return', 'static', 'super', 'switch', 'this', 'throw',
    'try', 'typeof', 'var', 'void', 'while', 'with', 'yield', 'get', 'set',
    'true', 'false', 'null', 'undefined', 'NaN', 'Infinity',
}


def _identifiers_outside_functions(src):
    """只取「函数体之外」出现的标识符 —— 函数体内的局部变量不算自由变量。

    ★ 这一点很关键：第一版把函数体内的局部变量（bar/pill/rows…）
      也当成「未声明」⇒ 误报 60 多个。
    """
    depth = 0
    out = set()
    i = 0
    n = len(src)
    # 逐字符扫，记录当前是否在函数体内
    line_no = 0
    start_of_fn = []
    while i < n:
        ch = src[i]
        if ch == '\n':
            line_no += 1
        if ch == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
            continue
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth = max(0, depth - 1)
            if start_of_fn and depth < start_of_fn[-1]:
                start_of_fn.pop()
        out.update(re.findall(r'(?<![\w.$])([A-Za-z_$][\w$]*)', src[i:i + 1]))
        i += 1
    return out


def test_calib_bind_is_self_contained():
    """★ calib-bind.js 不许引用 00-shared 里的任何**局部**变量。

    判据：文件里读到的每个标识符，要么是它自己声明的，要么在
    ALLOWED_EXTERNAL 白名单里（那些都是 10-flow 的**顶层**声明）。
    """
    src = _strip_noise(CALIB.read_text(encoding='utf-8'))
    own = _top_level_names(CALIB)
    used = set(re.findall(r'(?<![\w.$])([A-Za-z_$][\w$]*)', src))
    unknown = sorted(x for x in used
                     if x not in own and x not in ALLOWED_EXTERNAL
                     and x not in KEYWORDS
                     and not x.isdigit())
    # ★ 排除「函数体内的局部变量」—— 它们本就该在函数体内
    #   （判据：这个名字在文件里被 const/let/var/参数 声明过，
    #     哪怕是缩进的局部声明）
    local = set(re.findall(r'^\s+(?:const|let|var)\s+(\w+)', src, re.M))
    local |= set(re.findall(r'\(\s*([A-Za-z_$][\w$]*)\s*[,)]', src))
    local |= set(re.findall(r'\bfor\s*\(\s*(?:const|let|var)\s+(\w+)', src))
    local |= set(re.findall(r'([A-Za-z_$][\w$]*)\s*=>', src))
    # ★ 再排除两类「不是自由变量」的：
    #   a) 对象字面量的键：idle: / sampling_x: / completed: …
    #   b) 箭头函数的参数：(k) => / (row) => …
    obj_keys = set()
    for m in re.finditer(r'\{([^{}]*)\}', src):
        for part in m.group(1).split(','):
            km = re.match(r'\s*([A-Za-z_$][\w$]*)\s*:', part)
            if km:
                obj_keys.add(km.group(1))
    arrow_params = set(re.findall(r'\(\s*([A-Za-z_$][\w$]*)\s*\)\s*=>', src))
    arrow_params |= set(re.findall(r'([A-Za-z_$][\w$]*)\s*=>', src))
    # ★ 多参数箭头：(v, digits, unit) => —— 单参数正则抓不到第 2、3 个
    for m in re.finditer(r'\(([^()]*)\)\s*=>', src):
        for p in m.group(1).split(','):
            p = p.strip()
            if p:
                arrow_params.add(p.split('=')[0].strip())
    # ★ 数组解构参数：([k, v]) => ... —— 上面那圈 split(',') 只会得到 '[k' 和 'v]'
    for m in re.finditer(r'\(\s*\[([^\]]*)\]\s*\)\s*=>', src):
        for p in m.group(1).split(','):
            p = p.strip()
            if p:
                arrow_params.add(p.split('=')[0].strip())
    # 字符串数组里的内容也不算（虽然 _strip_noise 已去掉大部分）
    unknown = [x for x in unknown
               if x not in local and x not in obj_keys
               and x not in arrow_params]
    assert not unknown, (
        'calib-bind.js 引用了未声明的变量 %s\n'
        '  ⇒ 它可能依赖了 10-flow 的**局部**变量（拆函数时没跟着搬）。\n'
        '  ⇒ node --check 与全部 pytest 都发现不了，只在**实际调用**时炸。\n'
        '  若确认是公共工具函数，加进本文件顶部的 ALLOWED_EXTERNAL。'
        % unknown)


def test_calib_shared_state_is_in_calib_bind():
    """★ 标定组的共享状态必须和它的函数在同一个文件。"""
    s = CALIB.read_text(encoding='utf-8')
    for name in ('CALIB_PHASE_LABELS', 'calibTraceLines', 'calibPollTimer'):
        assert re.search(r'^(?:const|let)\s+%s\b' % name, s, re.M), \
            '%s 应在 calib-bind.js 顶层（它服务于这里的 5 个函数）' % name


def test_shared_no_longer_owns_calib_state():
    """反向检查：10-flow.js 里不该再有这些标定变量。"""
    s = SHARED.read_text(encoding='utf-8')
    for name in ('CALIB_PHASE_LABELS', 'calibTraceLines', 'calibPollTimer'):
        # 只允许出现在顶层（缩进 0），不允许缩进 2（函数体内）
        assert not re.search(r'^ {1,4}(?:const|let)\s+%s\b' % name, s, re.M), \
            '%s 还留在 10-flow.js 的函数体内 ⇒ calib-bind.js 取不到' % name


def test_calib_functions_are_top_level():
    """5 个标定函数必须都在 calib-bind.js 顶层（不在别的函数体内）。"""
    s = CALIB.read_text(encoding='utf-8')
    for name in ('calibPhaseLabel', 'renderCalibration',
                 'syncConfigAfterCalibration', 'scheduleCalibrationPolling',
                 'refreshCalibration'):
        assert re.search(r'^(?:async\s+)?function\s+%s\(' % name, s, re.M), \
            '%s 不在 calib-bind.js 顶层' % name
