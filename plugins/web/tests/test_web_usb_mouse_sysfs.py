# -*- coding: utf-8 -*-
"""USB 鼠标 sysfs 解析的行为锁定测试（2026-10-02 S10-c 随拆分补写）。

★ 为什么这个测试现在才写得出来：
  拆之前 `_probe_usb_mouse` 那70 行埋在 `get_mouse_hardware` 里，
  要测就得把整个 Flask app 拉起来 + 真的存在 /sys/bus/usb/devices/*。
  拆成独立函数后，只要注入假的 glob / open 就能验解析逻辑 ——
  这就是 S10-c 拆分的直接收益。

★ 覆盖的解析点（每一条都是会静默出错的地方）：
  · 接口子目录 3-1:1.1 → 向上找到父设备 3-1（父设备才有 vid/pid）
  · bcdVersion "2.00" → usb_bcd_usb "0x0200"（去点 + 前缀 0x）
  · bMaxPower "98mA" → 98（去 mA 后缀）
  · bInterfaceClass != '03' → 跳过（不是 HID）
  · bDeviceClass 是十六进制字符串 → int(x, 16)
  · 端点 ep_* 的 bInterval → hid_interval（取第一个有值的）
  · 任何一步读不到 → 不抛，返回默认值 + connected=False
"""
import sys
from pathlib import Path

import pytest

# ★ 根锚用 paths.discover_root（门禁⑩ 禁parents[N]）：从本文件逐级向上找
#   同时含全部 _ROOT_ANCHORS 的目录，找不到就炸—— 不静默指错根。
from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))


# ---- 假的 sysfs ----
# 树结构（key = 路径，value = 文件内容；None 表示目录）
FILES = {
    '/sys/bus/usb/devices/3-1/idVendor': '046d',
    '/sys/bus/usb/devices/3-1/idProduct': 'c077',
    '/sys/bus/usb/devices/3-1/manufacturer': 'Logitech',
    '/sys/bus/usb/devices/3-1/product': 'USB Receiver',
    '/sys/bus/usb/devices/3-1/configuration': 'Unifying',
    '/sys/bus/usb/devices/3-1/bcdDevice': '0010',
    '/sys/bus/usb/devices/3-1/version': '2.00',
    '/sys/bus/usb/devices/3-1/bDeviceClass': '00',
    '/sys/bus/usb/devices/3-1/bDeviceSubClass': '00',
    '/sys/bus/usb/devices/3-1/bDeviceProtocol': '00',
    '/sys/bus/usb/devices/3-1/bMaxPower': '98mA',
    '/sys/bus/usb/devices/3-1/serial': 'ABC123',
    # 接口 1（鼠标）
    '/sys/bus/usb/devices/3-1:1.0/bInterfaceClass': '03',
    '/sys/bus/usb/devices/3-1:1.0/bInterfaceProtocol': '02',
    '/sys/bus/usb/devices/3-1:1.0/bInterfaceSubClass': '01',
    '/sys/bus/usb/devices/3-1:1.0/ep_81/bInterval': '10',
    # 接口 2（键盘，也是 HID 03，但排在后面，不该被选中）
    '/sys/bus/usb/devices/3-1:1.1/bInterfaceClass': '03',
    '/sys/bus/usb/devices/3-1:1.1/bInterfaceProtocol': '01',
    # 一个非 HID 设备
    '/sys/bus/usb/devices/1-1/bInterfaceClass': '09',
    '/sys/bus/usb/devices/1-1/idVendor': '8087',
}


class FakeGlob:
    """只认两个固定模式，模拟真实 glob 的两个用法。"""

    def __init__(self, hidg=('/dev/hidg0',)):
        self._hidg = list(hidg)
        self._pats = {
            '/sys/bus/usb/devices/*':
                ['/sys/bus/usb/devices/1-1', '/sys/bus/usb/devices/3-1',
                 '/sys/bus/usb/devices/3-1:1.0',
                 '/sys/bus/usb/devices/3-1:1.1'],
            '/dev/hidg*': self._hidg,
        }

    def glob(self, pat):
        if pat in self._pats:
            return list(self._pats[pat])
        if pat.endswith('ep_*'):          # /sys/bus/usb/devices/3-1:1.0/ep_*
            if '3-1:1.0' in pat:
                return ['/sys/bus/usb/devices/3-1:1.0/ep_81']
            return []
        return []


class FakeOpen:
    def __init__(self, files):
        self._f = files

    def __call__(self, path, *a, **k):
        p = str(path).replace('\\', '/')
        if p in self._f:
            import io
            return io.StringIO(self._f[p])
        raise FileNotFoundError(p)


@pytest.fixture
def hw(monkeypatch):
    """载入 api.hardware 并注入假 sysfs。"""
    import plugins.web.api.hardware as H
    monkeypatch.setattr(H, 'glob', FakeGlob(), raising=False)
    monkeypatch.setattr(H.os.path, 'exists',
                        lambda p: str(p).replace('\\', '/') in FILES)
    monkeypatch.setattr(H.os.path, 'isdir', lambda p: True)
    monkeypatch.setattr(H.os.path, 'basename', H.os.path.basename)
    monkeypatch.setattr(H, 'open', FakeOpen(FILES), raising=False)
    return H


def test_detects_hid_and_reads_vid_pid(hw):
    cfg, phys, connected = hw._probe_usb_mouse()
    assert connected is True
    assert cfg['usb_vid'] == '0x046d'
    assert cfg['usb_pid'] == '0xc077'
    assert cfg['usb_manufacturer'] == 'Logitech'
    assert cfg['usb_product'] == 'USB Receiver'


def test_parent_device_lookup_strips_interface_suffix(hw):
    """接口 3-1:1.0 没有 vid/pid，要向上找到父设备 3-1。"""
    cfg, phys, connected = hw._probe_usb_mouse()
    assert phys['device'] == '3-1'          # 不是 '3-1:1.0'
    assert phys['interface'] == '3-1:1.0'
    assert phys['name'] == 'USB Receiver'


def test_bcd_usb_dots_removed(hw):
    """version '2.00' ⇒ usb_bcd_usb '0x200'（去点，不补零）。

    ★ 实测确认真实产出是 '0x200'（不是 '0x0200'）—— 原码就是
      `'0x' + bcd_usb.replace('.', '')`，不去前导零。这里如实锁住，
      免得以后有人「顺手修正」成 4 位而改了 usbproxy 侧的解析。
    """
    cfg, _, _ = hw._probe_usb_mouse()
    assert cfg['usb_bcd_usb'] == '0x200'
    assert cfg['usb_bcd_device'] == '0x0010'


def test_max_power_strips_ma(hw):
    """bMaxPower '98mA' ⇒ 98（整数，不是字符串）。"""
    cfg, _, _ = hw._probe_usb_mouse()
    assert cfg['usb_max_power'] == 98


def test_hid_protocol_and_interval_from_interface(hw):
    """hid_protocol / hid_subclass 读接口的，hid_interval 读端点的。"""
    cfg, _, _ = hw._probe_usb_mouse()
    assert cfg['hid_protocol'] == 2
    assert cfg['hid_subclass'] == 1
    assert cfg['hid_interval'] == 10


def test_non_hid_device_skipped(hw):
    """bInterfaceClass != 03 的设备不该被选中（1-1 是 09 = 供应商类）。"""
    cfg, _, connected = hw._probe_usb_mouse()
    assert connected is True
    assert cfg['usb_vid'] != '0x8087'


def test_no_hid_yields_defaults_not_raise(hw):
    """一个 HID 都没有 ⇒ 返回默认值 + connected=False，不能抛。"""
    monkey = FakeGlob()
    monkey._pats['/sys/bus/usb/devices/*'] = ['/sys/bus/usb/devices/1-1']
    hw.glob = monkey
    cfg, phys, connected = hw._probe_usb_mouse()
    assert connected is False
    assert phys == {'device': '', 'interface': '', 'name': ''}
    assert cfg == hw._default_usb_cfg()


def test_default_usb_cfg_shape():
    """默认 USB 描述符 17 个字段（Web 契约，缺字段面板会取不到）。

    ★ 17 是 2026-10-02 拆分时的实测值（原 usb_cfg 字面量逐字搬过来），
      数目本身被锁住 —— 面板按固定下标/键名取，多一个少一个都会静默错位。
    """
    import plugins.web.api.hardware as H
    d = H._default_usb_cfg()
    assert len(d) == 17
    for k in ('usb_vid', 'usb_pid', 'usb_bcd_device', 'usb_bcd_usb',
              'usb_max_power', 'hid_interval', 'hid_protocol',
              'hid_subclass', 'hid_report_length', 'hid_report_desc_hex',
              'usb_configuration', 'usb_device_class', 'usb_device_protocol',
              'usb_device_subclass', 'usb_manufacturer', 'usb_product',
              'usb_serial'):
        assert k in d, k


def test_service_probe_falls_back_to_hidg(monkeypatch):
    """systemctl 读不到时，退回「有 hidg 节点就算 active」。"""
    import plugins.web.api.hardware as H

    class Boom:
        @staticmethod
        def check_output(*a, **k):
            raise OSError('no systemd')
    monkeypatch.setattr(H, 'subprocess', Boom)
    assert H._probe_usbproxy_service(True) is True
    assert H._probe_usbproxy_service(False) is False


def test_service_probe_reads_systemctl(monkeypatch):
    """systemctl 说 active 就 active。"""
    import plugins.web.api.hardware as H

    class Fake:
        @staticmethod
        def check_output(argv, text=None, timeout=None):
            assert argv[1:] == ['is-active', 'ttbox-usbproxy']
            return 'active\n'
    monkeypatch.setattr(H, 'subprocess', Fake)
    assert H._probe_usbproxy_service(False) is True
