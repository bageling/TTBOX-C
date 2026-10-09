// OtaExtract.cpp — 见 OtaExtract.hpp 的文件头说明。
#include "ota/OtaExtract.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "common/Shell.hpp"
#include "ota/OtaCrypto.hpp"

namespace ttbox::core::ota {

namespace {

namespace fs = std::filesystem;

// 按 '\n' 切分文本并去掉行尾 '\r'（用于解析 tar -tzf 输出）。
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

}  // namespace

// 判 tar 成员名是否安全：非空、非绝对路径、且按 '/' 切分后无任一分量为 ".."。
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
        if ((j - i) == 2 && name.compare(i, 2, "..") == 0) return false;
        i = j + 1;
    }
    return true;
}

// 展开 OTA 包：先全量校验成员名（fail-closed），再抽取 payload/ 与根 manifest 到 staging。
ExtractResult extract_package(const std::string& tgz_path, const std::string& staging) {
    ExtractResult r;
    std::error_code ec;
    fs::create_directories(staging, ec);

    // ① 先列全部成员并逐个校验（★ 安全关键：解包动作必须在"名字全可信"之后才发生）
    std::string listing;
    if (run_capture_cmd("tar -tzf " + shell_quote(tgz_path), &listing) != 0) {
        r.state = "tar_list_failed";
        r.detail = "无法列出包内成员（tar -tzf 失败）";
        return r;
    }
    bool has_manifest = false;
    bool has_payload = false;
    for (const std::string& raw : split_lines(listing)) {
        if (raw.empty()) continue;
        if (!safe_member_name(raw)) {
            r.state = "unsafe_member";
            r.detail = "非法成员名: " + raw;
            return r;
        }
        if (raw == "RELEASE_MANIFEST.json") has_manifest = true;
        if (raw == "payload" || raw.rfind("payload/", 0) == 0) has_payload = true;
    }
    if (!has_manifest) {
        r.state = "manifest_missing";
        r.detail = "包内缺 RELEASE_MANIFEST.json";
        return r;
    }
    if (!has_payload) {
        r.state = "payload_missing";
        r.detail = "包内缺 payload/ 成员";
        return r;
    }

    // ② 展开 payload/（剥掉首层前缀）——只取这一类成员
    const std::string q = shell_quote(tgz_path);
    const std::string c = shell_quote(staging);
    // ★★ 两个开关都不可省（V1.0.57/59 板端真机实测连踩两次）：
    //   --wildcards            GNU tar 对成员名参数**默认不做通配** ⇒ 'payload/*' 被当成
    //                         字面成员名 ⇒ "payload/*: Not found in archive"。
    //   --wildcards-match-slash 默认通配的 '*' **不跨 '/'** ⇒ 只匹配到 payload/ 第一层，
    //                         scripts/ plugins/*/ ttbox_motion/ 等深层文件**全部漏解**
    //                         （表现为 install 时几十条「staging 缺失 manifest 声明文件」）。
    //   这两条在 host 上无论怎么测都发现不了 —— 只有真 tar 才报。
    if (run_quiet_cmd("tar -xzf " + q + " -C " + c +
                      " --strip-components=1 --wildcards --wildcards-match-slash 'payload/*'") != 0) {
        r.state = "extract_failed";
        r.detail = "展开 payload/ 失败";
        return r;
    }
    // ③ 单独展开根 manifest（不剥前缀）
    if (run_quiet_cmd("tar -xzf " + q + " -C " + c + " 'RELEASE_MANIFEST.json'") != 0) {
        r.state = "extract_failed";
        r.detail = "展开 RELEASE_MANIFEST.json 失败";
        return r;
    }
    r.ok = true;
    return r;
}

// 读取并解析 staging 下的 RELEASE_MANIFEST.json（必须是 object）。
bool read_manifest_json(const std::string& staging, JsonValue* out) {
    const fs::path mp = fs::path(staging) / "RELEASE_MANIFEST.json";
    std::ifstream f(mp, std::ios::binary);
    if (!f.good()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    const JsonParseResult pr = json_parse(ss.str());
    if (!pr.ok || !pr.value.is_object()) return false;
    if (out != nullptr) *out = pr.value;
    return true;
}

// 逐文件核对 staging 内文件 sha256 与 manifest 是否一致；首个不符项写入 first_bad_rel。
bool verify_manifest_files(const JsonValue& manifest, const std::string& staging,
                           std::string* first_bad_rel) {
    std::error_code ec;
    const JsonValue* files = manifest.find("files_sha256");
    if (files == nullptr || !files->is_object() || files->as_object().empty()) return false;
    for (const auto& kv : files->as_object()) {
        const fs::path fp = fs::path(staging) / kv.first;
        std::string want;
        if (kv.second.is_string()) want = kv.second.as_string("");
        std::string got;
        if (!fs::is_regular_file(fp, ec) || !sha256_file_hex(fp.string(), &got) || got != want) {
            if (first_bad_rel != nullptr) *first_bad_rel = kv.first;
            return false;
        }
    }
    return true;
}

// 增量合并：按 manifest 用 payload + 当前树补齐成完整树（<staging>.full），再原子替换 staging。
ExtractResult materialize_delta(const std::string& staging, const JsonValue& manifest,
                                const std::string& current_tree) {
    ExtractResult r;
    std::error_code ec;
    const JsonValue* files = manifest.find("files_sha256");
    if (files == nullptr || !files->is_object() || files->as_object().empty()) {
        r.state = "manifest_mismatch";
        r.detail = "增量包 manifest 缺 files_sha256";
        return r;
    }
    const std::string full = staging + ".full";
    fs::remove_all(full, ec);
    fs::create_directories(full, ec);
    for (const auto& kv : files->as_object()) {
        const std::string& rel = kv.first;
        // fail-closed：manifest 已验签（真实性有保证），但 rel 仍拒 '..'
        if (!rel.empty() && rel[0] == '/') {
            r.state = "manifest_mismatch";
            r.detail = "manifest 出现非法路径: " + rel;
            return r;
        }
        {
            size_t i = 0;
            while (i <= rel.size()) {
                size_t j = rel.find('/', i);
                if (j == std::string::npos) j = rel.size();
                if ((j - i) == 2 && rel.compare(i, 2, "..") == 0) {
                    r.state = "manifest_mismatch";
                    r.detail = "manifest 出现非法路径: " + rel;
                    return r;
                }
                i = j + 1;
            }
        }
        const std::string want = kv.second.is_string() ? kv.second.as_string("") : std::string();
        const fs::path dst = fs::path(full) / rel;
        fs::create_directories(dst.parent_path(), ec);
        // 优先取 payload 里的新文件
        {
            const std::string src_payload = (fs::path(staging) / rel).string();
            std::string got;
            if (fs::is_regular_file(src_payload, ec) && sha256_file_hex(src_payload, &got) &&
                got == want) {
                fs::copy_file(src_payload, dst, fs::copy_options::overwrite_existing, ec);
                continue;
            }
        }
        // 否则取当前 release 树里的未变文件
        {
            const std::string src_cur = (fs::path(current_tree) / rel).string();
            std::string got;
            if (fs::is_regular_file(src_cur, ec) && sha256_file_hex(src_cur, &got) &&
                got == want) {
                fs::copy_file(src_cur, dst, fs::copy_options::overwrite_existing, ec);
                continue;
            }
        }
        r.state = "delta_base_mismatch";
        r.detail = "增量合并缺文件（payload 与当前树都没有/哈希不符）: " + rel;
        return r;
    }
    // manifest 自身不在 files_sha256 清单里，必须单独带上
    // （2026-09-19 板端实测：漏带 ⇒ ⑥ 全量复验读不到 manifest ⇒ manifest_mismatch）
    {
        const std::string manifest_src = (fs::path(staging) / "RELEASE_MANIFEST.json").string();
        if (fs::is_regular_file(manifest_src, ec)) {
            fs::copy_file(manifest_src, fs::path(full) / "RELEASE_MANIFEST.json",
                          fs::copy_options::overwrite_existing, ec);
        }
    }
    fs::remove_all(staging, ec);
    fs::rename(full, staging, ec);
    if (ec) {
        r.state = "delta_rename_failed";
        r.detail = "增量合并目录替换失败: " + ec.message();
        return r;
    }
    r.ok = true;
    return r;
}

// 递归删除目录树；成功返回 true。
bool remove_tree(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

}  // namespace ttbox::core::ota
