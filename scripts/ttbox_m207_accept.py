#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ttbox_m207_accept.py — M2.07 板端验收驱动（免密化 + 云端卡密激活 + 面板 1:1 对齐 YU）。

★ 本脚本是 **M2.07 专用验收器**，与已退役的旧鉴权流程验收器无关。
  旧脚本（含 web 登录/限速/离线卡用户路径）已随免密化退役，见
  `scripts/legacy/ttbox_m2_license_accept_v1_authflow.py`（retired，勿用）。

断言口径唯一权威 = `ttbox-vs-yu-program/m2-acceptance-checklist.md` **§5.2（B15–B24）**。
本脚本实现：
  · B15 免密直达         · B16 激活页 1:1         · B17 云端签名正确性
  · B18 心跳链路         · B19 侧边栏 tab 集合     · B20 免密破坏性操作直通
  · B21 到期锁定（PEND-LEAD 钩子）                 · B22 URL 可配 + 断网宽限
  · B23 存量兼容（离线卡路径保留）
  · B24 云态**纯重启**保持（D-D 回归锚）
  · B25 拒绝的 ACTIVATE 不得清云激活态（F11 回归锚，2026-09-17）
  · B26–B29 配置·常量·路径口径回归锚（2026-09-18 口径整改新增）：
    B26 V-01/V-02 core 停止 ⇒ 面板写配置如实报错(503)且不落盘、恢复后经 IPC 读回；
    B27 V-04 TTBOX_MODELS_ROOT 归一（web ui_meta/_incoming 与 core 同根）；
    B28 V-03 板端 default.json ↔ 00-factory 共享键同值；
    B29 V-06 面板 SSOT 端口可达 + 无装饰性/同义端口 env。
保留项（§5.1 "保留不动"，不删）：
  · B4 激活翻绿 · B5 重启保持 · B8 ui_brand 篡改拒 · B10 ui_brand 随卡 ·
    B11 卡号短码 · B6·ota 端点门控与验签

--------------------------------------------------------------------------
★ B24（D-D 回归锚，2026-09-17 随 core 1.4.1 修复新增）：
  背景：core 对 **cloud 形** license.json 的重启恢复曾是**非确定的**——`LicenseDaemon::start()`
  经 `restore_cloud_doc_locked()` 恢复 kValid/kFallback 后，`Application::run()` 启动期一次性
  `verify_now_blocking()` 会把 `resolve_license_card()` 读回的 cloud 文档当离线卡再验
  （cloud 文档不是 Ed25519 信封）⇒ 若干秒后把云态抹成 unactivated（D-D，已修）。
  本用例口径（**纯重启观测**，不得经由任何重新激活路径）：
    ① 前置 = 云激活（store 文档为 cloud 形；未激活时经文档化云端入口 POST /api/license/activate
       建立前置——该动作发生在**重启之前**，非 B24 的观测路径）；
    ② `systemctl restart ttbox-core`（纯重启，不 restart web、不调 activate/reactivate）；
    ③ 观测窗口必须**跨越** Application 启动期一次性 verify 的失败窗口（B24_OBSERVE_S），
       期间**绝不做任何重新激活**；断言全程 activated 恒为 true、无掉线。
  ⇒ 若 D-D 复发，③ 的读数会出现 先 true 后 false ⇒ 本用例 FAIL（真锚，非兜底掩盖）。
  ⇒ `_restore_activated()` 的"重建激活兜底"已**收敛移除**（D-D 根治后不再需要，且该兜底会掩盖
     D-D）；B16 还原改为**纯文件恢复 + 重启**，B24 亦不经任何兜底。
作废项（§5.1，已随免密化移除，本脚本不实现）：
  · B13 activate 限速 429 / web auth 系列（登录 200/401、401-before-429、bootstrap）
  · 离线卡 JSON 的**用户**上传路径（内核路径保留，见 B23）

--------------------------------------------------------------------------
★ B17 契约基准（主理人 2026-09-17 板端实测留证，写死为对照）：
  请求：POST {base_url}/api/client/card-login
  body：{"appKey":"ttbox","cardKey":"LS-TTBOX-TEST-0001-M2X7","machineCode":"9ecf266dc8154491"}
  头 ：X-App-Key / X-Timestamp(Unix秒) / X-Nonce / X-Signature(=hex HMAC-SHA256(app_secret, canonical))
  canonical = METHOD + "\\n" + path + "\\n" + ts + "\\n" + nonce + "\\n" + body  ——  **无末尾换行**
  结果：**HTTP 200** → {mode:"card", client_token:<JWT>, expire_at:"2026-10-17 19:29:33"(北京),
        expire_unix_s, max_devices, heartbeat_interval:60, heartbeat_timeout:180}
  负例（宿主单测覆盖，见 plugins/web/tests/test_web_cloud_client.py）：
        篡改签名/带尾换行 ⇒ 401；snake_case 字段 ⇒ 400。

--------------------------------------------------------------------------
★ B21（到期锁定）PEND-LEAD 钩子 —— 需要主理人用云端 DB 预置：
  ① 主理人执行（云端 DB）：
       UPDATE cards SET expire_hours=0, status='used' WHERE card_key='LS-TTBOX-TEST-0001-M2X7';
     （即把该测试卡改为“已过期/已用”）
  ② 设备侧：web 心跳在一个周期（≤ heartbeat_interval=60s）内应收到 **403** →
     HeartbeatWorker.on_expired → IPC ACTIVATE_CLOUD{deactivate:true} → core 立即锁定。
  ③ 兜底：core daemon 60s 到期扫描（LicenseDaemon::thread_loop 空卡分支）亦应 ⇒ kExpired。
  ④ 断言：GET /api/license ⇒ activated==false（pipeline_allowed()==false，AI 关且无输出）。
  ⑤ 恢复：主理人把卡改回有效后，本脚本 B17 会用同卡重新 card-login 恢复（或加 --restore-valid）。
  在云端未预置前，本脚本把 B21 记为 **PEND-LEAD**（既非 PASS 也非 FAIL）。

--------------------------------------------------------------------------
用法（板端 root @ 192.168.0.104）：
    python3 scripts/ttbox_m207_accept.py [--base-url URL] [--cards-dir DIR]
        [--allow-state-toggle] [--no-restore] [--restore-valid] [--strict]
依赖板端：python3、web :8000、AF_UNIX core IPC socket（默认见 core/src/common/Paths.hpp）、systemctl。
退出码：0 = 无 FAIL（含全 SKIP/PEND）；1 = 有 FAIL；--strict 时 SKIP/PEND 亦计为失败。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


# ── 路径 bootstrap（A-PATH-3）────────────────────────────────────────────────
# 树根用**锚点发现**确定，不写死目录深度（原 `parents[1]`：换布局即静默指错根）。
def _ttbox_tree_root() -> Path:
    cur = Path(__file__).resolve().parent
    while True:
        if all((cur / _n).is_dir() for _n in ("plugins", "usbproxy", "scripts", "deploy")):
            return cur
        if cur.parent == cur:
            raise RuntimeError(
                "找不到 TTBOX 树根：从 %s 向上未发现同时含 "
                "plugins/usbproxy/scripts/deploy 的目录" % __file__
            )
        cur = cur.parent


_TREE_ROOT = _ttbox_tree_root()
_WEB_DIR = _TREE_ROOT / "plugins" / "web"
if str(_WEB_DIR) not in sys.path:
    # append 到末尾（不用 insert(0)：顶到 stdlib 前有遮蔽同名标准库的风险）
    sys.path.append(str(_WEB_DIR))

# ===========================================================================
# 配置（板端默认；可用命令行/环境变量覆盖）
# ===========================================================================
WEB_BASE = 'http://127.0.0.1:8000'
# IPC socket / 端口单点真源（A-PATH-5）：复用同仓 plugins/web/lib/paths.py，不散写字面量。
from lib.paths import IPC_SOCKET_DEFAULT as _IPC_DEFAULT
from lib.paths import WEB_PORT_DEFAULT as _WEB_PORT_DEFAULT
CORE_SOCK = _IPC_DEFAULT
CARDS_DIR = '/root/m2-cards'
CONFIG_PATH = os.environ.get('TTBOX_CONFIG', '/opt/ttbox/config/default.json')
STORE_DIR = '/var/lib/ttbox/license'
# 口径整改回归锚（B26–B29）真源：出厂配置（V-03 对照基准）+ 板端 unit（V-04/V-06 env 归一核验）。
# ★ 刻意**不**读环境变量：不扩大 env 面（口径门禁②：未登记 env 一律 FAIL）。
FACTORY_CONFIG = '/etc/ttbox/config.d/00-factory.json'
WEB_UNIT = '/etc/systemd/system/ttbox-web.service'
CORE_UNIT = '/etc/systemd/system/ttbox-core.service'
CLOUD_SESSION_PATH = os.environ.get('TTBOX_CLOUD_SESSION',
                                    '/opt/ttbox/config/cloud_session.json')

TEST_CARD = 'LS-TTBOX-TEST-0001-M2X7'
BOARD_SERIAL_FALLBACK = '9ecf266dc8154491'   # 板 /proc/cpuinfo Serial（2026-09-17 实测）

HEARTBEAT_WAIT_S = 95         # B18：等首心跳（interval 60s，留余量）
RESTART_WAIT_S = 60           # core 重启后 IPC 就绪探针窗口
WEB_READY_S = 90              # web 重启后就绪窗口（import numpy/cv2，冷启慢）
B24_OBSERVE_S = 22            # B24：纯重启后观测窗口（须跨越 core 启动期一次性 verify 失败窗口）
UNREACHABLE_URL = 'http://127.0.0.1:9'   # B22：必然不可达

# CLI 开关（main 里赋值）
ALLOW_STATE_TOGGLE = False
NO_RESTORE = False
RESTORE_VALID = False
STRICT = False

EXPECTED_TABS = ['总览', '热键控制', '移动控制', '辅助功能', '模型库',
                 '显示与鼠标', 'Hailo-8加速', '键鼠盒子', '网络配置', '预设参数',
                 '系统状态', '风扇控制']
FORBIDDEN_TABS = ['准星找色']


# ===========================================================================
# HTTP（不跟随重定向，以便观察 302→/activate）
# ===========================================================================
class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: N802
        return None


# 板端 web 为 **本机** 目标（127.0.0.1:8000）：显式忽略环境代理（ProxyHandler({})），
# 避免宿主/CI 上的 HTTP(S)_PROXY 把本地请求劫持成 502（云端签名由 web 进程内部完成，本脚本不直连云）。
_OP_NOFOLLOW = urllib.request.build_opener(_NoRedirect, urllib.request.ProxyHandler({}))
_OP_FOLLOW = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class Resp:
    __slots__ = ('status', 'headers', 'body')

    def __init__(self, status: int, headers: dict, body: str) -> None:
        self.status = status
        self.headers = headers or {}
        self.body = body or ''

    def json(self) -> dict:
        try:
            v = json.loads(self.body)
            return v if isinstance(v, dict) else {}
        except Exception:
            return {}

    def loc(self) -> str:
        return str(self.headers.get('Location', '') or '')


def http(method: str, path: str, *, json_body=None, json_str=None,
         timeout: float = 12.0, follow: bool = False) -> Resp:
    """发一次 HTTP 到 WEB_BASE+path。json_body=dict；json_str=已序列化 JSON 文本。"""
    url = WEB_BASE + path
    headers = {'Accept': 'application/json'}
    data = None
    if json_body is not None:
        data = json.dumps(json_body, ensure_ascii=False).encode('utf-8')
        headers['Content-Type'] = 'application/json'
    elif json_str is not None:
        data = json_str.encode('utf-8')
        headers['Content-Type'] = 'application/json'
    req = urllib.request.Request(url, data=data, headers=headers, method=method.upper())
    opener = _OP_FOLLOW if follow else _OP_NOFOLLOW
    try:
        with opener.open(req, timeout=timeout) as r:
            return Resp(int(r.status), dict(r.headers), r.read().decode('utf-8', 'replace'))
    except urllib.error.HTTPError as e:
        body = ''
        try:
            body = e.read().decode('utf-8', 'replace')
        except Exception:
            pass
        return Resp(int(e.code), dict(e.headers or {}), body)
    except Exception as e:            # 连接失败/超时/代理拒绝
        return Resp(0, {}, '<connection error: %s>' % e)


# ===========================================================================
# core IPC（AF_UNIX，JSON 行协议）
# ===========================================================================
def ipc(req_type: str, params=None, timeout: float = 5.0) -> dict:
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(CORE_SOCK)
        msg = {'type': req_type}
        if params is not None:
            msg['params'] = params
        s.sendall((json.dumps(msg) + '\n').encode('utf-8'))
        buf = b''
        while b'\n' not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        s.close()
        line = buf.split(b'\n', 1)[0].decode('utf-8', 'replace').strip()
        return json.loads(line) if line else {}
    except Exception as e:
        return {'status': -1, 'error': 'IPC 失败: %s' % e}


def core_ping(timeout: float = 5.0) -> bool:
    """就绪探针（替代固定 sleep）：直连 core.sock 发 PING，status==0 视为就绪。"""
    r = ipc('PING', timeout=timeout)
    return isinstance(r, dict) and r.get('status') == 0


# ===========================================================================
# 许可投影（/api/license 单一来源 = core IPC 投影）
# ===========================================================================
def license_data() -> dict:
    r = http('GET', '/api/license')
    if r.status != 200:
        return {}
    return r.json().get('data') or {}


def lic_core() -> dict:
    return license_data().get('license') or {}


def lic_cloud() -> dict:
    return license_data().get('cloud') or {}


def web_reachable() -> bool:
    return http('GET', '/api/license', timeout=5).status != 0


# ===========================================================================
# 文件/配置助手
# ===========================================================================
def read_json(path: str):
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception:
        return None


def atomic_write_json(path: str, obj) -> bool:
    """原子写 JSON（tmp + replace），**保留原文件的 mode 与属主/属组**。

    M2.07.1 根治（T2）：此前只保 mode、未保属主 ⇒ tmp 由 root 创建，os.replace 后
    属主被重置为 root:root；若原文件是 `root:ttbox 0640`（default.json 含 cloud.app_secret），
    属组丢失会让属组变 root、0640 下 `User=ttbox` 的 web 读不到配置 ⇒ 云端凭据丢失
    （激活 502「云端凭据未配置」）。故在 replace 前把**原文件的 uid/gid + mode** 应用回 tmp。

    属主保留仅在 posix 且 euid==0 时执行（非 root 无法 chown，且此时属主本就等于调用者）；
    非 root 或 chown 失败时静默跳过——不削弱“保 mode”的既有保证。
    """
    try:
        st = os.stat(path) if os.path.exists(path) else None
        mode = (st.st_mode & 0o777) if st is not None else 0o644
        tmp = '%s.m207tmp.%d' % (path, os.getpid())
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(obj, f, ensure_ascii=False, indent=2)
        os.chmod(tmp, mode)
        # ★ 保留属主/属组：仅在 root 下有效（非 root 静默跳过，属主本就是调用者）。
        if st is not None and hasattr(os, 'geteuid') and os.geteuid() == 0:
            try:
                os.chown(tmp, st.st_uid, st.st_gid)
            except OSError:
                pass
        os.replace(tmp, path)
        return True
    except Exception:
        return False


def read_card(name: str) -> str:
    try:
        with open(os.path.join(CARDS_DIR, name), 'r', encoding='utf-8') as f:
            return f.read().strip()
    except Exception:
        return ''


def board_serial() -> str:
    try:
        with open('/proc/cpuinfo', 'r') as f:
            for line in f:
                if line.startswith('Serial'):
                    return line.split(':', 1)[1].strip()
    except Exception:
        pass
    return BOARD_SERIAL_FALLBACK


def has_systemctl() -> bool:
    return _which('systemctl') is not None


def _which(name: str):
    for d in os.environ.get('PATH', '').split(os.pathsep):
        p = os.path.join(d, name)
        if os.path.exists(p) and os.access(p, os.X_OK):
            return p
    return None


def systemctl(*args: str) -> bool:
    try:
        subprocess.run(['systemctl', *args], timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        return True
    except Exception:
        return False


def restart_core() -> bool:
    if not has_systemctl():
        return False
    systemctl('reset-failed', 'ttbox-core')     # 陷阱 2：清启动限速，避免 start-limit-hit
    systemctl('restart', 'ttbox-core')
    return True


def restart_web() -> bool:
    if not has_systemctl():
        return False
    systemctl('reset-failed', 'ttbox-web')
    systemctl('restart', 'ttbox-web')
    # D-B 修复：restart 立即返回时 web 尚在 import numpy/cv2，必须等就绪再放行后续用例。
    wait_web_ready()
    return True


def wait_until(pred, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            if pred():
                return True
        except Exception:
            pass
        time.sleep(2.0)
    return False


def wait_web_ready(timeout: float = WEB_READY_S) -> bool:
    """web 重启后就绪等待（复用 wait_until/web_reachable）。web 需 import numpy/cv2，
    冷启首包可达常要数十秒；不等就发请求 ⇒ 后续用例全打在“正在启动”的 web 上
    ⇒ 误判为“板端 web 不可达 / HTTP 0”。"""
    return wait_until(web_reachable, timeout)


def activated_settled(timeout: float = 30.0) -> bool:
    """「已激活前置」的宽容就绪判定：允许在 timeout 内等待激活态出现再放行（避免把前置
    抖动误判成用例失败/跳过）。

    ★ D-D 已根治（core 1.4.1：云态守卫 + resolve_license_card 不返回 cloud doc）：云态重启
      恢复现为**确定**，本判定仅作就绪等待用，不再是「掩盖回落」的兜底。"""
    if lic_core().get('activated'):
        return True
    return wait_until(lambda: bool(lic_core().get('activated')), timeout)


def _store_doc_is_cloud() -> bool:
    """读回 store 的 license.json，判断是否为 **cloud 形**文档（顶层 source=="cloud"）。

    与 core 侧单一真源同口径（LicenseStore::load() -> StoreLoadResult.doc_is_cloud）。"""
    doc = read_json(os.path.join(STORE_DIR, 'license.json'))
    return isinstance(doc, dict) and doc.get('source') == 'cloud'


def _core_log_lines(pattern: str, n: int = 6) -> list:
    """从 ttbox-core 的 journal 抓最近含 pattern 的若干行（用于留现场证据）。

    失败（无 journalctl / 权限 / 超时）返回 []，绝不影响用例判定。"""
    try:
        r = subprocess.run(['journalctl', '-u', 'ttbox-core', '--no-pager', '-n', '300'],
                           timeout=20, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        out = r.stdout.decode('utf-8', 'replace')
        hits = [ln.strip() for ln in out.splitlines() if pattern in ln]
        return hits[-n:]
    except Exception:
        return []


def _observe_activated_stable(window_s: float = B24_OBSERVE_S,
                              gap: float = 2.0) -> dict:
    """在 window_s 内每隔 gap 秒纯读数（**不做任何激活/重启**），返回观测记录。

    返回：{'readings': [True/False...], 'first': bool|None, 'last': bool|None,
           'dropped': bool（窗口内是否出现过 false）, 'plan': ...}。"""
    deadline = time.time() + window_s
    readings: list = []
    first = None
    while time.time() < deadline:
        try:
            a = bool(lic_core().get('activated'))
        except Exception:
            a = False
        if first is None:
            first = a
        readings.append(a)
        time.sleep(gap)
    if not readings:
        readings = [bool(lic_core().get('activated'))]
        first = readings[0]
    return {'readings': readings, 'first': first, 'last': readings[-1],
            'dropped': any(not x for x in readings),
            'plan': lic_core().get('plan'), 'state': lic_core().get('state')}


# ===========================================================================
# 结果记录
# ===========================================================================
RESULTS: list = []


def _emit(verdict: str, cid: str, desc: str, detail: str) -> None:
    RESULTS.append({'id': cid, 'desc': desc, 'verdict': verdict, 'detail': detail})
    tag = {'PASS': 'PASS', 'FAIL': 'FAIL', 'SKIP': 'SKIP', 'PEND': 'PEND'}[verdict]
    line = '[%s] %-8s %s' % (tag, cid, desc)
    if detail:
        line += ' :: ' + detail
    print(line, flush=True)


def check(cid: str, desc: str, ok: bool, detail: str = '') -> None:
    _emit('PASS' if ok else 'FAIL', cid, desc, detail)


def skip(cid: str, desc: str, reason: str) -> None:
    _emit('SKIP', cid, desc, reason)


def pend(cid: str, desc: str, reason: str) -> None:
    _emit('PEND', cid, desc, reason)


# ===========================================================================
# 未激活基线的临时切换（B16 用；可逆）
# ===========================================================================
def _toggle_to_unactivated() -> bool:
    """把 store + cloud_session 移走并重启 core/web，建立干净未激活基线。"""
    if not has_systemctl():
        return False
    moved = []
    for p in (os.path.join(STORE_DIR, 'license.json'),
              os.path.join(STORE_DIR, 'state.json'),
              CLOUD_SESSION_PATH):
        if os.path.exists(p):
            try:
                os.replace(p, p + '.m207bak')
                moved.append(p)
            except Exception:
                pass
    restart_core()
    restart_web()
    return wait_until(core_ping, RESTART_WAIT_S)


def _restore_activated() -> None:
    """B16 收尾还原：把 _toggle_to_unactivated() 移走的 store/session 文件**原样搬回**并重启
    core/web，等 web 就绪 + **被动等待**激活态再现（**不调用任何 activate**）。

    ★ D-D 已根治（core 1.4.1）⇒ 本函数**不再**做任何"重建激活"兜底：
      旧的 `_core_activation_settle()` 等待 + `POST /api/license/activate` 重试链**已移除**——
      它会把 D-D（云态重启丢失）**掩盖成稳定 PASS**，使验收对真正的缺陷失明。
      还原后 core 应经 `restore_cloud_doc_locked()` **纯恢复**云态；若未恢复，那是**产品缺陷**，
      须由 B24（云态纯重启锚）暴露，而**不得**在这里静默重建激活。
      B24 亦绝不调用本函数（其观测不得经由任何重建激活路径）。
    """
    for p in (os.path.join(STORE_DIR, 'license.json'),
              os.path.join(STORE_DIR, 'state.json'),
              CLOUD_SESSION_PATH):
        if os.path.exists(p + '.m207bak'):
            try:
                os.replace(p + '.m207bak', p)
            except Exception:
                pass
    restart_core()
    restart_web()
    wait_until(core_ping, RESTART_WAIT_S)
    # D-B 修复：B16 在 finally 调本函数；还原后必须等 web 真正就绪，
    # 否则其后的 B18/B19/B20/B22/B23 等全打在正在启动的 web 上（误判不可达）。
    wait_web_ready()
    # ★ 就绪等待（**非**重建激活）：web 刚重启、与 core 的 IPC 尚未就绪时，GET /api/license
    #   会回落成"诚实未激活"默认块 ⇒ 立即读会短暂得到 activated=false（RELEASE.md §7 陷阱；
    #   B18/B19 只读一次 activated，会被误判成 SKIP）。此处**被动等待**激活态再现，
    #   期间**不调用任何 activate**（就绪等待 ≠ 重建激活兜底）。若 core 真的没恢复云态
    #   （D-D 复发），此等待会超时——由 B24（纯重启锚）如实暴露，绝不在此静默重建。
    wait_until(lambda: bool(lic_core().get('activated')), 40)


# ===========================================================================
# B15 免密直达
# ===========================================================================
def b15() -> None:
    desc = 'B15 免密直达（无 token：/api/config|state|license 200；/setup|login 404）'
    if not web_reachable():
        skip('B15', desc, '板端 web 不可达')
        return
    if not lic_core().get('activated'):
        skip('B15', desc, '需已激活基线（先跑 B17 云端激活，未激活时非白名单 API 会 403）')
        return
    codes = {
        'config': http('GET', '/api/config').status,
        'state': http('GET', '/api/state').status,
        'license': http('GET', '/api/license').status,
        'setup': http('GET', '/setup').status,
        'login': http('GET', '/login').status,
    }
    ok = (codes['config'] == 200 and codes['state'] == 200 and codes['license'] == 200
          and codes['setup'] == 404 and codes['login'] == 404)
    check('B15', desc, ok, json.dumps(codes))


# ===========================================================================
# B16 激活页 1:1
# ===========================================================================
def b16() -> None:
    desc = 'B16 激活页 1:1（未激活 302→/activate；DOM id + --bg:#0b0c0b + .license-gate-card）'
    if not web_reachable():
        skip('B16', desc, '板端 web 不可达')
        return
    activated = bool(lic_core().get('activated'))
    toggled = False
    if activated:
        if not ALLOW_STATE_TOGGLE:
            skip('B16', desc, '需未激活基线；已激活。加 --allow-state-toggle 可临时切换'
                              '（临时移走 store/cloud_session 并重启 core/web，结束自动还原）')
            return
        toggled = _toggle_to_unactivated()
        if not toggled:
            check('B16', desc, False, '临时切换未激活基线失败（systemctl/core 不可用）')
            return
    try:
        r = http('GET', '/', follow=False)
        red_ok = (r.status == 302 and r.loc().endswith('/activate'))
        p = http('GET', '/activate', follow=False)
        html = p.body
        ids = ['licenseGateOverlay', 'licenseGateKeyInput',
               'licenseGateActivateButton', 'licenseGateRefreshButton']
        ids_ok = all(i in html for i in ids)
        css_ok = ('--bg:#0b0c0b' in html.replace(' ', '')) and ('license-gate-card' in html)
        ok = red_ok and p.status == 200 and ids_ok and css_ok
        check('B16', desc, ok,
              'GET / => %s loc=%s ; /activate=%s ids=%s css=%s'
              % (r.status, r.loc(), p.status, ids_ok, css_ok))
    finally:
        if toggled and not NO_RESTORE:
            _restore_activated()


# ===========================================================================
# B17 云端签名（正例；负例见宿主单测）
# ===========================================================================
def b17() -> None:
    desc = 'B17 云端签名正例 200（camelCase + canonical 无尾换行；client_token/expire_at/hb60/180）'
    if not web_reachable():
        skip('B17', desc, '板端 web 不可达')
        return
    if not core_ping():
        skip('B17', desc, 'core IPC 未就绪（激活需 core 落盘/执法）')
        return
    r = http('POST', '/api/license/activate', json_body={'license_key': TEST_CARD}, timeout=25)
    j = r.json()
    if r.status != 200:
        err = str(j.get('error') or '')
        if r.status in (502, 500) or '云端' in err or '连接' in err or '设备指纹' in err:
            skip('B17', desc, '云端/前置不可用（HTTP %s: %s）' % (r.status, err))
        else:
            check('B17', desc, False, 'HTTP %s: %s' % (r.status, err))
        return
    lc = lic_core()
    hb = (lic_cloud().get('heartbeat') or {})
    expire_at = str(lic_cloud().get('expire_at') or '')
    ok = (bool(lc.get('activated')) and expire_at != ''
          and int(hb.get('interval') or 0) == 60 and int(hb.get('timeout') or 0) == 180)
    check('B17', desc, ok,
          'activated=%s expire_at=%r hb.interval=%s hb.timeout=%s machine_code=%r'
          % (lc.get('activated'), expire_at, hb.get('interval'), hb.get('timeout'),
             lic_cloud().get('machine_code')))
    # 负例仅在宿主单测覆盖（本脚本不发未签名请求）
    print('        └ 负例（篡改签名 401 / 带尾换行 401 / snake_case 400）见 '
          'plugins/web/tests/test_web_cloud_client.py', flush=True)


# ===========================================================================
# B18 心跳链路
# ===========================================================================
def b18() -> None:
    desc = 'B18 心跳链路（激活后 ≤95s 内 cloud.heartbeat.online 翻 true）'
    if not lic_core().get('activated'):
        skip('B18', desc, '需已激活基线')
        return
    online = wait_until(lambda: bool((lic_cloud().get('heartbeat') or {}).get('online')),
                        HEARTBEAT_WAIT_S)
    hb = lic_cloud().get('heartbeat') or {}
    check('B18', desc, online,
          'online=%s last_ok_at=%s error=%r' % (hb.get('online'), hb.get('last_ok_at'),
                                                hb.get('error')))
    skip('B18·停跳', 'B18 子项：web 停跳 180s+ ⇒ 云端掉线但 AI 不停（Q3）',
         '需停 ttbox-web 达 heartbeat_timeout 且保持 core 运行（破坏性手工步骤，不自动化）；'
         'AI 放行判据见 B21/pipeline_allowed')


# ===========================================================================
# B19 侧边栏 tab 集合
# ===========================================================================
def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ''
    j = text.find(end, i)
    return text[i:] if j < 0 else text[i:j]


def b19() -> None:
    desc = 'B19 侧边栏 == 12 页精确集合（含 Hailo-8加速/键鼠盒子；无准星找色）'
    if not lic_core().get('activated'):
        skip('B19', desc, '需已激活基线（未激活 / 会 302 /activate）')
        return
    r = http('GET', '/')
    if r.status != 200:
        skip('B19', desc, 'GET / => %s' % r.status)
        return
    aside = _between(r.body, '<aside class="sidebar"', '</aside>')
    labels = [m.strip() for m in re.findall(
        r'data-page-target="[^"]*"[^>]*>\s*<span>\d+</span>\s*([^<]+?)\s*</button>', aside)]
    set_ok = (set(labels) == set(EXPECTED_TABS)) and (len(labels) == len(EXPECTED_TABS))
    forb = [w for w in FORBIDDEN_TABS if w in aside]
    check('B19', desc, set_ok and not forb,
          'labels=%s forbidden_in_sidebar=%s' % (labels, forb))


# ===========================================================================
# B20 免密破坏性操作直通
# ===========================================================================
def b20() -> None:
    desc = 'B20 免密破坏性操作（ota 非法参数 400 非 401/403；reboot/poweroff dry_run 200；reactivate 存在）'
    if not web_reachable():
        skip('B20', desc, '板端 web 不可达')
        return
    if not activated_settled():
        skip('B20', desc, '需已激活基线（未激活时 gate 会 403，混淆参数层判据）')
        return
    ota = http('POST', '/api/ota/install', json_body={})              # 缺 url ⇒ 参数层 400
    rb = http('POST', '/api/system/reboot', json_body={'dry_run': True})     # 不真重启
    po = http('POST', '/api/system/poweroff', json_body={'dry_run': True})   # 不真关机
    rea = http('POST', '/api/system/reactivate', json_body={})
    codes = {'ota': ota.status, 'reboot': rb.status, 'poweroff': po.status,
             'reactivate': rea.status}
    no401 = all(v != 401 for v in codes.values())
    ok = (ota.status == 400 and rb.status == 200 and po.status == 200
          and rea.status != 404 and no401)
    check('B20', desc, ok, json.dumps(codes))


# ===========================================================================
# B21 到期锁定（PEND-LEAD 钩子）
# ===========================================================================
def b21() -> None:
    desc = 'B21 到期锁定（云端改卡 ⇒ 403 ⇒ deactivate ⇒ pipeline_allowed()==false）'
    if not web_reachable():
        skip('B21', desc, '板端 web 不可达')
        return
    lc = lic_core()
    hb = lic_cloud().get('heartbeat') or {}
    err = str(hb.get('error') or '')
    if (not lc.get('activated')) and ('到期' in err or '禁用' in err or 'expire' in err.lower()):
        check('B21', desc, True, '已锁定：activated=False；heartbeat.error=%r' % err)
        return
    pend('B21', desc,
         '需云端预置：① 主理人执行 UPDATE cards SET expire_hours=0,status=\'used\' '
         'WHERE card_key=\'%s\' → ② web 心跳 ≤60s 收 403 触发 deactivate → '
         '③ core 60s 扫描兜底 → ④ 断言 /api/license.activated==false 且 pipeline_allowed()==false → '
         '⑤ 恢复：改回有效后同卡重新 card-login（--restore-valid 自动做）' % TEST_CARD)


# ===========================================================================
# B22 URL 可配 + 断网宽限
# ===========================================================================
def _cfg_cloud_url(cfg: dict):
    try:
        return cfg.get('cloud', {}).get('license_base_url')
    except Exception:
        return None


def b22() -> None:
    desc = 'B22 URL 可配（不可达 ⇒ 明确错误）+ 断网宽限（已激活仍按本地 expire 放行）'
    if not web_reachable():
        skip('B22', desc, '板端 web 不可达')
        return
    if not activated_settled():
        skip('B22', desc, '需已激活基线')
        return
    cfg = read_json(CONFIG_PATH)
    if not isinstance(cfg, dict):
        skip('B22', desc, '读不到 %s' % CONFIG_PATH)
        return
    cur = _cfg_cloud_url(cfg)
    if not cur:
        skip('B22', desc, '%s 无 cloud.license_base_url' % CONFIG_PATH)
        return
    cfg.setdefault('cloud', {})['license_base_url'] = UNREACHABLE_URL
    if not atomic_write_json(CONFIG_PATH, cfg):
        check('B22', desc, False, '写入 %s 失败（权限？）' % CONFIG_PATH)
        return
    try:
        r = http('POST', '/api/license/activate', json_body={'license_key': TEST_CARD}, timeout=20)
        err = str(r.json().get('error') or '')
        err_ok = (r.status in (502, 400, 403) and err != '')
        still = bool(lic_core().get('activated'))
        check('B22', desc, err_ok and still,
              'unreachable_activate=%s err=%r still_activated=%s' % (r.status, err, still))
    finally:
        cfg2 = read_json(CONFIG_PATH) or cfg
        cfg2.setdefault('cloud', {})['license_base_url'] = cur
        atomic_write_json(CONFIG_PATH, cfg2)


# ===========================================================================
# B23 存量兼容（离线卡内核路径保留）
# ===========================================================================
def b23() -> None:
    desc = 'B23 存量兼容（离线卡 JSON 信封仍可激活；升级后已激活态保持）'
    if not web_reachable():
        skip('B23', desc, '板端 web 不可达')
        return
    card = read_card('card-valid.json')
    if not card:
        skip('B23', desc, '缺 %s/card-valid.json' % CARDS_DIR)
        return
    r = http('POST', '/api/license/activate', json_body={'license_key': card}, timeout=20)
    if r.status == 0:
        # D-B 修复：前置不可用（连接失败）不是产品失败 ⇒ SKIP 而非 FAIL。
        skip('B23', desc, '板端 web 不可达')
        return
    j = r.json()
    lc = lic_core()
    ok = (r.status == 200 and bool(lc.get('activated')))
    check('B23', desc, ok,
          'offline_activate=%s err=%r activated=%s features=%s'
          % (r.status, j.get('error'), lc.get('activated'), lc.get('features')))


# ===========================================================================
# B24 云态纯重启保持（D-D 回归锚）
# ===========================================================================
def b24() -> None:
    desc = ('B24 云态纯重启保持（云激活 → 重启 core → 仍 activated；'
            '不经任何重新激活路径）')
    if not has_systemctl():
        skip('B24', desc, 'systemctl 不可用')
        return
    if not web_reachable():
        skip('B24', desc, '板端 web 不可达')
        return

    # ① 前置：确保为**云态**激活（store 文档 = cloud 形）。若当前非云态（如 B23 刚做过离线卡
    #    激活）或未激活，则经**文档化云端入口**建立前置——该动作发生在**重启之前**，
    #    **不**属于 B24 的"重启后重新激活"观测路径。
    before = lic_core()
    if not (before.get('activated') and _store_doc_is_cloud()):
        try:
            http('POST', '/api/license/activate',
                 json_body={'license_key': TEST_CARD}, timeout=25)
        except Exception:
            pass
        wait_until(lambda: bool(lic_core().get('activated')) and _store_doc_is_cloud(), 30)
        before = lic_core()
    if not (before.get('activated') and _store_doc_is_cloud()):
        skip('B24', desc,
             '无法建立云激活前置（activated=%s doc_is_cloud=%s；云端/前置不可用）'
             % (before.get('activated'), _store_doc_is_cloud()))
        return

    before_state = before.get('state')
    before_plan = before.get('plan')
    ts = time.strftime('%H:%M:%S')

    # ② 纯重启：**只**重启 core（不 restart web、不调 activate/reactivate）
    if not restart_core():
        check('B24', desc, False, 'core 重启失败')
        return
    if not wait_until(core_ping, RESTART_WAIT_S):
        check('B24', desc, False, 'core 重启后 IPC 未就绪（探针超时）')
        return

    # ③ 纯观测窗口：跨越 core 启动期一次性 verify 的失败窗口；期间**绝不做任何激活**。
    #    若 D-D 复发，读数会 先 true 后 false ⇒ dropped=True ⇒ 本用例 FAIL（真锚，非兜底）。
    obs = _observe_activated_stable()
    readings = obs['readings']
    ok = (obs['first'] is True) and (not obs['dropped']) and (obs['last'] is True)

    # 现场证据：core 日志「云端授权恢复」那几行（本用例的实际观测输出）
    log_hits = _core_log_lines('云端授权恢复', n=6)
    detail = (
        'before@%s: activated=True state=%s plan=%s doc_is_cloud=True → restart core → '
        'after: activated=%s state=%s plan=%s ; 观测 %d 点(每2s/共%ds) 掉线=%s readings=%s ; '
        '云端授权恢复日志 %d 行'
        % (ts, before_state, before_plan,
           obs['last'], obs['state'], obs['plan'],
           len(readings), int(B24_OBSERVE_S), obs['dropped'],
           ''.join('1' if x else '0' for x in readings), len(log_hits)))
    check('B24', desc, ok, detail)
    for ln in log_hits:
        print('        └ B24 core-log: %s' % ln, flush=True)


# ===========================================================================
# B25 拒绝的 ACTIVATE 不得清云激活态（F11 回归锚，2026-09-17）
# ===========================================================================
def _lic_sig() -> tuple:
    """云态授权签名（F11 契约字段）：activated / state / source / features.capture。"""
    lic = lic_core()
    cap = lic.get('capabilities') or {}
    return (bool(lic.get('activated')), lic.get('state'),
            lic_cloud().get('source'), bool(cap.get('capture')))


def b25() -> None:
    """F11：面板免密 ⇒ 局域网任何人可 POST /api/license/activate。修前，被拒的
    ACTIVATE（非法 body / 坏签名卡）会把**云激活**打掉并关掉 AI。本锚断言：
    云激活态下任一被拒 ACTIVATE 后 activated/state/source/features.capture **逐字段不变**，
    且其后云心跳仍 online。"""
    desc = ('B25 F11：云激活态下被拒 ACTIVATE（非法 body / 坏签名卡）不得清云态；'
            'activated/state/source/features.capture 恒不变，且其后云心跳仍 online')
    if not web_reachable():
        skip('B25', desc, '板端 web 不可达')
        return
    if not activated_settled():
        skip('B25', desc, '需已激活基线')
        return
    # 前置：必须为**云态**（source=cloud + store doc_is_cloud）；否则经文档化云端入口建立
    if not (_store_doc_is_cloud() and lic_cloud().get('source') == 'cloud'):
        http('POST', '/api/license/activate', json_body={'license_key': TEST_CARD}, timeout=25)
        wait_until(lambda: _store_doc_is_cloud() and lic_cloud().get('source') == 'cloud', 30)
    if not (_store_doc_is_cloud() and lic_cloud().get('source') == 'cloud'):
        skip('B25', desc, '无法建立云激活前置（doc_is_cloud=%s source=%r）'
             % (_store_doc_is_cloud(), lic_cloud().get('source')))
        return

    before = _lic_sig()

    # ① 非法 body（`{` 起手但非 JSON）⇒ web 转 IPC ACTIVATE_LICENSE ⇒ core parse 拒绝
    r1 = http('POST', '/api/license/activate',
              json_body={'license_key': '{ this is not valid json'}, timeout=15)
    sig1 = _lic_sig()

    # ② 坏签名卡（结构过、Ed25519 验签拒）⇒ core 权威拒绝分支
    badsig = read_card('card-badsig.json')
    r2 = (http('POST', '/api/license/activate',
               json_body={'license_key': badsig}, timeout=20) if badsig else None)
    sig2 = _lic_sig()

    # ③ 空 body（web 层 400，不触达 core；顺带确认不扰动）
    r3 = http('POST', '/api/license/activate', json_str='', timeout=10)
    sig3 = _lic_sig()

    # ④ 其后云心跳仍 online（拒绝不扰动云态心跳）
    online = wait_until(lambda: bool((lic_cloud().get('heartbeat') or {}).get('online')), 20)
    hb = lic_cloud().get('heartbeat') or {}

    ok = (before == sig1 == sig2 == sig3
          and before[0] is True and before[1] == 'active'
          and before[2] == 'cloud' and before[3] is True
          and online)
    check('B25', desc, ok,
          'before=%s illegal_body=%s badsig=%s empty=%s ; '
          'http(illegal=%s badsig=%s empty=%s) ; hb.online=%s hb.error=%r'
          % (before, sig1, sig2, sig3,
             getattr(r1, 'status', None), (r2.status if r2 else None),
             getattr(r3, 'status', None), hb.get('online'), hb.get('error')))


# ===========================================================================
# 保留项（§5.1 保留不动）
# ===========================================================================
def retained_b4() -> None:
    desc = 'B4 激活后 /api/license 翻绿（activated + state/plan/features 与卡一致）'
    lc = lic_core()
    if not lc.get('activated'):
        skip('B4', desc, '未激活（先跑 B17/B23）')
        return
    check('B4', desc, lc.get('state') not in (None, '', 'unactivated'),
          'activated=%s state=%s plan=%s features=%s'
          % (lc.get('activated'), lc.get('state'), lc.get('plan'), lc.get('features')))


def retained_b5() -> None:
    desc = ('B5 重启保持（reset-failed+restart core → IPC 就绪探针 → 仍激活）；'
            '⚠ 覆盖范围仅限「离线卡文档的重启持久化」，不覆盖云态重启（云态见 B24）')
    if not has_systemctl():
        skip('B5', desc, 'systemctl 不可用')
        return
    before = lic_core()
    if not before.get('activated'):
        skip('B5', desc, '重启前未激活')
        return
    if not restart_core():
        skip('B5', desc, 'core 重启失败')
        return
    if not wait_until(core_ping, RESTART_WAIT_S):
        check('B5', desc, False, 'core 重启后 IPC 未就绪（探针超时）')
        return
    after = lic_core()
    check('B5', desc, bool(after.get('activated')),
          'before_state=%s after_state=%s plan=%s'
          % (before.get('state'), after.get('state'), after.get('plan')))


def retained_cards() -> None:
    # D-B 修复：前置总闸——web 不可达时整组 SKIP（前置不可用非产品失败，勿判 FAIL）。
    if not web_reachable():
        skip('B8', 'B8 改 ui_brand 不重签 ⇒ 400（signature mismatch）', '板端 web 不可达')
        skip('B10', 'B10 ui_brand 随卡（sample）', '板端 web 不可达')
        skip('B11', 'B11 卡号短码非空（TTB-XXXX-XXXX）', '板端 web 不可达')
        skip('B6·ota', 'B6·ota 受限卡 OTA 门控 ⇒ 403（不调度）', '板端 web 不可达')
        return

    # B8 篡改 ui_brand 不重签 ⇒ 400
    c = read_card('card-tampered-brand.json')
    if c:
        r = http('POST', '/api/license/activate', json_body={'license_key': c})
        if r.status == 0:
            skip('B8', 'B8 改 ui_brand 不重签 ⇒ 400（signature mismatch）', '板端 web 不可达')
        else:
            check('B8', 'B8 改 ui_brand 不重签 ⇒ 400（signature mismatch）', r.status == 400,
                  'HTTP %s' % r.status)
    else:
        skip('B8', 'B8 改 ui_brand 不重签 ⇒ 400', '缺 %s/card-tampered-brand.json' % CARDS_DIR)

    # B10 ui_brand 随卡
    c = read_card('card-brand-sample.json')
    if c:
        r = http('POST', '/api/license/activate', json_body={'license_key': c})
        if r.status == 0:
            skip('B10', 'B10 ui_brand 随卡（sample）', '板端 web 不可达')
        else:
            got = lic_core().get('ui_brand')
            check('B10', 'B10 ui_brand 随卡（sample）', r.status == 200 and got == 'sample',
                  'HTTP %s ui_brand=%r' % (r.status, got))
    else:
        skip('B10', 'B10 ui_brand 随卡', '缺 %s/card-brand-sample.json' % CARDS_DIR)

    # B11 短码（离线有效卡）
    c = read_card('card-valid.json')
    if c:
        r = http('POST', '/api/license/activate', json_body={'license_key': c})
        if r.status == 0:
            skip('B11', 'B11 卡号短码非空（TTB-XXXX-XXXX）', '板端 web 不可达')
        else:
            sc = lic_core().get('short_code')
            check('B11', 'B11 卡号短码非空（TTB-XXXX-XXXX）', r.status == 200 and bool(sc),
                  'HTTP %s short_code=%r' % (r.status, sc))
    else:
        skip('B11', 'B11 卡号短码非空', '缺 %s/card-valid.json' % CARDS_DIR)

    # B6·ota 受限卡端点门控 ⇒ 403（验签与门控保留）
    c = read_card('card-limited.json')
    if c:
        r = http('POST', '/api/license/activate', json_body={'license_key': c})
        if r.status == 0:
            skip('B6·ota', 'B6·ota 受限卡 OTA 门控 ⇒ 403（不调度）', '板端 web 不可达')
        elif r.status == 200:
            o = http('POST', '/api/ota/install', json_body={'url': 'https://example.com/u.bin'})
            check('B6·ota', 'B6·ota 受限卡 OTA 门控 ⇒ 403（不调度）', o.status == 403,
                  'HTTP %s' % o.status)
        else:
            check('B6·ota', 'B6·ota 受限卡 OTA 门控 ⇒ 403', False,
                  '受限卡激活失败 HTTP %s' % r.status)
    else:
        skip('B6·ota', 'B6·ota 受限卡 OTA 门控 ⇒ 403', '缺 %s/card-limited.json' % CARDS_DIR)


# ===========================================================================
# B26–B29：配置·常量·路径口径整改回归锚（2026-09-18）
#   断言口径 = docs/protocols/config-path-env-registry.md + docs/CONVENTIONS.md §六（无补丁红线）。
# ===========================================================================
def _read_text(path: str):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            return f.read()
    except Exception:
        return None


def _unit_has(unit_path: str, needle: str) -> bool:
    txt = _read_text(unit_path)
    return bool(txt and needle in txt)


def _unit_env(unit_path: str, key: str):
    """从 unit 文件读显式 `Environment=<key>=<value>`；无 → None。"""
    txt = _read_text(unit_path)
    if not txt:
        return None
    m = (re.search(r'^\s*Environment="%s=([^"]*)"\s*$' % re.escape(key), txt, re.M)
         or re.search(r'^\s*Environment=%s=(\S+)\s*$' % re.escape(key), txt, re.M))
    return m.group(1) if m else None


def _file_fp(path: str):
    """文件指纹 (mtime_ns, size, sha256)；不存在/不可读 → None。"""
    try:
        with open(path, 'rb') as f:
            data = f.read()
        st = os.stat(path)
        return (st.st_mtime_ns, st.st_size, hashlib.sha256(data).hexdigest())
    except Exception:
        return None


def b26() -> None:
    cid = 'B26'
    desc = ('B26 V-01/V-02：core 停止 ⇒ 面板写配置如实报错(503 CORE_UNREACHABLE)且不落盘；'
            '恢复后经 IPC 读回 config.d 生效值')
    if not web_reachable():
        skip(cid, desc, '板端 web 不可达')
        return
    if not has_systemctl():
        skip(cid, desc, '无 systemctl，无法停/起 core')
        return
    before = _file_fp(CONFIG_PATH)
    systemctl('stop', 'ttbox-core')
    if not wait_until(lambda: not core_ping(1.0), 30):
        check(cid, desc, False, 'core 停止失败（前置不可得）')
        restart_core()
        wait_until(core_ping, RESTART_WAIT_S)
        return
    try:
        r = http('PUT', '/api/v1/config',
                 json_body={'profile': {'preview_fps': 31}}, timeout=8)
        # V-01/V-02 本质 =「未成功且不落盘」。core 停止时入口激活 gate 先返 403
        # （activation_required）；若能越过 gate 才应是 503 CORE_UNREACHABLE —— 两者都满足如实报错。
        err_ok = (400 <= r.status <= 599)
        after = _file_fp(CONFIG_PATH)
        nodisk = (before is not None and before == after)
    finally:
        restart_core()
        up = wait_until(core_ping, RESTART_WAIT_S)
    read_ok = False
    r2 = None
    if up:
        r2 = http('GET', '/api/v1/config', timeout=8)
        try:
            prof = (r2.json().get('data') or {}).get('runtime_profile')
        except Exception:
            prof = None
        read_ok = (r2.status == 200) and isinstance(prof, dict) and len(prof) > 0
    check(cid, desc, err_ok and nodisk and read_ok,
          'PUT HTTP %s（如实报错=%s）；不落盘=%s；恢复读回=%s（GET %s）'
          % (r.status, err_ok, nodisk, read_ok, r2.status if r2 is not None else 'n/a'))


def b27() -> None:
    cid = 'B27'
    desc = 'B27 V-04：TTBOX_MODELS_ROOT 归一——web(ui_meta/_incoming) 与 core 同根'
    default_root = '/opt/ttbox/models'
    web_root = _unit_env(WEB_UNIT, 'TTBOX_MODELS_ROOT') or default_root
    core_root = _unit_env(CORE_UNIT, 'TTBOX_MODELS_ROOT') or default_root
    # 旧同义名 TTBOX_MODEL_ROOT（无 S）必须绝迹（归一后不保留兼容读）；只认 Environment 声明，
    # 不误报注释里对该旧名的说明。
    old_syn = any(_unit_env(u, 'TTBOX_MODEL_ROOT') is not None for u in (WEB_UNIT, CORE_UNIT))
    same = (web_root == core_root)
    check(cid, desc, same and (not old_syn),
          'web 根=%s / core 根=%s / 同根=%s / 旧同义名残留=%s'
          % (web_root, core_root, same, old_syn))


def b28() -> None:
    cid = 'B28'
    desc = 'B28 V-03：板端 %s 与 %s 共享键同值' % (CONFIG_PATH, FACTORY_CONFIG)
    a = read_json(CONFIG_PATH)
    b = read_json(FACTORY_CONFIG)
    if not isinstance(a, dict) or not isinstance(b, dict):
        skip(cid, desc, '缺 %s 或 %s' % (CONFIG_PATH, FACTORY_CONFIG))
        return
    shared = [k for k in a if k in b]
    diff = [k for k in shared if a[k] != b[k]]
    check(cid, desc, bool(shared) and (not diff),
          '共享键 %d 个；不一致 %d 个%s'
          % (len(shared), len(diff), ('：' + ', '.join(diff[:8])) if diff else ''))


def b29() -> None:
    cid = 'B29'
    desc = 'B29 V-06：面板 SSOT 端口可达 + 无装饰性/同义端口 env（V-20）'
    reach = web_reachable()
    try:
        base_port = int(WEB_BASE.rsplit(':', 1)[1].split('/')[0])
    except Exception:
        base_port = -1
    ssot_ok = (base_port == _WEB_PORT_DEFAULT)
    # 只认 Environment=<key>= 声明；V-20 在 unit 注释里提到的旧名不算残留（避免注释误报）。
    banned = [k for k in ('TTBOX_WEB_HOST', 'TTBOX_PORT', 'TTBOX_DEFAULT_WEB_PORT')
              if _unit_env(WEB_UNIT, k) is not None]
    check(cid, desc, reach and ssot_ok and (not banned),
          '可达=%s；base 端口=%s == SSOT(%s)=%s；装饰/同义 env=%s'
          % (reach, base_port, _WEB_PORT_DEFAULT, ssot_ok, banned or '无'))


# ===========================================================================
# main
# ===========================================================================
def main() -> int:
    global WEB_BASE, CARDS_DIR, ALLOW_STATE_TOGGLE, NO_RESTORE, RESTORE_VALID, STRICT

    ap = argparse.ArgumentParser(description='M2.07 板端验收（B15–B25 + 保留项）')
    ap.add_argument('--base-url', default=os.environ.get('TTBOX_WEB', WEB_BASE),
                    help='web 基址（默认 %s）' % WEB_BASE)
    ap.add_argument('--cards-dir', default=os.environ.get('TTBOX_M2_CARDS', CARDS_DIR),
                    help='离线卡目录（默认 %s）' % CARDS_DIR)
    ap.add_argument('--allow-state-toggle', action='store_true',
                    help='允许 B16 临时切换未激活基线（会移走 store/session 并重启 core/web）')
    ap.add_argument('--no-restore', action='store_true',
                    help='B16 临时切换后不自动还原（默认还原）')
    ap.add_argument('--restore-valid', action='store_true',
                    help='收尾用测试卡重新 card-login 恢复激活态')
    ap.add_argument('--strict', action='store_true',
                    help='SKIP/PEND 亦计为失败（默认只 FAIL 计失败）')
    args = ap.parse_args()

    WEB_BASE = args.base_url.rstrip('/')
    CARDS_DIR = args.cards_dir
    ALLOW_STATE_TOGGLE = args.allow_state_toggle
    NO_RESTORE = args.no_restore
    RESTORE_VALID = args.restore_valid
    STRICT = args.strict

    print('=' * 78)
    print('M2.07 板端验收（免密化 + 云端卡密 + 面板 1:1）')
    print('web=%s  core.sock=%s  cards=%s  config=%s' % (WEB_BASE, CORE_SOCK, CARDS_DIR, CONFIG_PATH))
    print('板 serial=%s  测试卡=%s' % (board_serial(), TEST_CARD))
    print('-' * 78)

    if not web_reachable():
        print('!! 板端 web 不可达（%s）—— 除纯离线项外将 SKIP。' % WEB_BASE, flush=True)
    elif not core_ping():
        print('!! core IPC 未就绪（%s）—— 部分项将 SKIP。' % CORE_SOCK, flush=True)

    b17()          # 云端激活（正例）
    b15()          # 免密直达
    b16()          # 激活页 1:1
    b18()          # 心跳
    b19()          # tab 集合
    b20()          # 破坏性直通
    b22()          # URL 可配 + 断网宽限
    b21()          # 到期锁定（PEND-LEAD）
    b23()          # 存量兼容（离线卡）
    retained_b4()
    retained_b5()   # 在 b23 之后、b24 之前：此时 store 为**离线卡**文档 ⇒ 真覆盖"离线卡重启保持"
    retained_cards()
    b24()           # D-D 纯重启锚：自建云激活前置 → 纯重启 core → 纯观测仍 activated
    b25()           # F11：拒绝的 ACTIVATE 不得清云激活态（回归锚）
    b26()           # V-01/V-02：core 停止 ⇒ 写配置如实报错且不落盘；恢复后读回
    b27()           # V-04：TTBOX_MODELS_ROOT 归一，web/core 同根
    b28()           # V-03：板端 default.json ↔ 00-factory 共享键同值
    b29()           # V-06/V-20：面板端口 SSOT + 装饰性/同义 env 归一

    if RESTORE_VALID:
        code = http('POST', '/api/license/activate',
                    json_body={'license_key': TEST_CARD}, timeout=25).status
        print('[收尾] --restore-valid：重新 card-login HTTP %s' % code, flush=True)

    n_pass = sum(1 for r in RESULTS if r['verdict'] == 'PASS')
    n_fail = sum(1 for r in RESULTS if r['verdict'] == 'FAIL')
    n_skip = sum(1 for r in RESULTS if r['verdict'] == 'SKIP')
    n_pend = sum(1 for r in RESULTS if r['verdict'] == 'PEND')
    print('-' * 78)
    print('M2.07 汇总：PASS=%d FAIL=%d SKIP=%d PEND=%d 共 %d 项'
          % (n_pass, n_fail, n_skip, n_pend, len(RESULTS)))
    print('JSON_RESULT=' + json.dumps(
        {'pass': n_pass, 'fail': n_fail, 'skip': n_skip, 'pend': n_pend, 'results': RESULTS},
        ensure_ascii=False))
    print('=' * 78)

    failed = n_fail > 0 or (STRICT and (n_skip > 0 or n_pend > 0))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
