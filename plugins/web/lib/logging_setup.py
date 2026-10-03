"""logging_setup.py — web 进程日志骨架（《代码书写规矩·技术版》§5，批次 1.4）。

为什么需要本模块：
  §5.4 明文「允许同时输出到 journald，但**必须**同时有文件 sink —— journald 在板端会
  滚动丢失」。改动前 web 进程只有 stdout（进 journald）+ 7 处散落的 print，没有任何
  文件落盘 ⇒ 现场只出事就看不清、更导不出来。

落点（与 core 侧 Logger 的 FileSink 分工对齐，各写各的文件、互不污染）：
  · /var/log/ttbox/web.log        web 进程运行日志（§5.3 同一格式）
  · /var/log/ttbox/operation.log  客户操作 —— **与 core 共用同一个文件**（追加写）

★ 为什么 operation.log 两边都写：
  §5.2 列的六类客户操作（启动/停止/换模型/改参数/导出/恢复出厂）里，**多数最终经 IPC
  落到 core**（core 是权威记录点，见 core/src/common/Logger.cpp::Logger::operation），
  但「导出日志」「恢复出厂」这类是 web 自己的动作，不经过 core。所以两侧各自在自己负责
  的边界上记，写同一个文件、同一种行格式（``<ISO8601> OPERATION <what>``）、都用追加
  打开 ⇒ 天然串接，不需要跨进程锁。

降级（硬要求）：
  日志目录不可写时只留 stdout handler（journald 里仍可见），**不抛、不阻断启动**。
  日志是观测设施，不是业务依赖。
"""
from __future__ import annotations

import logging
import os
import sys
import threading
import time
from datetime import datetime

from . import paths as _paths

# §5.3 级别名与 C++ 侧 log_level_name() 逐字一致（Python 默认的 WARNING/CRITICAL 不用）。
_LEVEL_NAMES = {
    logging.DEBUG: "DEBUG",
    logging.INFO: "INFO",
    logging.WARNING: "WARN",
    logging.ERROR: "ERROR",
    logging.CRITICAL: "FATAL",
}

# §5.1 级别字符串（与 core 的 --log-level 同一套取值，便于两侧用同一个值调）。
_LEVELS = {
    "debug": logging.DEBUG,
    "info": logging.INFO,
    "warn": logging.WARNING,
    "error": logging.ERROR,
    "fatal": logging.CRITICAL,
    "off": logging.CRITICAL + 10,
}

_OPERATION_FILE = "operation.log"
_WEB_FILE = "web.log"

_root_logger_name = "ttbox"
_configured = False
_configure_lock = threading.Lock()


def iso8601_local(ts: float | None = None) -> str:
    """§5.3 的 ISO8601 本地时间，带时区偏移，例：``2026-10-01T12:03:44+08:00``。

    与 C++ 侧 ``log_timestamp_iso8601()`` 输出同构 —— 两边共用 operation.log，时间格式
    必须一致，否则同一份文件里两种写法混排，肉眼与脚本都没法排序。
    """
    dt = datetime.fromtimestamp(time.time() if ts is None else ts).astimezone()
    offset = dt.strftime("%z")  # +0800 / -0500；某些平台可能给空串
    if len(offset) == 5:
        offset = offset[:3] + ":" + offset[3:]
    return dt.strftime("%Y-%m-%dT%H:%M:%S") + offset


class Iso8601Formatter(logging.Formatter):
    """行格式：``<ISO8601 本地时间> <LEVEL> <module> <message>``（§5.3）。

    module 取 ``record.module`` —— Python 的「模块文件名去 .py」，与 C++ 侧由 ``__FILE__``
    派生短名的口径一致。不输出行号：Python 侧 kv（位置参数/%s）已在 message 内。
    """

    def format(self, record: logging.LogRecord) -> str:
        level = _LEVEL_NAMES.get(record.levelno, record.levelname)
        return "%s %s %s %s" % (
            iso8601_local(record.created),
            level,
            record.module,
            record.getMessage(),
        )


def resolve_level(name: str = "") -> int:
    """级别字符串 → logging 级别。

    取值链：显式参数 > ``TTBOX_LOG_LEVEL`` > 默认 INFO（§5.1「默认级别 INFO」）。
    """
    key = (name or os.environ.get("TTBOX_LOG_LEVEL", "")).strip().lower()
    return _LEVELS.get(key, logging.INFO)


def setup_logging(level: str = "") -> logging.Logger:
    """幂等挂载「stdout（→journald）+ 文件 sink」。返回 web 根 logger。

    在 ``main()`` 最开头调用一次即可。重复调用是安全的（第二次直接返回）。
    """
    global _configured
    with _configure_lock:
        root = logging.getLogger(_root_logger_name)
        if _configured:
            return root

        root.setLevel(resolve_level(level))
        # 不向 root 冒泡：免得被宿主（waitress / werkzeug）或第三方库的 handler 重复输出。
        root.propagate = False

        formatter = Iso8601Formatter()

        stream = logging.StreamHandler(sys.stdout)
        stream.setFormatter(formatter)
        root.addHandler(stream)

        directory = _paths.log_dir()
        log_path = os.path.join(directory, _WEB_FILE)
        try:
            os.makedirs(directory, exist_ok=True)
            file_handler = logging.FileHandler(log_path, encoding="utf-8")
            file_handler.setFormatter(formatter)
            root.addHandler(file_handler)
        except OSError as exc:
            # §5.4 要求文件 sink，但目录不可用时降级为「只有 journald」而不是崩在启动处。
            # 板端该目录由 scripts/ttbox_fhs_init.sh 以 ttbox:ttbox 0755 创建，正常不会走到这里。
            root.warning("日志目录不可写，仅输出 stdout: %s (%s)", directory, exc)

        _configured = True
        return root


def get_logger(module: str = "") -> logging.Logger:
    """取子 logger（``ttbox.<module>``）；留空则取 web 根 logger。

    注意 module 参数只影响 logger 层级，**不影响**输出里的 module 字段 —— 后者恒为
    调用方所在文件的模块名（``record.module``），这样一处调用、行标永远对得上代码位置。
    """
    return logging.getLogger(_root_logger_name + ("." + module if module else ""))


def log_kv(logger: logging.Logger, level: int, msg: str, **kv: object) -> None:
    """§5.3 的带键值对输出：``<时间> <LEVEL> <module> <msg> k1=v1 k2=v2``。

    推荐给新代码：不要把这些值拼进 msg 里（规矩 §5.3「禁止把变量塞进 message 里」），
    走这里让 kv 落在行尾固定位置，便于自动解析。
    """
    tail = " ".join("%s=%s" % (key, value) for key, value in kv.items())
    logger.log(level, "%s %s" % (msg, tail) if tail else msg)


def log_operation(what: str) -> None:
    """§5.2 客户操作通道：同步追加写 ``<log_dir>/operation.log``。

    行格式与 core 侧 ``Logger::operation()`` 逐字同构（``<ISO8601> OPERATION <what>``）。
    写入失败只记一条告警，**不阻断客户操作本身** —— 留痕是附属职责。
    """
    line = "%s OPERATION %s\n" % (iso8601_local(), what)
    try:
        path = os.path.join(_paths.log_dir(), _OPERATION_FILE)
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(line)
    except OSError as exc:
        logging.getLogger(_root_logger_name).warning(
            "operation.log 写入失败 (%s): %s", exc, what)
