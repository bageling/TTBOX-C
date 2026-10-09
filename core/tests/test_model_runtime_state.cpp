// test_model_runtime_state.cpp — 模型运行状态提交门测试。
#include "model/ModelRuntimeState.hpp"
#include "test_util.hpp"

using ttbox::core::ModelRuntimeState;

TEST(rknn_init_failure_keeps_previous_running_model) {
    ModelRuntimeState state;
    state.select("new_model");
    state.set_running_model("old_model");
    state.mark_init_failed();

    CHECK(state.selected_model_id() == "new_model");
    CHECK(state.running_model_id() == "old_model");
    CHECK(state.failure_code() == "RKNN_INIT_FAILED");
    CHECK(!state.commit_ready());
}

TEST(inference_failure_keeps_previous_running_model) {
    ModelRuntimeState state;
    state.select("new_model");
    state.set_running_model("old_model");
    state.mark_inference_failed();

    CHECK(state.selected_model_id() == "new_model");
    CHECK(state.running_model_id() == "old_model");
    CHECK(state.failure_code() == "INFERENCE_FAILED");
    CHECK(!state.commit_ready());
}

TEST(decode_failure_keeps_previous_running_model) {
    ModelRuntimeState state;
    state.select("new_model");
    state.set_running_model("old_model");
    state.mark_inference_pass();

    CHECK(!state.commit_ready());
    state.mark_decode_failed();
    CHECK(state.selected_model_id() == "new_model");
    CHECK(state.running_model_id() == "old_model");
    CHECK(state.failure_code() == "DECODE_FAILED");
    CHECK(!state.commit_ready());
}

TEST(running_model_commits_only_after_inference_and_decode) {
    ModelRuntimeState state;
    state.select("jwdl_sjzv11");
    state.set_running_model("old_model");
    state.mark_inference_pass();

    CHECK(state.running_model_id() == "old_model");
    CHECK(!state.commit_ready());

    state.mark_decode_pass();
    CHECK(state.commit_ready());
    CHECK(state.running_model_id() == "jwdl_sjzv11");
    CHECK(state.failure_code().empty());
}

int main() {
    return ttbox_test::run_all() == 0 ? 0 : 1;
}