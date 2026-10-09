// inject_clock.hpp — AI 位移的「自有时钟」投递策略（纯逻辑，无线程，可单测）
//
// 为什么需要它（2026-09-24 板端实测）：
//   compat 模式下 AI 位移原本只在**物理报告到来时**才被取走合并（"搭车"）。
//   而现场那只 dongle 静止时 IN 端点报告率是 **0/s**（移动时才 105~170/s）⇒
//   手一停，AI 算出来的位移就全堆在挂起量里出不去；手一动，一次性把堆的量塞进
//   一份报告 ⇒ 准星瞬移（复算：单次投递最大 39.7 count ≈ 26 px；参数正常后可到 353 count）。
//
// 本模块只做两件纯计算的事：
//   1) 把累计位移**切成一份可投递的步长**：每轴限幅（余量顺延，不丢）＋余量上限（
//      超出的陈旧量丢弃，免得几十毫秒前的意图还在拖着准星跑）；
//   2) 按接口描述符解析出的真实布局**构造一份 HID 状态报告**（只写 buttons/X/Y/wheel，
//      其余字节一律 0 —— 不猜、不复制未知字段）。
//
// 投递由 mouse_control 的 1ms 节拍线程驱动（见 mouse_control.cpp 的 inject_loop）。
#pragma once

#include <cstdint>

#include "hid_report_layout.hpp"

namespace ttbox_usbproxy {

struct InjectClockConfig {
    int32_t step_max_x = 96;      // 单份报告 X 位移上限（count）
    int32_t step_max_y = 96;      // 单份报告 Y 位移上限（count）
    int32_t step_max_wheel = 1;   // 滚轮一格就是一次事件，绝不叠加
    int32_t backlog_max = 768;    // 余量上限（count）：超出即丢弃（防陈旧意图积压）
};

struct InjectStep {
    int32_t dx = 0, dy = 0, wheel = 0;               // 本次报告实际投递的量
    int32_t rest_x = 0, rest_y = 0, rest_wheel = 0;  // 顺延到下一拍的余量
    int32_t dropped_x = 0, dropped_y = 0, dropped_wheel = 0;  // 因余量上限丢弃的量（诊断）
};

// 把累计位移切成一份可投递的步长。
InjectStep inject_clock_plan(const InjectClockConfig& cfg, int32_t dx, int32_t dy, int32_t wheel);

// 按布局构造一份 HID 报告：buttons 写掩码，X/Y/wheel 写位移，其余字节 0。
// 返回 false = 布局里既没有 X/Y 也没有 buttons ⇒ 调用方必须放弃这一拍。
bool inject_clock_build_report(const HidMouseDescriptor& desc,
                               uint8_t buttons,
                               const InjectStep& step,
                               uint8_t* out, uint32_t cap, uint32_t* out_len);

}  // namespace ttbox_usbproxy
