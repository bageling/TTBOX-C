// imgdetect.cpp — 板端单图 GT 推理工具（真实 RKNN + TTBOX DecodeNMS）
// 对齐 TTBOX 摄像头链路的预处理几何：中心 ROI(500x500) → 模型输入，解码映射回原图。
// 用法:
//   imgdetect --model <model.rknn> --raw <BGR24.raw> --w 2560 --h 1440
//             [--roi-x 1030 --roi-y 470 --roi-w 500 --roi-h 500]
//             [--conf 0.55 --iou 0.45] [--json]
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include <cmath>

#include "common/Types.hpp"
#include "rknn/Preprocess.hpp"
#include "rknn/RKNNEngine.hpp"
#include "rknn/DecodeNMS.hpp"
#include "model/ModelAdapter.hpp"
#include "mouse/TargetSelector.hpp"

using namespace ttbox::core;

static void print_det(const DetectionBox& d, bool json) {
    if (json) {
        printf("{\"class\":%d,\"conf\":%.4f,\"x1\":%.2f,\"y1\":%.2f,\"x2\":%.2f,\"y2\":%.2f,"
               "\"cx\":%.2f,\"cy\":%.2f}\n", d.class_id, d.score, d.x1, d.y1, d.x2, d.y2,
               (d.x1 + d.x2) * 0.5f, (d.y1 + d.y2) * 0.5f);
    } else {
        printf("class=%d conf=%.4f box=[%.2f,%.2f,%.2f,%.2f] center=(%.2f,%.2f) w=%.2f h=%.2f\n",
               d.class_id, d.score, d.x1, d.y1, d.x2, d.y2,
               (d.x1 + d.x2) * 0.5f, (d.y1 + d.y2) * 0.5f, (d.x2 - d.x1), (d.y2 - d.y1));
    }
}

int main(int argc, char** argv) {
    std::string model_path, raw_path;
    uint32_t frame_w = 2560, frame_h = 1440;
    uint32_t roi_x = 1030, roi_y = 470, roi_w = 500, roi_h = 500;
    float conf = 0.55f, iou = 0.45f;
    float raw_scale = 1.0f / 255.0f;  // FP16 输入归一化系数（yolo261n 期望 0-1）
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* n) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺值: %s\n", n); std::exit(1); }
            return argv[++i];
        };
        std::string a = argv[i];
        if (a == "--model") model_path = next("--model");
        else if (a == "--raw") raw_path = next("--raw");
        else if (a == "--w") frame_w = (uint32_t)std::atoi(next("--w"));
        else if (a == "--h") frame_h = (uint32_t)std::atoi(next("--h"));
        else if (a == "--roi-x") roi_x = (uint32_t)std::atoi(next("--roi-x"));
        else if (a == "--roi-y") roi_y = (uint32_t)std::atoi(next("--roi-y"));
        else if (a == "--roi-w") roi_w = (uint32_t)std::atoi(next("--roi-w"));
        else if (a == "--roi-h") roi_h = (uint32_t)std::atoi(next("--roi-h"));
        else if (a == "--conf") conf = (float)std::atof(next("--conf"));
        else if (a == "--iou") iou = (float)std::atof(next("--iou"));
        else if (a == "--raw-scale") raw_scale = (float)std::atof(next("--raw-scale"));
        else if (a == "--json") json = true;
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); return 2; }
    }
    if (model_path.empty() || raw_path.empty()) {
        std::fprintf(stderr, "usage: imgdetect --model <rknn> --raw <raw>\n");
        return 2;
    }

    // ---- 读取 BGR24 原始帧 ----
    const size_t frame_bytes = (size_t)frame_w * frame_h * 3;
    std::ifstream f(raw_path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "无法打开 raw: %s\n", raw_path.c_str()); return 3; }
    std::vector<uint8_t> pixel(frame_bytes);
    f.read(reinterpret_cast<char*>(pixel.data()), frame_bytes);
    const size_t got = f.gcount();
    f.close();
    if (got < frame_bytes) { std::fprintf(stderr, "raw 不完整 %zu/%zu\n", got, frame_bytes); return 3; }

    FrameBuffer fb;
    fb.data = std::shared_ptr<uint8_t[]>(new uint8_t[frame_bytes]);
    std::memcpy(fb.data.get(), pixel.data(), frame_bytes);
    fb.size = frame_bytes;
    fb.info.width = frame_w;
    fb.info.height = frame_h;
    fb.info.stride = frame_w * 3;
    fb.info.format = PixelFormat::kBGR888;
    fb.info.cpu_va = fb.data.get();
    // 保持 pixel 存活供 info.cpu_va 引用（两者指针同地址）
    (void)pixel;

    // ---- RKNN 引擎 ----
    RKNNEngine engine;
    RKNNEngine::Params ep;
    ep.model_path = model_path;
    ep.core_mask = 0; // AUTO
    ep.pass_through = false; // 走兼容 I/O（FP16/INT8 通用）
    std::string err;
    if (!engine.init(ep, &err)) {
        std::fprintf(stderr, "RKNN init 失败: %s\n", err.c_str()); return 3;
    }
    const auto& info = engine.info();
    std::fprintf(stderr, "model: in=%dx%d type=%d fmt=%d outputs=%u\n",
                 info.input_width, info.input_height, info.input_type, info.input_fmt, info.n_outputs);
    for (uint32_t i = 0; i < info.n_outputs && i < info.outputs.size(); ++i) {
        const auto& output = info.outputs[i];
        std::fprintf(stderr, "tensor[%u]: dims=", i);
        for (uint32_t dim : output.dims) std::fprintf(stderr, "%u,", dim);
        std::fprintf(stderr, " type=%d fmt=%d size=%u n_elems=%u quant=scale:%g zp:%d\n",
                     output.type, output.fmt, output.size, output.n_elems,
                     output.scale, output.zp);
    }

    ModelAdapter adapter;
    ModelAdapterConfig adapter_config;
    adapter_config.color_order = ColorOrder::kBgr;
    adapter_config.conf_thres = conf;
    adapter_config.iou_thres = iou;
    if (!adapter.analyze(info, adapter_config, &err)) {
        std::fprintf(stderr, "adapter analyze fail: %s\n", err.c_str());
        return 3;
    }
    auto decoder = adapter.create_decoder(&err);
    if (!decoder) {
        std::fprintf(stderr, "adapter decoder fail: %s\n", err.c_str());
        return 3;
    }
    decoder->set_frame(frame_w, frame_h);

    // ---- 预处理（CPU fallback，对齐 RGA ROI 中心缩放几何）----
    Preprocess pre;
    PreprocessConfig pc;
    pc.detect_size = {info.input_width, info.input_height};
    pc.input_type = info.input_type;
    pc.backend = PreprocessBackend::kCpuFallback;
    pc.center_crop = true;
    pc.color_order = 0; // BGR
    pc.crop_x = roi_x; pc.crop_y = roi_y; pc.crop_width = roi_w; pc.crop_height = roi_h;
    if (!pre.init(pc, &err)) { std::fprintf(stderr, "preprocess init fail: %s\n", err.c_str()); return 3; }
    PreprocessedFrame pf;
    if (!pre.process(fb, &pf, &err)) { std::fprintf(stderr, "preprocess fail: %s\n", err.c_str()); return 3; }

    // ---- 推理 ----
    if (!engine.set_input(pf.tensor_data, pf.tensor_size, &err)) {
        std::fprintf(stderr, "set_input fail: %s\n", err.c_str()); return 3;
    }
    if (!engine.run(&err)) { std::fprintf(stderr, "run fail: %s\n", err.c_str()); return 3; }
    // 原生输出
    std::vector<std::vector<uint8_t>> raw_out(info.n_outputs);
    std::vector<void*> optr(info.n_outputs);
    std::vector<size_t> osz(info.n_outputs);
    for (uint32_t i = 0; i < info.n_outputs; ++i) {
        raw_out[i].resize(info.outputs[i].size);
        optr[i] = raw_out[i].data();
        osz[i] = info.outputs[i].size;
    }
    if (!engine.get_raw_outputs(optr.data(), osz.data(), &err)) {
        std::fprintf(stderr, "get_raw_outputs fail: %s\n", err.c_str()); return 3;
    }
    std::fprintf(stderr, "DEBUG in_size=%zu out0_size=%zu\n",
                 pf.tensor_size, osz[0]);
    // 打印输入张量统计（判定 preprocess 是否喂入有效图像；FP16 需转 half 读出）
    {
        size_t ne = info.input_size / 2;
        const uint16_t* ip = (const uint16_t*)pf.tensor_data;
        auto ih=[&](uint16_t h)->float{const uint32_t s=(static_cast<uint32_t>(h)&0x8000u)<<16;const uint32_t e=(h>>10)&0x1Fu;const uint32_t m=h&0x3FFu;uint32_t b;if(e==0){if(m==0)b=s;else{uint32_t ee=127-15+1,mm=m;while((mm&0x400u)==0){mm<<=1;ee--;}mm&=0x3FFu;b=s|((ee+15)<<23)|(mm<<13);}}else if(e==0x1Fu)b=s|0x7F800000u|(m<<13);else b=s|((e-15+127)<<23)|(m<<13);float f;memcpy(&f,&b,4);return f;};
        float mn=1e30f,mx=-1e30f,sm=0; for(size_t i=0;i<ne;++i){float f=ih(ip[i]);if(f<mn)mn=f;if(f>mx)mx=f;sm+=f;}
        std::fprintf(stderr,"DEBUG input fp16 %zu elems min=%f max=%f avg=%f (expect avg~0.4-0.6 为有效图像归一化)\n",ne,mn,mx,sm/ne);
    }
    if (info.n_outputs == 1 && !json) {
        // 单输出 FP16 [1,C,M]：打印全张量浮点统计 + 两种布局最大类分数
        const auto* oi = &info.outputs[0];
        uint32_t C = oi->dims.size()>=3 ? oi->dims[1] : 0;
        uint32_t M = 1; for (size_t d=2;d<oi->dims.size();++d) M*=oi->dims[d];
        float mn=1e30f,mx=-1e30f,sum=0; size_t nn=0;
        auto rv=[&](size_t idx)->float{ // read fp16 at idx (小端 half, 用与 read_elem 一致的转换)
            const uint16_t h=((const uint16_t*)raw_out[0].data())[idx];
            const uint32_t sign=(static_cast<uint32_t>(h)&0x8000u)<<16;
            const uint32_t exp=(h>>10)&0x1Fu; const uint32_t man=h&0x3FFu;
            uint32_t bits;
            if(exp==0){ if(man==0) bits=sign; else { uint32_t e=127-15+1,m=man; while((m&0x400u)==0){m<<=1;e--;} m&=0x3FFu; bits=sign|((e+15)<<23)|(m<<13);} }
            else if(exp==0x1Fu) bits=sign|0x7F800000u|(man<<13);
            else bits=sign|((exp-15+127)<<23)|(man<<13);
            float f; memcpy(&f,&bits,4); return f;
        };
        for (size_t i=0;i<raw_out[0].size()/2;++i){float f=rv(i);if(f<mn)mn=f;if(f>mx)mx=f;sum+=f;nn++;}
        std::fprintf(stderr,"DEBUG fp16 %zu elems min=%f max=%f avg=%f  C=%u M=%u\n", nn,mn,mx,sum/nn,C,M);
        // 按 C×M 布局（process_single 的读法）打印各 channel 均值
        std::fprintf(stderr,"-- C*M layout: per-channel avg (C0..C83) --\n");
        for (uint32_t ch=0;ch<C;++ch){ double s2=0; for(uint32_t a=0;a<M;++a) s2+=rv((size_t)ch*M+a); std::fprintf(stderr,"ch%u:%.2f ", ch,(float)(s2/M)); if((ch+1)%8==0) std::fprintf(stderr,"\n"); }
        std::fprintf(stderr,"\n-- M*C layout: per-anchor avg ch-average of first8 anchors, and anchor0 class top --\n");
        for (uint32_t a=0;a<8;++a){ double s2=0; for(uint32_t ch=0;ch<C;++ch) s2+=rv((size_t)a*C+ch); std::fprintf(stderr,"anchor%u avg=%.2f\n",a,(float)(s2/C)); }
        // 看 M*C 下 bus.jpg 里已知 person 是否命中（先看 anchor0-20 的 cls4=person logit: ch=8 in M*C）
        std::fprintf(stderr,"-- M*C anchor0-40 cls(person=0) [ch=4+?]: --\n");
        for (uint32_t a=0;a<40;++a){ float v=rv((size_t)a*C+64/*cls0? no person index*/); float v2=rv((size_t)a*C+4+0); std::fprintf(stderr,"a%u:cls0ch use=%f@64 clsperson0@[cbias4+0]=%f\n",a,v,v2);}
        // 找 M*C 下最大 cls
        {
            float b=-1e30f; int ba=0,bc=0; for(uint32_t a=0;a<M;++a)for(uint32_t ch2=4;ch2<C;++ch2){float f=rv((size_t)a*C+ch2); if(f>b){b=f;ba=a;bc=ch2;}}
            std::fprintf(stderr,"M*C max cls anchor=%d ch=%d val=%.3f  -> (x=%f y=%f)\n",ba,bc,b,
               rv((size_t)ba*C+0),rv((size_t)ba*C+1));
        }
        // 找 top5 跨 channel 最大（layout C*M）
        for (int k=0;k<8;++k){ float b=-1e30f; size_t bidx=0; for (size_t i=0;i<raw_out[0].size()/2;++i){float f=rv(i);if(f>b){b=f;bidx=i;}} std::fprintf(stderr,"top%d idx=%zu val=%f (ch=%zu anchor=%zu)\n",k,bidx,b,bidx/M,bidx%M);
            ((uint16_t*)raw_out[0].data())[bidx]=0; }
    }

    // ---- 解码（ModelAdapter -> Decoder -> DecodeNMS）----
    std::vector<DetectionBox> dets;
    if (!decoder->process(info, optr.data(), &dets, &err)) {
        std::fprintf(stderr, "decode fail: %s\n", err.c_str()); return 3;
    }

    // ---- 真实 DetectionResult -> TargetSelector ----
    aim::TargetSelector selector;
    aim::TargetSelectorConfig selector_config;
    selector_config.roi_w = frame_w;
    selector_config.roi_h = frame_h;
    selector_config.confidence = conf;
    selector_config.fov_range = 1.0f;
    selector_config.aim_ratio_x = 0.5f;
    selector_config.aim_ratio_y = 0.2f;
    const auto selected = selector.select(dets, selector_config, 1);

    // ---- 输出 ----
    std::printf("MODEL_ID=jwdl_sjzv11\n");
    std::printf("RKNN_INFERENCE=PASS OUTPUT_TENSORS=%u\n", info.n_outputs);
    std::printf("DECODE_NMS=PASS DETECTIONS=%zu CANDIDATES=%llu\n", dets.size(),
                static_cast<unsigned long long>(decoder->stats().candidates.load()));
    for (const auto& d : dets) print_det(d, json);
    std::fprintf(stderr, "TargetSelector: candidate_count=%zu selected=%d reason=%d target_id=%d\n",
                 dets.size(), selected.valid ? 1 : 0, static_cast<int>(selected.reason),
                 selected.target_id);
    if (selected.valid) {
        std::fprintf(stderr, "TargetSelector selected DetectionResult: class_id=%d score=%.4f box=[%.2f,%.2f,%.2f,%.2f]\n",
                     selected.box.class_id, selected.box.score, selected.box.x1, selected.box.y1,
                     selected.box.x2, selected.box.y2);
    }
    return 0;
}