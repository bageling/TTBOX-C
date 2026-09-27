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

// 从 hidraw 的 device 链接解析出 (usb接口号, usb设备sysfs目录, 接口目录名)。
// realpath(...) → .../usb3/3-1/3-1:1.2/0003:373B:10C9.0020
//   = usb设备目录 / 接口目录 / HID设备目录
// hidraw 的 device 链接指向 HID 设备目录，向上两层分别是接口目录和 usb 设备目录。
bool resolve_hidraw_iface(const char* sysfs_link, uint8_t* iface_num,
			  char* usb_dev_dir, size_t usb_dev_dir_cap,
			  char* usb_iface_name, size_t usb_iface_name_cap)
{
	char real[PATH_MAX];
	if (!realpath(sysfs_link, real))
		return false;

	// 分层拷贝，避免 dirname() 改写原串。
	char hid_dir[PATH_MAX], iface_dir[PATH_MAX], dev_dir[PATH_MAX];
	snprintf(hid_dir, sizeof(hid_dir), "%s", real);
	snprintf(iface_dir, sizeof(iface_dir), "%s", dirname(hid_dir));
	snprintf(dev_dir, sizeof(dev_dir), "%s", dirname(iface_dir));
	if (strcmp(dev_dir, "/") == 0 || strcmp(dev_dir, ".") == 0)
		return false;

	const char* iface_name = basename(iface_dir);   // "3-1:1.2"
	const char* colon = strrchr(iface_name, ':');
	if (!colon || strncmp(colon, ":1.", 3) != 0)
		return false;
	const int n = atoi(colon + 3);
	if (n < 0 || n >= kHidRawMaxIfaces)
		return false;
	*iface_num = static_cast<uint8_t>(n);

	snprintf(usb_iface_name, usb_iface_name_cap, "%s", iface_name);
	snprintf(usb_dev_dir, usb_dev_dir_cap, "%s", dev_dir);
	return true;
}

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

int hidraw_prefetch_descriptors(uint8_t bus, uint8_t addr)
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
		if (read_sysfs_int(dev_dir, "busnum") != bus ||
		    read_sysfs_int(dev_dir, "devnum") != addr)
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

int hidraw_refetch_via_rebind(uint8_t bus, uint8_t addr)
{
	// 记下现有 hidraw 节点，rebind 后「新出现的」才属于本次绑定。
	snapshot_known_hidraw();

	// 找到目标设备（bus/addr 匹配）的所有 USB 接口目录。
	char iface_names[kHidRawMaxIfaces][128];
	int iface_nums[kHidRawMaxIfaces];
	int found = 0;
	DIR* d = opendir("/sys/bus/usb/devices");
	if (!d)
		return 0;
	struct dirent* e;
	while ((e = readdir(d)) != nullptr && found < kHidRawMaxIfaces) {
		const char* colon = strrchr(e->d_name, ':');
		if (!colon || strncmp(colon, ":1.", 3) != 0)
			continue;
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s",
			 e->d_name);
		char dev_dir[PATH_MAX];
		snprintf(dev_dir, sizeof(dev_dir), "%s", path);
		char* slash = strrchr(dev_dir, '/');
		if (!slash)
			continue;
		*slash = '\0';
		if (read_sysfs_int(dev_dir, "busnum") != bus ||
		    read_sysfs_int(dev_dir, "devnum") != addr)
			continue;
		const int n = atoi(colon + 3);
		if (n < 0 || n >= kHidRawMaxIfaces || g_cache[n].valid)
			continue;
		snprintf(iface_names[found], sizeof(iface_names[0]), "%s",
			 e->d_name);
		iface_nums[found] = n;
		++found;
	}
	closedir(d);

	int got = 0;
	for (int i = 0; i < found; ++i) {
		// 短暂绑回 usbhid：内核在「正常枚举上下文」里重新读报告描述符，
		// quirk 固件只认这一口。读完立刻解绑（后面 usb-proxy 自己 detach/claim）。
		if (write_sysfs("/sys/bus/usb/drivers/usbhid/bind",
				iface_names[i]) != 0) {
			printf("hidraw: rebind %s failed (usbhid unavailable?)\n",
			       iface_names[i]);
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
				if (ifn ==
					    static_cast<uint8_t>(iface_nums[i]) &&
				    strcmp(iname, iface_names[i]) == 0) {
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
				new_node, g_cache[iface_nums[i]].data,
				sizeof(g_cache[iface_nums[i]].data));
			if (len > 0) {
				g_cache[iface_nums[i]].valid = true;
				g_cache[iface_nums[i]].len = len;
				++got;
				printf("hidraw: rebind %s -> iface %u report descriptor %u bytes\n",
				       iface_names[i], iface_nums[i], len);
			}
		}
		// 无论成败都解绑，交还给 usb-proxy 的 detach/claim 流程。
		char unbind_value[128];
		snprintf(unbind_value, sizeof(unbind_value), "%s",
			 iface_names[i]);
		write_sysfs("/sys/bus/usb/drivers/usbhid/unbind",
			    unbind_value);
	}
	return got;
}

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

void hidraw_clear()
{
	memset(g_cache, 0, sizeof(g_cache));
}

HidLayoutAction hid_layout_decide(bool iface_ready, bool prefetch_available,
                                  uint32_t class_attempts, uint32_t stall_rounds)
{
	if (iface_ready)
		return HidLayoutAction::kWait;
	if (prefetch_available)
		return HidLayoutAction::kFeedPrefetch;
	if (class_attempts == 0)
		return HidLayoutAction::kTryClassRequest;
	if (stall_rounds >= 10)
		return HidLayoutAction::kFailOpen;
	return HidLayoutAction::kWait;
}

}  // namespace ttbox_usbproxy
