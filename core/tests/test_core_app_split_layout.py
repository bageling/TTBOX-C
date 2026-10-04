# test_core_app_split_layout.py — Application 四TU 布局守卫（2026-10-04 core S1）
#
# ★ 为什么需要这个测试：
#   Application.cpp 从2341 行拆成 4 个 TU（1146 + 569 + 56 + 623）后，
#   **没有任何机制阻止它被合回去**。一旦有人为了"方便"把三个文件的内容
#   复制回 Application.cpp，就会：
#     ① 变成两份定义 → 链接期 duplicate symbol（会响，OK）；
#     ② 但更危险的是**只搬一半**：某个成员悄悄消失 → undefined reference（会响）；
#     ③ 最危险的是**搬错组**：handle_* 混进 runtime 组 —— 编译链接全过，
#        结构治理的收益（改一条 IPC 命令只动一个文件）静默消失。
#   ⇒ 本文件锁三件事：成员归属、单一定义、共享辅助不落回匿名命名空间。
#
# ★ 判据口径：成员枚举**只认行首的类外定义**（`^[type] Application::name(`），
#   不用 `grep -c "Application::"` —— 后者会把注释里的引用也计入
#   （历史上就因此把 36 个成员数成 40 个）。
import re
from pathlib import Path

from plugins.web.lib.paths import discover_root

# ★ 根锚用 paths.discover_root（门禁⑩ 禁 parents[N] / "../.."）：从本文件逐级向上找
#   含 core/src/app 与 plugins 的目录，找不到就抛 —— 不静默指错根。
#   ★ 之前写死 parents[2]：少一级会落到工作区上一层，那**不会**报"路径错"，
#     只会全线 FileNotFoundError，看起来像"文件不存在"（很难联想到层级写错）。
#     另叠 test_repo_root_is_correct 做前置守卫。
REPO = Path(discover_root(__file__))
APP_DIR = REPO / "core" / "src" / "app"

MAIN = APP_DIR / "Application.cpp"
IPC = APP_DIR / "ApplicationIpc.cpp"
LICENSE = APP_DIR / "ApplicationLicense.cpp"
RUNTIME = APP_DIR / "ApplicationRuntime.cpp"
INTERNAL = APP_DIR / "ApplicationInternal.hpp"
CMAKE = REPO / "core" / "CMakeLists.txt"

MEMBER_RE = re.compile(r'^(?:[A-Za-z_][\w:<>,\s\*&]*?)\bApplication::([a-z_][\w]*)\s*\(')

# 期望归属（与拆分时一致；改归属必须同步改本表 + 同步改拆分的 commit 说明）
EXPECTED = {
    MAIN: [
        "initialize", "run", "shutdown", "request_shutdown",
        "status", "status_provider", "config_provider",
    ],
    IPC: [
        "handle_config_update", "handle_license_activate",
        "handle_license_activate_cloud", "handle_model_activate",
        "handle_model_import", "handle_model_install", "handle_model_list",
        "handle_model_remove", "handle_model_set_concurrency",
        "handle_model_validate", "handle_runtime_control",
    ],
    LICENSE: [
        "license_allow_run", "license_is_pro",
        "license_status_snapshot", "resolve_license_card",
    ],
    RUNTIME: [
        "apply_preview_degrade", "apply_startup_runtime_intent", "brand_upper",
        "build_runtime_params", "current_feature_gates", "gate_missing_summary",
        "has_capture_signal", "load_runtime_intent", "migrate_output_enabled",
        "persist_runtime_intent", "persist_runtime_profile",
        "switch_active_model_runtime", "sync_model_id_to_profile",
        "try_resume_from_degraded",
    ],
}

ALL_FILES = [MAIN, IPC, LICENSE, RUNTIME]


def _members(path: Path) -> list:
    return [m.group(1) for m in
            (MEMBER_RE.match(ln) for ln in path.read_text(encoding="utf-8").splitlines())
            if m]


def test_repo_root_is_correct():
    """前置守卫：仓库根必须同时含 core/ 与 plugins/。

    ★ 为什么单独一条：paths 层级写错（parents[n] 少一级/多一级）**不会**给出
      "路径错"的提示，只会全线 FileNotFoundError，看起来像"文件不存在"。
      先断言根是对的，后面的报错才有意义。
    """
    assert (REPO / "core").is_dir(), f"REPO 层级错：{REPO} 下没有 core/（parents[n] 写错了？）"
    assert (REPO / "plugins").is_dir(), f"REPO 层级错：{REPO} 下没有 plugins/"
    assert (REPO / "core" / "CMakeLists.txt").is_file(), "REPO 下没有 core/CMakeLists.txt"


def test_all_four_files_exist():
    """四个 TU + 共享头必须都在（少一个 = 拆分不完整，链接会失败或静默丢成员）。"""
    missing = [p.name for p in ALL_FILES + [INTERNAL] if not p.is_file()]
    assert not missing, f"Application 拆分后的文件缺失：{missing}"


def test_members_are_in_expected_files():
    """★ 主断言：成员必须待在它该在的 TU 里。

    把handle_* 挪回 Application.cpp（或挪进 runtime 组）都能编译通过，
    但会让"改一条 IPC 命令只动一个文件"的结构收益静默消失。
    """
    problems = []
    for path, expected in EXPECTED.items():
        actual = set(_members(path))
        want = set(expected)
        if actual - want:
            problems.append(f"{path.name} 多出成员 {sorted(actual - want)}")
        if want - actual:
            problems.append(f"{path.name} 缺少成员 {sorted(want - actual)}")
    assert not problems, "成员归属错位：\n  " + "\n  ".join(problems)


def test_no_duplicate_definition_across_files():
    """同一个成员不能在两个 TU 里各定义一次（链接期duplicate，故必须提前拦）。"""
    seen: dict[str, list] = {}
    for path in ALL_FILES:
        for name in _members(path):
            seen.setdefault(name, []).append(path.name)
    dup = {k: v for k, v in seen.items() if len(v) > 1}
    assert not dup, f"成员被重复定义：{dup}"


def test_member_total_is_36():
    """成员总数锁定 36 —— 少一个就是丢功能，多一个就是加了没登记的。

    ★ 这是"数量必须机械枚举"的落地：改代码后这个数变了要么是有意增删，
      要么是搬漏了，两种都必须显式改本数并想清楚。
    """
    total = sum(len(_members(p)) for p in ALL_FILES)
    assert total == 36, (
        f"Application 成员总数 = {total}（期望 36）。"
        "若确实增删了成员，请同步更新 EXPECTED 与本断言；"
        "若是搬漏了，本测试就是最后一道拦截。"
    )


def test_cmake_registers_all_four_sources():
    """CMake 必须编入四个 .cpp。

    ★ 关键：三个新文件里有成员**无条件引用** WorkerPool / RKNNEngine
      （build_runtime_params / switch_active_model_runtime 等）⇒ 少编一个就是
      链接期 undefined reference。
    ★ 而且它们必须与 Application.cpp **同在那个 `if(TTBOX_CORE_HAS_RKNN OR WIN32)`
      条件块**里 —— 放到无条件段会在无 RKNN 的 host 上炸 undefined。
    """
    text = CMAKE.read_text(encoding="utf-8")
    for name in ("src/app/Application.cpp",
                 "src/app/ApplicationIpc.cpp",
                 "src/app/ApplicationLicense.cpp",
                 "src/app/ApplicationRuntime.cpp"):
        assert text.count(name) >= 1, f"CMake 未注册 {name}"

    # 定位条件块，确认四个都在里面
    m = re.search(r"if\(TTBOX_CORE_HAS_RKNN OR WIN32\)(.*?)endif\(\)", text, re.S)
    assert m, "找不到 `if(TTBOX_CORE_HAS_RKNN OR WIN32)` 条件块"
    block = m.group(1)
    for name in ("Application.cpp", "ApplicationIpc.cpp",
                 "ApplicationLicense.cpp", "ApplicationRuntime.cpp"):
        assert name in block, (
            f"{name} 不在 `if(TTBOX_CORE_HAS_RKNN OR WIN32)` 块内 —— "
            "放错位置会在无 RKNN 的 host 构建上炸 undefined reference"
        )


def test_shared_helpers_are_not_in_anonymous_namespace():
    """★ 拆分的头号陷阱：跨 TU 用的辅助不能在任何 TU 的匿名命名空间里。

    匿名命名空间按标准「每个 TU 一份副本」。拆前它们同在一个 TU 里所以能用；
    拆后若还留在原 TU，其他 TU 引用时会拿到**未声明/ 别人的同名**，
    编译或链接才炸（`now_ms` 这类更可能是静默用错）。
    """
    helpers = ["now_ms", "strip", "parse_color_order",
               "parse_worker_cores", "incoming_dir_of"]
    text = INTERNAL.read_text(encoding="utf-8")
    for h in helpers:
        assert h in text, f"{h} 不在 ApplicationInternal.hpp（拆分后无处可见）"

    for path in ALL_FILES:
        src = path.read_text(encoding="utf-8")
        # 若某个 TU 里还有 `double now_ms() {` 这种**定义**，就是漏网
        for h in helpers:
            bad = re.search(r'^(?:static\s+|inline\s+)?[\w:<>]+\s+' + h + r'\s*\([^)]*\)\s*\{',
                            src, re.M)
            assert not bad, (
                f"{path.name} 里还有 {h} 的本地定义（第 {src[:bad.start()].count(chr(10))+1} 行）"
                "—— 它属于 ApplicationInternal.hpp，留在本 TU 会让其他 TU 拿不到"
            )


def test_shutdown_flag_lives_in_header():
    """★ shutdown_flag 必须在 Application.hpp 里（不能留在任何 .cpp 的匿名命名空间）。

    `request_shutdown()` 写它、`run()` 读它，拆分后两者分处不同 TU。
    留在 .cpp 的匿名命名空间 ⇒ 两个 TU 各一份副本 ⇒ **关机信号静默丢失**
    （不崩、不报错、进程只能被 SIGKILL）。
    """
    hpp = (APP_DIR / "Application.hpp").read_text(encoding="utf-8")
    assert "shutdown_flag" in hpp, (
        "shutdown_flag 不在 Application.hpp —— 拆分后 request_shutdown() 与 run() "
        "分处不同 TU，留在 .cpp 里会各拿一份副本，关机信号静默丢失"
    )
    assert "g_shutdown_requested" in hpp, "g_shutdown_requested 变量不在 Application.hpp"
    for path in ALL_FILES:
        src = path.read_text(encoding="utf-8")
        # ★ 判据要同时覆盖「有参」和「无参」两种写法。
        #   第一版只写 `shutdown_flag\(\{`，注入一个无参的
        #   `shutdown_flag() { static ... }` 竟然报绿 —— 守卫自己漏了最常见的写法。
        #   现在改为：只要出现**任何形式的定义**（名字 + 可选参数列表 + `{`）就报红。
        m = re.search(r'\b(?:std::atomic<bool>\s+g_shutdown_requested\s*\{'
                      r'|std::atomic<bool>\s*&\s*shutdown_flag\s*\([^)]*\)\s*\{)', src)
        assert not m, (
            f"{path.name} 里还有 shutdown_flag/g_shutdown_requested 的本地定义 —— "
            "它必须只在 Application.hpp（inline），否则各 TU 各拿一份副本"
        )


def test_every_tu_has_the_namespace_wrapper():
    """三个新 TU 必须有 `namespace ttbox::core {` 包裹。

    ★ 拆分脚本第一版漏了这层 —— 报出来是"'Application' has not been declared"，
      属于能立刻发现的错；但同类错（命名空间层级写错成ttbox）就未必。
    """
    for path in ALL_FILES:
        src = path.read_text(encoding="utf-8")
        assert "namespace ttbox::core {" in src, f"{path.name} 缺 namespace ttbox::core 包裹"
        assert src.count("namespace ttbox::core") >= 2, (
            f"{path.name} 的 namespace ttbox::core 没有收尾（开了没关）"
        )
