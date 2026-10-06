// OtaExtract.cpp — 见 OtaExtract.hpp 的文件头说明。
#include "ota/OtaExtract.hpp"

namespace ttbox::core::ota {

bool safe_member_name(const std::string& name) {
    // 1) 非空
    if (name.empty()) return false;
    // 2) 绝对路径一律拒
    if (name[0] == '/') return false;
    // 3) 按 '/' 切分，任一分量为 ".." 即拒。
    //    ★ 注意：空分量（"a//b"）与 "."（"a/./b"）都**合法** —— 与现役 Python 的
    //      PurePosixPath 归一行为一致（它消解 "." 与空分量、保留 ".."）。
    size_t i = 0;
    while (i <= name.size()) {
        size_t j = name.find('/', i);
        if (j == std::string::npos) j = name.size();
        if (name.compare(i, j - i, "..") == 0 && (j - i) == 2) return false;
        i = j + 1;
    }
    return true;
}

}  // namespace ttbox::core::ota
