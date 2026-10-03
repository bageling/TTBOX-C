# cloud_client.py — M2.07 云端 License-SaaS 客户端（HMAC-SHA256 四头签名）
#
# 协议契约（主理人云端实测钉死，勿改；与 core/src/auth/TtboxLicenseClient 同协议参考）：
#   · base_url **无编译期缺省**（quick tunnel URL 重启会变 ⇒ 必须由配置项提供 +
#     每请求热读；config `cloud.license_base_url` 改后下一次请求立即生效，验收 B15-8）。
#   · 请求签名（/api/client/* 全部）：4 头
#       X-App-Key / X-Timestamp(Unix 秒, ±300s) / X-Nonce(每次随机) / X-Signature
#       X-Signature = hex(HMAC-SHA256(app_secret, canonical))
#       canonical   = HTTP方法 + "\n" + path带query + "\n" + ts + "\n" + nonce + "\n" + body原文
#     ★ canonical **无末尾换行**（主理人实测：带尾换行 401，无尾换行签名通过）。
#       GET 空 body 时 body 为空串 ⇒ canonical 以 nonce 后的 "\n" 结尾（join 语义自然覆盖）。
#   · body 字段名 **camelCase**（Go 端 struct 实测）：card-login {appKey,cardKey,machineCode}；
#     heartbeat {machineCode,clientVersion}。snake_case ⇒ 400。
#   · card-login：POST /api/client/card-login ⇒ 200 {ok,mode:"card",client_token,
#     expire_at:"YYYY-MM-DD HH:MM:SS"(北京),max_devices,heartbeat_interval:60,heartbeat_timeout:180}
#     400 卡密无效；403 机器码不匹配/已到期。
#   · heartbeat：POST /api/client/heartbeat，头 Authorization: Bearer <client_token>。
#   · app-info：GET /api/client/app-info?appKey=ttbox（P2 公告/强更预留）。
#
# 依赖：仅 stdlib（urllib.request / hmac / hashlib / json / time / secrets）——零新第三方包。
# 错误语义：统一 CloudLicenseError(status, code, message)；message = 云端原文（激活页透传）。
# 超时 8s；card_login 不自动重试（激活是用户动作，重试由人发起）——退避重试归 HeartbeatWorker。
from __future__ import annotations

import hashlib
import hmac
import json
import secrets
import time
import urllib.error
import urllib.request
from typing import Callable

# 配置缺省值（config/cloud.example.json 同源；环境变量可覆盖，云段热读优先级更高）
# ★ M2.07.1 安全收敛（方案 a：删编译期缺省 + fail-closed 占位符）：
#   生产云端凭据【绝不硬编码进源码】——否则随 .py / .pyc 出货即泄露。真值唯一落点 =
#   板端 `/opt/ttbox/config/default.json` 的 cloud.* 段（0640 root:ttbox）与本地密钥库，
#   **严禁入库**。此处仅保留**非生产占位符**（空串 = 无可用缺省）：
#     · _request() 在 cloud.app_secret 缺失/空时先 fail-closed（'云端凭据未配置'），
#       该守卫**先于任何签名**发生 ⇒ 占位符永不会参与真实签名（fail-closed 语义不变）；
#     · 出货开箱可用性不受影响：板端 default.json 出厂即含 license_base_url/app_secret，
#       web 每请求热读配置 ⇒ 删缺省不影响客户设备激活。
#   注：DEFAULT_APP_SECRET 保留为符号（值为空串）仅为兼容既有导入；不再是任何“真值”。
DEFAULT_LICENSE_BASE_URL = ''
DEFAULT_APP_KEY = 'ttbox'
DEFAULT_APP_SECRET = ''

REQUEST_TIMEOUT_S = 8.0

PATH_CARD_LOGIN = '/api/client/card-login'
PATH_HEARTBEAT = '/api/client/heartbeat'
PATH_APP_INFO = '/api/client/app-info'


class CloudLicenseError(Exception):
    """云端交互失败归一异常。

    status: HTTP 状态码（400/401/403/5xx）；网络失败/超时 = 0。
    code  : 归一错误码（'card_invalid'|'machine_mismatch'|'expired'|'forbidden'|
            'unauthorized'|'network'|'not_configured'|'http_error'）。
    message: 云端 error/message 字段原文（激活页原样透传，不做二次措辞，§7.1）。
    """

    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message or code)
        self.status = int(status)
        self.code = str(code)
        self.message = str(message or '')


def _classify_error(status: int, payload: dict) -> str:
    """HTTP 状态码 + 云端载荷 → 归一错误码（映射表唯一实现点）。"""
    msg = str(payload.get('error') or payload.get('message') or '')
    low = msg.lower()
    if status == 400:
        return 'card_invalid'
    if status == 401:
        return 'unauthorized'
    if status == 403:
        if '到期' in msg or 'expire' in low:
            return 'expired'
        if '机器' in msg or 'machine' in low or '绑定' in msg:
            return 'machine_mismatch'
        return 'forbidden'
    if status == 0:
        return 'network'
    return 'http_error'


class CloudLicenseClient:
    """云端 License-SaaS 客户端（签名 / card-login / heartbeat / app-info）。

    config_loader: 每次请求热读 TTBOX 配置（/opt/ttbox/config/default.json 全文 dict），
    取其中 `cloud` 段的 license_base_url / app_key / app_secret —— 配置变更即时生效。
    """

    def __init__(self, config_loader: Callable[[], dict]) -> None:
        self._config_loader = config_loader

    # ------------------------------------------------------------------
    # 配置热读
    # ------------------------------------------------------------------
    def _cloud_cfg(self) -> dict:
        try:
            cfg = self._config_loader() or {}
        except Exception:
            cfg = {}
        cloud = cfg.get('cloud')
        return cloud if isinstance(cloud, dict) else {}

    def base_url(self) -> str:
        return str(self._cloud_cfg().get('license_base_url')
                   or DEFAULT_LICENSE_BASE_URL).rstrip('/')

    def app_key(self) -> str:
        return str(self._cloud_cfg().get('app_key') or DEFAULT_APP_KEY)

    def app_secret(self) -> str:
        # 配置缺失 ⇒ 空串（非生产占位符）。真实签名前必经 _request() 的 fail-closed 守卫，
        # 故空串永不落到 _sign()（缺失即报 '云端凭据未配置'）。
        return str(self._cloud_cfg().get('app_secret') or DEFAULT_APP_SECRET)

    # ------------------------------------------------------------------
    # 签名（canonical 无末尾换行 —— 见模块头注释，实测契约）
    # ------------------------------------------------------------------
    def _sign(self, method: str, path_with_query: str, ts: str, nonce: str,
              body: str) -> str:
        canonical = '\n'.join([method, path_with_query, ts, nonce, body])
        digest = hmac.new(self.app_secret().encode('utf-8'),
                          canonical.encode('utf-8'), hashlib.sha256)
        return digest.hexdigest()  # 64 位小写 hex

    # ------------------------------------------------------------------
    # 传输
    # ------------------------------------------------------------------
    def _request(self, method: str, path: str, body_json: dict | None = None,
                 bearer: str | None = None) -> tuple[int, dict]:
        """签名 + 发送 + 归一。2xx ⇒ (status, payload)；非 2xx/网络失败 ⇒ raise。"""
        cloud = self._cloud_cfg()
        if not str(cloud.get('app_secret') or '').strip():
            # fail-closed：app_secret 缺失/空 ⇒ 明确错误（对齐 TtboxLicenseClient §3.3 语义）
            raise CloudLicenseError(0, 'not_configured', '云端凭据未配置')

        # body 先序列化后签名（禁止二次 dumps）：签名与发送必须是同一字节串
        body_str = ''
        if body_json is not None:
            body_str = json.dumps(body_json, ensure_ascii=False)

        ts = str(int(time.time()))
        nonce = secrets.token_hex(8)
        path_with_query = path
        headers = {
            'X-App-Key': self.app_key(),
            'X-Timestamp': ts,
            'X-Nonce': nonce,
            'X-Signature': self._sign(method.upper(), path_with_query, ts,
                                      nonce, body_str),
            'Accept': 'application/json',
        }
        if body_json is not None:
            headers['Content-Type'] = 'application/json'
        if bearer:
            headers['Authorization'] = f'Bearer {bearer}'

        url = self.base_url() + path
        data = body_str.encode('utf-8') if body_json is not None else None
        req = urllib.request.Request(url, data=data, headers=headers,
                                     method=method.upper())
        try:
            with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT_S) as resp:
                status = int(resp.status)
                raw = resp.read()
        except urllib.error.HTTPError as e:
            raw = b''
            try:
                raw = e.read()
            except Exception:
                pass
            payload = _safe_json(raw)
            raise CloudLicenseError(int(e.code), _classify_error(int(e.code), payload),
                                    str(payload.get('error') or payload.get('message')
                                        or f'HTTP {e.code}')) from None
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise CloudLicenseError(0, 'network', f'云端连接失败：{e}') from None

        payload = _safe_json(raw)
        if status < 200 or status >= 300:
            raise CloudLicenseError(status, _classify_error(status, payload),
                                    str(payload.get('error') or payload.get('message')
                                        or f'HTTP {status}'))
        return status, payload

    # ------------------------------------------------------------------
    # 业务端点
    # ------------------------------------------------------------------
    def card_login(self, card_key: str, machine_code: str) -> dict:
        """POST /api/client/card-login（camelCase 字段，实测契约）。

        成功 ⇒ {'client_token', 'expire_at'(北京串), 'expire_unix_s', 'max_devices',
                'heartbeat_interval', 'heartbeat_timeout', 'mode'}
        失败 ⇒ raise CloudLicenseError（400 卡密无效 / 403 机器码不符或已到期 / 网络 0）。
        """
        body = {
            'appKey': self.app_key(),
            'cardKey': str(card_key or '').strip(),
            'machineCode': str(machine_code or '').strip(),
        }
        _, payload = self._request('POST', PATH_CARD_LOGIN, body_json=body)
        token = str(payload.get('client_token') or payload.get('clientToken') or '')
        if not token:
            raise CloudLicenseError(0, 'http_error', '云端响应缺少 client_token')
        expire_at = str(payload.get('expire_at') or payload.get('expireAt') or '')
        from .cloud_session import parse_expire_at  # 局部导入避免环（§7.2 唯一换算点）
        return {
            'mode': str(payload.get('mode') or 'card'),
            'client_token': token,
            'expire_at': expire_at,
            'expire_unix_s': parse_expire_at(expire_at),
            'max_devices': _as_int(payload.get('max_devices',
                                               payload.get('maxDevices')), 1),
            'heartbeat_interval': _as_int(payload.get('heartbeat_interval',
                                                      payload.get('heartbeatInterval')), 60),
            'heartbeat_timeout': _as_int(payload.get('heartbeat_timeout',
                                                     payload.get('heartbeatTimeout')), 180),
        }

    def heartbeat(self, client_token: str, machine_code: str,
                  client_version: str) -> dict:
        """POST /api/client/heartbeat（Bearer token；body 同样 camelCase）。

        成功 ⇒ {'server_time', 'heartbeat_interval', 'heartbeat_timeout'}
        401（token 过期）/403（到期/禁用）⇒ raise（worker 据此重登/降级）。
        """
        body = {
            'machineCode': str(machine_code or ''),
            'clientVersion': str(client_version or ''),
        }
        _, payload = self._request('POST', PATH_HEARTBEAT, body_json=body,
                                   bearer=str(client_token or ''))
        return {
            'server_time': _as_int(payload.get('server_time',
                                               payload.get('serverTime')), 0),
            'heartbeat_interval': _as_int(payload.get('heartbeat_interval',
                                                      payload.get('heartbeatInterval')), 60),
            'heartbeat_timeout': _as_int(payload.get('heartbeat_timeout',
                                                     payload.get('heartbeatTimeout')), 180),
        }

    def app_info(self) -> dict:
        """GET /api/client/app-info?appKey=ttbox（P2 公告/强更；签名头同族）。"""
        _, payload = self._request('GET', f'{PATH_APP_INFO}?appKey={self.app_key()}')
        return payload


def _safe_json(raw: bytes) -> dict:
    try:
        data = json.loads(raw.decode('utf-8'))
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _as_int(value, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default
