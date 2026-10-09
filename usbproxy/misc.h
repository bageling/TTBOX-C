#pragma once
/* misc.h 被 device-libusb.h 与多个 .cpp 同时包含，必须有 include guard ——
   否则同一 TU 里重复展开会直接报重定义（新增 fs_ms_to_hs_interval 后踩到）。 */
#include <assert.h>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <stdio.h>
#include <string>
#include <getopt.h>
#include <signal.h>
#include <chrono>
#include <sys/stat.h>
#include <linux/usb/ch9.h>
#include <jsoncpp/json/json.h>

extern int verbose_level;
extern bool please_stop_ep0;
extern std::atomic<bool> please_stop_eps;

extern bool injection_enabled;
extern std::string injection_file;
extern Json::Value injection_config;

extern bool customized_config_enabled;
extern bool reset_device_before_proxy;
extern bool bmaxpacketsize0_must_greater_than_64;
extern bool hid_passthrough_compat;
extern bool set_config_ack_before_configure;
extern int iso_batch_size;

// 把 "\xFF\x0A" 形式的转义串解码成原始字节串（injection.json 的 content_pattern/replacement 用）。
std::string hexToAscii(std::string input);
// 把"用十进制写的十六进制"（如 81 代表 0x81）转成十进制数值（injection.json 的地址/请求字段用）。
int hexToDecimal(int input);

/*
 * bInterval 的单位**随速度域变**：全速/低速是毫秒，高速是 2^(n-1) 个 125µs 微帧。
 * 物理设备跑全速、而 gadget 以高速连电脑时，描述符必须换算 —— 否则一个"1ms"的
 * 端点在电脑侧会被解释成 125µs（8kHz），语义差 8 倍。
 *
 * 中断端点与等时端点都要换算；批量端点不使用 bInterval，别传进来。
 * 1 ms = 8 个微帧 = 2^(4-1) ⇒ 返回 4。上限 16（规范上限 2^15 微帧）。
 */
static inline uint8_t fs_ms_to_hs_interval(uint8_t fs_interval)
{
	if (fs_interval <= 1)
		return 4;
	int val = (int)fs_interval * 8;		/* ms -> 125µs 微帧数 */
	uint8_t n = 1;
	while (val > 1) {
		val >>= 1;
		n++;
	}
	if (n > 16)
		n = 16;
	return n;
}

/*
 * 转发路径的实时性调参（2026-09-22）。
 * 端点转发/EP0/鼠标控制各自 SCHED_FIFO + 绑核，配合 mlockall 消除换页抖动。
 * 一切由环境变量驱动，默认给一套保守值；置 0 即关（排障时第一件事就是全关）。
 * 返回实际生效的优先级（<0 表示未启用）。
 */
int usbproxy_env_int(const char *name, int def);
int usbproxy_rt_apply(const char *role_upper, int prio_def,
			const char *cpu_env, int cpu_def);
void usbproxy_mlockall(void);
