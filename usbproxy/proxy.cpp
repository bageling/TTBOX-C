#include <vector>
#include <algorithm>
#include <map>
#include <atomic>
#include <memory>
#include <errno.h>
#include <string.h>

#include "host-raw-gadget.h"
#include "device-libusb.h"
#include "proxy.h"
#include "misc.h"
#include "mouse_control.hpp"
#include "synthetic.h"

#ifdef HAVE_LUA
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

// UVC Video Streaming interface selectors (USB Video Class spec)
#define UVC_VS_PROBE_CONTROL		0x01
#define UVC_VS_COMMIT_CONTROL		0x02
#define UVC_VS_INPUT_HEADER		0x01
#define UVC_SC_VIDEOSTREAMING		0x02

// Offset of dwMaxPayloadTransferSize in UVC probe/commit response
#define UVC_PROBE_MAX_PAYLOAD_OFFSET	22

#define HID_REQ_SET_REPORT		0x09
#define HID_REQ_SET_IDLE		0x0a
#define HID_REQ_SET_PROTOCOL		0x0b
#define HID_DT_REPORT			0x22

extern bool auto_remap_endpoints;
// 定义在 usb-proxy.cpp（A-PATH-5 同族：跨编译单元全局，仅读）
extern bool enable_mouse_control;
extern bool synthetic_mode;

static std::atomic<uint32_t> hid_ready_interfaces{0};
static std::atomic<uint32_t> hid_expected_interfaces{0};

static uint32_t interface_bit(uint8_t interface_number)
{
	if (interface_number >= 32)
		return 0;
	return 1u << interface_number;
}

static uint32_t hid_interface_mask_for_config(int config_index)
{
	uint32_t mask = 0;
	struct raw_gadget_config *config = &host_device_desc.configs[config_index];
	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		struct raw_gadget_altsetting *alt = &iface->altsettings[0];
		if (alt->interface.bInterfaceClass == USB_CLASS_HID)
			mask |= interface_bit(alt->interface.bInterfaceNumber);
	}
	return mask;
}

static void reset_hid_readiness_for_config(int config_index)
{
	if (!hid_passthrough_compat)
		return;
	uint32_t expected = hid_interface_mask_for_config(config_index);
	uint32_t ready = hid_ready_interfaces.load(std::memory_order_acquire) & expected;
	hid_ready_interfaces.store(ready, std::memory_order_release);
	hid_expected_interfaces.store(expected, std::memory_order_release);
	printf("HID passthrough: waiting for report descriptors, interface mask 0x%08x, ready mask 0x%08x\n",
		expected, ready);
}

static void clear_hid_readiness()
{
	if (!hid_passthrough_compat)
		return;
	hid_ready_interfaces.store(0, std::memory_order_release);
	hid_expected_interfaces.store(0, std::memory_order_release);
}

static void mark_hid_report_descriptor_ready(uint8_t interface_number)
{
	if (!hid_passthrough_compat)
		return;
	uint32_t bit = interface_bit(interface_number);
	if (!bit)
		return;
	uint32_t old = hid_ready_interfaces.fetch_or(bit, std::memory_order_acq_rel);
	if ((old & bit) == 0)
		printf("HID passthrough: interface %u report descriptor ready\n",
			interface_number);
}

static bool all_hid_report_descriptors_ready()
{
	if (!hid_passthrough_compat)
		return true;
	uint32_t expected = hid_expected_interfaces.load(std::memory_order_acquire);
	if (!expected)
		return true;
	uint32_t ready = hid_ready_interfaces.load(std::memory_order_acquire);
	return (ready & expected) == expected;
}

static bool should_gate_hid_interrupt_in(const struct thread_info &thread_info)
{
	return hid_passthrough_compat &&
	       thread_info.interface_class == USB_CLASS_HID &&
	       (thread_info.endpoint.bEndpointAddress & USB_DIR_IN) &&
	       usb_endpoint_type(&thread_info.endpoint) == USB_ENDPOINT_XFER_INT &&
	       !all_hid_report_descriptors_ready();
}

static bool is_hid_report_descriptor_request(const struct usb_ctrlrequest *ctrl)
{
	return (ctrl->bRequestType & USB_DIR_IN) &&
	       (ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
	       (ctrl->bRequestType & USB_RECIP_MASK) == USB_RECIP_INTERFACE &&
	       ctrl->bRequest == USB_REQ_GET_DESCRIPTOR &&
	       (ctrl->wValue >> 8) == HID_DT_REPORT;
}

// ── 2026-09-27：报告描述符「主动拉取」兜底（用户反馈「有些鼠标不识别」）──
// 依赖说明：此前布局学习只发生在「主机来要描述符」的嗅探路径上（ep0 转发顺带解析）。
// 若主机枚举时跳过 GET_DESCRIPTOR(Report)（系统描述符缓存 / 私有协议栈 / 复用旧实例），
// mouse_control 永远拿不到布局 ⇒ fail-closed = 该鼠标按键门与注入全废，用户视角就是
// 「盒子不识别这只鼠标」（鼠标本身照常透传）。这里在 SET_CONFIGURATION 落定后，
// 对还没就绪的 HID 接口**直接向物理设备**发一次 GET_DESCRIPTOR(Report) 补课，
// 让布局学习不再依赖主机的行为。
static void active_fetch_hid_report_descriptors(int config_index)
{
	if (!enable_mouse_control || synthetic_mode)
		return;
	if (config_index < 0 || config_index >= host_device_desc.device.bNumConfigurations)
		return;
	struct raw_gadget_config *config = &host_device_desc.configs[config_index];
	for (int i = 0; i < config->config.bNumInterfaces; ++i) {
		struct raw_gadget_altsetting *alt = &config->interfaces[i].altsettings[0];
		if (alt->interface.bInterfaceClass != USB_CLASS_HID)
			continue;
		const uint8_t iface_num = alt->interface.bInterfaceNumber;
		const uint32_t bit = interface_bit(iface_num);
		if (bit && (hid_ready_interfaces.load(std::memory_order_acquire) & bit))
			continue;  // 嗅探路径已经拿到，不重复拉

		struct usb_ctrlrequest req = {};
		req.bRequestType = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
		req.bRequest = USB_REQ_GET_DESCRIPTOR;
		req.wValue = static_cast<uint16_t>(HID_DT_REPORT << 8);
		req.wIndex = iface_num;
		req.wLength = MAX_TRANSFER_SIZE;

		unsigned char data[MAX_TRANSFER_SIZE];
		int nbytes = 0;
		unsigned char *dataptr = data;
		const int rc = control_request(&req, &nbytes, &dataptr, USB_REQUEST_TIMEOUT);
		if (rc == 0 && nbytes > 0) {
			ttbox_usbproxy::mouse_control_set_report_descriptor(
				iface_num, data, static_cast<uint32_t>(nbytes));
			mark_hid_report_descriptor_ready(iface_num);
			printf("ep0: active fetch: iface %u report descriptor %d bytes\n",
			       iface_num, nbytes);
		} else {
			printf("ep0: active fetch: iface %u report descriptor failed rc=%d\n",
			       iface_num, rc);
		}
	}
}

static void log_ep0_ack_failed(int rv)
{
	printf("ep0: ack failed: %d (%s)\n", rv, strerror(errno));
}

static bool should_ack_hid_setup_without_forwarding(const struct usb_ctrlrequest *ctrl,
						    bool set_configuration_status_succeeded)
{
	if (!hid_passthrough_compat)
		return false;
	if (!set_configuration_status_succeeded)
		return false;
	if (ctrl->bRequestType & USB_DIR_IN)
		return false;
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS ||
	    (ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE)
		return false;

	// Windows often sends these during HID driver startup. Some Logitech
	// devices stall them when proxied through libusb, but Windows treats the
	// stall as a broken "USB Input Device" child even though interrupt reports
	// can still move the mouse.
	return ctrl->wLength == 0 &&
	       (ctrl->bRequest == HID_REQ_SET_IDLE ||
		ctrl->bRequest == HID_REQ_SET_PROTOCOL);
}

static uint16_t find_udc_maxpacket_for_interface(uint8_t interface_number)
{
	struct raw_gadget_config *config =
		&host_device_desc.configs[host_device_desc.current_config];
	uint16_t max_limit = 0;

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			if (alt->interface.bInterfaceNumber != interface_number)
				continue;
			for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
				struct raw_gadget_endpoint *ep = &alt->endpoints[k];
				if (usb_endpoint_type(&ep->endpoint) != USB_ENDPOINT_XFER_ISOC)
					continue;
				if (ep->udc_maxpacket_limit &&
				    ep->udc_maxpacket_limit > max_limit)
					max_limit = ep->udc_maxpacket_limit;
			}
		}
	}

	return max_limit;
}

// Translate a gadget-side endpoint address back to the physical device's
// endpoint address. Needed for endpoint-directed class requests (e.g.,
// USB Audio SET_CUR for sampling frequency) when endpoint remapping is active.
static uint16_t remap_endpoint_for_device(uint16_t gadget_ep_addr)
{
	if (!auto_remap_endpoints)
		return gadget_ep_addr;

	struct raw_gadget_config *config =
		&host_device_desc.configs[host_device_desc.current_config];

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
				struct raw_gadget_endpoint *ep = &alt->endpoints[k];
				if (ep->endpoint.bEndpointAddress == (uint8_t)gadget_ep_addr)
					return ep->device_bEndpointAddress;
			}
		}
	}

	return gadget_ep_addr;
}

static void clamp_uvc_probe_commit(const usb_ctrlrequest *ctrl,
				   struct usb_raw_transfer_io &io)
{
	if (!auto_remap_endpoints)
		return;
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS)
		return;

	uint8_t interface_number = ctrl->wIndex & 0xff;
	uint8_t selector = ctrl->wValue >> 8;
	if (selector != UVC_VS_PROBE_CONTROL && selector != UVC_VS_COMMIT_CONTROL)
		return;

	uint16_t maxp = find_udc_maxpacket_for_interface(interface_number);
	if (!maxp)
		return;

	if (io.inner.length < UVC_PROBE_MAX_PAYLOAD_OFFSET + 4)
		return;

	uint8_t *payload = (uint8_t *)io.data;
	uint8_t *p = payload + UVC_PROBE_MAX_PAYLOAD_OFFSET;
	uint32_t max_payload = (uint32_t)p[0] |
		((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) |
		((uint32_t)p[3] << 24);
	if (max_payload > maxp) {
		p[0] = maxp & 0xff;
		p[1] = (maxp >> 8) & 0xff;
		p[2] = 0;
		p[3] = 0;
	}
}

static struct raw_gadget_altsetting *find_altsetting(struct raw_gadget_config *config,
						    uint8_t interface_number,
						    uint8_t alt_setting)
{
	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			if (alt->interface.bInterfaceNumber == interface_number &&
			    alt->interface.bAlternateSetting == alt_setting)
				return alt;
		}
	}
	return NULL;
}

static struct raw_gadget_endpoint *find_first_streaming_ep(struct raw_gadget_config *config,
							  uint8_t interface_number)
{
	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			if (alt->interface.bInterfaceNumber != interface_number)
				continue;
			if (alt->interface.bNumEndpoints > 0)
				return &alt->endpoints[0];
		}
	}
	return NULL;
}

static void rewrite_descriptor_addresses(uint8_t descriptor_type, uint8_t descriptor_index,
					 uint8_t *data, size_t length)
{
	if (!auto_remap_endpoints)
		return;

	if (descriptor_type != USB_DT_CONFIG &&
	    descriptor_type != USB_DT_OTHER_SPEED_CONFIG)
		return;

	if (descriptor_index >= host_device_desc.device.bNumConfigurations)
		return;

	struct raw_gadget_config *config = &host_device_desc.configs[descriptor_index];
	struct raw_gadget_altsetting *current_alt = NULL;
	int current_endpoint = 0;

	size_t offset = 0;
	while (offset + 2 <= length) {
		uint8_t dlen = data[offset];
		if (!dlen)
			break;
		if (offset + dlen > length)
			break;

		uint8_t dtype = data[offset + 1];
		if (dtype == USB_DT_INTERFACE) {
			uint8_t interface_number = data[offset + 2];
			uint8_t alt_setting = data[offset + 3];
			current_alt = find_altsetting(config, interface_number, alt_setting);
			current_endpoint = 0;
		}
		else if (dtype == USB_DT_ENDPOINT) {
			if (current_alt && current_endpoint < current_alt->interface.bNumEndpoints) {
				struct raw_gadget_endpoint *ep = &current_alt->endpoints[current_endpoint];
				data[offset + 2] = ep->endpoint.bEndpointAddress;
				if (offset + 6 < length) {
					uint16_t maxp = ep->endpoint.wMaxPacketSize;
					data[offset + 4] = maxp & 0xff;
					data[offset + 5] = (maxp >> 8) & 0xff;
				}
				// Also rewrite bInterval (offset 6 in endpoint descriptor).
				if (offset + 7 <= length)
					data[offset + 6] = ep->endpoint.bInterval;
				current_endpoint++;
			}
		}
		else if (dtype == USB_DT_CS_INTERFACE) {
			if (current_alt && current_alt->interface.bInterfaceClass == USB_CLASS_VIDEO) {
				uint8_t subtype = data[offset + 2];
				// VideoStreaming Input Header descriptor: bEndpointAddress at offset 6.
				if (subtype == UVC_VS_INPUT_HEADER &&
				    current_alt->interface.bInterfaceSubClass == UVC_SC_VIDEOSTREAMING) {
					if (offset + 6 < length) {
						struct raw_gadget_endpoint *ep = NULL;
						if (current_alt->interface.bNumEndpoints > 0) {
							ep = &current_alt->endpoints[0];
						}
						else {
							ep = find_first_streaming_ep(config,
								current_alt->interface.bInterfaceNumber);
						}
						if (ep)
							data[offset + 6] = ep->endpoint.bEndpointAddress;
					}
				}
			}
		}

		offset += dlen;
	}
}

static void maybe_override_descriptor(struct usb_ctrlrequest *ctrl,
				      struct usb_raw_transfer_io &io)
{
	if (!auto_remap_endpoints)
		return;
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD)
		return;
	if (ctrl->bRequest != USB_REQ_GET_DESCRIPTOR)
		return;

	uint8_t descriptor_type = ctrl->wValue >> 8;
	uint8_t descriptor_index = ctrl->wValue & 0xff;
	rewrite_descriptor_addresses(descriptor_type, descriptor_index,
				     (uint8_t *)io.data, io.inner.length);
}

// Returns the index of the best altsetting that fits UDC limits, or -1 if none.
// The desired_altsetting is returned directly if remapping is disabled or if it has no endpoints.
static int find_best_compatible_altsetting(struct raw_gadget_interface *iface,
					   int desired_interface,
					   int desired_altsetting)
{
	if (!auto_remap_endpoints)
		return desired_altsetting;

	struct raw_gadget_altsetting *desired_alt = &iface->altsettings[desired_altsetting];
	if (desired_alt->interface.bNumEndpoints == 0) {
		// Alt 0 (no endpoints) is usually the idle state; never remap it.
		return desired_altsetting;
	}

	const struct libusb_interface_descriptor *alts =
		device_config_desc[host_device_desc.current_config]
			->interface[desired_interface].altsetting;

	int best_alt = -1;
	int best_packet = -1;

	for (int i = 0; i < iface->num_altsettings; i++) {
		struct raw_gadget_altsetting *alt = &iface->altsettings[i];
		if (alt->interface.bNumEndpoints == 0)
			continue;

		bool fits = true;
		int alt_packet = 0;

		for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
			struct raw_gadget_endpoint *ep = &alt->endpoints[k];
			if (usb_endpoint_type(&ep->endpoint) != USB_ENDPOINT_XFER_ISOC)
				continue;

			uint16_t udc_limit = ep->udc_maxpacket_limit;
			if (!udc_limit)
				continue;

			uint16_t dev_maxp = alts[i].endpoint[k].wMaxPacketSize & 0x7ff;
			if (dev_maxp > udc_limit) {
				fits = false;
				break;
			}
			if (dev_maxp > alt_packet)
				alt_packet = dev_maxp;
		}

		if (fits && alt_packet >= best_packet) {
			best_packet = alt_packet;
			best_alt = i;
		}
	}

	return best_alt;
}

// ── Approach 1: declarative per-byte operations ──────────────────────────────
//
// Applies the "operations" array from an injection rule to the packet in-place.
// Operations are applied in order. Offsets are 0-based.
//
// Supported types (size=1 is int8 default, size=2 is int16 LE):
//   negate  { offset [, size] }           – two's-complement negate signed value
//   scale   { offset, factor [, size] }   – multiply by float, clamp to range
//   add     { offset, value  [, size] }   – add signed constant, clamp to range
//   clamp   { offset, min, max [, size] } – clamp signed value to [min, max]
//   xor     { offset, mask }              – XOR byte with mask (integer)
//   swap    { offset, offset_b }          – swap two bytes
//   copy    { offset, dst_offset }        – copy byte to another position
//   set     { offset, value }             – force byte to unsigned value 0-255
//
static void apply_operations(uint8_t *data, int len, const Json::Value &ops)
{
	for (unsigned int i = 0; i < ops.size(); i++) {
		const Json::Value &op = ops[i];
		std::string type = op.get("type", "").asString();
		int offset = op.get("offset", -1).asInt();

		if (type == "negate") {
			if (offset < 0) continue;
			int size = op.get("size", 1).asInt();
			if (size == 2) {
				if (offset + 1 >= len) continue;
				int32_t v = (int32_t)(int16_t)((uint16_t)data[offset] |
				                               ((uint16_t)data[offset + 1] << 8));
				v = -v;
				if (v < -32768) v = -32768;
				if (v > 32767)  v = 32767;
				uint16_t uv = (uint16_t)(int16_t)v;
				data[offset]     = (uint8_t)(uv & 0xFF);
				data[offset + 1] = (uint8_t)(uv >> 8);
			} else {
				if (offset >= len) continue;
				data[offset] = (uint8_t)(-(int8_t)data[offset]);
			}

		} else if (type == "scale") {
			if (offset < 0) continue;
			double factor = op.get("factor", 1.0).asDouble();
			int size = op.get("size", 1).asInt();
			if (size == 2) {
				if (offset + 1 >= len) continue;
				int32_t v = (int32_t)(int16_t)((uint16_t)data[offset] |
				                               ((uint16_t)data[offset + 1] << 8));
				v = (int32_t)(v * factor);
				if (v < -32768) v = -32768;
				if (v > 32767)  v = 32767;
				uint16_t uv = (uint16_t)(int16_t)v;
				data[offset]     = (uint8_t)(uv & 0xFF);
				data[offset + 1] = (uint8_t)(uv >> 8);
			} else {
				if (offset >= len) continue;
				int result = (int)((int8_t)data[offset] * factor);
				result = std::max(-128, std::min(127, result));
				data[offset] = (uint8_t)(int8_t)result;
			}

		} else if (type == "add") {
			if (offset < 0) continue;
			int size = op.get("size", 1).asInt();
			if (size == 2) {
				if (offset + 1 >= len) continue;
				int32_t v = (int32_t)(int16_t)((uint16_t)data[offset] |
				                               ((uint16_t)data[offset + 1] << 8));
				v += op.get("value", 0).asInt();
				if (v < -32768) v = -32768;
				if (v > 32767)  v = 32767;
				uint16_t uv = (uint16_t)(int16_t)v;
				data[offset]     = (uint8_t)(uv & 0xFF);
				data[offset + 1] = (uint8_t)(uv >> 8);
			} else {
				if (offset >= len) continue;
				int result = (int)(int8_t)data[offset] + op.get("value", 0).asInt();
				result = std::max(-128, std::min(127, result));
				data[offset] = (uint8_t)(int8_t)result;
			}

		} else if (type == "clamp") {
			if (offset < 0) continue;
			int size = op.get("size", 1).asInt();
			if (size == 2) {
				if (offset + 1 >= len) continue;
				int32_t v = (int32_t)(int16_t)((uint16_t)data[offset] |
				                               ((uint16_t)data[offset + 1] << 8));
				int min_v = op.get("min", -32768).asInt();
				int max_v = op.get("max",  32767).asInt();
				v = std::max(min_v, std::min(max_v, (int)v));
				uint16_t uv = (uint16_t)(int16_t)v;
				data[offset]     = (uint8_t)(uv & 0xFF);
				data[offset + 1] = (uint8_t)(uv >> 8);
			} else {
				if (offset >= len) continue;
				int min_v = op.get("min", -128).asInt();
				int max_v = op.get("max",  127).asInt();
				int result = std::max(min_v, std::min(max_v, (int)(int8_t)data[offset]));
				data[offset] = (uint8_t)(int8_t)result;
			}

		} else if (type == "xor") {
			if (offset < 0 || offset >= len) continue;
			data[offset] ^= (uint8_t)op.get("mask", 0).asInt();

		} else if (type == "swap") {
			int b = op.get("offset_b", -1).asInt();
			if (offset < 0 || offset >= len) continue;
			if (b < 0 || b >= len) continue;
			uint8_t tmp = data[offset];
			data[offset] = data[b];
			data[b] = tmp;

		} else if (type == "copy") {
			int dst = op.get("dst_offset", -1).asInt();
			if (offset < 0 || offset >= len) continue;
			if (dst < 0 || dst >= len) continue;
			data[dst] = data[offset];

		} else if (type == "set") {
			if (offset < 0 || offset >= len) continue;
			data[offset] = (uint8_t)op.get("value", 0).asInt();

		} else {
			printf("apply_operations: unknown op type '%s'\n", type.c_str());
		}
	}
}

// ── Approach 2: Lua scripting ─────────────────────────────────────────────────
//
// Each unique script_file gets one lua_State loaded on first use, protected
// by a per-state mutex (Lua states are not thread-safe).
//
// The script must export:
//   function transform(data, len)  →  data, new_len
//
// where `data` is a 1-indexed Lua table of byte values (0-255),
// `len` is the original packet length, and the function returns the
// (possibly modified) table and the new length.
//
#ifdef HAVE_LUA
struct LuaRuleState {
	lua_State *L = nullptr;
	std::mutex call_mutex;
};

static std::mutex                          lua_registry_mutex;
static std::map<std::string, LuaRuleState *> lua_states;

static LuaRuleState *get_lua_state(const std::string &script_file)
{
	std::lock_guard<std::mutex> guard(lua_registry_mutex);
	auto it = lua_states.find(script_file);
	if (it != lua_states.end())
		return it->second;

	auto *state = new LuaRuleState();
	state->L = luaL_newstate();
	luaL_openlibs(state->L);
	if (luaL_dofile(state->L, script_file.c_str()) != LUA_OK) {
		fprintf(stderr, "Lua: failed to load '%s': %s\n",
			script_file.c_str(), lua_tostring(state->L, -1));
		lua_close(state->L);
		state->L = nullptr;
	} else {
		printf("Lua: loaded '%s'\n", script_file.c_str());
	}
	lua_states[script_file] = state;
	return state;
}

static bool apply_lua_transform(const std::string &script_file,
				uint8_t *data, int &len)
{
	LuaRuleState *state = get_lua_state(script_file);
	if (!state || !state->L)
		return false;

	std::lock_guard<std::mutex> guard(state->call_mutex);
	lua_State *L = state->L;

	lua_getglobal(L, "transform");
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "Lua: '%s' has no 'transform' function\n",
			script_file.c_str());
		lua_pop(L, 1);
		return false;
	}

	// Build 1-indexed Lua table from packet bytes
	lua_newtable(L);
	for (int i = 0; i < len; i++) {
		lua_pushinteger(L, i + 1);
		lua_pushinteger(L, data[i]);
		lua_rawset(L, -3);
	}
	lua_pushinteger(L, len);

	// Call transform(data, len) → data, new_len
	if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
		fprintf(stderr, "Lua: transform error in '%s': %s\n",
			script_file.c_str(), lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}

	// Second return value: new length
	if (!lua_isnumber(L, -1)) {
		fprintf(stderr, "Lua: '%s' transform must return (table, integer)\n",
			script_file.c_str());
		lua_pop(L, 2);
		return false;
	}
	int new_len = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);

	// First return value: modified byte table
	if (!lua_istable(L, -1)) {
		fprintf(stderr, "Lua: '%s' transform must return (table, integer)\n",
			script_file.c_str());
		lua_pop(L, 1);
		return false;
	}
	new_len = std::min(new_len, MAX_TRANSFER_SIZE);
	for (int i = 0; i < new_len; i++) {
		lua_pushinteger(L, i + 1);
		lua_rawget(L, -2);
		data[i] = (uint8_t)(lua_tointeger(L, -1) & 0xFF);
		lua_pop(L, 1);
	}
	lua_pop(L, 1); // pop table

	len = new_len;
	return true;
}
#endif // HAVE_LUA

// Apply the 3-step injection pipeline (pattern+replace, operations, Lua)
// to the transfer buffer.  Returns true if anything was modified.
static bool apply_injection_pipeline(struct usb_raw_transfer_io &io,
				     const Json::Value &rule)
{
	bool modified = false;

	// Step 1: pattern match + replacement
	if (rule.isMember("content_pattern") && rule.isMember("replacement")) {
		Json::Value patterns = rule["content_pattern"];
		std::string replacement_hex = rule["replacement"].asString();
		if (patterns.size() > 0 && !replacement_hex.empty()) {
			std::string data(io.data, io.inner.length);
			std::string replacement = hexToAscii(replacement_hex);
			for (unsigned int j = 0; j < patterns.size(); j++) {
				std::string pattern_hex = patterns[j].asString();
				std::string pattern = hexToAscii(pattern_hex);

				std::string::size_type pos = data.find(pattern);
				while (pos != std::string::npos) {
					if (data.length() - pattern.length() + replacement.length() > 1023)
						break;
					data = data.replace(pos, pattern.length(), replacement);
					printf("Modified from %s to %s at Index %ld\n",
						pattern_hex.c_str(), replacement_hex.c_str(), pos);
					modified = true;
					pos = data.find(pattern);
				}
			}
			if (modified) {
				io.inner.length = data.length();
				for (size_t j = 0; j < data.length(); j++)
					io.data[j] = data[j];
			}
		}
	}

	// Step 2: declarative operations
	if (rule.isMember("operations") && rule["operations"].size() > 0) {
		apply_operations(reinterpret_cast<uint8_t *>(io.data),
				 (int)io.inner.length,
				 rule["operations"]);
		modified = true;
	}

	// Step 3: Lua transform
#ifdef HAVE_LUA
	if (rule.isMember("script_file")) {
		int len = (int)io.inner.length;
		if (apply_lua_transform(rule["script_file"].asString(),
					reinterpret_cast<uint8_t *>(io.data),
					len)) {
			io.inner.length = (__u32)len;
			modified = true;
		}
	}
#endif

	return modified;
}

// ─────────────────────────────────────────────────────────────────────────────

void injection(struct usb_raw_control_event &event, struct usb_raw_transfer_io &io, int &injection_flags) {
	const std::vector<std::string> injection_type{"modify", "ignore", "stall"};

	for (unsigned int i = 0; i < injection_type.size(); i++) {
		for (unsigned int j = 0; j < injection_config["control"][injection_type[i]].size(); j++) {
			Json::Value rule = injection_config["control"][injection_type[i]][j];
			if (!rule["enable"].asBool())
				continue;

			if (event.ctrl.bRequestType != hexToDecimal(rule["bRequestType"].asInt()) ||
			    event.ctrl.bRequest     != hexToDecimal(rule["bRequest"].asInt()) ||
			    event.ctrl.wValue       != hexToDecimal(rule["wValue"].asInt()) ||
			    event.ctrl.wIndex       != hexToDecimal(rule["wIndex"].asInt()) ||
			    event.ctrl.wLength      != hexToDecimal(rule["wLength"].asInt()))
				continue;

			printf("Matched injection rule: %s, index: %d\n", injection_type[i].c_str(), j);
			if (injection_type[i] == "modify") {
				apply_injection_pipeline(io, rule);
				if (!(event.ctrl.bRequestType & USB_DIR_IN))
					event.ctrl.wLength = io.inner.length;
			}
			else if (injection_type[i] == "ignore") {
				printf("Ignore this control transfer\n");
				injection_flags = USB_INJECTION_FLAG_IGNORE;
			}
			else if (injection_type[i] == "stall") {
				injection_flags = USB_INJECTION_FLAG_STALL;
			}
		}
	}
}

void injection(struct usb_raw_transfer_io &io, __u8 device_ep_address, std::string transfer_type) {
	for (unsigned int i = 0; i < injection_config[transfer_type].size(); i++) {
		Json::Value rule = injection_config[transfer_type][i];
		if (!rule["enable"].asBool() ||
		    hexToDecimal(rule["ep_address"].asInt()) != device_ep_address)
			continue;

		// Snapshot for before/after logging (copy only incurred when verbose)
		uint32_t orig_len = io.inner.length;
		uint8_t orig_data[MAX_TRANSFER_SIZE];
		if (verbose_level >= 1)
			memcpy(orig_data, io.data, orig_len);

		if (apply_injection_pipeline(io, rule)) {
			if (verbose_level >= 1) {
				printf("Injection[%s EP%02x] before:", transfer_type.c_str(), device_ep_address);
				for (uint32_t j = 0; j < orig_len; j++)
					printf(" %02x", orig_data[j]);
				printf("\n");
				printf("Injection[%s EP%02x] after: ", transfer_type.c_str(), device_ep_address);
				for (uint32_t j = 0; j < io.inner.length; j++)
					printf(" %02x", (uint8_t)io.data[j]);
				printf("\n");
			}
			break;
		}
	}
}

void printData(struct usb_raw_transfer_io io, __u8 bEndpointAddress, std::string transfer_type, std::string dir) {
	printf("Sending data to EP%x(%s_%s):", bEndpointAddress,
		transfer_type.c_str(), dir.c_str());
	for (unsigned int i = 0; i < io.inner.length; i++) {
		printf(" %02hhx", (unsigned)io.data[i]);
	}
	printf("\n");
}

void noop_signal_handler(int) { }

// Multiple endpoint threads can observe the same physical disconnect at the
// same time. Send the cleanup signal only once; otherwise the second SIGINT
// takes the "force exiting" path before main() can release the UDC cleanly.
// ★ 2026-09-26：去掉 static 并在 proxy.h 声明 —— device-libusb.cpp 的 hotplug
//   回调此前无守卫地 kill(0, SIGINT)，与端点线程的断连路径并发时第二发命中
//   usb-proxy.cpp 的 "force exiting" 强退分支，跳过 UDC/接口清理。
std::atomic<bool> proxy_stop_requested{false};

void stop_proxy_after_physical_disconnect()
{
	if (proxy_stop_requested.exchange(true, std::memory_order_acq_rel))
		return;
	please_stop_ep0 = true;
	please_stop_eps = true;
	kill(0, SIGINT);
}

void *ep_loop_write(void *arg) {
	struct thread_info thread_info = *((struct thread_info*) arg);
	int fd = thread_info.fd;
	int ep_num = thread_info.ep_num;
	struct usb_endpoint_descriptor ep = thread_info.endpoint;
	std::string transfer_type = thread_info.transfer_type;
	std::string dir = thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = thread_info.data_queue;
	std::mutex *data_mutex = thread_info.data_mutex;
	std::atomic<bool> *please_stop = thread_info.please_stop;

	printf("Start writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	usbproxy_rt_apply("ENDPOINT", 60, "USB_PROXY_CPU_AFFINITY", -1);

	// Set a no-op handler for SIGUSR1. Sending this signal to the thread
	// will thus interrupt a blocking ioctl call without other side-effects.
	signal(SIGUSR1, noop_signal_handler);

	// Check both per-endpoint flag (interface change) and global flag (device reset)
	bool hid_gate_logged = false;
	while (!*please_stop && !please_stop_eps) {
		assert(ep_num != -1);

		if (should_gate_hid_interrupt_in(thread_info)) {
			if (!hid_gate_logged) {
				printf("EP%02x(%s_%s): waiting for all HID report descriptors before writing reports\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				hid_gate_logged = true;
			}
			usleep(1000);
			continue;
		}
		hid_gate_logged = false;

		data_mutex->lock();
		if (data_queue->empty()) {
			data_mutex->unlock();
			usleep(100);
			continue;
		}
		struct usb_raw_transfer_io io = data_queue->front();
		data_queue->pop_front();
		data_mutex->unlock();

		if (verbose_level >= 2)
			printData(io, ep.bEndpointAddress, transfer_type, dir);

		if (ep.bEndpointAddress & USB_DIR_IN) {
			int rv = usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
			if (rv < 0 && errno == ESHUTDOWN) {
				printf("EP%x(%s_%s): device likely reset, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && errno == EINTR) {
				printf("EP%x(%s_%s): interface likely changing, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && (errno == EXDEV || errno == ENODATA || errno == EOVERFLOW)) {
				printf("EP%x(%s_%s): isochronous timing error on write (errno=%d), ignoring transfer\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str(), errno);
				continue;
			}
			if (rv < 0) {
				perror("usb_raw_ep_write()");
				exit(EXIT_FAILURE);
			}
			if (verbose_level)
				printf("EP%x(%s_%s): wrote %d bytes to host\n", ep.bEndpointAddress,
					transfer_type.c_str(), dir.c_str(), rv);
		}
		else {
			int length = io.inner.length;
			unsigned char *data = new unsigned char[length];
			memcpy(data, io.data, length);

			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_ISOC) {
				// Mirror the ISO IN read path: call the dedicated ISO function
				// directly rather than going through the send_data() dispatcher.
				// On success the async callback owns and frees the buffer.
				int rv = send_iso_data(thread_info.device_bEndpointAddress,
						       data, length, USB_REQUEST_TIMEOUT);
					if (rv == LIBUSB_ERROR_NO_DEVICE) {
						delete[] data;
						printf("EP%x(%s_%s): device likely reset, stopping thread\n",
							ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
						stop_proxy_after_physical_disconnect();
						break;
					}
				if (rv != LIBUSB_SUCCESS)
					delete[] data;
			} else {
				int rv = send_data(thread_info.device_bEndpointAddress, ep.bmAttributes,
						   data, length, USB_REQUEST_TIMEOUT);
					if (rv == LIBUSB_ERROR_NO_DEVICE) {
						delete[] data;
						printf("EP%x(%s_%s): device likely reset, stopping thread\n",
							ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
						stop_proxy_after_physical_disconnect();
						break;
					}
				delete[] data;
			}
		}
	}

	printf("End writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void *ep_loop_read(void *arg) {
	struct thread_info thread_info = *((struct thread_info*) arg);
	int fd = thread_info.fd;
	int ep_num = thread_info.ep_num;
	struct usb_endpoint_descriptor ep = thread_info.endpoint;
	std::string transfer_type = thread_info.transfer_type;
	std::string dir = thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = thread_info.data_queue;
	std::mutex *data_mutex = thread_info.data_mutex;
	std::atomic<bool> *please_stop = thread_info.please_stop;

	printf("Start reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	// 端点转发线程上实时调度（SCHED_FIFO）+ 可选绑核，降低转发路径抖动。
	// 默认 60；环境变量 USB_PROXY_ENDPOINT_RT_PRIORITY=0 可关（排障第一步）。
	usbproxy_rt_apply("ENDPOINT", 60, "USB_PROXY_CPU_AFFINITY", -1);

	// Set a no-op handler for SIGUSR1. Sending this signal to the thread
	// will thus interrupt a blocking ioctl call without other side-effects.
	signal(SIGUSR1, noop_signal_handler);

	// Check both per-endpoint flag (interface change) and global flag (device reset)
	bool hid_gate_logged = false;
	int consecutive_receive_errors = 0;

	// One ring per endpoint: each endpoint owns its reader thread.  Created
	// lazily below, after the HID report-descriptor gate opens, so we never
	// buffer reports that would then be flushed as one stale burst.
	bool use_iring = (ep.bEndpointAddress & USB_DIR_IN) &&
		((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT);
	struct interrupt_ring *iring = NULL;

	while (!*please_stop && !please_stop_eps) {
		assert(ep_num != -1);
		struct usb_raw_transfer_io io;

		if (ep.bEndpointAddress & USB_DIR_IN) {
			if (should_gate_hid_interrupt_in(thread_info)) {
				if (!hid_gate_logged) {
					printf("EP%02x(%s_%s): waiting for all HID report descriptors before reading reports\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
					hid_gate_logged = true;
				}
				usleep(1000);
				continue;
			}
			hid_gate_logged = false;

			data_mutex->lock();
			bool queue_full = data_queue->size() >= 32;
			data_mutex->unlock();
			if (queue_full) {
				usleep(200);
				continue;
			}

			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_ISOC) {
				struct iso_batch_result batch;
				int rv = receive_iso_data_batched(thread_info.device_bEndpointAddress,
								usb_endpoint_maxp(&ep),
								&batch, iso_batch_size, USB_REQUEST_TIMEOUT);
					if (rv == LIBUSB_ERROR_NO_DEVICE) {
						printf("EP%x(%s_%s): device likely reset, stopping thread\n",
							ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
						stop_proxy_after_physical_disconnect();
						break;
					}
					if (rv != LIBUSB_SUCCESS && rv != LIBUSB_ERROR_TIMEOUT) {
						consecutive_receive_errors++;
						if (consecutive_receive_errors >= 20) {
							printf("EP%x(%s_%s): repeated receive errors, restarting proxy\n",
								ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
							if (batch.buffer)
								delete[] batch.buffer;
							stop_proxy_after_physical_disconnect();
							break;
						}
					}
					else if (rv == LIBUSB_SUCCESS) {
						consecutive_receive_errors = 0;
					}

					if (rv != LIBUSB_SUCCESS || !batch.success) {
						if (batch.buffer)
						delete[] batch.buffer;
					continue;
				}

				int packets_enqueued = 0;
				for (int i = 0; i < batch.num_packets; i++) {
					if (batch.packets[i].status != LIBUSB_TRANSFER_COMPLETED) {
						if (verbose_level > 1)
							printf("EP%x(%s_%s): packet %d status %d, skipping\n",
								ep.bEndpointAddress, transfer_type.c_str(),
								dir.c_str(), i, batch.packets[i].status);
						continue;
					}
					if (batch.packets[i].actual_length <= 0)
						continue;

					memcpy(io.data, batch.packets[i].data, batch.packets[i].actual_length);
					io.inner.ep = ep_num;
					io.inner.flags = 0;
					io.inner.length = batch.packets[i].actual_length;

					if (injection_enabled)
						injection(io, thread_info.device_bEndpointAddress, transfer_type);

					data_mutex->lock();
					data_queue->push_back(io);
					data_mutex->unlock();
					packets_enqueued++;
				}
				if (verbose_level)
					printf("EP%x(%s_%s): enqueued %d/%d packets (%d bytes total)\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str(),
						packets_enqueued, batch.num_packets, batch.total_length);

				if (batch.buffer)
					delete[] batch.buffer;
			}
			else {
				// Non-isochronous: use original single-packet path
				unsigned char *data = NULL;
				int nbytes = -1;

				// Interrupt IN: keep several transfers in flight.  With a
				// single in-flight URB the host controller misses every
				// other frame and the report rate is halved (500 Hz
				// measured vs 1000 Hz with 2+ transfers in flight).
				if (use_iring && !iring) {
					iring = interrupt_ring_create(thread_info.device_bEndpointAddress,
								      usb_endpoint_maxp(&ep), INT_RING_DEPTH);
					if (!iring)
						use_iring = false;	/* fall back to the sync path */
				}

				int rv;
				if (iring)
					rv = interrupt_ring_next(iring, &data, &nbytes);
				else
					rv = receive_data(thread_info.device_bEndpointAddress, ep.bmAttributes,
								usb_endpoint_maxp(&ep),
								&data, &nbytes, USB_REQUEST_TIMEOUT);
						if (rv == LIBUSB_ERROR_NO_DEVICE) {
							printf("EP%x(%s_%s): device likely reset, stopping thread\n",
								ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
							if (data)
								delete[] data;
							stop_proxy_after_physical_disconnect();
							break;
						}
						if (rv == LIBUSB_ERROR_INTERRUPTED) {
							/* the ring is being torn down */
							if (data)
								delete[] data;
							break;
						}
					if (rv != LIBUSB_SUCCESS && rv != LIBUSB_ERROR_TIMEOUT) {
						consecutive_receive_errors++;
						if (consecutive_receive_errors >= 20) {
							printf("EP%x(%s_%s): repeated receive errors, restarting proxy\n",
								ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
							if (data)
								delete[] data;
							stop_proxy_after_physical_disconnect();
							break;
						}
					}
					else if (rv == LIBUSB_SUCCESS) {
						consecutive_receive_errors = 0;
					}

						if (rv == LIBUSB_SUCCESS && nbytes > 0) {
							memcpy(io.data, data, nbytes);
							io.inner.ep = ep_num;
							io.inner.flags = 0;
							io.inner.length = nbytes;

							if (injection_enabled)
								injection(io, thread_info.device_bEndpointAddress, transfer_type);

							// ── mouse_control：AI 位移"搭车"合并 + 物理按钮事件 ──
							// 接口号必须传：报告布局是**按接口**解析的
							// （同一 dongle 的键盘/鼠标/厂商接口描述符各不相同），
							// 传 0 或写死接口号都会把键盘报告当成鼠标改。详见 mouse_control.hpp
							ttbox_usbproxy::mouse_control_notify_physical_report(
								thread_info.interface_number,
								reinterpret_cast<const uint8_t*>(io.data), io.inner.length);
							ttbox_usbproxy::mouse_control_merge_report(
								thread_info.interface_number,
								reinterpret_cast<uint8_t*>(io.data), io.inner.length);

							data_mutex->lock();
							data_queue->push_back(io);
							data_mutex->unlock();
							if (verbose_level)
								printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
										transfer_type.c_str(), dir.c_str(), nbytes);
							}

				if (data)
					delete[] data;
			}
		}
		else {
			io.inner.ep = ep_num;
			io.inner.flags = 0;
			// For ISO OUT, limit the buffer to one packet (wMaxPacketSize).
			// Passing a larger buffer (e.g. 4096) causes musb-hdrc to report
			// req->actual = req->length instead of the real frame size, which
			// then triggers EMSGSIZE (-90) when forwarding to the physical device.
			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_ISOC)
				io.inner.length = usb_endpoint_maxp(&ep);
			else
				io.inner.length = sizeof(io.data);

			int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
			if (rv < 0 && errno == ESHUTDOWN) {
				printf("EP%x(%s_%s): device likely reset, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && errno == EINTR) {
				printf("EP%x(%s_%s): interface likely changing, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && (errno == EXDEV || errno == ENODATA || errno == EOVERFLOW)) {
				if (verbose_level)
					printf("EP%x(%s_%s): isochronous timing error on read (errno=%d), continuing\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str(), errno);
				continue;
			}
			if (rv < 0) {
				perror("usb_raw_ep_read()");
				exit(EXIT_FAILURE);
			}
			if (verbose_level)
				printf("EP%x(%s_%s): read %d bytes from host\n", ep.bEndpointAddress,
						transfer_type.c_str(), dir.c_str(), rv);
			io.inner.length = rv;

			if (injection_enabled)
				injection(io, thread_info.device_bEndpointAddress, transfer_type);

			data_mutex->lock();
			data_queue->push_back(io);
			data_mutex->unlock();
			if (verbose_level)
				printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
						transfer_type.c_str(), dir.c_str(), rv);
		}
	}

	if (iring)
		interrupt_ring_destroy(iring);

	printf("End reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

// ── AI 位移注入出口（mouse_control 的 1ms 节拍线程调用）──────────────
//
// 为什么要有它（2026-09-24 板端实测）：物理报告只在"用户在动鼠标"时才来，
// 现场那只 dongle 静止时 IN 端点报告率 0/s ⇒ 只靠把 AI 位移搭在物理报告上，
// 手一停准星就完全不动、手一动又把积压一次性抖出来。所以给 AI 位移一条**自己的出路**：
// 由 mouse_control 按 1ms 节拍构造报告，经这里推进该接口 IN 端点的队列 —— 与物理报告
// 走同一条出主机的路（同一队列、同一个 ep），主机侧看到的仍是一个普通的 HID 鼠标。
struct InjectSinkCtx {
	std::deque<usb_raw_transfer_io>	*queue = NULL;
	std::mutex			*mutex = NULL;
	int				ep_num = -1;
	uint8_t				iface = 0xFF;
};

static struct InjectSinkCtx g_inject_ctx[8];

static bool inject_sink_push(void *user, uint8_t interface_number,
			     const uint8_t *data, uint32_t len)
{
	(void)interface_number;
	struct InjectSinkCtx *c = (struct InjectSinkCtx *)user;
	if (!c || !c->queue || !c->mutex) return false;
	if (!data || len == 0 || len > MAX_TRANSFER_SIZE) return false;

	struct usb_raw_transfer_io io;
	memcpy(io.data, data, len);
	io.inner.ep = c->ep_num;
	io.inner.flags = 0;
	io.inner.length = len;

	std::lock_guard<std::mutex> lk(*c->mutex);
	// 与物理路径同一个上限：队列满就返回 false，让调用方把这一拍退回挂起量、下一拍再投
	if (c->queue->size() >= 32) return false;
	c->queue->push_back(io);
	return true;
}

void process_eps(int fd, int config, int interface, int altsetting) {
	struct raw_gadget_altsetting *alt = &host_device_desc.configs[config]
					.interfaces[interface].altsettings[altsetting];

	printf("Activating %d endpoints on interface %d\n", (int)alt->interface.bNumEndpoints, interface);

	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];

		int addr = usb_endpoint_num(&ep->endpoint);
		assert(addr != 0);

		ep->thread_info.fd = fd;
		ep->thread_info.endpoint = ep->endpoint;
		ep->thread_info.device_bEndpointAddress = ep->device_bEndpointAddress;
		ep->thread_info.interface_number = alt->interface.bInterfaceNumber;
		ep->thread_info.interface_class = alt->interface.bInterfaceClass;
		ep->thread_info.data_queue = new std::deque<usb_raw_transfer_io>;
		ep->thread_info.data_mutex = new std::mutex;
		ep->thread_info.please_stop = new std::atomic<bool>(false);

		switch (usb_endpoint_type(&ep->endpoint)) {
		case USB_ENDPOINT_XFER_ISOC:
			ep->thread_info.transfer_type = "isoc";
			break;
		case USB_ENDPOINT_XFER_BULK:
			ep->thread_info.transfer_type = "bulk";
			break;
		case USB_ENDPOINT_XFER_INT:
			ep->thread_info.transfer_type = "int";
			break;
		default:
			printf("transfer_type %d is invalid\n", usb_endpoint_type(&ep->endpoint));
			assert(false);
		}

		if (usb_endpoint_dir_in(&ep->endpoint))
			ep->thread_info.dir = "in";
		else
			ep->thread_info.dir = "out";

		ep->thread_info.ep_num = usb_raw_ep_enable(fd, &ep->thread_info.endpoint);
		printf("%s_%s: addr = %u, ep = #%d\n",
			ep->thread_info.transfer_type.c_str(),
			ep->thread_info.dir.c_str(),
			addr, ep->thread_info.ep_num);

		// 给 mouse_control 的注入节拍线程留一个出口：只有 interrupt IN 端点能回主机。
		// 具体哪个接口是"鼠标"由 mouse_control 决定（它按描述符解析结果挑 inject_iface），
		// 这里只把每个接口的出口登记好。
		if (usb_endpoint_dir_in(&ep->endpoint) &&
		    usb_endpoint_type(&ep->endpoint) == USB_ENDPOINT_XFER_INT) {
			const uint8_t iface = ep->thread_info.interface_number;
			if (iface < 8) {
				g_inject_ctx[iface].queue = ep->thread_info.data_queue;
				g_inject_ctx[iface].mutex = ep->thread_info.data_mutex;
				g_inject_ctx[iface].ep_num = ep->thread_info.ep_num;
				g_inject_ctx[iface].iface = iface;
				ttbox_usbproxy::mouse_control_set_inject_sink(
					iface, inject_sink_push, &g_inject_ctx[iface]);
			}
		}

		if (verbose_level)
			printf("Creating thread for EP%02x\n",
				ep->thread_info.endpoint.bEndpointAddress);
		pthread_create(&ep->thread_read, 0,
			ep_loop_read, (void *)&ep->thread_info);
		pthread_create(&ep->thread_write, 0,
			ep_loop_write, (void *)&ep->thread_info);
	}

	printf("process_eps done\n");
}

void terminate_eps(int fd, int config, int interface, int altsetting) {
	struct raw_gadget_altsetting *alt = &host_device_desc.configs[config]
					.interfaces[interface].altsettings[altsetting];

	// Phase 1: Signal all threads to stop and interrupt blocking calls.
	// Set per-endpoint stop flags (not global - only affects this interface's threads).
	// Send SIGUSR1 to interrupt threads blocked on Raw Gadget ioctls.
	// The threads have a no-op handler for this signal, so the ioctl gets
	// interrupted with no other side-effects. Libusb transfer handling
	// does not get interrupted directly and instead times out.
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];
		if (ep->thread_info.please_stop)
			*ep->thread_info.please_stop = true;
		if (ep->thread_read)
			pthread_kill(ep->thread_read, SIGUSR1);
		if (ep->thread_write)
			pthread_kill(ep->thread_write, SIGUSR1);
	}

	// Phase 2: Wait for all threads to exit.
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];
		if (ep->thread_read && pthread_join(ep->thread_read, NULL))
			fprintf(stderr, "Error join thread_read\n");
		if (ep->thread_write && pthread_join(ep->thread_write, NULL))
			fprintf(stderr, "Error join thread_write\n");
		ep->thread_read = 0;
		ep->thread_write = 0;
	}

	// Phase 3: Clean up resources after all threads have exited.
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];
		// 先摘掉注入出口：队列马上要被 delete，绝不能让注入节拍线程拿到悬空指针
		if (ep->thread_info.interface_number < 8)
			ttbox_usbproxy::mouse_control_clear_inject_sink(ep->thread_info.interface_number);
		usb_raw_ep_disable(fd, ep->thread_info.ep_num);
		ep->thread_info.ep_num = -1;

		delete ep->thread_info.data_queue;
		delete ep->thread_info.data_mutex;
		delete ep->thread_info.please_stop;
		ep->thread_info.data_queue = nullptr;
		ep->thread_info.data_mutex = nullptr;
		ep->thread_info.please_stop = nullptr;
	}
}

void ep0_loop(int fd) {
	bool set_configuration_done_once = false;
	bool set_configuration_status_succeeded = false;

	printf("Start for EP0, thread id(%d)\n", gettid());

	// EP0 控制传输（枚举/SET_IDLE/报告描述符）不能被端点转发饿死，优先级比它高一点。
	usbproxy_rt_apply("EP0", 65, "USB_PROXY_CPU_AFFINITY", -1);

	if (verbose_level)
		print_eps_info(fd);

	while (!please_stop_ep0) {
		struct usb_raw_control_event event;
		event.inner.type = 0;
		event.inner.length = sizeof(event.ctrl);

		usb_raw_event_fetch(fd, (struct usb_raw_event *)&event);
		log_event((struct usb_raw_event *)&event);

		if (event.inner.length == 4294967295) {
			printf("End for EP0, thread id(%d)\n", gettid());
			return;
		}

		// Normally, we would only need to check for USB_RAW_EVENT_RESET to handle a reset event.
		// However, dwc2 is buggy and it reports a disconnect event instead of a reset.
			if (event.inner.type == USB_RAW_EVENT_RESET || event.inner.type == USB_RAW_EVENT_DISCONNECT) {
				printf("Resetting device\n");
			// Normally, we would need to stop endpoint threads first and only then
			// reset the device. However, libusb does not allow interrupting queued
			// requests submitted via sync I/O. Thus, we reset the proxied device to
			// force libusb to interrupt the requests and allow the endpoint threads
			// to exit on please_stop_eps checks.
				if (set_configuration_done_once)
					please_stop_eps = true;
				set_configuration_status_succeeded = false;
				clear_hid_readiness();
				reset_device();
			if (set_configuration_done_once) {
				struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
				printf("Stopping endpoint threads\n");
				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					struct raw_gadget_interface *iface = &config->interfaces[i];
					int interface_num = iface->altsettings[iface->current_altsetting]
						.interface.bInterfaceNumber;
					terminate_eps(fd, host_device_desc.current_config, i,
							iface->current_altsetting);
					release_interface(interface_num);
					iface->current_altsetting = 0;
				}
				printf("Endpoint threads stopped\n");
				please_stop_eps = false;
					host_device_desc.current_config = 0;
					set_configuration_done_once = false;
					set_configuration_status_succeeded = false;
				}
				continue;
			}

			if (event.inner.type == USB_RAW_EVENT_CONNECT) {
				if (set_configuration_done_once) {
					struct raw_gadget_config *config =
						&host_device_desc.configs[host_device_desc.current_config];
					printf("Host reconnected, stopping stale endpoint threads\n");
					please_stop_eps = true;
					for (int i = 0; i < config->config.bNumInterfaces; i++) {
						struct raw_gadget_interface *iface = &config->interfaces[i];
						int interface_num = iface->altsettings[iface->current_altsetting]
							.interface.bInterfaceNumber;
						terminate_eps(fd, host_device_desc.current_config, i,
								iface->current_altsetting);
						release_interface(interface_num);
						iface->current_altsetting = 0;
					}
					please_stop_eps = false;
					host_device_desc.current_config = 0;
					set_configuration_done_once = false;
					set_configuration_status_succeeded = false;
					clear_hid_readiness();
				}
				continue;
			}

			if (event.inner.type != USB_RAW_EVENT_CONTROL)
				continue;

		struct usb_raw_transfer_io io;
		io.inner.ep = 0;
		io.inner.flags = 0;
		// ★ 2026-09-26（第三轮审计）：wLength 是主机可控字段（uint16 最大 65535），
		//   而 io.data 只有 MAX_TRANSFER_SIZE(4096) 字节 —— 不钳制的话
		//   usb_raw_ep0_read/write 会让内核向栈上 io.data 越界读写
		//   （GET_DESCRIPTOR(CONFIG) 的大 config 描述符即可触发）。
		const uint16_t ctrl_wlen = event.ctrl.wLength;
		const uint16_t transfer_len =
			ctrl_wlen > MAX_TRANSFER_SIZE ? MAX_TRANSFER_SIZE : ctrl_wlen;
		io.inner.length = transfer_len;

		int injection_flags = USB_INJECTION_FLAG_NONE;
		int nbytes = 0;
		int result = 0;
		// ★ RAII：后面多条 continue 路径此前都跳过 delete[]（IN 被 stall 是
		//   枚举期高频路径）⇒ 常驻进程内存持续增长，mlockall 还会把泄漏页
		//   锁进不换页 RAM。unique_ptr 兜住所有路径。
		std::unique_ptr<unsigned char[]> control_data_owner(new unsigned char[ctrl_wlen]);
		unsigned char *control_data = control_data_owner.get();

		// For endpoint-directed class requests, translate the gadget-side
		// endpoint address in wIndex to the physical device's address.
		if ((event.ctrl.bRequestType & USB_RECIP_MASK) == USB_RECIP_ENDPOINT) {
			uint16_t device_ep = remap_endpoint_for_device(event.ctrl.wIndex & 0xff);
			if (device_ep != (event.ctrl.wIndex & 0xff)) {
				printf("ep0: remapping wIndex endpoint 0x%02x -> 0x%02x\n",
					event.ctrl.wIndex & 0xff, device_ep);
				event.ctrl.wIndex = (event.ctrl.wIndex & 0xff00) | device_ep;
			}
		}

		int rv = -1;
		if (event.ctrl.bRequestType & USB_DIR_IN) {
			result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
			if (result == 0) {
				// nbytes 可超过 MAX_TRANSFER_SIZE（大 config 描述符）⇒ 钳到栈缓冲内
				const int copy_len =
					nbytes > MAX_TRANSFER_SIZE ? MAX_TRANSFER_SIZE : nbytes;
				memcpy(&io.data[0], control_data, copy_len);
				io.inner.length = copy_len;

				if (injection_enabled) {
					injection(event, io, injection_flags);
					switch(injection_flags) {
					case USB_INJECTION_FLAG_NONE:
						break;
					case USB_INJECTION_FLAG_IGNORE:
						continue;
					case USB_INJECTION_FLAG_STALL:
						usb_raw_ep0_stall(fd);
						continue;
					default:
						printf("[Warning] Unknown injection flags: %d\n", injection_flags);
						break;
					}
				}

				maybe_override_descriptor(&event.ctrl, io);
				clamp_uvc_probe_commit(&event.ctrl, io);

				// Some UDCs require bMaxPacketSize0 to be at least 64.
				// Ideally, the information about UDC limitations needs to be
				// exposed by Raw Gadget, but this is not implemented at the moment;
				// see https://github.com/xairy/raw-gadget/issues/41.
				if (bmaxpacketsize0_must_greater_than_64 &&
				    (event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
				    event.ctrl.bRequest == USB_REQ_GET_DESCRIPTOR &&
				    (event.ctrl.wValue >> 8) == USB_DT_DEVICE) {
					struct usb_device_descriptor *dev = (struct usb_device_descriptor *)&io.data;
					if (dev->bMaxPacketSize0 < 64)
						dev->bMaxPacketSize0 = 64;
				}

					if (verbose_level >= 2)
						printData(io, 0x00, "control", "in");

					bool hid_report_descriptor =
						is_hid_report_descriptor_request(&event.ctrl);
					uint8_t hid_report_interface = event.ctrl.wIndex & 0xff;
					rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
					if (rv < 0) {
						log_ep0_ack_failed(rv);
						if (hid_report_descriptor) {
							printf("ep0: HID report descriptor iface %u failed after %d/%d bytes prepared\n",
								hid_report_interface, nbytes, event.ctrl.wLength);
						}
					}
					else {
						printf("ep0: transferred %d bytes (in)\n", rv);
						if (hid_report_descriptor) {
							printf("ep0: HID report descriptor iface %u transferred %d/%d bytes\n",
								hid_report_interface, rv, event.ctrl.wLength);
							mark_hid_report_descriptor_ready(hid_report_interface);
							// 把该接口的报告描述符交给 mouse_control 解析出**真实布局**
							//（buttons/X/Y/wheel 各在第几个字节）—— 这是"不再猜偏移"的唯一来源。
							// io.data 就是刚转发给主机的描述符原文。
							ttbox_usbproxy::mouse_control_set_report_descriptor(
								hid_report_interface,
								reinterpret_cast<const uint8_t*>(io.data),
								static_cast<uint32_t>(rv > 0 ? rv : 0));
						}
					}
			}
			else {
				usb_raw_ep0_stall(fd);
				continue;
			}
		}
		else {
			if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
					event.ctrl.bRequest == USB_REQ_SET_CONFIGURATION) {
				int desired_config = -1;
				for (int i = 0; i < host_device_desc.device.bNumConfigurations; i++) {
					if (host_device_desc.configs[i].config.bConfigurationValue == event.ctrl.wValue) {
						desired_config = i;
						break;
					}
				}
					if (desired_config < 0) {
						printf("[Warning] Skip changing configuration, wValue(%d) is invalid\n", event.ctrl.wValue);
						continue;
						}

							struct raw_gadget_config *config = &host_device_desc.configs[desired_config];

								reset_hid_readiness_for_config(desired_config);
								set_configuration_status_succeeded = false;
								if (!set_config_ack_before_configure)
									usb_raw_configure(fd);
						rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
						if (rv < 0) {
							int ep0_errno = errno;
							log_ep0_ack_failed(rv);
							if (!hid_passthrough_compat ||
							    (ep0_errno != ECONNRESET &&
							     ep0_errno != ESHUTDOWN &&
							     ep0_errno != EEXIST))
								continue;
							printf("ep0: continuing SET_CONFIGURATION after hidden host state change\n");
						}
							else {
								printf("ep0: request acked\n");
								set_configuration_status_succeeded = true;
							}
						if (set_config_ack_before_configure)
							usb_raw_configure(fd);

						if (set_configuration_done_once) { // Need to stop all threads for eps and cleanup
						printf("Changing configuration\n");
						for (int i = 0; i < config->config.bNumInterfaces; i++) {
							struct raw_gadget_interface *iface = &config->interfaces[i];
						int interface_num = iface->altsettings[iface->current_altsetting]
							.interface.bInterfaceNumber;
						terminate_eps(fd, host_device_desc.current_config, i,
								iface->current_altsetting);
						release_interface(interface_num);
						}
					}

						set_configuration(config->config.bConfigurationValue);
					host_device_desc.current_config = desired_config;

				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					struct raw_gadget_interface *iface = &config->interfaces[i];
					iface->current_altsetting = 0;
					int interface_num = iface->altsettings[0].interface.bInterfaceNumber;
					claim_interface(interface_num);
					process_eps(fd, desired_config, i, 0);
				}

					set_configuration_done_once = true;

					// 主动补拉还没嗅探到的 HID 报告描述符（见函数头注释）：
					// 布局学习不再单点依赖「主机来要」。
					active_fetch_hid_report_descriptors(desired_config);
				}
			else if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
					event.ctrl.bRequest == USB_REQ_SET_INTERFACE) {
				struct raw_gadget_config *config =
					&host_device_desc.configs[host_device_desc.current_config];

				int desired_interface = -1;
				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					if (config->interfaces[i].altsettings[0].interface.bInterfaceNumber ==
							event.ctrl.wIndex) {
						desired_interface = i;
						break;
					}
				}
				if (desired_interface < 0) {
					printf("[Warning] Skip changing interface, wIndex(%d) is invalid\n", event.ctrl.wIndex);
					continue;
				}

				struct raw_gadget_interface *iface = &config->interfaces[desired_interface];

				int desired_altsetting = -1;
				for (int i = 0; i < iface->num_altsettings; i++) {
					if (iface->altsettings[i].interface.bAlternateSetting == event.ctrl.wValue) {
						desired_altsetting = i;
						break;
					}
				}
				if (desired_altsetting < 0) {
					printf("[Warning] Skip changing alt_setting, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

				int effective_altsetting = find_best_compatible_altsetting(
					iface, desired_interface, desired_altsetting);

				if (effective_altsetting < 0) {
					printf("[Warning] No compatible altsetting for interface %d, stalling\n",
						iface->altsettings[desired_altsetting].interface.bInterfaceNumber);
					usb_raw_ep0_stall(fd);
					continue;
				}

				if (effective_altsetting != desired_altsetting) {
					printf("[Warning] Altsetting %d exceeds UDC limit; using %d instead\n",
						iface->altsettings[desired_altsetting].interface.bAlternateSetting,
						iface->altsettings[effective_altsetting].interface.bAlternateSetting);
				}

				struct raw_gadget_altsetting *alt = &iface->altsettings[effective_altsetting];

				if (effective_altsetting == iface->current_altsetting) {
					printf("Interface/altsetting already set\n");
					// But lets propagate the request to the device.
					set_interface_alt_setting(alt->interface.bInterfaceNumber,
						alt->interface.bAlternateSetting);
				}
				else {
					printf("Changing interface/altsetting\n");
					terminate_eps(fd, host_device_desc.current_config,
						desired_interface, iface->current_altsetting);
					set_interface_alt_setting(alt->interface.bInterfaceNumber,
						alt->interface.bAlternateSetting);
					process_eps(fd, host_device_desc.current_config,
						desired_interface, effective_altsetting);
					iface->current_altsetting = effective_altsetting;
				}

				// Ack request after spawning endpoint threads.
					rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
					if (rv < 0)
						log_ep0_ack_failed(rv);
					else
						printf("ep0: request acked\n");
				}
			else {
				if (injection_enabled) {
					injection(event, io, injection_flags);
					switch(injection_flags) {
					case USB_INJECTION_FLAG_NONE:
						break;
					case USB_INJECTION_FLAG_IGNORE:
						continue;
					case USB_INJECTION_FLAG_STALL:
						usb_raw_ep0_stall(fd);
						continue;
					default:
						printf("[Warning] Unknown injection flags: %d\n", injection_flags);
						break;
					}
				}

					if (event.ctrl.wLength == 0) {
						// For 0-length request, we can ack or stall the request via
						// Raw Gadget, depending on what the proxied device does.

						if (verbose_level >= 2)
							printData(io, 0x00, "control", "out");

						if (should_ack_hid_setup_without_forwarding(
								&event.ctrl,
								set_configuration_status_succeeded)) {
							rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
							if (rv < 0) {
								log_ep0_ack_failed(rv);
								continue;
							}
							printf("ep0: HID setup request acked locally\n");
							continue;
						}

						result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
						if (result == 0) {
							// Ack the request.
							rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
							if (rv < 0)
								log_ep0_ack_failed(rv);
							else
								printf("ep0: request acked\n");
						}
					else {
						// Stall the request.
						usb_raw_ep0_stall(fd);
						continue;
					}
				}
				else {
					// For non-0-length requests, we cannot retrieve the request data
					// without acking the request due to the Gadget subsystem limitations.
					// Thus, we cannot stall such request for the host even if the proxied
					// device stalls. This is not ideal but seems to work fine in practice.

					// Retrieve data for sending request to proxied device
					// (and ack the request).
						rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
						if (rv < 0) {
							log_ep0_ack_failed(rv);
							continue;
						}

					if (verbose_level >= 2)
						printData(io, 0x00, "control", "out");

				clamp_uvc_probe_commit(&event.ctrl, io);
				// ★ 用 rv（实际收到字节数），不能用 wLength：短包时会读越界 io.data
				//   并把未初始化栈数据转发给物理设备（io.inner.length 已钳到 4096）。
				memcpy(control_data, io.data, rv);

					result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
					if (result == 0) {
						printf("ep0: transferred %d bytes (out)\n", rv);
					}
				}
			}
		}
	}

	struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		int interface_num = iface->altsettings[iface->current_altsetting]
			.interface.bInterfaceNumber;
		terminate_eps(fd, host_device_desc.current_config, i,
				iface->current_altsetting);
		release_interface(interface_num);
	}

	printf("End for EP0, thread id(%d)\n", gettid());
}
