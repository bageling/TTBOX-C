// hidraw_source.hpp — HID 报告描述符的「内核抄底」来源（2026-09-27，1.5.57）
//
// 背景（1.5.56 板端事故，Compx Nearlink Dongle 373b:10c9 实测）：
//   ec49c73 加的「active fetch」直接向物理设备发 class GET_DESCRIPTOR(Report)。
//   有些 quirk 固件（本例 Nearlink dongle）在 usbfs/libusb 上下文里收到该请求
//   **一律超时且控制端点被打死**（板上实测 3 接口 × 3 种 wLength 全 -7，之后连
//   枚举都失败 error -110）；而内核 hid-generic 正常枚举时**能拿到**描述符。
//   ⇒ 结论：能不走 USB 就不走 USB——内核把描述符缓存在 hidraw 里（HIDIOCGRDESC
//   返回缓存，不发任何 USB 请求），在 usb-proxy detach 内核驱动**之前**抄走。
//
// 三个来源的优先级（谁成功用谁）：
//   ① hidraw_prefetch_descriptors()：connect 时驱动还挂着，直接读 hidraw 缓存。
//      零 USB 请求，对任何固件都安全。
//   ② hidraw_refetch_via_rebind()：usb-proxy 重启而 dongle 未重插时驱动已被
//      上一轮 detach、hidraw 已消失——把接口**短暂绑回 usbhid**，让内核用
//      「正常枚举上下文」重新读一次描述符（quirk 固件只吃这一口），读完立刻解绑。
//   ③ class 请求 active fetch（proxy.cpp 的 worker，每接口至多 1 次）：正常
//      鼠标可用；quirk 固件会超时——所以限次数，不反复打。
//   ④ 全部失败 → fail-open（proxy.cpp：开门保物理透传，注入降级禁用）。
#ifndef TTBOX_HIDRAW_SOURCE_H
#define TTBOX_HIDRAW_SOURCE_H

#include <stdint.h>

namespace ttbox_usbproxy {

// 每个 USB 设备最多几个 HID 接口参与布局学习（与 mouse_control 的接口上限一致）。
static const int kHidRawMaxIfaces = 8;

// 在 detach 内核驱动【之前】调用：扫描 /sys/class/hidraw，把属于 (bus,addr)
// 这台设备的 hidraw 描述符抄进缓存。返回成功读到的接口数。
int hidraw_prefetch_descriptors(uint8_t bus, uint8_t addr);

// hidraw 已消失（重启未重插）时的兜底：把 (bus,addr) 设备的 HID 接口短暂绑回
// usbhid，等内核枚举出 hidraw、读到缓存描述符后再解绑。返回新读到的接口数。
int hidraw_refetch_via_rebind(uint8_t bus, uint8_t addr);

// 取某接口的缓存描述符；没有该接口的缓存返回 false。
bool hidraw_get(uint8_t iface, const uint8_t** data, uint32_t* len);

// 换设备重连时清缓存。
void hidraw_clear();

// ── worker 的决策函数（纯函数，单测钉行为）────────────────────────────────
// 输入：该接口是否已就绪 / 是否有 hidraw 缓存 / 该接口 class 请求已试过几次 /
//       连续无进展轮数。输出：这轮该做什么。
enum class HidLayoutAction {
	kFeedPrefetch,    // 用 hidraw 缓存喂 mouse_control（零 USB 请求）
	kTryClassRequest, // 试一次 class 请求（每接口全程至多 1 次）
	kFailOpen,        // 放弃：开门保物理透传（注入无布局自动降级）
	kWait,            // 本轮无事可做
};
// 规则（板端 1.5.56 事故钉死）：
//   · 已就绪 → kWait；
//   · 有 hidraw 缓存 → 永远 kFeedPrefetch；
//   · 没缓存且 class 请求没试过 → kTryClassRequest（只此一次，quirk 固件禁不起反复打）；
//   · 无进展轮数达到阈值（10 轮 × 500ms = 5s）→ kFailOpen，物理鼠标先用起来。
HidLayoutAction hid_layout_decide(bool iface_ready, bool prefetch_available,
                                  uint32_t class_attempts, uint32_t stall_rounds);

}  // namespace ttbox_usbproxy

#endif  // TTBOX_HIDRAW_SOURCE_H
