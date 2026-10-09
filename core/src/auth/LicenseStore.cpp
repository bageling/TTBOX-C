// LicenseStore.cpp — T1.09 授权持久化（施工图：t1.09-t1.10-impl-spec.md §2.2）
//
// 设计要点（逐条对齐规格，改动前请先读 §2.2 / §6 陷阱清单）：
//   · 原子写五步（缺一不可）：
//       ① 同目录建 tmp（<path>.tmp.<pid>，同一文件系统 ⇒ rename 原子）
//       ② 写全部字节 + fsync(fd) + close      —— 掉电不丢已 fsync 内容
//       ③ 回读 tmp 与 content 逐字节比对       —— 自证"写的就是想写的"（本项目"沉默失败"多出于省掉自证）
//       ④ rename(tmp, path)                    —— 旧文件要么完整、要么被完整替换
//       ⑤ fsync(父目录)                        —— 目录项落盘
//     任一步失败：unlink(tmp) + 返回 false，**绝不触碰旧文件**（无"先删后写"窗口）。
//   · load() 自愈：state.json 缺失 ⇒ 基线全 0（STATE_MISSING）；
//     state.json 截断/非法 JSON ⇒ **不信任其内容**，last_seen_issued_at 从 license.json 的
//     issued_at 重建（STATE_RECOVERED_FRESH）。否则攻击者破坏 state.json 即可回放旧的
//     低 issued_at 文档 ⇒ 反降级失效。
//   · 依赖：文件 I-O（open/fsync|commit/rename|MoveFileEx/stat/dirent）+ 仓库内置 Json；无 OpenSSL。
//
// 可移植性（★ 本轮修复）：
//   本 TU 在 CORE_SOURCES 的**无条件列表** ⇒ POSIX 与 Windows/MinGW 都必须编译通过。
//   以下 POSIX-only 调用按平台分派（Linux 分支行为逐字节不变）：
//     · mkdir(dir, 0700)   → Windows 无 mode 参数 ⇒ ::_mkdir(dir)（<direct.h>）
//     · fsync(fd)          → Windows 无 fsync     ⇒ ::_commit(fd)（<io.h>）
//     · fsync(父目录)       → Windows 不支持目录 fsync ⇒ best-effort no-op
//     · rename(tmp,path)   → MSVCRT rename 目标存在即失败，破坏"原子替换" ⇒
//                            Windows 走 MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)（<windows.h>）
#include "auth/LicenseStore.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>   // ::_mkdir
#include <io.h>       // ::_commit
#include <windows.h>  // ::MoveFileExA / MOVEFILE_REPLACE_EXISTING
#endif

#include <string>
#include <utility>

#include "common/Json.hpp"
#include "common/Logger.hpp"

namespace ttbox::core::auth {

namespace {

constexpr const char* kDocFile = "license.json";
constexpr const char* kStateFile = "state.json";
constexpr const char* kTmpMarker = ".tmp.";

// 非空时把错误信息写入调用方指针。
void set_err(std::string* error, const std::string& msg) {
    if (error != nullptr) *error = msg;
}

// 拼接目录与文件名（目录为空或已带 '/' 时避免重复分隔符）。
std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

// 判断路径是否存在（stat 成功即视为存在）。
bool path_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// 落盘一个 fd 的内容：POSIX=fsync；Windows=_commit（同义：把已写缓冲持久化）。
int sync_fd(int fd) {
#ifdef _WIN32
    return ::_commit(fd);
#else
    return ::fsync(fd);
#endif
}

// 原子替换 tmp→path：POSIX=rename（原子覆盖）；Windows=MoveFileExA(REPLACE_EXISTING)。
bool replace_file(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return ::MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// best-effort：确保目录存在（0700）。失败不致命，交由后续 open 报错。
void ensure_dir(const std::string& dir) {
    if (dir.empty()) return;
#ifdef _WIN32
    ::_mkdir(dir.c_str());  // Windows：单参；EEXIST 忽略
#else
    ::mkdir(dir.c_str(), 0700);  // EEXIST 忽略
#endif
}

// 读取整个文件（含二进制安全）。失败置 error 返回 false。
bool read_file(const std::string& path, std::string* out, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        set_err(error, std::string("open failed: ") + std::strerror(errno));
        return false;
    }
    std::string data;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            set_err(error, std::string("read failed: ") + std::strerror(errno));
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        data.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    if (out != nullptr) *out = std::move(data);
    return true;
}

// 删除目录内遗留的 *.tmp.*（上次崩溃残留）；返回是否删过。
bool remove_stale_tmps(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    bool removed = false;
    struct dirent* e = nullptr;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name.find(kTmpMarker) != std::string::npos) {
            ::unlink(join_path(dir, name).c_str());
            removed = true;
        }
    }
    ::closedir(d);
    return removed;
}

// ⑤ fsync 父目录：让 rename 的目录项落盘。best-effort（部分平台/FS 不支持 O_RDONLY 打开目录）。
void fsync_parent_dir(const std::string& path) {
#ifdef _WIN32
    (void)path;  // Windows 不支持目录 fsync：best-effort no-op（文件系统语义已保证替换可见）
#else
    const std::size_t slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd < 0) return;
    ::fsync(dfd);  // 失败忽略（EINVAL：目录不支持 fsync 的平台）
    ::close(dfd);
#endif
}

}  // namespace

// 构造：记录授权目录（默认 /var/lib/ttbox/license）。
LicenseStore::LicenseStore(std::string dir) : dir_(std::move(dir)) {}

// 防降级判定：本次文档的 issued_at 早于已记录基线即为降级。
bool LicenseStore::is_downgrade(int64_t doc_issued_at, int64_t last_seen_issued_at) {
    // 同 license_id 下 issued_at 不得回退（LIC-17 防降级）。
    return doc_issued_at < last_seen_issued_at;
}

// 原子写文件：tmp 写入 + fsync + 回读自证 + rename + 目录 fsync；任一步失败即清理 tmp 返回 false。
bool LicenseStore::write_file_atomic_(const std::string& path,
                                      const std::string& content,
                                      std::string* error) const {
    ensure_dir(dir_);
    const std::string tmp = path + kTmpMarker + std::to_string(static_cast<long>(::getpid()));
    ::unlink(tmp.c_str());  // 清掉同名残留，保证新写干净

    // ① 建同目录 tmp（同一文件系统 ⇒ rename 原子）
    const int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        set_err(error, std::string("open(tmp) failed: ") + std::strerror(errno));
        return false;
    }

    // ② 写全部字节 + fsync + close
    const char* p = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
        const ssize_t n = ::write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            set_err(error, std::string("write failed: ") + std::strerror(errno));
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    if (sync_fd(fd) != 0) {
        set_err(error, std::string("fsync failed: ") + std::strerror(errno));
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        set_err(error, std::string("close failed: ") + std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }

    // ③ 回读自证：写的就是想写的
    std::string back;
    std::string rerr;
    if (!read_file(tmp, &back, &rerr) || back != content) {
        set_err(error, "read-back self-check failed: " + rerr);
        ::unlink(tmp.c_str());
        return false;
    }

    // ④ rename（原子替换；旧文件要么完整、要么被完整替换）
    if (!replace_file(tmp, path)) {
        set_err(error, std::string("rename failed: ") + std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }

    // ⑤ fsync 父目录
    fsync_parent_dir(path);
    return true;
}

// 读取 license.json + state.json：清残留 tmp、解析文档、损坏时从 doc 重建回滚基线。
StoreLoadResult LicenseStore::load() const {
    StoreLoadResult r;

    // 0) 清遗留 tmp（不读、不采信）
    if (remove_stale_tmps(dir_)) {
        TTBOX_LOG_WARN(std::string("[LicenseStore] STALE_TMP_REMOVED in ") + dir_);
    }

    // 1) license.json 原文（不解析为授权语义，仅提取 issued_at 供自愈重建基线）
    const std::string doc_path = join_path(dir_, kDocFile);
    std::string doc;
    std::string doc_err;
    int64_t doc_issued_at = 0;
    if (read_file(doc_path, &doc, &doc_err) && !doc.empty()) {
        const ttbox::core::JsonParseResult pr = ttbox::core::json_parse(doc);
        if (pr.ok && pr.value.is_object()) {
            r.doc_ok = true;
            r.doc_json = doc;
            // ★ M2.07（D-D）：登记 doc 是否 cloud 形（**唯一真源**）——cloud 形文档由
            //   activate_cloud() 落盘（顶层 source:"cloud"），不是 Ed25519 签名信封；
            //   上层据此**不得**把它交给离线验签链（否则重启后云态被抹，见 D-D）。
            if (const ttbox::core::JsonValue* src = pr.value.find("source")) {
                r.doc_is_cloud = (src->is_string() && src->as_string() == "cloud");
            }
            // ★ F4：防回滚基线必须取**卡真实的** issued_at。
            //   M2 卡的 issued_at 位于嵌套 license 对象内（见 LicenseCard.hpp canonical 定义）；
            //   若只读顶层 `issued_at`，M2 文档基线恒为 0 ⇒ 反降级形同虚设。
            //   读取顺序：license.issued_at（M2，权威）→ 顶层 issued_at（M1 旧格式兜底）。
            int64_t issued_at = 0;
            bool found = false;
            if (const ttbox::core::JsonValue* lic = pr.value.find("license")) {
                if (lic->is_object()) {
                    if (const ttbox::core::JsonValue* ia = lic->find("issued_at")) {
                        issued_at = ia->as_int(0);
                        found = true;
                    }
                }
            }
            if (!found) {
                if (const ttbox::core::JsonValue* ia = pr.value.find("issued_at")) {
                    issued_at = ia->as_int(0);
                }
            }
            doc_issued_at = issued_at;
        } else {
            r.doc_ok = false;
            r.error = "LICENSE_DOC_CORRUPT";
            TTBOX_LOG_WARN(std::string("[LicenseStore] license.json 非法/截断 ⇒ doc_ok=false：") +
                           (pr.ok ? std::string("非对象") : pr.error));
        }
    } else {
        r.doc_ok = false;
        r.error = "LICENSE_DOC_MISSING";
    }
    ::chmod(doc_path.c_str(), 0600);  // best-effort 权限兜底

    // 2) state.json（派生状态；防回滚资产）
    const std::string state_path = join_path(dir_, kStateFile);
    if (!path_exists(state_path)) {
        r.state_ok = false;
        if (r.error.empty()) r.error = "STATE_MISSING";
        TTBOX_LOG_WARN(std::string("[LicenseStore] STATE_MISSING（基线取 0）：") + state_path);
        return r;
    }

    std::string state;
    std::string state_err;
    if (!read_file(state_path, &state, &state_err)) {
        // 存在但读不出 ⇒ 视为损坏，从 doc 重建基线
        r.state_ok = false;
        r.recovered_fresh = (doc_issued_at > 0);
        r.last_seen_issued_at = doc_issued_at;
        if (r.error.empty()) r.error = "STATE_RECOVERED_FRESH";
        TTBOX_LOG_WARN(std::string("[LicenseStore] state.json 不可读 ⇒ 从 doc 重建基线 issued_at=") +
                       std::to_string(doc_issued_at));
        return r;
    }

    const ttbox::core::JsonParseResult pr = ttbox::core::json_parse(state);
    if (!pr.ok || !pr.value.is_object()) {
        // 截断/非法 JSON ⇒ 不信任其内容；从仍然存在的签名文档重建防降级基线
        r.state_ok = false;
        r.recovered_fresh = (doc_issued_at > 0);
        r.last_seen_issued_at = doc_issued_at;
        if (r.error.empty()) r.error = "STATE_RECOVERED_FRESH";
        TTBOX_LOG_WARN(std::string("[LicenseStore] STATE_RECOVERED_FRESH：state.json 损坏 ⇒ 由 doc 重建 issued_at=") +
                       std::to_string(doc_issued_at));
        ::chmod(state_path.c_str(), 0600);
        return r;
    }

    r.state_ok = true;
    if (const ttbox::core::JsonValue* v = pr.value.find("last_seen_issued_at")) {
        r.last_seen_issued_at = v->as_int(0);
    }
    if (const ttbox::core::JsonValue* v = pr.value.find("server_time_floor_unix")) {
        r.server_time_floor_unix = v->as_int(0);
    }
    if (const ttbox::core::JsonValue* v = pr.value.find("grant_expires_at")) {
        r.grant_expires_at = v->as_int(0);
    }
    if (const ttbox::core::JsonValue* v = pr.value.find("grace_until")) {
        r.grace_until = v->as_int(0);
    }
    ::chmod(state_path.c_str(), 0600);
    return r;
}

// 空文档直接拒绝；否则原子写 license.json。
bool LicenseStore::save_doc(const std::string& doc_json, std::string* error) {
    if (doc_json.empty()) {
        set_err(error, "empty doc_json");
        return false;
    }
    return write_file_atomic_(join_path(dir_, kDocFile), doc_json, error);
}

// 把四个派生状态字段序列化为 JSON 原子写入 state.json（未给出的语义由调用方保证）。
bool LicenseStore::save_state(int64_t last_seen_issued_at,
                              int64_t server_time_floor_unix,
                              int64_t grant_expires_at,
                              int64_t grace_until,
                              std::string* error) {
    ttbox::core::JsonValue o = ttbox::core::JsonValue::object();
    o.set("last_seen_issued_at",
          ttbox::core::JsonValue::number(static_cast<double>(last_seen_issued_at)));
    o.set("server_time_floor_unix",
          ttbox::core::JsonValue::number(static_cast<double>(server_time_floor_unix)));
    o.set("grant_expires_at",
          ttbox::core::JsonValue::number(static_cast<double>(grant_expires_at)));
    o.set("grace_until", ttbox::core::JsonValue::number(static_cast<double>(grace_until)));
    return write_file_atomic_(join_path(dir_, kStateFile), o.dump(), error);
}

}  // namespace ttbox::core::auth
