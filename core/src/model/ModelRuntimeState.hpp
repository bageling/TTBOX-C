// ModelRuntimeState.hpp — 模型运行状态提交门。
// 只允许在真实 inference 与 Decode 都成功后更新 running_model_id。
//
// ★★★ 2026-10-04 代码体检：**本类已被生产实现取代，当前是死代码。**
//
//   实测：本类的 8 个写方法（set_running_model / mark_init_failed /
//   mark_inference_failed / mark_decode_failed / mark_inference_pass /
//   mark_decode_pass / commit_ready / stop）在 **core/src 里引用次数 = 0**，
//   只有 core/tests 两个文件在用（test_model_runtime_state.cpp /
//   test_rknn_failure_injection.cpp）。
//   ⇒ 那两个测试是**绿色假象**：它们验证的分支生产代码一行都不走。
//
//   **生产里真正的提交门在哪**（别再找本类）：
//     ApplicationRuntime.cpp::switch_active_model_runtime() 第 5 步「首帧门槛」——
//     轮询 `core_runtime_->model_ready()` 最多 5s，通过才写 running_model_id_。
//     而 model_ready（CoreRuntime.cpp:301）读的是 worker 的**真实原子计数**：
//         任一 worker 的 stats.inference_ok.load() > 0 && decode_ok.load() > 0
//     ⇒ 它比本类强得多：本类只能靠"别人告诉它通过了"，
//        model_ready 是自己去查真实推理/解码计数。
//   所以**不是"该用没接"，是"被更好的实现取代了"**。
//
//   为什么先标注不删：两个测试文件正在用它，删类要连带改测试；
//   而"留着但标清楚"能立刻消除"查提交门去哪找"的绕路。
//   ★ 处置决定权在业主（铁律：不擅自删有争议的东西）。
#pragma once

#include <string>

namespace ttbox::core {

class ModelRuntimeState {
public:
    // 记录用户选择的目标模型 id 并清空失败码（尚未提交为运行态）
    void select(const std::string& model_id) {
        selected_model_id_ = model_id;
        failure_code_.clear();
    }

    // 直接写入运行态模型 id（绕过提交门，仅测试/兼容用）
    void set_running_model(const std::string& model_id) {
        running_model_id_ = model_id;
    }

    // 标记初始化失败：清通过标志并置失败码 RKNN_INIT_FAILED
    void mark_init_failed() {
        inference_passed_ = false;
        decode_passed_ = false;
        failure_code_ = "RKNN_INIT_FAILED";
    }

    // 标记推理失败：清通过标志并置失败码 INFERENCE_FAILED
    void mark_inference_failed() {
        inference_passed_ = false;
        decode_passed_ = false;
        failure_code_ = "INFERENCE_FAILED";
    }

    // 标记解码失败：仅清解码通过标志并置失败码 DECODE_FAILED
    void mark_decode_failed() {
        decode_passed_ = false;
        failure_code_ = "DECODE_FAILED";
    }

    // 标记推理已通过
    void mark_inference_pass() {
        inference_passed_ = true;
    }

    // 标记解码已通过
    void mark_decode_pass() {
        decode_passed_ = true;
    }

    // 提交门：推理与解码均通过且已选模型非空时，才把选择提升为运行态
    bool commit_ready() {
        if (!inference_passed_ || !decode_passed_ || selected_model_id_.empty()) return false;
        running_model_id_ = selected_model_id_;
        failure_code_.clear();
        return true;
    }

    // 停机：清运行态 id、失败码与全部通过标志
    void stop() {
        running_model_id_.clear();
        failure_code_.clear();
        inference_passed_ = false;
        decode_passed_ = false;
    }

    // 只读访问器：选择/运行 id、失败码、通过标志
    const std::string& selected_model_id() const { return selected_model_id_; }
    const std::string& running_model_id() const { return running_model_id_; }
    const std::string& failure_code() const { return failure_code_; }
    bool inference_passed() const { return inference_passed_; }
    bool decode_passed() const { return decode_passed_; }

private:
    std::string selected_model_id_;
    std::string running_model_id_;
    std::string failure_code_;
    bool inference_passed_ = false;
    bool decode_passed_ = false;
};

}  // namespace ttbox::core
