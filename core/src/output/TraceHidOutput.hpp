// TraceHidOutput.hpp — 安全的输出观测后端。
#pragma once
#include <atomic>
#include <cstdint>
#include "output/IHidOutput.hpp"
namespace ttbox::core::output {
// TraceHidOutput：只记录最近一次动作的观测后端（不触设备，恒成功）。
class TraceHidOutput final : public IHidOutput {
public:
 // 记录动作并计数，恒成功（不触设备）。
 bool send(const OutputAction& a) override { last_x_.store(a.move_x); last_y_.store(a.move_y); last_frame_.store(a.frame_number); count_.fetch_add(1); return true; }
 // 统计访问器：动作计数 / 最近一次 dx,dy / 最近帧号。
 uint64_t count() const{return count_.load();} int16_t last_x()const{return last_x_.load();} int16_t last_y()const{return last_y_.load();} uint64_t last_frame()const{return last_frame_.load();}
private: std::atomic<uint64_t> count_{0},last_frame_{0}; std::atomic<int16_t> last_x_{0},last_y_{0};
};
}
