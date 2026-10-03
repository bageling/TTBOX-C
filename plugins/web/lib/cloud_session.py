# cloud_session.py — M2.07 云端会话持久化 + 设备指纹（单源）助手
#
# 职责（impl-spec §3.1 CloudSessionStore）：
#   1. cloud_session.json 读写：0600 原子写（tmp → fsync → rename；LicenseStore 同工艺
#      的 Python 等价形，工艺搬自已退役的 _save_credentials）。
#   2. device_serial()：/proc/cpuinfo `Serial` 行 —— 与 core `DeviceFingerprint::bind_string()`
#      同源同算法（machine_code 单源，§7.4）。
#   3. parse_expire_at()：云端 `expire_at` 北京时间串（'YYYY-MM-DD HH:MM:SS'，固定 UTC+8）
#      → unix 秒。★ 全仓唯一换算点（§7.2：不得二次实现）。
#   4. card_mask()：卡号短码派生（展示用，非安全边界）。
#
# schema（§7.6，0600）：
#   {card_key, client_token, expire_at, expire_unix_ms, max_devices,
#    heartbeat_interval, heartbeat_timeout, updated_at}
# 损坏/缺失 ⇒ 视为未登录云端（面板仍可用，仅心跳 online=false）。
from __future__ import annotations

import json
import os
import time
import uuid
from datetime import datetime, timedelta, timezone

from . import paths as _paths

# 云端 expire_at 的固定时区：北京时间 UTC+8（契约钉死，不做本地时区推断）
_CLOUD_TZ = timezone(timedelta(hours=8))

SESSION_FILENAME = 'cloud_session.json'


def _default_session_path() -> str:
    # 与其它状态同区：<config_dir>（0700，fhs_init 已建）。路径经 lib/paths.py 派生
    # （A-PATH-4）：散写 "/opt/ttbox/config/…" 会让 TTBOX_PREFIX 失效。
    # 环境变量覆盖仅供宿主侧测试注入，板端恒用默认路径。
    return os.environ.get('TTBOX_CLOUD_SESSION',
                          _paths.join_path(_paths.config_dir(), 'cloud_session.json'))


class CloudSessionStore:
    """cloud_session.json 的读写器（0600 原子写；损坏 ⇒ 空会话，绝不抛）。"""

    def __init__(self, path: str | None = None) -> None:
        self.path = path or _default_session_path()

    # ---- 读 ----
    def load(self) -> dict:
        """读会话；缺失/损坏/非 dict ⇒ {}（语义 = 未登录云端，不阻断面板）。"""
        try:
            with open(self.path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            return data if isinstance(data, dict) else {}
        except Exception:
            return {}

    # ---- 写（原子 + 0600）----
    def save(self, session: dict) -> bool:
        """原子保存会话。跨平台目录 fsync 门控（Windows 无目录 fsync 语义）。"""
        try:
            d = os.path.dirname(self.path)
            os.makedirs(d, mode=0o700, exist_ok=True)
            doc = dict(session)
            doc['updated_at'] = int(time.time())
            tmp = (f'{self.path}.tmp.{os.getpid()}.'
                   f'{uuid.uuid4().hex[:8]}')
            fd = os.open(tmp, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o600)
            try:
                os.write(fd, json.dumps(doc, ensure_ascii=False).encode('utf-8'))
                os.fsync(fd)
            finally:
                os.close(fd)
            os.replace(tmp, self.path)
            if os.name == 'posix':  # 目录 fsync 仅 POSIX 有效（对齐 C++ 侧工艺）
                dfd = os.open(d, os.O_RDONLY)
                try:
                    os.fsync(dfd)
                finally:
                    os.close(dfd)
            return True
        except Exception:
            return False

    def clear(self) -> None:
        """清除会话（best-effort；文件不存在视为已清除）。"""
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass
        except Exception:
            pass


def device_serial() -> str:
    """cpu_serial 单源（/proc/cpuinfo `Serial` 行）。

    与 core `read_cpuinfo_serial()` 同源同算法；读取失败 ⇒ ''（调用方负责
    回退 GET_STATUS.license.bind_device 或报错，不得在本层伪造指纹）。
    """
    try:
        with open('/proc/cpuinfo', 'r') as f:
            for line in f:
                if line.startswith('Serial'):
                    return line.split(':', 1)[1].strip()
    except Exception:
        pass
    return ''


def parse_expire_at(value: str) -> int:
    """云端 expire_at（北京时间串 'YYYY-MM-DD HH:MM:SS'）→ unix 秒。

    ★ 全仓唯一换算点（§7.2）。解析固定 UTC+8；非法/空 ⇒ 0（调用方按"无到期"处理）。
    """
    try:
        s = str(value or '').strip()
        if not s:
            return 0
        dt = datetime.strptime(s, '%Y-%m-%d %H:%M:%S').replace(tzinfo=_CLOUD_TZ)
        return int(dt.timestamp())
    except (ValueError, TypeError, OverflowError, OSError):
        return 0


def card_mask(card_key: str) -> str:
    """卡号 → 可读短码 'LS-****-XXXX'（前 5 + **** + 后 4；过短卡整体打码）。"""
    s = str(card_key or '').strip()
    if not s:
        return ''
    if len(s) <= 9:
        return '****'
    return f'{s[:5]}****{s[-4:]}'
