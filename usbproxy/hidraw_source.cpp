// hidraw_source.cpp — 见 hidraw_source.hpp 头注释
#include "hidraw_source.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ttbox_usbproxy {
namespace {

// 单个接口的缓存描述符（数据 + 是否有效 + 长度）。
struct PrefetchedDesc {
	bool valid = false;
	uint32_t len = 0;
	uint8_t data[4096];
};

// iface -> 缓存。g_mutex 只保护写读并发；连接期写入、worker 期读。
PrefetchedDesc g_cache[kHidRawMaxIfaces];

// 上一次 rebind 前的 hidraw 快照（用来发现 rebind 新长出来的 hidraw 节点）。
char g_known_hidraw[64][64];
int g_known_hidraw_count = 0;

// 记下当前 /sys/class/hidraw 里的节点名，供 rebind 后辨认"新出现"的节点。
void snapshot_known_hidraw()
{
	g_known_hidraw_count = 0;
	DIR* d = opendir("/sys/class/hidraw");
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		if (e->d_name[0] == '.')
			continue;
		if (g_known_hidraw_count < 64)
			snprintf(g_known_hidraw[g_known_hidraw_count++],
				 sizeof(g_known_hidraw[0]), "%s", e->d_name);
	}
	closedir(d);
}

// 判断某 hidraw 节点是否不在快照里（= rebind 后新长出来的）。
bool is_new_hidraw(const char* name)
{
	for (int i = 0; i < g_known_hidraw_count; ++i)
		if (strcmp(g_known_hidraw[i], name) == 0)
			return false;
	return true;
}

// 读一个 hidrawN 节点的缓存描述符。成功返回长度（>0），失败返回 0。
// HIDIOCGRDESC 返回的是内核 probe 时缓存的描述符，不发任何 USB 请求。
uint32_t read_hidraw_cached(const char* node, uint8_t* out, uint32_t cap)
{
	char path[128];
	snprintf(path, sizeof(path), "/dev/%s", node);
	const int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;

	int size = 0;
	uint32_t got = 0;
	if (ioctl(fd, HIDIOCGRDESCSIZE, &size) == 0 && size > 0 &&
	    size <= static_cast<int>(cap)) {
		struct hidraw_report_descriptor rd;
		memset(&rd, 0, sizeof(rd));
		rd.size = static_cast<unsigned int>(size);
		if (ioctl(fd, HIDIOCGRDESC, &rd) == 0) {
			memcpy(out, rd.value, static_cast<size_t>(size));
			got = static_cast<uint32_t>(size);
		}
	}
	close(fd);
	return got;
}

// 判断一条 hidraw 是否属于目标 USB 设备，并解出它挂在哪个接口上。
// 做法：从 realpath(…/device) 逐级向上找名字里含 ":1.<数字>" 的接口目录分量，
// 它的父亲就是 USB 设备目录。比原先"硬上两层"稳：不同内核的子目录层级不同，
// 之前那版因而在板端一个都没匹配上（1.5.57 日志 prefetch 0/3）。
bool resolve_hidraw_iface(const char* sysfs_link, uint8_t* iface_num,
			  char* usb_dev_dir, size_t usb_dev_dir_cap,
			  char* usb_iface_name, size_t usb_iface_name_cap)
{
	char real[PATH_MAX];
	if (!realpath(sysfs_link, real))
		return false;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s", real);
	for (;;) {
		char cur[PATH_MAX];
		snprintf(cur, sizeof(cur), "%s", path);
		const char* name = basename(cur);
		const char* colon = strstr(name, ":1.");
		if (colon) {
			bool all_digits = true;
			for (const char* p = colon + 3; *p; ++p) {
				if (*p < '0' || *p > '9') { all_digits = false; break; }
			}
			if (all_digits && *(colon + 3)) {
				const int n = atoi(colon + 3);
				if (n >= 0 && n < kHidRawMaxIfaces) {
					*iface_num = static_cast<uint8_t>(n);
					snprintf(usb_iface_name, usb_iface_name_cap, "%s", name);
					char parent[PATH_MAX];
					snprintf(parent, sizeof(parent), "%s", path);
					snprintf(usb_dev_dir, usb_dev_dir_cap, "%s", dirname(parent));
					return true;
				}
			}
		}
		// 往上走一层
		char parent[PATH_MAX];
		snprintf(parent, sizeof(parent), "%s", path);
		dirname(parent);
		if (strcmp(parent, path) == 0)
			return false;
		snprintf(path, sizeof(path), "%s", parent);
	}
}

// 读 sysfs 目录下某文件的十进制整数；读不到返回 -1。
int read_sysfs_int(const char* dir, const char* name)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE* f = fopen(path, "r");
	if (!f)
		return -1;
	int v = -1;
	if (fscanf(f, "%d", &v) != 1)
		v = -1;
	fclose(f);
	return v;
}

// 读 sysfs 目录下某文件的十六进制整数（idVendor/idProduct）；读不到返回 -1。
int read_sysfs_hex16(const char* dir, const char* name)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE* f = fopen(path, "r");
	if (!f)
		return -1;
	int v = -1;
	if (fscanf(f, "%x", &v) != 1)
		v = -1;
	fclose(f);
	return v;
}

// 往 sysfs 文件写字符串值（用于 usbhid bind/unbind）；成功返回 0。
int write_sysfs(const char* path, const char* value)
{
	FILE* f = fopen(path, "w");
	if (!f)
		return -1;
	const int n = fprintf(f, "%s", value);
	fclose(f);
	return (n < 0) ? -1 : 0;
}

}  // namespace

// 扫描 /sys/class/hidraw，把属于 (bus,addr) 或 VID:PID 设备的描述符抄进缓存。返回读到的接口数。
int hidraw_prefetch_descriptors(uint8_t bus, uint8_t addr,
                                uint16_t vendor, uint16_t product)
{
	int got = 0;
	DIR* d = opendir("/sys/class/hidraw");
	if (!d)
		return 0;
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		if (e->d_name[0] == '.')
			continue;
		char link[PATH_MAX];
		snprintf(link, sizeof(link), "/sys/class/hidraw/%s/device",
			 e->d_name);
		uint8_t iface = 0;
		char dev_dir[PATH_MAX];
		char iface_name[128];
		if (!resolve_hidraw_iface(link, &iface, dev_dir,
					  sizeof(dev_dir), iface_name,
					  sizeof(iface_name)))
			continue;
		// 归属判定：优先 bus/devnum；读不到（或地址已变）时退化为 VID:PID。
		const int b = read_sysfs_int(dev_dir, "busnum");
		const int a = read_sysfs_int(dev_dir, "devnum");
		const int v = read_sysfs_hex16(dev_dir, "idVendor");
		const int p = read_sysfs_hex16(dev_dir, "idProduct");
		const bool by_addr = (b >= 0 && a >= 0) && (b == bus && a == addr);
		const bool by_id = (v >= 0 && p >= 0) &&
				   (v == vendor && p == product);
		if (!by_addr && !by_id)
			continue;  // 别人的设备
		if (g_cache[iface].valid)
			continue;  // 已经有了
		const uint32_t len =
			read_hidraw_cached(e->d_name, g_cache[iface].data,
					   sizeof(g_cache[iface].data));
		if (len > 0) {
			g_cache[iface].valid = true;
			g_cache[iface].len = len;
			++got;
			printf("hidraw: iface %u <- %s cached report descriptor %u bytes\n",
			       iface, e->d_name, len);
		}
	}
	closedir(d);
	return got;
}

// 兜底重取：把目标 HID 接口短暂绑回 usbhid，让内核重读描述符后再解绑。返回新读到的接口数。
int hidraw_refetch_via_rebind(uint8_t bus, uint8_t addr,
                              uint16_t vendor, uint16_t product)
{
	(void)bus; (void)addr;
	// 记下现有 hidraw 节点，rebind 后「新出现的」才属于本次绑定。
	snapshot_known_hidraw();

	// 找目标 USB 设备目录（按 VID:PID；同型号视为等价，描述符本就是同一份）。
	char dev_name[128] = "";
	DIR* d = opendir("/sys/bus/usb/devices");
	if (!d)
		return 0;
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		if (e->d_name[0] == '.' || strchr(e->d_name, ':'))
			continue;  // 接口目录形如 3-1:1.2，跳过
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s",
			 e->d_name);
		if (read_sysfs_hex16(path, "idVendor") != vendor ||
		    read_sysfs_hex16(path, "idProduct") != product)
			continue;
		snprintf(dev_name, sizeof(dev_name), "%s", e->d_name);
		break;
	}
	closedir(d);
	if (!dev_name[0]) {
		printf("hidraw: rebind skipped (target device dir not found)\n");
		return 0;
	}

	int got = 0;
	for (int ifn_expected = 0; ifn_expected < kHidRawMaxIfaces; ++ifn_expected) {
		if (g_cache[ifn_expected].valid)
			continue;
		char iface_name[160];
		snprintf(iface_name, sizeof(iface_name), "%s:1.%d", dev_name, ifn_expected);
		char iface_path[PATH_MAX];
		snprintf(iface_path, sizeof(iface_path),
			 "/sys/bus/usb/devices/%s", iface_name);
		if (read_sysfs_int(iface_path, "bInterfaceClass") != 0x03)
			continue;  // 非 HID 接口（也可能不存在）
		// 短暂绑回 usbhid：内核在「正常枚举上下文」里重新读报告描述符，
		// quirk 固件只认这一口。读完立刻解绑（后面 usb-proxy 自己 detach/claim）。
		if (write_sysfs("/sys/bus/usb/drivers/usbhid/bind",
				iface_name) != 0) {
			printf("hidraw: rebind %s failed (already bound or usbhid unavailable?)\n",
			       iface_name);
			continue;
		}
		// 轮询等新 hidraw 节点出现（最多 1s）。
		char new_node[64] = "";
		for (int t = 0; t < 20; ++t) {
			usleep(50 * 1000);
			DIR* hd = opendir("/sys/class/hidraw");
			if (!hd)
				continue;
			struct dirent* he;
			while ((he = readdir(hd)) != nullptr) {
				if (he->d_name[0] == '.')
					continue;
				if (!is_new_hidraw(he->d_name))
					continue;
				// 确认这个新节点属于本接口
				char link[PATH_MAX];
				snprintf(link, sizeof(link),
					 "/sys/class/hidraw/%s/device",
					 he->d_name);
				uint8_t ifn = 0;
				char dd[PATH_MAX];
				char iname[128];
				if (!resolve_hidraw_iface(link, &ifn, dd,
							  sizeof(dd), iname,
							  sizeof(iname)))
					continue;
				if (ifn == static_cast<uint8_t>(ifn_expected) &&
				    strcmp(iname, iface_name) == 0) {
					snprintf(new_node, sizeof(new_node),
						 "%s", he->d_name);
					break;
				}
			}
			closedir(hd);
			if (new_node[0])
				break;
		}
		if (new_node[0]) {
			const uint32_t len = read_hidraw_cached(
				new_node, g_cache[ifn_expected].data,
				sizeof(g_cache[ifn_expected].data));
			if (len > 0) {
				g_cache[ifn_expected].valid = true;
				g_cache[ifn_expected].len = len;
				++got;
				printf("hidraw: rebind %s -> iface %d report descriptor %u bytes\n",
				       iface_name, ifn_expected, len);
			}
		}
		// 无论成败都解绑，交还给 usb-proxy 的 detach/claim 流程。
		char unbind_value[128];
		snprintf(unbind_value, sizeof(unbind_value), "%s", iface_name);
		write_sysfs("/sys/bus/usb/drivers/usbhid/unbind",
			    unbind_value);
	}
	return got;
}

// 取某接口的缓存描述符（指针 + 长度）；无缓存返回 false。
bool hidraw_get(uint8_t iface, const uint8_t** data, uint32_t* len)
{
	if (iface >= kHidRawMaxIfaces || !g_cache[iface].valid)
		return false;
	if (data)
		*data = g_cache[iface].data;
	if (len)
		*len = g_cache[iface].len;
	return true;
}

// 清空全部接口的缓存（换设备重连时调用）。
void hidraw_clear()
{
	memset(g_cache, 0, sizeof(g_cache));
}

// 布局学习的纯决策函数：依据就绪/缓存/尝试次数/停滞轮数，决定本轮动作（详见头文件规则）。
HidLayoutAction hid_layout_decide(bool iface_ready, bool prefetch_available,
                                  uint32_t class_attempts, uint32_t stall_rounds,
                                  bool class_fetch_allowed)
{
	if (iface_ready)
		return HidLayoutAction::kWait;
	if (prefetch_available)
		return HidLayoutAction::kFeedPrefetch;
	if (!class_fetch_allowed)
		return HidLayoutAction::kFailOpen;  // 不许问设备 ⇒ 立刻保透传
	if (class_attempts == 0)
		return HidLayoutAction::kTryClassRequest;
	if (stall_rounds >= 10)
		return HidLayoutAction::kFailOpen;
	return HidLayoutAction::kWait;
}

}  // namespace ttbox_usbproxy
