// EdidApply.cpp — 见 EdidApply.hpp 的文件头说明。
#include "edid/EdidApply.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "common/Json.hpp"
#include "edid/EdidBuilder.hpp"
#include "edid/EdidConfig.hpp"
#include "edid/EdidValidator.hpp"

namespace ttbox::core::edid {

namespace {

// 拼接两段路径（a 为空返回 b；a 以 '/' 结尾则不再重复补 '/'）。
std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (!a.empty() && a.back() == '/') return a + b;
    return a + "/" + b;
}

// 单引号包裹（路径受控：来自配置与环境，不含单引号）。用于拼 shell 命令行。
std::string sq(const std::string& s) { return "'" + s + "'"; }

// 跑命令并捕获输出（含二进制 —— v4l2-ctl --get-edid 输出 256 字节）。
// ★ Windows 的 popen 是文本模式、会做 \r\n 转换 ⇒ 本函数**只适用于 Linux**（板端）。
//   本机（MSYS）编得过但跑不了 EDID 注入，这正是我们想要的：本机只验编译。
bool run_capture(const std::string& cmd, std::string* out) {
    FILE* f = ::popen(cmd.c_str(), "r");
    if (f == nullptr) return false;
    char buf[4096];
    std::string s;
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    const int rc = ::pclose(f);  // 不解析 WEXITSTATUS（需 sys/wait.h）
    if (out != nullptr) *out = s;
    return rc == 0;
}

// run_capture 的只判成败包装（不取输出）。
bool run_quiet(const std::string& cmd) { return run_capture(cmd, nullptr); }

// 读文本文件内容；打不开返回空串。
std::string read_text_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 读二进制文件内容到 out；打不开返回 false。
bool read_bytes_file(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

// sysfs 写（HPD）。用 open/write 而非 ofstream —— 信号处理函数里也要调，
// 而 ofstream 不是 async-signal-safe。
bool write_sysfs(const std::string& path, const std::string& value) {
    if (path.empty()) return true;  // 脚本： [ -n "$HPD_STATUS" ] || return 0
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const std::string s = value + "\n";
    const ssize_t w = ::write(fd, s.data(), s.size());
    ::close(fd);
    return w == static_cast<ssize_t>(s.size());
}

// 原子写 current.bin（同目录临时文件 + rename）。
// ★ 为什么必须原子：半截 EDID 会让驱动 EDID 状态损坏、源端 fallback 800x600。
// ★ 为什么 rename 而非直写：面板以 ttbox 身份跑，而开机由 root 重写后文件属主为 root
//   ⇒ 直写会 PermissionError；rename 只需**目录**写权限（目录是 root:ttbox 0775）。
bool write_atomic(const std::string& path, const std::string& data, std::string* err) {
    const std::string tmp = path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            if (err != nullptr) *err = "临时文件不可写: " + tmp;
            return false;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f.good()) {
            if (err != nullptr) *err = "写入临时文件失败: " + tmp;
            return false;
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        // 极端兜底：目录也不可写时退回直写（与脚本同款降级）
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            if (err != nullptr) *err = "EDID 写入失败: " + path;
            return false;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        return f.good();
    }
    return true;
}

// ---- HPD 节点（两个候选，取第一个存在且可写的）----
const char* kHpdCandidates[] = {"/sys/class/hdmirx/hdmirx/status",
                                "/sys/devices/platform/fdee0000.hdmirx-controller/hdmirx/hdmirx/status"};

// 取第一个存在且可写的 HPD status 节点路径；都不可写则返回空串。
std::string find_hpd_path() {
    for (const char* p : kHpdCandidates) {
        if (::access(p, W_OK) == 0) return std::string(p);
    }
    return std::string();
}

// ---- 信号处理：被 systemd 的 timeout 杀掉时，必须先把 HPD 拉回 on ----
// ★ 为什么必须做（原脚本文件头 ★★ 记录）：HPD 停在 off ⇒ 源端认为"显示器被拔了"，
//   客户看到的是**持续黑屏**。bash 的 EXIT trap 在收到 SIGTERM 时不执行，
//   所以脚本额外接了 TERM/INT；C++ 这里用 signal handler 等价实现。
//   退出码 143 = 128 + SIGTERM（与脚本 `exit 143` 同）。
char g_hpd_path[256] = {0};

extern "C" void on_term_signal(int sig) {
    if (g_hpd_path[0] != '\0') {
        const int fd = ::open(g_hpd_path, O_WRONLY);
        if (fd >= 0) {
            static const char kOn[] = "on\n";
            const ssize_t ignore = ::write(fd, kOn, 3);
            (void)ignore;
            ::close(fd);
        }
    }
    _exit(sig == SIGTERM ? 143 : 130);
}

// 记录 HPD 路径并安装 SIGTERM/SIGINT 处理，保证被强杀前把 HPD 拉回 on。
void install_hpd_guard(const std::string& hpd_path) {
    if (hpd_path.empty() || hpd_path.size() >= sizeof(g_hpd_path)) return;
    std::memset(g_hpd_path, 0, sizeof(g_hpd_path));
    std::memcpy(g_hpd_path, hpd_path.c_str(), hpd_path.size());
    std::signal(SIGTERM, on_term_signal);
    std::signal(SIGINT, on_term_signal);
}

// ---- 注入 + 回读全字节比对 ----
// ★ 必须带 format=raw：不带 format 的 `--get-edid=pad=0` 返回的是**源端(SOURCE)的 EDID**
//   （实测 769 B = 显示器真实身份），拿它比永远比不出真相。
bool apply_and_verify(const std::string& dev, const std::string& edid_path) {
    const std::string set_cmd = "v4l2-ctl -d " + sq(dev) + " --set-edid=pad=0,file=" +
                                sq(edid_path) + ",format=raw";
    if (!run_quiet(set_cmd)) return false;

    std::string raw;
    const std::string get_cmd =
        "v4l2-ctl -d " + sq(dev) + " --get-edid=pad=0,format=raw";
    if (!run_capture(get_cmd, &raw)) return false;

    std::string want;
    if (!read_bytes_file(edid_path, &want)) return false;
    // 长度与内容都必须一致；半截回读不算成功
    return !raw.empty() && raw.size() == want.size() && raw == want;
}

// 只回读：判断驱动当前持有的 EDID 是否已等于目标文件（不写入、不切 HPD）。
bool edid_matches_driver(const std::string& dev, const std::string& edid_path) {
    std::string raw;
    if (!run_capture("v4l2-ctl -d " + sq(dev) + " --get-edid=pad=0,format=raw", &raw)) return false;
    std::string want;
    if (!read_bytes_file(edid_path, &want)) return false;
    return !raw.empty() && raw.size() == want.size() && raw == want;
}

// 即时判锁（只给幂等短路用，绝不用于"等锁"）
bool lock_probe_quick(const std::string& dev) {
    std::string t;
    run_capture("v4l2-ctl -d " + sq(dev) + " --query-dv-timing", &t);
    return v4l2_timing_locked(t);
}

// 等输入锁：debugfs（仅 root 可读）优先；读不到时降级为只用 v4l2 判锁
// ★ V-EDID-4：ttbox（web 路径）读不到 debugfs ⇒ 若也走"未锁则等"会必然超时。
bool wait_for_lock(const std::string& dev, int timeout_sec) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string status = read_text_file("/sys/kernel/debug/hdmirx/status");
        if (!status.empty() && !debugfs_locked(status)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::string t;
        run_capture("v4l2-ctl -d " + sq(dev) + " --query-dv-timing", &t);
        if (v4l2_timing_locked(t)) return true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

// JsonValue → 紧凑 JSON 文本（统一出口）。
std::string json_out(const JsonValue& v) { return v.dump(); }

}  // namespace

// ---------------- 纯函数（单测）----------------

bool debugfs_locked(const std::string& status_text) {
    // 脚本判据：Clk-Ch:Lock + Ch0:Lock + Ch1:Lock + Ch2:Lock 四个都在同一行
    const size_t pos = status_text.find("Clk-Ch:");
    if (pos == std::string::npos) return false;
    size_t end = status_text.find('\n', pos);
    if (end == std::string::npos) end = status_text.size();
    const std::string line = status_text.substr(pos, end - pos);
    return line.find("Clk-Ch:Lock") != std::string::npos &&
           line.find("Ch0:Lock") != std::string::npos &&
           line.find("Ch1:Lock") != std::string::npos &&
           line.find("Ch2:Lock") != std::string::npos;
}

// v4l2 判锁：文本中既无 "failed" 也无 "No locks" 即视为已锁定。
bool v4l2_timing_locked(const std::string& timing_text) {
    return timing_text.find("failed") == std::string::npos &&
           timing_text.find("No locks") == std::string::npos;
}

// 构造成功 JSON（结构与旧脚本 stdout 一致，供 core/面板原样转发）。
std::string make_success_json(bool rehandshake, const std::string& file,
                              const std::string& mode_name) {
    JsonValue o = JsonValue::object();
    o.set("ok", JsonValue::boolean(true));
    o.set("hpd", JsonValue::string(rehandshake ? "rehandshake" : "unchanged"));
    o.set("method", JsonValue::string("v4l2_ctl"));
    o.set("file", JsonValue::string(file));
    o.set("mode", JsonValue::string(mode_name));
    return json_out(o);
}

// 构造失败 JSON（含 edid_applied / locked 两个现场标志）。
std::string make_failure_json(bool edid_applied, bool locked, const std::string& error) {
    JsonValue o = JsonValue::object();
    o.set("ok", JsonValue::boolean(false));
    o.set("error", JsonValue::string(error));
    o.set("edid_applied", JsonValue::boolean(edid_applied));
    o.set("locked", JsonValue::boolean(locked));
    return json_out(o);
}

// ---------------- 主流程 ----------------

// EDID 应用主入口：按 0~8 步执行（设备白名单→读配置→native_mode 保护→构建校验→原子落盘
// →dry_run/纯注入/重协商等锁），逐步见下方内联注释。
ApplyResult apply(const ApplyOptions& opt) {
    ApplyResult r;

    // 0) 设备白名单：EDID 注入**只能**用 /dev/video0（脚本同款硬拦）
    if (opt.video_dev != "/dev/video0") {
        r.error = "错误的 HDMI-RX 设备 " + opt.video_dev +
                  "：EDID 注入必须使用 /dev/video0；/dev/dri/card0 仅用于 loopout";
        r.json = make_failure_json(false, false, r.error);
        return r;
    }

    const std::string config_path =
        opt.config_path.empty() ? join_path(opt.prefix, "config/hardware_display.json")
                                : opt.config_path;
    const std::string out_path =
        opt.output_path.empty() ? join_path(opt.prefix, "runtime/edid/current.bin")
                                : opt.output_path;
    r.output_path = out_path;

    // 1) 读配置
    const std::string cfg_text = read_text_file(config_path);
    if (cfg_text.empty()) {
        r.error = "hardware_display.json 不存在或不可读";
        r.json = make_failure_json(false, false, r.error);
        return r;
    }
    const JsonParseResult pr = json_parse(cfg_text);
    if (!pr.ok || !pr.value.is_object()) {
        r.error = "hardware_display.json 解析失败";
        r.json = make_failure_json(false, false, r.error);
        return r;
    }
    JsonValue cfg = pr.value;

    // 2) native_mode 保护（空/auto/非法 ⇒ profile 首选）—— 唯一真源在 EdidConfig
    {
        const JsonValue* pv = cfg.find("profile");
        const JsonValue* nv = cfg.find("native_mode");
        const std::string profile = pv != nullptr ? pv->as_string("") : std::string();
        const std::string nm = nv != nullptr ? nv->as_string("") : std::string();
        cfg.set("native_mode", JsonValue::string(resolve_native_mode(profile, nm)));
    }

    // 3) 构建 + 校验
    const BuildResult built = build_from_config(cfg);
    if (!built.ok) {
        r.error = built.error;
        r.json = make_failure_json(false, false, r.error);
        return r;
    }
    std::vector<std::string> verrs;
    if (!verify_edid(built.edid, &verrs)) {
        r.error = "EDID 验证失败";
        r.json = make_failure_json(false, false, r.error);
        return r;
    }
    const std::string edid_bytes(reinterpret_cast<const char*>(built.edid.data()),
                                 built.edid.size());

    // 4) 落盘（原子替换）。目录可能不存在（首次运行）—— 脚本对应 `mkdir -p "$EDID_DIR"`；
    //    漏掉它会在"干净机器首次注入"时直接失败。
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(out_path).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    }
    std::string werr;
    if (!write_atomic(out_path, edid_bytes, &werr)) {
        r.error = werr;
        r.json = make_failure_json(false, false, r.error);
        return r;
    }
    {
        const JsonValue* nv = cfg.find("name");
        r.mode_name = nv != nullptr ? nv->as_string("TTBOX") : std::string("TTBOX");
    }
    r.edid_applied = true;

    if (opt.dry_run) {
        r.ok = true;
        r.exit_code = 0;
        r.json = make_success_json(false, out_path, r.mode_name);
        return r;
    }

    // 5) 纯注入模式（不切 HPD、不判锁）
    if (!opt.rehandshake) {
        if (!apply_and_verify(opt.video_dev, out_path)) {
            r.edid_applied = false;
            r.error = "EDID 注入或回读校验失败，未执行重试/HPD切换";
            r.json = make_failure_json(false, false, r.error);
            return r;
        }
        r.ok = true;
        r.exit_code = 0;
        r.json = make_success_json(false, out_path, r.mode_name);
        return r;
    }

    // 6) 重协商：先找可写 HPD 节点
    const std::string hpd = find_hpd_path();
    if (hpd.empty()) {
        r.edid_applied = false;
        r.error = "已请求重协商，但未找到可写 HDMI-RX HPD 节点";
        r.json = make_failure_json(false, false, r.error);
        return r;
    }
    install_hpd_guard(hpd);

    // 7) 幂等短路：驱动已持有目标 EDID **且**输入已锁 ⇒ 一次 HPD 都不切（零黑屏）
    bool applied = false;
    bool locked = false;
    if (edid_matches_driver(opt.video_dev, out_path) && lock_probe_quick(opt.video_dev)) {
        applied = true;
        locked = true;
    }

    // 8) 重协商循环：一轮 = 恰好一次 HPD 周期，且 off 之后无论如何都要 on
    int attempts = opt.attempts <= 0 ? 1 : opt.attempts;
    for (int a = 1; a <= attempts && !locked; ++a) {
        write_sysfs(hpd, "off");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (apply_and_verify(opt.video_dev, out_path)) applied = true;
        write_sysfs(hpd, "on");
        if (applied) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<int>(opt.hpd_settle_sec * 1000.0)));
            if (wait_for_lock(opt.video_dev, opt.lock_timeout_sec)) locked = true;
        }
        if (!locked) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!locked) {
        write_sysfs(hpd, "on");  // 失败路径也必须让 HPD 回到 on
        r.edid_applied = applied;
        r.locked = false;
        r.error = applied ? "EDID 已写入且回读一致，但 HDMI-RX 多轮重新枚举后仍未锁定输入"
                          : "EDID 注入或回读校验失败，重新枚举未完成";
        r.json = make_failure_json(applied, false, r.error);
        return r;
    }

    r.ok = true;
    r.edid_applied = applied;
    r.locked = true;
    r.exit_code = 0;
    r.json = make_success_json(true, out_path, r.mode_name);
    return r;
}

}  // namespace ttbox::core::edid
