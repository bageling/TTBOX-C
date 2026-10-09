#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <vector>

#include "device-libusb.h"
#include "proxy.h"
#include "hidraw_source.hpp"

libusb_device 			**devs;
libusb_device_handle 		*dev_handle;
libusb_context 			*context = NULL;
libusb_hotplug_callback_handle	callback_handle = -1;

struct libusb_device_descriptor		device_device_desc;
struct libusb_config_descriptor		**device_config_desc;
// 当前 device_config_desc 持有的条目数（释放上一轮扫描结果用）
static int device_config_desc_count = 0;

pthread_t hotplug_monitor_thread;

// libusb 热插拔（设备拔出）回调：走带原子守卫的停机流程。
int hotplug_callback(struct libusb_context *ctx __attribute__((unused)),
			struct libusb_device *dev __attribute__((unused)),
			libusb_hotplug_event envet __attribute__((unused)),
			void *user_data __attribute__((unused))) {
	printf("Hotplug event: device disconnected, stopping proxy...\n");
	// ★ 2026-09-26：改走带原子守卫的停机。旧实现无守卫地 kill(0, SIGINT)，
	//   与端点线程的 LIBUSB_ERROR_NO_DEVICE 断连路径并发时第二发 SIGINT 命中
	//   usb-proxy.cpp 的 "force exiting" 分支 ⇒ 跳过 UDC/接口清理。
	stop_proxy_after_physical_disconnect();
	return 0;
}

// 唯一的 libusb 事件泵线程：循环 libusb_handle_events，驱动全部异步传输回调。
void *hotplug_monitor(void *arg __attribute__((unused))) {
	printf("Start hotplug_monitor/event thread, thread id(%d)\n", gettid());
	// 唯一的 libusb 事件泵：回调（含中断环）都靠它驱动，掉队就直接体现为丢帧。
	usbproxy_rt_apply("THREAD", 55, "USB_PROXY_CPU_AFFINITY", -1);
	while(!please_stop_ep0) {
		// This is the SOLE thread that calls libusb_handle_events.
		// All other threads (ISO IN, ISO OUT) submit async transfers
		// and spin-wait on their completion flags.  This avoids event
		// lock contention that would otherwise starve ISO OUT sends.
		struct timeval tv = {1, 0};
		libusb_handle_events_timeout(context, &tv);
	}
	printf("End hotplug_monitor/event thread, thread id(%d)\n", gettid());
	return NULL;
}

// 读某设备描述符与全部配置描述符（会先释放上一轮扫描结果，避免泄漏）。
int get_descriptor(libusb_device *device) {
	int result;
	result = libusb_get_device_descriptor(device, &device_device_desc);
	if (result != LIBUSB_SUCCESS) {
		if (verbose_level) {
			fprintf(stderr, "Error retrieving device descriptor: %s\n",
					libusb_strerror((libusb_error)result));
		}
		return result;
	}

	// ★ 2026-09-26：扫描循环每秒对所有设备重调本函数，旧实现直接覆盖全局
	// device_config_desc ⇒ 上一轮 new[] 数组和全部 libusb_get_config_descriptor
	// 结果失联（线性泄漏，等待目标设备期间最明显）。先释放上一轮再分配。
	if (device_config_desc) {
		for (int i = 0; i < device_config_desc_count; i++) {
			if (device_config_desc[i])
				libusb_free_config_descriptor(device_config_desc[i]);
		}
		delete[] device_config_desc;
		device_config_desc = NULL;
		device_config_desc_count = 0;
	}

	device_config_desc = new struct libusb_config_descriptor *[device_device_desc.bNumConfigurations];
	device_config_desc_count = device_device_desc.bNumConfigurations;
	for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
		device_config_desc[i] = NULL;
		result = libusb_get_config_descriptor(device, i, &device_config_desc[i]);
		if (result != LIBUSB_SUCCESS) {
			if (verbose_level) {
				fprintf(stderr, "Error retrieving configuration(%d) descriptor: %s\n",
						i, libusb_strerror((libusb_error)result));
			}
			return result;
		}
	}

	return LIBUSB_SUCCESS;
}

// 初始化 libusb、轮询找到并打开目标物理设备、抄走 hidraw 描述符、detach 内核驱动、注册热插拔。
int connect_device(int vendor_id, int product_id) {
	int result;
	result = libusb_init(&context);
	if (result < 0) {
		fprintf(stderr, "Init error: %s\n", libusb_strerror((libusb_error)result));
		return 1;
	}
	libusb_set_debug(context, 3);

	libusb_device *found = NULL;

	while (found == NULL) {
		int cnt = libusb_get_device_list(context, &devs);
		if (cnt < 0) {
			fprintf(stderr, "Get Device Error: %s\n",
					libusb_strerror((libusb_error)cnt));
			return 1;
		}
		if (verbose_level)
			printf("%d Devices in list\n", cnt);

		for (int i = 0; i < cnt; i++) {
			libusb_device *dvc = devs[i];
			result = get_descriptor(dvc);
			if (result != LIBUSB_SUCCESS)
				continue;

			if (device_device_desc.bDeviceClass == LIBUSB_CLASS_HUB)
				continue;

			if (vendor_id == -1 && product_id == -1) {
				found = dvc;
				break;
			}
			else if ((vendor_id == device_device_desc.idVendor || vendor_id == LIBUSB_HOTPLUG_MATCH_ANY) &&
				(product_id == device_device_desc.idProduct || product_id == LIBUSB_HOTPLUG_MATCH_ANY)) {
				found = dvc;
				break;
			}
		}

		if (!found) {
			if (verbose_level && vendor_id != -1 && product_id != -1)
				printf("Target device not found\n");
			libusb_free_device_list(devs, 1);
			sleep(1);
		}
	}

	result = libusb_open(found, &dev_handle);
	libusb_free_device_list(devs, 1);
	if (result != LIBUSB_SUCCESS) {
		if (verbose_level) {
			fprintf(stderr, "Error opening device handle: %s\n",
					libusb_strerror((libusb_error)result));
		}
		dev_handle = NULL;
		return result;
	}

	result = libusb_set_auto_detach_kernel_driver(dev_handle, 0);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "libusb_set_auto_detach_kernel_driver() failed: %s\n",
				libusb_strerror((libusb_error)result));
		return result;
	}

	int config = 0;
	result = libusb_get_configuration(dev_handle, &config);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "libusb_get_configuration() failed: %s\n",
				libusb_strerror((libusb_error)result));
		return result;
	}

	// ★ 2026-09-27（1.5.57）：detach 内核驱动【之前】，把内核缓存在 hidraw 里的
	// HID 报告描述符抄走（HIDIOCGRDESC 只读内核缓存，零 USB 请求）。背景：
	// quirk 固件（Compx Nearlink Dongle 373b:10c9 板端实测）在 usbfs/libusb
	// 上下文里对 class GET_DESCRIPTOR(Report) 一律超时且控制端点被打死，而
	// 内核正常枚举时拿得到 ⇒ hidraw 是唯一可靠的描述符来源。若 hidraw 缺失
	// （usb-proxy 重启而设备未重插：驱动已被上一轮 detach），把接口短暂绑回
	// usbhid 让内核在「正常枚举上下文」里重读一次，读完立刻解绑。
	{
		const uint8_t bus = libusb_get_bus_number(found);
		const uint8_t addr = libusb_get_device_address(found);
		ttbox_usbproxy::hidraw_clear();
		int hid_total = 0;
		for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
			if (device_config_desc[i]->bConfigurationValue != config)
				continue;
			for (int j = 0; j < device_config_desc[i]->bNumInterfaces; j++) {
				const struct libusb_interface* itf =
					&device_config_desc[i]->interface[j];
				for (int a = 0; a < itf->num_altsetting; a++) {
					if (itf->altsetting[a].bInterfaceClass == LIBUSB_CLASS_HID) {
						hid_total++;
						break;
					}
				}
			}
		}
		const int got = ttbox_usbproxy::hidraw_prefetch_descriptors(
			bus, addr, device_device_desc.idVendor,
			device_device_desc.idProduct);
		printf("hidraw: prefetch %d/%d HID report descriptors for %d:%d (pre-detach)\n",
			got, hid_total, bus, addr);
		if (got < hid_total) {
			const int got2 = ttbox_usbproxy::hidraw_refetch_via_rebind(
				bus, addr, device_device_desc.idVendor,
				device_device_desc.idProduct);
			printf("hidraw: rebind refetch +%d (total %d/%d)\n",
				got2, got + got2, hid_total);
		}
	}

	for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
		if (device_config_desc[i]->bConfigurationValue != config)
			continue;
		for (int j = 0; j < device_config_desc[i]->bNumInterfaces; j++)
			libusb_detach_kernel_driver(dev_handle, j);
	}

	if (reset_device_before_proxy) {
		result = libusb_reset_device(dev_handle);
		if (result != LIBUSB_SUCCESS) {
			fprintf(stderr, "libusb_reset_device() failed: %s\n",
					libusb_strerror((libusb_error)result));
			return result;
		}
	} else {
		// 1.5.61 起默认跳过：quirk 固件扛不住 usb reset（见 usb-proxy.cpp 注释）。
		printf("claim: skip reset before proxy (quirk 友好，1.5.61 默认)\n");
	}

	//check that device is responsive
	unsigned char unused[4];
	result = libusb_get_string_descriptor(dev_handle, 0, 0, unused, sizeof(unused));
	if (result < 0) {
		fprintf(stderr, "Device unresponsive: %s\n",
				libusb_strerror((libusb_error)result));
		return result;
	}

	if (callback_handle == -1) {
		result = libusb_hotplug_register_callback(context,
			(libusb_hotplug_event) (LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
			(libusb_hotplug_flag) 0, vendor_id, product_id,
			LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL, &callback_handle);

		if (result != LIBUSB_SUCCESS) {
			fprintf(stderr, "Error registering callback\n");
			libusb_exit(context);
			return result;
		}
		pthread_create(&hotplug_monitor_thread, 0,
			hotplug_monitor, nullptr);
	}

	return 0;
}

// 复位物理设备（libusb_reset_device），失败仅打印。
void reset_device() {
	int result = libusb_reset_device(dev_handle);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error resetting device: %s\n",
				libusb_strerror((libusb_error)result));
	}
}

// 给物理设备设置配置值，失败仅打印。
void set_configuration(int configuration) {
	int result = libusb_set_configuration(dev_handle, configuration);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error setting configuration(%d): %s\n",
				configuration, libusb_strerror((libusb_error)result));
	}
}

// 认领接口（独占），失败仅打印。
void claim_interface(int interface) {
	int result = libusb_claim_interface(dev_handle, interface);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error claiming interface(%d): %s\n",
				interface, libusb_strerror((libusb_error)result));
	}
}

// 释放接口；NOT_FOUND 属正常（未占用）不报错。
void release_interface(int interface) {
	int result = libusb_release_interface(dev_handle, interface);
	if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_FOUND) {
		fprintf(stderr, "Error releasing interface(%d): %s\n",
				interface, libusb_strerror((libusb_error)result));
	}
}

// 切换接口备用设置（altsetting），失败仅打印。
void set_interface_alt_setting(int interface, int altsetting) {
	int result = libusb_set_interface_alt_setting(dev_handle, interface, altsetting);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error setting interface altsetting(%d, %d): %s\n",
				interface, altsetting, libusb_strerror((libusb_error)result));
	}
}

// 向物理设备发一次控制传输；返回 0 成功、-1 为 PIPE(stall)、其余为 libusb 错误码。
int control_request(const usb_ctrlrequest *setup_packet, int *nbytes,
			unsigned char **dataptr, int timeout) {
	int result = libusb_control_transfer(dev_handle,
					setup_packet->bRequestType, setup_packet->bRequest,
					setup_packet->wValue, setup_packet->wIndex, *dataptr,
					setup_packet->wLength, timeout);

	if (result < 0) {
		if (verbose_level) {
			fprintf(stderr, "Error sending setup packet: %s\n",
					libusb_strerror((libusb_error)result));
		}
		if (result == LIBUSB_ERROR_PIPE)
			return -1;
		return result;
	}
	else {
		if (verbose_level)
			printf("Control transfer succeed\n");
	}

	*nbytes = result;
	return 0;
}

int send_iso_data(uint8_t endpoint, uint8_t *dataptr, int length, int timeout);

// 发送数据到指定端点（bulk/interrupt），带重试与 clear_halt；返回 libusb 结果码。
int send_data(uint8_t endpoint, uint8_t attributes, uint8_t *dataptr,
			int length, int timeout) {
	int transferred;
	int attempt = 0;
	int result = LIBUSB_SUCCESS;
	int sent = 0;

	bool incomplete_transfer = false;
	bool tracked = false;

	switch (attributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		fprintf(stderr, "Can't send on a control endpoint.\n");
		break;
	case USB_ENDPOINT_XFER_BULK:
		tracked = true;
		do {
			transferred = 0;
			result = libusb_bulk_transfer(dev_handle, endpoint, dataptr + sent,
							  length - sent, &transferred, timeout);
			if (transferred > 0)
				sent += transferred;
			if (sent != length) {
				fprintf(stderr, "Incomplete Bulk transfer on EP%02x for attempt %d. length(%d), transferred(%d)\n",
					endpoint, attempt, length, sent);
				incomplete_transfer = true;
			}
			if (result == LIBUSB_SUCCESS) {
				if (incomplete_transfer)
					printf("Resent Bulk transfer on EP%02x for attempt %d. length(%d), transferred(%d)\n",
						endpoint, attempt, length, sent);
				if (verbose_level > 2)
					printf("Sent %d bytes (Bulk) to EP%02x\n", sent, endpoint);
			}
			if ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT))
				libusb_clear_halt(dev_handle, endpoint);

			attempt++;
		} while ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT || sent != length)
					&& attempt < MAX_ATTEMPTS);
		break;
	case USB_ENDPOINT_XFER_INT:
		tracked = true;
		do {
			transferred = 0;
			result = libusb_interrupt_transfer(dev_handle, endpoint, dataptr + sent,
								  length - sent, &transferred, timeout);
			if (transferred > 0)
				sent += transferred;

			if (sent != length)
				fprintf(stderr, "Incomplete Interrupt transfer on EP%02x\n", endpoint);
			if (result == LIBUSB_SUCCESS && verbose_level > 2)
				printf("Sent %d bytes (Int) to libusb EP%02x\n", sent, endpoint);
			if ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT))
				libusb_clear_halt(dev_handle, endpoint);
			attempt++;
		} while ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT || sent != length)
					&& attempt < MAX_ATTEMPTS);
		break;
	}
	if (tracked && sent != length) {
		fprintf(stderr,
			"Incomplete transfer on EP%02x after %d attempts: %d/%d bytes sent\n",
			endpoint, attempt, sent, length);
		return result == LIBUSB_SUCCESS ? LIBUSB_ERROR_OTHER : result;
	}
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Transfer error sending on EP%02x: %s\n",
				endpoint, libusb_strerror((libusb_error)result));
	}
	return result;
}

// 等时 IN 同步等待回调：置完成标志。
void iso_transfer_callback(struct libusb_transfer *transfer) {
	int *iso_completed = (int *)transfer->user_data;
	*iso_completed = 1;
}

// Bounded async ISO OUT: submit and return immediately.
// The dedicated event thread (hotplug_monitor) processes completions.
// We limit in-flight transfers to avoid flooding the kernel.
#define ISO_OUT_MAX_IN_FLIGHT 8
static std::atomic<int> iso_out_in_flight(0);

// 等时 OUT 完成回调：递减在途计数、按需打印错误、释放 transfer 与缓冲。
static void iso_out_callback(struct libusb_transfer *transfer) {
	iso_out_in_flight--;
	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		static int iso_out_err_count = 0;
		iso_out_err_count++;
		if (iso_out_err_count <= 10 || iso_out_err_count % 100 == 0)
			fprintf(stderr, "ISO OUT EP%02x failed: status=%d (total errors: %d)\n",
				transfer->endpoint, transfer->status, iso_out_err_count);
	}
	// Free the buffer passed via user_data.
	delete[] (unsigned char *)transfer->user_data;
	libusb_free_transfer(transfer);
}

// 异步提交一个等时 OUT 包；在途满时丢弃（缓冲由回调释放，丢弃时此处释放）。
int send_iso_data(uint8_t endpoint, uint8_t *dataptr, int length, int timeout) {
	// If at capacity, wait briefly for a slot.
	int waited_us = 0;
	while (iso_out_in_flight >= ISO_OUT_MAX_IN_FLIGHT && waited_us < 2000) {
		usleep(50);
		waited_us += 50;
	}
	if (iso_out_in_flight >= ISO_OUT_MAX_IN_FLIGHT) {
		// Drop this packet -- ISO is inherently lossy.
		// Free the buffer since the callback won't run.
		delete[] dataptr;
		return LIBUSB_SUCCESS;
	}

	struct libusb_transfer *transfer = libusb_alloc_transfer(1);
	if (!transfer) {
		fprintf(stderr, "Failed to allocate libusb_transfer for ISO OUT.\n");
		return LIBUSB_ERROR_OTHER;
	}

	// The callback frees both the transfer and the buffer (via user_data).
	libusb_fill_iso_transfer(transfer, dev_handle, endpoint, dataptr, length,
				1, iso_out_callback, dataptr, timeout);
	libusb_set_iso_packet_lengths(transfer, length);

	int rv = libusb_submit_transfer(transfer);
	if (rv != LIBUSB_SUCCESS) {
		fprintf(stderr, "ISO OUT submit failed on EP%02x: %s (len=%d)\n",
			endpoint, libusb_strerror((libusb_error)rv), length);
		libusb_free_transfer(transfer);
		return rv;
	}

	iso_out_in_flight++;
	return LIBUSB_SUCCESS;
}

// 从等时端点批量接收 batch_size 个包，自旋等待完成后逐包回填到 result。
int receive_iso_data_batched(uint8_t endpoint, uint16_t maxPacketSize,
			struct iso_batch_result *result, int batch_size, int timeout) {
	if (batch_size < 1)
		batch_size = 1;
	if (batch_size > ISO_BATCH_SIZE_MAX)
		batch_size = ISO_BATCH_SIZE_MAX;

	memset(result, 0, sizeof(*result));
	result->buffer = new uint8_t[maxPacketSize * batch_size];

	struct libusb_transfer *transfer = libusb_alloc_transfer(batch_size);
	if (!transfer) {
		fprintf(stderr, "Failed to allocate libusb_transfer for ISO batch.\n");
		delete[] result->buffer;
		result->buffer = nullptr;
		return LIBUSB_ERROR_OTHER;
	}

	volatile int iso_completed = 0;
	libusb_fill_iso_transfer(transfer, dev_handle, endpoint, result->buffer,
				maxPacketSize * batch_size, batch_size,
				iso_transfer_callback, (void *)&iso_completed, timeout);
	libusb_set_iso_packet_lengths(transfer, maxPacketSize);

	int rv = libusb_submit_transfer(transfer);
	if (rv != LIBUSB_SUCCESS) {
		if (verbose_level)
			fprintf(stderr, "ISO batch submit failed on EP%02x: %s\n",
				endpoint, libusb_strerror((libusb_error)rv));
		libusb_free_transfer(transfer);
		delete[] result->buffer;
		result->buffer = nullptr;
		return rv;
	}

	// Spin-wait for completion; the dedicated event thread
	// (hotplug_monitor) will call iso_transfer_callback.
	while (!iso_completed)
		usleep(50);

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
	    transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
		if (verbose_level)
			fprintf(stderr, "ISO batch transfer failed on EP%02x: status %d\n",
				endpoint, transfer->status);
		if (transfer->status == LIBUSB_TRANSFER_STALL)
			libusb_clear_halt(dev_handle, endpoint);
		libusb_free_transfer(transfer);
		delete[] result->buffer;
		result->buffer = nullptr;
		return LIBUSB_ERROR_IO;
	}

	result->num_packets = batch_size;
	uint8_t *packet_ptr = result->buffer;
	for (int i = 0; i < batch_size; i++) {
		result->packets[i].data = packet_ptr;
		result->packets[i].actual_length = transfer->iso_packet_desc[i].actual_length;
		result->packets[i].status = transfer->iso_packet_desc[i].status;
		result->total_length += result->packets[i].actual_length;
		packet_ptr += maxPacketSize;

		if (result->packets[i].status == LIBUSB_TRANSFER_COMPLETED &&
		    result->packets[i].actual_length > 0)
			result->success = true;
	}

	if (verbose_level > 2)
		printf("ISO batch received: %d packets, %d total bytes\n",
			batch_size, result->total_length);

	libusb_free_transfer(transfer);
	return LIBUSB_SUCCESS;
}

// 从端点同步接收数据（bulk/interrupt）；内部 new[] 出缓冲，交由调用方释放。
int receive_data(uint8_t endpoint, uint8_t attributes, uint16_t maxPacketSize,
			uint8_t **dataptr, int *length, int timeout) {
	int result = LIBUSB_SUCCESS;

	int attempt = 0;
	switch (attributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		fprintf(stderr, "Can't read on a control endpoint.\n");
		break;
	case USB_ENDPOINT_XFER_ISOC:
		// ISO IN is handled by receive_iso_data_batched() directly.
		fprintf(stderr, "receive_data() should not be called for ISO endpoints.\n");
		break;
	case USB_ENDPOINT_XFER_BULK:
		*dataptr = new uint8_t[maxPacketSize * 8];
		do {
			result = libusb_bulk_transfer(dev_handle, endpoint, *dataptr, maxPacketSize, length, timeout);
			if (result == LIBUSB_SUCCESS && verbose_level > 2)
				printf("Received bulk data(%d) bytes\n", *length);
			if ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT))
				libusb_clear_halt(dev_handle, endpoint);

			attempt++;
		} while ((result == LIBUSB_ERROR_PIPE || result == LIBUSB_ERROR_TIMEOUT) && attempt < MAX_ATTEMPTS);
		break;
	case USB_ENDPOINT_XFER_INT:
		*dataptr = new uint8_t[maxPacketSize];
		result = libusb_interrupt_transfer(dev_handle, endpoint, *dataptr, maxPacketSize, length, timeout);
		if (result == LIBUSB_SUCCESS && verbose_level > 2)
			printf("Received int data(%d) bytes\n", *length);
		break;
	}

	if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_TIMEOUT) {
		fprintf(stderr, "Transfer error receiving on EP%02x: %s\n",
				endpoint, libusb_strerror((libusb_error)result));
	}

	return result;
}

/*
 * ---- Interrupt IN receive ring -------------------------------------------
 * See the comment in device-libusb.h for why this exists.  Summary of the
 * measured effect on a full-speed link: 1 transfer in flight = 500 Hz,
 * 2+ transfers in flight = 1000 Hz.
 *
 * Conventions followed from the rest of this file:
 *  - we never call libusb_handle_events here; hotplug_monitor is the sole
 *    event pump and it invokes interrupt_transfer_cb().
 *  - a transfer that is not resubmitted decrements inflight exactly once,
 *    so stop() can safely wait for the pipe to drain.
 */

struct interrupt_slot {
	struct libusb_transfer *transfer;
	uint8_t *buffer;
};

struct interrupt_ring {
	uint8_t			endpoint;
	int			max_packet;
	int			depth;
	struct interrupt_slot	*slots;

	std::deque<std::vector<uint8_t> > pending;

	pthread_mutex_t		lock;
	pthread_cond_t		cond;

	std::atomic<bool>	stopping;
	std::atomic<bool>	stalled;	/* endpoint halted, cleared by the reader */
	std::atomic<int>	inflight;
	std::atomic<int>	fatal;		/* libusb error that ends the reader */
	std::atomic<long>	dropped;	/* queue overflows, diagnostic only */
	std::atomic<long>	window_count;	/* reports since the last rate print */
	uint64_t		window_start;	/* CLOCK_MONOTONIC, ms */
};

/* Rate self-diagnostic: this is the number the whole change is judged on
   (500 Hz before, 1000 Hz after).  Printed from the reader thread, so it
   also reports 0 while the mouse is idle — which is the normal state of a
   mouse that is not being moved. */
#define INT_RING_RATE_WINDOW_MS	2000

// 取 CLOCK_MONOTONIC 毫秒时间戳（速率诊断用）。
static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 中断传输完成回调（事件线程内）：把报告入队并重提交 transfer 保持管道常满。
static void interrupt_transfer_cb(struct libusb_transfer *transfer)
{
	struct interrupt_ring *ring =
		(struct interrupt_ring *)transfer->user_data;
	if (!ring)
		return;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		if (transfer->actual_length > 0) {
			pthread_mutex_lock(&ring->lock);
			if ((int)ring->pending.size() >= INT_RING_QUEUE_MAX) {
				ring->pending.pop_front();
				ring->dropped++;
			}
			ring->pending.push_back(std::vector<uint8_t>(
				transfer->buffer,
				transfer->buffer + transfer->actual_length));
			ring->window_count++;
			pthread_cond_signal(&ring->cond);
			pthread_mutex_unlock(&ring->lock);
		}
	} else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
		ring->fatal = LIBUSB_ERROR_NO_DEVICE;
	} else if (transfer->status == LIBUSB_TRANSFER_STALL) {
		/* We are running inside the event thread, so a sync control
		   transfer here (libusb_clear_halt) would need the very thread
		   we are blocking.  Hand it to the reader instead. */
		ring->stalled = true;
		pthread_mutex_lock(&ring->lock);
		pthread_cond_signal(&ring->cond);
		pthread_mutex_unlock(&ring->lock);
	}

	/* Keep the pipe full unless we are tearing down. */
	if (!ring->stopping) {
		int rv = libusb_submit_transfer(transfer);
		if (rv == LIBUSB_SUCCESS)
			return;		/* still in flight, do not decrement */
		if (verbose_level)
			fprintf(stderr, "interrupt ring: resubmit on EP%02x failed: %s\n",
				transfer->endpoint, libusb_strerror((libusb_error)rv));
		ring->fatal = rv;
	}

	ring->inflight--;
	if (ring->inflight <= 0 || ring->stopping) {
		pthread_mutex_lock(&ring->lock);
		pthread_cond_broadcast(&ring->cond);
		pthread_mutex_unlock(&ring->lock);
	}
}

// 创建一个中断 IN 环：分配 depth 个 transfer/缓冲并全部提交，保持多份在途以免丢帧。
struct interrupt_ring *interrupt_ring_create(uint8_t endpoint,
			uint16_t maxPacketSize, int depth)
{
	if (!dev_handle)
		return NULL;
	if (depth < 2)
		depth = INT_RING_DEPTH;
	if (depth > INT_RING_DEPTH_MAX)
		depth = INT_RING_DEPTH_MAX;
	int pkt = maxPacketSize ? maxPacketSize : 64;

	struct interrupt_ring *ring = new struct interrupt_ring;

	ring->endpoint = endpoint;
	ring->max_packet = pkt;
	ring->depth = depth;
	ring->slots = new struct interrupt_slot[depth]();
	pthread_mutex_init(&ring->lock, NULL);
	pthread_cond_init(&ring->cond, NULL);
	ring->stopping = false;
	ring->stalled = false;
	ring->inflight = 0;
	ring->fatal = 0;
	ring->dropped = 0;
	ring->window_count = 0;
	ring->window_start = now_ms();

	for (int i = 0; i < depth; i++) {
		ring->slots[i].buffer = new uint8_t[pkt];
		ring->slots[i].transfer = libusb_alloc_transfer(0);
		if (!ring->slots[i].transfer) {
			fprintf(stderr, "interrupt ring: out of transfers on EP%02x\n",
				endpoint);
			interrupt_ring_destroy(ring);
			return NULL;
		}
		libusb_fill_interrupt_transfer(ring->slots[i].transfer, dev_handle,
				endpoint, ring->slots[i].buffer, pkt,
				interrupt_transfer_cb, ring, 0);
	}

	for (int i = 0; i < depth; i++) {
		int rv = libusb_submit_transfer(ring->slots[i].transfer);
		if (rv != LIBUSB_SUCCESS) {
			if (verbose_level)
				fprintf(stderr, "interrupt ring: submit on EP%02x failed: %s\n",
					endpoint, libusb_strerror((libusb_error)rv));
			ring->fatal = rv;
			break;
		}
		ring->inflight++;
	}

	if (verbose_level)
		printf("interrupt ring: EP%02x started, %d transfers in flight, %d bytes each\n",
			endpoint, ring->inflight.load(), pkt);

	return ring;
}

// 从环里取一份报告（队列空则限时等待）；超时返回 LIBUSB_ERROR_TIMEOUT，停机返回 INTERRUPTED。
int interrupt_ring_next(struct interrupt_ring *ring, unsigned char **dataptr,
			int *length)
{
	*dataptr = NULL;
	*length = 0;
	if (!ring)
		return LIBUSB_ERROR_INVALID_PARAM;

	for (;;) {
		/* Rate self-diagnostic.  Runs on the way in as well as on the
		   timeout path, so an idle mouse reports "0 reports" instead of
		   printing nothing at all. */
		uint64_t now = now_ms();
		uint64_t dt = now - ring->window_start;
		if (dt >= INT_RING_RATE_WINDOW_MS) {
			long got = ring->window_count.exchange(0);
			if (verbose_level)
				printf("interrupt ring: EP%02x %ld reports in %llums (%.0f/s)\n",
					ring->endpoint, got, (unsigned long long)dt,
					got * 1000.0 / (double)dt);
			ring->window_start = now;
		}

		/* Clear a halt from the reader thread, never from the callback
		   (libusb_clear_halt() is a sync transfer and the callback runs
		   inside the event thread). */
		if (ring->stalled.exchange(false))
			libusb_clear_halt(dev_handle, ring->endpoint);

		pthread_mutex_lock(&ring->lock);
		while (ring->pending.empty()) {
			if (ring->fatal.load()) {
				int err = ring->fatal.load();
				pthread_mutex_unlock(&ring->lock);
				return err;
			}
			if (ring->stopping.load()) {
				pthread_mutex_unlock(&ring->lock);
				return LIBUSB_ERROR_INTERRUPTED;
			}

			/* Bounded wait: the caller's loop polls its own stop flags,
			   so we must come back for air even when the device is idle.
			   A mouse only reports on movement. */
			struct timespec deadline;
			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_nsec += 100 * 1000 * 1000;	/* 100 ms */
			if (deadline.tv_nsec >= 1000000000) {
				deadline.tv_nsec -= 1000000000;
				deadline.tv_sec += 1;
			}
			int waited = pthread_cond_timedwait(&ring->cond, &ring->lock,
							    &deadline);
			if (waited == ETIMEDOUT && ring->pending.empty()) {
				pthread_mutex_unlock(&ring->lock);
				return LIBUSB_ERROR_TIMEOUT;
			}
		}
		std::vector<uint8_t> one = ring->pending.front();
		ring->pending.pop_front();
		pthread_mutex_unlock(&ring->lock);

		*dataptr = new unsigned char[one.size()];
		memcpy(*dataptr, &one[0], one.size());
		*length = (int)one.size();
		return LIBUSB_SUCCESS;
	}
}

// 销毁中断环：置停止位、取消在途 transfer、等回调退场后释放；仍在途则故意泄漏避免 UAF。
void interrupt_ring_destroy(struct interrupt_ring *ring)
{
	if (!ring)
		return;

	ring->stopping = true;

	/* Wake any reader blocked in interrupt_ring_next(). */
	pthread_mutex_lock(&ring->lock);
	pthread_cond_broadcast(&ring->cond);
	pthread_mutex_unlock(&ring->lock);

	for (int i = 0; i < ring->depth; i++)
		if (ring->slots[i].transfer)
			libusb_cancel_transfer(ring->slots[i].transfer);

	/* Wait for the callbacks to retire.  300 ms is plenty; the transfers
	   have no timeout but cancellation is serviced by the event thread. */
	for (int w = 0; w < 300 && ring->inflight.load() > 0; w++)
		usleep(1000);

	if (ring->inflight.load() > 0) {
		/* Freeing now would be a use-after-free if a callback is still
		   pending.  Leak instead: this only happens at teardown. */
		fprintf(stderr, "interrupt ring: EP%02x still has %d transfers in flight,"
			" leaking ring to avoid use-after-free\n",
			ring->endpoint, ring->inflight.load());
		return;
	}

	for (int i = 0; i < ring->depth; i++) {
		if (ring->slots[i].transfer)
			libusb_free_transfer(ring->slots[i].transfer);
		delete[] ring->slots[i].buffer;
	}
	delete[] ring->slots;

	if (verbose_level && ring->dropped.load())
		printf("interrupt ring: EP%02x dropped %ld reports (queue overflow)\n",
			ring->endpoint, ring->dropped.load());

	pthread_cond_destroy(&ring->cond);
	pthread_mutex_destroy(&ring->lock);
	delete ring;
}
