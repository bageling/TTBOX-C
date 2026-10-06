"""test_tools_python_is_orchestration_only.py — 门禁：Python 只许做编排（V1.0.46）

业主 2026-10-06 定案：**「代码全部用 C++，只有脚本用 py」**。

这条纪律靠人记不住，必须由测试钉死。禁止的行为（在 core/tools 下）：
  · 用 Python **重新实现**产品控制器 / 物理仿真 / 闭环回放
    —— 那是"用替身验证真身"。已犯过的错：V1.0.43 的 τ=MT 错误在 Python 与 C++
    两边犯**一模一样的错** ⇒ 所谓互验形同虚设；V1.0.46 又在 C++ 回放器里抓到
    延迟队列「先写再读」bug（Python 那版是对的 ⇒ 替身根本发现不了）。
  · 出现与产品头文件同名的模块（fitts.py / pid1.py 这类"影子实现"）

允许的：调 C++ 可执行文件、判阈值、生成测试轨迹、写报告、比对基线。
"""
from __future__ import annotations

import ast
import re
from pathlib import Path

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402

REPO = Path(_ttbox_repo_root())
TOOLS = REPO / "core" / "tools"

# 产品控制器头文件（唯一真源）
PRODUCT_CTL_HEADERS = {
    "FittsAimController": REPO / "core/src/aim/FittsAimController.hpp",
    "Pid1Controller": REPO / "core/src/aim/Pid1Controller.hpp",
}


def py_files() -> list[Path]:
    return sorted(p for p in TOOLS.rglob("*.py") if "__pycache__" not in str(p))


# ────────────────────────────── ① 不得有"影子实现"模块 ──────────────────────────────

def test_no_shadow_controller_modules():
    """不得存在 fitts.py / pid1.py 这类"用 Python 重写产品控制器"的模块。

    影子实现是替身验证的根源：两份实现可能同时误解原始算法而互验不出。
    """
    shadows = []
    for p in py_files():
        stem = p.stem.lower()
        for cls in PRODUCT_CTL_HEADERS:
            if stem == cls.replace("Controller", "").lower() or stem == cls.lower():
                shadows.append(f"{p.relative_to(REPO)}")
    assert not shadows, (
        "发现影子实现（用 Python 重写了产品控制器）：\n  "
        + "\n  ".join(shadows)
        + "\n控制逻辑只有一份，就是 C++（core/src/aim/*.hpp）。"
    )


@pytest.mark.parametrize("cls,header", sorted(PRODUCT_CTL_HEADERS.items()))
def test_python_does_not_define_product_control_class(cls, header):
    """Python 里不得 `class FittsAimController` / `class Pid1Controller`。"""
    for p in py_files():
        src = p.read_text(encoding="utf-8", errors="ignore")
        assert f"class {cls}" not in src, (
            f"{p.relative_to(REPO)} 定义了 {cls} —— "
            f"产品控制器在 {header.relative_to(REPO)}，Python 重写它就是替身验证"
        )


def test_python_does_not_reimplement_control_formula():
    """不得在 Python 里复刻控制公式的关键项（Fitts 的 log2 难度指数 / MT 时间模型）。

    这两个是控制器的"灵魂"（dat58 的 Fitts 模型）：一旦在 Python 里再写一遍，
    改一边忘另一边 ⇒ 两套参数语义悄悄分叉（V1.0.43 就是这么错的）。
    """
    # Fitts 的 MT = A + B·log2(2·|err|/W + 1)
    mt_pattern = re.compile(r"log2?\s*\(\s*2\.?0?\s*\*\s*abs|log2?\s*\(\s*2\s*\*\s*\|?e")
    for p in py_files():
        src = p.read_text(encoding="utf-8", errors="ignore")
        # selftest 只做"跑 C++ + 判阈值"，不应出现难度指数公式
        if mt_pattern.search(src):
            assert "replay_main" in src or p.name == "selftest.py", (
                f"{p.relative_to(REPO)} 里出现 Fitts 难度指数公式 —— "
                f"控制公式只在 C++（replay_main.cpp / FittsAimController.hpp）"
            )


def test_python_does_not_project_3d_physics():
    """不得在 Python 里算透视投影（sx = f·X/Z 之类）—— 3D 物理是 C++ 的事。

    业主 2026-10-06 指出三角洲是 3D 后，3D 场景生成迁进了 C++
    （replay_main.cpp 的 Proj/gen_scene3d）。若 Python 又写一份 ⇒ 又一个替身。
    """
    pat = re.compile(r"tan\s*\(|/\\s*z\\b|math\.tan|np\.tan", re.IGNORECASE)
    for p in py_files():
        src = p.read_text(encoding="utf-8", errors="ignore")
        assert not pat.search(src), (
            f"{p.relative_to(REPO)} 里出现透视投影计算 —— 3D 物理只在 C++"
        )


# ────────────────────────────── ② 编排层应当调用 C++ ──────────────────────────────

def test_selftest_actually_calls_cpp_replay():
    """自测脚本必须真的调 C++ 回放器（subprocess），不能自己算。"""
    p = TOOLS / "pid_sim" / "selftest.py"
    assert p.exists(), f"找不到 {p}"
    src = p.read_text(encoding="utf-8")
    assert "subprocess" in src and "ttbox_replay" in src, (
        "selftest.py 必须 subprocess 调 C++ 回放器 ttbox_replay"
    )
    tree = ast.parse(src)
    # 确认没有从 fitts/pid1 之类的影子模块 import
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                assert a.name not in ("fitts", "pid1"), f"selftest.py 不应 import {a.name}"
        elif isinstance(node, ast.ImportFrom) and node.module:
            assert node.module not in ("fitts", "pid1"), f"selftest.py 不应 from {node.module} import"


def test_cpp_replay_exists_and_includes_product_headers():
    """C++ 回放器必须存在，且 include 产品控制器头文件（而不是拷贝一份实现）。"""
    p = REPO / "core/tools/replay/replay_main.cpp"
    assert p.exists(), f"找不到 C++ 回放器 {p}"
    src = p.read_text(encoding="utf-8")
    assert '#include "aim/FittsAimController.hpp"' in src
    assert '#include "aim/Pid1Controller.hpp"' in src
    # 不得在回放器里重写控制器类
    assert "class FittsAimController" not in src, "回放器不得重定义产品控制器"
    assert "class Pid1Controller" not in src, "回放器不得重定义产品控制器"


def test_cpp_replay_registers_in_cmake():
    """回放器必须注册进 CMake（否则编译不进 ⇒ 判定层拿不到 C++ 代码）。"""
    cm = (REPO / "core/CMakeLists.txt").read_text(encoding="utf-8")
    assert "ttbox_replay" in cm and "tools/replay/replay_main.cpp" in cm


def test_cpp_replay_not_in_product_payload():
    """回放器是开发工具，**不得**进产品 payload（板端不需要它）。"""
    p = REPO / "scripts/ttbox_pack_ota.sh"
    if p.exists():
        src = p.read_text(encoding="utf-8", errors="ignore")
        assert "ttbox_replay" not in src, "回放器不应进产品 OTA 包"


# ────────────────────────────── ③ 报告与门禁解释清楚 ──────────────────────────────

def test_orchestration_files_have_role_docstring():
    """每个 Python 工具文件开头必须写明"它是编排层、控制逻辑在 C++"。"""
    for p in py_files():
        src = p.read_text(encoding="utf-8", errors="ignore")
        head = src[:600]
        assert ("C++" in head or "cpp" in head.lower()), (
            f"{p.relative_to(REPO)} 开头没说明与 C++ 的分工 —— "
            f"每个工具都该写清「这里只做编排，控制逻辑在 C++」"
        )
