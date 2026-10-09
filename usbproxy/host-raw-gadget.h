#include <atomic>
#include <pthread.h>
#include <mutex>
#include <deque>

#include "misc.h"

/*----------------------------------------------------------------------*/

#define UDC_NAME_LENGTH_MAX 128

// Raw Gadget 初始化参数（驱动名 + UDC 设备名 + 速度）。
struct usb_raw_init {
	__u8 driver_name[UDC_NAME_LENGTH_MAX];
	__u8 device_name[UDC_NAME_LENGTH_MAX];
	__u8 speed;
};

// Raw Gadget 上报的事件类型枚举。
enum usb_raw_event_type {
	USB_RAW_EVENT_INVALID = 0,
	USB_RAW_EVENT_CONNECT = 1,
	USB_RAW_EVENT_CONTROL = 2,
	USB_RAW_EVENT_SUSPEND = 3,
	USB_RAW_EVENT_RESUME = 4,
	USB_RAW_EVENT_RESET = 5,
	USB_RAW_EVENT_DISCONNECT = 6,
};

// Raw Gadget 事件（类型 + 长度 + 变长数据体）。
struct usb_raw_event {
	__u32		type;
	__u32		length;
	__u8		data[0];
};

// 端点读写请求头（端点号 + 标志 + 长度 + 变长数据体）。
struct usb_raw_ep_io {
	__u16		ep;
	__u16		flags;
	__u32		length;
	__u8		data[0];
};

#define USB_RAW_EPS_NUM_MAX	30
#define USB_RAW_EP_NAME_MAX	16
#define USB_RAW_EP_ADDR_ANY	0xff

// 端点能力位（支持哪些传输类型/方向）。
struct usb_raw_ep_caps {
	__u32	type_control	: 1;
	__u32	type_iso	: 1;
	__u32	type_bulk	: 1;
	__u32	type_int	: 1;
	__u32	dir_in		: 1;
	__u32	dir_out		: 1;
};

// UDC 端点的能力上限（最大包长 / 流数）。
struct usb_raw_ep_limits {
	__u16	maxpacket_limit;
	__u16	max_streams;
	__u32	reserved;
};

// 单个可用端点的信息（名字 + 地址 + 能力 + 上限）。
struct usb_raw_ep_info {
	__u8				name[USB_RAW_EP_NAME_MAX];
	__u32				addr;
	struct usb_raw_ep_caps		caps;
	struct usb_raw_ep_limits	limits;
};

// 全部可用端点信息集合。
struct usb_raw_eps_info {
	struct usb_raw_ep_info	eps[USB_RAW_EPS_NUM_MAX];
};

#define USB_RAW_IOCTL_INIT		_IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN		_IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH	_IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE		_IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ		_IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_ENABLE		_IOW('U', 5, struct usb_endpoint_descriptor)
#define USB_RAW_IOCTL_EP_DISABLE	_IOW('U', 6, __u32)
#define USB_RAW_IOCTL_EP_WRITE		_IOW('U', 7, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_READ		_IOWR('U', 8, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE		_IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW		_IOW('U', 10, __u32)
#define USB_RAW_IOCTL_EPS_INFO		_IOR('U', 11, struct usb_raw_eps_info)
#define USB_RAW_IOCTL_EP0_STALL		_IO('U', 12)
#define USB_RAW_IOCTL_EP_SET_HALT	_IOW('U', 13, __u32)
#define USB_RAW_IOCTL_EP_CLEAR_HALT	_IOW('U', 14, __u32)
#define USB_RAW_IOCTL_EP_SET_WEDGE	_IOW('U', 15, __u32)

/*----------------------------------------------------------------------*/

#define MAX_TRANSFER_SIZE 4096

// 控制事件（事件头 + setup 包）。
struct usb_raw_control_event {
	struct usb_raw_event		inner;
	struct usb_ctrlrequest		ctrl;
};

// 端点读写用的固定大小缓冲（头 + MAX_TRANSFER_SIZE 数据）。
struct usb_raw_transfer_io {
	struct usb_raw_ep_io		inner;
	char				data[MAX_TRANSFER_SIZE];
};

/*----------------------------------------------------------------------*/

// 单个端点线程的上下文（fd、端点号/描述符、接口信息、共享队列/锁/停止标志）。
struct thread_info {
	int				fd;
	int				ep_num;
	struct usb_endpoint_descriptor 	endpoint;
	__u8				device_bEndpointAddress;
	__u8				interface_number;
	__u8				interface_class;
	std::string			transfer_type;
	std::string			dir;
	std::deque<usb_raw_transfer_io> *data_queue;
	std::mutex			*data_mutex;
	std::atomic<bool>		*please_stop;
};

// 一个 gadget 端点（主机侧描述符 + 设备侧地址 + UDC 上限 + 读写线程）。
struct raw_gadget_endpoint {
	struct usb_endpoint_descriptor	endpoint;
	__u8				device_bEndpointAddress;
	__u16				udc_maxpacket_limit;
	pthread_t			thread_read;
	pthread_t			thread_write;
	struct thread_info		thread_info;
};

// 一个备用设置（接口描述符 + 端点数组）。
struct raw_gadget_altsetting {
	struct usb_interface_descriptor	interface;
	struct raw_gadget_endpoint	*endpoints;
};

// 一个接口（多个备用设置 + 当前选择）。
struct raw_gadget_interface {
	struct raw_gadget_altsetting	*altsettings;
	int				num_altsettings;
	int				current_altsetting;
};

// 一个配置（配置描述符 + 接口数组）。
struct raw_gadget_config {
	struct usb_config_descriptor	config;
	struct raw_gadget_interface	*interfaces;
};

// 转发的 gadget 设备全貌（设备描述符 + 配置数组 + 当前配置）。
struct raw_gadget_device {
	struct usb_device_descriptor 	device;
	struct raw_gadget_config	*configs;
	int				current_config;
};

extern struct raw_gadget_device host_device_desc;

/*----------------------------------------------------------------------*/

// 控制请求注入的结果标记（放行 / 忽略 / stall）。
enum usb_injection_flags {
	USB_INJECTION_FLAG_NONE,
	USB_INJECTION_FLAG_IGNORE,
	USB_INJECTION_FLAG_STALL,
};

/*----------------------------------------------------------------------*/

// 打开 /dev/raw-gadget，失败即退出进程。
int usb_raw_open();
// 对 Raw Gadget 做 INIT（UDC 忙时重试）。
void usb_raw_init(int fd, enum usb_device_speed speed,
			const char *driver, const char *device);
// 启动 gadget（RUN）。
void usb_raw_run(int fd);
// 取一个 Raw Gadget 事件。
void usb_raw_event_fetch(int fd, struct usb_raw_event *event);
// 从 EP0 读（主机 OUT 数据 / 状态阶段）。
int usb_raw_ep0_read(int fd, struct usb_raw_ep_io *io);
// 向 EP0 写（主机 IN 数据 / 状态阶段）。
int usb_raw_ep0_write(int fd, struct usb_raw_ep_io *io);
// 使能一个端点，返回其 Raw Gadget 端点号。
int usb_raw_ep_enable(int fd, struct usb_endpoint_descriptor *desc);
// 关闭一个端点。
int usb_raw_ep_disable(int fd, uint32_t num);
// 从端点读数据。
int usb_raw_ep_read(int fd, struct usb_raw_ep_io *io);
// 向端点写数据。
int usb_raw_ep_write(int fd, struct usb_raw_ep_io *io);
// 让 Raw Gadget 进入 CONFIGURE 状态。
void usb_raw_configure(int fd);
// 上报 VBUS 取电电流。
void usb_raw_vbus_draw(int fd, uint32_t power);
// 查询 UDC 可用端点信息。
int usb_raw_eps_info(int fd, struct usb_raw_eps_info *info);
// EP0 stall。
void usb_raw_ep0_stall(int fd);
// 对端点置 halt。
void usb_raw_ep_set_halt(int fd, int ep);

// 打印一个控制请求的解析结果（调试）。
void log_control_request(struct usb_ctrlrequest *ctrl);
// 打印一个 Raw Gadget 事件（调试）。
void log_event(struct usb_raw_event *event);
// 打印 UDC 端点信息（调试）。
void print_eps_info(int fd);
