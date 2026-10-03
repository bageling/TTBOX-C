# test_web_ota_version_scheme.py — 「V 线」版本号方案的回归锁（业主 2026-09-29 令）
#
# 背景：业主令「全面改版本号：1.5.70 改成 V1.0.01」。这不是改一个字符串那么简单 ——
#   板端判「这个包能不能升」有两道闸，**都只看版本排序，不看别的东西**：
#     ① 更新器 `scripts/ttbox_ota_updater.py::is_downgrade`
#        ⇒ 包版本不高于当前版本 = `downgrade_rejected`，直接拒装；
#     ② 面板 `plugins/web/bin/ttbox-web.py` 的 `update_available`
#        （`_ota_ver_key(ver) > _ota_ver_key(cur)`）⇒ 不高于当前版本 = 显示「已是最新」，连按钮都没有。
#   现网盒子装的是 `1.5.68` / `1.5.70`。若新品版本写成**纯数字** `1.0.01`，两道闸都会判它更旧
#   ⇒ 永远升不上来。更麻烦的是：这两段判定代码**跑在板子已装的旧版本里**，
#   改仓库代码救不了现网盒子 —— 只有「新版本串在旧代码眼里就是更新的」这一条路可走。
#
#   而这三处同源实现（更新器 / 面板 / 云端 bridge ota.js::versionKey）的规则是：
#   **数字段按数值比、非数字段按字典序比，且非数字段的排序权重高于数字段**（tag 1 > tag 0）
#   ⇒ 带字母前缀的 `"V1.0.01"` 排在 `"1.5.70"` **之上**。这就是「V 必须带着」的真正原因，
#   不是审美问题：去掉 V 就等于切回了被拒绝的那条路。
#
# 本文件锁死四件事：
#   ① 从**真实源码**里抽出 `_version_key` / `_ota_ver_key` / `is_downgrade` 执行 ——
#      抽真源码而不是抄一份（抄的那份，源码改了它也不会红，等于假护栏）；
#   ② `"V1.0.01"` 排序 > `"1.5.68"` / `"1.5.70"`；且 `is_downgrade("V1.0.01", "1.5.68") is False`；
#   ③ 反面同样钉住：纯数字 `"1.0.01"` 排序**低于** `"1.5.70"`（说清为什么不能去掉 V）；
#   ④ 当前 `version.hpp::kCoreVersion` 必须落在 V 线，且排序高于数字线。
#
# 云端 bridge 的 `versionKey` 只在服务器上（仓库无副本），它的同源性用命令核：
#   node -e '…读 /opt/ttbox-server/bridge/src/ota.js…'（见 ttbox-ops 技能 04-云端授权与OTA.md）
#
# 运行：python -m pytest plugins/web/tests/test_web_ota_version_scheme.py -v
from __future__ import annotations

import ast
import pathlib
import re

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
UPDATER_SRC = REPO_ROOT / 'scripts' / 'ttbox_ota_updater.py'
# ★ 2026-10-02（web 换写法 S6）：_ota_ver_key 已搬到 lib/ota.py。面板侧的版本排序规则
#   现在住在这个文件里 —— 抽真源码的目标跟着走，规则本身一个字没改。
OTA_SRC = REPO_ROOT / 'plugins' / 'web' / 'lib' / 'ota.py'
VERSION_HPP = REPO_ROOT / 'core' / 'include' / 'ttbox' / 'core' / 'version.hpp'

V_LINE = 'V1.0.01'
# 现网盒子可能装的版本（切 V 线时的比较基准）
NUMERIC_LINE = ('1.5.63', '1.5.64', '1.5.67', '1.5.68', '1.5.70')


def _extract_functions(path: pathlib.Path, names: set) -> dict:
    """从源码里抽出指定顶层函数的**真实源码段**并编译执行，返回 {函数名: 可调用对象}。

    - 只抽被点名的函数；`_version_key` 是 `is_downgrade` 的依赖，一并抽出来。
    - 找不到函数就 fail（防「函数改名后测试静默变空跑」——那正是假护栏的典型形状）。
    """
    src = path.read_text(encoding='utf-8')
    tree = ast.parse(src)
    segs = []
    found = set()
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name in names:
            seg = ast.get_source_segment(src, node)
            assert seg, 'ast 未能取出 %s::%s 的源码段' % (path.name, node.name)
            segs.append(seg)
            found.add(node.name)
    missing = names - found
    assert not missing, '%s 里找不到函数: %s' % (path, sorted(missing))
    ns = {'re': re}
    exec('\n\n'.join(segs), ns)  # noqa: S102 —— 执行的是仓库自身的源码段，测试期专用
    return {n: ns[n] for n in names}


def _norm_quotes(src: str) -> str:
    """把单引号统一成双引号，好让「源码形状」断言不受引号风格影响。"""
    return src.replace("'", '"')


@pytest.fixture(scope='module')
def updater():
    return _extract_functions(UPDATER_SRC, {'_version_key', 'is_downgrade'})


@pytest.fixture(scope='module')
def web_key():
    return _extract_functions(OTA_SRC, {'_ota_ver_key'})['_ota_ver_key']


def test_v_prefix_sorts_above_the_numeric_line(updater, web_key):
    """两道闸都靠这个排序；更新器与面板两处实现必须一致。"""
    for name, key in (('updater._version_key', updater['_version_key']), ('web._ota_ver_key', web_key)):
        for old in NUMERIC_LINE:
            assert key(V_LINE) > key(old), '%s: %s 未排在 %s 之上' % (name, V_LINE, old)


def test_dropping_the_v_prefix_would_be_rejected(updater, web_key):
    """反面证据：纯数字 "1.0.01" 会被两道闸判成降级 —— 这就是 V 不能省的原因。"""
    assert updater['_version_key']('1.0.01') < updater['_version_key']('1.5.70')
    assert web_key('1.0.01') <= web_key('1.5.68')
    for old in NUMERIC_LINE:
        assert updater['is_downgrade']('1.0.01', old) is True


def test_updater_accepts_v_line_from_every_shipped_box(updater):
    is_downgrade = updater['is_downgrade']
    for old in NUMERIC_LINE:
        assert is_downgrade(V_LINE, old) is False, '更新器会把 %s -> %s 判成降级' % (old, V_LINE)
    # 边界：同版本仍拒（无意义重装），历史行为不许被本方案改掉
    assert is_downgrade(V_LINE, V_LINE) is True
    # 空/未知当前版本 = 放行（既有语义，别顺手改）
    assert is_downgrade(V_LINE, '') is False


def test_panel_would_show_update_available(web_key):
    """面板判据原文：`_ota_ver_key(ver) > _ota_ver_key(cur)` ⇒ True 才会出现「发现更新」。"""
    for old in NUMERIC_LINE:
        assert web_key(V_LINE) > web_key(old)


def test_current_version_is_on_the_v_line(updater):
    m = re.search(r'kCoreVersion\s*=\s*"([^"]*)"', VERSION_HPP.read_text(encoding='utf-8'))
    assert m, 'version.hpp 里找不到 kCoreVersion'
    cur = m.group(1)
    assert re.match(r'^[A-Za-z]', cur), '产品版本必须带字母前缀（V 线），当前为 %r' % cur
    assert updater['_version_key'](cur) > updater['_version_key']('1.5.70'), \
        '当前版本 %s 未排在数字线之上 ⇒ 现网盒子会升不上来' % cur


def test_both_python_copies_share_one_rule():
    """两处 Python 实现必须是同一套规则（tag 0 = 数字段 / tag 1 = 非数字段）。"""
    for path in (UPDATER_SRC, OTA_SRC):
        src = _norm_quotes(path.read_text(encoding='utf-8'))
        assert 'parts.append((0, int(seg), ""))' in src, '%s 的数字段分支已变形' % path.name
        assert 'parts.append((1, 0, seg))' in src, '%s 的非数字段分支已变形' % path.name
        assert '"[.\\-_+]"' in src, '%s 的分段字符集已变形（会影响字母前缀的解析）' % path.name
