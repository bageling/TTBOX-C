// ModelManagement.hpp — 模型管理注册表（ModelRegistry 的进程内持有者 + 校验器注入）。
//
// 设计约束（与任务书一致）：
//   - 复用 core/src/model/ModelRegistry.hpp 的完整能力（import/validate/install/activate/remove/list）
//     不重写、不改 ModelRegistry 一行代码。
//   - validator 注入：生产环境应接入 RKNN+Adapter 真实加载校验。
//     本实现提供两级 validator：
//       1) 文件级校验（始终可用）：非空、最小尺寸、扩展名 .rknn
//       2) 若 TTBOX_CORE_HAS_RKNN 且注入了 RknnValidator，则做真实加载校验（板端）
//     Windows/无 RKNN 环境下用 (1)，板端建议启用 (2)。
//   - 该类由 Application 持有，生命周期 = 进程；IPC 层通过 Application 注册的回调访问。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "model/ModelRegistry.hpp"

namespace ttbox::core {

class ModelManagement {
public:
    // 构造时即创建内嵌 ModelRegistry（生命周期与持有者一致）
    explicit ModelManagement(const ModelRegistryOptions& opts = {}) {
        registry_ = std::make_unique<ModelRegistry>(opts);
    }

    // init：建目录。应在 Application::initialize 阶段调用。
    bool init(std::string* error = nullptr) { return registry_->init(error); }

    // 注入真实 RKNN 校验器（板端启用）。未注入时使用内置文件级校验。
    void set_validator(std::function<bool(const std::string&, JsonValue*, std::string*)> v) {
        registry_->set_validator(std::move(v));
    }

    // 默认 validator：文件存在 + 非空 + ≥1KB（防手滑传文本文件）。
    // metadata 只能给占位（无 RKNN 无法解析输入尺寸/类别数），UI 对缺失字段显示"暂无数据"。
    static bool file_level_validator(const std::string& rknn_path, JsonValue* meta_out,
                                     std::string* error);

    // 访问底层 ModelRegistry（Application / IPC 用）
    ModelRegistry& registry() { return *registry_; }
    const ModelRegistry& registry() const { return *registry_; }

private:
    std::unique_ptr<ModelRegistry> registry_;
};

}  // namespace ttbox::core
