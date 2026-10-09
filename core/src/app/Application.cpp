// Application.cpp — 应用生命周期实现
#include "app/Application.hpp"
#include "app/ApplicationInternal.hpp"   // 四个 TU 共享的内部辅助（inline，见该文件头说明）
#include "app/RuntimeIntent.hpp"   // R5/R6：启动意图裁决（纯函数，host 可单测）

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "common/Logger.hpp"
#include "common/CpuAffinity.hpp"
#include "common/RtSched.hpp"
#include "common/Json.hpp"           // R5：启停意愿文件读写（json_parse_file / dump）
#include "common/Paths.hpp"        // A-PATH-5：运行期路径字面量单点真源
#include "common/ConfigDefaults.hpp"  // C-CFG-4：出厂默认值镜像（== deploy/config/00-factory.json）
#include "model/ModelManagement.hpp"
#include "model/ModelAdapter.hpp"
#ifdef TTBOX_CORE_HAS_RKNN
#include "rknn/RKNNEngine.hpp"
#endif
#include "output/AiboxHidOutput.hpp"
#include "output/FifoHidOutput.hpp"
#include "output/OutputBackend.hpp"
#include "ttbox/core/version.hpp"
#include "auth/LicenseGate.hpp"         // T1.10：唯一授权执法快照（会话边界 / IPC 投影）
#include "auth/OfflineCardClient.hpp"   // M2.01/M2.02：离线签名卡验证（fail-closed）

#include <cstdio>

namespace ttbox::core {

namespace {

// ★ shutdown_flag() / g_shutdown_requested 已搬到 Application.hpp（inline 变量）。
//   原因：Application.cpp 拆成 4 个 TU 后，request_shutdown()（写）与 run()（读）
//   分处不同 TU —— 留在匿名命名空间会让两个 TU 各拿一份独立副本 ⇒ 关机信号静默丢失。

#ifndef TTBOX_PROJECT_ROOT
#error "TTBOX_PROJECT_ROOT must be injected by CMake (-DTTBOX_PROJECT_ROOT); refuse silent fallback to '.'."
#endif
const char* kDefaultConfigPath = TTBOX_PROJECT_ROOT "/config/default.json";
// 系统 license 文件路径**唯一真源** = common/Paths.hpp::kSystemLicenseFile（A-PATH-5）；
// 本文件不再另写 "/etc/ttbox/license.key" 字面量。
// ★ M2.03：受限预览帧率封顶常量 kRestrictedPreviewFps 定义在 runtime/CoreRuntime.hpp
//   （Application 与 CoreRuntime 共用同一取值，避免两处各写一个 5）。

// 取非空环境变量（空串视为未设置，避免把 "" 当成合法覆盖值）
std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

// ---- 风扇 PWM 节点解析（P8：不写死 hwmon 编号）-------------------------------
// 历史实现在启动段里写死 "/sys/class/hwmon/hwmon8/pwm1"。hwmon 编号是内核按枚举
// 顺序分配的，不是板子的身份（换内核 / 加传感器即移位）。写死一旦落空，风扇就
// 静默不转 ⇒ NPU 热节流降频，而且没有任何报错。
// 口径与 Web 面板完全一致（plugins/web/bin/ttbox-web.py::_fan_control_payload）：
//   ① 按 /sys/class/hwmon/hwmon*/name 匹配 pwm-fan / pwmfan / fan / soc-thermal；
//   ② 无匹配则取排序后第一个 hwmon*/pwm1（尽力而为，且与面板显示同一节点）；
//   ③ 一个都没有 ⇒ 返回空串，调用方只告警、不假装成功。
// 读文件首行并去掉尾部换行/空白（用于读取 hwmon name 这类单行节点）。
std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

// base_dir 是唯一的输入（生产调用传 "/sys/class/hwmon"）——留出这个参数是为了能
// 对真实实现做**离线可复现**测试（假 hwmon 树 → 断言解析结果），不是给业务用的开关。
std::string resolve_fan_pwm_path(const std::string& base_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base(base_dir);
    if (!fs::is_directory(base, ec)) return std::string();
    std::string matched;   // name 命中的首选节点
    std::string fallback;  // 排序后第一个 hwmon*/pwm1
    for (fs::directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path dir = it->path();
        const fs::path pwm = dir / "pwm1";
        std::error_code ec2;
        if (!fs::exists(pwm, ec2)) continue;
        const std::string p = pwm.string();
        if (fallback.empty() || p < fallback) fallback = p;
        if (matched.empty()) {
            const std::string nm = read_first_line((dir / "name").string());
            if (nm == "pwm-fan" || nm == "pwmfan" || nm == "fan" || nm == "soc-thermal") {
                matched = p;
            }
        }
    }
    if (!matched.empty()) return matched;
    return fallback;
}


// ★ now_ms() / strip() / parse_color_order() / parse_worker_cores() /
//   incoming_dir_of() / kPostUpdateSmokeMaxMs 已搬到 ApplicationInternal.hpp
//   （inline 共享）—— 这 6 个符号被本 TU 与另外三个 TU 同时使用，
//   留在本文件的匿名命名空间里对其他 TU 不可见（链接期 undefined）。
//   下方本文件用它们时走 `using app_internal::xxx;`。

namespace {

// 读 ota_status.json 并判定"更新器终态"。判定逻辑在 RuntimeIntent.hpp::ota_terminal_state
// （纯函数、host 可单测）；这里只做文件 I/O 薄封装。
std::string read_ota_terminal_state(const std::string& path,
                                    const std::string& expected_version) {
    if (path.empty()) return std::string();
    const JsonParseResult parsed = json_parse_file(path);
    if (!parsed.ok) return std::string();
    return ota_terminal_state(parsed.value, expected_version);
}

}  // namespace

using app_internal::now_ms;
using app_internal::parse_color_order;
using app_internal::parse_worker_cores;
using app_internal::strip;

// 把 --log-level 的字符串解析为 LogLevel 枚举（未知值回退 kInfo）。
LogLevel parse_log_level(const std::string& s) {
    if (s == "debug") return LogLevel::kDebug;
    if (s == "warn") return LogLevel::kWarn;
    if (s == "error") return LogLevel::kError;
    if (s == "fatal") return LogLevel::kFatal;  // §5.1 新增最高级
    if (s == "off") return LogLevel::kOff;
    return LogLevel::kInfo;
}

}  // namespace


// 应用总入口：解析 CLI → 初始化 Logger/风扇/配置/授权/模型库 → 起 IPC → 构建 CoreRuntime。
// initialize 失败（参数/配置/授权）返回非 0；CoreRuntime 未就绪则降级为“待配置”并返回 0。
int Application::initialize(int argc, char** argv) {
    if (initialized_) {
        TTBOX_LOG_WARN("Application 已初始化，忽略重复调用");
        return 0;
    }
    std::string cli_license;
    std::string cli_secret;
    bool verify_only = false;
    bool ipc_from_cli = false;  // --ipc 显式给出时，环境变量不再覆盖

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto consume_value = [&](const char* name) -> bool {
            if (i + 1 >= argc) {
                TTBOX_LOG_ERROR(std::string("缺少参数值: ") + name);
                return false;
            }
            ++i;
            return true;
        };
        if (arg == "--config") {
            if (!consume_value("--config")) return 1;
            config_path_ = argv[i];
        } else if (arg == "--ipc") {
            if (!consume_value("--ipc")) return 1;
            ipc_path_ = argv[i];
            ipc_from_cli = true;
        } else if (arg == "--log-level") {
            if (!consume_value("--log-level")) return 1;
            Logger::instance().set_level(parse_log_level(argv[i]));
        } else if (arg == "--license") {
            if (!consume_value("--license")) return 1;
            cli_license = argv[i];
        } else if (arg == "--license-server-secret") {
            if (!consume_value("--license-server-secret")) return 1;
            cli_secret = argv[i];
        } else if (arg == "--debug-license-pro-endpoint") {
            if (!consume_value("--debug-license-pro-endpoint")) return 1;
            license_override_pro_endpoint_ = argv[i];
        } else if (arg == "--debug-license-normal-endpoint") {
            if (!consume_value("--debug-license-normal-endpoint")) return 1;
            license_override_normal_endpoint_ = argv[i];
        } else if (arg == "--verify-only") {
            verify_only = true;
        } else if (arg == "--help" || arg == "-h") {
            TTBOX_LOG_INFO(
                "用法: ttbox_core [--config <path>] [--ipc <path>]\n"
                "                [--log-level debug|info|warn|error|fatal|off]\n"
                "                [--license <card>] [--license-server-secret <secret>]\n"
                "                [--verify-only]\n"
                "                [--debug-license-pro-endpoint <host>]\n"
                "                [--debug-license-normal-endpoint <host>]");
            return 1;
        } else {
            TTBOX_LOG_WARN(std::string("忽略未知参数: ") + arg);
        }
    }

    Logger::instance().add_sink(std::make_shared<ConsoleSink>());
    // §5.2 文件 sink（批次 1.4）：日志落 /var/log/ttbox —— 三条链路各自的落点见 Logger.hpp
    // 顶部说明。目录不可用（开发机 / 未做 FHS 化的树）时 sink 自降级为不可用：只影响落盘，
    // 不影响启动，也不影响 ConsoleSink 转发 journald。
    {
        const std::string env_log_dir = env_or_empty("TTBOX_LOG_DIR");
        const std::string log_dir =
            env_log_dir.empty() ? std::string(paths::kLogDirDefault) : env_log_dir;
        std::shared_ptr<FileSink> file_sink = std::make_shared<FileSink>(log_dir);
        Logger::instance().add_sink(file_sink);

        TTBOX_LOG_INFO("=== " + std::string(kAppName) + " v" +
                       std::string(kCoreVersion) + " 启动 ===");
        if (file_sink->usable()) {
            TTBOX_LOG_INFO("日志落盘目录: " + log_dir);
        } else {
            TTBOX_LOG_WARN("日志目录不可写，仅输出到控制台: " + log_dir);
        }
    }

    // ---- 风扇满转（fan_control min_pwm=100）：防热节流拖慢 NPU ----
    // 节点由 resolve_fan_pwm_path() 动态解析（P8），不再写死 hwmon 编号。
    {
        const std::string fan_pwm = resolve_fan_pwm_path("/sys/class/hwmon");
        if (!fan_pwm.empty()) {
            std::ofstream pwm(fan_pwm);
            if (pwm) {
                pwm << 255;
                TTBOX_LOG_INFO("风扇已设满转（防热节流）：" + fan_pwm);
            } else {
                TTBOX_LOG_WARN("风扇控制不可写（权限或只读挂载）：" + fan_pwm);
            }
        } else {
            TTBOX_LOG_WARN("风扇控制不可用（/sys/class/hwmon 下无 pwm1 节点）");
        }
    }


    // 配置路径优先级链（T01）：--config 参数 > TTBOX_CONFIG 环境变量 > 编译期默认。
    // --config 指向目录时自动走分层加载（config.d/*.json 按文件名升序深合并，
    // 00-factory.json 只读基线 ← 10-device.json 客户覆盖，写回目标为最高层文件）。
    if (config_path_.empty()) {
        const std::string env_cfg = env_or_empty("TTBOX_CONFIG");
        config_path_ = env_cfg.empty() ? kDefaultConfigPath : env_cfg;
    }
    // IPC socket 唯一真源（T01）：--ipc 参数 > TTBOX_IPC_SOCKET 环境变量 > /run/ttbox/core.sock。
    // Web / 预览 / Python 工具侧默认值与此保持一致（TTBOX_IPC_SOCKET 为唯一真源）。
    if (!ipc_from_cli) {
        const std::string env_ipc = env_or_empty("TTBOX_IPC_SOCKET");
        if (!env_ipc.empty()) ipc_path_ = env_ipc;
    }
    if (ipc_path_.empty()) ipc_path_ = paths::kIpcSocketDefault;
    std::string cfg_error;
    if (!config_.load(config_path_, &cfg_error)) {
        TTBOX_LOG_ERROR(cfg_error);
        TTBOX_LOG_ERROR("配置加载失败，拒绝启动");
        return 1;
    }
    TTBOX_LOG_INFO("配置已加载: " + config_path_);

    // ---- 启动意图裁决（R5 用户意愿 / R6 刚更新过）（R5/R6）----
    // OTA 更新 = systemctl restart ttbox-core；两条诉求必须一起满足：
    //   · 业主 2026-09-20：**更新后应该是停止状态**（不点启动不跑）——R6；
    //   · 普通重启/断电恢复：尊重用户上次显式 start/stop——R5。
    // 目录基址：TTBOX_STATE 环境变量（与 scripts/ttbox.sh 同源）> /opt/ttbox/state。
    {
        std::string state_dir = env_or_empty("TTBOX_STATE");
        if (state_dir.empty()) state_dir = paths::kStateDirDefault;
        runtime_intent_path_ = state_dir + "/" + paths::kRuntimeIntentFileName;
        ota_status_path_ = state_dir + "/" + paths::kOtaStatusFileName;
        apply_startup_runtime_intent(state_dir);
    }

    // ---- 实时调度准备：自己放开 RLIMIT_RTPRIO/MEMLOCK 并锁页 ----
    // 必须在任何采集/推理线程起来之前做。不依赖 systemd 单元的 LimitRTPRIO：
    // 板端实测 ttbox-core 单元 LimitRTPRIO=0，且 OTA 不保证覆盖单元文件。
    // 失败只告警，线程会降级为普通调度。
    {
        std::string detail;
        RtSched::prepare_process(&detail);
    }

    // ---- CPU 调频策略：使用系统默认动态调频，不强制拉满频率 ----
    // 绑核已经保证采集/推理/瞄准/预览在大核上运行；频率交给内核
    // schedutil 按负载自适应，避免长期满频导致温度过高。
    {
        for (const char* pol : {"policy0", "policy4", "policy6"}) {
            std::ofstream g(std::string("/sys/devices/system/cpu/cpufreq/") + pol + "/scaling_governor");
            if (g) {
                g << "schedutil";
                if (!g.good()) TTBOX_LOG_WARN(std::string("governor 切换失败: ") + pol);
            }
        }
        const int pct = static_cast<int>(config_.get_int("cpu_min_freq_percent", cfg::kCpuMinFreqPercentDefault));
        auto fr = CpuAffinity::lock_min_freq_percent(pct);
        if (fr.freq_ok) {
            TTBOX_LOG_INFO("CPU 调频策略完成（schedutil+min" + std::to_string(pct) + "%）: " + fr.detail);
        } else {
            TTBOX_LOG_WARN("CPU 调频策略部分失败: " + fr.detail);
        }
    }

    // ---- 3. 授权层初始化（M2：离线签名卡，fail-closed）----
    // M2.01/M2.02（路线 §1.2）：NullLicenseClient（M1 离线 fail-open）退役，换
    // OfflineCardClient——本地 Ed25519 验签（自包含 TweetNaCl 路径，无 OpenSSL ⇒
    // AUTH=OFF 出货向量不变、A32 ELF 闭集不破）。语义翻转为 fail-closed：
    //   · 无卡 ⇒ kInvalidCard("card not set") ⇒ ai_allowed()=false ⇒ AI 路径不启动
    //     （透传/Web 管理照常，激活入口可用——A21 红线不破）。
    //   · 坏卡/他板卡/过期卡 ⇒ 权威拒绝，不进 kFallback（离线验签 req_ok 恒 true，
    //     网络类失败结构性不存在 ⇒ fail-open 分支不可达，正是商业化门控语义）。
    //   · TtboxLicenseClient（在线，/api/client/*）源码保留，仍由 AUTH=ON 门控，
    //     在线接入推迟到 T2.03。
    license_client_ = std::make_unique<auth::OfflineCardClient>();
    TTBOX_LOG_INFO("授权模式：M2 离线签名卡（OfflineCardClient / fail-closed）");
    license_server_secret_ = cli_secret.empty()
                                 ? config_.get_string("license_server_secret", "")
                                 : cli_secret;
    license_daemon_ = std::make_unique<auth::LicenseDaemon>(*license_client_);
    const std::string card = resolve_license_card(cli_license);
    if (!card.empty()) {
        license_daemon_->set_card(card);
        TTBOX_LOG_INFO("授权卡号已加载 (prefix: " +
                       card.substr(0, std::min<size_t>(8, card.size())) + "...)");
    }
    // 开发模式：没有卡号时允许 --license-server-secret 为空，后续 --verify-only 可快速失败
    verify_only_ = verify_only;
    if (!license_daemon_->start()) {
        TTBOX_LOG_ERROR("授权线程启动失败");
        return 1;
    }
    // --verify-only 模式：立即同步触发一次，打印结果后退出，不进入推理
    if (verify_only_) {
        std::string err;
        bool ok = license_daemon_->verify_now_blocking(&err);
        auto st = license_daemon_->status_snapshot();
        TTBOX_LOG_INFO(std::string("verify-only: ok=") + (ok ? "true" : "false") +
                       " state=" + std::to_string(static_cast<int>(st.state)) +
                       " is_pro=" + (st.is_pro ? "true" : "false") +
                       " error=" + (st.last_error.empty() ? err : st.last_error));
        // verify-only 模式：无论结果如何，打印后直接退出
        license_daemon_->stop();
        return (ok && (st.state == auth::LicenseState::kValid ||
                       st.state == auth::LicenseState::kFallback))
                   ? 0
                   : 2;
    }

    // ---- 4. 加载 RuntimeProfile：让配置文件真正进入 Worker/AimThread ----
    if (const JsonValue* profile_json = config_.root().find("runtime_profile")) {
        RuntimeProfile profile = RuntimeProfile::from_json(*profile_json);
        std::string profile_error;
        if (!profile.validate(&profile_error)) {
            TTBOX_LOG_ERROR("RuntimeProfile 校验失败: " + profile_error);
            return 1;
        }
        // from_json() 可能修复历史坏值（例如 capture=1×1 → 0×0）。
        // 若 canonical 与磁盘原值不同，通过唯一提交入口一次完成磁盘和内存更新；
        // 未发生自愈时只发布已校验的内存快照。
        if (profile.to_json().dump() != profile_json->dump()) {
            std::string repair_error;
            if (persist_runtime_profile(profile, &repair_error)) {
                TTBOX_LOG_WARN("RuntimeProfile 历史坏配置已自愈并写回磁盘");
            } else {
                TTBOX_LOG_WARN("RuntimeProfile 自愈写回失败，使用内存合法值启动: " + repair_error);
                runtime_config_.update(profile);
            }
        } else {
            runtime_config_.update(profile);
        }
        TTBOX_LOG_INFO("RuntimeProfile 已加载");
    }

    // ---- 模型管理（v0.3）：先初始化 Registry，再构建 Runtime 参数 ----
    {
        // 模型库根优先级链（T01）：TTBOX_MODELS_ROOT 环境变量 > 配置 model_registry_root
        // > ModelRegistry 构造内的编译期宏兜底（TTBOX_PROJECT_ROOT/models）。
        std::string reg_root = config_.get_string("model_registry_root", "");
        const std::string env_models = env_or_empty("TTBOX_MODELS_ROOT");
        if (!env_models.empty()) reg_root = env_models;
        model_management_ = std::make_unique<ModelManagement>(
            ModelRegistryOptions{reg_root, true});
        std::string mm_error;
        if (!model_management_->init(&mm_error)) {
            TTBOX_LOG_ERROR("ModelRegistry 初始化失败: " + mm_error);
            model_management_.reset();
            return 1;
        }
#ifdef TTBOX_CORE_HAS_RKNN
        model_management_->set_validator([](const std::string& rknn_path, JsonValue* meta_out,
                                            std::string* error) -> bool {
            std::error_code fec;
            if (!std::filesystem::exists(rknn_path, fec)) {
                if (error) *error = "模型文件不存在: " + rknn_path;
                return false;
            }
            RKNNEngine probe;
            RKNNEngine::Params pp;
            pp.model_path = rknn_path;
            pp.core_mask = 0;
            std::string perr;
            if (!probe.init(pp, &perr)) {
                if (error) *error = "RKNN 探测加载失败: " + perr;
                return false;
            }
            ModelAdapter adapter;
            ModelAdapterConfig adapter_cfg;
            std::string adapter_error;
            if (!adapter.analyze(probe.info(), adapter_cfg, &adapter_error)) {
                if (error) *error = "ModelAdapter 分析失败: " + adapter_error;
                return false;
            }
            const ModelMetadata& metadata = adapter.metadata();
            if (metadata.class_count == 0) {
                if (error) *error = "METADATA_INVALID: RKNN 输出无法确定 class_count";
                return false;
            }
            // ---- 真实冒烟推理（入库硬门槛）----
            // 用合成输入（中性灰 128）真实跑一次 NPU 推理 + Decode：
            //  - 推理失败 → SMOKE_INFERENCE_FAILED（NPU 不兼容/内存不足等在此暴露）
            //  - Decode 崩溃或输出 NaN/Inf → DECODE_INVALID（解码器与输出结构不匹配在此暴露）
            // 冒烟通过不要求检出目标（合成图没有真目标），只要求"跑得动、解得动、数值合法"。
            {
                const uint32_t in_bytes = probe.info().input_size > 0
                                              ? probe.info().input_size
                                              : static_cast<uint32_t>(metadata.input_width *
                                                                      metadata.input_height * 3);
                std::vector<uint8_t> smoke_input(in_bytes, 128);  // 中性灰，避免极端激活
                std::string smoke_error;
                // 与生产路径完全一致：set_input（原生字节）→ run → get_raw_outputs（原生输出）
                if (!probe.set_input(smoke_input.data(), smoke_input.size(), &smoke_error) ||
                    !probe.run(&smoke_error)) {
                    if (error) *error = "SMOKE_INFERENCE_FAILED: " + smoke_error;
                    probe.destroy();
                    return false;
                }
                const uint32_t n_out = probe.info().n_outputs;
                std::vector<std::vector<uint8_t>> raw_bufs;
                std::vector<void*> out_ptrs(n_out, nullptr);
                std::vector<size_t> out_sizes(n_out, 0);
                for (uint32_t i = 0; i < n_out; ++i) {
                    const uint32_t sz = probe.info().output_sizes[i];
                    raw_bufs.emplace_back(sz, 0);
                    out_ptrs[i] = raw_bufs.back().data();
                    out_sizes[i] = sz;
                }
                if (!probe.get_raw_outputs(out_ptrs.data(), out_sizes.data(), &smoke_error)) {
                    if (error) *error = "SMOKE_INFERENCE_FAILED: " + smoke_error;
                    probe.destroy();
                    return false;
                }
                // Decode 合法性：用与生产一致的 Decoder 跑冒烟输出
                auto decoder = adapter.create_decoder(&adapter_error);
                if (!decoder) {
                    if (error) *error = "UNSUPPORTED_OUTPUT: " + adapter_error;
                    probe.destroy();
                    return false;
                }
                std::vector<DetectionBox> smoke_dets;
                if (!decoder->process(probe.info(), out_ptrs.data(), &smoke_dets,
                                      &adapter_error)) {
                    if (error) *error = "DECODE_INVALID: " + adapter_error;
                    probe.destroy();
                    return false;
                }
                // 数值合法性：不允许 NaN/Inf 混入解码结果
                for (const auto& d : smoke_dets) {
                    const float v[4] = {d.x1, d.y1, d.x2, d.y2};
                    for (float x : v) {
                        if (!std::isfinite(x)) {
                            if (error) *error = "DECODE_INVALID: 检测框含 NaN/Inf";
                            probe.destroy();
                            return false;
                        }
                    }
                    if (!std::isfinite(d.score) || d.score < 0.0f || d.score > 1.0f) {
                        if (error) *error = "DECODE_INVALID: 置信度越界";
                        probe.destroy();
                        return false;
                    }
                }
                if (meta_out) {
                    meta_out->set("smoke_inference", JsonValue::boolean(true));
                }
            }
            if (meta_out) {
                JsonValue obj = JsonValue::object();
                obj.set("input_width", JsonValue::number(static_cast<double>(metadata.input_width)));
                obj.set("input_height", JsonValue::number(static_cast<double>(metadata.input_height)));
                obj.set("input_channels", JsonValue::number(static_cast<double>(metadata.input_channels)));
                obj.set("input_dtype", JsonValue::number(static_cast<double>(metadata.input_dtype)));
                obj.set("input_layout", JsonValue::number(static_cast<double>(metadata.input_layout)));
                obj.set("quantization", JsonValue::number(static_cast<double>(static_cast<int>(metadata.quantization_type))));
                obj.set("output_count", JsonValue::number(static_cast<double>(metadata.output_count)));
                obj.set("class_count", JsonValue::number(static_cast<double>(metadata.class_count)));
                obj.set("decode_type", JsonValue::string(ModelAdapter::decode_type_name(metadata.decode_type)));
                obj.set("objectness", JsonValue::boolean(metadata.objectness));
                obj.set("dfl", JsonValue::boolean(metadata.dfl));
                JsonValue strides = JsonValue::array();
                for (uint32_t stride : metadata.strides) strides.push_back(JsonValue::number(static_cast<double>(stride)));
                obj.set("strides", std::move(strides));
                JsonValue output_shapes = JsonValue::array();
                for (const auto& shape : metadata.output_shapes) {
                    JsonValue dims = JsonValue::array();
                    for (uint32_t dim : shape) dims.push_back(JsonValue::number(static_cast<double>(dim)));
                    output_shapes.push_back(std::move(dims));
                }
                obj.set("output_shapes", std::move(output_shapes));
                *meta_out = std::move(obj);
            }
            probe.destroy();
            return true;
        });
#else
        model_management_->set_validator(ModelManagement::file_level_validator);
#endif
        if (!model_management_->registry().refresh(&mm_error)) {
            TTBOX_LOG_ERROR("ModelRegistry 刷新失败: " + mm_error);
            model_management_.reset();
            return 1;
        }
        TTBOX_LOG_INFO("ModelRegistry 已就绪: " + model_management_->registry().root_dir());
    }

    // ---- 5. 启动 IPC 服务（必须先于 CoreRuntime：T02 启动死锁修复）----
    // 原死锁：build_runtime_params 失败（未选模型）→ return 1 退出，而 IPC 尚未启动，
    // Web 无法连接 → 无法选模型 → systemd Restart=always 无限重启（现场实测 82 次/31 分钟）。
    // 修复后：IPC 先起，CoreRuntime 失败进入"待配置"降级态（进程存活、IPC 可用），
    // 由 MODEL_ACTIVATE / 主循环自动重试拉起（见 try_resume_from_degraded）。
    ipc_.set_status_provider([this] { return status_provider(); });
    ipc_.set_preview_provider([this](std::vector<uint8_t>* out, uint64_t* seq) {
        std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
        const bool ok = core_runtime_ && core_runtime_->preview() && core_runtime_->preview()->running()
                   ? core_runtime_->preview()->snapshot(out)
                   : false;
        if (ok && seq) {
            *seq = core_runtime_->preview()->metrics().frames.load();
        }
        return ok;
    });
    ipc_.set_config_provider([this] { return config_provider(); });
    ipc_.set_config_update_handler(
        [this](const JsonValue& profile_json, std::string* error, bool* persisted) {
            return handle_config_update(profile_json, error, persisted);
        });
    ipc_.set_runtime_control_handler(
        [this](const std::string& action, std::string* error) {
            return handle_runtime_control(action, error);
        });
    // V1.0.50：硬件写入（EDID 注入 / USB 透传切换）——core 是 root，web 无 sudo。
    ipc_.set_hardware_action_handler(
        [this](const std::string& action, const JsonValue& params, JsonValue* data,
               std::string* error) {
            return handle_hardware_action(action, params, data, error);
        });
    // M2.02：离线卡激活（Web → IPC → daemon 原子序 → Gate publish）。
    ipc_.set_license_activate_handler(
        [this](const std::string& card, JsonValue* data, std::string* error) {
            return handle_license_activate(card, data, error);
        });
    // M2.07：云端卡密激活（ACTIVATE_CLOUD）
    ipc_.set_license_cloud_activate_handler(
        [this](const JsonValue& params, JsonValue* data, std::string* error) {
            return handle_license_activate_cloud(params, data, error);
        });

    if (model_management_) {
        ipc_.set_model_list_handler([this] { return handle_model_list(); });
        ipc_.set_model_import_handler(
            [this](const std::string& src, const std::string& id,
                   const std::string& label, const std::string& source_format,
                   const std::string& sha256, std::string* error) {
                return handle_model_import(src, id, label, source_format, sha256, error);
            });
        ipc_.set_model_validate_handler(
            [this](const std::string& id, std::string* error) {
                return handle_model_validate(id, error);
            });
        ipc_.set_model_install_handler(
            [this](const std::string& id, std::string* error) {
                return handle_model_install(id, error);
            });
        ipc_.set_model_activate_handler(
            [this](const std::string& id, std::string* error) {
                return handle_model_activate(id, error);
            });
        ipc_.set_model_remove_handler(
            [this](const std::string& id, std::string* error) {
                return handle_model_remove(id, error);
            });
        ipc_.set_model_concurrency_handler(
            [this](const std::string& id, int count, std::string* error) {
                return handle_model_set_concurrency(id, count, error);
            });
    }
    std::string ipc_error;
    if (!ipc_.start(ipc_path_, &ipc_error)) {
        TTBOX_LOG_ERROR("IPC 启动失败: " + ipc_error);
        return 1;
    }
    start_time_ms_ = now_ms();
    initialized_ = true;

    // ---- 6. 构建 CoreRuntime 参数 & 初始化（失败不退出：进入"待配置"降级态）----
    // 设计铁律（docs 设计方案 4.5）："等待用户配置"绝不能是致命错误。
    // core_runtime_ 保持"已分配但未 initialize"：switch_active_model_runtime 的
    // "停机重初始化"路径（stop → initialize → start）可在 MODEL_ACTIVATE 时直接复用。
    core_runtime_ = std::make_unique<CoreRuntime>();
    CoreRuntime::Params rt_params{};
    std::string rt_error;
    // ★ M2.03：构建期用「默认全 true」gates（此刻尚无卡态投影；保持历史"最大能力"构建语义）。
    //   真实卡态在 run() 会话边界由 set_feature_gates() 收窄。
    if (!build_runtime_params(rt_params, CoreRuntime::FeatureGates{}, &rt_error)) {
        runtime_waiting_config_ = true;
        degraded_reason_ = rt_error;
        TTBOX_LOG_WARN("CoreRuntime 待配置（WAITING_FOR_MODEL，进程保持存活等待模型激活）: "
                       + rt_error);
        return 0;
    }
    if (!core_runtime_->initialize(rt_params, &rt_error)) {
        runtime_waiting_config_ = true;
        degraded_reason_ = rt_error;
        TTBOX_LOG_WARN("CoreRuntime 初始化失败（进程保持存活，等待配置/硬件恢复）: "
                       + rt_error);
        return 0;
    }
    TTBOX_LOG_INFO("CoreRuntime 初始化完成 (workers=" +
                   std::to_string(rt_params.workers.worker_cores.size()) + ")");

    // ---- 推理预加载（可选）：把「模型加载 + 预热」提前到开机 ----
    // 默认关闭：预加载会让 NPU 和内存常驻占用，且采集未起时 worker 空转。
    // 打开后「点开始」只需拉起轮询线程（见 CoreRuntime::preload_workers）。
    // 失败只告警：不影响后续正常 start()。
    if (config_.get_bool("inference_preload", false)) {
        std::string plerr;
        if (!core_runtime_->preload_workers(&plerr)) {
            TTBOX_LOG_WARN("推理预加载失败（不影响后续启动）: " + plerr);
        }
    }
    return 0;
}
// 事件循环：先做一次会话边界授权投影与自动启动，然后 50ms 一拍阻塞轮询，
// 直到 shutdown_flag() 置位；退出沿用“先停 CoreRuntime、再停 IPC”的既有顺序。
void Application::run() {
    if (!initialized_) {
        TTBOX_LOG_ERROR("Application 未初始化，拒绝 run()");
        return;
    }
    running_.store(true);

    // 授权门控：先等待一次立即验卡；允许的状态：kValid 或 Fallback
    // 超过 60s 仍未通过 → 打印但仍然继续（开发期离线）
    {
        std::string err;
        (void)license_daemon_->verify_now_blocking(&err);
        int waited = 0;
        while (!license_daemon_->allow_run() && waited < 60) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ++waited;
        }
        auto st = license_daemon_->status_snapshot();
        if (!license_daemon_->allow_run()) {
            TTBOX_LOG_WARN("授权未通过，但继续启动（离线开发模式/未填卡）；推理结果仍会按配置过滤");
        } else {
            TTBOX_LOG_INFO(std::string("授权通过 state=") +
                           std::to_string(static_cast<int>(st.state)) +
                           " is_pro=" + (st.is_pro ? "true" : "false"));
        }
    }

    // T1.10①：进入会话边界前的唯一一次投影——把 daemon 当前态发布到 LicenseGate，
    // 使紧随其后的 ai_allowed() 基于**本次**状态判定（否则 gate 从未 publish = kUnknown 恒拒）。
    auth::LicenseGate::instance().publish(auth::to_snapshot(
        license_daemon_->status_snapshot(), license_daemon_->store_load_result(),
        static_cast<int64_t>(now_ms())));
    // ★ M2.03：会话级 gate —— pipeline_allowed() == feature_enabled("capture")（无采集=无帧源）。
    //   7 处会话边界统一用它；唯一例外 = switch_active_model_runtime()（模型切换属推理子系统）。
    if (!auth::LicenseGate::instance().pipeline_allowed()) {
        // 红线：授权未通过只关 AI 路径；透传 / Web 管理**永不停**（不 return）。
        TTBOX_LOG_WARN(std::string("授权未通过：AI 功能路径不启动（缺 feature: ") +
                       gate_missing_summary() + "；透传/Web 管理不受影响）");
    }
    // ★ M2.03：会话边界把 gates 下传给 runtime（start() 据此逐模块启停 + 预览降级）。
    //   与 initialize() 的构建同源（同一张卡 ⇒ 同一组 gates），此处再读一次以覆盖启动前的授权变化。
    if (core_runtime_) {
        core_runtime_->set_feature_gates(current_feature_gates(), brand_upper(),
                                         static_cast<int>(config_.get_int("preview_fps", cfg::kPreviewFpsDefault)));
    }

    // ---- 自动启动 AI 流水线 ----
    // 语义：want_runtime_running_=true 时，尽力保持 runtime 运行。
    //   1) 首次启动：先立即尝试一次；失败则进入后台重试（HDMI 未锁定 / V4L2 CMA 碎片
    //      是板端常见瞬时故障，几秒后即可恢复）。
    //   2) 运行中崩溃/退出：主循环每 tick 检测到 runtime 停但 want=true 时自动重启。
    // 用户 /api/control/stop 会把 want 置 false，此后不再自动拉起。
    std::string rt_error;
    {
        // IPC 已在 initialize() 中先于 CoreRuntime 启动（T02 死锁修复）；首次自动 start
        // 也必须加入同一生命周期事务，否则进程刚启动时收到 restart/MODEL_ACTIVATE
        // 会并发操作同一个 CoreRuntime。
        std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
        if (want_runtime_running_.load()) {
            running_model_id_.clear();
            // 待配置降级态：模型/参数可能已补全，先尝试整链重建再 start；
            // 重建仍缺配置时保持降级（不盲目 start 未初始化对象），等下一轮重试。
            bool degraded_ready = true;
            if (runtime_waiting_config_ && !try_resume_from_degraded(&rt_error)) {
                degraded_ready = false;
                model_failure_code_ = "WAITING_FOR_MODEL";
                model_failure_message_ = rt_error;
                TTBOX_LOG_DEBUG("待配置降级态保持: " + rt_error);
            }
            if (degraded_ready && auth::LicenseGate::instance().pipeline_allowed() && core_runtime_ &&
                core_runtime_->start(&rt_error)) {
                runtime_started_ = true;
                model_failure_code_.clear();
                model_failure_message_.clear();
                TTBOX_LOG_INFO("CoreRuntime 已启动，等待首帧真实 RKNN 推理与 Decode");
            } else if (degraded_ready && !auth::LicenseGate::instance().pipeline_allowed()) {
                // T1.10① / M2.03：授权未通过（缺 capture）⇒ 不启动 AI 路径（透传/Web 管理红线：永不停）。
                runtime_started_ = false;
            } else if (degraded_ready) {
                // 仅在真正尝试过 start 失败时才标 RKNN_INIT_FAILED（B3）；
                // 降级保持（degraded_ready=false）时上面已设 WAITING_FOR_MODEL，
                // 此处不得覆盖成误导性的"首次启动失败"。
                model_failure_code_ = "RKNN_INIT_FAILED";
                model_failure_message_ = rt_error;
                TTBOX_LOG_WARN("CoreRuntime 首次启动失败，进入后台自动重试: " + model_failure_message_);
                runtime_started_ = false;
            } else {
                runtime_started_ = false;  // 降级保持：等 MODEL_ACTIVATE / 下轮重试
            }
        } else {
            runtime_started_ = false;
        }
    }

    constexpr auto kTickMs = std::chrono::milliseconds(50);
    constexpr auto kHeartbeatSec = std::chrono::seconds(10);
    auto last_heartbeat = std::chrono::steady_clock::now();
    // 启动失败重试间隔（与 heartbeat 解耦：重试更激进，HDMI 恢复后 ~2s 内拉起）
    constexpr auto kRetryInterval = std::chrono::seconds(2);
    auto last_retry = std::chrono::steady_clock::now();
    // 更新冒烟自检的 ota_status 轮询间隔（1s 足够：更新器终态在自检跑起来后几秒内落盘）
    constexpr auto kPostUpdateSmokePoll = std::chrono::seconds(1);
    auto last_smoke_check = std::chrono::steady_clock::now();

    // ---- 采集活体看门狗 ----
    // capture 线程被驱动卡死时 CoreRuntime::running() 仍是 true，主循环的“自动重试”
    // 不会触发，表现为采集/推理同时零帧、Web 显示冻结前 FPS。这里监视 V4L2 帧计数器：
    // 运行满足宽限期后连续 3s 无新帧 → 强制 stop/start 重枚举 /dev/video0 恢复链路。
    // 同时监视推理 worker 聚合帧数：采集还在走但 worker 卡死（NPU/RGA 挂住）时，
    // 检测帧 5s 不增长也会触发同一套 stop/start，避免“画面在动但检测静默归零”。
    uint64_t watch_capture_frames = 0;
    uint64_t watch_processed = std::numeric_limits<uint64_t>::max();
    auto watch_capture_seen_at = std::chrono::steady_clock::now();
    auto watch_processed_seen_at = std::chrono::steady_clock::now();
    auto watch_capture_restart_at = std::chrono::steady_clock::now();
    bool watch_capture_running_seen = false;
    auto watch_capture_running_since = std::chrono::steady_clock::now();
    constexpr auto kCaptureStallTime = std::chrono::seconds(3);
    constexpr auto kInferStallTime = std::chrono::seconds(5);
    constexpr auto kCaptureWatchGrace = std::chrono::seconds(10);
    constexpr auto kCaptureRestartMinGap = std::chrono::seconds(15);

    while (!shutdown_flag().load()) {
        std::this_thread::sleep_for(kTickMs);
        // 授权失效时仍保持进程存活（通过 supervisor recover() 重启恢复），不主动自杀
        const auto now = std::chrono::steady_clock::now();
        // 自动重试、首帧提交、错误读取与 IPC 生命周期操作共享同一把锁，
        // 防止 MODEL_ACTIVATE / restart 正在重建对象时主循环并发 start/model_ready。
        {
            std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
            if (want_runtime_running_.load() && core_runtime_ && !core_runtime_->running()) {
                if (now - last_retry >= kRetryInterval) {
                    last_retry = now;
                    std::string retry_error;
                    // 待配置降级态：模型/参数可能已通过 IPC 补全，先尝试整链重建；
                    // 重建仍缺配置时保持降级，本轮不 start，等下一轮重试。
                    bool degraded_ready = true;
                    if (runtime_waiting_config_ && !try_resume_from_degraded(&retry_error)) {
                        degraded_ready = false;
                    }
                    if (degraded_ready && auth::LicenseGate::instance().pipeline_allowed() &&
                        core_runtime_ && core_runtime_->start(&retry_error)) {
                        runtime_started_ = true;
                        model_failure_code_.clear();
                        model_failure_message_.clear();
                        TTBOX_LOG_INFO("CoreRuntime 自动重试成功，等待真实模型首帧");
                    } else if (!degraded_ready) {
                        // B3：降级保持不得误标 RKNN_INIT_FAILED，刷新最新等待原因即可
                        model_failure_code_ = "WAITING_FOR_MODEL";
                        model_failure_message_ = retry_error;
                    } else if (!retry_error.empty()) {
                        model_failure_code_ = "RKNN_INIT_FAILED";
                        model_failure_message_ = retry_error;
                        TTBOX_LOG_DEBUG("CoreRuntime 自动重试中: " + retry_error);
                    }
                }
            }
            // 采集卡死检测：只有 runtime 稳定运行超宽限期后才允许触发，
            // 重启后 watch 状态重置，避免启动阶段 V4L2 打开慢造成误杀。
            if (core_runtime_ && core_runtime_->running() && core_runtime_->capture()) {
                if (!watch_capture_running_seen) {
                    watch_capture_running_seen = true;
                    watch_capture_running_since = now;
                }
                const uint64_t frames =
                    core_runtime_->capture()->metrics().capture_frames.load();
                if (watch_capture_frames == 0) {
                    watch_capture_frames = frames;
                    watch_capture_seen_at = now;
                } else if (frames != watch_capture_frames) {
                    watch_capture_frames = frames;
                    watch_capture_seen_at = now;
                }
                const uint64_t processed = core_runtime_->workers_processed();
                if (watch_processed == std::numeric_limits<uint64_t>::max() ||
                    processed != watch_processed) {
                    // 首观测或 worker 池被重建（模型热切换/看门狗重启）后重新基线，
                    // 避免把计数归零误判成卡死。
                    watch_processed = processed;
                    watch_processed_seen_at = now;
                }
                const bool grace_ok = watch_capture_running_seen &&
                    (now - watch_capture_running_since) >= kCaptureWatchGrace;
                const bool gap_ok =
                    (now - watch_capture_restart_at) >= kCaptureRestartMinGap;
                const bool capture_stalled =
                    (now - watch_capture_seen_at) >= kCaptureStallTime;
                // 推理卡死只在“采集仍然健康”时判断：若采集也停了，归采集看门狗处理。
                const bool capture_alive =
                    (now - watch_capture_seen_at) < kCaptureStallTime;
                const bool infer_stalled = capture_alive &&
                    (now - watch_processed_seen_at) >= kInferStallTime;
                if (grace_ok && gap_ok && (capture_stalled || infer_stalled)) {
                    const int64_t stall_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            (infer_stalled ? now - watch_processed_seen_at
                                           : now - watch_capture_seen_at)).count();
                    TTBOX_LOG_WARN(std::string(infer_stalled ? "推理卡死看门狗触发" : "采集卡死看门狗触发") +
                                   ": " + std::to_string(stall_ms) + "ms 无进展（frames=" +
                                   std::to_string(frames) + " processed=" +
                                   std::to_string(processed) + "），执行 stop/start 重枚举");
                    running_model_id_.clear();
                    want_runtime_running_.store(true);
                    core_runtime_->stop();
                    std::string restart_error;
                    if (auth::LicenseGate::instance().pipeline_allowed() &&
                        core_runtime_->start(&restart_error)) {
                        runtime_started_ = true;
                        model_failure_code_.clear();
                        model_failure_message_.clear();
                        TTBOX_LOG_INFO("采集卡死看门狗重枚举成功");
                    } else {
                        runtime_started_ = false;
                        model_failure_code_ = "CAPTURE_WATCHDOG_RESTART_FAILED";
                        model_failure_message_ = restart_error;
                        TTBOX_LOG_WARN("采集卡死看门狗重枚举失败: " + restart_error);
                    }
                    watch_capture_restart_at = now;
                    watch_capture_frames = 0;
                    watch_capture_seen_at = now;
                    watch_processed = std::numeric_limits<uint64_t>::max();
                    watch_processed_seen_at = now;
                    watch_capture_running_seen = false;
                }
            } else {
                watch_capture_running_seen = false;
            }
            if (core_runtime_ && core_runtime_->model_ready()) {
                const std::string selected = model_management_ ? model_management_->registry().active_model() : "";
                if (!selected.empty() && running_model_id_ != selected) {
                    // 模型实际就绪时先提交 runtime_profile.model_id；只有配置同步成功，
                    // 才把“当前模型”和首帧 PASS 对外发布，避免底层失败却报告成功。
                    std::string sync_error;
                    if (sync_model_id_to_profile(selected, &sync_error)) {
                        running_model_id_ = selected;
                        model_failure_code_.clear();
                        model_failure_message_.clear();
                        TTBOX_LOG_INFO("真实模型已就绪: running_model_id=" + running_model_id_ +
                                       " inference+decode=PASS");
                    } else {
                        running_model_id_.clear();
                        model_failure_code_ = "MODEL_PROFILE_PERSIST_FAILED";
                        model_failure_message_ = sync_error;
                        TTBOX_LOG_ERROR("真实模型首帧已完成，但当前模型状态提交失败: " + sync_error);
                    }
                }
            } else if (core_runtime_ && core_runtime_->running() && core_runtime_->model_errors() > 0) {
                if (model_failure_code_.empty()) {
                    model_failure_code_ = "INFERENCE_FAILED_OR_OUTPUT_INVALID";
                    model_failure_message_.clear();
                }
            }
            // ---- 更新冒烟自检收尾（2026-09-20 方案B）----
            // 命中"刚更新过"时已把流水线跑起来满足更新器健康门禁；这里等更新器把
            // ota_status.json 落成 SUCCESS/FAILED（且 version == 本版本）后，停回停止态并
            // 落盘 want=false。超时兜底：更新器异常/不写终态时也必须停，绝不无限运行。
            if (post_update_smoke_.load()) {
                if (now - last_smoke_check >= kPostUpdateSmokePoll) {
                    last_smoke_check = now;
                    const std::string terminal =
                        read_ota_terminal_state(ota_status_path_, post_update_smoke_expected_version_);
                    const bool timed_out = now_ms() >= post_update_smoke_deadline_ms_;
                    if (!terminal.empty() || timed_out) {
                        post_update_smoke_.store(false);
                        // 落盘 want=false：更新后一直保持停止，直到用户显式点启动（约定）。
                        want_runtime_running_.store(false);
                        persist_runtime_intent(false);
                        if (core_runtime_) core_runtime_->stop();
                        runtime_started_ = false;
                        running_model_id_.clear();
                        model_failure_code_.clear();
                        model_failure_message_.clear();
                        watch_capture_running_seen = false;
                        if (!terminal.empty()) {
                            TTBOX_LOG_INFO("更新自检完成（更新器状态=" + terminal +
                                           "）：AI 流水线停回停止态（更新后不点启动不跑）");
                        } else {
                            TTBOX_LOG_WARN("更新自检超时未见更新器终态，仍按约定停回停止态");
                        }
                    }
                }
            }
        }
        if (now - last_heartbeat >= kHeartbeatSec) {
            last_heartbeat = now;
            const bool rt_ok = core_runtime_ ? core_runtime_->running() : false;
            auto st = license_daemon_->status_snapshot();
            // T1.10①③：心跳刷新唯一执法快照（IPC 投影与后续会话边界判定都读它）。
            auth::LicenseGate::instance().publish(auth::to_snapshot(
                st, license_daemon_->store_load_result(), static_cast<int64_t>(now_ms())));
            TTBOX_LOG_DEBUG(std::string("heartbeat: runtime=") +
                            (rt_ok ? "running" : "stopped") +
                            " license_state=" +
                            std::to_string(static_cast<int>(st.state)));
        }
    }
    TTBOX_LOG_INFO("收到退出请求，正在停止...");
}
// 清理：先停 IPC（provider/handler 都捕获 this）→ 停/销毁 CoreRuntime 与输出 →
// 停授权守护 → 清 initialized_。幂等，可被 run() 之后或析构调用。
void Application::shutdown() {
    const bool was_running = running_.exchange(false);
    if (was_running) TTBOX_LOG_INFO("Application shutdown() 开始");

    // 先停止并排空 IPC：所有 provider/handler 都捕获 this，并可能读取 CoreRuntime、
    // ModelRegistry、授权对象。必须等连接线程退出后，才能销毁这些依赖。
    ipc_.stop();

    {
        std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
        if (core_runtime_) {
            TTBOX_LOG_INFO("停止 CoreRuntime...");
            core_runtime_->stop();
            runtime_started_ = false;
        }
        running_model_id_.clear();
        core_runtime_.reset();
        hid_output_.reset();
    }

    if (license_daemon_) {
        license_daemon_->stop();
        license_daemon_.reset();
    }
    license_client_.reset();

    initialized_ = false;
    TTBOX_LOG_INFO("=== " + std::string(kAppName) + " 已退出 ===");
}
// 请求退出：仅置进程级原子标志（signal handler 可直接调用，async-signal-safe）。
void Application::request_shutdown() {
    shutdown_flag().store(true);
}
// 对外状态查询：转发到 status_provider()（IPC 与内部共用同一实现）。
SystemStatus Application::status() const { return status_provider(); }
// 汇总运行状态：持生命周期锁，拼装进程/授权/指标，供 IPC SystemStatus 投影。
SystemStatus Application::status_provider() const {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    SystemStatus st;
    st.running = running_.load();
    st.app_name = kAppName;
    st.version = kCoreVersion;
    if (start_time_ms_ > 0.0) st.uptime_ms = now_ms() - start_time_ms_;
    st.ipc_socket = ipc_.socket_path();
    st.config_file = config_.path();
    // 当前模型只报告“已真实运行并通过首帧推理”的 running_model_id_。
    // active.json 只是待运行/已选中模型，由 MODEL_LIST.selected_model_id 单独表达；
    // 加载失败时禁止把 selected 当 current，避免 API 显示 B、底层实际没跑 B。
    st.current_model_id = running_model_id_;
    st.runtime_running = core_runtime_ ? core_runtime_->running() : false;
    // T1.10②：授权投影（唯一来源 = LicenseGate 快照；本层只映射字段，**不**重算授权语义）。
    // 无卡（M1 默认）⇒ gate 为 kUnknown ⇒ state="unactivated"、activated=false（A23 期望）。
    {
        const auth::LicenseSnapshot ls = auth::LicenseGate::instance().snapshot();
        st.license.activated = auth::wire_activated(ls);
        st.license.state = auth::wire_state_name(ls);
        st.license.plan = ls.plan;
        st.license.is_pro = ls.is_pro;
        st.license.features = ls.features;
        st.license.ui_brand = ls.ui_brand;  // M2：品牌由签名卡驱动（不可信态已回落 "ttbox"）
        // M2.05：短码（Gate 已派生；不可信态为 ""）。本层只映射。
        st.license.short_code = ls.short_code;
        // M2.03：能力位投影 —— ★ 只映射（值来自 Gate 快照的 features），**不重算**授权语义。
        st.license.capabilities.capture   = ls.capability(auth::feature_name::kCapture);
        st.license.capabilities.inference = ls.capability(auth::feature_name::kInference);
        st.license.capabilities.aim       = ls.capability(auth::feature_name::kAim);
        st.license.capabilities.ota       = ls.capability(auth::feature_name::kOta);
        // 契约（§3.1）：expires_at / grace_until 为 unix 秒（0 = 无期限/无）。
        st.license.expires_at = ls.expire_unix_ms > 0 ? ls.expire_unix_ms / 1000 : 0;
        st.license.grace_until = ls.grace_until_ms > 0 ? ls.grace_until_ms / 1000 : 0;
        st.license.last_error = ls.last_error;
        st.license.heartbeat_interval_s = ls.heartbeat_interval_s;
    }
    // G1：真实流水线指标（runtime 未运行时保持全 0 = unavailable）
    if (core_runtime_) {
        core_runtime_->collect_metrics(&st.metrics);
    }
    return st;
}
// 对 IPC 暴露的配置快照：优先返回运行时 canonical，缺失时回退宿主配置的 runtime_profile。
JsonValue Application::config_provider() const {
    std::lock_guard<std::mutex> config_lock(config_persist_mutex_);
    // G4 契约：Web 需要 RuntimeProfile 结构（前端 ConfigContext 深拷贝改字段 → 全量 PUT）。
    // 数据源优先级（唯一真源 = RuntimeConfig 内存 canonical）：
    //   1) runtime_config_ 内存快照（SET_CONFIG 热更新后的最新值）
    //   2) 回退到宿主配置文件的 runtime_profile 键（启动时来源）
    JsonValue data = JsonValue::object();
    JsonValue prof = JsonValue::object();
    if (auto snap = runtime_config_.snapshot()) {
        prof = snap->to_json();
    } else if (config_.loaded()) {
        const JsonValue* p = config_.root().find("runtime_profile");
        if (p != nullptr && p->is_object()) {
            prof = *p;
        }
    }
    data.set("runtime_profile", prof);
    data.set("config_file", JsonValue::string(config_.path()));
    return data;
}
// 析构兜底：若 run/shutdown 未被显式调用，补一次 shutdown()，绝不抛出。
Application::~Application() {
    if (running_.load() || initialized_) {
        try { shutdown(); } catch (...) {}
    }
}























// incoming_dir_of() 原在此定义（static，只给本 TU 用）；拆分后唯一使用者是
// ApplicationIpc.cpp 里的 handle_model_import ⇒ 已搬到 ApplicationInternal.hpp
// 供那个 TU 用（留在本 TU 就是一份没人用的死代码 + 一个"看起来在用"的假象）。













}  // namespace ttbox::core
