// OtaExtract.hpp — OTA 包展开与校验（自 ttbox_ota_updater.py 移植）。
//
// ★ 本模块是解包环节的安全命门：更新器以 **root** 身份解**不可信**的 tar，
//   一个 `payload/../../etc/x` 成员就是本机 RCE。因此成员名校验必须 fail-closed，
//   且判定口径必须与现役 Python **逐条一致**（对拍基准 F 段）。
#pragma once

#include <string>

#include "common/Json.hpp"

namespace ttbox::core::ota {

// 失败态（与 Python 抛出的 OtaError.state 同名，便于对照排查）
struct ExtractResult {
    bool ok = false;
    std::string state;   // unsafe_member / manifest_missing / manifest_mismatch / ...
    std::string detail;
};

// Python OtaUpdater._safe_members 的成员名判定（纯逻辑，可对拍）。
//
// 精确语义（来自现役实现，勿凭直觉改）：
//   1. name 非空
//   2. name 不以 '/' 开头
//   3. **按 '/' 切分后任一分量等于 ".." 即拒** —— ★ 不是"规范化后仍在目录内即可"。
//      例：`payload/a/../b` 规范化后确实落在 dest 内，但现役实现**拒绝**它。
//   4. （Python 还做一次 realpath 越界检查；在 1~3 通过后它恒不触发，
//       且它检查的是**解包前**的成员名，对包内 symlink 逃逸同样无防护 ——
//       故此处不额外实现，避免与现役行为分叉。）
bool safe_member_name(const std::string& name);

// ---- 展开 ----
// 只取 payload/ 前缀成员（**剥掉前缀**落到 staging 根）+ 根 RELEASE_MANIFEST.json，
// 其余成员一律不解。展开前对**每个**成员跑 safe_member_name，任一非法即整包拒。
// ★ 为什么用系统 tar 而不是自己解：tar 是板端基础工具；自己写 tar 解析器是纯风险。
//   安全性不依赖"我们解得对"，而依赖"解之前先把名字全查一遍"—— 解包动作交给成熟工具。
ExtractResult extract_package(const std::string& tgz_path, const std::string& staging);

// ---- manifest ----
// 读 <staging>/RELEASE_MANIFEST.json。
bool read_manifest_json(const std::string& staging, JsonValue* out);

// 逐文件校验 sha256（对应 Python _verify_manifest）。任一不符 ⇒ false，first_bad_rel 记首个。
bool verify_manifest_files(const JsonValue& manifest, const std::string& staging,
                           std::string* first_bad_rel);

// ---- 增量（对应 Python _materialize_delta）----
// fail-closed：manifest 已验签（真实性有保证），但 rel 仍拒 '..'；
// 任一文件在 payload 与当前树里都取不到 / 哈希不符 ⇒ 整包失败，绝不带病浇筑。
// 成功后 staging 会被替换成"完整树"（先写 <staging>.full 再 rename）。
ExtractResult materialize_delta(const std::string& staging, const JsonValue& manifest,
                                const std::string& current_tree);

// ---- 小工具 ----
bool remove_tree(const std::string& path);

}  // namespace ttbox::core::ota
