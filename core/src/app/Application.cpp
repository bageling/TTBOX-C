// Application.cpp — 应用生命周期实现
#include "app/Application.hpp"
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

std::atomic<bool> g_shutdown_requested{false};
std::atomic<bool>& shutdown_flag() { return g_shutdown_requested; }

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


double now_ms() {
    using clock = std::chrono::steady_clock;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch())
            .count());
}

namespace {

// ---- 更新冒烟自检（2026-09-20 方案B）----
// 自检最长等待（ms）：更新器异常/未写终态时的兜底停回时限，绝不无限运行。
constexpr double kPostUpdateSmokeMaxMs = 200000.0;

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

LogLevel parse_log_level(const std::string& s) {
    if (s == "debug") return LogLevel::kDebug;
    if (s == "warn") return LogLevel::kWarn;
    if (s == "error") return LogLevel::kError;
    if (s == "off") return LogLevel::kOff;
    return LogLevel::kInfo;
}

int parse_color_order(const std::string& s) {
    if (s == "rgb") return 1;
    return 0;
}

std::vector<int> parse_worker_cores(const std::string& s) {
    if (s.empty()) return cfg::default_worker_cores();

    std::vector<int> result;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, ',')) {
        // 去掉首尾空格和换行
        size_t start = token.find_first_not_of(" \t\r\n");
        size_t end = token.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        token = token.substr(start, end - start + 1);

        try {
            int core = std::stoi(token);
            // RKNN core_mask 是位掩码，不是 CPU 编号。RK3588 三个 NPU
            // 核心的独占掩码只有 1、2、4；3/5/6 会造成核心重叠调度。
            if (core == 1 || core == 2 || core == 4) {
                if (std::find(result.begin(), result.end(), core) == result.end()) {
                    result.push_back(core);
                }
            }
        } catch (...) {
            // 跳过非法值，不报错
        }
    }
    // 配置中出现非法/重叠 mask 时回退到稳定的独占组合，避免把错误
    // 参数直接传给 RKNN 后出现吞吐下降或不同版本驱动行为不一致。
    return result.empty() ? cfg::default_worker_cores() : result;
}

std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && (std::isspace(static_cast<unsigned char>(s[b-1])) ||
                     s[b-1] == '\n' || s[b-1] == '\r')) --b;
    return s.substr(a, b - a);
}

}  // namespace

Application::~Application() {
    if (running_.load() || initialized_) {
        try { shutdown(); } catch (...) {}
    }
}

std::string Application::resolve_license_card(const std::string& cli_license) const {
    if (!cli_license.empty()) return cli_license;

    // 系统路径 /etc/ttbox/license.key：与原 aibox /etc/aibox/ 1:1 对齐语义
    {
        std::ifstream f(paths::kSystemLicenseFile);
        if (f) {
            std::string s;
            std::getline(f, s);
            s = strip(s);
            if (!s.empty()) return s;
        }
    }

    // 配置 fallback（开发期）
    const std::string cfg = config_.get_string("license_card_key", "");
    if (!cfg.empty()) return cfg;

    // M2.02：激活过的卡持久化在 LicenseStore（license.json = 信封原文）。
    // 重启后的恢复链：--license > /etc/ttbox/license.key > config > store。
    // （不解析、不验签——读原文交给 LicenseDaemon.verify_once。）
    // ★ M2.07（D-D）：**cloud 形文档不是离线卡信封** —— 不得作为"卡"交给 OfflineCardClient
    //   （否则 Application::run()→verify_now_blocking() 会把它离线验签并抹掉 LicenseDaemon
    //   刚由 restore_cloud_doc_locked() 恢复的云态 ⇒ 重启后云激活丢失）。云态恢复归
    //   LicenseDaemon 负责，故对 cloud 形文档返回空。判定唯一真源 = StoreLoadResult::doc_is_cloud。
    {
        auth::LicenseStore store;
        const auth::StoreLoadResult lr = store.load();
        // 判定唯一真源 = auth::offline_card_doc()（含 D-D 守卫：cloud 形文档不得当离线卡）。
        const std::string doc = auth::offline_card_doc(lr);
        if (!doc.empty()) return doc;
    }
    return {};
}

// ★ M2.03：会话边界读一次 LicenseGate 快照 → 三项特性 gate（唯一读点，不每帧查）。
CoreRuntime::FeatureGates Application::current_feature_gates() const {
    const auth::LicenseSnapshot snap = auth::LicenseGate::instance().snapshot();
    CoreRuntime::FeatureGates g;
    g.capture   = snap.feature_enabled(auth::feature_name::kCapture);
    g.inference = snap.feature_enabled(auth::feature_name::kInference);
    g.aim       = snap.feature_enabled(auth::feature_name::kAim);
    return g;
}

// ★ M2.03：卡内 ui_brand 的大写形（水印文本用）。只读 Gate 快照的投影字段，不推导授权。
std::string Application::brand_upper() const {
    std::string brand = auth::LicenseGate::instance().snapshot().ui_brand;
    if (brand.empty()) brand = auth::default_ui_brand();
    for (char& c : brand) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return brand;
}

// ★ M2.03：点名缺失 feature（诊断文案用；全缺 = "capture,inference,aim"）。
std::string Application::gate_missing_summary() const {
    const auth::LicenseSnapshot snap = auth::LicenseGate::instance().snapshot();
    std::string miss;
    const auto add = [&miss](const char* name) {
        if (!miss.empty()) miss += ",";
        miss += name;
    };
    if (!snap.feature_enabled(auth::feature_name::kCapture)) add("capture");
    if (!snap.feature_enabled(auth::feature_name::kInference)) add("inference");
    if (!snap.feature_enabled(auth::feature_name::kAim)) add("aim");
    if (miss.empty()) miss = "none";
    return miss;
}

// ★ M2.03：预览降级参数（唯一计算点；PreviewModule 内不查授权，只执行参数）。
//   全功能 ⇔ capture ∧ inference ∧ aim ⇒ 配置帧率、无水印；
//   否则 ⇒ fps = min(配置值, 5) + 水印（文本 = 卡内 ui_brand 大写形 + " - LIMITED"）。
void Application::apply_preview_degrade(CoreRuntime::Params* params,
                                        const CoreRuntime::FeatureGates& gates) const {
    if (params == nullptr) return;
    const int cfg_fps = static_cast<int>(config_.get_int("preview_fps", cfg::kPreviewFpsDefault));
    const bool full = gates.capture && gates.inference && gates.aim;
    params->preview.watermark = !full;
    params->preview.fps = full
                              ? cfg_fps
                              : std::max(1, std::min(cfg_fps, kRestrictedPreviewFps));
    params->preview.watermark_text.clear();
    if (!full) {
        // 全 ASCII 文本（内嵌 5×7 位图字体可绘）；'·' 为非 ASCII，故用 '-' 作分隔。
        params->preview.watermark_text = brand_upper() + " - LIMITED";
    }
}

// 输出总闸自愈（一次性、只补不盖）
//
// 背景：出厂基线 00-factory.json 曾把 output_enabled 固定为 false，而设备层
// 10-device.json 从不覆盖它 ⇒ 合并值恒为 false ⇒ OutputBackend::gate_allows()
// 第一句就 return false，注入一条都发不出去。更糟的是 AimThread 自己的
// injection_allowed 只看 mouse.enabled + 热键，面板因此显示"允许注入"，
// 两个闸门判断不一致，排障时极易被误导（表现为"能识别但自瞄完全没效果"）。
//
// 触发条件（三条全满足才写，且写完后设备层就有了显式键 ⇒ 只跑一次）：
//   1. 分层配置生效（存在可写的设备层）
//   2. output_backend 是真实输出端点（usb_proxy / local_hid）
//   3. 设备层**未显式**设置 output_enabled，且合并值仍为 false
// 设备层显式写过的一律尊重，绝不覆盖（部署方可能真的要用它做 kill switch）。
bool Application::migrate_output_enabled(std::string* note) {
    if (note) note->clear();
    if (!config_.is_layered()) return false;

    const std::string kind = config_.get_string("output_backend", "aibox");
    if (kind != "usb_proxy" && kind != "local_hid") return false;
    if (config_.device_layer_has("output_enabled")) return false;
    if (config_.get_bool("output_enabled", true)) return false;

    JsonValue view = config_.root();
    view.set("output_enabled", JsonValue::boolean(true));
    std::string err;
    if (!config_.persist(view, &err)) {
        if (note) *note = "写回设备层失败: " + err;
        return false;
    }
    config_.replace_root(std::move(view));
    if (note) {
        *note = "出厂基线 output_enabled=false 会永久封死注入，已在设备层补写 true";
    }
    return true;
}

bool Application::build_runtime_params(CoreRuntime::Params& out_params,
                                       const CoreRuntime::FeatureGates& gates,
                                       std::string* error) {
    out_params.gates = gates;  // ★ M2.03：特性级启停（start() 据此逐模块启停）
    out_params.capture.device = config_.get_string("capture_device", "/dev/video0");
    // HDMI-RX 输入必须走 V4L2 video 节点。DRM card/render 节点仅用于
    // loopout/NPU，误填会导致采集线程打开错误设备并让整条链路失效。
    if (out_params.capture.device != "/dev/video0") {
        if (error) {
            *error = "INVALID_CAPTURE_DEVICE: RK3588 HDMI-RX 输入必须使用 /dev/video0";
        }
        out_params.capture.device = "/dev/video0";
        return false;
    }
    out_params.capture.num_buffers =
        static_cast<uint32_t>(config_.get_int("capture_buffers", 8));
    out_params.capture.poll_timeout_ms =
        config_.get_int("capture_poll_timeout_ms", 1000);
    // P-ZC-1 采集层硬件裁剪（VIDIOC_S_SELECTION）。
    // ★ 这里补的是一条真漏掉的接线：此前 config 的 crop_width/crop_height 只喂给了
    //   预览（out_params.preview.crop_*），V4L2Capture::Params::crop_* 从来没被赋值，
    //   于是 open() 里那段 Selection 代码整段被跳过 —— 采集一直是全帧（2560×1440）。
    //   后果有两个：① RGA 每帧要把 3.7M 像素缩到模型输入（面积约 56×），这是
    //   resize_ms 的大头；② 16:9 硬压成 1:1，画面变形，检测框坐标跟着歪。
    //   接上后按配置只采集中心这一块，两个问题一起消失。
    //   取值优先级：runtime_profile.video（面板设过） > 全局 config > 0（不裁剪）。
    {
        uint32_t crop_w = static_cast<uint32_t>(config_.get_int("crop_width", 0));
        uint32_t crop_h = static_cast<uint32_t>(config_.get_int("crop_height", 0));
        if (auto snap = runtime_config_.snapshot(); snap && !snap->video.using_global_crop()) {
            crop_w = snap->video.crop_width;
            crop_h = snap->video.crop_height;
        }
        out_params.capture.crop_width = crop_w;
        out_params.capture.crop_height = crop_h;
        out_params.capture.crop_center = true;  // 自瞄画面语义就是中心，居中在 open() 内按帧尺寸算
        if (crop_w > 0 && crop_h > 0) {
            TTBOX_LOG_INFO("采集层硬件裁剪已启用: " + std::to_string(crop_w) + "x" +
                           std::to_string(crop_h) + "（居中）");
        } else {
            TTBOX_LOG_INFO("采集层硬件裁剪未启用（crop=0）⇒ 采集全帧，RGA 负责缩放");
        }
    }

    out_params.workers.model_path = config_.get_string("model_path", "");
    // ★ M2.03 特性感知：仅当 gates.inference 才解析/校验模型；关闭时跳过（"仅采集"可起）。
    if (gates.inference && model_management_) {
        const std::string selected = model_management_->registry().active_model();
        if (selected.empty()) {
            if (error) *error = "MODEL_NOT_SELECTED";
            return false;
        }
        const auto records = model_management_->registry().records();
        const ModelRecord* selected_record = nullptr;
        for (const auto& record : records) {
            if (record.model_id == selected) {
                selected_record = &record;
                break;
            }
        }
        if (selected_record == nullptr) {
            if (error) *error = "MODEL_NOT_FOUND: " + selected;
            return false;
        }
        if (selected_record->status == ModelStatus::kInvalid ||
            selected_record->status == ModelStatus::kFailed ||
            selected_record->path.empty()) {
            if (error) {
                *error = selected_record->failure_code.empty()
                             ? "MODEL_METADATA_INVALID: " + selected
                             : selected_record->failure_code + ": " + selected_record->failure_message;
            }
            return false;
        }
        out_params.workers.model_path = selected_record->path;
        if (selected_record->manifest.input_width > 0) {
            out_params.workers.out_w = selected_record->manifest.input_width;
        }
        if (selected_record->manifest.input_height > 0) {
            out_params.workers.out_h = selected_record->manifest.input_height;
        }
        // 每个模型的并发独立配置（manifest.worker_cores），未配置时回退到全局 config。
        const std::string& manifest_cores = selected_record->manifest.worker_cores;
        if (!manifest_cores.empty()) {
            out_params.workers.worker_cores = parse_worker_cores(manifest_cores);
        } else {
            out_params.workers.worker_cores =
                parse_worker_cores(config_.get_string("worker_cores", ""));
        }
    } else if (gates.inference) {
        out_params.workers.worker_cores =
            parse_worker_cores(config_.get_string("worker_cores", ""));
    } else {
        // ★ M2.03：inference 未授权（受限卡）⇒ 不解析模型、**不因 MODEL_NOT_SELECTED 失败**
        //   （否则"仅采集"起不来 ⇒ B6 必 FAIL）。worker_cores 仍填（CoreRuntime::initialize
        //   要求非空）但 WorkerPool 不会被 start()。
        out_params.workers.worker_cores =
            parse_worker_cores(config_.get_string("worker_cores", ""));
        TTBOX_LOG_INFO("feature 'inference' 未启用：跳过模型解析（仅采集模式）");
    }
    if (gates.inference && out_params.workers.model_path.empty()) {
        if (error) *error = "MODEL_NOT_FOUND: model_path 为空";
        return false;
    }
    // 跳过 CPU↔NPU 缓存同步：零拷贝 I/O 下可降低推理延迟（实测对比后决定是否开启）。
    out_params.workers.disable_cache_flush =
        config_.get_bool("rknn_disable_cache_flush", false);
    // 实验开关：RGA 输出 DMA-BUF 直绑 RKNN 输入，省掉每帧 CPU memcpy。
    // 默认关闭；开启后 WorkerPool 会在 fd 变化时重新绑定，失败自动回退 CPU 拷贝。
    // P-ZC-1：面板可在「性能」里改（runtime_profile.video.zero_copy_input）。
    //   之所以要这个覆盖层：运行配置是客户层单文件、OTA 不覆盖（见 P-ZC-1），
    //   老机器升级到 1.5.33 后这里的全局值仍是 false，零拷贝永远打不开。
    //   只有 profile 里**真的写过**这个键才覆盖（zero_copy_input_set）。
    out_params.workers.external_dma_input =
        config_.get_bool("rknn_external_dma_input", false);
    if (auto snap = runtime_config_.snapshot(); snap && snap->video.zero_copy_input_set) {
        out_params.workers.external_dma_input = snap->video.zero_copy_input;
    }
    if (out_params.workers.out_w == 0) {
        out_params.workers.out_w =
            static_cast<uint32_t>(config_.get_int("model_input_width", 640));
    }
    if (out_params.workers.out_h == 0) {
        out_params.workers.out_h =
            static_cast<uint32_t>(config_.get_int("model_input_height", 640));
    }
    // ★ M2.03：预览降级（fps 封顶 + 水印）唯一计算点在此（PreviewModule 内不查授权）。
    apply_preview_degrade(&out_params, gates);
    out_params.preview.crop_width = static_cast<uint32_t>(config_.get_int("crop_width", 640));
    out_params.preview.crop_height = static_cast<uint32_t>(config_.get_int("crop_height", 640));
    out_params.preview.jpeg_quality = static_cast<int>(config_.get_int("preview_quality", 60));
    out_params.preview.draw_detections = config_.get_bool("preview_draw_detections", false);
    out_params.workers.conf_thres =
        static_cast<float>(config_.get_double("conf", 0.25));
    out_params.workers.iou_thres =
        static_cast<float>(config_.get_double("nms", 0.45));
    out_params.workers.color_order =
        parse_color_order(config_.get_string("model_color_order", "bgr"));
    out_params.workers.pass_through =
        config_.get_bool("model_pass_through", true);
    // 第13阶段：链路诊断开关（默认关闭；开启后每 N 帧输出一次完整链路）
    out_params.pipeline_debug_enabled =
        config_.get_bool("pipeline_debug_enabled", false);
    out_params.pipeline_debug_interval =
        static_cast<uint32_t>(config_.get_int("pipeline_debug_interval", 60));
    // 第13阶段：PID Trace 采集（默认关闭；开启后逐帧写 CSV，只记录不改变行为）
    out_params.pid_trace_enabled = config_.get_bool("pid_trace_enabled", false);
    out_params.pid_trace_path = config_.get_string("pid_trace_path", "/tmp/pid_trace.csv");
    // 第15阶段：目标预测时域（秒；0=关闭预测，保持原行为）
    out_params.prediction_time_s =
        static_cast<float>(config_.get_double("prediction_time_s", 0.0));

    // 先做一次输出总闸自愈（见 Application::migrate_output_enabled 说明）：
    // 必须在读取 output_enabled 之前跑，否则读到的是出厂基线的陈旧值。
    std::string migration_note;
    if (migrate_output_enabled(&migration_note) && !migration_note.empty()) {
        TTBOX_LOG_WARN("输出总闸自愈: " + migration_note);
    }

    const std::string output_kind = config_.get_string("output_backend", "aibox");
    // output_enabled = 后端静态总闸（kill switch）：只有显式置 false 才关闭。
    // 历史上出厂基线把它固定为 false 且没有任何 UI/流程会置 true，
    // 等价于出厂即永久封死注入（表现为"能识别但自瞄完全没效果"）。
    // 用户级开关是 runtime mouse.enabled（面板可控、实时生效），这里默认启用。
    bool enabled = config_.get_bool("output_enabled", true);
    if (output_kind == "fifo") {
        const std::string fifo_path =
            config_.get_string("output_fifo_path", "/tmp/ttbox_hid.fifo");
        hid_output_ = std::make_shared<output::FifoHidOutput>(fifo_path);
    } else if (output_kind == "local_hid" || output_kind == "usb_proxy") {
        // 统一 OutputBackend：按 kind 选择后端，行为与 AiboxHidOutput 完全一致
        // （local_hid 即原 aibox 逻辑迁移）。
        auto backend = std::make_shared<output::OutputBackend>();
        output::OutputBackend::Params bp;
        bp.kind = output_kind;
        bp.hidg_path = config_.get_string("output_hidg_path", "/dev/hidg0");
        bp.proxy_socket_path = config_.get_string("output_proxy_socket", paths::kMouseCmdSocketDefault);
        bp.enabled = enabled;
        bp.runtime_config = &runtime_config_;
        // button_source 由 Application::start 阶段绑定（见 add_hid_button_source 处）
        std::string berr;
        if (!backend->configure(bp, &berr)) {
            TTBOX_LOG_WARN("OutputBackend 配置失败（回退 aibox）: " + berr);
        } else {
            hid_output_ = std::move(backend);
        }
    } else if (output_kind == "kmboxnet" || output_kind == "makcu" ||
               output_kind == "ferrum" || output_kind == "kmboxb" ||
               output_kind == "catnet") {
        // S1-2026-09-18（A801）：键鼠盒子功能整体移除。
        // 历史配置里残留的盒子 kind 在此显式告警后落到下方 aibox 兜底，
        // 不走"未知 kind 一律 fail-closed"（默认值 "aibox" 本身就依赖兜底分支）。
        TTBOX_LOG_WARN("output_backend='" + output_kind +
                       "' 为已移除的键鼠盒子后端（A801），回退 aibox 本机 HID");
    }
    if (!hid_output_) {
        const std::string hidg_path =
            config_.get_string("output_hidg_path", "/dev/hidg0");
        auto output = std::make_shared<output::AiboxHidOutput>(hidg_path);
        // output_enabled 是后端静态总闸（不写配置时默认关闭，fail-closed）。
        // mouse.enabled 由 AimThread 与输出后端每周期实时读取 RuntimeConfig，
        // 不在此快照 —— 用户改配置后无需重启即生效。
        output->set_enabled(enabled);
        output->set_config_source(&runtime_config_);
        hid_output_ = std::move(output);
    }
    out_params.output = hid_output_;
    out_params.runtime_config = &runtime_config_;
    out_params.mouse_event_socket =
        config_.get_string("input_event_socket", paths::kMouseEventSocketDefault);
    return true;
}

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
                "                [--log-level debug|info|warn|error|off]\n"
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
    TTBOX_LOG_INFO("=== " + std::string(kAppName) + " v" +
                   std::string(kCoreVersion) + " 启动 ===");

    // ---- 风扇满转（fan_control min_pwm=100）：防热节流拖慢 NPU ----
    {
        std::ofstream pwm("/sys/class/hwmon/hwmon8/pwm1");
        if (pwm) {
            pwm << 255;
            TTBOX_LOG_INFO("风扇已设满转（防热节流）");
        } else {
            TTBOX_LOG_WARN("风扇控制不可用（hwmon8/pwm1）");
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

void Application::request_shutdown() {
    shutdown_flag().store(true);
}

SystemStatus Application::status() const { return status_provider(); }

auth::LicenseStatus Application::license_status_snapshot() const {
    return license_daemon_ ? license_daemon_->status_snapshot()
                           : auth::LicenseStatus{};
}
bool Application::license_allow_run() const {
    return license_daemon_ && license_daemon_->allow_run();
}
bool Application::license_is_pro() const {
    return license_daemon_ && license_daemon_->is_pro();
}

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

// SET_CONFIG 原子更新：
//   JSON 解析 → RuntimeProfile::validate → RuntimeConfig.update（内存原子替换）→ 持久化。
// 任何一步失败都直接返回 false；内存与磁盘均保证不被污染。
bool Application::handle_config_update(const JsonValue& profile_json, std::string* error,
                                       bool* persisted) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (persisted) *persisted = false;
    if (!profile_json.is_object()) {
        if (error) *error = "profile 必须是 JSON 对象";
        return false;
    }

    // 1) API 原始输入严格校验：from_json() 对“磁盘历史坏配置”会执行自愈，
    // 但 SET_CONFIG 是新写入请求，不能把非法 1×1 偷偷改成 0×0 后报告成功。
    // 必须在解析前检查原始 capture；0=全帧，非零下限与 RuntimeProfile 一致。
    if (const JsonValue* capture = profile_json.find("capture");
        capture && capture->is_object()) {
        // 常量单一权威源：ttbox::core::kMinCaptureRoiPx / kMaxCaptureRoiPx
        // （定义在 RuntimeProfile.hpp，DUP-10），与 RuntimeProfile::validate 共用。
        for (const char* key : {"width", "height"}) {
            if (const JsonValue* value = capture->find(key); value && value->is_number()) {
                const int64_t pixels = value->as_int();
                if (pixels != 0 &&
                    (pixels < kMinCaptureRoiPx || pixels > kMaxCaptureRoiPx)) {
                    if (error) {
                        *error = std::string("profile 校验失败: capture.") + key +
                                 " 非法（0=全帧，或需在 " + std::to_string(kMinCaptureRoiPx) +
                                 "~" + std::to_string(kMaxCaptureRoiPx) + " 之间）";
                    }
                    return false;
                }
            }
        }
    }

    // 2) 解析 canonical profile，并执行完整语义校验
    RuntimeProfile profile = RuntimeProfile::from_json(profile_json);
    std::string validate_error;
    if (!profile.validate(&validate_error)) {
        if (error) *error = validate_error.empty() ? "profile 校验失败" : ("profile 校验失败: " + validate_error);
        return false;
    }

    // model_id 代表真正完成 RKNN 加载与首帧验证的当前模型，只允许 MODEL_ACTIVATE
    // 事务修改。普通 SET_CONFIG 改它会绕过 ModelRegistry、切换和回滚门槛。
    if (auto current = runtime_config_.snapshot(); current && profile.model_id != current->model_id) {
        if (error) *error = "model_id 只能通过模型激活接口修改";
        return false;
    }

    // 3) 唯一提交入口在同一把锁内完成：canonical 落盘、ConfigManager 根节点刷新、
    // RuntimeConfig 内存发布。并发请求因此不会产生“磁盘最后是 B、内存最后是 A”。
    std::string persist_error;
    if (!persist_runtime_profile(profile, &persist_error)) {
        if (error) *error = "写入配置文件失败，现配置保持不变: " + persist_error;
        return false;
    }
    if (persisted) *persisted = true;
    return true;
}

// M2.02：ACTIVATE_LICENSE 的 Application 侧——激活 + Gate publish + wire 投影。
// 失败语义 = fail-closed：拒绝时（坏卡/他板卡/过期卡）**不 publish 投影数据**，
// 但 Gate 快照仍刷新（拒绝态也是真状态；AI 路径由 ai_allowed() 决定，恒不放开）。
bool Application::handle_license_activate(const std::string& card_envelope,
                                         JsonValue* data, std::string* error) {
    if (!license_daemon_) {
        if (error) *error = "license daemon 未初始化";
        return false;
    }
    std::string aerr;
    const bool ok = license_daemon_->activate(card_envelope, &aerr);
    // 无论成败：把最新授权态 publish 到 LicenseGate（唯一执法快照），
    // 使主循环自动拉起逻辑（run loop 的 ai_allowed 重查）立即看到新状态。
    auth::LicenseGate::instance().publish(auth::to_snapshot(
        license_daemon_->status_snapshot(), license_daemon_->store_load_result(),
        static_cast<int64_t>(now_ms())));
    if (!ok) {
        if (error) *error = aerr.empty() ? "激活被拒绝" : aerr;
        return false;
    }
    // wire 投影（§3.2 口径：state 小写名 + activated 派生 + 签名卡内容字段）
    if (data) {
        const auth::LicenseSnapshot ls = auth::LicenseGate::instance().snapshot();
        *data = JsonValue::object();
        data->set("state", JsonValue::string(auth::wire_state_name(ls)));
        data->set("activated", JsonValue::boolean(auth::wire_activated(ls)));
        data->set("plan", JsonValue::string(ls.plan));
        data->set("is_pro", JsonValue::boolean(ls.is_pro));
        data->set("ui_brand", JsonValue::string(ls.ui_brand));
        data->set("expire_unix_ms",
                  JsonValue::number(static_cast<double>(ls.expire_unix_ms)));
        JsonValue feats = JsonValue::array();
        for (const std::string& f : ls.features) {
            feats.push_back(JsonValue::string(f));
        }
        data->set("features", std::move(feats));
    }
    return true;
}

// ---------------------------------------------------------------------------
// M2.07：ACTIVATE_CLOUD 的 Application 侧（契约 §3.2）
// ---------------------------------------------------------------------------
// params: {
//   "expire_unix_ms": <ms, 必填, >now 否则 400>,
//   "features": ["capture","inference","aim","ota"],   // web 下发（D5 全闭集），
//                                                      // core normalize 收窄
//   "plan": "subscription",        // 可选，缺省 'none'
//   "card_mask": "LS-****-XXXX",   // 可选
//   "max_devices": 3,              // 可选，仅投影
//   "source": "cloud",             // 固定
//   "deactivate": false            // true ⇒ 立即锁定（快路径），不落盘
// }
// 成功 data 与 ACTIVATE_LICENSE 同形（state/activated/plan/is_pro/ui_brand/
// expire_unix_ms/features）。授权执法点（LicenseGate/FeatureGates）不变。
bool Application::handle_license_activate_cloud(const JsonValue& params,
                                                JsonValue* data, std::string* error) {
    if (!license_daemon_) {
        if (error) *error = "license daemon 未初始化";
        return false;
    }

    // ---- deactivate:true ⇒ 快路径锁定（D4；web 收到云端 403 时调用）----
    const JsonValue* deact_v = params.find("deactivate");
    if (deact_v != nullptr && deact_v->as_bool(false)) {
        std::string reason;
        if (const JsonValue* r = params.find("reason");
            r != nullptr && r->is_string()) {
            reason = r->as_string();
        }
        license_daemon_->deactivate_cloud(reason);
        // publish：锁定态立即进入唯一执法快照 → pipeline_allowed()=false → AI 停
        auth::LicenseGate::instance().publish(auth::to_snapshot(
            license_daemon_->status_snapshot(), license_daemon_->store_load_result(),
            static_cast<int64_t>(now_ms())));
        if (data) {
            const auth::LicenseSnapshot ls = auth::LicenseGate::instance().snapshot();
            *data = JsonValue::object();
            data->set("state", JsonValue::string(auth::wire_state_name(ls)));
            data->set("activated", JsonValue::boolean(auth::wire_activated(ls)));
            data->set("plan", JsonValue::string(ls.plan));
            data->set("is_pro", JsonValue::boolean(ls.is_pro));
            data->set("ui_brand", JsonValue::string(ls.ui_brand));
            data->set("expire_unix_ms",
                      JsonValue::number(static_cast<double>(ls.expire_unix_ms)));
            JsonValue feats = JsonValue::array();
            for (const std::string& f : ls.features) {
                feats.push_back(JsonValue::string(f));
            }
            data->set("features", std::move(feats));
        }
        return true;
    }

    // ---- 正常云激活 ----
    const JsonValue* expire_v = params.find("expire_unix_ms");
    if (expire_v == nullptr || !expire_v->is_number()) {
        if (error) *error = "缺少必需字段 'expire_unix_ms'（数字，Unix 毫秒）";
        return false;
    }
    const int64_t expire_unix_ms = expire_v->as_int(0);

    std::vector<std::string> features;
    if (const JsonValue* f = params.find("features");
        f != nullptr && f->is_array()) {
        for (const JsonValue& e : f->as_array()) {
            if (e.is_string()) features.push_back(e.as_string());
        }
    }
    std::string plan;
    if (const JsonValue* p = params.find("plan"); p != nullptr && p->is_string()) {
        plan = p->as_string();
    }
    std::string card_mask;
    if (const JsonValue* m = params.find("card_mask"); m != nullptr && m->is_string()) {
        card_mask = m->as_string();
    }

    std::string aerr;
    const bool ok = license_daemon_->activate_cloud(expire_unix_ms, features, plan,
                                                    card_mask, &aerr);
    // 无论成败：把最新授权态 publish 到 LicenseGate（唯一执法快照）。
    auth::LicenseGate::instance().publish(auth::to_snapshot(
        license_daemon_->status_snapshot(), license_daemon_->store_load_result(),
        static_cast<int64_t>(now_ms())));
    if (!ok) {
        if (error) *error = aerr.empty() ? "云端激活被拒绝" : aerr;
        return false;
    }
    // wire 投影（与 handle_license_activate 同形；max_devices 仅回显）
    if (data) {
        const auth::LicenseSnapshot ls = auth::LicenseGate::instance().snapshot();
        *data = JsonValue::object();
        data->set("state", JsonValue::string(auth::wire_state_name(ls)));
        data->set("activated", JsonValue::boolean(auth::wire_activated(ls)));
        data->set("plan", JsonValue::string(ls.plan));
        data->set("is_pro", JsonValue::boolean(ls.is_pro));
        data->set("ui_brand", JsonValue::string(ls.ui_brand));
        data->set("expire_unix_ms",
                  JsonValue::number(static_cast<double>(ls.expire_unix_ms)));
        JsonValue feats = JsonValue::array();
        for (const std::string& f : ls.features) {
            feats.push_back(JsonValue::string(f));
        }
        data->set("features", std::move(feats));
        if (const JsonValue* md = params.find("max_devices");
            md != nullptr && md->is_number()) {
            data->set("max_devices", JsonValue::number(md->as_number()));
        }
    }
    return true;
}

// RUNTIME_CONTROL：start / stop / restart。
// 直接复用 CoreRuntime start/stop（幂等），不触碰平台 RuntimeController 状态机。
bool Application::handle_runtime_control(const std::string& action, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!core_runtime_) {
        if (error) *error = "core_runtime 未初始化";
        return false;
    }
    if (action == "start") {
        // 用户显式点启动 ⇒ 取消更新冒烟自检的"稍后自动停"（用户意愿优先）。
        post_update_smoke_.store(false);
        if (core_runtime_->running()) return true;  // 幂等
        // T1.10① / M2.03：授权未通过不启动 AI 路径（透传/Web 管理不受影响）。
        if (!auth::LicenseGate::instance().pipeline_allowed()) {
            if (error) *error = "授权未通过：缺少 feature 'capture'，AI 功能路径不启动";
            return false;
        }
        want_runtime_running_.store(true);
        persist_runtime_intent(true);
        running_model_id_.clear();
        // R4：降级态（core_runtime_ 已分配但未 initialize）先整链重建再 start；
        // 重建失败返回真实等待原因，而不是"内部对象未初始化"这类误导性内部错误。
        if (runtime_waiting_config_ && !try_resume_from_degraded(error)) {
            if (error && error->empty()) *error = "等待模型配置（WAITING_FOR_MODEL）";
            model_failure_code_ = "WAITING_FOR_MODEL";
            model_failure_message_ = error ? *error : "";
            // want 保持 true：主循环 2s 自动重试，配置补全即自动拉起
            return false;
        }
        if (!core_runtime_->start(error)) {
            if (error && error->empty()) *error = "CoreRuntime 启动失败";
            // 启动失败仍保留 want=true：主循环每 2s 自动重试，直到 HDMI/模型就绪
            return false;
        }
        runtime_started_ = true;
        return true;
    }
    if (action == "stop") {
        // 用户显式点停止 ⇒ 同时取消更新冒烟自检（用户意愿优先）。
        post_update_smoke_.store(false);
        want_runtime_running_.store(false);
        persist_runtime_intent(false);
        core_runtime_->stop();
        runtime_started_ = false;
        running_model_id_.clear();
        model_failure_code_.clear();
        model_failure_message_.clear();
        return true;
    }
    if (action == "restart") {
        // 用户显式重启 ⇒ 取消更新冒烟自检（视为"保持运行"，不再自动停）。
        post_update_smoke_.store(false);
        // T1.10① / M2.03：授权未通过不重启 AI 路径。
        if (!auth::LicenseGate::instance().pipeline_allowed()) {
            if (error) *error = "授权未通过：缺少 feature 'capture'，AI 功能路径不启动";
            return false;
        }
        want_runtime_running_.store(true);
        persist_runtime_intent(true);
        // 新运行世代在首帧 inference+decode 通过前没有 current model。
        // 禁止 restart 返回后继续展示旧世代 running_model_id。
        running_model_id_.clear();
        model_failure_code_.clear();
        model_failure_message_.clear();
        core_runtime_->stop();
        // R4：同 start——降级态先整链重建，失败返回真实等待原因
        if (runtime_waiting_config_ && !try_resume_from_degraded(error)) {
            if (error && error->empty()) *error = "等待模型配置（WAITING_FOR_MODEL）";
            model_failure_code_ = "WAITING_FOR_MODEL";
            model_failure_message_ = error ? *error : "";
            runtime_started_ = false;
            return false;
        }
        if (!core_runtime_->start(error)) {
            if (error && error->empty()) *error = "CoreRuntime 重启失败";
            runtime_started_ = false;
            return false;
        }
        runtime_started_ = true;
        return true;
    }
    if (error) *error = "未知 action: " + action;
    return false;
}

// ---- R6 启动意图裁决（唯一入口）----
// 判据（任一成立即视为"刚更新过"）：
//   ① <state>/core_boot_version 存在且 != 当前 kCoreVersion ⇒ 中间换过版本；
//   ② 标记不存在（升到本特性首个版本时会这样），但 <state>/ota_status.json 记着
//      state=SUCCESS 且 version == 当前版本 ⇒ 更新器刚把本版本装上来。
// 命中 ⇒ 走「更新冒烟自检」（2026-09-20 方案B）：先置 want=true 把流水线跑起来，
//   满足旧版更新器"模型真跑过首帧"的健康门禁；等 ota_status 落成 SUCCESS/FAILED 后，
//   由主循环停回停止态并落盘 want=false（更新后一直保持停止，直到用户显式点启动）。
// 未命中 ⇒ 走 R5 还原用户显式意愿（无记录则保持默认自动启动）。
void Application::apply_startup_runtime_intent(const std::string& state_dir) {
    const std::string boot_version_path = state_dir + "/" + paths::kCoreBootVersionFileName;
    const std::string ota_status_path = state_dir + "/" + paths::kOtaStatusFileName;
    const std::string cur_version(kCoreVersion);

    std::string prev_boot_version;
    {
        std::ifstream in(boot_version_path, std::ios::binary);
        if (in) {
            std::getline(in, prev_boot_version);
            while (!prev_boot_version.empty() &&
                   (prev_boot_version.back() == '\r' || prev_boot_version.back() == '\n' ||
                    prev_boot_version.back() == ' ' || prev_boot_version.back() == '\t')) {
                prev_boot_version.pop_back();
            }
        }
    }

    // 判定规则见 app/RuntimeIntent.hpp（纯函数，host 可单测）。
    // ota_status.json 只在"还没有启动版本标记"时才用得上（规则②），省一次读盘。
    JsonValue ota_status;
    bool have_ota_status = false;
    if (prev_boot_version.empty()) {
        const JsonParseResult st = json_parse_file(ota_status_path);
        if (st.ok && st.value.is_object()) {
            ota_status = st.value;
            have_ota_status = true;
        }
    }
    const StartupIntent intent =
        decide_startup_intent(prev_boot_version, cur_version,
                              have_ota_status ? &ota_status : nullptr);

    // 记录本次启动版本（best-effort：失败只告警，绝不影响启停本身）。
    {
        const std::string tmp = boot_version_path + ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) {
            out << cur_version << "\n";
            out.flush();
            out.close();
            std::error_code ec;
            std::filesystem::rename(tmp, boot_version_path, ec);
            if (ec) {
                TTBOX_LOG_WARN("启动版本标记发布失败（忽略）: " + ec.message());
                std::error_code rm_ec;
                std::filesystem::remove(tmp, rm_ec);
            }
        } else {
            TTBOX_LOG_WARN("启动版本标记写入失败（打不开临时文件，忽略）: " + tmp);
        }
    }

    if (intent.just_updated) {
        // 2026-09-20 方案B：更新后不直接停，而是先自动跑一次推理当「更新冒烟自检」。
        // 为什么：旧版 OTA 更新器的健康门禁要求 IPC current_model_id 非空（= 模型真跑过
        // 首帧），而「更新后保持停止」会让门禁恒失败 ⇒ 每次更新都被判 health_check_failed
        // 并自动回滚（1.5.17 板端实测）。先自检跑起来满足门禁，等更新器把 ota_status.json
        // 落成 SUCCESS/FAILED 后，由主循环停回停止态（业主约定：更新后不点启动不跑）。
        want_runtime_running_.store(true);
        post_update_smoke_expected_version_ = cur_version;
        post_update_smoke_deadline_ms_ = now_ms() + kPostUpdateSmokeMaxMs;
        post_update_smoke_.store(true);
        TTBOX_LOG_INFO("检测到刚完成版本更新（" + intent.reason +
                       "）：先自动跑一次推理当更新自检，待更新器确认后自动停回停止态");
        return;
    }
    load_runtime_intent();
}

// ---- R5 用户启停意愿持久化 ----
// 只记录**用户显式** start / stop 的意愿（restart 视为 start），供 core 重启后还原。
// 语义边界：
//   · 无文件 / 字段缺失 / JSON 损坏 → 不改默认（want 保持 true），并留日志；
//   · 运行期崩溃、看门狗重启、模型热切换等**内部** want 变更**不落盘**——
//     那些不是用户意愿，语义仍是"应保持运行"。
// 目录由发布脚本 mkdir -p 且升级不清理（与 ota_status.json 同处），故 OTA 后仍能读到。
void Application::load_runtime_intent() {
    if (runtime_intent_path_.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(runtime_intent_path_, ec)) {
        TTBOX_LOG_INFO("无启停意愿记录，按默认处理（自动启动推理）: " + runtime_intent_path_);
        return;
    }
    const JsonParseResult parsed = json_parse_file(runtime_intent_path_);
    const JsonValue* field = parsed.ok ? parsed.value.find("want_runtime_running") : nullptr;
    if (!field || !field->is_bool()) {
        TTBOX_LOG_WARN("启停意愿文件不可用（" +
                       (parsed.ok ? std::string("缺 want_runtime_running 字段") : parsed.error) +
                       "），按默认处理（自动启动推理）");
        return;
    }
    const bool want = field->as_bool(true);
    want_runtime_running_.store(want);
    TTBOX_LOG_INFO(std::string("启停意愿已还原：推理 ") + (want ? "开启" : "关闭") +
                   "（上次用户显式 " + (want ? "start" : "stop") + "）");
}

// 原子发布：临时文件 + rename，避免断电/崩溃留下半截 JSON（与 persist_runtime_profile 同法）。
// 任一环节失败都只降级为 WARN：意愿落盘失败绝不能影响启停本身。
bool Application::persist_runtime_intent(bool want_running) {
    if (runtime_intent_path_.empty()) return false;
    JsonValue root = JsonValue::object();
    root.set("want_runtime_running", JsonValue::boolean(want_running));
    const std::string tmp = runtime_intent_path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            TTBOX_LOG_WARN("启停意愿写入失败（打不开临时文件，忽略）: " + tmp);
            return false;
        }
        out << root.dump();
        out.flush();
        if (!out.good()) {
            TTBOX_LOG_WARN("启停意愿写入失败（写临时文件出错，忽略）: " + tmp);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, runtime_intent_path_, ec);
    if (ec) {
        TTBOX_LOG_WARN("启停意愿发布失败（rename，忽略）: " + ec.message());
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }
    return true;
}

// ---- 模型管理（v0.3）实现 ----
// 收件目录：<registry_root>/_incoming。Gateway 上传端点先把文件写到这里，
// 再发 MODEL_IMPORT 引用路径。import 只允许引用收件目录内的文件（防任意路径读取）。
static std::string incoming_dir_of(const ModelRegistry& reg) {
    return reg.root_dir() + "/_incoming";
}

bool Application::handle_model_import(const std::string& src_path, const std::string& model_id,
                                      const std::string& label, const std::string& source_format,
                                      const std::string& sha256, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用（初始化失败）";
        return false;
    }
    ModelRegistry& reg = model_management_->registry();
    // 路径安全：src_path 必须位于收件目录内（防 ../ 任意文件读取）。
    // Windows 下 fs 返回反斜杠路径，统一归一化成 '/' 再比较。
    auto normalize = [](std::string s) {
        for (char& c : s) if (c == '\\') c = '/';
        return s;
    };
    std::error_code path_ec;
    const std::filesystem::path incoming_path =
        std::filesystem::weakly_canonical(incoming_dir_of(reg), path_ec);
    const std::filesystem::path source_path =
        std::filesystem::weakly_canonical(src_path, path_ec);
    const std::string incoming = normalize(incoming_path.string());
    const std::string src_norm = normalize(source_path.string());
    const std::string incoming_prefix = incoming.empty() || incoming.back() == '/'
                                            ? incoming : incoming + "/";
    // canonical 后再做“目录边界”判断：拒绝 ../、符号链接逃逸和
    // /_incoming_fake 这类仅字符串前缀相同的相邻目录。
    if (path_ec || src_norm.rfind(incoming_prefix, 0) != 0) {
        if (error) *error = "模型文件必须先上传到收件目录（" + incoming + "）";
        return false;
    }
    ModelManifest manifest;
    manifest.label = label.empty() ? model_id : label;
    manifest.origin = "local";
    // 来源格式：直接上传=rknn（默认）；ONNX 转换产物=onnx。落 manifest 供前端/排查。
    manifest.source_format = (source_format == "onnx") ? "onnx" : "rknn";
    if (!sha256.empty()) manifest.sha256 = sha256;  // Web 层 hashlib 计算（标准实现），Core 只存不算
    if (!reg.import(src_path, model_id, manifest, error)) return false;
    TTBOX_LOG_INFO("模型已导入 staging: " + model_id);
    return true;
}

JsonValue Application::handle_model_list() {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    JsonValue data = JsonValue::object();
    if (!model_management_) {
        data.set("models", JsonValue::array());
        data.set("selected_model_id", JsonValue::string(""));
        data.set("running_model_id", JsonValue::string(""));
        data.set("state", JsonValue::string("failed"));
        data.set("available", JsonValue::boolean(false));
        return data;
    }
    const ModelRegistry& reg = model_management_->registry();
    const std::string selected = reg.active_model();
    const std::string running = running_model_id_;
    JsonValue arr = JsonValue::array();
    for (const auto& record : reg.records()) {
        ModelRecord current = record;
        current.selected = (current.model_id == selected);
        current.running = (!running.empty() && current.model_id == running);
        if (current.running) current.status = ModelStatus::kRunning;
        else if (current.selected && current.status == ModelStatus::kReady) current.status = ModelStatus::kSelected;
        arr.push_back(current.to_json());
    }
    data.set("models", std::move(arr));
    data.set("selected_model_id", JsonValue::string(selected));
    data.set("running_model_id", JsonValue::string(running));
    data.set("state", JsonValue::string(selected.empty() ? "stopped" :
                                        (selected == running ? "running" : "switching")));
    data.set("failure_code", JsonValue::string(model_failure_code_));
    data.set("failure_message", JsonValue::string(model_failure_message_));
    data.set("available", JsonValue::boolean(true));
    return data;
}

bool Application::handle_model_validate(const std::string& model_id, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    return model_management_->registry().validate(model_id, error);
}

bool Application::handle_model_install(const std::string& model_id, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    return model_management_->registry().install(model_id, error);
}

bool Application::handle_model_activate(const std::string& model_id, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    // 热切换语义：activate 只改注册表选中态；真正"切过去跑"由 switch_active_model_runtime
    // 完成（stop → 以新模型重建 Worker 参数 → start → 首帧真实推理通过才提交 running）。
    // 旧模型路径：runtime 切换失败时回滚 active.json 并恢复旧模型运行，当前推理不中断窗口由
    // stop→start 间隙承担（~1s，与进程级重启相比无 IPC/端口中断）。
    const std::string previous_active = model_management_->registry().active_model();
    if (previous_active == model_id && core_runtime_ && core_runtime_->running() &&
        running_model_id_ == model_id) {
        std::string sync_error;
        if (!sync_model_id_to_profile(model_id, &sync_error)) {
            if (error) *error = "模型已运行，但配置同步失败: " + sync_error;
            return false;
        }
        return true;  // 幂等：运行态与配置均已确认一致
    }
    // ★ 无画面预检：见 has_capture_signal 注释。HDMI 没接/信号源没开时，下面的
    //   switch_active_model_runtime 必然在首帧门槛超时（5s），而回滚路径要走同一条门槛，
    //   实测整个请求 ~10.7s 才返回一个看不出真因的失败。这里提前 200ms 判定并直接拒绝；
    //   此刻**尚未改动 registry 与 runtime**，调用方无需回滚，状态零副作用。
    if (!has_capture_signal()) {
        if (error) {
            *error = "未检测到画面输入（HDMI 无信号或信号源未输出）：采集持续 0 帧，"
                     "无法验证新模型，本次切换未执行。请先点亮信号源再试。";
        }
        return false;
    }
    if (!model_management_->registry().activate(model_id, error)) {
        return false;  // 激活校验失败：active 未变，无需回滚
    }
    if (!core_runtime_ || !core_runtime_->running()) {
        // runtime 未在跑（如 HDMI 断流后）：同样立即用新模型拉起，
        // 不留"选中未生效"的中间态（用户视角=切换必须真实生效）
        TTBOX_LOG_INFO("模型已激活，runtime 未运行 → 直接以新模型拉起: " + model_id);
        if (!switch_active_model_runtime(model_id, error)) {
            std::string switch_error = error && !error->empty() ? *error : "拉起失败";
            // 拉起失败：回滚 active 到旧模型（主循环会自动重试恢复）
            std::string rb_error;
            if (!previous_active.empty() && previous_active != model_id) {
                model_management_->registry().activate(previous_active, &rb_error);
            } else if (previous_active.empty()) {
                model_management_->registry().deactivate(&rb_error);
            }
            if (error) *error = "模型切换失败（runtime 未运行，拉起新模型失败已回滚）: " + switch_error;
            return false;
        }
        TTBOX_LOG_INFO("模型热切换完成（runtime 原本未运行）: running_model_id=" + running_model_id_);
        std::string sync_error;
        if (!sync_model_id_to_profile(model_id, &sync_error)) {
            std::string rb_error;
            if (!previous_active.empty() && previous_active != model_id) {
                model_management_->registry().activate(previous_active, &rb_error);
                if (switch_active_model_runtime(previous_active, &rb_error)) {
                    sync_model_id_to_profile(previous_active, nullptr);
                }
            } else if (previous_active.empty()) {
                model_management_->registry().deactivate(&rb_error);
                core_runtime_->stop();
                runtime_started_ = false;
                running_model_id_.clear();
                want_runtime_running_.store(false);
            }
            if (error) *error = "新模型已启动但配置提交失败，已执行回滚: " + sync_error;
            return false;
        }
        return true;
    }
    if (!switch_active_model_runtime(model_id, error)) {
        // 切换失败：回滚 active 到旧模型，恢复旧模型运行
        std::string switch_error = error && !error->empty() ? *error : "切换失败";
        std::string rb_error;
        bool rb_capture_missing = false;
        if (!previous_active.empty()) {
            model_management_->registry().activate(previous_active, &rb_error);
        } else {
            model_management_->registry().deactivate(&rb_error);
        }
        if (!previous_active.empty() &&
            switch_active_model_runtime(previous_active, &rb_error, &rb_capture_missing)) {
            if (error) *error = "模型切换失败已回滚到 " + previous_active + ": " + switch_error;
        } else if (previous_active.empty()) {
            core_runtime_->stop();
            runtime_started_ = false;
            running_model_id_.clear();
            want_runtime_running_.store(false);
            if (error) *error = "模型切换失败，已恢复为未选择模型状态: " + switch_error;
        } else if (rb_capture_missing) {
            // ★ 旧模型也只是"没画面可验证"，而注册表与配置都已回到旧模型 ⇒ 这不是回滚失败。
            //   报"回滚失败"会让操作者以为设备状态坏了，实际只需点亮信号源。
            if (error) {
                *error = "模型切换失败（无画面输入，无法验证新模型）；已回滚到 " +
                         previous_active + "，接入信号源后会自动恢复运行。原因: " + switch_error;
            }
        } else {
            // 旧模型也起不来：让主循环 2s 自动重试拉起（want 仍为 true）
            if (error) {
                *error = "模型切换失败且回滚失败: " + switch_error;
            }
        }
        return false;
    }
    TTBOX_LOG_INFO("模型热切换完成: running_model_id=" + running_model_id_);
    std::string sync_error;
    if (!sync_model_id_to_profile(model_id, &sync_error)) {
        std::string rb_error;
        if (!previous_active.empty()) {
            model_management_->registry().activate(previous_active, &rb_error);
        } else {
            model_management_->registry().deactivate(&rb_error);
        }
        if (!previous_active.empty() && switch_active_model_runtime(previous_active, &rb_error)) {
            std::string restore_sync_error;
            if (sync_model_id_to_profile(previous_active, &restore_sync_error)) {
                if (error) *error = "新模型配置提交失败，已回滚到 " + previous_active + ": " + sync_error;
            } else if (error) {
                *error = "新模型配置提交失败，旧模型已恢复运行但配置回写失败: " + restore_sync_error;
            }
        } else if (previous_active.empty()) {
            core_runtime_->stop();
            runtime_started_ = false;
            running_model_id_.clear();
            want_runtime_running_.store(false);
            if (error) *error = "新模型配置提交失败，已恢复为未选择模型状态: " + sync_error;
        } else if (error) {
            *error = "新模型配置提交失败且回滚失败: " + sync_error + "; " + rb_error;
        }
        return false;
    }
    return true;
}

bool Application::sync_model_id_to_profile(const std::string& model_id, std::string* error) {
    RuntimeProfile updated;
    if (auto base = runtime_config_.snapshot()) {
        updated = *base;
    }
    updated.model_id = model_id;
    return persist_runtime_profile(updated, error);
}

bool Application::persist_runtime_profile(const RuntimeProfile& profile, std::string* error) {
    std::lock_guard<std::mutex> lock(config_persist_mutex_);
    if (!config_.loaded()) {
        if (error) *error = "配置文件未加载";
        return false;
    }

    // B1 修正：写回目标/内容都由 ConfigManager::persist 决定——
    //   目标 = config_.path()（分层模式为 <dir>/10-device.json 文件；此前误用
    //          config_path_，分层模式下是目录 → rename(file→dir) 必 EISDIR，
    //          且 /etc/ttbox 对 ttbox 用户 EACCES，SET_CONFIG/MODEL_ACTIVATE 落盘全断）
    //   内容 = 分层模式只写与基线的差异（R3），单文件模式全量（行为不变）
    JsonValue merged = config_.root();
    merged.set("runtime_profile", profile.to_json());
    std::string persist_error;
    if (!config_.persist(merged, &persist_error)) {
        if (error) *error = persist_error;
        return false;
    }
    config_.replace_root(std::move(merged));
    runtime_config_.update(profile);
    return true;
}

// T02 启动死锁修复配套：从"待配置"降级态重建 CoreRuntime。
// 调用方必须持有 runtime_lifecycle_mutex_。返回 true = 参数重建 + initialize 成功，
// 可继续 start；false = 仍缺配置/硬件（error 带原因，状态保持降级，下轮重试）。
bool Application::try_resume_from_degraded(std::string* error) {
    if (!runtime_waiting_config_) return true;
    // 丢弃未成功 initialize 的旧对象，按当前配置重建整链：
    // build_runtime_params 会重读 registry active（用户可能已通过 Web 完成选模型）。
    core_runtime_ = std::make_unique<CoreRuntime>();
    CoreRuntime::Params rt_params{};
    std::string rt_error;
    // ★ M2.03：用**当前卡态** gates 重建——受限卡（features=[capture]）在未选模型时也能起。
    if (!build_runtime_params(rt_params, current_feature_gates(), &rt_error)) {
        if (error) *error = rt_error;
        return false;
    }
    if (!core_runtime_->initialize(rt_params, &rt_error)) {
        if (error) *error = rt_error;
        return false;
    }
    // ★ M2.03：重建后同步 gate + 预览降级（initialize 已按同一 gates 构建，这里保证 gates_ 成员一致）。
    core_runtime_->set_feature_gates(current_feature_gates(), brand_upper(),
                                     static_cast<int>(config_.get_int("preview_fps", cfg::kPreviewFpsDefault)));
    runtime_waiting_config_ = false;
    degraded_reason_.clear();
    TTBOX_LOG_INFO("待配置状态已恢复：CoreRuntime 参数重建成功");
    return true;
}

// 无画面预检：采集在跑却在 probe_ms 内一帧都收不到 ⇒ 一定没有画面输入。
// 背景：模型切换末尾有"首帧门槛"（等 model_ready，最多 5s），而 model_ready 要求至少
//   成功推理过一次（CoreRuntime::model_ready = inference_ok>0 && decode_ok>0）⇒ 必须有帧。
//   HDMI 没信号时这道门槛必然超时，且**回滚路径会再走一次同一门槛**，实测整个
//   /api/models/select 请求耗时 ~10.7s（新模型 5s + 回滚旧模型 5s）才返回，
//   用户拿到的是"切换失败且回滚失败"这种看不出真正原因的错误。
// 这里用短采样把"没画面"从"模型有问题"里分出来，让调用方能提前拒绝、给可操作文案。
// 采集未启动（capture 为空或未 running）时没有判据 ⇒ 返回 true，交给首帧门槛兜底。
bool Application::has_capture_signal(int probe_ms) const {
    if (!core_runtime_ || !core_runtime_->capture() || !core_runtime_->capture()->running()) {
        return true;
    }
    const uint64_t baseline = core_runtime_->capture()->metrics().capture_frames.load();
    const int steps = probe_ms < 50 ? 1 : (probe_ms / 50);
    for (int i = 0; i < steps; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (core_runtime_->capture()->metrics().capture_frames.load() != baseline) {
            return true;  // 有新帧 ⇒ 画面在
        }
    }
    return false;
}

bool Application::switch_active_model_runtime(const std::string& new_model_id,
                                              std::string* error,
                                              bool* capture_missing) {
    if (capture_missing) *capture_missing = false;
    if (!core_runtime_ || !model_management_) {
        if (error) *error = "core_runtime 或模型仓库未初始化";
        return false;
    }
    // T1.10① / M2.03：会话边界执法——模型切换属**推理子系统**，以 feature 'inference' 为闸门
    //   （非 pipeline_allowed：切模型语义 = 推理，capture/aim 是否在跑不影响"能否换模型"）。
    if (!auth::LicenseGate::instance().feature_enabled(auth::feature_name::kInference)) {
        if (error) *error = "授权未通过：feature 'inference' 未启用，不切换模型";
        return false;
    }
    // 1) 准备切换模型：热切换保留 capture 在跑，停机时才需全量停止
    want_runtime_running_.store(false);  // 防止主循环 2s 重试在我们重建期间抢跑
    const bool was_running = core_runtime_->running();
    if (!was_running) {
        core_runtime_->stop();
        runtime_started_ = false;
    }
    running_model_id_.clear();

    // 2) 以新 active 模型重建 Worker 参数（build_runtime_params 读 registry active）
    CoreRuntime::Params rt_params{};
    std::string rt_error;
    if (!build_runtime_params(rt_params, current_feature_gates(), &rt_error)) {
        if (error) *error = "新模型参数构建失败: " + rt_error;
        want_runtime_running_.store(true);
        return false;
    }
    if (was_running) {
        // 3) 热切换：同一路 V4L2 采集保持在跑，只重建 RKNN worker 池
        if (!core_runtime_->reload_workers(rt_params.workers, &rt_error)) {
            if (error) *error = "新模型 worker 热切换失败: " + rt_error;
            want_runtime_running_.store(true);
            return false;
        }
    } else {
        // 3) 重初始化 CoreRuntime（worker_params_ 仅在 initialize 时拷入，必须重走）
        if (!core_runtime_->initialize(rt_params, &rt_error)) {
            if (error) *error = "CoreRuntime 重初始化失败: " + rt_error;
            want_runtime_running_.store(true);
            return false;
        }
        // 4) 启动新模型
        if (!core_runtime_->start(&rt_error)) {
            if (error) *error = "新模型启动失败: " + rt_error;
            want_runtime_running_.store(true);
            return false;
        }
        runtime_started_ = true;
    }
    // ★ M2.03：切换后同步 gate + 预览降级（与当前卡态一致；受限态保持封顶帧率与水印）。
    core_runtime_->set_feature_gates(current_feature_gates(), brand_upper(),
                                     static_cast<int>(config_.get_int("preview_fps", cfg::kPreviewFpsDefault)));
    // T02 死锁修复：runtime 已用有效模型完成初始化，清除"待配置"降级标志。
    runtime_waiting_config_ = false;
    degraded_reason_.clear();
    want_runtime_running_.store(true);

    // 5) 首帧门槛：等待真实推理+Decode 成功（最多 5s），通过才提交 running_model_id
    constexpr int kWaitFramesMs = 5000;
    const uint64_t gate_frames_begin = core_runtime_->capture()
        ? core_runtime_->capture()->metrics().capture_frames.load() : 0;
    for (int waited = 0; waited < kWaitFramesMs; waited += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (core_runtime_->model_ready()) {
            running_model_id_ = new_model_id;
            model_failure_code_.clear();
            model_failure_message_.clear();
            return true;
        }
        if (!core_runtime_->running()) {
            if (error) *error = "新模型流水线中途退出";
            return false;
        }
    }
    // 超时：把"没有画面"与"模型本身有问题"分开——两者对操作者的处置完全不同，
    //   混成一句话会让用户以为模型库坏了（实际只需点亮信号源）。
    const uint64_t gate_frames_end = core_runtime_->capture()
        ? core_runtime_->capture()->metrics().capture_frames.load() : 0;
    const bool no_frame_input = (gate_frames_end == gate_frames_begin);
    if (capture_missing) *capture_missing = no_frame_input;
    if (error) {
        *error = no_frame_input
                     ? "未检测到画面输入（HDMI 无信号或信号源未输出）："
                       "5s 内采集 0 帧，无法验证新模型是否可用"
                     : "新模型 5s 内未完成首次真实推理（首帧门槛未通过）";
    }
    return false;
}

bool Application::handle_model_remove(const std::string& model_id, std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    return model_management_->registry().remove(model_id, error);
}

bool Application::handle_model_set_concurrency(const std::string& model_id, int count,
                                               std::string* error) {
    std::lock_guard<std::mutex> lifecycle_lock(runtime_lifecycle_mutex_);
    if (!model_management_) {
        if (error) *error = "模型仓库不可用";
        return false;
    }
    if (!model_management_->registry().set_concurrency(model_id, count, error)) {
        return false;
    }
    // 当前运行的是该模型时，立即重建 worker 池使并发生效；否则下次启动该模型时生效。
    if (core_runtime_ && core_runtime_->running() && running_model_id_ == model_id) {
        std::string switch_error;
        bool rebuild_capture_missing = false;
        if (!switch_active_model_runtime(model_id, &switch_error, &rebuild_capture_missing)) {
            if (rebuild_capture_missing) {
                // 并发数已落盘；只是当前没画面、无法验证重建后的首帧 ⇒ 有信号后主循环会自动拉起。
                if (error) {
                    *error = "并发数已保存；当前无画面输入，运行时重建将在信号恢复后自动生效。原因: " +
                             switch_error;
                }
                return false;
            }
            if (error) *error = "并发已保存，但运行时重建失败: " + switch_error;
            return false;
        }
    }
    return true;
}

}  // namespace ttbox::core
