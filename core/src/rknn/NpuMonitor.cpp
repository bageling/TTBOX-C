// NpuMonitor.cpp — NPU 利用率采集实现
#include "rknn/NpuMonitor.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace ttbox::core {

namespace {

// 解析 "NPU load:  Core0:  12%, Core1:  3%, Core2:  0%,"
float parse_core(const std::string& text, const char* tag) {
    const size_t p = text.find(tag);
    if (p == std::string::npos) return -1.0f;
    size_t q = p + std::strlen(tag);
    while (q < text.size() && (text[q] == ' ' || text[q] == ':')) ++q;
    float v = -1.0f;
    std::sscanf(text.c_str() + q, "%f", &v);
    return v;
}

}  // namespace

// 单次读取 debugfs 节点，解析三核利用率。
NpuLoadSample NpuMonitor::read_once() {
    NpuLoadSample s;
    std::ifstream f("/sys/kernel/debug/rknpu/load");
    if (!f.is_open()) return s;
    std::string text;
    std::getline(f, text);
    if (text.empty()) return s;
    s.core0 = parse_core(text, "Core0");
    s.core1 = parse_core(text, "Core1");
    s.core2 = parse_core(text, "Core2");
    s.ok = s.core0 >= 0.0f && s.core1 >= 0.0f && s.core2 >= 0.0f;
    return s;
}

// 启动后台采样线程（先预检节点是否可读）。
bool NpuMonitor::start(int interval_ms, std::string* error) {
    if (running_.load()) {
        if (error) *error = "NpuMonitor 已在运行";
        return false;
    }
    if (interval_ms <= 0) {
        if (error) *error = "interval_ms 必须 > 0";
        return false;
    }
    // 预检节点可读
    const NpuLoadSample probe = read_once();
    if (!probe.ok) {
        if (error) *error = "/sys/kernel/debug/rknpu/load 不可读（需 root/debugfs）";
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this, interval_ms] {
        while (running_.load()) {
            const NpuLoadSample s = read_once();
            if (s.ok) {
                std::lock_guard<std::mutex> lock(mutex_);
                sum0_ += s.core0;
                sum1_ += s.core1;
                sum2_ += s.core2;
                ++samples_;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    });
    return true;
}

// 停止后台采样线程并等待其退出。
void NpuMonitor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

// 返回三核利用率的累计平均值。
NpuLoadSummary NpuMonitor::summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    NpuLoadSummary s;
    s.samples = samples_;
    if (samples_ > 0) {
        s.core0 = sum0_ / static_cast<float>(samples_);
        s.core1 = sum1_ / static_cast<float>(samples_);
        s.core2 = sum2_ / static_cast<float>(samples_);
    }
    return s;
}

}  // namespace ttbox::core
