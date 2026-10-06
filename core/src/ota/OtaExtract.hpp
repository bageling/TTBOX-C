// OtaExtract.hpp — OTA 包展开与校验（自 ttbox_ota_updater.py 移植）。
//
// ★ 本模块是解包环节的安全命门：更新器以 **root** 身份解**不可信**的 tar，
//   一个 `payload/../../etc/x` 成员就是本机 RCE。因此成员名校验必须 fail-closed，
//   且判定口径必须与现役 Python **逐条一致**（对拍基准 F 段）。
#pragma once

#include <string>
#include <vector>

namespace ttbox::core::ota {

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

}  // namespace ttbox::core::ota
