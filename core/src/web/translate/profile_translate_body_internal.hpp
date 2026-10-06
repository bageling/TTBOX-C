// profile_translate_body_internal.hpp — web_body_to_profile 的 mouse 段拆分声明。
//
// 仅供 profile_translate_body.cpp 与 profile_translate_body_mouse.cpp 内部使用：
// 把 body→mouse 的搬运单独拆出，控制单文件 ≤300 行（方案附 D 硬约束）。
#pragma once

#include <cstdint>

#include "common/Json.hpp"

namespace ttbox::core::web {
namespace detail {

// body → mouse 段结果：mouse 子对象 + 全档类别并集掩码（-1 = 未提交哨兵）。
struct MouseBuildResult {
    JsonValue mouse;
    int64_t class_union_mask = -1;
};

// 面板提交体 → mouse 子对象（web_body_to_profile 的第 1~5b 段）。
MouseBuildResult build_mouse(const JsonValue& body);

}  // namespace detail
}  // namespace ttbox::core::web
