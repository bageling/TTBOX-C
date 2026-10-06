// OtaVersion.hpp — OTA 版本比较与标识符消毒（自 ttbox_ota_updater.py 移植）。
//
// 两块纯逻辑，且都直接决定"能不能装"：
//   · 版本比较 —— 判错会把可升级的包当成降级拒掉（板子升不动），或反之（危险重装）
//   · 标识符消毒 —— 版本号/key_id 会被拼进 releases 路径，不消毒就是 root 下的路径穿越
// 逐值等价性由 core/tests/test_ota_golden.cpp 对 frozen 样本（B/C/D 段）断言。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttbox::core::ota {

// 版本串的分段键（对应 Python 的 (cls, num, text) 三元组）。
// cls=0 表示数字段（比 num），cls=1 表示非数字段（比 text）。
struct VersionSeg {
    int cls = 0;
    long long num = 0;
    std::string text;
};

// Python _version_key(v)：按 [.\-_+] 分段；空段丢弃；纯数字段按**数值**、其余按字典序。
// ★ 关键语义：V1.0.9 < V1.0.10（数值比较，不是字典序 "9" > "10"）。
std::vector<VersionSeg> version_key(const std::string& v);

// 键序比较（Python tuple 的逐元素比较语义）。
int compare_version_key(const std::vector<VersionSeg>& a, const std::vector<VersionSeg>& b);

// Python is_downgrade(new_ver, cur_ver)：新包版本必须**严格高于**当前版本；
// 相等也拒（无意义重装）。★ cur 为空（版本未知）时**放行**（返回 false）。
bool is_downgrade(const std::string& new_ver, const std::string& cur_ver);

// Python _check_safe_id(value, what)。
// 规则：`.strip()` 后须匹配 ^[A-Za-z0-9][A-Za-z0-9._-]*$ 且不含 ".."。
// 返回 true 并把 strip 后的值写入 out；false 表示非法（Python 侧抛 unsafe_field）。
bool check_safe_id(const std::string& value, std::string* out);

}  // namespace ttbox::core::ota
