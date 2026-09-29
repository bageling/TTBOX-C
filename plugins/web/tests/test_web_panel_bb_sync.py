# test_web_panel_bb_sync.py — 面板（index.html）与后端（ttbox-web.py）的「一套口径」守卫
#
# 背景：辅助功能页在 2026-09-24 收敛成 5 个分区（压枪 / 自动开火 / 提前量 / 拟人化 / 选靶），
# 分区里的 127 个字段（2026-09-29 重数；删 lead1/trigger 后）由 index.html 的 BB_CTRL_MODULES **表驱动**读写，而这张表是从
# ttbox-web.py 的后端表机械生成的。两处一旦不同步，症状是"面板能调但存不下去"或者
# "存下去了但面板打开显示成别的值"—— 都不会报错，只会静默错。
#
# 本文件锁三件事：
#   ① 面板表 == 后端表（逐项字段名/类型/默认值）；
#   ② 老界面（老压枪速率模型 12 参数、连点 rapid_fire、自动开火占位编辑器）已经下线；
#   ③ 压枪三段查表的「三预设本地镜像」在切换预设时不丢另外两套数据
#      （这段是纯 JS 状态机，用 node 跑真代码验证；没有 node 就跳过）。
#
# 运行：python -m pytest plugins/web/tests/test_web_panel_bb_sync.py -v
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

import pytest

WEB_SRC = pathlib.Path(__file__).resolve().parents[3] / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
INDEX = pathlib.Path(__file__).resolve().parents[3] / 'plugins' / 'web' / 'templates' / 'index.html'

_load_seq = 0


def _load_backend():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_sync_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def _panel_source():
    return INDEX.read_text(encoding='utf-8')


def _panel_modules():
    """把 index.html 里的 BB_CTRL_MODULES 解析成 {前缀: [(字段, 类型, 默认值), ...]}。"""
    src = _panel_source()
    start = src.index('const BB_CTRL_MODULES = [')
    end = src.index('\n];', start)
    body = src[start + len('const BB_CTRL_MODULES = '):end + 2]
    body = re.sub(r',(\s*[\]\}])', r'\1', body)          # 去掉尾逗号，变成合法 JSON
    raw = json.loads(body)
    out = {}
    for prefix, fields in raw:
        out[prefix] = [(f[0], f[1], f[2]) for f in fields]
    return out


# ---------------------------------------------------------------------------
# ① 面板表 == 后端表
# ---------------------------------------------------------------------------

def test_panel_table_matches_backend_tables():
    mod = _load_backend()
    panel = _panel_modules()

    want = {prefix: [(n, k, d) for (n, k, d) in fields] for (prefix, _obj, fields) in mod.CTRL_BLOCKS}
    want['recoil_bb'] = [(n, k, d) for (n, k, d) in mod.CTRL_RECOIL_BB_FIELDS]
    want['selector'] = [(wk[len('selector_'):], k, d)
                        for (wk, _core, k, d) in mod.CTRL_SELECTOR_FIELDS]

    assert sorted(panel) == sorted(want), '面板模块前缀与后端不一致'

    for prefix in sorted(want):
        got = panel[prefix]
        exp = list(want[prefix])
        if prefix == 'recoil_bb':
            # ★ 唯一一处刻意不同：面板只有一个压枪总开关，不发 recoil_bb_enabled，
            #   由后端把 recoil.enabled 镜像过去（见 web_body_to_profile）。
            exp = [t for t in exp if t[0] != 'enabled']
            assert ('enabled', 'b', False) not in got
        assert got == exp, '模块 %s 的面板表与后端表不一致' % prefix


def test_panel_table_values_are_json_scalars():
    """表里的默认值必须是标量（数组字段走虚拟键单独处理，不进表）。"""
    for prefix, fields in _panel_modules().items():
        for name, kind, dflt in fields:
            assert kind in ('b', 'n', 'i', 'key'), (prefix, name, kind)
            assert isinstance(dflt, (bool, int, float)), (prefix, name, dflt)
            if kind == 'b':
                assert isinstance(dflt, bool), (prefix, name)
            if kind == 'key':
                assert isinstance(dflt, int), (prefix, name)


def test_panel_element_ids_cover_the_table():
    """每个表里的键都要有对应的界面元素：id = 前缀_字段（表里没有的界面元素会写不进配置）。"""
    src = _panel_source()
    ids = set(re.findall(r'\bid="([^"]+)"', src))
    missing = ['%s_%s' % (p, n) for p, fields in _panel_modules().items()
               for (n, _k, _d) in fields if '%s_%s' % (p, n) not in ids]
    assert missing == [], '表里有键但界面没渲染：%s' % missing


# ---------------------------------------------------------------------------
# ② 老界面已下线
# ---------------------------------------------------------------------------

def test_retired_panel_surfaces_are_gone():
    src = _panel_source()
    for token in ('recoil_strength', 'recoil_speed', 'recoil_humanize', 'recoil_trigger_delay',
                  'recoil_target_lost_release_ms', 'recoil_only_when_target_visible',
                  'rapid_fire', 'RAPID_FIRE', 'autoTrigger', 'auto-trigger',
                  'assist-section-rapid', 'unimplemented'):
        assert token not in src, '老界面残留：%s' % token


def test_panel_has_exactly_five_assist_sections():
    src = _panel_source()
    sections = re.findall(r'<section id="(assist-section-[a-z0-9-]+)"', src)
    assert sections == ['assist-section-recoil', 'assist-section-trigger',
                        'assist-section-lead', 'assist-section-humanize',
                        'assist-section-selector'], sections
    tabs = re.findall(r'data-assist-section-target="(assist-section-[a-z0-9-]+)"', src)
    assert tabs == sections
    # 五个页签都不该是 disabled（内核已实现，功能可进入）
    assert not re.search(r'data-assist-section-target="[^"]+"[^>]*\bdisabled\b', src)


# ---------------------------------------------------------------------------
# ③ 三段查表预设状态机（用 node 跑真代码）
# ---------------------------------------------------------------------------

_NODE_HINTS = ('node', 'nodejs')


def _node_bin():
    for name in _NODE_HINTS:
        found = shutil.which(name)
        if found:
            return found
    return None


_PRESET_HARNESS = r'''
const _fields = {};
globalThis.getNumber = (id, fallback) => (
  Object.prototype.hasOwnProperty.call(_fields, id) ? Number(_fields[id]) : fallback);
globalThis.setValue = (id, value) => { _fields[id] = String(value); };
globalThis.getCheckbox = () => false;
globalThis.setCheckbox = () => {};

%s

let failures = 0;
function check(cond, msg) {
  if (!cond) { failures += 1; console.log('FAIL: ' + msg); }
}
const seg = (kind, i) => _fields['recoil_bb_seg_' + kind + (i + 1)];
function typeInput(id, value) { _fields[id] = String(value); }

// ★ 切预设走的是 index.html 里**真实的 change 回调体**（下面那段从源文件抠出来），
//   不是测试自己重写一遍 —— 否则回调漏掉"先收当前这组"这一步，测试也照样绿。
function switchPreset(next) {
  typeInput('recoil_bb_preset', next);
  (function () {
%s
  })();
}

populateRecoilBbPresets({});
check(_fields['recoil_bb_preset'] === undefined, '空 profile 不动预设索引');
typeInput('recoil_bb_preset', 1);
recoilBbPresetState.activeIndex = recoilBbPresetIndexFromPanel();
renderRecoilBbPresetInputs();
check(Number(seg('v', 0)) === 1.5, '预设1 第1段垂直 = 默认 1.5');
check(Number(_fields['recoil_bb_seg_total']) === 1500, '预设1 总时长 = 默认 1500');

typeInput('recoil_bb_seg_total', 1111);
typeInput('recoil_bb_seg_v1', 9.9);
switchPreset(2);
check(Number(seg('v', 0)) === 1.5, '切到预设2后显示自己的默认值');
check(Number(_fields['recoil_bb_seg_total']) === 1500, '预设2 总时长 = 默认 1500');

typeInput('recoil_bb_seg_total', 2222);
typeInput('recoil_bb_seg_v1', 8.8);
typeInput('recoil_bb_seg_h3', -3.5);
const payload = collectRecoilBbPresets();
check(payload.recoil_bb_preset_total_time_ms[0] === 1111,
  '预设1 的 1111 没被预设2 覆盖');
check(payload.recoil_bb_preset_total_time_ms[1] === 2222, '预设2 的 2222 落进下标 1');
check(payload.recoil_bb_preset_total_time_ms[2] === 1500, '预设3 未被碰过');
check(payload.recoil_bb_preset_vert[0][0] === 9.9, '预设1 垂直第1段落进 [0][0]');
check(payload.recoil_bb_preset_vert[1][0] === 8.8, '预设2 垂直第1段落进 [1][0]');
check(payload.recoil_bb_preset_vert[2][2] === 1.6, '预设3 第3段保留 Core 默认');
check(payload.recoil_bb_preset_horiz[1][2] === -3.5, '水平第3段落进 [1][2]');
check(payload.recoil_bb_preset_horiz[0][0] === 0, '预设1 水平未被串味');

switchPreset(1);
check(Number(_fields['recoil_bb_seg_total']) === 1111, '切回预设1 找回 1111');
check(Number(seg('v', 0)) === 9.9, '切回预设1 找回 9.9');

// 索引越界要夹住，不能写出第 4 组把 Core 的表写坏
typeInput('recoil_bb_preset', 99);
check(recoilBbPresetIndexFromPanel() === 2, '预设号 99 夹到第 3 组');
typeInput('recoil_bb_preset', 0);
check(recoilBbPresetIndexFromPanel() === 0, '预设号 0 夹到第 1 组');
typeInput('recoil_bb_preset', '');
check(recoilBbPresetIndexFromPanel() === 0, '空预设号回落第 1 组');

// 结构不对的返回体不能被当成有效数据
populateRecoilBbPresets({recoil_bb_preset_total_time_ms: [1, 2],
                         recoil_bb_preset_vert: [[1, 2], [3, 4]]});
check(recoilBbPresetState.total.length === 3, '长度不对的数组整套丢弃');
check(recoilBbPresetState.vert.length === 3, '形状不对的表整套丢弃');
check(recoilBbPresetState.vert[2][2] === 1.6, '丢弃后落回默认值');

console.log('failures=' + failures);
process.exit(failures ? 1 : 0);
'''


def _preset_change_handler_body(src):
    """抠出 index.html 里 recoil_bb_preset 的 change 回调体（真实代码，不是重写版）。"""
    m = re.search(r'on\("recoil_bb_preset", "change", \(\) => \{\n(.*?)\n  \}\);', src, re.S)
    assert m, '找不到 recoil_bb_preset 的 change 监听'
    return m.group(1)


def test_recoil_bb_preset_state_machine_keeps_all_three_sets():
    node = _node_bin()
    if not node:
        pytest.skip('环境里没有 node，跳过 JS 状态机验证')

    src = _panel_source()
    start = src.index('const RECOIL_BB_SEG_DEFAULTS = {')
    end = src.index('\nfunction populateForm(config) {', start)
    block = src[start:end]

    script = _PRESET_HARNESS % (block, _preset_change_handler_body(src))
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ttbox_preset_')) / 'harness.js'
    tmp.write_text(script, encoding='utf-8', newline='')
    proc = subprocess.run([node, str(tmp)], capture_output=True, text=True)
    assert proc.returncode == 0, '预设状态机用例失败：\n%s\n%s' % (proc.stdout, proc.stderr)
    assert 'failures=0' in proc.stdout, proc.stdout
