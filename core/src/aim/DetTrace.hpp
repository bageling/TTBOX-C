// DetTrace.hpp — 逐帧「原始检测框序列」采集器（V1.0.46，自测自动化基础设施）
//
// 为什么单独一个类（不塞进 PidTrace）：
//   1. 数据量差一个量级 —— PidTrace 每帧 26 个标量；本文件每帧带**全帧检测框**
//      （最多 8 框 × 5 字段 = 40 个）。塞进 PidTrace 会让它的内存/带宽翻数倍。
//   2. 用途不同 —— PidTrace 回答「控制链算出了什么」；本文件回答「目标当时在哪」。
//      离线回放需要的是**能重放目标运动**的那一半，缺了它就无法在 PC 上
//      复现板端的手感问题（历史遗留：板端 Python 录制器是 10Hz IPC 轮询，
//      控制链 144fps ⇒ 90% 帧永久丢失，且砍掉了 x1/x2，横向几何不全）。
//   3. 开关独立 —— 只想看控制链时不付这个代价。
//
// 记录内容（CSV，一行一帧）：
//   timestamp_us, frame_number, frame_w, frame_h, dt_ms,
//   target_id, box_source, sel_x1, sel_y1, sel_x2, sel_y2, sel_cls, sel_score,
//   aim_x1, aim_y1, aim_x2, aim_y2,           // 控制链实际用的框（可能是冻结框）
//   tx, ty, ref_x, ref_y,                      // 落点与参考点
//   smooth_x, smooth_y, vel_x, vel_y,          // tracker 平滑位置与速度（Fitts 前馈输入源）
//   ctrl_x, ctrl_y,                            // 进控制器的误差（预测后）
//   move_x, move_y,                            // 最终注入 count
//   nbox, cls0,y10,y20,x10,x20,sc0, ...        // 全帧检测框（按 x1 升序，最多 8 个）
//
// ★ box_source 标记（回放时必须知道用哪个框复刻）：
//   0 = selected.box（无冻结）
//   1 = frozen_rect（V1.0.10 腿被裁切时冻结上一帧可看全的框）
//   2 = selected.box 但走了 clip 外推（框底被裁剪区截断）
//   三者的落点算法不同，混用会让回放与板端不一致。
//
// 写盘：缓冲 512 行批量 fwrite（不是每帧 fflush）—— 逐帧 fflush 在 8 框数据量下
// 会把 syscalls 放大一个量级反过来影响控制线程节奏（观测行为扰动被测系统）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ttbox::core::aim {

// 单帧最多记录的检测框数（照抄历史板端录制器 recoil-rec-board.py 的 DET_MAX=8，
// 超过部分丢弃但**在 nbox 里记真实总数**，回放侧能知道被截断）。
static const int kDetTraceMaxBoxes = 8;
static const int kDetTraceFlushRows = 512;

class DetTrace {
public:
    struct Entry {
        uint64_t timestamp_us = 0;
        uint64_t frame_number = 0;
        int frame_w = 0;
        int frame_h = 0;
        float dt_ms = 0.0f;
        int target_id = -1;
        int box_source = 0;
        float sel_x1 = 0.0f, sel_y1 = 0.0f, sel_x2 = 0.0f, sel_y2 = 0.0f;
        int sel_cls = -1;
        float sel_score = 0.0f;
        float aim_x1 = 0.0f, aim_y1 = 0.0f, aim_x2 = 0.0f, aim_y2 = 0.0f;
        float tx = 0.0f, ty = 0.0f;
        float ref_x = 0.0f, ref_y = 0.0f;
        float smooth_x = 0.0f, smooth_y = 0.0f;
        float vel_x = 0.0f, vel_y = 0.0f;
        float ctrl_x = 0.0f, ctrl_y = 0.0f;
        int32_t move_x = 0, move_y = 0;
        // 全帧检测框（原始模型输出，不做任何平滑/关联）
        int nbox = 0;  // 真实框数（可能 > kDetTraceMaxBoxes）
        float box[8][5] = {{0}};  // [cls, x1, y1, x2, y2] —— score 另存
        float score[8] = {0};
    };

    DetTrace() = default;
    ~DetTrace() { close(); }

    bool open(const std::string& path = "") {
        close();
        const std::string p = path.empty() ? "/tmp/det_trace.csv" : path;
        fp_ = std::fopen(p.c_str(), "w");
        if (!fp_) return false;
        buf_.reserve(kDetTraceFlushRows * 512);
        enabled_ = true;
        std::fprintf(fp_,
            "timestamp_us,frame_number,frame_w,frame_h,dt_ms,"
            "target_id,box_source,sel_x1,sel_y1,sel_x2,sel_y2,sel_cls,sel_score,"
            "aim_x1,aim_y1,aim_x2,aim_y2,"
            "tx,ty,ref_x,ref_y,smooth_x,smooth_y,vel_x,vel_y,ctrl_x,ctrl_y,"
            "move_x,move_y,nbox");
        for (int i = 0; i < kDetTraceMaxBoxes; ++i) {
            std::fprintf(fp_, ",cls%d,x1_%d,y1_%d,x2_%d,y2_%d,sc%d",
                         i, i, i, i, i, i);
        }
        std::fprintf(fp_, "\n");
        std::fflush(fp_);
        return true;
    }

    void close() {
        if (fp_) { flush(); std::fclose(fp_); fp_ = nullptr; }
        enabled_ = false;
        rows_ = 0;
    }

    bool enabled() const { return enabled_; }

    // 记录一帧（调用方 = AimThread 单线程循环，无并发问题）
    void record(const Entry& e) {
        if (!enabled_ || !fp_) return;
        append_row(e);
        if (++rows_ >= kDetTraceFlushRows) flush();
    }

    // 强制落盘（板端 stop 流程用，保证异常退出也不丢数据）
    void flush() {
        if (!fp_ || buf_.empty()) return;
        std::fwrite(buf_.data(), 1, buf_.size(), fp_);
        std::fflush(fp_);
        buf_.clear();
        rows_ = 0;
    }

private:
    void append_row(const Entry& e) {
        // ★ 关键：vector::data() 在**空 vector 上返回 nullptr** ⇒ 首次 append_row
        //   会往 nullptr 写（UB，MinGW/gcc 上表现为静默不写）⇒ 只有表头没有数据行。
        //   正确做法：先 resize 出一个写入窗口，再往里填；或直接用局部栈缓冲。
        //   这里用局部栈缓冲（单行 ≤ 512B，栈开销可忽略），避免自增扩容的指针失效问题。
        char line[512];
        int n = std::snprintf(line, sizeof(line),
            "%llu,%llu,%d,%d,%.3f,%d,%d,"
            "%.2f,%.2f,%.2f,%.2f,%d,%.4f,"
            "%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%d,%d,%d",
            static_cast<unsigned long long>(e.timestamp_us),
            static_cast<unsigned long long>(e.frame_number),
            e.frame_w, e.frame_h, e.dt_ms,
            e.target_id, e.box_source,
            e.sel_x1, e.sel_y1, e.sel_x2, e.sel_y2, e.sel_cls, e.sel_score,
            e.aim_x1, e.aim_y1, e.aim_x2, e.aim_y2,
            e.tx, e.ty, e.ref_x, e.ref_y,
            e.smooth_x, e.smooth_y, e.vel_x, e.vel_y,
            e.ctrl_x, e.ctrl_y,
            e.move_x, e.move_y, e.nbox);
        if (n < 0) { return; }
        if (static_cast<size_t>(n) >= sizeof(line)) { n = static_cast<int>(sizeof(line)) - 1; }
        const int m = e.nbox < kDetTraceMaxBoxes ? e.nbox : kDetTraceMaxBoxes;
        for (int i = 0; i < kDetTraceMaxBoxes; ++i) {
            int w;
            if (i < m) {
                w = std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                                  ",%d,%.2f,%.2f,%.2f,%.2f,%.4f",
                                  static_cast<int>(e.box[i][0]), e.box[i][1],
                                  e.box[i][2], e.box[i][3], e.box[i][4], e.score[i]);
            } else {
                // ★ 6 个空字段必须写 **6 个逗号**（每个逗号分隔一个字段）。
                //   原写成 5 个（",,,,,"）⇒ 整行少 1 列 ⇒ 7 个空组共少 7 列
                //   ⇒ 回放侧按列名索引会整体错位。列数一致性由 test_det_trace 钉死。
                w = std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), ",,,,,,");
            }
            if (w < 0) { return; }
            n += w;
            if (static_cast<size_t>(n) >= sizeof(line)) { n = static_cast<int>(sizeof(line)) - 1; }
        }
        const int w = std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "\n");
        if (w < 0) { return; }
        buf_.insert(buf_.end(), line, line + n + w);
    }

    std::FILE* fp_ = nullptr;
    bool enabled_ = false;
    int rows_ = 0;
    std::vector<char> buf_;
};

}  // namespace ttbox::core::aim
