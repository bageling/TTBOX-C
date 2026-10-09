#include <math.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "misc.h"

// 把 "\xNN" 转义序列就地替换成对应字节，返回解码后的字节串。
std::string hexToAscii(std::string input) {
	std::string output = input;
	size_t pos = output.find("\\x");
	while (pos != std::string::npos) {
		std::string substr = output.substr(pos + 2, 2);

		std::istringstream iss(substr);
		iss.flags(std::ios::hex);
		int i;
		iss >> i;
		output = output.replace(pos, 4, 1, char(i));
		pos = output.find("\\x");
	}
	return output;
}

// 把数字按十六进制权重解析（输入 0x81 的十进制写法 81 → 输出 129）。
int hexToDecimal(int input) {
	int output = 0;
	int i = 0;
	while (input != 0) {
		output += (input % 10) * pow(16, i);
		input /= 10;
		i++;
	}
	return output;
}

/* ---- 实时性调参 -------------------------------------------------------- */

// 读环境变量并转 int；未设置或空串时返回 def。
int usbproxy_env_int(const char *name, int def)
{
	const char *v = getenv(name);
	if (!v || !*v)
		return def;
	return atoi(v);
}

/*
 * 给**当前线程**上 SCHED_FIFO + 绑核。role_upper 决定读哪个环境变量：
 *   "ENDPOINT"        → USB_PROXY_ENDPOINT_RT_PRIORITY
 *   "EP0"             → USB_PROXY_EP0_RT_PRIORITY
 *   "MOUSE_CONTROL"   → USB_PROXY_MOUSE_CONTROL_RT_PRIORITY
 *   "THREAD"          → USB_PROXY_THREAD_RT_PRIORITY（事件泵等辅助线程）
 * 绑核用 cpu_env（为 NULL 则不绑）。prio<=0 表示不启用；cpu<0 表示不绑。
 *
 * 每条都打印实际结果 —— 只写"我设了"不算数，得能看出到底生效没有。
 */
int usbproxy_rt_apply(const char *role_upper, int prio_def,
			const char *cpu_env, int cpu_def)
{
	char name[80];
	snprintf(name, sizeof(name), "USB_PROXY_%s_RT_PRIORITY", role_upper);
	int prio = usbproxy_env_int(name, prio_def);
	int cpu = cpu_env ? usbproxy_env_int(cpu_env, cpu_def) : -1;

	if (prio > 0) {
		struct sched_param sp;
		memset(&sp, 0, sizeof(sp));
		sp.sched_priority = prio;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
			fprintf(stderr, "[rt] %s: SCHED_FIFO(%d) failed: %s\n",
				role_upper, prio, strerror(errno));
		else
			printf("[rt] %s: SCHED_FIFO priority %d\n", role_upper, prio);
	}

	if (cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) != 0)
			fprintf(stderr, "[rt] %s: CPU affinity %d failed: %s\n",
				role_upper, cpu, strerror(errno));
		else
			printf("[rt] %s: pinned to CPU %d\n", role_upper, cpu);
	}

	return prio;
}

/*
 * 锁住进程地址空间：转发路径上的一次换页就够丢一帧，
 * 在 1ms（乃至 125µs）节奏下这是肉眼可见的抖动。
 * 失败只告警不退出 —— 没有 CAP_IPC_LOCK 时也不该让服务起不来。
 */
void usbproxy_mlockall(void)
{
	if (usbproxy_env_int("USB_PROXY_MLOCK", 1) <= 0) {
		printf("[rt] mlockall: disabled by USB_PROXY_MLOCK=0\n");
		return;
	}

	// 自己把两个 rlimit 顶上去，别依赖 unit 里的 LimitMEMLOCK/LimitRTPRIO ——
	// 客户机上跑的可能是镜像里那份旧 unit（OTA 不保证覆盖单元文件），
	// 指望外部配置 = 把生效条件放在自己控制之外。
	struct rlimit rl;
	if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0 &&
	    (rl.rlim_cur != RLIM_INFINITY)) {
		struct rlimit want = rl;
		want.rlim_cur = RLIM_INFINITY;
		want.rlim_max = RLIM_INFINITY;
		if (setrlimit(RLIMIT_MEMLOCK, &want) != 0 && verbose_level)
			fprintf(stderr, "[rt] raise RLIMIT_MEMLOCK failed: %s\n",
				strerror(errno));
	}
	if (getrlimit(RLIMIT_RTPRIO, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
		struct rlimit want = rl;
		want.rlim_cur = RLIM_INFINITY;
		want.rlim_max = RLIM_INFINITY;
		setrlimit(RLIMIT_RTPRIO, &want);
	}

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		fprintf(stderr, "[rt] mlockall failed: %s (转发路径可能换页抖动)\n",
			strerror(errno));
	else
		printf("[rt] mlockall: locked\n");
}
