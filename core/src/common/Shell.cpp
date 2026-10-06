// Shell.cpp — 见 Shell.hpp 的文件头说明。
#include "common/Shell.hpp"

#include <cstdio>

namespace ttbox::core {

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

int run_quiet_cmd(const std::string& cmd) { return run_capture_cmd(cmd, nullptr); }

std::string shell_quote(const std::string& s) { return "'" + s + "'"; }

}  // namespace ttbox::core
