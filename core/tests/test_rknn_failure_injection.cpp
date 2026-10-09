// test_rknn_failure_injection.cpp — RKNN Engine 失败注入测试。
// Windows：仅验证 ModelRuntimeState 状态门（RKNNEngine 实现仅板端编译）。
// 板端（TTBOX_CORE_HAS_RKNN）：额外验证 RKNNEngine 测试 hook 的真实接线。
#include <string>

#include "model/ModelRuntimeState.hpp"
#include "rknn/RKNNEngine.hpp"

#ifdef TTBOX_CORE_HAS_RKNN
#include <cstdio>
#endif

using namespace ttbox::core;

#ifdef TTBOX_CORE_HAS_RKNN
static RknnModelInfo fake_model_info() {
    RknnModelInfo info;
    info.n_inputs = 1;
    info.n_outputs = 6;
    info.input_width = 256;
    info.input_height = 256;
    return info;
}
#endif

int main() {
    // ---- 状态门：init 失败保持旧模型 ----
    {
        ModelRuntimeState state;
        state.select("new_model");
        state.set_running_model("old_model");
        state.mark_init_failed();
        if (state.selected_model_id() != "new_model" ||
            state.running_model_id() != "old_model" ||
            state.failure_code() != "RKNN_INIT_FAILED") return 1;
    }

    // ---- 状态门：推理失败保持旧模型 ----
    {
        ModelRuntimeState state;
        state.select("new_model");
        state.set_running_model("old_model");
        state.mark_inference_failed();
        if (state.running_model_id() != "old_model" ||
            state.failure_code() != "INFERENCE_FAILED") return 2;
    }

    // ---- 状态门：仅推理成功不提交 ----
    {
        ModelRuntimeState state;
        state.select("new_model");
        state.set_running_model("old_model");
        state.mark_inference_pass();
        if (state.commit_ready() || state.running_model_id() != "old_model") return 3;
    }

    // ---- 状态门：decode 失败不提交 ----
    {
        ModelRuntimeState state;
        state.select("new_model");
        state.set_running_model("old_model");
        state.mark_inference_pass();
        state.mark_decode_failed();
        if (state.commit_ready() || state.running_model_id() != "old_model" ||
            state.failure_code() != "DECODE_FAILED") return 4;
    }

    // ---- 状态门：推理+解码都成功才提交新模型 ----
    {
        ModelRuntimeState state;
        state.select("new_model");
        state.set_running_model("old_model");
        state.mark_inference_pass();
        state.mark_decode_pass();
        if (!state.commit_ready() || state.running_model_id() != "new_model" ||
            !state.failure_code().empty()) return 5;
    }

#ifdef TTBOX_CORE_HAS_RKNN
    // ---- Engine hook：init 失败路径 ----
    {
        RKNNEngine engine;
        RKNNEngine::Params params;
        params.model_path = "TEST_MODEL";
        params.test_init_hook = [](RknnModelInfo*, std::string* error) {
            if (error) *error = "injected init failure";
            return false;
        };
        std::string error;
        if (engine.init(params, &error) || error != "injected init failure") return 6;
    }

    // ---- Engine hook：init 成功 + run 失败路径 ----
    {
        RKNNEngine engine;
        RKNNEngine::Params params;
        params.model_path = "TEST_MODEL";
        params.test_init_hook = [](RknnModelInfo* info, std::string*) {
            *info = fake_model_info();
            return true;
        };
        params.test_run_hook = [](std::string* error) {
            if (error) *error = "injected inference failure";
            return false;
        };
        std::string error;
        if (!engine.init(params, &error)) return 7;
        if (!engine.initialized()) return 8;
        if (engine.run(&error) || error != "injected inference failure") return 9;
    }
#endif

    return 0;
}
