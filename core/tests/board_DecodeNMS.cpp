// DecodeNMS.cpp — 检测解码 + NMS 实现（阶段 A-6）
//
// 对齐基准：Python Demo ttbox/inference/decode.py::decode_outputs()
//   - 单输出 (1,C,M)：前 4 通道为 xywh 绝对坐标（模型输入空间），其余为类别分数
//   - C-4==81 时带 objectness：score = objectness * max(class_probs)
//   - conf 过滤 → classwise NMS（iou_thres）→ 坐标缩放回原图
/*
 * TTBOX 文件说明
 *
 * 文件：DecodeNMS.cpp
 *
 * 作用：
 *   把 AI 模型的原始输出解析成目标框（DetectionBox）。
 *   包括 DFL 解码、NMS 去重、几何过滤。
 *
 * 小白理解：
 *   AI 模型输出的不是"这里有个敌人"，而是一堆数字。
 *   这个文件负责把这堆数字翻译成：目标框位置、置信度、类别。
 *   NMS 负责去掉重复检测，几何过滤去掉不合理的目标。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "rknn/DecodeNMS.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>

#include "common/Logger.hpp"

namespace ttbox::core {

namespace {

constexpr uint32_t kTypeFloat32 = 0;  // RKNN_TENSOR_FLOAT32
constexpr uint32_t kTypeFloat16 = 1;  // RKNN_TENSOR_FLOAT16
constexpr uint32_t kTypeInt8 = 2;     // RKNN_TENSOR_INT8
constexpr uint32_t kTypeUint8 = 3;    // RKNN_TENSOR_UINT8
constexpr uint32_t kTypeInt16 = 4;    // RKNN_TENSOR_INT16

}  // namespace

// IEEE half -> float（纯位运算，跨平台；正确处理 denormal/Inf/NaN）
float DecodeNMS::half_to_float(uint16_t h) {
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t f;
    if (exp == 0) {
        if (man == 0) {
            f = sign;  // ±0
        } else {
            // denormal：规格化
            uint32_t e = 127 - 15 + 1;
            uint32_t m = man;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                --e;
            }
            f = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 0x1Fu) {
        f = sign | 0x7F800000u | (man << 13);  // Inf/NaN
    } else {
        f = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float out;
    std::memcpy(&out, &f, sizeof(out));
    return out;
}

// 读输出 tensor 第 idx 个元素并转为 float（按原生类型反量化）
float DecodeNMS::read_elem(const RknnOutputInfo& oi, const uint8_t* buf, size_t idx) {
    switch (oi.type) {
        case kTypeFloat32:
            return *reinterpret_cast<const float*>(buf + idx * sizeof(float));
        case kTypeFloat16: {
            uint16_t h;
            std::memcpy(&h, buf + idx * sizeof(uint16_t), sizeof(h));
            return half_to_float(h);
        }
        case kTypeInt8:
            return (static_cast<float>(*reinterpret_cast<const int8_t*>(buf + idx)) -
                    static_cast<float>(oi.zp)) * oi.scale;
        case kTypeUint8:
            return (static_cast<float>(*reinterpret_cast<const uint8_t*>(buf + idx)) -
                    static_cast<float>(oi.zp)) * oi.scale;
        case kTypeInt16:
            return (static_cast<float>(*reinterpret_cast<const int16_t*>(buf + idx)) -
                    static_cast<float>(oi.zp)) * oi.scale;
        default:
            return 0.0f;
    }
}

bool DecodeNMS::configure(const DecodeParams& params, std::string* error) {
    if (params.conf_thres <= 0.0f || params.iou_thres <= 0.0f ||
        params.input_w == 0 || params.input_h == 0) {
        if (error) *error = "DecodeNMS 参数无效（conf/iou/输入尺寸）";
        return false;
    }
    params_ = params;
    return true;
}

// 经典 NMS（与 Python nms_boxes 对齐：score 降序，IoU<=thres 保留）
void DecodeNMS::nms_boxes(const std::vector<DetectionBox>& cands,
                          std::vector<int>* keep) const {
    keep->clear();
    const size_t n = cands.size();
    if (n == 0) return;

    // 按 score 降序
    std::vector<int> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return cands[a].score > cands[b].score;
    });

    std::vector<bool> removed(n, false);
    for (size_t oi = 0; oi < n; ++oi) {
        const int i = order[oi];
        if (removed[i]) continue;
        keep->push_back(i);
        const DetectionBox& bi = cands[i];
        const float bi_area = std::max(0.0f, bi.x2 - bi.x1) * std::max(0.0f, bi.y2 - bi.y1);
        for (size_t oj = oi + 1; oj < n; ++oj) {
            const int j = order[oj];
            if (removed[j]) continue;
            const DetectionBox& bj = cands[j];
            const float xx1 = std::max(bi.x1, bj.x1);
            const float yy1 = std::max(bi.y1, bj.y1);
            const float xx2 = std::min(bi.x2, bj.x2);
            const float yy2 = std::min(bi.y2, bj.y2);
            const float w = std::max(0.0f, xx2 - xx1);
            const float h = std::max(0.0f, yy2 - yy1);
            const float inter = w * h;
            const float bj_area = std::max(0.0f, bj.x2 - bj.x1) * std::max(0.0f, bj.y2 - bj.y1);
            const float union_area = bi_area + bj_area - inter;
            if (union_area > 1e-9f && inter / union_area > params_.iou_thres) {
                removed[j] = true;
            }
        }
    }
}

bool DecodeNMS::process(const RknnModelInfo& info,
                        const void* const* out_bufs,
                        std::vector<DetectionBox>* detections,
                        std::string* error) {
    if (detections == nullptr || out_bufs == nullptr) {
        if (error) *error = "DecodeNMS 参数无效";
        return false;
    }
    detections->clear();
    if (info.n_outputs == 0 || info.outputs.empty()) {
        if (error) *error = "模型无输出信息";
        return false;
    }
    // 按实际输出数量分发：单输出（yolo261n / v26m e2e）或多输出 DFL（黄瓦模型）
    if (info.n_outputs == 1) {
        // 单输出二义性：3D [1,N,F] 且 F 小（每行固定字段）→ 端到端已解码检测；
        // 否则按 (1,C,M) 传统格式（前 4 通道 xywh + 类别）
        const auto& oi = info.outputs[0];
        const bool e2e = (oi.dims.size() == 3 && oi.dims[2] > 0 &&
                          oi.dims[2] <= 16 && oi.dims[1] > 0);
        if (e2e) {
            return process_e2e(info, out_bufs, detections, error);
        }
        return process_single(info, out_bufs, detections, error);
    }
    // 多输出 reg-bins DFL：必须先于“偶数输出”的通用成对格式判断。
    // 当前 sjz-XCSH 是 6 输出：3 个尺度 × (reg/cls)，每组 reg=64、cls=7。
    if (info.n_outputs >= 6 && info.n_outputs % 2 == 0) {
        bool pair_reg_cls = true;
        for (uint32_t i = 0; i + 1 < info.n_outputs; i += 2) {
            const auto& ro = info.outputs[i];
            const auto& co = info.outputs[i + 1];
            if (ro.dims.size() != 4 || co.dims.size() != 4 ||
                ro.dims[1] < 32 || ro.dims[1] % 4 != 0 || co.dims[1] == 0 ||
                ro.dims[2] != co.dims[2] || ro.dims[3] != co.dims[3]) {
                pair_reg_cls = false;
                break;
            }
        }
        if (pair_reg_cls) return process_dfl_pair_dist(info, out_bufs, detections, error);
    }
    if (info.n_outputs >= 2 && info.n_outputs % 2 == 0) {
        return process_dfl(info, out_bufs, detections, error);
    }
    if (error) *error = "不支持该输出数量";
    return false;
}

bool DecodeNMS::process_single(const RknnModelInfo& info,
                               const void* const* out_bufs,
                               std::vector<DetectionBox>* detections,
                               std::string* error) {

    const auto t_begin = std::chrono::steady_clock::now();
    const RknnOutputInfo& oi = info.outputs[0];
    const uint8_t* buf = static_cast<const uint8_t*>(out_bufs[0]);

    // 张量布局：NCHW (1,C,M) 或 (1,C,H,W)。C = dims[1]，M = H*W*N。
    uint32_t n_channels = 0;
    uint32_t n_anchors = 0;
    if (oi.dims.size() >= 3) {
        n_channels = oi.dims[1];
        n_anchors = 1;
        for (size_t d = 2; d < oi.dims.size(); ++d) n_anchors *= oi.dims[d];
    }
    if (n_channels < 5 || n_anchors == 0) {
        if (error) *error = "输出形状无法解析（需要 (1,C,M)，C>=5）";
        return false;
    }

    const uint32_t n_classes = n_channels - 4;
    const bool has_objectness = (n_classes == 81);  // 80 类 + 1 objectness

    // ---- 1. 候选解码：每 anchor 读 4 坐标 + 类别分数，conf 过滤 ----
    cands_.clear();
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t a = 0; a < n_anchors; ++a) {
            // 类别分数：max + argmax（跳过 objectness 通道的索引偏移）
            const uint32_t cls_begin = has_objectness ? 5u : 4u;
            float best = -1.0f;
            int best_id = 0;
            for (uint32_t c = cls_begin; c < n_channels; ++c) {
                const float s = read_elem(oi, buf, static_cast<size_t>(c) * n_anchors + a);
                if (s > best) {
                    best = s;
                    best_id = static_cast<int>(c - cls_begin);
                }
            }
            if (has_objectness) {
                const float obj = read_elem(oi, buf, static_cast<size_t>(4) * n_anchors + a);
                best *= obj;
            }
            if (best < params_.conf_thres) continue;

            const float x = read_elem(oi, buf, static_cast<size_t>(0) * n_anchors + a);
            const float y = read_elem(oi, buf, static_cast<size_t>(1) * n_anchors + a);
            const float w = read_elem(oi, buf, static_cast<size_t>(2) * n_anchors + a);
            const float h = read_elem(oi, buf, static_cast<size_t>(3) * n_anchors + a);
            DetectionBox d;
            d.x1 = x - w / 2.0f;
            d.y1 = y - h / 2.0f;
            d.x2 = x + w / 2.0f;
            d.y2 = y + h / 2.0f;
            d.score = best;
            d.class_id = best_id;
            cands_.push_back(d);
        }
        stats_.decode.add(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }

    // ---- 2. classwise NMS ----
    kept_.clear();
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (params_.classwise) {
            // 按类别分组后分别 NMS（与 Python run_classwise_nms 对齐）
            std::vector<int> classes;
            for (const auto& c : cands_) {
                if (std::find(classes.begin(), classes.end(), c.class_id) == classes.end())
                    classes.push_back(c.class_id);
            }
            std::sort(classes.begin(), classes.end());
            for (const int cls : classes) {
                cands_cls_.clear();
                for (const auto& c : cands_) {
                    if (c.class_id == cls) cands_cls_.push_back(c);
                }
                nms_boxes(cands_cls_, &keep_idx_);
                for (const int k : keep_idx_) kept_.push_back(cands_cls_[k]);
            }
        } else {
            nms_boxes(cands_, &keep_idx_);
            for (const int k : keep_idx_) kept_.push_back(cands_[k]);
        }
        stats_.nms.add(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }

    // ---- 3. 坐标映射回原图（ROI 偏移；线性缩放；NMS 已在模型空间完成）----
    map_coords(&kept_);

    apply_post_filter(&kept_);
    apply_fov_filter(&kept_);

    *detections = kept_;

    stats_.candidates.fetch_add(cands_.size());
    stats_.detections.fetch_add(kept_.size());
    stats_.frames.fetch_add(1);
    stats_.total.add(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_begin).count());
    return true;
}

// ---------------------------------------------------------------------------
// 多输出 DFL（黄瓦模型：3 尺度 × [box(1,1,4,M), cls(1,C,H,W)]，全部 INT8）
//
// decode（标准 YOLOv8）：
//   每尺度 stride_x = input_w / grid_w，stride_y = input_h / grid_h
//   cls → sigmoid → max/argmax（每类独立，BCE）
//   box 4 通道 = [dl, dt, dr, db]（DFL 已解码的距离，单位=网格）
//   cx = (col+0.5)*stride_x, cy = (row+0.5)*stride_y
//   x1 = cx - dl*stride_x, y1 = cy - dt*stride_y, x2 = cx + dr*stride_x, y2 = cy + db*stride_y
// ---------------------------------------------------------------------------
bool DecodeNMS::process_dfl(const RknnModelInfo& info,
                            const void* const* out_bufs,
                            std::vector<DetectionBox>* detections,
                            std::string* error) {
    if (params_.input_w == 0 || params_.input_h == 0) {
        if (error) *error = "DFL decode 需要 input 尺寸";
        return false;
    }

    const auto t_begin = std::chrono::steady_clock::now();
    cands_.clear();

    const auto t_decode0 = std::chrono::steady_clock::now();
    for (uint32_t p = 0; p + 1 < info.n_outputs; p += 2) {
        const RknnOutputInfo& bo = info.outputs[p];      // box
        const RknnOutputInfo& co = info.outputs[p + 1];  // cls
        const uint8_t* bbuf = static_cast<const uint8_t*>(out_bufs[p]);
        const uint8_t* cbuf = static_cast<const uint8_t*>(out_bufs[p + 1]);

        // box：期望 4D [1,1,4,M] 或 3D [1,4,M]
        uint32_t n_dist = 0, n_anchors_box = 0;
        if (bo.dims.size() == 4) {
            n_dist = bo.dims[2];
            n_anchors_box = bo.dims[3];
        } else if (bo.dims.size() == 3) {
            n_dist = bo.dims[1];
            n_anchors_box = bo.dims[2];
        }
        if (n_dist != 4 || n_anchors_box == 0) {
            if (error) *error = "box 输出格式无法解析";
            return false;
        }
        // cls：期望 [1,C,H,W]
        uint32_t n_classes = 0, grid_h = 0, grid_w = 0;
        if (co.dims.size() == 4) {
            n_classes = co.dims[1];
            grid_h = co.dims[2];
            grid_w = co.dims[3];
        }
        if (n_classes == 0 || grid_h == 0 || grid_w == 0) {
            if (error) *error = "cls 输出格式无法解析";
            return false;
        }
        const uint32_t n_anchors = grid_h * grid_w;
        if (n_anchors != n_anchors_box) {
            if (error) *error = "box/cls 锚点数不一致";
            return false;
        }
        const float stride_x = static_cast<float>(params_.input_w) / static_cast<float>(grid_w);
        const float stride_y = static_cast<float>(params_.input_h) / static_cast<float>(grid_h);

        for (uint32_t a = 0; a < n_anchors; ++a) {
            // cls：sigmoid + max/argmax
            float best = -1.0f;
            int best_id = 0;
            for (uint32_t c = 0; c < n_classes; ++c) {
                const float v = read_elem(co, cbuf, static_cast<size_t>(c) * n_anchors + a);
                const float s = 1.0f / (1.0f + std::exp(-v));
                if (s > best) {
                    best = s;
                    best_id = static_cast<int>(c);
                }
            }
            if (best < params_.conf_thres) continue;

            const uint32_t row = a / grid_w;
            const uint32_t col = a % grid_w;
            const float dl = read_elem(bo, bbuf, static_cast<size_t>(0) * n_anchors + a);
            const float dt = read_elem(bo, bbuf, static_cast<size_t>(1) * n_anchors + a);
            const float dr = read_elem(bo, bbuf, static_cast<size_t>(2) * n_anchors + a);
            const float db = read_elem(bo, bbuf, static_cast<size_t>(3) * n_anchors + a);
            const float cx = (static_cast<float>(col) + 0.5f) * stride_x;
            const float cy = (static_cast<float>(row) + 0.5f) * stride_y;
            DetectionBox d;
            d.x1 = cx - dl * stride_x;
            d.y1 = cy - dt * stride_y;
            d.x2 = cx + dr * stride_x;
            d.y2 = cy + db * stride_y;
            d.score = best;
            d.class_id = best_id;
            cands_.push_back(d);
        }
    }
    stats_.decode.add(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_decode0).count());

    // ---- classwise NMS ----
    kept_.clear();
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (params_.classwise) {
            std::vector<int> classes;
            for (const auto& c : cands_) {
                if (std::find(classes.begin(), classes.end(), c.class_id) == classes.end())
                    classes.push_back(c.class_id);
            }
            std::sort(classes.begin(), classes.end());
            for (const int cls : classes) {
                cands_cls_.clear();
                for (const auto& c : cands_) {
                    if (c.class_id == cls) cands_cls_.push_back(c);
                }
                nms_boxes(cands_cls_, &keep_idx_);
                for (const int k : keep_idx_) kept_.push_back(cands_cls_[k]);
            }
        } else {
            nms_boxes(cands_, &keep_idx_);
            for (const int k : keep_idx_) kept_.push_back(cands_[k]);
        }
        stats_.nms.add(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }

    // ---- 坐标映射回原图（ROI 偏移）----
    map_coords(&kept_);

    apply_post_filter(&kept_);
    apply_fov_filter(&kept_);

    *detections = kept_;

    stats_.candidates.fetch_add(cands_.size());
    stats_.detections.fetch_add(kept_.size());
    stats_.frames.fetch_add(1);
    stats_.total.add(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_begin).count());
    return true;
}

// ---------------------------------------------------------------------------
// 多输出 reg-bins DFL（大腕256：3 尺度 × [reg(1,64,H,W), cls(1,C,H,W), aux(1,1,H,W)]）
//
// decode：
//   reg 通道 = 4 边 × reg_max bins（64 = 4×16，边主序：l 的 16 bins、t 的 16 bins...）
//   每边 DFL softmax 加权 → 距离（单位网格）
//   cls → sigmoid → max/argmax；aux 若值域 (0,1) 且非恒定，作为 objectness 乘入
// ---------------------------------------------------------------------------

bool DecodeNMS::process_dfl_pair_dist(const RknnModelInfo& info, const void* const* out_bufs,
                                      std::vector<DetectionBox>* detections, std::string* error) {
    if (!detections || params_.input_w == 0 || params_.input_h == 0) { if(error)*error="DFL pair 参数无效"; return false; }
    const auto t_begin = std::chrono::steady_clock::now();
    cands_.clear();
    const auto t_decode0 = std::chrono::steady_clock::now();
    for (uint32_t p=0; p+1<info.n_outputs; p+=2) {
        const auto& ro=info.outputs[p]; const auto& co=info.outputs[p+1];
        if(ro.dims.size()!=4 || ro.dims[1]%4!=0 || co.dims.size()!=4 || ro.dims[2]!=co.dims[2] || ro.dims[3]!=co.dims[3]) { if(error)*error="DFL pair 输出格式无法解析"; return false; }
        const uint32_t bins=ro.dims[1]/4, gh=ro.dims[2], gw=ro.dims[3], n=gh*gw, nc=co.dims[1];
        const auto* rb=static_cast<const uint8_t*>(out_bufs[p]); const auto* cb=static_cast<const uint8_t*>(out_bufs[p+1]);
        for(uint32_t a=0;a<n;++a){ float best=-1; int bi=0; for(uint32_t c=0;c<nc;++c){float v=read_elem(co,cb,(size_t)c*n+a);float q=1.f/(1.f+std::exp(-v));if(q>best){best=q;bi=(int)c;}} if(best<params_.conf_thres)continue; float d[4]{}; for(uint32_t e=0;e<4;++e){float mx=-1e30f,sum=0;for(uint32_t b=0;b<bins;++b)mx=std::max(mx,read_elem(ro,rb,(size_t)e*bins*n+(size_t)b*n+a));for(uint32_t b=0;b<bins;++b)sum+=std::exp(read_elem(ro,rb,(size_t)e*bins*n+(size_t)b*n+a)-mx);for(uint32_t b=0;b<bins;++b)d[e]+=b*std::exp(read_elem(ro,rb,(size_t)e*bins*n+(size_t)b*n+a)-mx)/sum;}float sx=(float)params_.input_w/gw,sy=(float)params_.input_h/gh,cx=(a%gw+.5f)*sx,cy=(a/gw+.5f)*sy;DetectionBox x{cx-d[0]*sx,cy-d[1]*sy,cx+d[2]*sx,cy+d[3]*sy,best,bi};cands_.push_back(x);}
    }
    stats_.decode.add(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-t_decode0).count());
    // 与其它 Decoder 一致：按类别执行 NMS、坐标映射和后过滤。
    stats_.candidates.fetch_add(cands_.size());
    kept_.clear();
    const auto t_nms0 = std::chrono::steady_clock::now();
    if (params_.classwise) {
        std::vector<int> classes;
        for (const auto& c : cands_) if (std::find(classes.begin(), classes.end(), c.class_id) == classes.end()) classes.push_back(c.class_id);
        for (int cls : classes) {
            cands_cls_.clear();
            for (const auto& c : cands_) if (c.class_id == cls) cands_cls_.push_back(c);
            nms_boxes(cands_cls_, &keep_idx_);
            for (int k : keep_idx_) kept_.push_back(cands_cls_[k]);
        }
    } else {
        nms_boxes(cands_, &keep_idx_);
        for (int k : keep_idx_) kept_.push_back(cands_[k]);
    }
    stats_.nms.add(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-t_nms0).count());
    map_coords(&kept_);
    apply_post_filter(&kept_);
    apply_fov_filter(&kept_);
    if (params_.max_detections > 0 && kept_.size() > static_cast<size_t>(params_.max_detections)) kept_.resize(params_.max_detections);
    *detections = kept_;
    stats_.detections.fetch_add(kept_.size());
    stats_.total.add(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-t_begin).count());
    return true;
}
bool DecodeNMS::process_dfl_dist(const RknnModelInfo& info,
                                 const void* const* out_bufs,
                                 std::vector<DetectionBox>* detections,
                                 std::string* error) {
    if (params_.input_w == 0 || params_.input_h == 0) {
        if (error) *error = "DFL dist decode 需要 input 尺寸";
        return false;
    }

    const auto t_begin = std::chrono::steady_clock::now();
    cands_.clear();

    const auto t_decode0 = std::chrono::steady_clock::now();
    for (uint32_t p = 0; p + 2 < info.n_outputs; p += 3) {
        const RknnOutputInfo& ro = info.outputs[p];      // reg
        const RknnOutputInfo& co = info.outputs[p + 1];  // cls
        const RknnOutputInfo& ao = info.outputs[p + 2];  // aux
        const uint8_t* rbuf = static_cast<const uint8_t*>(out_bufs[p]);
        const uint8_t* cbuf = static_cast<const uint8_t*>(out_bufs[p + 1]);
        const uint8_t* abuf = static_cast<const uint8_t*>(out_bufs[p + 2]);

        if (ro.dims.size() != 4 || co.dims.size() != 4 || ao.dims.size() != 4) {
            if (error) *error = "DFL dist 输出需 4D";
            return false;
        }
        const uint32_t reg_ch = ro.dims[1];   // 64
        const uint32_t grid_h = ro.dims[2];
        const uint32_t grid_w = ro.dims[3];
        const uint32_t n_classes = co.dims[1];
        if (reg_ch < 32 || reg_ch % 16 != 0 || grid_h == 0 || grid_w == 0 ||
            n_classes == 0) {
            if (error) *error = "DFL dist 输出格式无法解析";
            return false;
        }
        if (co.dims[2] != grid_h || co.dims[3] != grid_w ||
            ao.dims[2] != grid_h || ao.dims[3] != grid_w) {
            if (error) *error = "DFL dist reg/cls/aux 网格不一致";
            return false;
        }
        const uint32_t n_bins = reg_ch / 4;   // 16
        const uint32_t n_anchors = grid_h * grid_w;
        const float stride_x = static_cast<float>(params_.input_w) / static_cast<float>(grid_w);
        const float stride_y = static_cast<float>(params_.input_h) / static_cast<float>(grid_h);

        // aux 是否可作为 objectness：值域 (0,1) 且非恒定（有目标响应）
        bool aux_obj = false;
        if (ao.dims[1] == 1 && n_anchors > 1) {
            float mn = 1e30f, mx = -1e30f;
            for (uint32_t a = 0; a < n_anchors; ++a) {
                const float v = read_elem(ao, abuf, a);
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            aux_obj = (mn > 0.0f && mx <= 1.0f && (mx - mn) > 1e-3f);
        }

        for (uint32_t a = 0; a < n_anchors; ++a) {
            // cls：sigmoid + max/argmax
            float best = -1.0f;
            int best_id = 0;
            for (uint32_t c = 0; c < n_classes; ++c) {
                const float v = read_elem(co, cbuf, static_cast<size_t>(c) * n_anchors + a);
                const float s = 1.0f / (1.0f + std::exp(-v));
                if (s > best) {
                    best = s;
                    best_id = static_cast<int>(c);
                }
            }
            float score = best;
            if (aux_obj) {
                score *= read_elem(ao, abuf, a);
            }
            if (score < params_.conf_thres) continue;

            // DFL softmax 解码 4 边距离（通道边主序：edge*n_bins + bin）
            float dist[4];
            for (uint32_t e = 0; e < 4; ++e) {
                const size_t base = static_cast<size_t>(e) * n_bins * n_anchors;
                float mx = -1e30f;
                for (uint32_t b = 0; b < n_bins; ++b) {
                    const float v = read_elem(ro, rbuf,
                                              base + static_cast<size_t>(b) * n_anchors + a);
                    if (v > mx) mx = v;
                }
                float sum = 0.0f;
                for (uint32_t b = 0; b < n_bins; ++b) {
                    sum += std::exp(read_elem(ro, rbuf,
                                              base + static_cast<size_t>(b) * n_anchors + a) - mx);
                }
                float d = 0.0f;
                if (sum > 1e-12f) {
                    for (uint32_t b = 0; b < n_bins; ++b) {
                        const float w = std::exp(read_elem(ro, rbuf,
                                              base + static_cast<size_t>(b) * n_anchors + a) - mx) / sum;
                        d += static_cast<float>(b) * w;
                    }
                }
                dist[e] = d;
            }
            const uint32_t row = a / grid_w;
            const uint32_t col = a % grid_w;
            const float cx = (static_cast<float>(col) + 0.5f) * stride_x;
            const float cy = (static_cast<float>(row) + 0.5f) * stride_y;
            DetectionBox d;
            d.x1 = cx - dist[0] * stride_x;
            d.y1 = cy - dist[1] * stride_y;
            d.x2 = cx + dist[2] * stride_x;
            d.y2 = cy + dist[3] * stride_y;
            d.score = score;
            d.class_id = best_id;
            cands_.push_back(d);
        }
    }
    stats_.decode.add(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_decode0).count());

    // ---- classwise NMS ----
    kept_.clear();
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (params_.classwise) {
            std::vector<int> classes;
            for (const auto& c : cands_) {
                if (std::find(classes.begin(), classes.end(), c.class_id) == classes.end())
                    classes.push_back(c.class_id);
            }
            std::sort(classes.begin(), classes.end());
            for (const int cls : classes) {
                cands_cls_.clear();
                for (const auto& c : cands_) {
                    if (c.class_id == cls) cands_cls_.push_back(c);
                }
                nms_boxes(cands_cls_, &keep_idx_);
                for (const int k : keep_idx_) kept_.push_back(cands_cls_[k]);
            }
        } else {
            nms_boxes(cands_, &keep_idx_);
            for (const int k : keep_idx_) kept_.push_back(cands_[k]);
        }
        stats_.nms.add(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }

    // ---- 坐标映射回原图（ROI 偏移）----
    map_coords(&kept_);

    apply_post_filter(&kept_);
    apply_fov_filter(&kept_);

    *detections = kept_;

    stats_.candidates.fetch_add(cands_.size());
    stats_.detections.fetch_add(kept_.size());
    stats_.frames.fetch_add(1);
    stats_.total.add(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_begin).count());
    return true;
}

// ---------------------------------------------------------------------------
// 端到端单输出（v26m 640：模型内已做 TopK 选择，输出 [1,N,F]）
//
//   F>=6 布局: [x1,y1,x2,y2,score,class, ...]（坐标在模型输入空间 0~input_w/h）
//   N 为最大检测数，不足部分以 score=0 填充（自动被 conf 过滤）
//   模型已做选择（内部 TopK/NMS），按 A-8 不再无条件二次 NMS：
//   仅 conf 过滤 + FOV Filter（保留模型语义）
// ---------------------------------------------------------------------------
bool DecodeNMS::process_e2e(const RknnModelInfo& info,
                            const void* const* out_bufs,
                            std::vector<DetectionBox>* detections,
                            std::string* error) {
    const auto t_begin = std::chrono::steady_clock::now();
    const RknnOutputInfo& oi = info.outputs[0];
    const uint8_t* buf = static_cast<const uint8_t*>(out_bufs[0]);

    const uint32_t N = oi.dims[1];  // 最大检测数（如 300）
    const uint32_t F = oi.dims[2];  // 每行字段数（>=6 时使用前 6）
    if (N == 0 || F < 6) {
        if (error) *error = "e2e 输出形状无法解析（需要 [1,N,F]，F>=6）";
        return false;
    }

    // ---- 1. 候选读取：每行 [x1,y1,x2,y2,score,class]，conf 过滤（含 0 填充行）----
    cands_.clear();
    kept_.clear();
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t r = 0; r < N; ++r) {
            const size_t base = static_cast<size_t>(r) * F;
            const float score = read_elem(oi, buf, base + 4);
            if (score < params_.conf_thres) continue;  // score=0 的填充行自动跳过
            DetectionBox d;
            d.x1 = read_elem(oi, buf, base + 0);
            d.y1 = read_elem(oi, buf, base + 1);
            d.x2 = read_elem(oi, buf, base + 2);
            d.y2 = read_elem(oi, buf, base + 3);
            d.score = score;
            d.class_id = static_cast<int>(read_elem(oi, buf, base + 5));
            cands_.push_back(d);
        }
        // A-8：E2E 模型内部已 TopK/NMS → 不再无条件二次 NMS；直接作为保留结果
        if (params_.e2e_skip_nms) {
            kept_ = cands_;
        } else {
            kept_ = cands_;  // 兼容关闭 skip 时语义（不额外 NMS，保持模型输出）
        }
        stats_.decode.add(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }

    // ---- 2. 坐标映射回原图（模型输入空间 → 原图线性缩放 + ROI 偏移）----
    map_coords(&kept_);

    apply_post_filter(&kept_);
    apply_fov_filter(&kept_);

    *detections = kept_;

    stats_.candidates.fetch_add(cands_.size());
    stats_.detections.fetch_add(kept_.size());
    stats_.frames.fetch_add(1);
    stats_.total.add(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_begin).count());
    return true;
}

// ---------------------------------------------------------------------------
// A-7 输出后处理：class_filter（空=全部）过滤 + max_detections（0=不限）截断
// ---------------------------------------------------------------------------

void DecodeNMS::apply_post_filter(std::vector<DetectionBox>* dets) const {
    if (dets == nullptr) return;
    if (!params_.class_filter.empty()) {
        std::vector<DetectionBox> kept;
        kept.reserve(dets->size());
        for (auto& d : *dets) {
            for (const int c : params_.class_filter) {
                if (d.class_id == c) {
                    kept.push_back(d);
                    break;
                }
            }
        }
        *dets = std::move(kept);
    }
    if (params_.max_detections > 0 &&
        dets->size() > static_cast<size_t>(params_.max_detections)) {
        std::partial_sort(dets->begin(), dets->begin() + params_.max_detections, dets->end(),
                          [](const DetectionBox& a, const DetectionBox& b) {
                              return a.score > b.score;
                          });
        dets->resize(static_cast<size_t>(params_.max_detections));
    }
}

// ---------------------------------------------------------------------------
// A-8 坐标映射：模型输入空间 → 原图（ROI 偏移）
//
//   RGA 已把屏幕 ROI(roi_x,roi_y,roi_w,roi_h) 裁剪后缩放到模型输入 input_w/h。
//   因此：模型坐标 x → ROI 空间 x*(roi_w/input_w) → 原图 +roi_x
//   ROI 未启用（roi_w/h==0）时等价于全帧映射（frame_w/input_w）。
// ---------------------------------------------------------------------------

void DecodeNMS::map_coords(std::vector<DetectionBox>* dets) const {
    if (dets == nullptr || params_.input_w == 0 || params_.input_h == 0) return;
    if (params_.roi_w == 0 || params_.roi_h == 0) {
        // 未启用 ROI：全帧线性缩放
        const float sx = params_.frame_w > 0
                             ? static_cast<float>(params_.frame_w) / static_cast<float>(params_.input_w)
                             : 1.0f;
        const float sy = params_.frame_h > 0
                             ? static_cast<float>(params_.frame_h) / static_cast<float>(params_.input_h)
                             : 1.0f;
        if (sx != 1.0f || sy != 1.0f) {
            for (auto& d : *dets) {
                d.x1 *= sx; d.x2 *= sx;
                d.y1 *= sy; d.y2 *= sy;
            }
        }
        return;
    }
    // ROI 启用：模型空间 → ROI 空间 → 原图（加偏移）
    const float sx = static_cast<float>(params_.roi_w) / static_cast<float>(params_.input_w);
    const float sy = static_cast<float>(params_.roi_h) / static_cast<float>(params_.input_h);
    for (auto& d : *dets) {
        d.x1 = d.x1 * sx + static_cast<float>(params_.roi_x);
        d.x2 = d.x2 * sx + static_cast<float>(params_.roi_x);
        d.y1 = d.y1 * sy + static_cast<float>(params_.roi_y);
        d.y2 = d.y2 * sy + static_cast<float>(params_.roi_y);
    }
}

// ---------------------------------------------------------------------------
// A-8 FOV Filter：NMS 之后，按检测框中心是否落入 FOV 区域过滤
//   circle: 中心到 FOV 中心归一化距离 ≤ radius
//   rect:   |dx| ≤ radius && |dy| ≤ radius（radius=半宽/半高）
// ---------------------------------------------------------------------------

void DecodeNMS::apply_fov_filter(std::vector<DetectionBox>* dets) const {
    if (dets == nullptr || !params_.fov_enabled) return;
    const float fw = params_.frame_w > 0 ? static_cast<float>(params_.frame_w) : 1.0f;
    const float fh = params_.frame_h > 0 ? static_cast<float>(params_.frame_h) : 1.0f;
    const float cx_px = params_.fov_center_x * fw;
    const float cy_px = params_.fov_center_y * fh;
    // radius 归一化基准：circle/rect 均相对短边（各向同性视觉语义）
    const float r_px = params_.fov_radius * std::min(fw, fh);

    std::vector<DetectionBox> kept;
    kept.reserve(dets->size());
    for (const auto& d : *dets) {
        const float bx = (d.x1 + d.x2) * 0.5f;
        const float by = (d.y1 + d.y2) * 0.5f;
        const float dx = bx - cx_px;
        const float dy = by - cy_px;
        bool inside = false;
        if (params_.fov_shape == FovShape::kCircle) {
            inside = (dx * dx + dy * dy) <= (r_px * r_px);
        } else {
            inside = (std::fabs(dx) <= r_px) && (std::fabs(dy) <= r_px);
        }
        if (inside) kept.push_back(d);
    }
    *dets = std::move(kept);
}

}  // namespace ttbox::core
