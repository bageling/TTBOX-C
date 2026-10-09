// HidRuntime.cpp — HID Runtime 实现
/*
 * TTBOX 文件说明
 *
 * 文件：HidRuntime.cpp
 *
 * 作用：
 *   HID（人机交互设备）运行时管理。
 *   负责加载和卸载 HID 设备包，管理 HID 输出。
 *
 * 小白理解：
 *   HID 就是 USB 鼠标、键盘这类设备的统称。
 *   这个模块负责把 TTBOX 生成的鼠标指令包装成 USB 协议，
 *   然后通过 USB 线发送给电脑，让电脑以为是真的鼠标在动。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "hid/HidRuntime.hpp"

#if defined(_WIN32)
namespace ttbox::core {
// 状态名（Windows 分支统一返回 unsupported）
const char* hid_runtime_status_name(HidRuntimeStatus) { return "unsupported"; }
// Windows 无 HID gadget 支持：start 直接失败
bool HidRuntime::start(std::string* error) {
    if (error) *error = "Windows 不支持 HID Runtime";
    return false;
}
// Windows 无资源可释放：空实现
void HidRuntime::stop() {}
// Windows 无鼠标状态：返回默认
MouseState HidRuntime::get_mouse_state() const { return {}; }
// Windows 无键盘状态：返回默认
KeyboardState HidRuntime::get_keyboard_state() const { return {}; }
// Windows 无指标：返回默认
HidRuntimeMetrics HidRuntime::get_metrics() const { return {}; }
// Windows 无 gadget：直接 false
bool HidRuntime::setup_gadget_if_needed(std::string*) { return false; }
// Windows 无 hidraw：直接 false
bool HidRuntime::find_hidraw(const std::string&, const char*, std::string*) const { return false; }
// Windows 无 forwarder：空实现
void HidRuntime::parse_state_from_forwarders() {}
}  // namespace ttbox::core
#else

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "common/Logger.hpp"
#include "hid/UdcResolve.hpp"  // P8：UDC 名解析唯一口径（不写死硬件编号）

namespace ttbox::core {

// 状态枚举转字符串（供日志/接口）
const char* hid_runtime_status_name(HidRuntimeStatus s) {
    switch (s) {
        case HidRuntimeStatus::kRunning: return "running";
        case HidRuntimeStatus::kError: return "error";
        default: return "stopped";
    }
}

namespace {

// 路径是否存在
bool path_exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

// 列举 /dev/hidraw* 或 /dev/hidg*（按前缀）
std::vector<std::string> list_dev(const std::string& prefix) {
    std::vector<std::string> out;
    DIR* d = ::opendir("/dev");
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (std::strncmp(e->d_name, prefix.c_str(), prefix.size()) == 0) {
            out.push_back("/dev/" + std::string(e->d_name));
        }
    }
    ::closedir(d);
    return out;
}

}  // namespace

// 定位键盘/鼠标 hidraw：优先用配置路径；否则枚举 /dev/hidraw* 按 input 名称或
// HID_PHYS(/input0) 关键词匹配；无匹配退化为首个。
bool HidRuntime::find_hidraw(const std::string& configured, const char* keyword,
                             std::string* out) const {
    if (!configured.empty() && path_exists(configured)) {
        *out = configured;
        return true;
    }
    const auto devs = list_dev("hidraw");
    const std::string kw = keyword ? keyword : "";
    // 匹配规则：
    //   Mouse     → input 名称含 "Mouse"
    //   Keyboard  → input 名称含 "Keyboard"；或 HID_PHYS 以 "input0" 结尾
    //               （USB HID 惯例：interface 0 = 键盘，罗技 Receiver 亦是）
    const bool is_kb = (kw == "Keyboard");
    for (const auto& dev : devs) {
        const std::string name = dev.substr(std::strlen("/dev/"));
        const std::string dev_base = "/sys/class/hidraw/" + name + "/device";
        // HID_PHYS 兜底（键盘）：usb-.../input0
        if (is_kb) {
            std::ifstream uf(dev_base + "/uevent");
            std::string u;
            std::getline(uf, u);  // 第一行不是 PHYS，直接读全部
            uf.clear();
            uf.seekg(0);
            std::string phys;
            std::string line;
            while (std::getline(uf, line)) {
                if (line.rfind("HID_PHYS=", 0) == 0) phys = line.substr(9);
            }
            if (phys.size() >= 7 && phys.compare(phys.size() - 7, 7, "/input0") == 0) {
                *out = dev;
                return true;
            }
        }
        // input 名称关键词匹配
        const std::string base = dev_base + "/input";
        DIR* id = ::opendir(base.c_str());
        if (!id) continue;
        bool match = false;
        struct dirent* ie;
        while ((ie = ::readdir(id)) != nullptr) {
            if (std::strncmp(ie->d_name, "input", 5) != 0) continue;
            std::ifstream nf(base + "/" + ie->d_name + "/name");
            std::string nm;
            std::getline(nf, nm);
            if (nm.find(kw) != std::string::npos) { match = true; break; }
        }
        ::closedir(id);
        if (match) {
            *out = dev;
            return true;
        }
    }
    if (!devs.empty()) {
        *out = devs[0];  // 无匹配：退化为第一个（保持可用）
        return true;
    }
    return false;
}

// 确保 USB gadget 已建立：已绑定（UDC state=configured/attached）则跳过，
// 否则调用 <root>/bin/a9_setup_hid_gadget.sh enable。
bool HidRuntime::setup_gadget_if_needed(std::string* error) {
    // 已绑定则跳过（cfg_.udc 已由 start() 解析；为空 = 板上没枚举到 UDC，
    // 直接跳到下面的启动脚本，由脚本自行解析，不在这里假装成功）
    if (!cfg_.udc.empty()) {
        const std::string udc_state = std::string("/sys/class/udc/") + cfg_.udc + "/state";
        std::ifstream st(udc_state);
        std::string state;
        std::getline(st, state);
        if (state == "configured" || state == "attached") return true;
    }

    // 通过脚本建立 gadget（独立于 AI Runtime）
    const std::string script = root_ + "/bin/a9_setup_hid_gadget.sh";
    if (path_exists(script)) {
        const std::string cmd = "bash " + script + " enable >/dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        if (rc != 0) {
            if (error) *error = "gadget 启动脚本失败 rc=" + std::to_string(rc);
            return false;
        }
        return true;
    }
    if (error) *error = "gadget 未配置且无启动脚本: " + script;
    return false;
}

// 启动 HID Runtime：解析 root/UDC、确保 gadget、枚举设备并拉起键盘/鼠标 forwarder。
bool HidRuntime::start(std::string* error) {
    if (status_.load() == HidRuntimeStatus::kRunning) return true;
    if (root_.empty()) {
        // P1 环境变量 TTBOX_HID_ROOT：与 HidPackageRegistry 同一覆盖链，
        // 此前只能靠编译期宏（换目录必须重编译），现在运行期即可覆盖。
        if (const char* env_root = std::getenv("TTBOX_HID_ROOT")) {
            if (*env_root) root_ = env_root;
        }
    }
    if (root_.empty()) root_ = std::string(TTBOX_PROJECT_ROOT) + "/hid";  // P3 编译期兜底

    // 配置：优先注入，否则从包 config 加载
    if (cfg_.gadget_name.empty()) {
        cfg_ = HidPackageConfig::load(root_ + "/config/hid_config.json", nullptr);
    }
    // UDC 名不写死硬件编号（P8）：配置未给则按 env → /sys/class/udc 枚举解析，
    // 与 usbproxy/board/run-ttbox-usb-proxy.sh 同一覆盖链（USB_PROXY_DEVICE）。
    cfg_.udc = resolve_udc(cfg_.udc);
    if (cfg_.udc.empty()) {
        TTBOX_LOG_WARN("未解析到 UDC（/sys/class/udc 为空）：跳过\"已绑定\"快路径，交启动脚本处理");
    }

    // 1. 确保 gadget（configfs）
    if (!setup_gadget_if_needed(error)) {
        status_ = HidRuntimeStatus::kError;
        return false;
    }
    // 2. 枚举输入设备 + 启动 forwarder（键盘/鼠标各一路）
    std::string kb_hidraw, ms_hidraw;
    const bool kb_ok = cfg_.keyboard_enabled &&
                       find_hidraw(cfg_.keyboard_hidraw, "Keyboard", &kb_hidraw) &&
                       path_exists(cfg_.keyboard_hidg);
    const bool ms_ok = cfg_.mouse_enabled &&
                       find_hidraw(cfg_.mouse_hidraw, "Mouse", &ms_hidraw) &&
                       path_exists(cfg_.mouse_hidg);

    if (!kb_ok && !ms_ok) {
        if (error) *error = "无可用 HID 输入设备（hidraw 未枚举）且/或 gadget 不可用";
        status_ = HidRuntimeStatus::kError;
        return false;
    }
    std::vector<std::unique_ptr<HidForwarder>> fwds;
    if (kb_ok) {
        auto f = std::make_unique<HidForwarder>();
        HidForwarder::Params p;
        p.hidraw_path = kb_hidraw;
        p.hidg_path = cfg_.keyboard_hidg;
        p.kind = HidKind::kKeyboard;
        p.cpu = cfg_.cpu_affinity;
        p.raw_pass = false;  // 重编码为 boot 键盘 8 字节
        std::string ferr;
        if (!f->start(p, &ferr)) {
            TTBOX_LOG_WARN("键盘 forwarder 启动失败: " + ferr);
        } else {
            fwds.push_back(std::move(f));
        }
    }
    if (ms_ok) {
        auto f = std::make_unique<HidForwarder>();
        HidForwarder::Params p;
        p.hidraw_path = ms_hidraw;
        p.hidg_path = cfg_.mouse_hidg;
        p.kind = HidKind::kMouse;
        p.cpu = cfg_.cpu_affinity;
        p.raw_pass = false;  // 重编码为 boot 鼠标 4 字节 + 过滤非鼠标报告
        std::string ferr;
        if (!f->start(p, &ferr)) {
            TTBOX_LOG_WARN("鼠标 forwarder 启动失败: " + ferr);
        } else {
            fwds.push_back(std::move(f));
        }
    }
    if (fwds.empty()) {
        if (error) *error = "所有 forwarder 启动失败";
        status_ = HidRuntimeStatus::kError;
        return false;
    }
    forwarders_ = std::move(fwds);
    status_ = HidRuntimeStatus::kRunning;
    return true;
}

// 停止所有 forwarder 并复位状态为 stopped。
void HidRuntime::stop() {
    if (status_.load() == HidRuntimeStatus::kStopped && forwarders_.empty()) return;
    for (auto& f : forwarders_) f->stop();
    forwarders_.clear();
    status_ = HidRuntimeStatus::kStopped;
}

// 返回最近鼠标状态快照（加锁拷贝）
MouseState HidRuntime::get_mouse_state() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return last_mouse_;
}

// 返回最近键盘状态快照（加锁拷贝）
KeyboardState HidRuntime::get_keyboard_state() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return last_keyboard_;
}

// 汇总各 forwarder 统计为运行时指标快照。
HidRuntimeMetrics HidRuntime::get_metrics() const {
    HidRuntimeMetrics m;
    m.status = status_.load();
    for (const auto& f : forwarders_) {
        const auto& s = f->stats();
        m.rx_reports += s.rx_reports.load();
        m.tx_reports += s.tx_reports.load();
        m.drop += s.push_drops.load();
        m.backpressure += s.tx_backpressure.load();
        if (s.latency_us.count() > 0) {
            m.latency_avg_us = s.latency_us.avg();
            m.latency_p50_us = s.latency_us.percentile(50);
            m.latency_p95_us = s.latency_us.percentile(95);
            m.latency_p99_us = s.latency_us.percentile(99);
        }
        if (s.rx_interval_us.count() > 1) {
            const double a = s.rx_interval_us.avg();
            if (a > 0) m.report_rate_hz = 1e6 / a;
        }
        m.max_queue_depth += s.max_queue_depth.load();
    }
    return m;
}

}  // namespace ttbox::core

#endif  // !_WIN32
