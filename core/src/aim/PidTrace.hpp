// PidTrace.hpp — 第13阶段：PID 逐帧 Trace 采集器
//
// 目的：用真实 HDMI + NPU 检测数据产生控制轨迹，供离线分析：
//   收敛速度 / 稳态误差 / 抖动 / 过冲 / 响应延迟 / 输出饱和。
// 每帧记录（CSV）：
//   timestamp_us, frame_number, target_id, target_x, target_y,
//   reference_x, reference_y, error_x, error_y,
//   controller_raw_x, controller_raw_y, final_command_x, final_command_y,
//   target_switch, target_lost, confidence
//
// 设计：默认关闭；开启后写 CSV 文件（板端 /opt/ttbox/data/pid_trace.csv）。
// 只记录、不改变任何控制行为；不涉及 HID/输出设备。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace ttbox::core::aim {

class PidTrace {
public:
    // 单帧记录（与 AimThread 每帧可得的量一一对应）
    struct Entry {
        uint64_t timestamp_us = 0;
        uint64_t frame_number = 0;
        int target_id = -1;
        float target_x = 0.0f;
        float target_y = 0.0f;
        float reference_x = 0.0f;
        float reference_y = 0.0f;
        float error_x = 0.0f;
        float error_y = 0.0f;
        // 第15阶段：预测点与预测误差（AimTracker 输出）
        float predicted_x = 0.0f;
        float predicted_y = 0.0f;
        float pred_error_x = 0.0f;
        float pred_error_y = 0.0f;
        float controller_raw_x = 0.0f;   // PID 输出（未缩放）
        float controller_raw_y = 0.0f;
        int32_t final_command_x = 0;     // 最终命令（Gate 后）
        int32_t final_command_y = 0;
        // 第15阶段：输出链中间值（可完整 Trace 到 MouseCommand）
        float deadzone_out_x = 0.0f;     // deadzone 应用后（死区归零前）
        float deadzone_out_y = 0.0f;
        float rate_limited_x = 0.0f;     // rate limit 后
        float rate_limited_y = 0.0f;
        float remainder_x = 0.0f;        // 小数余量累积（写入前）
        float remainder_y = 0.0f;
        int target_switch = 0;           // 1 = 本帧发生目标切换
        int target_lost = 0;             // 1 = 本帧目标丢失（宽限耗尽）
        float confidence = 0.0f;
    };

    PidTrace() = default;
    ~PidTrace() { close(); }
    // 持有裸 FILE* + 析构里 close() ⇒ 不可拷贝：复制即 double-fclose，
    // 且两个对象会往同一个文件里交错写，trace 直接损坏。
    PidTrace(const PidTrace&) = delete;
    PidTrace& operator=(const PidTrace&) = delete;

    // 开启采集（写文件）。path 为空用默认 /tmp/pid_trace.csv
    bool open(const std::string& path = "") {
        close();
        const std::string p = path.empty() ? "/tmp/pid_trace.csv" : path;
        fp_ = std::fopen(p.c_str(), "w");
        if (!fp_) return false;
        enabled_ = true;
        std::fprintf(fp_, "timestamp_us,frame_number,target_id,target_x,target_y,"
                          "reference_x,reference_y,error_x,error_y,"
                          "predicted_x,predicted_y,pred_error_x,pred_error_y,"
                          "controller_raw_x,controller_raw_y,"
                          "deadzone_out_x,deadzone_out_y,"
                          "rate_limited_x,rate_limited_y,"
                          "remainder_x,remainder_y,"
                          "final_command_x,final_command_y,"
                          "target_switch,target_lost,confidence\n");
        return true;
    }

    void close() {
        if (fp_) { std::fclose(fp_); fp_ = nullptr; }
        enabled_ = false;
    }

    bool enabled() const { return enabled_; }

    // 记录一帧（线程安全由调用方保证——AimThread 单线程循环内调用）
    void record(const Entry& e) {
        if (!enabled_ || !fp_) return;
        std::fprintf(fp_, "%llu,%llu,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                          "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                          "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                          "%d,%d,%d,%d,%.4f\n",
                     static_cast<unsigned long long>(e.timestamp_us),
                     static_cast<unsigned long long>(e.frame_number),
                     e.target_id,
                     e.target_x, e.target_y, e.reference_x, e.reference_y,
                     e.error_x, e.error_y,
                     e.predicted_x, e.predicted_y, e.pred_error_x, e.pred_error_y,
                     e.controller_raw_x, e.controller_raw_y,
                     e.deadzone_out_x, e.deadzone_out_y,
                     e.rate_limited_x, e.rate_limited_y,
                     e.remainder_x, e.remainder_y,
                     e.final_command_x, e.final_command_y,
                     e.target_switch, e.target_lost, e.confidence);
        std::fflush(fp_);  // 逐帧落盘，便于中途查看
    }

private:
    std::FILE* fp_ = nullptr;
    bool enabled_ = false;
};

}  // namespace ttbox::core::aim
