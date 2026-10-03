# heartbeat_worker.py — M2.07 心跳守护线程（D3 裁决：心跳归 web 进程）
#
# 设计裁决依据（impl-spec §0 D3 / §3.1）：
#   · web 挂 ≠ AI 停：core 完全不依赖云端可达性；心跳只影响面板"在线"显示。
#   · 心跳失败/超时**不锁 AI**：仅面板状态提示；只有云端 403（到期/禁用，权威否定）
#     才触发 on_expired 回调 → web 侧发 core ACTIVATE_CLOUD{deactivate:true} 立即锁定。
#   · token 过期（401）⇒ 用 session 内 card_key 自动重 card-login（P0-4）。
#   · ★ 403 需再分层（2026-09-19 真机发现）：服务端把两种 403 都答成同一句话时，
#     "会话行被置 invalid"（清理任务/换绑/多端登录顶号）与"卡级否定"（到期/禁用）
#     无法区分 ⇒ 前者也会走 on_expired 把 core 锁死，且此后不再重登（只 401 才重登）
#     ⇒ 真机实测表现 = 激活后 1 分钟内在面板看到"登录状态已失效"、core 掉 restricted，
#     重启也不自愈。现按**契约消息**分层：`_SESSION_LOSS_MARKERS` 命中 ⇒ 先重登自愈
#     （连续 `_SESSION_RECOVER_MAX` 次仍失败才降级为锁）；其余 403 维持原快路径锁死。
#     配套：服务端 bridge 必须对 forceOffline 与 isValid 给出**不同**文案（见 bridge
#     `license.js` 心跳段），否则强制下线会被自动重登抵消。
#   · 网络失败：5s 起指数退避封顶 60s；连续失败超过 heartbeat_timeout（默认 180s）
#     才把 snapshot.online 置 False（断网不误杀在线态；断网宽限 Q3/Q4）。
# 全程不阻塞 Flask 请求（daemon 线程；stop() 用 Event 等待替代 sleep 以便快速退出）。
from __future__ import annotations

import threading
import time
from typing import Callable

from .cloud_client import CloudLicenseClient, CloudLicenseError
from .cloud_session import CloudSessionStore, device_serial

_BACKOFF_BASE_S = 5.0
_BACKOFF_CAP_S = 60.0
_NO_SESSION_POLL_S = 10.0

# 会话级失效的 403 文案（服务端 bridge `license.js` 心跳段：isValid != 1 时下发）。
# 命中 ⇒ 允许用落盘 card_key 重登自愈；**不**命中（卡到期/禁用/强制下线）⇒ 照旧锁死。
_SESSION_LOSS_MARKERS = ('登录状态已失效',)
# 连续自愈上限：防止"重登→立刻又失效"的服务端故障把循环打成热循环（无 sleep 连打）。
_SESSION_RECOVER_MAX = 3


def _is_session_loss(message: str) -> bool:
    """403 文案是否属于"会话级失效"（可重登自愈）。纯函数，便于单测。"""
    return any(m in (message or '') for m in _SESSION_LOSS_MARKERS)


class HeartbeatWorker:
    """云端心跳循环（daemon 线程）。snapshot() 供 /api/license 投影（§3.3）。"""

    def __init__(self,
                 client: CloudLicenseClient,
                 session: CloudSessionStore,
                 on_expired: Callable[[], None] | None = None,
                 client_version: str = '',
                 machine_code: Callable[[], str] | None = None,
                 fast_tick_s: float | None = None) -> None:
        # fast_tick_s：测试注入 —— 覆盖循环睡眠时长（板端恒 None）。
        self._client = client
        self._session = session
        self._on_expired = on_expired
        self._version = client_version
        self._machine_code = machine_code or device_serial
        self._fast_tick_s = fast_tick_s
        self._lock = threading.Lock()
        self._stop_evt = threading.Event()
        self._thread: threading.Thread | None = None
        self._expired_sent = False   # 403 权威否定已发过 on_expired（恢复成功后复位）
        self._state: dict = {
            'online': False,
            'last_ok_at': 0.0,
            'expire_at': '',
            'interval': 60,
            'timeout': 180,
            'error': '',
        }

    # ------------------------------------------------------------------
    # 生命周期
    # ------------------------------------------------------------------
    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop_evt.clear()
        self._thread = threading.Thread(target=self._loop, name='ttbox-heartbeat',
                                        daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_evt.set()
        t = self._thread
        if t is not None and t.is_alive() and t is not threading.current_thread():
            t.join(timeout=3.0)

    def running(self) -> bool:
        t = self._thread
        return t is not None and t.is_alive()

    # ------------------------------------------------------------------
    # 投影（/api/license 的 cloud.heartbeat 子块）
    # ------------------------------------------------------------------
    def snapshot(self) -> dict:
        with self._lock:
            return dict(self._state)

    def _set_state(self, **kw) -> None:
        with self._lock:
            self._state.update(kw)

    # ------------------------------------------------------------------
    # 主循环
    # ------------------------------------------------------------------
    def _sleep(self, seconds: float) -> None:
        if self._fast_tick_s is not None:
            seconds = min(seconds, self._fast_tick_s)
        self._stop_evt.wait(max(0.01, seconds))

    def _loop(self) -> None:
        fail_since: float | None = None
        backoff = _BACKOFF_BASE_S
        recover_left = _SESSION_RECOVER_MAX
        while not self._stop_evt.is_set():
            sess = self._session.load()
            token = str(sess.get('client_token') or '')
            card_key = str(sess.get('card_key') or '')
            if not token or not card_key:
                # 未登录云端（未激活/会话缺失）⇒ 面板仍可用，仅 online=false
                fail_since = None
                backoff = _BACKOFF_BASE_S
                self._set_state(online=False, error='')
                self._sleep(_NO_SESSION_POLL_S)
                continue

            interval = self._state_interval()
            try:
                result = self._client.heartbeat(token, self._machine_code(),
                                                self._version)
            except CloudLicenseError as e:
                if e.status == 401:
                    # token 过期 ⇒ 用落盘 card_key 自动重登（P0-4）
                    login = self._login_again()
                    if login is True:
                        fail_since = None
                        backoff = _BACKOFF_BASE_S
                        # ★ 2026-09-26（第四轮审计）：重登成功也不能全速补打 ——
                        #   服务端对新 token 也持续 401（token 校验回归/时钟漂移签名
                        #   过窗）时，旧实现是无 sleep 热循环，对云端自我 DoS。
                        self._sleep(_BACKOFF_BASE_S)
                        continue  # 用新 token 补一次心跳
                    if login == 'network':
                        # 网络异常 ≠ 权威否定：按网络类失败退避，不烧任何配额
                        now = time.time()
                        if fail_since is None:
                            fail_since = now
                        self._set_state(error='token 过期重登遇网络异常')
                        self._sleep(self._backoff_or_interval(backoff, interval, True))
                        backoff = min(backoff * 2, _BACKOFF_CAP_S)
                        continue
                    self._set_state(error='token 已过期，重新登录失败')
                    self._sleep(self._backoff_or_interval(backoff, interval, True))
                    backoff = min(backoff * 2, _BACKOFF_CAP_S)
                    continue
                if e.status == 403:
                    msg = e.message or ''
                    # 会话级失效（会话行被置 invalid）⇒ 先试重登自愈；失败/超限才锁。
                    if _is_session_loss(msg) and recover_left > 0:
                        login = self._login_again()
                        if login is True:
                            fail_since = None
                            backoff = _BACKOFF_BASE_S
                            recover_left -= 1
                            self._sleep(_BACKOFF_BASE_S)   # 防热循环（同 401 分支）
                            continue  # 立即用新 token 补一次心跳
                        if login == 'network':
                            # ★ 2026-09-26：瞬时断网 ≠ 权威否定 —— 旧实现把它当
                            # "重登失败"直接烧光自愈机会并锁死 core，弱网下把本可
                            # 自愈的会话失效放大成永久锁机。改按网络失败退避重试。
                            now = time.time()
                            if fail_since is None:
                                fail_since = now
                            self._set_state(error='会话失效恢复中（网络异常），稍后重试')
                            self._sleep(self._backoff_or_interval(backoff, interval, True))
                            backoff = min(backoff * 2, _BACKOFF_CAP_S)
                            continue
                        recover_left = 0
                    # 云端权威否定（到期/禁用/强制下线）⇒ 快路径：回调让 core 立即锁定（D4）
                    fail_since = None
                    self._set_state(online=False, error=msg or '卡密已到期')
                    # ★ 2026-09-26：on_expired 只发一次（成功恢复后复位）—— 旧实现
                    #   锁死后每轮心跳仍 403，ACTIVATE_CLOUD{deactivate} 每分钟重发。
                    if self._on_expired is not None and not self._expired_sent:
                        self._expired_sent = True
                        try:
                            self._on_expired()
                        except Exception:
                            pass
                    self._sleep(_BACKOFF_CAP_S)
                    continue
                # 网络类失败：不触碰 core（Q3：断网 ≠ AI 停）；
                # 连续失败 > heartbeat_timeout 才把 online 置 False
                now = time.time()
                if fail_since is None:
                    fail_since = now
                timeout_s = max(30, self._state_timeout())
                offline = (now - fail_since) > timeout_s
                self._set_state(error=e.message or '网络异常',
                                **({'online': False} if offline else {}))
                self._sleep(self._backoff_or_interval(backoff, interval, True))
                backoff = min(backoff * 2, _BACKOFF_CAP_S)
                continue

            # 成功：退避清零，采纳云端下发的节奏参数
            fail_since = None
            backoff = _BACKOFF_BASE_S
            recover_left = _SESSION_RECOVER_MAX
            self._expired_sent = False   # 恢复正常 ⇒ 允许下一次权威否定再发 on_expired
            interval = int(result.get('heartbeat_interval') or interval)
            self._set_state(online=True, last_ok_at=time.time(),
                            expire_at=str(sess.get('expire_at') or ''),
                            interval=interval,
                            timeout=int(result.get('heartbeat_timeout')
                                        or self._state_timeout()),
                            error='')
            # 云端可下发 interval ⇒ 持久化，重启后立即按新节奏跑
            # ★ 2026-09-26：sess 是至多一个 interval 之前的旧快照，整体回写会把
            #   期间面板 card-login 写入的新 token/expire 顶掉（"激活后偶发掉线"）。
            #   以最新落盘会话为 base 合并节奏字段；token 已变则本轮放弃持久化。
            fresh = self._session.load()
            if not isinstance(fresh, dict) or not fresh:
                fresh = sess
            if str(fresh.get('client_token') or '') != token:
                self._sleep(interval)
                continue
            upd = dict(fresh)
            upd['heartbeat_interval'] = interval
            upd['heartbeat_timeout'] = self._state_timeout()
            self._session.save(upd)
            self._sleep(interval)

    # ------------------------------------------------------------------
    # token 过期自动重登
    # ------------------------------------------------------------------
    def _login_again(self) -> bool:
        """重新 card-login。返回 True=成功 / False=卡被否定 / 'network'=瞬时网络异常。
        ★ 2026-09-26：网络失败与权威否定必须区分 —— 调用方（403 会话自愈窗口）
        把网络异常当"重登失败"会误锁 core。"""
        sess = self._session.load()
        card_key = str(sess.get('card_key') or '')
        if not card_key:
            return False
        try:
            result = self._client.card_login(card_key, self._machine_code())
        except CloudLicenseError as e:
            if e.status == 0:
                return 'network'
            return False
        # ★ 以最新落盘会话为 base（面板可能刚写过），整体覆盖前先保住新字段
        fresh = self._session.load()
        if not isinstance(fresh, dict) or not fresh:
            fresh = sess
        upd = dict(fresh)
        upd.update({
            'card_key': card_key,
            'client_token': result['client_token'],
            'expire_at': result['expire_at'],
            'expire_unix_ms': int(result['expire_unix_s']) * 1000,
            'max_devices': result['max_devices'],
            'heartbeat_interval': result['heartbeat_interval'],
            'heartbeat_timeout': result['heartbeat_timeout'],
        })
        self._session.save(upd)
        self._expired_sent = False
        self._set_state(error='', expire_at=result['expire_at'])
        return True

    # ------------------------------------------------------------------
    def _state_interval(self) -> int:
        with self._lock:
            return int(self._state.get('interval') or 60)

    def _state_timeout(self) -> int:
        with self._lock:
            return int(self._state.get('timeout') or 180)

    def _backoff_or_interval(self, backoff: float, interval: int,
                             use_backoff: bool) -> float:
        if not use_backoff:
            return float(interval)
        return max(1.0, min(backoff, _BACKOFF_CAP_S, float(interval)))
