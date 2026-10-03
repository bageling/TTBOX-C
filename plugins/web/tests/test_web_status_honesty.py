# test_web_status_honesty.py — 第一档 T1-2/T1-3/T1-4 回归锁：状态与结果必须与事实一致
#
# 审计结论（docs/交付前Web按钮落实审计-2026-09-19.md）：
#   D-6 鼠标侧：apply_mouse_config 恒返 applied=True；Core 没有 usb_*/proxy_mode 成员
#       （真源在 usbproxy/gadget-config.json）；前端还谎报"Windows 会重新枚举"。
#       另外它返回的 env_path=/etc/default/ttbox-usb-proxy 全仓无人读，纯装饰。
#   D-7 风扇：_fan_control_payload() 恒返 enabled=False，而 Core 启动就把
#       /sys/class/hwmon/hwmon8/pwm1 写成 255（Application.cpp "风扇满转"段）
#       ⇒ 风扇实际满转、面板显示"未启用"，与物理事实相反。
#   D-8 电源：reboot/poweroff 起线程 sleep 1.5s 后 os.system(...)，同时立刻回
#       scheduled=True —— 授权通不过也照样报成功。
#
# 本文件锁死修复后的三处口径：
#   ① 风扇 enabled 由真实 PWM 读数决定；
#   ② 电源动作先向 systemd-logind 要真实权限结论，再决定要不要承诺；
#   ③ USB 身份配置真下发到 usb-proxy 的 cmd.sock（0x4F50 协议），applied 由回包决定。
#
# 运行：python -m pytest plugins/web/tests/test_web_status_honesty.py -v
from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import struct
import sys
import tempfile
import types

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
#★ 2026-10-02（web 换写法 S7）：风扇/loopout 载荷已从入口搬到 lib/hw_payloads.py。
#   下面那条「源码级兜底」断言抽的是 _fan_control_payload 的函数体，
#   目标跟着搬 —— 规则（不许恒False、必须调 _fan_enabled）一个字没改。
PAYLOAD_SRC = REPO_ROOT / 'plugins' / 'web' / 'lib' / 'hw_payloads.py'
TEMPLATE = REPO_ROOT / 'plugins' / 'web' / 'templates' / 'index.html'
from plugins.web.tests import panel_src  # noqa: E402  2026-10-03 面板外链：读整个面板

_load_seq = 0


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_honesty_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    tmp = tempfile.mkdtemp(prefix='ttbox_honesty_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PREFIX', tmp)
    monkeypatch.setenv('TTBOX_PRESETS_DIR', os.path.join(tmp, 'presets'))
    monkeypatch.setenv('TTBOX_CONFIG_DIR', os.path.join(tmp, 'config'))
    monkeypatch.setenv('TTBOX_MODELS_ROOT', os.path.join(tmp, 'models'))
    return _load()


def _client(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_activation_ok', lambda: True)
    # 运行期配置走 IPC（本机无 Core，socket.AF_UNIX 在 Windows 上也不存在），
    # 替换成空 profile —— 被测的是路由自身的行为，不是 IPC。
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: {})
    return web_mod.app.test_client()


def _code_only(path) -> str:
    """去掉 `#` 注释尾巴 —— 注释里引用的旧字面量不算违规，代码里的才算。"""
    lines = []
    for ln in path.read_text(encoding='utf-8').split('\n'):
        idx = ln.find('#')
        lines.append(ln if idx < 0 else ln[:idx])
    return '\n'.join(lines)


# ── T1-3 风扇：状态必须反映真实 PWM ──────────────────────────────────────

def test_fan_enabled_follows_real_pwm_not_a_constant(web_mod):
    assert web_mod._fan_enabled('/sys/class/hwmon/hwmon8/pwm1', 255) is True
    assert web_mod._fan_enabled('/sys/class/hwmon/hwmon8/pwm1', 1) is True
    # 节点在但占空比为 0 ⇒ 确实没转
    assert web_mod._fan_enabled('/sys/class/hwmon/hwmon8/pwm1', 0) is False
    # 没有 PWM 节点 ⇒ 不可用，不能报"在转"
    assert web_mod._fan_enabled('', 255) is False


def test_fan_payload_does_not_hardcode_enabled_false():
    """源码级兜底：那行恒 False 不许回来。"""
    src = PAYLOAD_SRC.read_text(encoding='utf-8')
    start = src.index('def _fan_control_payload()')
    end = src.index('def _loopout_payload()')
    body = src[start:end]
    assert "'enabled': False" not in body
    assert '_fan_enabled(pwm_path, pwm_raw)' in body


# ── T1-4 电源：先确权，再承诺 ────────────────────────────────────────────

@pytest.mark.parametrize('answer,allowed,fragment', [
    ('s "yes"', True, ''),
    ('s "no"', False, '无权'),
    ('s "challenge"', False, '交互式授权'),
    ('', False, '无法查询'),
])
def test_power_action_allowed_reads_logind_verdict(web_mod, monkeypatch, answer,
                                                   allowed, fragment):
    monkeypatch.setattr(web_mod, '_run_quiet', lambda argv, timeout=5.0: answer)
    ok, why = web_mod._power_action_allowed('reboot')
    assert ok is allowed
    assert fragment in why


def test_power_action_uses_canreboot_for_reboot_and_canpoweroff_for_poweroff(web_mod, monkeypatch):
    seen = []

    def fake_run(argv, timeout=5.0):
        seen.append(argv[-1])
        return 's "yes"'

    monkeypatch.setattr(web_mod, '_run_quiet', fake_run)
    web_mod._power_action_allowed('reboot')
    web_mod._power_action_allowed('poweroff')
    assert seen == ['CanReboot', 'CanPowerOff']


def test_reboot_route_refuses_when_polkit_says_no(web_mod, monkeypatch):
    """授权不通过时必须 403，不能再回一句没有事实支撑的"已发送"。"""
    monkeypatch.setattr(web_mod, '_power_action_allowed',
                        lambda verb: (False, '当前账户无权执行该电源操作（polkit 拒绝）'))
    resp = _client(web_mod, monkeypatch).post('/api/system/reboot')
    assert resp.status_code == 403
    body = resp.get_json()
    assert body['ok'] is False
    assert body['data']['scheduled'] is False
    assert '无权' in body['error']


def test_power_dry_run_still_returns_200_for_acceptance_scripts(web_mod, monkeypatch):
    """dry_run 是验收脚本的连通性自检（按 200 判鉴权门），语义不能改。"""
    monkeypatch.setattr(web_mod, '_power_action_allowed', lambda verb: (False, '无权限'))
    for route in ('/api/system/reboot', '/api/system/poweroff'):
        resp = _client(web_mod, monkeypatch).post(route, json={'dry_run': True})
        assert resp.status_code == 200
        body = resp.get_json()
        assert body['ok'] is True
        # 真实权限结论仍要带出来，别只给一个好看的 200
        assert body['data']['allowed'] is False
        assert body['data']['reason'] == '无权限'


# ── T1-2 鼠标：身份配置真下发到 usb-proxy ────────────────────────────────

class _FakeSock:
    """假 cmd.sock：记下发出的字节，按脚本回包。"""

    def __init__(self, reply=b'', fail_connect=None):
        self.reply = reply
        self.fail_connect = fail_connect
        self.sent = b''
        self.closed = False

    def settimeout(self, _):
        pass

    def connect(self, path):
        self.path = path
        if self.fail_connect:
            raise OSError(self.fail_connect)

    def sendall(self, data):
        self.sent = data

    def recv(self, _n):
        return self.reply

    def close(self):
        self.closed = True


def _install_fake_socket(web_mod, monkeypatch, sock):
    # AF_UNIX/SOCK_SEQPACKET 用字面量：Windows 版 CPython 不保证有 AF_UNIX，
    # 而这里被测的是协议编码，不是内核 socket。
    monkeypatch.setattr(web_mod, 'socket', types.SimpleNamespace(
        AF_UNIX=1, SOCK_SEQPACKET=5, socket=lambda *a, **k: sock))
    return sock


def _ok_reply():
    return struct.pack('<HBB', 0x4F50, 1, 15) + struct.pack('<I', 1) + b'\x01'


CFG = {
    'usb_vid': 0x9A80, 'usb_pid': 0x7072, 'usb_bcd_usb': 0x0200, 'usb_bcd_device': 0x0100,
    'usb_device_class': 0, 'usb_device_subclass': 0, 'usb_device_protocol': 0,
    'usb_max_power': 250, 'hid_protocol': 2, 'hid_subclass': 1,
    'hid_report_length': 4, 'hid_interval': 1,
    'usb_manufacturer': 'Corsair', 'usb_product': 'Corsair USB Optical Mouse',
    'usb_serial': 'OPI5P-MOUSE', 'usb_configuration': 'Mouse',
    'hid_report_desc_hex': '05010902',
}


def test_set_config_wire_format_matches_cpp_encoder(web_mod, monkeypatch):
    """逐字节对照 mouse_control.cpp::encode_config_payload 的读写顺序。"""
    sock = _install_fake_socket(web_mod, monkeypatch, _FakeSock(reply=_ok_reply()))
    ok, why = web_mod._usbproxy_send_set_config(CFG, apply_now=True)
    assert (ok, why) == (True, '')

    data = sock.sent
    magic, version, rtype = struct.unpack_from('<HBB', data, 0)
    assert (magic, version, rtype) == (0x4F50, 1, 14)
    assert struct.unpack_from('<I', data, 4)[0] == 1
    assert data[8] == 1  # apply_now

    off = 9

    def r16():
        nonlocal off
        v = struct.unpack_from('<H', data, off)[0]
        off += 2
        return v

    def r8():
        nonlocal off
        v = data[off]
        off += 1
        return v

    def rstr():
        nonlocal off
        n = struct.unpack_from('<H', data, off)[0]
        off += 2
        s = data[off:off + n].decode('utf-8')
        off += n
        return s

    assert r16() == 0x9A80 and r16() == 0x7072
    assert r16() == 0x0200 and r16() == 0x0100
    assert r8() == 0 and r8() == 0 and r8() == 0
    assert r16() == 250          # usb_max_power 是 u16，不是 u8
    assert r8() == 2 and r8() == 1 and r8() == 4 and r8() == 1
    assert rstr() == 'Corsair'
    assert rstr() == 'Corsair USB Optical Mouse'
    assert rstr() == 'OPI5P-MOUSE'
    assert rstr() == 'Mouse'
    assert rstr() == '05010902'
    assert off == len(data)      # 一个字节不多、不少


def test_set_config_apply_now_flag_is_carried(web_mod, monkeypatch):
    sock = _install_fake_socket(web_mod, monkeypatch, _FakeSock(reply=_ok_reply()))
    web_mod._usbproxy_send_set_config(CFG, apply_now=False)
    assert sock.sent[8] == 0


def test_set_config_reports_unreachable_socket_as_failure(web_mod, monkeypatch):
    _install_fake_socket(web_mod, monkeypatch, _FakeSock(fail_connect='no such file'))
    ok, why = web_mod._usbproxy_send_set_config(CFG, apply_now=True)
    assert ok is False
    assert 'usb-proxy 未运行' in why


def test_set_config_reports_error_reply_as_failure(web_mod, monkeypatch):
    reply = struct.pack('<HBB', 0x4F50, 1, 3) + struct.pack('<I', 1) + b'config save failed\x00'
    _install_fake_socket(web_mod, monkeypatch, _FakeSock(reply=reply))
    ok, why = web_mod._usbproxy_send_set_config(CFG, apply_now=True)
    assert ok is False
    assert 'config save failed' in why


def test_set_config_rejects_wrong_magic_and_short_reply(web_mod, monkeypatch):
    _install_fake_socket(web_mod, monkeypatch,
                         _FakeSock(reply=struct.pack('<HBB', 0x1234, 1, 15) + b'xxxx'))
    ok, why = web_mod._usbproxy_send_set_config(CFG, apply_now=False)
    assert ok is False and '协议不符' in why

    _install_fake_socket(web_mod, monkeypatch, _FakeSock(reply=b'\x01\x02'))
    ok, why = web_mod._usbproxy_send_set_config(CFG, apply_now=False)
    assert ok is False and '过短' in why


def test_mouse_current_mode_comes_from_systemd_unit_not_profile(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_usbproxy_unit_mode', lambda: 'synthetic')
    assert web_mod._mouse_current_mode({'mode': 'full_passthrough'}) == 'synthetic'
    monkeypatch.setattr(web_mod, '_usbproxy_unit_mode', lambda: 'full')
    assert web_mod._mouse_current_mode({'mode': 'synthetic'}) == 'full_passthrough'
    # 读不到单元才回落 profile，且不再把 synthetic 错映射成 full_passthrough
    monkeypatch.setattr(web_mod, '_usbproxy_unit_mode', lambda: '')
    assert web_mod._mouse_current_mode({'mode': 'synthetic'}) == 'synthetic'


def test_mouse_config_for_form_renders_ids_as_hex_like_get(web_mod):
    out = web_mod._usbproxy_config_for_form(CFG)
    assert out['usb_vid'] == '0x9A80'
    assert out['usb_pid'] == '0x7072'
    assert out['usb_max_power'] == 250
    assert out['usb_manufacturer'] == 'Corsair'


def test_put_mouse_hardware_reports_failure_when_push_fails(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_mouse_save_or_ipc', lambda prof, mouse: (True, ''))
    monkeypatch.setattr(web_mod, '_usbproxy_send_set_config',
                        lambda cfg, apply_now: (False, 'usb-proxy 未运行'))
    resp = _client(web_mod, monkeypatch).put('/api/hardware/mouse', json={'config': CFG})
    body = resp.get_json()
    assert body['ok'] is False
    assert body['data']['applied'] is False
    assert body['data']['apply_error'] == 'usb-proxy 未运行'
    assert 'usb-proxy 未运行' in body['error']


def test_put_mouse_hardware_reports_success_only_when_both_channels_land(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_mouse_save_or_ipc', lambda prof, mouse: (True, ''))
    monkeypatch.setattr(web_mod, '_usbproxy_send_set_config',
                        lambda cfg, apply_now: (True, ''))
    resp = _client(web_mod, monkeypatch).put('/api/hardware/mouse', json={'config': CFG})
    body = resp.get_json()
    assert body['ok'] is True
    assert body['data']['applied'] is True
    assert body['error'] == ''


def test_put_mouse_hardware_fails_when_core_channel_fails(web_mod, monkeypatch):
    """Core 侧没落地也算失败 —— ok 取两个通道的合取。"""
    monkeypatch.setattr(web_mod, '_mouse_save_or_ipc',
                        lambda prof, mouse: (False, 'Core 未运行，无法保存配置'))
    monkeypatch.setattr(web_mod, '_usbproxy_send_set_config',
                        lambda cfg, apply_now: (True, ''))
    body = _client(web_mod, monkeypatch).put('/api/hardware/mouse',
                                             json={'config': CFG}).get_json()
    assert body['ok'] is False
    assert 'Core 未运行' in body['error']


def test_put_mouse_hardware_without_config_does_not_claim_applied(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_mouse_save_or_ipc', lambda prof, mouse: (True, ''))
    body = _client(web_mod, monkeypatch).put('/api/hardware/mouse', json={}).get_json()
    assert body['ok'] is False
    assert body['data']['applied'] is False


def test_put_mouse_mode_no_longer_pretends_success(web_mod, monkeypatch):
    """透传模式改不动（真源是 systemd 单元），必须如实报失败。"""
    monkeypatch.setattr(web_mod, '_mouse_save_or_ipc', lambda prof, mouse: (True, ''))
    monkeypatch.setattr(web_mod, '_usbproxy_unit_mode', lambda: 'full')
    resp = _client(web_mod, monkeypatch).put('/api/hardware/mouse/mode',
                                             json={'mode': 'synthetic'})
    body = resp.get_json()
    assert body['ok'] is False
    assert 'systemd' in body['error']
    assert body['data']['mode'] == 'full_passthrough'   # 报的是真实模式


def test_source_has_no_fabricated_mouse_env_path_or_constant_applied():
    src = _code_only(WEB_SRC)
    assert '/etc/default/ttbox-usb-proxy' not in src
    assert "'applied': True" not in src


# ── 前端契约：三个面板都要读真实字段 ─────────────────────────────────────

def test_frontend_reads_action_available_and_gadget_config():
    src = panel_src.all_src()
    assert 'rootfs.action_available' in src, '扩容按钮必须受执行通道开关约束'
    assert 'gadget_config' in src, '鼠标面板必须优先显示已落盘的真实身份'
    assert 'payload.rootfs' in src, '扩容失败也要按契约渲染真实 rootfs'
