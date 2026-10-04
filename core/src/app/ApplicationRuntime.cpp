// ApplicationRuntime.cpp — 运行时参数/模型热切换/启停意愿（14 个）
//
// ★ 由 Application.cpp 按职责拆分而来（2026-10-04 core 结构治理 S1）。
//   拆分方式 = **只搬定义，不改任何逻辑**：成员函数仍是 Application 的同一个类的成员，
//   只是定义落在别的编译单元（类外定义可跨 TU 分散，这是 C++ 标准允许的）。
//   ⇒ 外部行为、符号名、ABI 全部不变；唯一变化是 .o 的组织方式。
//
// 本文件负责：运行时参数/模型热切换/启停意愿（14 个）
// ★ 这一组是「装配零件」：功能门读取、预览降级、RuntimeProfile 持久化、
//   模型热切换回滚、启停意愿落盘。调用方是主 TU 的生命周期与 IPC 组。
#include "app/Application.hpp"
#include "app/ApplicationInternal.hpp"   // now_ms / parse_* / kPostUpdateSmokeMaxMs（inline 共享）
#include "common/Json.hpp"               // json_parse_file
#include "common/Logger.hpp"
#include "common/ConfigDefaults.hpp"     // cfg:: 出厂默认值镜像
#include "auth/LicenseGate.hpp"          // T1.10：会话边界读一次 LicenseGate 快照
#include "app/RuntimeIntent.hpp"         // R5/R6：StartupIntent（启停意愿文件读写）
#include "common/Paths.hpp"              // A-PATH-5：运行期路径字面量单点真源
#include "ttbox/core/version.hpp"        // kCoreVersion
#include "output/FifoHidOutput.hpp"      // 无 USB 时的 FIFO 回退后端
#include "output/AiboxHidOutput.hpp"     // 标准 HID 后端
#include "output/OutputBackend.hpp"
#include <fstream>

using ttbox::core::app_internal::now_ms;

namespace ttbox::core {
using ttbox::core::app_internal::parse_color_order;
using ttbox::core::app_internal::parse_worker_cores;
using ttbox::core::app_internal::kPostUpdateSmokeMaxMs;

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
// ★ M2.03：卡内 ui_brand 的大写形（水印文本用）。只读 Gate 快照的投影字段，不推导授权。
std::string Application::brand_upper() const {
    std::string brand = auth::LicenseGate::instance().snapshot().ui_brand;
    if (brand.empty()) brand = auth::default_ui_brand();
    for (char& c : brand) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return brand;
}
bool Application::build_runtime_params(CoreRuntime::Params& out_params,
                                       const CoreRuntime::FeatureGates& gates,
                                       std::string* error) {
    out_params.gates = gates;  // ★ M2.03：特性级启停（start() 据此逐模块启停）
    // ★ 2026-10-03 补注（防误改）：这里的 /dev/video0 **不是"忘了改成运行时解析"的
    //   残留**，与 V1.0.18 修掉的风扇 hwmon 编号 / UDC 名性质不同 ——
    //   ① 它确实有配置项capture_device 可覆盖（默认值就在这里）；
    //   ② 但 RK3588 的 HDMI-RX 输入**物理上只能绑这个 V4L2 节点**，
    //      DRM card/render 节点只服务 loopout 与 NPU，填错会让整条采集链路失效。
    //   所以下面那句「非 /dev/video0 直接报错」是**硬件约束的 fail-closed**，
    //   不要照 hwmon/UDC 那套改成"扫描 /sys 猜节点"。
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
// ★ M2.03：会话边界读一次 LicenseGate 快照 → 三项特性 gate（唯一读点，不每帧查）。
CoreRuntime::FeatureGates Application::current_feature_gates() const {
    const auth::LicenseSnapshot snap = auth::LicenseGate::instance().snapshot();
    CoreRuntime::FeatureGates g;
    g.capture   = snap.feature_enabled(auth::feature_name::kCapture);
    g.inference = snap.feature_enabled(auth::feature_name::kInference);
    g.aim       = snap.feature_enabled(auth::feature_name::kAim);
    return g;
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
bool Application::sync_model_id_to_profile(const std::string& model_id, std::string* error) {
    RuntimeProfile updated;
    if (auto base = runtime_config_.snapshot()) {
        updated = *base;
    }
    updated.model_id = model_id;
    return persist_runtime_profile(updated, error);
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

}  // namespace ttbox::core
