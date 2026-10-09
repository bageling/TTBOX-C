// IModelSource.cpp — 本地文件源实现
#include "model/IModelSource.hpp"

#include <cstdio>

namespace ttbox::core {

// 从本地路径分块拷贝模型文件到 dest_path；任一读写失败返回 false 并填 error
bool LocalFileSource::fetch_to(const std::string& dest_path, std::string* error) {
    FILE* in = std::fopen(path_.c_str(), "rb");
    if (!in) {
        if (error) *error = "无法打开源文件: " + path_;
        return false;
    }
    FILE* out = std::fopen(dest_path.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        if (error) *error = "无法创建目标文件: " + dest_path;
        return false;
    }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (std::ferror(in)) ok = false;
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (!ok && error) *error = "复制失败: " + path_ + " -> " + dest_path;
    return ok;
}

}  // namespace ttbox::core
