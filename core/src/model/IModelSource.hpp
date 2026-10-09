// IModelSource.hpp — 模型来源抽象（阶段 A-8，未来云端预留）
//
// 现状：只定义接口 + 本地文件源。云端（CloudModelSource）仅预留声明，
// 不实现（A8 明确：现在不要实现云端）。
//
// 未来链路：
//   云端下发 → staging → 验证 → installed → activate
#pragma once

#include <string>

namespace ttbox::core {

// 模型来源接口：获取模型文件到指定目标路径
class IModelSource {
public:
    virtual ~IModelSource() = default;

    // 来源标识（如 "local:<path>" / "cloud:<model_id>"）
    virtual std::string source_id() const = 0;

    // 将模型文件获取到 dest_path。成功返回 true。
    virtual bool fetch_to(const std::string& dest_path, std::string* error = nullptr) = 0;
};

// 本地文件源：从磁盘文件导入
class LocalFileSource : public IModelSource {
public:
    explicit LocalFileSource(std::string path) : path_(std::move(path)) {}
    std::string source_id() const override { return "local:" + path_; }
    bool fetch_to(const std::string& dest_path, std::string* error = nullptr) override;

private:
    std::string path_;
};

// 云端模型源（预留：A8 不实现；接口占位供未来接入）
class CloudModelSource : public IModelSource {
public:
    explicit CloudModelSource(std::string model_id) : model_id_(std::move(model_id)) {}
    std::string source_id() const override { return "cloud:" + model_id_; }
    // A8：未实现云端，调用返回 false
    bool fetch_to(const std::string& /*dest_path*/, std::string* error = nullptr) override {
        if (error) *error = "CloudModelSource 未实现（A8 仅预留接口）";
        return false;
    }

private:
    std::string model_id_;
};

}  // namespace ttbox::core
