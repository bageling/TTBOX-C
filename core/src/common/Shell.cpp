// Shell.cpp — 见 Shell.hpp 的文件头说明。
#include "common/Shell.hpp"

#include <cstdio>

namespace ttbox::core {

// 执行命令并把 stdout+stderr 合并写入 out，返回退出码（见 .hpp 契约）。
int run_capture_cmd(const std::string& cmd, std::string* out) {
    FILE* f = ::popen(cmd.c_str(), "r");
    if (f == nullptr) {
        if (out != nullptr) out->clear();
        return -1;
    }
    char buf[4096];
    std::string s;
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    const int rc = ::pclose(f);
    if (out != nullptr) *out = std::move(s);
    return rc;
}

// 只关心退出码、丢弃输出的版本。
int run_quiet_cmd(const std::string& cmd) { return run_capture_cmd(cmd, nullptr); }

// 用单引号包裹字符串，避免 shell 对路径中的特殊字符做解释。
// 单引号段内无法直接表示单引号 ⇒ 逐字符扫描：遇到 ' 就插入 '\''（收尾-转义-重开），
// 使任意输入（含空串、纯 '、连续 ''）经 /bin/sh 解析后都原样还原。
// 反例：不转义时输入 "a';id;'" 会因引号提前闭合而逃逸执行注入命令。
std::string shell_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";  // 4 字符 ' \ ' '：闭合当前引号段 + 转义单引号 + 重开引号段
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

}  // namespace ttbox::core
