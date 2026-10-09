// hardware_runner_main.cpp — 新架构 RK3588 硬件入口。
// 默认 NullHidOutput：只验证 Capture/RGA/RKNN/Worker/AimThread，不移动真实鼠标。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <climits>
#include "runtime/HardwareRunner.hpp"
#include "model/ModelAdapter.hpp"
#include "model/RuntimeProfile.hpp"
#include "mouse/MouseTypes.hpp"
#include "output/IHidOutput.hpp"
#include "output/TraceHidOutput.hpp"
#include "output/FifoHidOutput.hpp"
#include "output/AiboxHidOutput.hpp"
int main(int argc, char** argv) {
    std::string model;
    std::string device = "/dev/video0";
    std::string fifo_path = "/run/ttbox-aim.fifo";
    std::string profile_path;
    std::string cores_text;
    int workers = 1;
    int seconds = 10;
    uint32_t buffers = 4;
    uint32_t in_w = 0;
    uint32_t in_h = 0;
    bool trace = false;
    bool fifo_mode = false;
    bool simulate = false;
    bool aibox_gadget = false;

    const auto print_help = [&]() {
        std::fprintf(stdout,
            "Usage: hardware_runner_main --model <model.rknn> [options]\n"
            "  --model <path>          RKNN model path (required)\n"
            "  --device <path>         V4L2 device (default: /dev/video0)\n"
            "  --workers <N>           Worker count (default: 1)\n"
            "  --seconds <N>           Run duration in seconds (default: 10)\n"
            "  --cores <i,j,...>       RKNN core indexes; 0->mask1, 1->mask2, 2->mask4\n"
            "  --profile <path>        RuntimeProfile JSON path\n"
            "  --buffers <N>           V4L2 buffer count (default: 4)\n"
            "  --inw <N>               Worker/RGA output width (default: 0=auto)\n"
            "  --inh <N>               Worker/RGA output height (default: 0=auto)\n"
            "  --fifo                  Use FIFO output\n"
            "  --fifo-path <path>      FIFO path (default: /run/ttbox-aim.fifo)\n"
            "  --trace                 Use trace-only output\n"
            "  --aibox-gadget          Use direct /dev/hidg1 output\n"
            "  --simulate-hotkey       Simulate button input (Trace/Null only)\n"
            "  --help                  Show this help\n");
    };
    const auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "[FAIL] %s\n", message.c_str());
        return 2;
    };
    const auto need_value = [&](int& index, const std::string& option, std::string* value) -> bool {
        if (index + 1 >= argc) {
            std::fprintf(stderr, "[FAIL] %s requires a value\n", option.c_str());
            return false;
        }
        *value = argv[++index];
        if (value->empty() || value->front() == '-') {
            std::fprintf(stderr, "[FAIL] %s requires a non-empty value\n", option.c_str());
            return false;
        }
        return true;
    };
    const auto parse_uint = [&](const std::string& text, const std::string& option, uint32_t* out) -> bool {
        try {
            size_t used = 0;
            const unsigned long value = std::stoul(text, &used, 10);
            if (used != text.size() || value > UINT32_MAX) throw std::out_of_range("range");
            *out = static_cast<uint32_t>(value);
            return true;
        } catch (...) {
            std::fprintf(stderr, "[FAIL] %s requires an unsigned integer: %s\n", option.c_str(), text.c_str());
            return false;
        }
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--help") { print_help(); return 0; }
        if (arg == "--trace") { trace = true; continue; }
        if (arg == "--fifo") { fifo_mode = true; continue; }
        if (arg == "--simulate-hotkey") { simulate = true; continue; }
        if (arg == "--aibox-gadget") { aibox_gadget = true; continue; }
        if (arg == "--model" || arg == "--device" || arg == "--cores" || arg == "--profile" ||
            arg == "--fifo-path") {
            if (!need_value(i, arg, &value)) return 2;
            if (arg == "--model") model = value;
            else if (arg == "--device") device = value;
            else if (arg == "--cores") cores_text = value;
            else if (arg == "--profile") profile_path = value;
            else fifo_path = value;
            continue;
        }
        if (arg == "--workers" || arg == "--seconds" || arg == "--buffers" || arg == "--inw" || arg == "--inh") {
            if (!need_value(i, arg, &value)) return 2;
            uint32_t parsed = 0;
            if (!parse_uint(value, arg, &parsed)) return 2;
            if (arg == "--workers") workers = static_cast<int>(parsed);
            else if (arg == "--seconds") seconds = static_cast<int>(parsed);
            else if (arg == "--buffers") buffers = parsed;
            else if (arg == "--inw") in_w = parsed;
            else in_h = parsed;
            continue;
        }
        return fail("unknown argument: " + arg);
    }
    if (model.empty()) return fail("--model is required (use --help for usage)");
    if (workers < 1 || workers > static_cast<int>(ttbox::core::aim::AimTargetMailbox::kMaxWorkers))
        return fail("--workers must be between 1 and 3");
    if (seconds == 0) return fail("--seconds must be greater than zero");
    if (buffers == 0) return fail("--buffers must be greater than zero");
    if ((in_w == 0) != (in_h == 0)) return fail("--inw and --inh must both be zero or both be non-zero");

    std::vector<int> core_indexes;
    if (!cores_text.empty()) {
        size_t begin = 0;
        while (begin <= cores_text.size()) {
            const size_t comma = cores_text.find(',', begin);
            const std::string token = cores_text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
            uint32_t index = 0;
            if (token.empty() || !parse_uint(token, "--cores", &index) || index > 2)
                return fail("--cores indexes must be 0, 1, or 2");
            core_indexes.push_back(static_cast<int>(index));
            if (comma == std::string::npos) break;
            begin = comma + 1;
        }
        if (static_cast<int>(core_indexes.size()) != workers)
            return fail("--cores count must equal --workers count");
    } else {
        for (int i = 0; i < workers; ++i) core_indexes.push_back(i);
    }

    ttbox::core::HardwareRunner::Params p;
    p.capture.device = device;
    p.capture.num_buffers = buffers;
    p.workers.model_path = model;
    p.workers.worker_cores.clear();
    for (const int index : core_indexes) p.workers.worker_cores.push_back(1 << index);
    p.workers.out_w = in_w;
    p.workers.out_h = in_h;

    ttbox::core::RuntimeConfig runtime_config;
    ttbox::core::RuntimeProfile profile;
    if (!profile_path.empty()) {
        std::string profile_error;
        profile = ttbox::core::RuntimeProfile::from_json_file(profile_path, &profile_error);
        if (!profile_error.empty()) return fail(profile_error);
        if (!profile.validate(&profile_error)) return fail("invalid --profile: " + profile_error);
    } else {
        profile.model_id = model;
        profile.mouse.enabled = false;
        // 热键的唯一真源是 mouse.aim_profiles（老平铺字段已删除），这里显式写第 0 档。
        profile.mouse.aim_profiles[0].hotkey = 0x03;
        profile.mouse.aim_profiles[0].hotkey2 = 0x00;
        profile.mouse.aim_profiles[0].hotkey_mode = 0;
        profile.mouse.kp_x = 0.20f;
        profile.mouse.kp_y = 0.20f;
        profile.mouse.kd_x = profile.mouse.kd_y = 0.0f;
    }
    runtime_config.update(profile);
    p.runtime_config = &runtime_config;

    if (simulate && fifo_mode) return fail("--simulate-hotkey cannot be combined with --fifo");
    if (fifo_mode && aibox_gadget) return fail("--fifo cannot be combined with --aibox-gadget");
    p.simulated_buttons = simulate ? 0x01 : 0;
    p.workers.pass_through = false;
    auto trace_output = std::make_shared<ttbox::core::output::TraceHidOutput>();
    auto fifo_output = std::make_shared<ttbox::core::output::FifoHidOutput>(fifo_path);
    auto aibox_output = std::make_shared<ttbox::core::output::AiboxHidOutput>("/dev/hidg1");
    if (aibox_gadget) { aibox_output->set_enabled(true); p.output = aibox_output; }
    else if (fifo_mode) p.output = fifo_output;
    else if (trace) p.output = trace_output;
    else p.output = std::make_shared<ttbox::core::output::NullHidOutput>();

    ttbox::core::HardwareRunner runner;
    std::string error;
    if (!runner.initialize(p, &error)) { std::fprintf(stderr, "[FAIL] initialize: %s\n", error.c_str()); return 1; }
    if (!runner.start(&error)) { std::fprintf(stderr, "[FAIL] start: %s\n", error.c_str()); return 1; }
    std::printf("hardware_runner_main: running seconds=%d workers=%d output=%s\n", seconds, workers,
                aibox_gadget ? "aibox-hidg1" : (fifo_mode ? "fifo" : (trace ? "trace" : "null")));
    for (int i = 0; i < seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto status = runner.status();
        std::printf("[HW] t=%d format=%ux%u capture=%llu rga=%llu infer=%llu decode=%llu publish=%llu candidates=%llu detections=%llu worker=%llu errors=%llu skipped=%llu aim=%llu target=%llu no_target=%llu pred=(%.1f,%.1f) control=(%.1f,%.1f) smith=(%.1f,%.1f) move_range=[%d..%d,%d..%d] clipped=%llu last_frame=%llu\n",
                    i + 1, status.width, status.height, (unsigned long long)status.capture_frames,
                    (unsigned long long)status.worker_rga_ok, (unsigned long long)status.worker_inference_ok,
                    (unsigned long long)status.worker_decode_ok, (unsigned long long)status.worker_published,
                    (unsigned long long)status.worker_candidates, (unsigned long long)status.worker_detections,
                    (unsigned long long)status.worker_processed, (unsigned long long)status.worker_errors,
                    (unsigned long long)status.worker_skipped, (unsigned long long)status.aim_consumed,
                    (unsigned long long)status.aim_target_frames, (unsigned long long)status.aim_no_target_frames,
                    status.aim_predicted_x, status.aim_predicted_y, status.aim_control_x, status.aim_control_y,
                    status.aim_smith_dx, status.aim_smith_dy, status.aim_min_move_x, status.aim_max_move_x,
                    status.aim_min_move_y, status.aim_max_move_y, (unsigned long long)status.aim_clipped_frames,
                    (unsigned long long)status.aim_last_frame);
    }
    runner.stop();
    std::printf("hardware_runner_main: stopped cleanly\n");
    return 0;
}
