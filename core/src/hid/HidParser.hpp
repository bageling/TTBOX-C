// HidParser.hpp — A9 HID 报告解析（鼠标/键盘）
//
// RAW 透传不做重编码；本模块仅用于解析为 MouseState/KeyboardState
// （调试/接口），坐标转换见 HidTypes.hpp 的 CoordinateTransform。
#pragma once

#include "hid/HidTypes.hpp"

namespace ttbox::core {

// 解析鼠标 HID report（boot 布局 + 常见扩展）
//   [0]=buttons  [1]=dx  [2]=dy  [3]=wheel  [4]=hwheel(可选) [5..]=扩展
MouseState parse_mouse_report(const HidReport& rep);

// 解析键盘 boot report（8 字节：modifier + reserved + 6 keys）
KeyboardState parse_keyboard_report(const HidReport& rep);

}  // namespace ttbox::core
