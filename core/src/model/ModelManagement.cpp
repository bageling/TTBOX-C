// ModelManagement.cpp — 默认文件级校验器实现。
#include "model/ModelManagement.hpp"

#include <cstdio>

namespace ttbox::core {

// 文件级校验：确认可读、非空且 ≥1KB；无 RKNN 时无法解析 metadata，一律返回 false（fail-closed）
bool ModelManagement::file_level_validator(const std::string& rknn_path, JsonValue* meta_out,
                                           std::string* error) {
    FILE* f = std::fopen(rknn_path.c_str(), "rb");
    if (!f) {
        if (error) *error = "无法打开模型文件: " + rknn_path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);
    if (size <= 0) {
        if (error) *error = "模型文件为空";
        return false;
    }
    if (size < 1024) {
        if (error) *error = "模型文件过小（<1KB），疑似非 RKNN 文件";
        return false;
    }
    // 没有 RKNN Runtime 时只能确认文件可读，不能把 metadata 猜成合法值。
    if (meta_out) *meta_out = JsonValue::object();
    if (error) *error = "validator not configured: VALIDATOR_NOT_CONFIGURED; 当前环境没有 RKNN Runtime，无法验证输入/输出 metadata";
    return false;
}

}  // namespace ttbox::core
