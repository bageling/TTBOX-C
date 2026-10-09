#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/types.h>

#include "host-raw-gadget.h"

struct raw_gadget_device host_device_desc;

/*----------------------------------------------------------------------*/

// 打开 /dev/raw-gadget（只读失败即退出）。
int usb_raw_open() {
	int fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0) {
		perror("open() /dev/raw-gadget");
		exit(EXIT_FAILURE);
	}
	return fd;
}

// 对 Raw Gadget 发 INIT（UDC EBUSY 时最多重试 9 次，约 4.5s）。
void usb_raw_init(int fd, enum usb_device_speed speed,
			const char *driver, const char *device) {
	struct usb_raw_init arg;
	strcpy((char *)&arg.driver_name[0], driver);
	strcpy((char *)&arg.device_name[0], device);
	arg.speed = speed;
	int rv = ioctl(fd, USB_RAW_IOCTL_INIT, &arg);
	for (int attempt = 1; rv < 0 && errno == EBUSY && attempt < 10; attempt++) {
		fprintf(stderr,
			"ioctl(USB_RAW_IOCTL_INIT): UDC busy, waiting for ownership release "
			"(attempt %d/10)...\n", attempt);
		usleep(500000);
		rv = ioctl(fd, USB_RAW_IOCTL_INIT, &arg);
	}
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_INIT)");
		exit(EXIT_FAILURE);
	}
}

// 启动 gadget（RUN；UDC EBUSY 时重试）。
void usb_raw_run(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_RUN, 0);
	for (int attempt = 1; rv < 0 && errno == EBUSY && attempt < 10; attempt++) {
		fprintf(stderr,
			"ioctl(USB_RAW_IOCTL_RUN): UDC busy, waiting for ownership release "
			"(attempt %d/10)...\n", attempt);
		usleep(500000);
		rv = ioctl(fd, USB_RAW_IOCTL_RUN, 0);
	}
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_RUN)");
		exit(EXIT_FAILURE);
	}
}

// 取一个 Raw Gadget 事件；被信号打断时用 length=-1 表示（ep0 据此退出）。
void usb_raw_event_fetch(int fd, struct usb_raw_event *event) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, event);
	if (rv < 0) {
		if (errno == EINTR) {
			event->length = 4294967295;
			return;
		}
		perror("ioctl(USB_RAW_IOCTL_EVENT_FETCH)");
		exit(EXIT_FAILURE);
	}
}

// EP0 读；对 EBUSY/EINVAL/EINTR/ESHUTDOWN/ECONNRESET 返回错误码而不退出，其余致命。
int usb_raw_ep0_read(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
	if (rv < 0) {
		if (errno == EBUSY ||
		    errno == EINVAL ||
		    errno == EINTR ||
		    errno == ESHUTDOWN ||
		    errno == ECONNRESET)
			return rv;
		perror("ioctl(USB_RAW_IOCTL_EP0_READ)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

// EP0 写；可容忍的错误码同 ep0_read，其余致命。
int usb_raw_ep0_write(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io);
	if (rv < 0) {
		if (errno == EBUSY ||
		    errno == EINVAL ||
		    errno == EINTR ||
		    errno == ESHUTDOWN ||
		    errno == ECONNRESET)
			return rv;
		perror("ioctl(USB_RAW_IOCTL_EP0_WRITE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

// 使能端点，返回 Raw Gadget 分配的端点号；失败即退出。
int usb_raw_ep_enable(int fd, struct usb_endpoint_descriptor *desc) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, desc);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_ENABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

// 关闭端点；允许多种错误码（重复关/已断开等）。
int usb_raw_ep_disable(int fd, uint32_t num) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, num);
	if (rv < 0) {
		if (errno == EINVAL ||
		    errno == ESHUTDOWN ||
		    errno == ENODEV ||
		    errno == ENOENT ||
		    errno == EBUSY)
			return rv;
		perror("ioctl(USB_RAW_IOCTL_EP_DISABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

// 从端点读；容忍 halt/复位/信号/等时误差/忙等错误码，其余致命。
int usb_raw_ep_read(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_READ, io);
	if (rv < 0) {
		if (errno == EINPROGRESS) {
			// Ignore failures caused by the test that halts endpoints.
			return rv;
		}
		else if (errno == ESHUTDOWN) {
			// Ignore failures caused by device reset.
			return rv;
		}
		else if (errno == EINTR) {
			// Ignore failures caused by sending a signal to the
			// endpoint threads when shutting them down.
			return rv;
		}
		else if (errno == EXDEV || errno == ENODATA || errno == EOVERFLOW) {
			// Ignore failures caused by isochronous timing errors
			// (dwc3 returns EXDEV, dwc2 returns ENODATA,
			// data overrun returns EOVERFLOW).
			return rv;
		}
		else if (errno == EBUSY)
			return rv;
		perror("ioctl(USB_RAW_IOCTL_EP_READ)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

// 向端点写；容忍错误码同 ep_read，其余致命。
int usb_raw_ep_write(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_WRITE, io);
	if (rv < 0) {
		if (errno == EINPROGRESS) {
			// Ignore failures caused by the test that halts endpoints.
			return rv;
		}
		else if (errno == ESHUTDOWN) {
			// Ignore failures caused by device reset.
			return rv;
		}
		else if (errno == EINTR) {
			// Ignore failures caused by sending a signal to the
			// endpoint threads when shutting them down.
			return rv;
		}
		else if (errno == EXDEV || errno == ENODATA || errno == EOVERFLOW) {
			// Ignore failures caused by isochronous timing errors
			// (dwc3 returns EXDEV, dwc2 returns ENODATA,
			// data overrun returns EOVERFLOW).
			return rv;
		}
		else if (errno == EBUSY)
			return rv;
		perror("ioctl(USB_RAW_IOCTL_EP_WRITE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

// 让 Raw Gadget 进入 CONFIGURE 状态；失败即退出。
void usb_raw_configure(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_CONFIGURED)");
		exit(EXIT_FAILURE);
	}
}

// 上报 VBUS 取电电流（mA）；失败即退出。
void usb_raw_vbus_draw(int fd, uint32_t power) {
	int rv = ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, power);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_VBUS_DRAW)");
		exit(EXIT_FAILURE);
	}
}

// 查询 UDC 端点信息；失败即退出。
int usb_raw_eps_info(int fd, struct usb_raw_eps_info *info) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EPS_INFO, info);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EPS_INFO)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

// 让 EP0 stall（EBUSY 时忽略）。
void usb_raw_ep0_stall(int fd) {
	printf("ep0: stalling\n");
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0);
	if (rv < 0) {
		if (errno == EBUSY)
			return;
		perror("ioctl(USB_RAW_IOCTL_EP0_STALL)");
		exit(EXIT_FAILURE);
	}
}

// 对端点置 halt；失败即退出。
void usb_raw_ep_set_halt(int fd, int ep) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_SET_HALT, ep);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_SET_HALT)");
		exit(EXIT_FAILURE);
	}
}

/*----------------------------------------------------------------------*/

// 打印一个控制请求的各字段与解析结果（调试）。
void log_control_request(struct usb_ctrlrequest *ctrl) {
	printf("  bRequestType: 0x%02x %5s, bRequest: 0x%02x, wValue: 0x%04x,"
		" wIndex: 0x%04x, wLength: %d\n", ctrl->bRequestType,
		(ctrl->bRequestType & USB_DIR_IN) ? "(IN)" : "(OUT)",
		ctrl->bRequest, ctrl->wValue, ctrl->wIndex, ctrl->wLength);

	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		printf("  type = USB_TYPE_STANDARD\n");
		break;
	case USB_TYPE_CLASS:
		printf("  type = USB_TYPE_CLASS\n");
		break;
	case USB_TYPE_VENDOR:
		printf("  type = USB_TYPE_VENDOR\n");
		break;
	default:
		printf("  type = unknown = %d\n", (int)ctrl->bRequestType);
		break;
	}

	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		switch (ctrl->bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			printf("  req = USB_REQ_GET_DESCRIPTOR\n");
			switch (ctrl->wValue >> 8) {
			case USB_DT_DEVICE:
				printf("  desc = USB_DT_DEVICE\n");
				break;
			case USB_DT_CONFIG:
				printf("  desc = USB_DT_CONFIG\n");
				break;
			case USB_DT_STRING:
				printf("  desc = USB_DT_STRING\n");
				break;
			case USB_DT_INTERFACE:
				printf("  desc = USB_DT_INTERFACE\n");
				break;
			case USB_DT_ENDPOINT:
				printf("  desc = USB_DT_ENDPOINT\n");
				break;
			case USB_DT_DEVICE_QUALIFIER:
				printf("  desc = USB_DT_DEVICE_QUALIFIER\n");
				break;
			case USB_DT_OTHER_SPEED_CONFIG:
				printf("  desc = USB_DT_OTHER_SPEED_CONFIG\n");
				break;
			case USB_DT_INTERFACE_POWER:
				printf("  desc = USB_DT_INTERFACE_POWER\n");
				break;
			case USB_DT_OTG:
				printf("  desc = USB_DT_OTG\n");
				break;
			case USB_DT_DEBUG:
				printf("  desc = USB_DT_DEBUG\n");
				break;
			case USB_DT_INTERFACE_ASSOCIATION:
				printf("  desc = USB_DT_INTERFACE_ASSOCIATION\n");
				break;
			case USB_DT_SECURITY:
				printf("  desc = USB_DT_SECURITY\n");
				break;
			case USB_DT_KEY:
				printf("  desc = USB_DT_KEY\n");
				break;
			case USB_DT_ENCRYPTION_TYPE:
				printf("  desc = USB_DT_ENCRYPTION_TYPE\n");
				break;
			case USB_DT_BOS:
				printf("  desc = USB_DT_BOS\n");
				break;
			case USB_DT_DEVICE_CAPABILITY:
				printf("  desc = USB_DT_DEVICE_CAPABILITY\n");
				break;
			case USB_DT_WIRELESS_ENDPOINT_COMP:
				printf("  desc = USB_DT_WIRELESS_ENDPOINT_COMP\n");
				break;
			case USB_DT_PIPE_USAGE:
				printf("  desc = USB_DT_PIPE_USAGE\n");
				break;
			case USB_DT_SS_ENDPOINT_COMP:
				printf("  desc = USB_DT_SS_ENDPOINT_COMP\n");
				break;
			default:
				printf("  desc = unknown = 0x%x\n",
							ctrl->wValue >> 8);
				break;
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			printf("  req = USB_REQ_SET_CONFIGURATION\n");
			break;
		case USB_REQ_GET_CONFIGURATION:
			printf("  req = USB_REQ_GET_CONFIGURATION\n");
			break;
		case USB_REQ_SET_INTERFACE:
			printf("  req = USB_REQ_SET_INTERFACE\n");
			break;
		case USB_REQ_GET_INTERFACE:
			printf("  req = USB_REQ_GET_INTERFACE\n");
			break;
		case USB_REQ_GET_STATUS:
			printf("  req = USB_REQ_GET_STATUS\n");
			break;
		case USB_REQ_CLEAR_FEATURE:
			printf("  req = USB_REQ_CLEAR_FEATURE\n");
			break;
		case USB_REQ_SET_FEATURE:
			printf("  req = USB_REQ_SET_FEATURE\n");
			break;
		default:
			printf("  req = unknown = 0x%x\n", ctrl->bRequest);
			break;
		}
		break;
	case USB_TYPE_CLASS:
		switch (ctrl->bRequest) {
		default:
			printf("  req = unknown = 0x%x\n", ctrl->bRequest);
			break;
		}
		break;
	default:
		printf("  req = unknown = 0x%x\n", ctrl->bRequest);
		break;
	}
}

// 按类型打印一个 Raw Gadget 事件（控制事件会附带请求解析）。
void log_event(struct usb_raw_event *event) {
	switch (event->type) {
	case USB_RAW_EVENT_CONNECT:
		printf("event: connect, length: %u\n", event->length);
		break;
	case USB_RAW_EVENT_CONTROL:
		printf("event: control, length: %u\n", event->length);
		log_control_request((struct usb_ctrlrequest *)&event->data[0]);
		break;
	case USB_RAW_EVENT_SUSPEND:
		printf("event: suspend\n");
		break;
	case USB_RAW_EVENT_RESUME:
		printf("event: resume\n");
		break;
	case USB_RAW_EVENT_RESET:
		printf("event: reset\n");
		break;
	case USB_RAW_EVENT_DISCONNECT:
		printf("event: disconnect\n");
		break;
	default:
		printf("event: %d (unknown), length: %u\n", event->type, event->length);
	}
}

// 打印 UDC 全部端点的名称/地址/能力/上限（调试）。
void print_eps_info(int fd) {
	struct usb_raw_eps_info info;
	memset(&info, 0, sizeof(info));

	int num = usb_raw_eps_info(fd, &info);
	for (int i = 0; i < num; i++) {
		printf("ep #%d:\n", i);
		printf("  name: %s\n", &info.eps[i].name[0]);
		printf("  addr: %u\n", info.eps[i].addr);
		printf("  type: %s %s %s\n",
			info.eps[i].caps.type_iso ? "iso" : "___",
			info.eps[i].caps.type_bulk ? "blk" : "___",
			info.eps[i].caps.type_int ? "int" : "___");
		printf("  dir : %s %s\n",
			info.eps[i].caps.dir_in ? "in " : "___",
			info.eps[i].caps.dir_out ? "out" : "___");
		printf("  maxpacket_limit: %u\n",
			info.eps[i].limits.maxpacket_limit);
		printf("  max_streams: %u\n", info.eps[i].limits.max_streams);
	}
}
