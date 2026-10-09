// ConfigManager.hpp — 运行时配置读取（阶段 A-1：只做最基础读取）
//
// 约定（与现有 Python 配置兼容，不重新设计格式）：
//   - 读取 ttbox2/config/default.json（路径可由调用方/CLI 指定）
//   - 配置文件不存在 -> 明确错误，不允许 silent fallback
//   - JSON 解析失败 -> 明确错误（含位置信息），不允许静默忽略
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/Json.hpp"

namespace ttbox::core {

class ConfigManager {
public:
    // 加载配置文件。失败返回 false 并给出明确错误（路径/解析/结构）。
    // path 为目录时自动走分层加载（见 load_layered），单文件行为保持不变。
    bool load(const std::string& path, std::string* error = nullptr);

    // 分层加载（T01 配置分层）：按文件名升序读取 <dir>/*.json 并深合并，
    // 后加载者覆盖先加载者（00-factory.json 只读基线 ← 10-device.json 客户覆盖）。
    //   - 对象递归合并；标量/数组整体替换（与 SET_CONFIG 现有语义一致）
    //   - 写回目标 path_ 固定为 <dir>/10-device.json（客户层，可写）；
    //     00-factory.json 属只读基线，绝不作为写回目标
    //   - 目录为空或不存在 → 明确失败，不允许静默空配置
    bool load_layered(const std::string& dir, std::string* error = nullptr);

    // 分层模式标志（load_layered 成功后 true）：path() 为设备层文件，
    // layer_base() 为"基线视图"（除设备层文件外所有层的深合并）。
    bool is_layered() const { return layered_; }
    const JsonValue& layer_base() const { return layer_base_; }

    // 持久化：把 view（调用方组装好的完整有效配置视图）写回可写目标。
    //   单文件模式：整体写 path()（与历史行为一致）
    //   分层模式：只写 view 相对 layer_base() 的差异到 path()（10-device.json），
    //             与基线相同的键被剔除——00-factory.json 的值不被"快照冻结"进
    //             设备层，固件升级修改基线仍能生效（R3）
    // 原子性：先写 <目标>.tmp 再 rename 替换（Windows 用 MoveFileExW REPLACE_EXISTING）。
    // B1 回归防线：写回目标永远是 path()（文件），与本类加载来源解耦——
    // 即使 --config 传的是目录，也不会把目录当文件写。
    bool persist(const JsonValue& view, std::string* error = nullptr);

    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    // 分层模式：判断写回目标（设备层 10-device.json）里是否**显式**含有某个顶层键。
    // 用途：区分「部署/用户显式设置」与「继承自出厂基线」——自愈类迁移只补齐
    // 设备层缺失的键，绝不覆盖部署方显式写过的值。
    // 单文件模式（未分层）恒返回 false：只有一个文件，"继承"无从谈起。
    bool device_layer_has(const std::string& key) const;

    // 结构化读取：key 不存在时返回默认值（调用方显式给默认值，非静默兜底）
    std::string get_string(const std::string& key, const std::string& def = "") const;
    double get_double(const std::string& key, double def = 0.0) const;
    int64_t get_int(const std::string& key, int64_t def = 0) const;
    bool get_bool(const std::string& key, bool def = false) const;

    // 扁平化为字符串键值（供 IPC GET_CONFIG 输出）
    std::vector<std::pair<std::string, std::string>> flatten() const;

    // 访问已加载的配置根节点（只读）
    const JsonValue& root() const { return root_; }

    // 仅供 Application 在配置原子落盘成功后同步 canonical 内存根节点。
    // 调用方已完成结构校验；不改变 path/loaded 状态。
    void replace_root(JsonValue root) { root_ = std::move(root); }

private:
    JsonValue root_ = JsonValue::object();
    bool loaded_ = false;
    std::string path_;
    // 分层模式状态（load_layered 维护）：
    //   layered_    = 是否分层加载
    //   layer_base_ = 基线视图（除写回目标 10-device.json 外所有层的深合并），
    //                 persist 的 diff 计算用它做被减数
    bool layered_ = false;
    JsonValue layer_base_ = JsonValue::object();
};

}  // namespace ttbox::core
