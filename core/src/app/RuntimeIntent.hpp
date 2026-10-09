// RuntimeIntent.hpp — core 启动意图裁决（R5 用户意愿 / R6 刚更新过）
//
// 为什么独立成纯函数：这段判定决定"开机到底跑不跑 AI 流水线"，判错就是现场事故
// （要么更新完自己开跑，要么该跑的不跑）。做成不碰文件系统的纯函数，host 上可单测
// （tests/test_startup_intent.cpp），磁盘 I/O 留在 Application 里。
#pragma once

#include <cctype>
#include <string>

#include "common/Json.hpp"

namespace ttbox::core {

struct StartupIntent {
    // true = 判定为"刚完成版本更新"（OTA / 重装），按 R6 强制停止并落盘。
    bool just_updated = false;
    // 建议的 want_runtime_running_ 取值。
    bool want_running = true;
    // 诊断文案（进日志用）。
    std::string reason;
};

// 裁决规则（任一成立即"刚更新过"）：
//   ① prev_boot_version 非空且 != cur_version ⇒ 中间换过版本；
//   ② prev_boot_version 为空（升到本特性首个版本时会这样）+ ota_status 记着
//      state=SUCCESS 且 version == cur_version ⇒ 更新器刚把本版本装上来。
// 其余情况 want_running 保持 true，并由调用方继续按 R5 还原用户显式意愿。
//
// ota_status 可为 nullptr（文件缺失/解析失败）——此时 ② 不成立。
inline StartupIntent decide_startup_intent(const std::string& prev_boot_version,
                                           const std::string& cur_version,
                                           const JsonValue* ota_status) {
    StartupIntent out;
    if (!prev_boot_version.empty()) {
        if (prev_boot_version != cur_version) {
            out.just_updated = true;
            out.want_running = false;
            out.reason = "启动版本 " + prev_boot_version + " → 当前 " + cur_version;
        }
        return out;
    }
    if (ota_status == nullptr) {
        return out;
    }
    const JsonValue* state_v = ota_status->find("state");
    const JsonValue* ver_v = ota_status->find("version");
    std::string state_s =
        (state_v != nullptr && state_v->is_string()) ? state_v->as_string() : std::string();
    for (char& ch : state_s) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (state_s == "SUCCESS" && ver_v != nullptr && ver_v->is_string() &&
        ver_v->as_string() == cur_version) {
        out.just_updated = true;
        out.want_running = false;
        out.reason = "更新器记录 state=SUCCESS / version=" + cur_version;
    }
    return out;
}

// 更新冒烟自检（方案B，2026-09-20）：从**已解析**的 ota_status.json 判定"更新器终态"。
// 仅当 version == expected_version 且 state 为 SUCCESS/FAILED 时返回该状态串（大写）；
// 其余（RUNNING / 缺字段 / 版本不符）返回空串。
// ★ version 必须匹配：否则会把上一次更新残留的 SUCCESS 误当本次结果而提前收尾。
// 纯函数（无磁盘 I/O），磁盘读取留在 Application，host 可单测。
inline std::string ota_terminal_state(const JsonValue& ota_status,
                                      const std::string& expected_version) {
    if (expected_version.empty() || !ota_status.is_object()) return std::string();
    const JsonValue* ver = ota_status.find("version");
    if (ver == nullptr || !ver->is_string() || ver->as_string() != expected_version) {
        return std::string();
    }
    const JsonValue* st = ota_status.find("state");
    if (st == nullptr || !st->is_string()) return std::string();
    std::string s = st->as_string();
    for (char& ch : s) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    if (s == "SUCCESS" || s == "FAILED") return s;
    return std::string();
}

}  // namespace ttbox::core
