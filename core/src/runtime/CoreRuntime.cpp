// CoreRuntime.cpp — Capture/Worker/AimThread 统一生命周期。
#include "runtime/CoreRuntime.hpp"
#include "common/FrameRateMeter.hpp"
#include "common/Logger.hpp"
#include "output/AiboxHidOutput.hpp"

#include <algorithm>
#include <chrono>

namespace ttbox::core {

namespace {
// 单调时钟毫秒（算推理 FPS 分母用，避免墙钟跳变）。
int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

// 装配阶段：校验参数、保存 gate/配置，构造 mailbox/capture/workers 并配置采集（不起线程）。
bool CoreRuntime::initialize(const Params& p, std::string* error) {
    if (!p.output) {
        if (error) *error = "输出后端不能为空";
        return false;
    }
    if (p.workers.worker_cores.empty() ||
        p.workers.worker_cores.size() > aim::AimTargetMailbox::kMaxWorkers) {
        if (error) *error = "Worker 数量必须为 1~3";
        return false;
    }
    runtime_config_ = p.runtime_config;
    mouse_event_socket_ = p.mouse_event_socket;
    output_ = p.output;
    worker_params_ = p.workers;
    preview_params_ = p.preview;
    // ★ M2.03：记录本会话的特性 gate（会话边界会由 set_feature_gates() 再收窄/刷新）。
    gates_ = p.gates;
    // 预览降级可逆恢复所需的「原始配置帧率」：构建期 gates 默认全 true ⇒ p.preview.fps 即配置值。
    if (p.preview.fps > 0) configured_preview_fps_ = p.preview.fps;
    pipeline_debug_enabled_ = p.pipeline_debug_enabled;
    pipeline_debug_interval_ = p.pipeline_debug_interval;
    pid_trace_enabled_ = p.pid_trace_enabled;
    pid_trace_path_ = p.pid_trace_path;
    det_trace_enabled_ = p.det_trace_enabled;
    det_trace_path_ = p.det_trace_path;
    prediction_time_s_ = p.prediction_time_s;
    mailbox_ = std::make_unique<aim::AimTargetMailbox>(p.workers.worker_cores.size());
    capture_ = std::make_unique<V4L2Capture>();
    workers_ = std::make_unique<WorkerPool>();
    if (!capture_->configure(p.capture, error)) return false;
    return true;
}

// worker 参数的唯一绑定点（声明处有完整背景）。
// 凡是"要交给 WorkerPool 的 Params"都必须先经过这里，否则裸指针字段会被外部
// 构造的 Params 静默覆盖成 nullptr —— 1.5.37 的瞬时帧率就是这么被 reload 路径绕过的。
void CoreRuntime::bind_worker_params(WorkerPool::Params& p) {
    p.latest = capture_ ? capture_->latest_frame_ref() : nullptr;
    p.aim_mailbox = mailbox_.get();
    p.runtime_config = runtime_config_;
    p.fps_meter = &fps_meter_;
    // ★ 原图尺寸也必须在唯一绑定点里回填，不能指望调用方带上。
    //   外部构造的 Params（Application::build_runtime_params）从头到尾没设过
    //   frame_w/frame_h，而 reload_workers 是 `next = params` 整体覆盖 ⇒ 热切换模型后
    //   这两个值恒为 0 ⇒ DecodeNMS 走「0=不映射」分支 ⇒ 检测框不再映射回原图坐标。
    //   （CoreRuntime::start() 原本单独赋过值，只有热切换这条路漏了。）
    //   采集未启用时 format 为 0x0，保持 0，语义与原来一致（不映射）。
    if (capture_) {
        const auto& fmt = capture_->format();
        p.frame_w = fmt.width;
        p.frame_h = fmt.height;
    }
}

// 预加载：绑定 worker 参数后让 WorkerPool 完成模型加载+预热，但不拉起轮询线程（start() 再补）。
bool CoreRuntime::preload_workers(std::string* error) {
    if (!workers_) {
        if (error) *error = "WorkerPool 未初始化";
        return false;
    }
    if (workers_preloaded_) return true;
    if (worker_params_.worker_cores.empty()) {
        if (error) *error = "worker_cores 为空（worker 数量必须 ≥1）";
        return false;
    }
    worker_params_.latest = capture_ ? capture_->latest_frame_ref() : nullptr;
    worker_params_.aim_mailbox = mailbox_.get();
    worker_params_.fps_meter = &fps_meter_;
    worker_params_.runtime_config = runtime_config_;
    bind_worker_params(worker_params_);
    if (!workers_->preload(worker_params_, error)) {
        return false;
    }
    workers_preloaded_ = true;
    TTBOX_LOG_INFO("推理预加载完成：模型已加载并预热，等待采集启动后拉起轮询线程");
    return true;
}

// 按 gates 逐模块启动整条流水线；失败时条件回滚——只停本路径真正启动过的模块。
bool CoreRuntime::start(std::string* error) {
    if (!capture_ || !workers_ || !mailbox_) {
        if (error) *error = "内部对象未初始化";
        return false;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        if (error) *error = "已在运行中";
        return false;
    }
    // 重启（停止→启动）时清空 mailbox 残留任务：V4L2 sequence 重新从 0 计数，
    // 不清空会导致 AimThread last_frame 被旧任务抬高，新帧全被 take_latest 去重丢弃
    // （重启后 1~3 分钟检测框不更新，直到帧号重新涨回旧值）。
    mailbox_->clear();
    start_steady_ms_.store(steady_now_ms());
    fps_meter_.reset();  // 新会话：清掉上一轮的帧时间戳，避免首帧算出虚高/虚低

    // ★ M2.03：失败回滚必须**条件化** —— 只回滚本路径真正启动过的模块，
    //   否则 gates 关闭的模块被"回滚"会误伤（如 workers_ 从未 start 却调用 stop）。
    //   （各子模块 stop() 均为幂等，此处条件化的意义在于语义清晰与日志正确。）
    const bool want_capture = gates_.capture;
    const bool want_inference = gates_.inference;
    const bool want_aim = gates_.aim;

    const auto rollback = [&](std::string* /*err*/) {
        // 逆序条件回滚：aim → mouse → workers → capture。
        if (want_aim) aim_thread_.stop();
        mouse_reader_.stop();
        if (want_inference) workers_->stop();
        if (want_capture) {
            capture_->stop();
            capture_->close();
        }
        start_steady_ms_.store(0);
        running_ = false;
    };

    // ① Capture（仅 gates.capture；无采集 = 无帧源，整链无从谈起）。
    if (want_capture) {
        if (!capture_->open(error)) {
            start_steady_ms_.store(0);
            running_ = false;
            return false;
        }
        if (!capture_->start(error)) {
            // open 已成功，start 失败也必须关闭设备；否则 running=false 后 stop()
            // 会直接返回，半启动的 V4L2 fd 永久泄漏，后续自动重试持续报占用。
            capture_->close();
            start_steady_ms_.store(0);
            running_ = false;
            return false;
        }
    } else {
        TTBOX_LOG_INFO("feature 'capture' 未启用：跳过 V4L2 采集（受限预览/透传模式）");
    }

    // ② WorkerPool（仅 gates.inference）。
    if (want_inference) {
        const auto& fmt = capture_->format();
        // 原图尺寸由 bind_worker_params() 统一回填（见该函数注释），这里不再单独赋值，
        // 免得「两处都能设」将来又漏掉一处。fmt 仍要取：下面预加载分支要用。
        bind_worker_params(worker_params_);
        bool workers_ok = false;
        if (workers_preloaded_) {
            // 预加载发生在采集之前，那时还不知道真实帧尺寸；这里补刷新再拉起线程。
            workers_->set_frame_size(fmt.width, fmt.height);
            workers_ok = workers_->start_loops(error);
        } else {
            workers_ok = workers_->start(worker_params_, error);
        }
        if (!workers_ok) {
            rollback(error);
            return false;
        }
    } else {
        TTBOX_LOG_INFO("feature 'inference' 未启用：跳过 RKNN WorkerPool（无推理）");
    }

    // ③ 鼠标透传通道（红线段：与 AI 授权无关，恒启动；失败不阻塞流水线）。
    std::string mouse_error;
    mouse_reader_.set_event_socket_path(mouse_event_socket_);
    if (!mouse_reader_.start("", &mouse_error)) {
        TTBOX_LOG_WARN("PhysicalMouseReader 启动失败（不阻塞 AI 流水线）: " + mouse_error);
    }
    if (auto* backend = dynamic_cast<output::OutputBackend*>(output_.get())) {
        backend->set_button_source(mouse_reader_.button_source());
        backend->set_config_source(runtime_config_);
    } else if (auto* aibox = dynamic_cast<output::AiboxHidOutput*>(output_.get())) {
        // ★ 2026-09-25 补：兜底后端也要绑按键源。AiboxHidOutput **不是** OutputBackend，
        //   所以上面那个 dynamic_cast 永远失败 ⇒ 它的 button_source_ 一直是 nullptr
        //   ⇒ 输出层的热键二次防线等于没有，只剩 AimThread 一层。
        //   （AiboxHidOutput::send 已改成"没绑按键源就拒绝注入"，这里必须跟上，
        //    否则改成 fail-closed 之后兜底路径会一个 count 都发不出去。）
        aibox->set_button_source(mouse_reader_.button_source());
        aibox->set_config_source(runtime_config_);
    }

    // ④ AimThread（仅 gates.aim）。
    if (want_aim) {
        // ★ 配置必须在线程拉起**之前**写入（2026-10-06 修正）。
        //   start() 之后 loop 线程会无锁读 pipeline_debug_ / pid_trace_ /
        //   det_trace_ / prediction_time_s_，此时再调 setter 即构成数据竞争（UB）。
        //   顺序保证：先写完配置，再构造 std::thread —— 线程构造提供 happens-before，
        //   loop 线程必然看得到上面这几次写入。不改任何配置值，行为完全等价。
        aim_thread_.set_pipeline_debug(pipeline_debug_enabled_, pipeline_debug_interval_);
        if (pid_trace_enabled_) aim_thread_.set_pid_trace(true, pid_trace_path_);
        if (det_trace_enabled_) aim_thread_.set_det_trace(true, det_trace_path_);
        aim_thread_.set_prediction_time(prediction_time_s_);

        if (!aim_thread_.start(mailbox_.get(), output_, 4000, runtime_config_,
                               mouse_reader_.button_source())) {
            rollback(error);
            return false;
        }
    } else {
        TTBOX_LOG_INFO("feature 'aim' 未启用：跳过 AimThread（无瞄准/输出）");
    }

    // ⑤ Preview（受限预览：帧率封顶 + 水印由 Application 计算并写入 preview_params_；
    //   本层只执行参数，不查授权）。
    {
        std::string preview_error;
        preview_ = std::make_unique<PreviewModule>();
        PreviewModule::Params preview_params = preview_params_;
        preview_params.runtime_config = runtime_config_;
        if (runtime_config_) {
            if (auto profile = runtime_config_->snapshot()) {
                // 全功能态才允许用户配置覆盖帧率；受限态保持 Application 计算出的封顶值。
                if (feature_gates_full(gates_) && profile->preview.fps > 0 &&
                    profile->preview.fps <= 60) {
                    preview_params.fps = static_cast<int>(profile->preview.fps);
                }
                // 必须在 PreviewModule::start() 之前应用尺寸；此前只修改了
                // 启动后的日志变量，实际编码仍沿用默认 640x640。
                if (profile->preview.width > 0) preview_params.crop_width = profile->preview.width;
                if (profile->preview.height > 0) preview_params.crop_height = profile->preview.height;
            }
        }
        if (!preview_->start(capture_->latest_frame_ref(), preview_params, &preview_error)) {
            TTBOX_LOG_WARN("Preview 启动失败（不影响流水线）: " + preview_error);
            preview_.reset();
        } else {
            if (preview_params.draw_detections) {
                // ★★ V1.0.25（业主 2026-10-04 定口径）：预览画的必须是**实际瞄准的框**。
                //   旧实现喂 `status().detection_boxes`（本帧**全部**检测框），于是画面上
                //   同时出现红框(class 0)与绿框(class 1 加粗)，两个框叠在一起看着像
                //   "又大又含头" —— 那是候选框集合，不是瞄准框，预览在骗人。
                //   现在只喂 `target_*`（AimThread 那一帧真正在控制链上用的框，
                //   V1.0.24 起 = 上半身虚拟框、无并集），**所见即所控**。
                //   没有选中目标 ⇒ 返回 false ⇒ 画面不画框（诚实：此刻确实没在瞄）。
                //   `status()` 自带锁，返回值拷贝；只取 5 个 float + 1 个 int，
                //   比旧路径（拷贝整个 detection_boxes vector）更省。
                preview_->set_aim_box_provider([this](DetectionBox* out) -> bool {
                    if (out == nullptr) return false;
                    const auto st = aim_thread_.status();
                    if (!st.has_target) return false;
                    if (!(st.target_x2 > st.target_x1) || !(st.target_y2 > st.target_y1)) {
                        return false;
                    }
                    out->x1 = st.target_x1;
                    out->y1 = st.target_y1;
                    out->x2 = st.target_x2;
                    out->y2 = st.target_y2;
                    out->class_id = st.target_class_id;
                    out->score = st.target_score;  // 选中目标的模型置信度（0~1），预览角标显示
                    return true;
                });
                // ★★ V1.0.31 预览三层（对照 GitHub sunone_aimbot 2026-10-04 调研）：
                //   ① 全部检测框（细）—— 看得见 AI 此刻检出了哪些候选、各在哪；
                //   ③ 中心→落点连线 —— 看得见准星往哪走。
                //   ★ V1.0.37：FOV 蓝圆已移到前端（web 覆盖层实时画，与「瞄准范围」滑块联动，
                //     直径 = 截取尺寸 × 倍率，与 core 选靶半径 search_radius_px × fov_range 同源），
                //     core 视频流不再烧圆 —— 否则两圆不同步、重叠。
                preview_->set_detections_provider([this]() {
                    return aim_thread_.status().detection_boxes;
                });
                preview_->set_guides_provider([this]() {
                    PreviewModule::AimGuides g;
                    const auto st = aim_thread_.status();
                    g.has_aim_point = st.has_target;
                    // 落点：用 control_trace 的 target_point（控制链真正在追的那个点）。
                    // 退化时用 target_* 框内比例兜底。
                    g.aim_x = (st.has_target && st.target_x2 > st.target_x1)
                        ? (st.target_x1 + st.target_x2) * 0.5f : 0.0f;
                    g.aim_y = (st.has_target && st.target_y2 > st.target_y1)
                        ? st.target_y1 + (st.target_y2 - st.target_y1) * 0.24f : 0.0f;
                    return g;
                });
            }
            TTBOX_LOG_INFO("Preview 已启动: center crop " +
                           std::to_string(preview_params.crop_width) + "x" +
                           std::to_string(preview_params.crop_height) +
                           " @" + std::to_string(preview_params.fps) + "fps" +
                           (preview_params.watermark ? " +watermark" : "") +
                           (preview_params.draw_detections ? " +draw_detections" : ""));
        }
    }
    return true;
}

// 模型热切换：仅重建 RKNN worker 池（采集/预览/瞄准保持运行）；失败则回滚旧 worker。
bool CoreRuntime::reload_workers(const WorkerPool::Params& params, std::string* error) {
    if (!running_.load()) {
        if (error) *error = "runtime 未运行，无法热重载 worker";
        return false;
    }
    if (!capture_ || !workers_ || !mailbox_) {
        if (error) *error = "内部对象未初始化";
        return false;
    }
    // ★ 原图尺寸不在入参里（build_runtime_params 从不设 frame_w/h），由下面的
    //   bind_worker_params() 从 capture_ 现取后回填。此前这里取了 fmt 却没用，
    //   热切换后 frame_w/h 恒为 0（详见 bind_worker_params 注释）。
    const WorkerPool::Params previous = worker_params_;
    WorkerPool::Params next = params;
    bind_worker_params(next);
    workers_->stop();
    mailbox_->clear();
    worker_params_ = next;
    if (workers_->start(worker_params_, error)) {
        return true;
    }

    WorkerPool::Params restore = previous;
    bind_worker_params(restore);
    worker_params_ = restore;
    mailbox_->clear();
    std::string restore_error;
    if (!workers_->start(restore, &restore_error)) {
        if (error && !error->empty()) {
            *error = "worker 热重载失败且旧 worker 恢复失败: " + *error + " / " + restore_error;
        } else if (error) {
            *error = "worker 热重载失败且旧 worker 恢复失败: " + restore_error;
        }
        stop();
        return false;
    }
    if (error) *error = "worker 热重载失败，已恢复旧 worker";
    return false;
}

// 会话边界刷新特性 gate 与预览降级参数（授权变化后下次 start() 生效）。
void CoreRuntime::set_feature_gates(const FeatureGates& gates,
                                    const std::string& brand_upper,
                                    int configured_fps) {
    gates_ = gates;
    if (!brand_upper.empty()) brand_upper_ = brand_upper;
    if (configured_fps > 0) configured_preview_fps_ = configured_fps;
    // 预览降级（唯一计算在 Application；本方法只按已下传的 gates 做参数投影，不查授权）。
    const bool full = feature_gates_full(gates_);
    preview_params_.watermark = !full;
    preview_params_.fps = full
                              ? configured_preview_fps_
                              : std::max(1, std::min(configured_preview_fps_,
                                                     kRestrictedPreviewFps));
    preview_params_.watermark_text = full ? std::string() : (brand_upper_ + " - LIMITED");
}

// 是否已就绪：任一 worker 至少成功推理一帧并进入 Decode（首帧门槛判据）。
bool CoreRuntime::model_ready() const {
    if (!running_.load() || !workers_ || workers_->worker_count() == 0) return false;
    for (const auto& worker : workers_->workers()) {
        if (!worker) continue;
        const auto& stats = worker->stats();
        if (stats.inference_ok.load() > 0 && stats.decode_ok.load() > 0) return true;
    }
    return false;
}

// 推理出错累计计数（无 worker 时为 0）。
uint64_t CoreRuntime::model_errors() const {
    if (!workers_) return 0;
    return workers_->total_errors();
}

// 停机：清 running 与启动时间，逆序停 preview/aim/mouse/workers/capture（各 stop 幂等）。
void CoreRuntime::stop() {
    // stop 必须对“启动中途失败”和“已停止”同样生效：即使 running_ 已经
    // 被 start() 的失败路径清零，也要再次清理可能残留的线程、V4L2 fd、
    // preview 和 mailbox 资源。各子模块的 stop/close 都是幂等的。
    running_.store(false);
    start_steady_ms_.store(0);
    if (preview_) {
        preview_->stop();
        preview_.reset();
    }
    aim_thread_.stop();
    mouse_reader_.stop();
    if (workers_) workers_->stop();
    workers_preloaded_ = false;
    if (capture_) {
        capture_->stop();
        capture_->close();
    }
}

// 聚合采集/推理/预览/瞄准/输出指标到 PipelineMetrics（未运行时保持 0=unavailable）。
void CoreRuntime::collect_metrics(PipelineMetrics* out) const {
    if (out == nullptr || !running_.load()) return;
    // ★ M2.03：预览水印投影（受限态 true）。仅在运行中置位，未运行无预览 ⇒ 保持 false。
    out->preview_watermark = preview_params_.watermark;
    if (capture_) {
        const auto& cm = capture_->metrics();
        out->frames_total = cm.capture_frames.load();
        out->frames_superseded = cm.superseded_latest_frames.load();
        out->capture_fps = cm.capture_fps.load();
        out->last_dequeued_count = capture_->in_use_count();
        out->buffer_count = capture_->buffer_count();
        const auto& format = capture_->format();
        out->input_width = format.width;
        out->input_height = format.height;
    }
    if (workers_ && workers_->worker_count() > 0) {
        uint64_t published = 0;
        double infer_avg_us = 0.0;
        double si_avg_us = 0.0, run_avg_us = 0.0, out_avg_us = 0.0;
        double decode_avg_us = 0.0, e2e_avg_us = 0.0;
        double rga_avg_us = 0.0, qwait_avg_us = 0.0;
        const size_t worker_count = workers_->worker_count();
        // ★ 合并容器按 worker 数放大窗口（2026-09-23 全仓审查复核 #11）：默认 4096 窗口下，
        //   3 路各 4096 样本吸进来时后吸的会把先吸的整段挤掉 ⇒ p95/p99「名义全局、实际单路」。
        //   显式给到 kMaxSamples × worker_count，各路样本全部保留。
        StatsCollector e2e_all(StatsCollector::kMaxSamples * worker_count);
        StatsCollector infer_all(StatsCollector::kMaxSamples * worker_count);
        StatsCollector decode_all(StatsCollector::kMaxSamples * worker_count);
        for (const auto& worker : workers_->workers()) {
            if (!worker) continue;
            const auto& stats = worker->stats();
            published += stats.published.load();
            infer_avg_us += stats.stages.total.avg();
            si_avg_us += stats.stages.set_input.avg();
            run_avg_us += stats.stages.run.avg();
            out_avg_us += stats.stages.output.avg();
            decode_avg_us += stats.decode_stages.total.avg();
            e2e_avg_us += stats.e2e.avg();
            rga_avg_us += stats.rga.avg();
            qwait_avg_us += stats.queue_wait.avg();
            e2e_all.absorb(stats.e2e);
            infer_all.absorb(stats.stages.total);
            decode_all.absorb(stats.decode_stages.total);
        }
        out->infer_total = published;
        // 认领跳帧累计（纯观测：不参与任何控制/分配决策）。
        // WorkerPool::total_skipped() 内部已按 worker 求和并判空，这里只做一次搬运；
        // 代价 = N 次无锁 atomic 读（N=3），相对本函数已有的 3 路 StatsCollector
        // 合并与模型输入通路快照可忽略。字段语义与"非丢帧率"的告警见 Metrics.hpp 注释。
        out->worker_skipped = workers_->total_skipped();
        // 真丢帧/断流信号（等满 7ms 没等到新 seq）：与上面的轮转 skip 严格分开。
        // 稳态应≈0；持续增长说明采集断流/通知丢失/worker 调度饿死。
        out->no_frame_timeout = workers_->total_no_frame_timeout();
        // ★ fps 一律取**滚动窗口瞬时值**（FrameRateMeter），不再用
        //   published ÷ 启动至今秒数 —— 后者是累计平均，分母里永久含着启动期
        //   一次性开销（3 worker 加载 + 起流 + 预热），表现为"帧率从 140 出头
        //   慢慢爬升"（2026-09-23 板端现象）。瞬时窗口在第 2 帧即可给出真实值。
        //   窗口样本不足 2 帧时（刚启动）才回退累计平均，避免显示 0。
        out->fps = fps_meter_.fps();
        if (out->fps <= 0.0) {
            out->fps = published;
            const int64_t started = start_steady_ms_.load();
            if (started > 0) {
                const double elapsed_s =
                    static_cast<double>(steady_now_ms() - started) / 1000.0;
                if (elapsed_s > 0.0) out->fps = static_cast<double>(published) / elapsed_s;
                // ★ 自曝式诊断：跑了 2 秒以上还没有瞬时样本，说明 worker 根本没 tick
                //   ⇒ fps_meter 指针没绑上（reload 路径曾整份覆盖 Params 把它清成 nullptr）。
                //   累计平均只是"看起来在爬坡"的兜底，不该长期生效——必须留下痕迹。
                if (elapsed_s > 2.0) {
                    TTBOX_LOG_WARN("fps 瞬时窗口无样本（" +
                                   std::to_string(fps_meter_.sample_count()) +
                                   " 帧），已回退累计平均；检查 worker 参数是否漏绑 fps_meter");
                }
            }
        }
        out->infer_ms = infer_avg_us / static_cast<double>(worker_count) / 1000.0;
        out->infer_set_input_ms = si_avg_us / static_cast<double>(worker_count) / 1000.0;
        out->infer_run_ms = run_avg_us / static_cast<double>(worker_count) / 1000.0;
        out->infer_output_ms = out_avg_us / static_cast<double>(worker_count) / 1000.0;
        out->decode_ms = decode_avg_us / static_cast<double>(worker_count) / 1000.0;
        out->e2e_ms = e2e_avg_us / static_cast<double>(worker_count) / 1000.0;
        // resize_ms 的语义是完整 RGA 预处理耗时（含输入 DMA-BUF import、crop、resize、release），
        // 不能使用从未在生产路径吸收的 convert 统计，否则 Web 会稳定显示 0。
        out->resize_ms = rga_avg_us / static_cast<double>(worker_count) / 1000.0;
        out->buffer_age_ms = qwait_avg_us / static_cast<double>(worker_count) / 1000.0;
        out->e2e_p50_ms = e2e_all.percentile(50) / 1000.0;
        out->e2e_p95_ms = e2e_all.percentile(95) / 1000.0;
        out->e2e_p99_ms = e2e_all.percentile(99) / 1000.0;
        out->e2e_max_ms = e2e_all.max() / 1000.0;
        out->infer_p50_ms = infer_all.percentile(50) / 1000.0;
        out->infer_p95_ms = infer_all.percentile(95) / 1000.0;
        out->infer_p99_ms = infer_all.percentile(99) / 1000.0;
        out->decode_p50_ms = decode_all.percentile(50) / 1000.0;
        out->decode_p95_ms = decode_all.percentile(95) / 1000.0;
        out->decode_p99_ms = decode_all.percentile(99) / 1000.0;

        // ---- T1.15：模型输入通路诊断（换模型后"走了哪条快路径"投影到面板）----
        // 本处只做"取值 → 聚合 → 塞字段"的机械搬运；判定与文案全在纯函数
        // rknn/InputPathSummary.hpp::summarize_input_path（host 可单测，理由见该文件头注释）。
        {
            ModelInputPathSnapshot paths[aim::AimTargetMailbox::kMaxWorkers];
            size_t path_count = 0;
            for (const auto& worker : workers_->workers()) {
                if (!worker) continue;
                if (path_count >= aim::AimTargetMailbox::kMaxWorkers) break;
                paths[path_count++] = snapshot_input_path(worker->stats().input_path);
            }
            const ModelInputPathSummary summary = summarize_input_path(paths, path_count);
            out->model_input_pass_mode = summary.pass_mode_name;
            out->model_input_type = summary.input_type;
            out->model_input_type_name = summary.input_type_name;
            out->model_input_fmt = summary.input_fmt;
            out->model_input_fmt_name = summary.input_fmt_name;
            out->model_input_qnt_type = summary.input_qnt_type;
            out->model_input_qnt_name = summary.input_qnt_name;
            out->model_input_zp = summary.input_zp;
            out->model_input_scale = summary.input_scale;
            out->model_input_width = summary.input_w;
            out->model_input_height = summary.input_h;
            out->model_zero_copy_ready = summary.zero_copy_ready;
            out->model_fast_path_active = summary.fast_path_active;
            out->model_external_dma_requested = summary.external_dma_requested;
            out->model_external_dma_bound = summary.external_dma_bound;
            out->model_workers_total = summary.workers_total;
            out->model_workers_zero_copy = summary.workers_zero_copy;
            out->model_workers_fast_path = summary.workers_fast_path;
            out->model_input_note = summary.note;
        }
    }

    // ---- 预处理后端 + 并发吞吐（面板「预处理路径」「推理并发」的真源）----
    if (workers_) {
        using Kind = InferenceWorker::PreprocessBackendKind;
        switch (workers_->preprocess_backend()) {
            case Kind::kRga:         out->preprocess_backend = "rga"; break;
            case Kind::kCpuFallback: out->preprocess_backend = "cpu_fallback"; break;
            case Kind::kFailed:      out->preprocess_backend = "failed"; break;
            case Kind::kNone:        out->preprocess_backend.clear(); break;
        }
        out->preprocess_error = workers_->last_preprocess_error();
        // 聚合吞吐 = 路数 ÷ 单帧 e2e。这是**能力上限**，不是实际帧率
        //（实际被采集帧率封顶，144Hz 源就只会有 144）。
        const size_t wc = workers_->worker_count();
        if (wc > 0 && out->e2e_ms > 0.0) {
            out->inference_capacity_fps = static_cast<double>(wc) * 1000.0 / out->e2e_ms;
        }
    }
    if (mailbox_) {
        aim::AimTargetTask task;
        if (mailbox_->take_latest(&task)) out->detect_count = task.detections.size();
    }
    const auto aim_status = aim_thread_.status();
    out->tracks = aim_status.tracks;
    out->aim_error_x = aim_status.error_x;
    out->aim_error_y = aim_status.error_y;
    out->target_point_x = aim_status.target_point_x;
    out->target_point_y = aim_status.target_point_y;
    out->reference_x = aim_status.reference_x;
    out->reference_y = aim_status.reference_y;
    out->pid_output_x = aim_status.pid_output_x;
    out->pid_output_y = aim_status.pid_output_y;
    out->scheduler_input_x = aim_status.scheduler_input_x;
    out->scheduler_input_y = aim_status.scheduler_input_y;
    out->aim_control_y = aim_status.control_y;   // 闭环纠偏真正消费的控制域误差
    // 压枪 v1 闭环遥测（AimThread::Status → Metrics，再经 IpcServer 投影给面板）
    out->recoil_add_y = aim_status.recoil_add_y;
    out->recoil_acc_px = aim_status.recoil_acc_px;
    out->recoil_rate_px_s = aim_status.recoil_rate_px_s;
    out->aim_pos_x = aim_status.predicted_x;
    out->aim_pos_y = aim_status.predicted_y;
    out->aim_has_target = aim_status.has_target;
    out->aim_selector_hold_frames = aim_status.selector_hold_frames;  // V1.0.11 门控遥测
    out->aim_target_id = aim_status.target_id;
    out->aim_target_class_id = aim_status.target_class_id;
    out->aim_target_width = aim_status.target_width;
    out->aim_target_height = aim_status.target_height;
    out->aim_target_x1 = aim_status.target_x1;
    out->aim_target_y1 = aim_status.target_y1;
    out->aim_target_x2 = aim_status.target_x2;
    out->aim_target_y2 = aim_status.target_y2;
    out->detection_boxes = aim_status.detection_boxes;
    if (preview_) {
        const auto& metrics = preview_->metrics();
        out->preview_fps = metrics.fps.load();
        out->preview_encode_ms = metrics.encode_ms.load();
        out->preview_width = metrics.width.load();
        out->preview_height = metrics.height.load();
        out->preview_bytes = metrics.bytes.load();
        out->preview_frames = metrics.frames.load();
        out->preview_dropped = metrics.dropped.load();
    }
    out->mouse_dx = aim_status.move_x;
    out->mouse_dy = aim_status.move_y;
    out->gated_frames = aim_status.gated_frames;
    out->target_frames = aim_status.target_frames;
    out->no_target_frames = aim_status.no_target_frames;
    out->last_frame = aim_status.last_frame;
    out->last_timestamp_us = aim_status.last_timestamp_us;
    out->aim_active = aim_status.has_target;
    out->injection_allowed = aim_status.last_injection_allowed;
    out->aim_hotkeys_suspended = aim_status.hotkeys_suspended;
    out->aim_active_profile = aim_status.active_profile;
    if (auto* backend = dynamic_cast<output::OutputBackend*>(output_.get())) {
        const auto health = backend->health();
        out->output_backend_enabled = backend->enabled();
        out->mouse_control_connected = health.state == output::BackendState::kConnected;
        out->mouse_control_socket_write_ok = health.socket_write_ok;
        out->mouse_control_socket_write_fail = health.socket_write_fail;
        out->mouse_control_send_count = health.send_count;
        out->last_mouse_control_dx = health.last_dx;
        out->last_mouse_control_dy = health.last_dy;
        out->last_mouse_control_wheel = health.last_wheel;
        out->last_mouse_control_timestamp_us = health.last_timestamp_us;
    }
    // 标定分母真源：来自 AimThread 的请求投递计数（不是 socket 侧计数）。
    // 与 mouse_control_socket_write_ok 配对看：后者不涨说明发送失败 ⇒ 本计数不可用于算 gain。
    out->aim_out_counts_x = aim_status.out_counts_x;
    out->aim_out_counts_y = aim_status.out_counts_y;
}

}  // namespace ttbox::core
