// WorkerPool.cpp — 多 Worker 并发推理实现
#include "rknn/WorkerPool.hpp"

#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
namespace ttbox::core {
}
#else

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include "common/Logger.hpp"
#include "common/CpuAffinity.hpp"
#include "rknn/InputQuant.hpp"  // T1.14：输入量化分类 + XOR 搬运（唯一判定入口）

namespace ttbox::core {

namespace {

using clock = std::chrono::steady_clock;

// 取帧等待的兜底超时（微秒）。取"约一帧周期"：144fps ⇒ 6.94ms，故 7000。
//
// ★ 它**不是节拍器**，只是兜底。依据（两处都让"超时"几乎不可能在稳态触发）：
//   1) LatestFrame::publish() 内部是 `notify_cv_.notify_all()` 且**持 notify_mutex_**
//      （见 capture/V4L2Capture.cpp:73-76）⇒ 每帧唤醒全部等待者；worker 拿到锁后
//      wait_for 谓词会重查 current_ ⇒ 不存在漏唤醒。
//   2) wait_new() 首段还有"已有新帧就直接返回"的**快路径**（V4L2Capture.cpp:87-89，不碰锁）。
//   ⇒ 正常路径由 notify 唤醒，本超时在稳态下**基本不触发**；它不承担任何正确性/活性职责，
//     只用于 ① 停机时退出轮询循环 ② notify 路径异常时的自愈。
//
// ★ 为什么取"一帧周期"而不是更小的值（如原 400µs）：这是**语义正确性**考量，不是性能优化。
//   400µs 相对 144fps 的 6.94ms 帧周期小 17 倍，属语义错配，读代码的人会误以为它承担
//   轮询/节拍职责；而且若有人为"省唤醒"把它改回小值，一旦 notify 路径失效，恢复反而更慢。
//   实测口径：改前改后 stats_.queue_wait 的 p50 应无明显变化（预期性能收益≈0）。
//
// ★ 代价：stop() 的退出延迟上界 = 该超时（本设置约 7ms，原 0.4ms），相对模型/NPU
//   销毁耗时（~200ms 量级）可忽略。
constexpr int kFrameWaitTimeoutUs = 7000;

// float -> IEEE half（就近舍入，A-6 检测精度路径：u8->FP16 输入必须精确）

// 注：uint8→int8 XOR 搬运已迁至 rknn/InputQuant.hpp 的 xor_shift128_copy
//（header-only，Windows 默认构建亦可单测）。此处不再保留本地实现，避免两份漂移。
}  // namespace

// ---------------------------------------------------------------------------
// InferenceWorker
// ---------------------------------------------------------------------------

// 析构：确保轮询线程停止、引擎与预处理资源释放。
InferenceWorker::~InferenceWorker() {
    stop();
}

// 启动：加载模型、建立零拷贝 I/O、初始化预处理/解码器并按需预热（不启线程时留待 start_loop）。
bool InferenceWorker::start(const Params& params, std::string* error) {
    if (running_.load()) {
        if (error) *error = "worker 已在运行";
        return false;
    }
    if (params.latest == nullptr || params.model_path.empty()) {
        if (error) *error = "worker 参数无效（latest/model 缺失）";
        return false;
    }
    if (params.total_workers < 1) {
        if (error) *error = "total_workers 无效";
        return false;
    }
    params_ = params;
    id_ = params.id;

    // 模型加载先行：RGA 输出尺寸/decode 输入尺寸一律以模型实际输入为准
    // （不猜、不硬编码；黄瓦=320x320、yolo261n=640x640）
    engine_ = std::make_unique<RKNNEngine>();
    std::string eng_err;
    RKNNEngine::Params ep;
    ep.model_path = params.model_path;
    ep.core_mask = params.core_mask;
    ep.pass_through = params.pass_through;
    ep.disable_cache_flush = params.disable_cache_flush;
    if (!engine_->init(ep, &eng_err)) {
        if (error) *error = "worker RKNN init 失败: " + eng_err;
        engine_.reset();
        return false;
    }
    if (params.pass_through) {
        std::string zero_copy_error;
        if (!engine_->init_zero_copy(&zero_copy_error)) {
            // ★ 2026-09-23：分两种失败。
            //   · 普通失败（查询不过、输入没绑上）⇒ ctx 干净，照旧回退兼容 I/O；
            //   · zero_copy_fatal() ⇒ 输入已绑上且**无解绑 API**，ctx 处于半绑态，
            //     兼容 I/O 会与它冲突 ⇒ 这个 worker 会永久 100% 推理失败。
            //     宁可让 worker 启动失败（错误可见），也不能留一个静默坏的推理通道。
            if (engine_->zero_copy_fatal()) {
                std::string fatal = "worker[" + std::to_string(id_) +
                                    "] 零拷贝半绑失败，本引擎不可用（需重建）: " + zero_copy_error;
                TTBOX_LOG_ERROR(fatal);
                if (error) *error = fatal;
                engine_->destroy();
                engine_.reset();
                return false;
            }
            TTBOX_LOG_WARN("worker[" + std::to_string(id_) + "] 零拷贝不可用，回退兼容 I/O: " + zero_copy_error);
        }
    }
    const uint32_t in_w = engine_->info().input_width;
    const uint32_t in_h = engine_->info().input_height;

    // ---- T1.15：输入通路诊断一次性快照 ----
    // 位置必须在 init_zero_copy 之后：pass_mode_ / zero_copy_ready_ 都由它写入
    // （RKNNEngine.cpp:308 是唯一写分类结论之处）。放在这里 ⇒ 面板读到的就是
    // 本 worker 后续每帧真正会走的那条路径，而不是"配置想走的那条"。
    {
        const auto& mi = engine_->info();
        stats_.input_path.pass_mode.store(static_cast<int32_t>(engine_->input_pass_mode()),
                                          std::memory_order_relaxed);
        stats_.input_path.input_type.store(mi.input_type, std::memory_order_relaxed);
        stats_.input_path.input_fmt.store(mi.input_fmt, std::memory_order_relaxed);
        stats_.input_path.input_qnt_type.store(mi.input_qnt_type, std::memory_order_relaxed);
        stats_.input_path.input_zp.store(mi.input_zp, std::memory_order_relaxed);
        store_float_bits(stats_.input_path.input_scale_bits, mi.input_scale);
        stats_.input_path.input_w.store(in_w, std::memory_order_relaxed);
        stats_.input_path.input_h.store(in_h, std::memory_order_relaxed);
        stats_.input_path.zero_copy_ready.store(engine_->zero_copy_ready(), std::memory_order_relaxed);
        stats_.input_path.fast_path_active.store(engine_->pass_through_active(), std::memory_order_relaxed);
        stats_.input_path.external_dma_requested.store(params.external_dma_input, std::memory_order_relaxed);
        stats_.input_path.external_dma_bound.store(false, std::memory_order_relaxed);
    }

    preprocess_ = std::make_unique<Preprocess>();
    PreprocessConfig pcfg;
    pcfg.detect_size = {in_w, in_h};
    pcfg.input_type = engine_->info().input_type;
    pcfg.input_size = engine_->info().input_size;
    pcfg.backend = PreprocessBackend::kRga;
    pcfg.center_crop = true;
    pcfg.color_order = params.color_order;
    std::string preprocess_error;
    if (!preprocess_->init(pcfg, &preprocess_error)) {
        if (error) *error = "worker Preprocess/RGA init 失败: " + preprocess_error;
        preprocess_.reset();
        engine_.reset();
        return false;
    }

    raw_outputs_.clear();
    raw_buf_ptrs_.clear();
    raw_sizes_.clear();
    for (const auto& oi : engine_->info().outputs) {
        raw_outputs_.emplace_back(oi.size, 0);
        raw_buf_ptrs_.push_back(raw_outputs_.back().data());
        raw_sizes_.push_back(oi.size);
    }
    // ---- A-6/A-7：解码器（优先 ModelAdapter 创建；否则默认 DecodeNMS）----
    std::string derr;
    if (params.adapter != nullptr) {
        decoder_ = params.adapter->create_decoder(&derr);
        if (!decoder_) {
            if (error) *error = "worker decoder 创建失败: " + derr;
            preprocess_.reset();
            engine_.reset();
            return false;
        }
        decoder_->set_frame(params.frame_w, params.frame_h);
    } else {
        DecodeParams dp;
        dp.conf_thres = params.conf_thres;
        dp.iou_thres = params.iou_thres;
        dp.classwise = true;
        dp.input_w = in_w;  // 模型实际输入尺寸（DFL stride 计算依赖）
        dp.input_h = in_h;
        dp.frame_w = params.frame_w;
        dp.frame_h = params.frame_h;
        auto d = std::make_unique<DecoderImpl>();
        if (!d->configure(dp, &derr)) {
            if (error) *error = "worker DecodeNMS 配置失败: " + derr;
            preprocess_.reset();
            engine_.reset();
            return false;
        }
        decoder_ = std::move(d);
    }
    if (params.runtime_config != nullptr) {
        if (auto profile = params.runtime_config->snapshot()) geometry_filter_.set_config(profile->geometry_filter);
    }

    // ---- 预热：在起轮询线程之前把 NPU 上下文初始化掉 ----
    // 放在这里而不是等第一帧：首帧推理要现初始化 NPU 上下文，慢一个量级，
    // 用户感知就是「点开始后要等一会儿才出结果」。预热后统计会被清空。
    if (params.warmup_rounds > 0) {
        std::string werr2;
        if (!engine_->warmup(params.warmup_rounds, &werr2)) {
            // 预热失败不算致命：NPU 上下文会在首帧自然初始化，只是首帧仍慢。
            TTBOX_LOG_WARN("worker[" + std::to_string(id_) + "] 预热失败（不致命）: " + werr2);
        }
    }

    TTBOX_LOG_INFO("worker[" + std::to_string(id_) + "] 就绪: core_mask=" +
                   std::to_string(params.core_mask) + " 模型加载 " +
                   std::to_string(engine_->load_ms()) + "ms");

    // 预加载（deferred_start）时只加载+预热，不拉起轮询线程；
    // 等真实采集尺寸确定后由 start_loop() 补齐。
    if (!params.deferred_start) {
        running_.store(true);
        thread_ = std::thread(&InferenceWorker::loop, this);
    }
    return true;
}

// 预加载后拉起轮询线程（deferred_start 模式的第二阶段）。
bool InferenceWorker::start_loop(std::string* error) {
    if (running_.load()) {
        return true;
    }
    if (!engine_) {
        if (error) *error = "引擎未初始化（未预加载？）";
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&InferenceWorker::loop, this);
    return true;
}

// 更新采集帧尺寸（透传给解码器用于坐标映射）。
void InferenceWorker::set_frame_size(uint32_t w, uint32_t h) {
    params_.frame_w = w;
    params_.frame_h = h;
    if (decoder_) decoder_->set_frame(w, h);
}

// 停止：置停标志、join 轮询线程、销毁引擎/预处理并复位诊断。
void InferenceWorker::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (engine_) engine_->destroy();
    preprocess_.reset();
    engine_.reset();
    // T1.15：停后清空输入通路诊断。否则"停止态"面板仍会显示上一次运行时的
    // "xor_shift128 / 快路径已激活"，读者会把历史结论当成当前事实。
    stats_.input_path.reset();
}

// A-8：应用最新 RuntimeProfile（仅当配置快照变化时）。
// conf/iou/class_filter/max_detections/FOV → decoder；ROI → decoder+RGA（安全点）。
void InferenceWorker::apply_runtime_profile() {
    if (params_.runtime_config == nullptr || !decoder_) return;
    auto prof = params_.runtime_config->snapshot();
    if (!prof || prof == applied_profile_) return;

    decoder_->apply_runtime(prof->inference, prof->fov);
    geometry_filter_.set_config(prof->geometry_filter);
    // 准星找色参数：随 profile 一起热更新（不每帧读快照）。
    // 关掉时把命中清掉，避免沿用上一轮的「命中」让扳机一直停手。
    stop_detect_params_.enabled = prof->mouse.trigger2.stop_detect_enabled;
    stop_detect_params_.color_id = prof->mouse.trigger2.stop_detect_color_id;
    stop_detect_params_.tolerance = prof->mouse.trigger2.stop_detect_tolerance;
    stop_detect_params_.range_px = prof->mouse.trigger2.stop_detect_range;
    stop_detect_params_.interval_frames = prof->mouse.trigger2.stop_detect_interval > 0
                                              ? prof->mouse.trigger2.stop_detect_interval
                                              : 1;
    if (!stop_detect_params_.enabled) stop_detect_hit_ = false;
    const uint32_t rw = prof->capture.width;
    const uint32_t rh = prof->capture.height;
    const uint32_t fw = params_.frame_w, fh = params_.frame_h;
    if (rw > 0 && rh > 0 && fw > 0 && fh > 0 && rw <= fw && rh <= fh) {
        // V1.0.13：capture.offset_x/y 已删 ⇒ ROI 恒以屏幕中心为心，
        // 转左上角起点并 clamp 到全帧内。
        const int32_t cx = static_cast<int32_t>(fw / 2);
        const int32_t cy = static_cast<int32_t>(fh / 2);
        const int32_t rx = std::max<int32_t>(0, std::min<int32_t>(
            cx - static_cast<int32_t>(rw / 2), static_cast<int32_t>(fw - rw)));
        const int32_t ry = std::max<int32_t>(0, std::min<int32_t>(
            cy - static_cast<int32_t>(rh / 2), static_cast<int32_t>(fh - rh)));
        decoder_->set_roi(static_cast<uint32_t>(rx), static_cast<uint32_t>(ry), rw, rh);
        if (preprocess_) preprocess_->set_crop(static_cast<uint32_t>(rx), static_cast<uint32_t>(ry), rw, rh);
    } else if (rw == 0 && rh == 0 && fw > 0 && fh > 0) {
        // 0×0 是合法的“自动中心区域”语义：Preprocess 清除显式 ROI 后，RGA
        // 会按 center_crop 取输入画面的中心正方形。Decoder 必须显式使用同一个
        // 正方形坐标做模型→采集帧映射；若也设 0×0，会误按整张宽屏映射而偏框。
        const uint32_t side = std::min(fw, fh);
        const uint32_t rx = (fw - side) / 2;
        const uint32_t ry = (fh - side) / 2;
        decoder_->set_roi(rx, ry, side, side);
        if (preprocess_) preprocess_->set_crop(0, 0, 0, 0);
    }
    applied_profile_ = std::move(prof);
}

// worker 主循环：认领帧 → 预处理 → 推理 → 解码/几何过滤 → 发布目标并累积统计。
void InferenceWorker::loop() {
    using clock = std::chrono::steady_clock;
    // 每个推理 worker 固定到一个独立 A76 大核（CPU4/5/6），必须在 worker
    // 线程内部绑定；start() 里绑定会作用于调用线程（main），导致绑核失效。
    {
        std::string aerr;
        const uint64_t worker_cpu_mask =
            (id_ >= 0 && id_ < 3) ? (1ULL << static_cast<unsigned>(4 + id_))
                                  : CpuAffinity::kBigCoreMask;
        if (!CpuAffinity::set_thread_affinity(worker_cpu_mask, &aerr)) {
            TTBOX_LOG_WARN("worker[" + std::to_string(id_) + "] 绑定 CPU 失败: " + aerr);
        } else {
            TTBOX_LOG_INFO("worker[" + std::to_string(id_) + "] 已绑定独立大核 cpu" +
                           std::to_string(4 + id_));
        }
    }
    while (running_.load()) {
        // ★ 事件唤醒取帧（替代固定 400 µs 轮询）：帧一发布就被唤醒，
        //   消除"平均空等 ~200 µs"这笔 queue_wait（板端实测 0.239 ms，2026-09-23）。
        //   超时仅作兜底（publish 走 notify_all，不会漏唤醒），取值 = 约一帧周期，
        //   详见 kFrameWaitTimeoutUs 注释：① 停止时能退出循环；② 极端丢通知可自愈。
        auto frame = params_.latest->wait_new(last_seq_, kFrameWaitTimeoutUs);
        if (!frame) {
            // ★ 纯观测计数（2026-10-08）：等满 7ms 仍无新 seq ⇒ 采集断流/通知丢失/调度饿死。
            //   这是**真丢帧信号**，区别于下面的 skipped（那是轮转让帧，稳态 ≈(N−1)×processed）。
            //   ⚠ 已排除停止期误计：continue 之后 while(running_.load()) 会再判一次，
            //     故 stop() 路径最多让每个 worker 多计 1（一次性、可忽略），稳态下应为 0。
            stats_.no_frame_timeout.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const uint32_t seq = frame->info.sequence;
        if (seq == last_seq_) continue;   // 极小概率的重复唤醒
        // 固定轮转分配：避免多个 worker 同时处理同一张 latest frame。
        if (seq % static_cast<uint32_t>(params_.total_workers) !=
            static_cast<uint32_t>(params_.id)) {
            stats_.skipped.fetch_add(1);
            last_seq_ = seq;  // 该帧由其他 worker 处理
            continue;
        }
        last_seq_ = seq;

        // A-8：热更新配置（仅变化时应用，无逐帧 JSON/IPC）
        apply_runtime_profile();

        // ---- E2E 起点：帧采集时刻（v4l2 单调时钟，与 steady_clock 同基准）----
        const double recv_ms = frame->info.timestamp_ms;
        const auto e2e_t0 = clock::now();
        // 排队等待（buffer_age 同口径）：帧时间戳 → worker 认领
        const double claim_ms_q = std::chrono::duration<double, std::milli>(e2e_t0.time_since_epoch()).count();
        stats_.queue_wait.add(static_cast<uint64_t>(std::max(0.0, (claim_ms_q - recv_ms) * 1000.0)));

    // ---- 唯一预处理入口：Preprocess（生产默认 RGA，CPU 仅显式 fallback）----
    PreprocessedFrame prepared;
    std::string preprocess_error;
    const auto preprocess_begin = clock::now();
    if (!preprocess_ || !preprocess_->process(*frame, &prepared, &preprocess_error)) {
        stats_.errors.fetch_add(1);
        record_preprocess_error(preprocess_error);
        if (stats_.errors.load() <= 3) std::fprintf(stderr, "worker[%d] Preprocess: %s\n", id_, preprocess_error.c_str());
        continue;
    }
    if (preprocess_->using_rga()) {
        stats_.rga_ok.fetch_add(1);
        stats_.rga.add(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - preprocess_begin).count());
    } else {
        stats_.direct_ok.fetch_add(1);
    }
    const uint8_t* input_ptr = prepared.tensor_data ? prepared.tensor_data : prepared.data;
    const size_t input_bytes = prepared.tensor_size ? prepared.tensor_size : prepared.size;
    if (!input_ptr || input_bytes == 0) {
        stats_.errors.fetch_add(1);
        continue;
    }
    // 【两处 frame.reset() 的分工（勿删任一处）】此处为"条件提前释放"：仅当本帧之后不再用到
    // 像素（急停检测停用）时，立即归还 V4L2 buffer，保住原有优化。急停检测开启时必须把帧留到
    // 下面准星找色（frame->data/frame->info）之后，故此处不 reset；帧末还有一次幂等 reset 兜底。
    if (!stop_detect_params_.enabled) frame.reset();

        // ---- RKNN 推理（本 worker 独立 context）+ 原生输出 + Decode/NMS ----
        std::string ierr;
        const auto t_infer0 = clock::now();
        bool infer_ok = false;
        if (engine_->zero_copy_ready()) {
            if (!engine_->input_memory() || input_bytes > engine_->input_memory_size()) {
                ierr = "零拷贝输入尺寸不匹配";
            } else {
                // INT8/NHWC 模型可把 RGA 常驻 DMA-BUF 直接交给 RKNN，
                // 省掉每帧 RGA->CPU/RKNN 输入 memcpy；fd 变化时才重新绑定。
                // ★ 只有 UINT8 原生输入才**可能**直绑（engine->external_dma_supported()，
                //   init 时一次性判定）。INT8（kXorShift128）恒被拒，此处必须跳过：
                //   否则每帧一次 rknn_query + 一条 WARN（144fps 下每秒 144 条刷爆 journal）。
                if (params_.external_dma_input && engine_->external_dma_supported() &&
                    prepared.dma_fd >= 0 &&
                    prepared.dma_fd != bound_input_dma_fd_) {
                    if (engine_->bind_external_input_fd(prepared.dma_fd,
                            const_cast<uint8_t*>(input_ptr), input_bytes, &ierr)) {
                        bound_input_dma_fd_ = prepared.dma_fd;
                        stats_.input_path.external_dma_bound.store(true, std::memory_order_relaxed);
                    } else {
                        // 绑定新 fd 失败必须回退 CPU 拷贝，不能保留旧 fd 继续
                        // run_zero_copy，否则 rknn_run 会一直读取旧 DMA-BUF 画面。
                        bound_input_dma_fd_ = -1;
                        // 诊断必须跟着回落：否则面板会继续声称"DMA-BUF 已直绑"，
                        // 而实际每帧在走 CPU 拷贝 —— 正是本次要消灭的那种谎报。
                        stats_.input_path.external_dma_bound.store(false, std::memory_order_relaxed);
                    }
                }
                // external DMA-BUF 直传不做 XOR，仅 UINT8 原生（方案 D）正确；
                // bound_input_dma_fd_>=0 结构上已蕴含 kUint8Native（bind 只对该模式放行）。
                // 门控条件取 kUint8Native（不是 kXorShift128）：INT8 直传绕过 XOR 必错。
                if (params_.external_dma_input && bound_input_dma_fd_ >= 0 &&
                    engine_->input_pass_mode() == InputPassMode::kUint8Native) {
                    infer_ok = engine_->run_zero_copy(&ierr);
                } else {
                    const auto copy_begin = clock::now();
                    uint8_t* dst = static_cast<uint8_t*>(engine_->input_memory());
                    bool copy_ok = true;
                    // 搬运语义由引擎唯一判定结论决定（此处外层已保证
                    // zero_copy_ready()==true，故只可能 kXorShift128 / kUint8Native；
                    // kCompatible 不可达——init_zero_copy 已对其 return false）。
                    switch (engine_->input_pass_mode()) {
                    case InputPassMode::kXorShift128:
                        // pass_through=1 时 NPU 输入 mem 是原生 int8，直接 memcpy uint8
                        // 会破坏量化映射，需做 XOR 0x80（u-128）。
                        xor_shift128_copy(dst, input_ptr, input_bytes);
                        break;
                    case InputPassMode::kUint8Native:
                        std::memcpy(dst, input_ptr, input_bytes);  // 原生 UINT8，零变换
                        break;
                    default:  // kCompatible：理论不可达
                        ierr = "内部状态错误：kCompatible 到达零拷贝搬运路径";
                        copy_ok = false;
                        break;
                    }
                    stats_.stages.set_input.add(
                        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - copy_begin).count());
                    infer_ok = copy_ok && engine_->run_zero_copy(&ierr);  // copy 出错则短路，不 run
                }
            }
        } else {
            infer_ok = engine_->set_input(input_ptr, input_bytes, &ierr) && engine_->run(&ierr);
        }
        if (!infer_ok) {
            stats_.errors.fetch_add(1);
            if (stats_.errors.load() <= 3) std::fprintf(stderr, "worker[%d] infer: %s\n", id_, ierr.c_str());
            continue;
        }
        stats_.inference_ok.fetch_add(1);
        if (engine_->zero_copy_ready()) {
            raw_buf_ptrs_.clear();
            raw_sizes_.clear();
            for (uint32_t i = 0; i < engine_->info().n_outputs; ++i) {
                raw_buf_ptrs_.push_back(engine_->output_memory(i));
                raw_sizes_.push_back(engine_->output_memory_size(i));
            }
        } else if (!engine_->get_raw_outputs(raw_buf_ptrs_.data(), raw_sizes_.data(), &ierr)) {
            stats_.errors.fetch_add(1);
            if (stats_.errors.load() <= 3) std::fprintf(stderr, "worker[%d] raw_outputs: %s\n", id_, ierr.c_str());
            continue;
        }
        // 推理总耗时只覆盖输入拷贝/提交、NPU run 和输出准备；Decode/NMS
        // 在下面单独统计，避免 infer_ms 与 decode_ms 重复计算。
        stats_.stages.total.add(
            std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t_infer0).count());
        std::string derr;
        if (!decoder_->process(engine_->info(), raw_buf_ptrs_.data(), &detections_, &derr)) {
            stats_.errors.fetch_add(1);
            if (stats_.errors.load() <= 3) std::fprintf(stderr, "worker[%d] decode: %s\n", id_, derr.c_str());
            continue;
        }
        if (geometry_filter_.config().enabled && params_.frame_w > 0 && params_.frame_h > 0) {
            detections_ = geometry_filter_.filter(detections_, static_cast<float>(params_.frame_w) * 0.5f, static_cast<float>(params_.frame_h) * 0.5f);
        }
        stats_.decode_ok.fetch_add(1);
        stats_.processed.fetch_add(1);
        const uint64_t now_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count());
        if (params_.aim_mailbox) {
            aim::AimTargetTask task;
            task.frame_number = seq;
            task.timestamp_us = now_us;
            task.worker_id = id_;
            task.frame_width = params_.frame_w;
            task.frame_height = params_.frame_h;
            // detections_ 在本帧发布后不再需要（下一帧 decode 会先 clear），
            // 直接 move 省掉每帧一次候选 vector 深拷贝。
            task.detections = std::move(detections_);
            if (!task.detections.empty()) {
                task.has_target = true;
                task.target = task.detections.front();
                task.aim_point = {
                    (task.target.x1 + task.target.x2) * 0.5f,
                    (task.target.y1 + task.target.y2) * 0.5f};
                task.target_width = task.target.x2 - task.target.x1;
                task.target_height = task.target.y2 - task.target.y1;
            }
            // 准星找色：只在**这一侧**能看到像素（任务不传图像），算好的 bool 随任务带走。
            // 准星恒在画面几何中心（crop 只决定送给模型的区域，不移动准星）。
            // 按 interval 节流：取色要读 25 个像素，不必每帧做；未检测的帧沿用上次结果。
            if (stop_detect_params_.enabled) {
                const int interval = stop_detect_params_.interval_frames > 0
                                         ? stop_detect_params_.interval_frames : 1;
                if (stop_detect_last_frame_ == 0 ||
                    seq - stop_detect_last_frame_ >= static_cast<uint32_t>(interval)) {
                    stop_detect_last_frame_ = seq;
                    stop_detect_hit_ = aim::crosshair_probe_hit(
                        frame->data.get(), frame->info,
                        static_cast<float>(params_.frame_w) * 0.5f,
                        static_cast<float>(params_.frame_h) * 0.5f,
                        stop_detect_params_);
                }
            }
            task.stop_detect_hit = stop_detect_params_.enabled ? stop_detect_hit_ : false;
            params_.aim_mailbox->offer(static_cast<std::size_t>(id_), std::move(task));
            stats_.published.fetch_add(1);
            // 瞬时帧率采样（滚动窗口）：3 个 worker 合计即为真实推理帧率。
            // 不能只靠 published÷运行时长 —— 那是累计平均，会把启动开销永久摊进
            // 分母，表现为"帧率缓慢爬升"（2026-09-23 板端现象）。
            if (params_.fps_meter) params_.fps_meter->tick();
        }
        // 【两处 frame.reset() 的分工 · 帧末兜底】急停检测停用时上面已提前 reset，此处是幂等的
        // no-op；急停检测开启时则必须留到这里 —— 准星找色（frame->data/frame->info）是本帧最后
        // 一个 frame 使用者。两处不可合并，否则开启急停检测即解引用空 shared_ptr。
        frame.reset();
        // 吸收本帧 RKNNEngine 阶段统计（absorb 后 reset，避免重复累计）
        {
            const auto& est = engine_->stats();
            stats_.stages.set_input.absorb(est.set_input);
            stats_.stages.run.absorb(est.run);
            stats_.stages.output.absorb(est.output);
            stats_.stages.total.absorb(est.total);
            engine_->reset_stats();
        }
        // 吸收本帧 Decode/NMS 统计 + 候选/目标计数
        {
            const auto& ds = decoder_->stats();
            stats_.decode_stages.decode.absorb(ds.decode);
            stats_.decode_stages.nms.absorb(ds.nms);
            stats_.decode_stages.total.absorb(ds.total);
            stats_.candidates.fetch_add(ds.candidates.load());
            stats_.detections.fetch_add(ds.detections.load());
            decoder_->reset_stats();
        }
        // E2E（us）= 帧采集→认领（单调毫秒差值，v4l2 与 steady_clock 同基准）
        //          + 认领→完成（处理耗时）
        const double claim_ms =
            std::chrono::duration<double, std::milli>(e2e_t0.time_since_epoch()).count();
        const uint64_t e2e_us =
            static_cast<uint64_t>((claim_ms - recv_ms) * 1000.0) +
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      clock::now() - e2e_t0).count());
        stats_.e2e.add(e2e_us);
    }
}

// ---------------------------------------------------------------------------
// WorkerPool
// ---------------------------------------------------------------------------

namespace {

// 并行创建并（按需）启动 N 个 worker。
// 并行理由：每个 worker 都要 rknn_init 一遍模型（N 个 = N 次加载），串行时
// 「点开始」后的等待 = N × 单次加载耗时；init/预热互不依赖，可以并发。
// deferred=true 时只做加载+预热，不拉起轮询线程（预加载）。
bool create_workers(const WorkerPool::Params& params, bool deferred,
                    std::vector<std::unique_ptr<InferenceWorker>>& created,
                    std::string* error) {
    const size_t n = params.worker_cores.size();
    std::vector<std::future<std::pair<bool, std::string>>> futs;
    futs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto worker = std::make_unique<InferenceWorker>();
        InferenceWorker::Params wp;
        wp.id = static_cast<int>(i);
        wp.core_mask = params.worker_cores[i];
        wp.model_path = params.model_path;
        wp.pass_through = params.pass_through;
        wp.external_dma_input = params.external_dma_input;
        wp.disable_cache_flush = params.disable_cache_flush;
        wp.warmup_rounds = params.warmup_rounds;
        wp.deferred_start = deferred;
        wp.out_w = params.out_w;
        wp.out_h = params.out_h;
        wp.latest = params.latest;
        wp.fps_meter = params.fps_meter;
        wp.total_workers = static_cast<int>(n);
        wp.conf_thres = params.conf_thres;
        wp.iou_thres = params.iou_thres;
        wp.frame_w = params.frame_w;
        wp.frame_h = params.frame_h;
        wp.color_order = params.color_order;
        wp.adapter = params.adapter;
        wp.runtime_config = params.runtime_config;
        wp.aim_mailbox = params.aim_mailbox;
        InferenceWorker* raw = worker.get();
        created.push_back(std::move(worker));
        futs.push_back(std::async(std::launch::async, [raw, wp]() -> std::pair<bool, std::string> {
            std::string werr;
            const bool ok = raw->start(wp, &werr);
            return {ok, werr};
        }));
    }
    for (size_t i = 0; i < n; ++i) {
        auto r = futs[i].get();
        if (!r.first) {
            for (auto& w : created) {
                if (w) w->stop();
            }
            created.clear();
            if (error) *error = "worker[" + std::to_string(i) + "] 启动失败: " + r.second;
            return false;
        }
    }
    return true;
}

}  // namespace

// 创建并启动全部 worker（worker_cores.size() 决定数量，含轮询线程）。
bool WorkerPool::start(const Params& params, std::string* error) {
    if (!workers_.empty()) {
        if (error) *error = "WorkerPool 已在运行";
        return false;
    }
    if (params.worker_cores.empty()) {
        if (error) *error = "worker_cores 为空（worker 数量必须 ≥1）";
        return false;
    }
    return create_workers(params, false, workers_, error);
}

// 预加载：并行完成模型加载 + 预热，但不拉起轮询线程。
bool WorkerPool::preload(const Params& params, std::string* error) {
    if (!workers_.empty()) {
        if (error) *error = "WorkerPool 已在运行";
        return false;
    }
    if (params.worker_cores.empty()) {
        if (error) *error = "worker_cores 为空（worker 数量必须 ≥1）";
        return false;
    }
    return create_workers(params, true, workers_, error);
}

// 为已预加载的 worker 逐个拉起轮询线程。
bool WorkerPool::start_loops(std::string* error) {
    for (auto& w : workers_) {
        std::string e;
        if (!w || !w->start_loop(&e)) {
            stop();
            if (error) *error = "worker 轮询线程启动失败: " + e;
            return false;
        }
    }
    return true;
}

// 向所有 worker 透传采集帧尺寸。
void WorkerPool::set_frame_size(uint32_t w, uint32_t h) {
    for (auto& x : workers_) {
        if (x) x->set_frame_size(w, h);
    }
}

// 停止并销毁全部 worker（释放模型与 NPU 上下文）。
void WorkerPool::stop() {
    for (auto& w : workers_) {
        if (w) w->stop();
    }
    workers_.clear();
}

// ★ 2026-10-04 性能修复：只停线程、**保留 worker 对象**（模型与 NPU 上下文留着）。
//
// 为什么需要：原stop() 里的 workers_.clear() 会销毁 3 个 InferenceWorker，
//   下次 start() 必须重新 create_workers → 重新 rknn_init + 绑 I/O + 预热
//   （板端实测模型加载 79/56/75ms + 预热 27/32/27ms，并行后仍占~200ms）。
//   而用户点"停止"只是不想让 AI 干活，**模型文件并没有变**。
//   ⇒ 停循环不停对象：下次 start 只需 set_frame_size() + start_loops()，
//   省掉整个模型加载过程（实测可省 ~200ms）。
//
// ★ 为什么默认不开启（core_runtime 侧仍走 stop() 全清）：
//   保留 worker ⇒ NPU 三个核心与模型内存**常驻**（RK3588 单核 NPU 内存不小）。
//   业主"停止"的语义可能就是想彻底释放。所以本函数提供能力，
//   由调用方按产品语义决定用哪个 —— CoreRuntime::stop() 暂不切换。
void WorkerPool::stop_keep_workers() {
    for (auto& w : workers_) {
        if (w) w->stop();
    }
    // ★ 不workers_.clear()：对象留着，下次 start_loops() 直接拉起线程。
}

// ★ 下面 5 个只读统计函数原先不判空，与同文件的 set_frame_size(:589)/stop(:595) 不一致。
//   当前 workers_ 的元素只由 make_unique 结果 push_back（从不放 nullptr）⇒ 不可达，
//   属一致性债务；但停止/重载路径一旦引入空槽就是解引用空指针。（2026-09-23 审查复核 #33）
uint64_t WorkerPool::total_processed() const {
    uint64_t s = 0;
    for (const auto& w : workers_) {
        if (!w) continue;
        s += w->stats().processed;
    }
    return s;
}

uint64_t WorkerPool::total_errors() const {
    uint64_t s = 0;
    for (const auto& w : workers_) {
        if (!w) continue;
        s += w->stats().errors;
    }
    return s;
}

uint64_t WorkerPool::total_skipped() const {
    uint64_t s = 0;
    for (const auto& w : workers_) {
        if (!w) continue;
        s += w->stats().skipped;
    }
    return s;
}

// 取帧等待超时累计（跨 worker 求和）：帧根本没到 = 真断流信号。
// 与 total_skipped() 成对使用：前者是"轮转让帧"（稳态必然发生），本者是"没等到帧"。
uint64_t WorkerPool::total_no_frame_timeout() const {
    uint64_t s = 0;
    for (const auto& w : workers_) {
        if (!w) continue;
        s += w->stats().no_frame_timeout;
    }
    return s;
}

// ---- 预处理后端诊断 ----
// 这几个方法**故意放在平台无关区**：WorkerPool.cpp 主体被 `#if !_WIN32` 包着，
// 但 CoreRuntime 在 Windows 本机构建里也会调用它们（面板指标走同一条代码路径），
// 放主体里 ⇒ Windows 链接期 undefined reference。
// 判定只用 Preprocess::using_rga() 与 RgaMetrics（两者在 Windows 上都有安全退化的 inline 实现）。
InferenceWorker::PreprocessBackendKind InferenceWorker::preprocess_backend() const {
    if (!preprocess_) return PreprocessBackendKind::kNone;
    if (!preprocess_->using_rga()) return PreprocessBackendKind::kCpuFallback;
    const RgaMetrics* m = preprocess_->rga_metrics();
    if (m != nullptr && m->error_frames.load() > 0) return PreprocessBackendKind::kFailed;
    return PreprocessBackendKind::kRga;
}

std::string InferenceWorker::last_preprocess_error() const {
    std::lock_guard<std::mutex> lk(preprocess_err_mu_);
    return last_preprocess_error_;
}

// 记录最近一次预处理失败原因（供面板「最后错误」显示）。
void InferenceWorker::record_preprocess_error(const std::string& error) {
    if (error.empty()) return;
    std::lock_guard<std::mutex> lk(preprocess_err_mu_);
    last_preprocess_error_ = error;
}

InferenceWorker::PreprocessBackendKind WorkerPool::preprocess_backend() const {
    using Kind = InferenceWorker::PreprocessBackendKind;
    Kind worst = Kind::kNone;
    for (const auto& w : workers_) {
        if (!w) continue;
        const Kind k = w->preprocess_backend();
        // 数字越大越差：kNone(0) < kRga(1) < kCpuFallback(2) < kFailed(3)
        if (static_cast<int>(k) > static_cast<int>(worst)) worst = k;
    }
    return worst;
}

std::string WorkerPool::last_preprocess_error() const {
    for (const auto& w : workers_) {
        if (!w) continue;
        const std::string e = w->last_preprocess_error();
        if (!e.empty()) return e;
    }
    return std::string();
}

}  // namespace ttbox::core

#endif  // !_WIN32
