// HardwareRunner.cpp — 严格的下游先停、上游后停生命周期。
#include "runtime/HardwareRunner.hpp"
#include "output/AiboxHidOutput.hpp"
#include "output/OutputBackend.hpp"
namespace ttbox::core {
// 初始化：校验输出/worker 数，装配采集与 worker 池，并配置采集参数；不启动线程。
bool HardwareRunner::initialize(const Params& p,std::string* error){
 if(!p.output){if(error)*error="HardwareRunner 输出后端为空";return false;}
 if(p.workers.worker_cores.empty()||p.workers.worker_cores.size()>aim::AimTargetMailbox::kMaxWorkers){if(error)*error="Worker 数量必须为 1~3";return false;}
 params_=p; simulated_buttons_.store(p.simulated_buttons, std::memory_order_release); capture_=std::make_unique<V4L2Capture>(); workers_=std::make_unique<WorkerPool>(); mailbox_=std::make_unique<aim::AimTargetMailbox>(p.workers.worker_cores.size());
 return capture_->configure(p.capture,error);
}
// 启动流水线：采集→模型探测/适配→worker→鼠标读取→绑定输出按键源→瞄准线程。
bool HardwareRunner::start(std::string* error){
 if(running_.exchange(true)||!capture_||!workers_||!mailbox_)return false;
 if(!capture_->open(error)||!capture_->start(error)){running_=false;return false;}
 const auto& fmt=capture_->format();
 if(params_.workers.model_path.empty()){if(error)*error="模型路径为空";stop();return false;}
 // 查询模型元数据：先用一个临时 RKNN context，适配器只读共享给全部 Worker。
 RKNNEngine probe; RKNNEngine::Params ep; ep.model_path=params_.workers.model_path; ep.core_mask=params_.workers.worker_cores.front(); ep.pass_through=false;
 if(!probe.init(ep,error)){stop();return false;}
 const auto& mi=probe.info(); std::fprintf(stderr,"[MODEL] input=%ux%u type=%d fmt=%d outputs=%u\n",mi.input_width,mi.input_height,mi.input_type,mi.input_fmt,mi.n_outputs); for(uint32_t oi=0;oi<mi.n_outputs&&oi<mi.outputs.size();++oi){const auto& o=mi.outputs[oi]; std::fprintf(stderr,"[MODEL] output[%u] type=%d fmt=%d size=%u scale=%g zp=%d dims=",oi,o.type,o.fmt,o.size,o.scale,o.zp); for(auto d:o.dims)std::fprintf(stderr,"%u,",d); std::fprintf(stderr,"\n");}
 ModelAdapterConfig acfg; acfg.color_order=ColorOrder::kBgr; acfg.conf_thres=params_.workers.conf_thres; acfg.iou_thres=params_.workers.iou_thres;
 adapter_=std::make_unique<ModelAdapter>(); if(!adapter_->analyze(probe.info(),acfg,error)){probe.destroy();stop();return false;}
 probe.destroy();
 auto wp=params_.workers; wp.latest=capture_->latest_frame_ref(); wp.frame_w=fmt.width; wp.frame_h=fmt.height; wp.aim_mailbox=mailbox_.get(); wp.runtime_config=params_.runtime_config; wp.adapter=adapter_.get();
 if(!workers_->start(wp,error)){capture_->stop();capture_->close();running_=false;return false;}
 std::string mouse_error;
 if(!mouse_reader_.start("", &mouse_error)){ if(error)*error="物理鼠标读取器启动失败: "+mouse_error; workers_->stop(); capture_->stop(); capture_->close(); running_=false; return false; }
 if (auto backend = std::dynamic_pointer_cast<output::OutputBackend>(params_.output)) {
     // 统一后端：最终输出门控读取同一个真实鼠标 event11 按钮源。
     backend->set_button_source(mouse_reader_.button_source());
     backend->set_config_source(params_.runtime_config);
 } else if (auto aibox = std::dynamic_pointer_cast<output::AiboxHidOutput>(params_.output)) {
     // 旧实现兼容：保持原绑定路径，避免旧测试/旧调用者行为变化。
     aibox->set_button_source(mouse_reader_.button_source());
     aibox->set_config_source(params_.runtime_config);
 }
 if(!aim_thread_.start(mailbox_.get(),params_.output,4000,params_.runtime_config, params_.simulated_buttons ? &simulated_buttons_ : mouse_reader_.button_source())){workers_->stop();capture_->stop();capture_->close();running_=false;return false;}
 return true;
}
// 逆序停机：瞄准→鼠标→worker→采集，最后关设备；幂等。
void HardwareRunner::stop(){if(!running_.exchange(false))return; aim_thread_.stop(); mouse_reader_.stop(); workers_->stop(); capture_->stop(); capture_->close();}
// 聚合采集/worker/瞄准统计为一个 Status 快照（供 IPC 与工具读取）。
HardwareRunner::Status HardwareRunner::status() const {
    Status s; s.running = running_.load();
    if (capture_) { s.capture_frames = capture_->metrics().capture_frames.load(); const auto& f=capture_->format(); s.width=f.width; s.height=f.height; }
    if (workers_) {
        s.worker_processed=workers_->total_processed();
        s.worker_errors=workers_->total_errors();
        s.worker_skipped=workers_->total_skipped();
        for (const auto& w : workers_->workers()) {
            const auto& ws = w->stats();
            s.worker_rga_ok += ws.rga_ok.load();
            s.worker_inference_ok += ws.inference_ok.load();
            s.worker_decode_ok += ws.decode_ok.load();
            s.worker_published += ws.published.load();
            s.worker_candidates += ws.candidates.load();
            s.worker_detections += ws.detections.load();
        }
    }
    const auto a=aim_thread_.status();
    s.aim_consumed=a.consumed;
    s.aim_target_frames=a.target_frames;
    s.aim_no_target_frames=a.no_target_frames;
    s.aim_last_frame=a.last_frame;
    s.aim_predicted_x=a.predicted_x;
    s.aim_predicted_y=a.predicted_y;
    s.aim_control_x=a.control_x;
    s.aim_control_y=a.control_y;
    s.aim_smith_dx=a.smith_dx;
    s.aim_smith_dy=a.smith_dy;
    s.aim_min_move_x=a.min_move_x;
    s.aim_max_move_x=a.max_move_x;
    s.aim_min_move_y=a.min_move_y;
    s.aim_max_move_y=a.max_move_y;
    s.aim_clipped_frames=a.clipped_frames;
    return s;
}
}
