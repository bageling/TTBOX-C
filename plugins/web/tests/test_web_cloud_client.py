# test_web_cloud_client.py — M2.07 T02 云端客户端 / 心跳 worker 单测
#
# 覆盖（impl-spec T02 验收要点）：
#   ① 固定 ts/nonce 签名重放：HMAC 逐字节一致，canonical 无末尾换行（GET 空 body 自然收尾）；
#   ② card 字段名 camelCase（appKey/cardKey/machineCode）与实测契约一致；
#   ③ 400/403 错误语义原文透传（CloudLicenseError.message = 云端原文）；
#   ④ heartbeat Bearer 头 + camelCase body（machineCode/clientVersion）；
#   ⑤ token 过期 401 ⇒ worker 自动重 card-login（mock 客户端断言）；
#   ⑥ license_base_url 热读：改配置后下一请求立即走新地址；
#   ⑦ 网络失败 180s 内 online 不翻 false；403 ⇒ on_expired 回调触发。
#
# 运行：python -m pytest plugins/web/tests/test_web_cloud_client.py -v（从仓库根）
from __future__ import annotations

import hashlib
import hmac
import http.server
import json
import sys
import tempfile
import threading
import time
import pathlib
import urllib.error
import urllib.request

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_DIR = REPO_ROOT / 'plugins' / 'web'

# ★ P6：不再替被测代码插 sys.path（原来 insert(0, REPO_ROOT/WEB_DIR) 换掉了
#   "lib 包能不能在正常入口下导入"这件事）。`plugins.web.lib.*` 是包内全路径导入，
#   只需要仓库根在 sys.path —— `python -m pytest` 天然满足。
from plugins.web.lib.cloud_client import (CloudLicenseClient, CloudLicenseError,   # noqa: E402
                                          DEFAULT_APP_SECRET, PATH_CARD_LOGIN,
                                          PATH_HEARTBEAT)
from plugins.web.lib.cloud_session import CloudSessionStore, card_mask, parse_expire_at  # noqa: E402
from plugins.web.lib.heartbeat_worker import HeartbeatWorker                       # noqa: E402

TEST_SECRET = 'unit-test-secret-0123456789abcdef'


# ======================================================================
# mock 云端 HTTP 服务器（记录请求；用例可编程响应）
# ======================================================================
class _Handler(http.server.BaseHTTPRequestHandler):
    server_version = 'MockLicenseSaaS/1.0'

    def log_message(self, *args):  # 静默
        pass

    def _read_body(self) -> bytes:
        length = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(length) if length > 0 else b''

    def _respond(self, status: int, payload: dict) -> None:
        raw = json.dumps(payload, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_POST(self):  # noqa: N802
        self.server.requests.append({
            'method': 'POST', 'path': self.path,
            'headers': dict(self.headers.items()), 'body': self._read_body(),
        })
        cfg = self.server.behavior.get(self.path)
        if cfg is None:
            self._respond(404, {'error': 'no route'})
            return
        status, payload = cfg
        self._respond(status, payload)

    def do_GET(self):  # noqa: N802
        self.server.requests.append({
            'method': 'GET', 'path': self.path,
            'headers': dict(self.headers.items()), 'body': b'',
        })
        cfg = self.server.behavior.get(self.path)
        if cfg is None:
            self._respond(404, {'error': 'no route'})
            return
        status, payload = cfg
        self._respond(status, payload)


class MockCloud:
    """可编程云端桩：behavior = {path: (status, payload)}；requests 记录全部请求。"""

    def __init__(self):
        self.behavior = {}
        self.requests = []
        handler = type('BoundHandler', (_Handler,), {})
        self._srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        self._srv.behavior = self.behavior
        self._srv.requests = self.requests
        self._thread = threading.Thread(target=self._srv.serve_forever, daemon=True)
        self._thread.start()
        self.url = f'http://127.0.0.1:{self._srv.server_port}'

    def close(self):
        self._srv.shutdown()
        self._srv.server_close()

    def set(self, path: str, status: int, payload: dict) -> None:
        self.behavior[path] = (status, payload)


def make_client(url: str, override: dict | None = None) -> CloudLicenseClient:
    cfg = {'cloud': {'license_base_url': url, 'app_key': 'ttbox',
                     'app_secret': TEST_SECRET}}
    if override:
        cfg.update(override)
    return CloudLicenseClient(lambda: cfg)


OK_LOGIN = {'ok': True, 'mode': 'card', 'client_token': 'tok-abc',
            'expire_at': '2026-10-17 18:00:00', 'max_devices': 3,
            'heartbeat_interval': 60, 'heartbeat_timeout': 180}


# ======================================================================
# ① 签名重放（固定 ts/nonce）
# ======================================================================
def test_sign_replay_no_trailing_newline():
    client = make_client('http://127.0.0.1:1')
    # 固定 ts/nonce，重放比对：canonical = METHOD\nPATH\nTS\nNONCE\nBODY（无尾换行）
    ts, nonce = '1760000000', 'deadbeefdeadbeef'
    body = json.dumps({'appKey': 'ttbox', 'cardKey': 'K', 'machineCode': 'M'},
                      ensure_ascii=False)
    got = client._sign('POST', PATH_CARD_LOGIN, ts, nonce, body)
    canonical = 'POST\n' + PATH_CARD_LOGIN + '\n' + ts + '\n' + nonce + '\n' + body
    expect = hmac.new(TEST_SECRET.encode(), canonical.encode('utf-8'),
                      hashlib.sha256).hexdigest()
    assert got == expect
    assert len(got) == 64 and got == got.lower()
    # 关键：canonical 不得有末尾换行（带尾换行实测 401）
    assert not canonical.endswith('\n\n')


def test_sign_get_empty_body_canonical():
    client = make_client('http://127.0.0.1:1')
    ts, nonce = '1760000001', 'cafecafecafecafe'
    got = client._sign('GET', '/api/client/app-info?appKey=ttbox', ts, nonce, '')
    # GET 空 body：join 语义 ⇒ canonical 以 "\n" 结束（nonce 与空 body 之间唯一一个 \n）
    canonical = 'GET\n/api/client/app-info?appKey=ttbox\n' + ts + '\n' + nonce + '\n'
    expect = hmac.new(TEST_SECRET.encode(), canonical.encode('utf-8'),
                      hashlib.sha256).hexdigest()
    assert got == expect


# ======================================================================
# ② card-login：camelCase 字段 + 四头 + 响应归一
# ======================================================================
def test_card_login_camelcase_and_success():
    cloud = MockCloud()
    try:
        cloud.set(PATH_CARD_LOGIN, 200, OK_LOGIN)
        client = make_client(cloud.url)
        result = client.card_login('LS-TTBOX-TEST-0001-M2X7', '9ecf26dc8154491')
        req = cloud.requests[-1]
        assert req['path'] == PATH_CARD_LOGIN
        # 四头齐全
        assert req['headers'].get('X-App-Key') == 'ttbox'
        assert req['headers'].get('X-Timestamp', '').isdigit()
        assert req['headers'].get('X-Nonce')
        sig = req['headers'].get('X-Signature', '')
        assert len(sig) == 64
        # body 字段 camelCase（snake_case 实测 400）
        body = json.loads(req['body'].decode('utf-8'))
        assert set(body) == {'appKey', 'cardKey', 'machineCode'}
        assert body['cardKey'] == 'LS-TTBOX-TEST-0001-M2X7'
        # 归一输出：expire_unix_s 由北京时间串（固定 UTC+8）换算
        assert result['client_token'] == 'tok-abc'
        assert result['expire_unix_s'] == parse_expire_at('2026-10-17 18:00:00')
        assert result['max_devices'] == 3
        assert result['heartbeat_interval'] == 60
        assert result['heartbeat_timeout'] == 180
        # 签名与实际发送字节一致（服务端可用同 secret 复算）
        ts = req['headers']['X-Timestamp']
        nonce = req['headers']['X-Nonce']
        raw_body = req['body'].decode('utf-8')
        canonical = 'POST\n' + PATH_CARD_LOGIN + '\n' + ts + '\n' + nonce + '\n' + raw_body
        expect = hmac.new(TEST_SECRET.encode(), canonical.encode('utf-8'),
                          hashlib.sha256).hexdigest()
        assert sig == expect
    finally:
        cloud.close()


def test_parse_expire_at_fixed_utc8():
    # 固定 UTC+8：2026-10-17 18:00:00 CST == 10:00:00 UTC
    from datetime import datetime, timedelta, timezone
    expect = int(datetime(2026, 10, 17, 10, 0, 0,
                          tzinfo=timezone.utc).timestamp())
    assert parse_expire_at('2026-10-17 18:00:00') == expect
    assert parse_expire_at('') == 0
    assert parse_expire_at('not-a-date') == 0


def test_card_mask():
    assert card_mask('LS-TTBOX-TEST-0001-M2X7') == 'LS-TT****M2X7'
    assert card_mask('') == ''
    assert card_mask('SHORT') == '****'


# ======================================================================
# ③ 错误语义原文透传
# ======================================================================
def test_card_login_400_message_passthrough():
    cloud = MockCloud()
    try:
        cloud.set(PATH_CARD_LOGIN, 400, {'message': '卡密和机器码不能为空'})
        client = make_client(cloud.url)
        with pytest.raises(CloudLicenseError) as ei:
            client.card_login('', '')
        assert ei.value.status == 400
        assert ei.value.code == 'card_invalid'
        assert ei.value.message == '卡密和机器码不能为空'
    finally:
        cloud.close()


def test_card_login_403_expired_and_machine():
    cloud = MockCloud()
    try:
        client = make_client(cloud.url)
        cloud.set(PATH_CARD_LOGIN, 403, {'error': '卡密已到期'})
        with pytest.raises(CloudLicenseError) as ei:
            client.card_login('K', 'M')
        assert ei.value.status == 403 and ei.value.code == 'expired'
        assert ei.value.message == '卡密已到期'
        cloud.set(PATH_CARD_LOGIN, 403, {'error': '机器码不匹配'})
        with pytest.raises(CloudLicenseError) as ei2:
            client.card_login('K', 'M')
        assert ei2.value.code == 'machine_mismatch'
    finally:
        cloud.close()


def test_network_error_normalized(monkeypatch):
    # 连接层失败（拒绝/超时/DNS）⇒ status=0 / code=network（不抛裸 URLError）。
    # ★ 宿主机可能配置 HTTP(S)_PROXY：urllib 会先把 127.0.0.1:1 交给代理并收到 502，
    #   使"连不上"退化为"502"。故此处让底层 urlopen 直接抛 URLError，做到与代理无关的确定性。
    client = make_client('http://127.0.0.1:1')

    def _boom(*args, **kwargs):
        raise urllib.error.URLError('connection refused')

    monkeypatch.setattr(urllib.request, 'urlopen', _boom)
    with pytest.raises(CloudLicenseError) as ei:
        client.card_login('K', 'M')
    assert ei.value.status == 0 and ei.value.code == 'network'


def test_missing_secret_fail_closed():
    cfg = {'cloud': {'license_base_url': 'http://127.0.0.1:1', 'app_key': 'ttbox'}}
    client = CloudLicenseClient(lambda: cfg)
    with pytest.raises(CloudLicenseError) as ei:
        client.card_login('K', 'M')
    assert ei.value.code == 'not_configured'
    assert '未配置' in ei.value.message


# ======================================================================
# ⑥ base_url 热读
# ======================================================================
def test_base_url_hot_reload():
    cloud_a = MockCloud()
    cloud_b = MockCloud()
    try:
        holder = {'cfg': {'cloud': {'license_base_url': cloud_a.url,
                                    'app_key': 'ttbox', 'app_secret': TEST_SECRET}}}
        client = CloudLicenseClient(lambda: holder['cfg'])
        cloud_a.set(PATH_CARD_LOGIN, 403, {'error': 'old-url'})
        cloud_b.set(PATH_CARD_LOGIN, 200, OK_LOGIN)
        with pytest.raises(CloudLicenseError):
            client.card_login('K', 'M')
        # 改配置 ⇒ 下一次请求立即走新地址（无重启，B15-8）
        holder['cfg'] = {'cloud': {'license_base_url': cloud_b.url,
                                   'app_key': 'ttbox', 'app_secret': TEST_SECRET}}
        result = client.card_login('K', 'M')
        assert result['client_token'] == 'tok-abc'
        assert cloud_b.requests[-1]['path'] == PATH_CARD_LOGIN
    finally:
        cloud_a.close()
        cloud_b.close()


# ======================================================================
# ④ heartbeat：Bearer + camelCase body
# ======================================================================
def test_heartbeat_bearer_and_body():
    cloud = MockCloud()
    try:
        cloud.set(PATH_HEARTBEAT, 200, {'ok': True, 'server_time': 1760000000,
                                        'heartbeat_interval': 45,
                                        'heartbeat_timeout': 180})
        client = make_client(cloud.url)
        result = client.heartbeat('tok-abc', '9ecf26dc8154491', '2026.08.03.1')
        req = cloud.requests[-1]
        assert req['path'] == PATH_HEARTBEAT
        assert req['headers'].get('Authorization') == 'Bearer tok-abc'
        body = json.loads(req['body'].decode('utf-8'))
        assert set(body) == {'machineCode', 'clientVersion'}
        assert body['machineCode'] == '9ecf26dc8154491'
        assert result['heartbeat_interval'] == 45
    finally:
        cloud.close()


# ======================================================================
# ⑤⑦ HeartbeatWorker：401 重登 / 403 回调 / 网络宽限
# ======================================================================
class FakeClient:
    """编程式云端桩（绕过 HTTP 层，直接测 worker 策略）。"""

    def __init__(self):
        self.heartbeat_results = []   # 每次心跳弹出：dict 或 CloudLicenseError
        self.card_login_results = []
        self.heartbeat_calls = 0
        self.login_calls = 0

    def heartbeat(self, token, machine, version):
        self.heartbeat_calls += 1
        item = self.heartbeat_results.pop(0)
        if isinstance(item, CloudLicenseError):
            raise item
        return item

    def card_login(self, card_key, machine):
        self.login_calls += 1
        item = self.card_login_results.pop(0)
        if isinstance(item, CloudLicenseError):
            raise item
        # 忠实复刻 CloudLicenseClient.card_login 的**归一输出**（含 expire_unix_s）：
        # HeartbeatWorker._login_again 依赖该字段；桩少一字段会掩盖真实契约（曾致 401 重登用例假红）。
        out = dict(item)
        if 'expire_unix_s' not in out:
            out['expire_unix_s'] = parse_expire_at(str(out.get('expire_at') or ''))
        return out


def make_worker(tmp_path, client, on_expired=None):
    session = CloudSessionStore(str(tmp_path / 'cloud_session.json'))
    session.save({'card_key': 'LS-TTBOX-TEST-0001-M2X7', 'client_token': 'tok-1',
                  'expire_at': '2026-10-17 18:00:00',
                  'heartbeat_interval': 60, 'heartbeat_timeout': 180})
    worker = HeartbeatWorker(client, session, on_expired=on_expired,
                             client_version='test', machine_code=lambda: 'SERIAL01',
                             fast_tick_s=0.02)
    return session, worker


def test_worker_relogin_on_401(tmp_path):
    client = FakeClient()
    client.heartbeat_results = [
        CloudLicenseError(401, 'unauthorized', 'token invalid'),
        {'server_time': 1, 'heartbeat_interval': 60, 'heartbeat_timeout': 180},
    ]
    client.card_login_results = [
        dict(OK_LOGIN, client_token='tok-2'),
    ]
    session, worker = make_worker(tmp_path, client)
    worker.start()
    deadline = time.time() + 5
    while time.time() < deadline:
        if client.login_calls >= 1 and client.heartbeat_calls >= 2:
            break
        time.sleep(0.02)
    worker.stop()
    assert client.login_calls >= 1, '401 后必须自动重 card-login'
    assert session.load()['client_token'] == 'tok-2'
    assert worker.snapshot()['online'] is True


def test_worker_403_triggers_on_expired(tmp_path):
    client = FakeClient()
    client.heartbeat_results = [CloudLicenseError(403, 'expired', '卡密已到期')]
    fired = []
    session, worker = make_worker(tmp_path, client, on_expired=lambda: fired.append(1))
    worker.start()
    deadline = time.time() + 5
    while time.time() < deadline:
        if fired:
            break
        time.sleep(0.02)
    worker.stop()
    assert fired, '403 权威否定必须触发 on_expired（快路径 deactivate）'
    snap = worker.snapshot()
    assert snap['online'] is False
    assert '到期' in snap['error']


def test_is_session_loss_predicate():
    """403 文案分层判据（纯函数）：只有"会话级失效"允许重登自愈。"""
    from plugins.web.lib.heartbeat_worker import _is_session_loss
    assert _is_session_loss('登录状态已失效，请重新激活') is True
    assert _is_session_loss('卡密已到期') is False
    assert _is_session_loss('卡密已被禁用') is False
    # ★ 管理员强制下线必须**不**命中，否则自动重登会把强踢抵消掉
    assert _is_session_loss('会话已被强制下线，请联系管理员') is False
    assert _is_session_loss('') is False
    assert _is_session_loss(None) is False


def test_worker_403_session_loss_relogins_without_locking(tmp_path):
    """服务端只把会话行置 invalid ⇒ 必须重登自愈，**不得**锁 core。

    2026-09-19 真机缺陷回归：bridge 把 isValid=0 与 forceOffline=1 答成同一句
    '登录状态已失效，请重新激活' ⇒ worker 走 403 快路径 on_expired ⇒ core 掉
    restricted，且此后只在 401 才重登 ⇒ 重启也不自愈（实测激活后 1 分钟内掉线）。
    """
    client = FakeClient()
    client.heartbeat_results = [
        CloudLicenseError(403, 'session_invalid', '登录状态已失效，请重新激活'),
    ] + [{'server_time': 1, 'heartbeat_interval': 60, 'heartbeat_timeout': 180}] * 50
    client.card_login_results = [dict(OK_LOGIN, client_token='tok-2')]
    fired = []
    session, worker = make_worker(tmp_path, client, on_expired=lambda: fired.append(1))
    worker.start()
    deadline = time.time() + 5
    while time.time() < deadline:
        if client.login_calls >= 1 and worker.snapshot()['online'] is True:
            break
        time.sleep(0.02)
    worker.stop()
    assert client.login_calls == 1, '会话级 403 必须触发一次重登'
    assert not fired, '会话级 403 不得锁 core（on_expired 不得触发）'
    assert session.load()['client_token'] == 'tok-2', '新 token 必须落盘'
    snap = worker.snapshot()
    assert snap['online'] is True and snap['error'] == ''


def test_worker_403_forced_offline_still_locks(tmp_path):
    """管理员强制下线仍必须锁 core，且**不得**自动重登（否则强踢被抵消）。"""
    client = FakeClient()
    client.heartbeat_results = [
        CloudLicenseError(403, 'force_offline', '会话已被强制下线，请联系管理员'),
    ] * 50
    fired = []
    session, worker = make_worker(tmp_path, client, on_expired=lambda: fired.append(1))
    worker.start()
    deadline = time.time() + 5
    while time.time() < deadline:
        if fired:
            break
        time.sleep(0.02)
    worker.stop()
    assert client.login_calls == 0, '强制下线不得自动重登'
    assert fired, '强制下线必须锁 core'
    assert '强制下线' in worker.snapshot()['error']


def test_worker_403_session_loss_relogin_fails_locks(tmp_path):
    """自愈失败（卡已禁用/换绑超限等）⇒ 回落原快路径锁死。"""
    client = FakeClient()
    client.heartbeat_results = [
        CloudLicenseError(403, 'session_invalid', '登录状态已失效，请重新激活'),
    ] * 50
    client.card_login_results = [CloudLicenseError(403, 'disabled', '卡密已被禁用')]
    fired = []
    session, worker = make_worker(tmp_path, client, on_expired=lambda: fired.append(1))
    worker.start()
    deadline = time.time() + 5
    while time.time() < deadline:
        if fired:
            break
        time.sleep(0.02)
    worker.stop()
    assert client.login_calls == 1, '必须先试过一次重登'
    assert fired, '重登失败后必须锁 core'
    assert worker.snapshot()['online'] is False


def test_worker_session_loss_recovery_is_bounded(tmp_path):
    """服务端持续把会话打回 invalid 时，自愈次数必须封顶（防无 sleep 热循环）。"""
    from plugins.web.lib.heartbeat_worker import _SESSION_RECOVER_MAX
    client = FakeClient()
    client.heartbeat_results = [
        CloudLicenseError(403, 'session_invalid', '登录状态已失效，请重新激活'),
    ] * 50
    client.card_login_results = [dict(OK_LOGIN, client_token='tok-%d' % i)
                                 for i in range(1, 12)]
    fired = []
    session, worker = make_worker(tmp_path, client, on_expired=lambda: fired.append(1))
    worker.start()
    deadline = time.time() + 5
    while time.time() < deadline:
        if fired and client.login_calls >= _SESSION_RECOVER_MAX:
            break
        time.sleep(0.02)
    worker.stop()
    assert client.login_calls == _SESSION_RECOVER_MAX, '自愈次数必须恰好封顶'
    assert fired, '超限后必须回落锁死'


def test_worker_network_failure_grace_window(tmp_path):
    client = FakeClient()
    # 先成功一次建立在线态，随后持续网络失败
    client.heartbeat_results = [
        {'server_time': 1, 'heartbeat_interval': 60, 'heartbeat_timeout': 180},
    ] + [CloudLicenseError(0, 'network', '连接失败')] * 500
    session, worker = make_worker(tmp_path, client)
    worker.start()
    deadline = time.time() + 3
    while time.time() < deadline and client.heartbeat_calls < 4:
        time.sleep(0.02)
    mid = worker.snapshot()
    assert client.heartbeat_calls >= 4
    assert mid['online'] is True, '180s 宽限窗口内 online 不得翻 false'
    assert '失败' in mid['error'] or mid['error']
    worker.stop()


def test_session_store_roundtrip_0600(tmp_path):
    store = CloudSessionStore(str(tmp_path / 's.json'))
    assert store.load() == {}
    assert store.save({'card_key': 'K', 'client_token': 'T'})
    loaded = store.load()
    assert loaded['card_key'] == 'K'
    assert 'updated_at' in loaded
    store.clear()
    assert store.load() == {}
    # 损坏文件 ⇒ 空会话（不抛）
    (tmp_path / 'bad.json').write_text('{broken', encoding='utf-8')
    assert CloudSessionStore(str(tmp_path / 'bad.json')).load() == {}
