"""paths.py 根锚（A-PATH-3）双态单测。

为什么单独测这一个函数：
    锚点函数能坏得**无声**。把 ``discover_root`` 退回成硬编码层级（``parents[N]``）时，
    开发机上照样全绿 —— 因为开发机的深度恰好对得上；只有搬到板端 release 树、
    或者哪天仓库重排目录，才会指错根，而那时东西已经在客户手上。

怎么测才算数：
    用两套**真实目录树**（不是 mock、不是 monkeypatch）承载**同一份源文件**，
    只有布局不同 ⇒ 断言「结果只随布局走，不随本文件的层级深度走」。

    态 A（开发机） ``<tmp>/repo/{plugins,framework,scripts,deploy}/plugins/web/lib/paths.py``
    态 B（板端）   ``<tmp>/<prefix>/releases/V1.0.16/{同上四项}/plugins/web/lib/paths.py``

★ 本文件定位被测源文件也**不写死层级**（向上找 ``plugins/web/lib/paths.py``），
  否则测试自己就依赖了被测函数要取消的东西。
"""
from __future__ import annotations

import importlib.util
import shutil
from pathlib import Path

import pytest

# 与 paths.py::_ROOT_ANCHORS 同值。这里**独立再写一遍**是故意的：
# 两边同时被改错才会一起失灵，只改一边就会被测试拦住。
ANCHORS = ("plugins", "framework", "scripts", "deploy")


# ---------------------------------------------------------------------------
# 工具
# ---------------------------------------------------------------------------
def _live_source() -> Path:
    """本仓库里的 plugins/web/lib/paths.py（向上找，不写死层级）。"""
    cur = Path(__file__).resolve().parent
    while True:
        cand = cur / "plugins" / "web" / "lib" / "paths.py"
        if cand.is_file():
            return cand
        if cur.parent == cur:
            raise AssertionError("找不到被测源文件 plugins/web/lib/paths.py")
        cur = cur.parent


def _load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _lay_out(root: Path) -> Path:
    """在 root 下铺一层目录布局，并把被测源文件放到 plugins/web/lib/paths.py。"""
    for name in ANCHORS:
        (root / name).mkdir(parents=True, exist_ok=True)
    dst = root / "plugins" / "web" / "lib" / "paths.py"
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(_live_source(), dst)
    return dst


def _independent_root(start: Path):
    """不复用被测代码，自己走一遍同一判据；找得到就返回，找不到返回 None。"""
    cur = start.resolve()
    while True:
        if all((cur / d).is_dir() for d in ANCHORS):
            return cur
        if cur.parent == cur:
            return None
        cur = cur.parent


# ---------------------------------------------------------------------------
# 态 A / 态 B
# ---------------------------------------------------------------------------
def test_dev_tree_anchor(tmp_path):
    """态 A：开发机仓库布局 —— 根 = 仓库根，scripts 目录跟着走。"""
    root = tmp_path / "repo"
    src = _lay_out(root)
    mod = _load(src, "ttbox_paths_dev")

    assert Path(mod.repo_root()) == root.resolve()
    assert Path(mod.scripts_dir()) == (root / "scripts").resolve()
    assert (root / "scripts").is_dir()


def test_board_release_tree_anchor(tmp_path):
    """态 B：板端 release 树布局 —— 根 = ``releases/<ver>/``，不是运行根前缀。"""
    prefix = tmp_path / "opt" / "ttbox"
    (prefix / "releases").mkdir(parents=True)
    root = prefix / "releases" / "V1.0.16"
    # 板端运行根下本来就有 plugins / scripts 两个兼容软链；这里连 framework / deploy
    # 一起造出来，是为了顺带证明"取最近一层"而不是"取最上一层"。
    for name in ANCHORS:
        (prefix / name).mkdir(parents=True, exist_ok=True)
    src = _lay_out(root)
    mod = _load(src, "ttbox_paths_board")

    assert Path(mod.repo_root()) == root.resolve()
    assert Path(mod.repo_root()) != prefix.resolve()
    assert Path(mod.scripts_dir()) == (root / "scripts").resolve()


def test_same_source_serves_both_layouts(tmp_path):
    """同一份源文件、两种布局 ⇒ 各自解析到自己那一层根（锚点与深度无关）。

    这一条就是"双态"的核心断言：源码字节一致，结果只由布局决定。
    """
    a_root = tmp_path / "repo"
    b_root = tmp_path / "opt" / "ttbox" / "releases" / "V1.0.16"
    src_a = _lay_out(a_root)
    src_b = _lay_out(b_root)

    assert src_a.read_bytes() == src_b.read_bytes(), "两态必须加载同一份源码"
    mod_a = _load(src_a, "ttbox_paths_dual_a")
    mod_b = _load(src_b, "ttbox_paths_dual_b")

    assert Path(mod_a.repo_root()) == a_root.resolve()
    assert Path(mod_b.repo_root()) == b_root.resolve()
    assert mod_a.repo_root() != mod_b.repo_root()


# ---------------------------------------------------------------------------
# 负向控制：证明"没有锚"这件事不会被糊弄过去
# ---------------------------------------------------------------------------
def test_missing_one_anchor_is_not_a_root(tmp_path):
    """四项锚缺一不可：只有 plugins/framework/scripts 的那一层不算根。

    这正是"宽松判据"会踩的坑 —— 只用 plugins 一个标志时，``plugins/web`` 之类的中间层
    也可能被当成根；用四个必须同时存在，才把解钉成唯一。
    """
    near = tmp_path / "near"
    for name in ("plugins", "framework", "scripts"):     # 故意缺 deploy
        (near / name).mkdir(parents=True, exist_ok=True)
    if _independent_root(near) is not None:
        pytest.skip("环境特例：临时目录之上存在锚目录，无法构造'近处有假根'场景")

    mod = _load(_live_source(), "ttbox_paths_partial")
    assert _independent_root(near) is None, "缺一项就不该构成根（否则本用例无意义）"
    with pytest.raises(RuntimeError):
        mod.discover_root(near)


def test_loud_failure_when_no_anchor(tmp_path):
    """找不到主根必须**报错**，不许静默回退（回退到 "." 或上层目录都会指错根且无声）。"""
    bare = tmp_path / "bare" / "a" / "b"
    bare.mkdir(parents=True)
    if _independent_root(bare) is not None:
        pytest.skip("环境特例：临时目录之上存在锚目录，无法构造'无锚'场景")
    mod = _load(_live_source(), "ttbox_paths_noanchor")
    with pytest.raises(RuntimeError):
        mod.discover_root(bare)


# ---------------------------------------------------------------------------
# 生产路径回归：真实仓库里跑一遍
# ---------------------------------------------------------------------------
def test_live_repo_root_agrees_with_independent_probe():
    """真实本仓库：``repo_root()`` 必须等于独立推导的仓库根，且 scripts 目录真实存在。"""
    mod = _load(_live_source(), "ttbox_paths_live")
    got = Path(mod.repo_root())
    want = _independent_root(_live_source().parent)
    assert want is not None
    assert got == want
    # 生产锚点：仓库根必须能定位到出货清单与 scripts（板端 release 树同样满足）
    assert (got / "deploy" / "pack_manifest.txt").is_file() or (got / "scripts").is_dir()
    assert Path(mod.scripts_dir()).is_dir()
