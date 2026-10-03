# test_web_storage_expand_poll.py — P1-2 回归锁：扩容面板的数据源唯一性
#
# 板端实测（2026-09-19，见 docs/交付前Web实测报告-2026-09-19.md P1-2）：
#   "扩容存储"按钮**永久禁用**、pill 恒显示"未读取"。链路：
#     · 扩容面板的真源是 GET /api/system/storage（响应带 rootfs 检测结果）；
#     · 而 renderSystemStats() 把 /api/system 的 payload.storage（**磁盘占用简表，无 rootfs**）
#       也交给了 renderStorageExpansion() ⇒ 空 rootfs 被判成"未读取"、按钮 disabled=true；
#     · renderSystemStats 由 initSystemPolling 每 2.5s 调一次 ⇒ 用户点"刷新"读到的状态
#       活不过一个轮询周期 ⇒ 表现为"永久禁用"。
#
# 修复口径 = **数据源唯一**：扩容 UI 只由 /api/system/storage 驱动，其它调用方一律不碰。
# 本文件锁死该口径，防止以后有人"顺手"在系统状态轮询里再调一次。
#
# 运行：python -m pytest plugins/web/tests/test_web_storage_expand_poll.py -v（从仓库根）
from __future__ import annotations

import pathlib
import re

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
TEMPLATE = REPO_ROOT / 'plugins' / 'web' / 'templates' / 'index.html'
from plugins.web.tests import panel_src  # noqa: E402  2026-10-03 面板外链：读整个面板

# 驱动扩容面板的四个函数
EXPANSION_FUNCS = ('storageUsage', 'storageExpandLabel', 'storageExpandLog', 'renderStorageExpansion')

# 扩容面板的唯一数据源（后端路由字面量按门禁①口径写成相对路径即可，不必写主机绝对路径）
EXPANSION_SOURCE_ROUTE = '/api/system/storage'


def _src() -> str:
    return panel_src.all_src()


def _strip_comments(text: str) -> str:
    """去掉行尾 // 注释（保留 `://` 这类 URL 里的双斜杠）。

    必须去注释：判断"某函数有没有调用 X"时，一句解释性注释会把结论假阳性。
    """
    return '\n'.join(re.sub(r'(?<!:)//.*$', '', ln) for ln in text.split('\n'))


def _functions(src: str) -> dict[str, tuple[int, int]]:
    """所有顶层 `function NAME(...) {` / `async function NAME(...) {` 到行首 `}` 的行号区间。"""
    lines = src.split('\n')
    out: dict[str, tuple[int, int]] = {}
    for i, ln in enumerate(lines):
        m = re.match(r'(?:async\s+)?function ([A-Za-z0-9_$]+)\(', ln)
        if not m:
            continue
        end = i
        while lines[end] != '}':
            end += 1
        out[m.group(1)] = (i, end)
    return out


def _body(src: str, name: str) -> str:
    """取顶层函数的完整源码。"""
    lines = src.split('\n')
    span = _functions(src).get(name)
    assert span is not None, f'模板里找不到顶层函数 {name}'
    return '\n'.join(lines[span[0]:span[1] + 1])


def _enclosing(src: str, line_idx: int) -> str | None:
    lines = src.split('\n')
    for name, (start, end) in _functions(src).items():
        if start <= line_idx <= end:
            return name
    return None


def test_expansion_functions_still_exist():
    src = _src()
    for name in EXPANSION_FUNCS:
        assert f'function {name}(' in src, f'扩容面板函数 {name} 不见了（模板被裁剪？）'


def test_expansion_renderer_bails_out_before_touching_widgets_without_rootfs():
    """没有 rootfs 的载荷必须在**碰任何控件之前**返回。

    这是"轮询不得覆盖"的实现面：一旦先取了控件引用再判空，就必然写坏 UI。
    """
    body = _body(_src(), 'renderStorageExpansion')
    assert 'rootfs' in body, '渲染函数不再看 rootfs，扩容状态判定依据丢失'
    head = body.split('const rootfs', 1)[0]
    assert 'return;' in head, (
        'renderStorageExpansion 在取到 rootfs 之前没有提前返回 —— '
        '不含 rootfs 的载荷（如 /api/system 的磁盘简表）会再次覆盖扩容 UI')


def test_system_stats_renderer_does_not_drive_expansion_panel():
    """★ 本缺陷的根因行：系统状态轮询不得再驱动扩容面板。"""
    stats = _strip_comments(_body(_src(), 'renderSystemStats'))
    assert 'renderStorageExpansion' not in stats, (
        '/api/system 的 storage 是磁盘占用简表、不含 rootfs，'
        'renderSystemStats 每 2.5s 跑一次，会反复抹掉扩容状态 ⇒ 按钮永久禁用')


def test_system_polling_kicks_expansion_status_once():
    """进系统页时取一次扩容状态（否则面板停在上次的"未读取"，仍要手动点刷新）。"""
    poll = _strip_comments(_body(_src(), 'initSystemPolling'))
    assert 'refreshStorageStatus' in poll, 'initSystemPolling 未初始化扩容状态'


def test_every_expansion_call_site_is_backed_by_the_expansion_source():
    """每个调用点要么传带 rootfs 的对象字面量，要么位于取 /api/system/storage 的函数里。"""
    src = _src()
    lines = src.split('\n')
    callers = [i for i, ln in enumerate(lines)
               if 'renderStorageExpansion(' in ln
               and not _strip_comments(ln).strip().startswith('function ')]
    assert callers, '找不到任何 renderStorageExpansion 调用点'
    for i in callers:
        window = '\n'.join(lines[i:i + 5])
        owner = _enclosing(src, i)
        backed = ('rootfs' in _strip_comments(window)
                  or (owner is not None and EXPANSION_SOURCE_ROUTE in _body(src, owner)))
        assert backed, (
            f'index.html 第 {i + 1} 行的 renderStorageExpansion 调用既没带 rootfs，'
            f'所在函数 {owner} 也不是 {EXPANSION_SOURCE_ROUTE} 的消费者：\n{window}')
