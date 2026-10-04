r"""hub.py — 入口模块的动态查找点（web 拆分专用，2026-10-02）。

## 为什么需要它

``plugins/web/tests/*.py`` 用 ``importlib.util.spec_from_file_location`` 动态加载
``plugins/web/bin/ttbox-web.py``，然后**直接给模块级全局打补丁**。

★ 全量清单（2026-10-02 机械枚举，**130 处 / 13 个测试文件 / 27 个名字**）：

    _get_runtime_profile(29)  _get_status(20)          _license_block(12)
    ipc_request(11)           _activation_ok(8)        _mouse_save_or_ipc(5)
    _usbproxy_unit_mode(4)    _usbproxy_send_set_config(3)
    _run_quiet(3)             _loopout_payload(3)      _calib_target(3)
    _calib_out_counts(3)      PRESETS_DIR(3)           OTA_UPDATER_PATH(3)
    OTA_JOBS_DIR(3)           _power_action_allowed(2) _calib_sample_pair(2)
    TEMPLATE_DIR(2)           OTA_SERVER_URL(2)        DEFAULT_SSIDS(2)
    socket(1)                 _ui_block(1)             _sysfs_int(1)
    _fan_control_payload(1)   _core_state_payload(1)   _calibration_payload(1)
    UI_BRANDS_CONFIG(1)

★ **这张表必须机械枚举，不能靠"高频几个"拍脑袋**：第一版只列了 10 个名字，
漏掉的 ``_calib_sample_pair``（2 处）在 S4 首轮直接把测试打红（它虽在标定区，
却被 ``lib/calibration.py`` 内的 ``_calib_wait_settled`` / ``_calib_worker`` 调用）。
枚举命令：

    grep -rhoE "monkeypatch\.setattr\([A-Za-z_]+,\s*'[A-Za-z_]+'" --include=*.py plugins/web/tests

把函数搬进 ``lib/`` 子模块后，它读的是**新模块自己的全局**，
入口模块上打的补丁对它不再生效 —— 测试会变成"看起来跑了、其实没打上补丁"的假绿。

## 约定（拆分的接缝规则）

1. **被测试打补丁的名字一律留在入口模块** ``ttbox-web.py``。入口是运行期的"配置根"，
   不是历史包袱 —— 这是为了保住 482 个测试的语义一行不改。
2. 搬进 ``lib/`` 的模块**不在 import 期快照**这些值，改为**调用时**经
   ``hub.get()`` / ``hub.call()`` 向入口模块取。补丁因此在任何调用点都生效。
3. ``hub.bind()`` 由入口模块在**末尾**调用。所以 **lib 模块里禁止在模块级（import 期）
   调用 hub** —— 那时还没绑定。hub 只能出现在函数体里。
4. lib 侧对入口名字的转发一律写 ``def f(*args, **kwargs)`` **原样透传**：
   测试的替身常是少参数的 ``lambda``（如 ``lambda req_type, params: ...``），
   转发时替它补默认值会多塞实参而 ``TypeError``。
5. **共享状态不能靠 hub，也不能各模块各 new 一个**（各自 new 一把锁 = 没加锁）。
   单点放一个 lib 模块（现为 ``lib/locks.py`` 的 ``_CFG_WRITE_LOCK``），各侧 import 同一对象；
   但**消费点必须写裸名** ``with _CFG_WRITE_LOCK:`` —— 测试用 exec + 命名空间注锁，
   写成 ``hub.get('_CFG_WRITE_LOCK')`` 就注不进去了。

绑定用**模块全局命名空间本身**（入口传 ``globals()``）而非模块对象：入口可能被
``importlib.util.spec_from_file_location`` 以动态模块名（``ttbox_web_brand_<pid>_<n>``）
加载，那条路径**不进 ``sys.modules``** ⇒ ``sys.modules[__name__]`` 会KeyError，
而那个场景里没有可靠途径拿到"模块对象"（``__dict__`` 只读）。
dict 就是模块全局的确切语义，所以 ``get``/``set_`` 统一按字典存取。

★ 由此推出一条硬约定（2026-10-04 定）：**``entry()`` 返回 dict，不是模块对象**。
  想给入口全局赋值必须走 :func:`set_`；写 ``setattr(hub.entry(), ...)`` 会
  AttributeError（上一轮板端崩溃那三处 hub 转发函数误用正是同一族错误）。
"""
from __future__ import annotations

from typing import Any

_entry: Any = None  # 入口模块对象


def bind(entry_ns: Any) -> None:
    """由入口模块在末尾调用，把自己登记进来。

    ★ 2026-10-04：``entry_ns`` 是入口模块的 ``__dict__``（入口传``globals()``），
      不再要求是"模块对象"。
      为什么改：入口可能被 ``importlib.util.spec_from_file_location`` 以动态模块名
      （``ttbox_web_brand_<pid>_<n>``）加载 —— 那条路径**不把模块放进 sys.modules**，
      于是 ``sys.modules[__name__]`` 直接 KeyError。而"拿到模块对象"在那个场景里
      根本没有可靠途径（``__dict__`` 只读、``sys.modules`` 里没有）。
    ★ 保留 dict 而非模块对象的代价：hub 自己不能用 ``getattr``。这是**故意的** ——
      dict 存取就是模块全局的确切语义，比 getattr 更直白，也不会被模块的
      ``__getattr__``（PEP 562）劫持。
    """
    global _entry
    _entry = entry_ns


def entry() -> Any:
    """取入口模块的全局命名空间（未绑定时抛错，不静默返回 None）。"""
    if _entry is None:
        raise RuntimeError(
            "hub 未绑定：lib 模块不得在 import 期调用 hub —— "
            "请确认 ttbox-web.py 末尾执行了 hub.bind(globals())"
            "（★ 不要写 hub.bind(sys.modules[__name__])：经 "
            "importlib.util.spec_from_file_location 以动态模块名加载的入口"
            "不在 sys.modules 里，那样写会直接 KeyError）"
        )
    return _entry


def get(name: str) -> Any:
    """按名取入口模块的全局（取不到就抛 KeyError，不静默兜底）。"""
    ns = entry()
    try:
        return ns[name] if isinstance(ns, dict) else getattr(ns, name)
    except (KeyError, AttributeError):
        raise AttributeError(
            "入口模块没有全局 %r（lib 侧转发目标缺失）" % name
        ) from None


def call(name: str, *args: Any, **kwargs: Any) -> Any:
    """按名调用入口模块的函数。"""
    return get(name)(*args, **kwargs)


def set_(name: str, value: Any) -> None:
    """写入口模块的全局（转发函数**不是**被转发的对象本身时必须走这个，见文件头约定 3）。"""
    ns = entry()
    if isinstance(ns, dict):
        ns[name] = value
    else:
        setattr(ns, name, value)
