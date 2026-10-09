#include <libusb-1.0/libusb.h>

#include "misc.h"

#define USB_REQUEST_TIMEOUT 1000

#define MAX_ATTEMPTS 5

#define ISO_BATCH_SIZE_DEFAULT 8
#define ISO_BATCH_SIZE_MAX 32

// 一批等时接收里单个包的结果（数据指针/实际长度/libusb 状态）。
struct iso_packet_result {
	uint8_t *data;
	int actual_length;
	int status; // libusb_transfer_status
};

// 一次等时批量接收的结果（整块缓冲 + 逐包结果 + 汇总）。
struct iso_batch_result {
	uint8_t *buffer;
	int num_packets;
	struct iso_packet_result packets[ISO_BATCH_SIZE_MAX];
	int total_length;
	bool success;
};

extern libusb_device			**devs;
extern libusb_device_handle		*dev_handle;
extern libusb_context			*context;
extern libusb_hotplug_callback_handle	callback_handle;

extern struct libusb_device_descriptor		device_device_desc;
extern struct libusb_config_descriptor		**device_config_desc;

extern pthread_t hotplug_monitor_thread;

// 打开物理设备（按 VID/PID；-1/-1 表示取第一个非 Hub 设备），阻塞轮询直到找到。
int connect_device(int vendorId, int productId);
// 复位物理设备（libusb_reset_device）。
void reset_device();
// 设置物理设备的配置值。
void set_configuration(int configuration);
// 认领（独占）某接口。
void claim_interface(int interface);
// 释放接口；NOT_FOUND 视为正常不报错。
void release_interface(int interface);
// 切换接口的备用设置（altsetting）。
void set_interface_alt_setting(int interface, int altsetting);
// 向物理设备发一次控制传输（setup 包 + 数据缓冲），返回 0 成功。
int control_request(const usb_ctrlrequest *setup_packet, int *nbytes,
			unsigned char **dataptr, int timeout);
// 向指定端点发送数据（按属性分派 bulk/interrupt）；返回 libusb 结果码。
int send_data(uint8_t endpoint, uint8_t attributes, uint8_t *dataptr,
			int length, int timeout);
// 异步发送一个等时包（受在途上限约束，超限即丢包）。
int send_iso_data(uint8_t endpoint, uint8_t *dataptr, int length, int timeout);
// 从指定端点同步接收数据（bulk/interrupt）；缓冲由内部 new[]，调用方 delete[]。
int receive_data(uint8_t endpoint, uint8_t attributes, uint16_t maxPacketSize,
			uint8_t **dataptr, int *length, int timeout);
// 从等时端点批量接收若干包，结果填进 result（缓冲归 result 所有）。
int receive_iso_data_batched(uint8_t endpoint, uint16_t maxPacketSize,
			struct iso_batch_result *result, int batch_size, int timeout);

/*
 * Interrupt IN receive ring (2026-09-22).
 *
 * Reading an interrupt endpoint with a single in-flight libusb transfer
 * halves the effective report rate: measured on this board's full-speed
 * link, 1 transfer gives ~2.00 ms between reports (500 Hz) while 2+ give
 * ~1.00 ms (1000 Hz).  The host controller walks the periodic list once
 * per frame, so if the endpoint has no transfer queued at that instant the
 * frame is lost.  Keeping N transfers queued closes the gap between
 * completion and resubmission.
 *
 * One ring per endpoint (every endpoint owns a reader thread).  Completions
 * are served by the existing hotplug_monitor event thread.
 */
#define INT_RING_DEPTH		8	/* transfers kept in flight */
#define INT_RING_DEPTH_MAX	32
#define INT_RING_QUEUE_MAX	64	/* completed reports buffered before drop */

struct interrupt_ring;		/* opaque */

struct interrupt_ring *interrupt_ring_create(uint8_t endpoint, uint16_t maxPacketSize, int depth);
int interrupt_ring_next(struct interrupt_ring *ring, unsigned char **dataptr, int *length);
void interrupt_ring_destroy(struct interrupt_ring *ring);
