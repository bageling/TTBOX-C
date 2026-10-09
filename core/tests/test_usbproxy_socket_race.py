# -*- coding: utf-8 -*-
"""V1.0.27「usb-proxy event.sock 启动竞态」的护栏。

★ 事故（2026-10-04 18:20，业主升 V1.0.25 后报"出错了"）：
    core 与 ttbox-usbproxy **同一秒**启动（systemd unit 之间无依赖），
    core 去 connect event.sock（usb-proxy 侧，真源见 Paths.hpp） 时 usb-proxy 还没建 →
    ENOENT → 上层按「不阻塞 AI 流水线」直接放弃 PhysicalMouseReader
    → **物理鼠标输入链永久断开且不自动重连** → 板端日志：
        PhysicalMouseReader: event.sock fallback failed: 连接 usb-proxy event socket 失败
    手工 `systemctl restart ttbox-core` 立刻恢复（socket 那时已存在）。

★ 为什么只能源码级断言：PhysicalMouseReader 走 `#if defined(_WIN32)` 分流，
  host（本机 Windows）编译的是 Windows 分支 ⇒ 本机 ctest 对这段**零覆盖**。
  真正的验证是 WSL 交叉编译（编 `#else` 分支）+ 板端实跑。
"""
from __future__ import annotations

import re
from pathlib import Path

from _ttbox_paths import discover_root

REPO = Path(discover_root(__file__))
READER = REPO / 'core' / 'src' / 'input' / 'PhysicalMouseReader.cpp'
UNIT = REPO / 'deploy' / 'systemd' / 'ttbox-core.service'


def _src(p: Path) -> str:
    return p.read_text(encoding='utf-8')


def _span(text: str, start: str, end: str) -> str:
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


def test_event_socket_connect_retries_instead_of_giving_up():
    """★ 核心：start_event_socket 必须**重试**（socket 是 usb-proxy 异步建的）。"""
    src = _src(READER)
    body = _span(src, 'bool PhysicalMouseReader::start_event_socket(',
                 '\nbool PhysicalMouseReader::start(')
    assert re.search(r'for\s*\(int\s+attempt', body), (
        'start_event_socket 必须循环重试 connect —— usb-proxy 建 socket 比 core 晚，'
        '一次失败就放弃会让物理鼠标输入链永久断开（2026-10-04 板端事故）')
    m = re.search(r'kConnectAttempts\s*=\s*(\d+)', body)
    assert m and int(m.group(1)) >= 5, (
        '重试次数太少（%s）：usb-proxy 还要做设备枚举，1~2 次不够；至少 5 次'
        % (m.group(1) if m else '?'))
    assert re.search(r'kRetryInterval\s*=\s*std::chrono::milliseconds\s*\(\s*(\d+)', body), (
        '必须用可移植的 std::chrono::milliseconds（usleep 在 MSVC 下不存在，'
        '而这段代码不在 #if 分支里、Windows 也要编过 —— 2026-10-04 实测踩到）')
    # 重试之间要有等待（忙等没意义）
    assert ('usleep' in body or 'sleep_for' in body), '重试之间必须等待，否则忙等毫无意义'
    # 成功才起线程；全部失败才返回 false
    assert body.index('event_thread_ = std::thread') < body.rindex('return false'), (
        '线程必须在成功分支里启动，失败分支不能起线程')


def test_unit_orders_core_after_usbproxy():
    """systemd 侧的第二道保险：core 必须排在 usbproxy 之后。"""
    unit = _src(UNIT)
    m = re.search(r'^After=(.*)$', unit, re.M)
    assert m, 'unit 缺 After='
    assert 'ttbox-usbproxy.service' in m.group(1), (
        'After= 必须含 ttbox-usbproxy.service —— 否则两者并行启动，core 拿到 ENOENT')
    w = re.search(r'^Wants=(.*)$', unit, re.M)
    assert w and 'ttbox-usbproxy.service' in w.group(1), (
        'Wants= 也要含 usb-proxy（拉起它），否则冷启动时它可能根本没起')


def test_open_event_socket_is_reentrant():
    """重试的前提：open_event_socket 每次失败都要 close 并复位 fd（否则第二次调用泄漏 fd）。"""
    src = _src(READER)
    body = _span(src, 'bool PhysicalMouseReader::open_event_socket(',
                 '\nbool PhysicalMouseReader::start_event_socket(')
    # 每个失败分支都必须 close + 复位
    fails = body.count('return false')
    # close 有时写 ::close(...) 有时写 close(...) ⇒ 用词边界匹配（★ 不要写
    # `::?close`：那里的 `:` 是**必需**的，只能匹配到带冒号的那 1 处，漏掉 5 处裸 close。
    # 2026-10-04 实测栽过：判据数出 1、代码里其实有 6。）
    closes = len(re.findall(r'\bclose\s*\(\s*event_fd_\s*\)', body))
    resets = len(re.findall(r'event_fd_\s*=\s*-1', body))
    assert fails >= 3, '失败分支数量异常（%d）—— 判据可能失效' % fails
    assert closes >= 3 and resets >= 3, (
        'open_event_socket 的每个失败分支都必须 close+复位 event_fd_，'
        '否则重试会泄漏 fd（close=%d reset=%d fail=%d）' % (closes, resets, fails))
