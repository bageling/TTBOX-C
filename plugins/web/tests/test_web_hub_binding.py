# test_web_hub_binding.py — hub 与入口的绑定契约（2026-10-04）
#
# ★ 为什么补这个测试：
#   S9/S10 把web 后端拆成 lib/ 之后，入口靠 `hub.bind(sys.modules[__name__])`
#   把自己的全局暴露给 26 个 lib 模块。这行有 3 个各自能过一部分、但合起来
#   必然坏的写法组合，且**失败形态极不一致**：
#     · sys.modules[__name__]  → 动态加载（importlib.util.spec_from_file_location）
#                                路径下 KeyError；
#     · globals() + hub 用 getattr → getattr(dict, name) 全部 AttributeError；
#     · types.ModuleType +赋 __dict__ → `__dict__` 只读，AttributeError。
#   我为了修它实测踩了三条，framework/tests 的两条用例**从改动前就红**（stash 验过），
#   属于"没人发现的历史欠账"。本文件把正确形态钉住：
#     ① entry() 返回 dict（不是模块对象）；
#     ② 入口 bind 的是 globals()；
#     ③ lib侧写入口全局必须走 hub.set_，不许 setattr(hub.entry(), ...)。
from __future__ import annotations

import io
import re

import pytest

from plugins.web.lib import hub

ENTRY = "plugins/web/bin/ttbox-web.py"
LIB_GLOB = "plugins/web/lib/*.py"


def _entry_src() -> str:
    return io.open(ENTRY, encoding="utf-8").read()


def test_entry_binds_globals_not_sys_modules():
    """★ 入口必须 `hub.bind(globals())`，不得用 sys.modules[__name__]。"""
    src = _entry_src()
    m = re.search(r'^hub\.bind\((.+?)\)\s*$', src, re.M)
    assert m, "入口末尾找不到 hub.bind(...)"
    arg = m.group(1).strip()
    assert arg == "globals()", (
        f"入口绑定的是 {arg!r}，应为 globals()。"
        "sys.modules[__name__] 在 importlib.util.spec_from_file_location "
        "加载路径下会 KeyError（模块不进 sys.modules）"
    )


def test_entry_returns_dict_namespace():
    """★ 契约：hub.entry() 返回 dict（模块全局命名空间），不是模块对象。

    为什么必须是 dict：动态加载路径下"模块对象"没有可靠取法（sys.modules 没有它、
    __dict__ 只读）。dict 就是模块全局的确切语义。
    """
    hub.bind({"__probe__": 123})
    try:
        ns = hub.entry()
        assert isinstance(ns, dict), f"entry() 应返回 dict，实得 {type(ns)}"
        assert hub.get("__probe__") == 123
    finally:
        hub.bind(None)


def test_set_writes_namespace():
    """hub.set_ 必须真写进去（改入口全局的唯一正规途径）。"""
    hub.bind({})
    try:
        hub.set_("__probe2__", 456)
        assert hub.entry()["__probe2__"] == 456
        # 再取一次应拿到同一个对象（共享语义，不是复制）
        assert hub.get("__probe2__") == 456
    finally:
        hub.bind(None)


def test_get_missing_name_raises_attribute_error():
    """取不到必须抛 AttributeError，且消息点名是哪个名字（不静默返回 None）。"""
    hub.bind({})
    try:
        with pytest.raises(AttributeError) as ei:
            hub.get("__definitely_missing__")
        assert "__definitely_missing__" in str(ei.value), (
            f"报错消息没点名缺失的全局：{ei.value}"
        )
    finally:
        hub.bind(None)


def test_unbound_raises_runtime_error():
    """未 bind 时必须抛 RuntimeError，且消息指向正确写法。"""
    saved = hub._entry
    hub._entry = None
    try:
        with pytest.raises(RuntimeError) as ei:
            hub.entry()
        msg = str(ei.value)
        assert "hub.bind(globals())" in msg, f"消息没指向正确写法：{msg}"
    finally:
        hub._entry = saved


def test_lib_never_uses_setattr_on_entry():
    """★ lib 侧不得出现 `setattr(hub.entry(), ...)` —— dict 没有属性赋值。

    这正是上一轮板端连续崩两次的同族错误（把 hub 转发函数/命名空间当对象用）：
      `with _HEARTBEAT_START_LOCK:` / `global _HEARTBEAT` / `_ACTIVATION_CACHE['ts']=`
    写入口全局一律走 hub.set_。
    """
    import glob
    bad = []
    for path in glob.glob(LIB_GLOB):
        # ★ 排除 hub.py 自己：set_() 里那条 `setattr(ns, ...)` 是**兼容分支**
        #   （入口若传了模块对象而非 dict 时才走），它是合法实现的一部分。
        #   不排除的话本测试会命中 hub 自己 ⇒ 假红。
        if path.replace("\\", "/").endswith("lib/hub.py"):
            continue
        src = io.open(path, encoding="utf-8").read()
        for i, ln in enumerate(src.splitlines(), 1):
            code = ln.split("#", 1)[0]
            if re.search(r'\bsetattr\s*\(\s*hub\.entry\s*\(', code):
                bad.append(f"{path}:{i}")
            if re.search(r'\bsetattr\s*\(\s*entry\s*\(', code):
                bad.append(f"{path}:{i}")
    assert not bad, (
        f"lib 侧出现对 hub.entry() 的 setattr：{bad}\n"
        "hub 绑定的是 dict，必须改用 hub.set_(name, value)"
    )


def test_lib_never_binds_or_touches_entry_binding():
    """lib 侧不得改 _entry（只有入口能 bind）。"""
    import glob
    bad = []
    for path in glob.glob(LIB_GLOB):
        src = io.open(path, encoding="utf-8").read()
        if re.search(r'hub\._entry\s*=', src):
            bad.append(path)
    assert not bad, f"lib 侧直接改 hub._entry：{bad}（应只用 hub.bind/hub.get/hub.set_）"
