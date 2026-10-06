// OtaUpdater.cpp — 见 OtaUpdater.hpp 的文件头说明。
#include "ota/OtaUpdater.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "common/Json.hpp"
#include "common/Shell.hpp"
#include "ota/OtaCanonical.hpp"
#include "ota/OtaCrypto.hpp"
#include "ota/OtaExtract.hpp"
#include "ota/OtaVersion.hpp"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ttbox::core::ota {

namespace {

namespace fs = std::filesystem;

const char* kSignedFields[] = {"sha256", "version", "built_at", "key_id"};

std::string prefix_path(const std::string& prefix, const std::string& name) {
    std::string root = prefix;
    while (!root.empty() && root.back() == '/') root.pop_back();
    return root + "/" + name;
}

std::string now_iso8601_utc() {
    // 与 Python _progress 的时间格式对齐（strftime %Y-%m-%dT%H:%M:%SZ）
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

int fail_with(const OtaEnv& env, const std::string& state, const std::string& detail) {
    JsonValue o = JsonValue::object();
    o.set("state", JsonValue::string("FAILED"));
    o.set("error", JsonValue::string(state));
    o.set("detail", JsonValue::string(detail));
    o.set("at", JsonValue::string(now_iso8601_utc()));
    write_status(env, o.dump());
    std::fprintf(stderr, "[ota] FAILED %s: %s\n", state.c_str(), detail.c_str());
    return 1;
}

bool is_active_unit(const std::string& unit) {
    return run_quiet_cmd("systemctl is-active --quiet " + shell_quote(unit)) == 0;
}

// core IPC GET_STATUS：连 unix socket 发一行 NDJSON，等一行 JSON。
// ★ socket 路径不在这里写字面量：从 ttbox-core.service 的 Environment 读
//   （与 updater 的 _ipc_socket_path 同款口径；该文件是门禁① 白名单成员）。
bool core_ipc_ready(const std::string& prefix) {
#ifdef _WIN32
    (void)prefix;
    return true;  // 本机（host）无 unix socket，跳过；板端才走真检查
#else
    std::string sock;
    const std::string unit = prefix_path(prefix, "current/deploy/systemd/ttbox-core.service");
    std::ifstream f(unit, std::ios::binary);
    if (f.good()) {
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        const size_t k = text.find("TTBOX_IPC_SOCKET=");
        if (k != std::string::npos) {
            size_t b = k + std::strlen("TTBOX_IPC_SOCKET=");
            size_t e = text.find('\n', b);
            if (e == std::string::npos) e = text.size();
            sock = text.substr(b, e - b);
        }
    }
    if (sock.empty()) return false;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    struct timeval tv {};
    tv.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    const char* req = "{\"type\":\"GET_STATUS\"}\n";
    const ssize_t sent = ::write(fd, req, std::strlen(req));
    (void)sent;
    char buf[65536];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    ::close(fd);
    if (n <= 0) return false;
    const std::string resp(buf, static_cast<size_t>(n));
    // 判据对齐 Python _core_ipc_ready：能解析出非空 version
    const JsonParseResult pr = json_parse(resp);
    if (!pr.ok || !pr.value.is_object()) return false;
    const JsonValue* data = pr.value.find("data");
    if (data == nullptr || !data->is_object()) return false;
    const JsonValue* ver = data->find("version");
    return ver != nullptr && !ver->as_string("").empty();
#endif
}

}  // namespace

std::string current_version(const std::string& prefix) {
    // current 软链目录名优先
    const std::string current = prefix_path(prefix, "current");
    std::error_code ec;
    const fs::path real = fs::canonical(current, ec);
    if (!ec) {
        const std::string base = real.filename().string();
        if (!base.empty() && base != "current") return base;
    }
    // 回退 <prefix>/state/version
    std::ifstream f(prefix_path(prefix, "state/version"), std::ios::binary);
    if (f.good()) {
        std::string s;
        std::getline(f, s);
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    }
    return std::string();
}

void write_status(const OtaEnv& env, const std::string& json_body) {
    const std::string dir = prefix_path(env.prefix, "state");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = dir + "/ota_status.json";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.good()) return;
        f << json_body << "\n";
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        std::error_code e2;
        fs::rename(tmp, path, e2);
    }
}

bool fetch_file(const std::string& url, const std::string& dest, std::string* err) {
    std::string last;
    for (int i = 1; i <= 3; ++i) {
        // -f 失败返回非 0；-s 静默；--max-time 120（2026-09-19：七牛隧道到板端 ~12KB/s
        // 且偶发 >60s 停顿，60s 会一卡就整包失败）；-o 原子写目标
        const std::string cmd = "curl -fsS --max-time 120 -o " + shell_quote(dest) + " " +
                                shell_quote(url);
        if (run_quiet_cmd(cmd) == 0) return true;
        last = "curl rc!=0";
        std::fprintf(stderr, "[ota] 下载第 %d/3 次失败: %s\n", i, last.c_str());
        if (i < 3) std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if (err != nullptr) *err = last;
    return false;
}

bool verify_package(const std::string& tgz_path, const std::string& signs_text,
                    const std::string& key_id, const OtaEnv& env, std::string* out_sha256,
                    std::string* fail_state, std::string* fail_detail) {
    const JsonParseResult pr = json_parse(signs_text);
    if (!pr.ok || !pr.value.is_object()) {
        *fail_state = "sign_unreadable";
        *fail_detail = "旁车签名不可解析";
        return false;
    }
    const JsonValue& signs = pr.value;
    // ③ 完整性
    std::string actual;
    if (!sha256_file_hex(tgz_path, &actual)) {
        *fail_state = "sha256_mismatch";
        *fail_detail = "无法计算包摘要";
        return false;
    }
    const JsonValue* rec_sha = signs.find("sha256");
    if (rec_sha == nullptr || rec_sha->as_string("") != actual) {
        *fail_state = "sha256_mismatch";
        *fail_detail = "package digest != 记录 sha256";
        return false;
    }
    // ④ 真实性：key_id 必须匹配，且 SIGNED_FIELDS 齐全
    const JsonValue* rec_key = signs.find("key_id");
    if (rec_key == nullptr || rec_key->as_string("") != key_id) {
        *fail_state = "signature_invalid";
        *fail_detail = "key_id 不匹配";
        return false;
    }
    for (const char* fld : kSignedFields) {
        if (signs.find(fld) == nullptr) {
            *fail_state = "signature_invalid";
            *fail_detail = std::string("签名记录缺字段: ") + fld;
            return false;
        }
    }
    // 公钥：<current>/deploy/keys/<key_id>.pub
    std::string safe_key;
    if (!check_safe_id(key_id, &safe_key)) {
        *fail_state = "unsafe_field";
        *fail_detail = "非法 key_id";
        return false;
    }
    const std::string pub_path =
        prefix_path(env.prefix, "current/deploy/keys/" + safe_key + ".pub");
    std::ifstream pf(pub_path, std::ios::binary);
    if (!pf.good()) {
        *fail_state = "pubkey_missing";
        *fail_detail = "无该 key_id 的公钥: " + key_id;
        return false;
    }
    std::ostringstream pss;
    pss << pf.rdbuf();
    uint8_t pk[32] = {0};
    if (!load_ed25519_pem(pss.str(), pk)) {
        *fail_state = "pubkey_missing";
        *fail_detail = "公钥 PEM 解析失败: " + key_id;
        return false;
    }
    const std::vector<std::string> fields(std::begin(kSignedFields), std::end(kSignedFields));
    const std::string canon = canonical_signed_fields(signs, fields);
    std::vector<uint8_t> sig;
    const JsonValue* sig_v = signs.find("signature");
    if (sig_v == nullptr || !base64_decode(sig_v->as_string(""), &sig) || sig.size() != 64) {
        *fail_state = "signature_invalid";
        *fail_detail = "签名编码非法";
        return false;
    }
    if (!verify_ed25519(sig.data(), canon, pk)) {
        *fail_state = "signature_invalid";
        *fail_detail = "Ed25519 验签失败";
        return false;
    }
    if (out_sha256 != nullptr) *out_sha256 = actual;
    return true;
}

bool health_check(const OtaEnv& env, int timeout_sec) {
    // 对齐 Python default_health：三服务 active **且** core IPC 已就绪
    const char* kUnits[] = {"ttbox-core", "ttbox-web", "ttbox-usbproxy"};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        bool units_ok = true;
        for (const char* u : kUnits) {
            if (!is_active_unit(u)) {
                units_ok = false;
                break;
            }
        }
        if (units_ok && core_ipc_ready(env.prefix)) return true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

int run_update(const OtaEnv& env, const std::string& url, const std::string& key_id,
               const std::string& version) {
    // ① scheme 白名单：仅 https
    if (url.rfind("https://", 0) != 0) {
        return fail_with(env, "scheme_rejected", "non-https URL: " + url);
    }
    // 进度
    {
        JsonValue o = JsonValue::object();
        o.set("state", JsonValue::string("RUNNING"));
        o.set("progress", JsonValue::number(5));
        o.set("phase", JsonValue::string("下载更新包"));
        write_status(env, o.dump());
    }
    std::error_code ec;
    const std::string base = (fs::temp_directory_path(ec) / "ttbox-ota").string();
    fs::create_directories(base, ec);
    const std::string work = (fs::path(base) / ("job-" + std::to_string(::getpid()))).string();
    fs::create_directories(work, ec);
    const std::string tgz = work + "/pkg.tgz";
    const std::string sign_path = work + "/pkg.tgz.sign.json";
    std::string staging;

    // RAII：无论成败，临时目录与 staging 零残留（§0.2）
    struct Cleanup {
        std::string work, staging;
        ~Cleanup() {
            std::error_code e;
            if (!work.empty()) fs::remove_all(work, e);
            if (!staging.empty()) fs::remove_all(staging, e);
        }
    } cleanup{work, ""};

    std::string err;
    if (!fetch_file(url, tgz, &err)) {
        return fail_with(env, "download_failed", "下载包失败: " + err);
    }
    if (!fetch_file(url + ".sign.json", sign_path, &err)) {
        return fail_with(env, "download_failed", "下载旁车签名失败: " + err);
    }
    std::string actual_sha;
    std::string fstate, fdetail;
    {
        std::ifstream f(sign_path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!verify_package(tgz, ss.str(), key_id, env, &actual_sha, &fstate, &fdetail)) {
            return fail_with(env, fstate, fdetail);
        }
    }
    // 版本：入参优先，否则取签名记录
    std::string ver = version;
    if (ver.empty()) {
        std::ifstream f(sign_path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        const JsonParseResult pr = json_parse(ss.str());
        const JsonValue* v = pr.value.find("version");
        if (v != nullptr) ver = v->as_string("");
    }
    if (ver.empty()) return fail_with(env, "version_missing", "签名记录缺 version");
    std::string safe_ver;
    if (!check_safe_id(ver, &safe_ver)) {
        // ★ 任务文件的 version 未消毒就拼 releases 路径 ⇒ "../../x" 可把 staging 建到树外
        return fail_with(env, "unsafe_field", "非法 version: " + ver);
    }
    // staging 目录名 <ver>.ota.staging：不能与 release_install 的 <ver>.staging 同名
    // （否则其 tar 管道自拷自，RELEASE_MANIFEST.json 会在拷贝中消失 —— 2026-09-18 板端实测）
    const std::string releases = prefix_path(env.prefix, "releases");
    staging = releases + "/" + safe_ver + ".ota.staging";
    cleanup.staging = staging;

    const ExtractResult ex = extract_package(tgz, staging);
    if (!ex.ok) return fail_with(env, ex.state, ex.detail);

    // manifest 的 version 是安装版本号的唯一真源（2026-09-20 板端教训：
    // 面板发起的 delta job 无 version 字段，从旁车取到 "1.5.6-delta-from-1.5.5" ⇒ 目录名被污染）
    JsonValue mdoc;
    if (read_manifest_json(staging, &mdoc)) {
        const JsonValue* mv = mdoc.find("version");
        const std::string mver = mv != nullptr ? mv->as_string("") : std::string();
        if (!mver.empty()) {
            std::string safe_mver;
            if (!check_safe_id(mver, &safe_mver)) {
                return fail_with(env, "unsafe_field", "非法 manifest version: " + mver);
            }
            ver = mver;
        }
        const JsonValue* is_delta = mdoc.find("delta");
        if (is_delta != nullptr && is_delta->is_bool() && is_delta->as_bool(false)) {
            const JsonValue* bv = mdoc.find("base_version");
            const std::string base_ver = bv != nullptr ? bv->as_string("") : std::string();
            const std::string cur0 = current_version(env.prefix);
            if (base_ver != cur0) {
                return fail_with(env, "delta_base_mismatch",
                                 "增量包基线 " + (base_ver.empty() ? "<缺>" : base_ver) +
                                     " != 当前版本 " + (cur0.empty() ? "<未知>" : cur0) +
                                     "，请在面板改用全量包");
            }
            const ExtractResult dr =
                materialize_delta(staging, mdoc, releases + "/" + base_ver);
            if (!dr.ok) return fail_with(env, dr.state, dr.detail);
        }
    }
    // 全量复验
    {
        std::string bad;
        if (!verify_manifest_files(mdoc, staging, &bad)) {
            return fail_with(env, "manifest_mismatch", "staging sha256 != RELEASE_MANIFEST: " + bad);
        }
    }
    // 降级拒绝：包必须新于当前版本
    {
        const std::string cur = current_version(env.prefix);
        if (is_downgrade(ver, cur)) {
            return fail_with(env, "downgrade_rejected",
                             "包版本 " + ver + " 不高于当前版本 " +
                                 (cur.empty() ? "<未知>" : cur) + "（禁止降级）");
        }
    }
    // 调安装脚本
    const std::string install_script = prefix_path(env.prefix, "current/scripts/ttbox_release_install.sh");
    {
        JsonValue o = JsonValue::object();
        o.set("state", JsonValue::string("RUNNING"));
        o.set("progress", JsonValue::number(65));
        o.set("phase", JsonValue::string("安装新版本"));
        o.set("version", JsonValue::string(ver));
        write_status(env, o.dump());
    }
    const int rc = run_quiet_cmd(install_script + " " + shell_quote(ver) + " " +
                                 shell_quote(staging) + " --activate");
    if (rc != 0) {
        return fail_with(env, "install_failed", "release_install rc=" + std::to_string(rc));
    }
    // 健康检查；失败自动回滚（保命路径，不受降级限制）
    {
        JsonValue o = JsonValue::object();
        o.set("state", JsonValue::string("RUNNING"));
        o.set("progress", JsonValue::number(85));
        o.set("phase", JsonValue::string("重启服务，健康检查"));
        o.set("version", JsonValue::string(ver));
        write_status(env, o.dump());
    }
    if (!health_check(env, env.health_timeout_sec)) {
        run_quiet_cmd(install_script + " --rollback");
        return fail_with(env, "health_check_failed", "已 rollback");
    }
    JsonValue ok = JsonValue::object();
    ok.set("state", JsonValue::string("SUCCESS"));
    ok.set("progress", JsonValue::number(100));
    ok.set("version", JsonValue::string(ver));
    ok.set("sha256", JsonValue::string(actual_sha));
    ok.set("key_id", JsonValue::string(key_id));
    ok.set("at", JsonValue::string(now_iso8601_utc()));
    write_status(env, ok.dump());
    return 0;
}

int process_jobs(const OtaEnv& env, const std::string& jobs_dir) {
    std::error_code ec;
    const fs::path jp(jobs_dir);
    if (!fs::is_directory(jp, ec)) {
        std::fprintf(stderr, "[ota] 任务目录不存在: %s\n", jobs_dir.c_str());
        return 0;
    }
    const fs::path done_dir = jp / "processed";
    const fs::path fail_dir = jp / "failed";
    std::vector<fs::path> jobs;
    for (const auto& e : fs::directory_iterator(jp, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".json") jobs.push_back(e.path());
    }
    std::sort(jobs.begin(), jobs.end());  // 对齐 Python sorted(glob)
    int rc_all = 0;
    for (const fs::path& f : jobs) {
        const std::time_t t = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        int rc = 1;
        std::ifstream in(f, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        const JsonParseResult pr = json_parse(ss.str());
        const JsonValue* url_v = pr.ok && pr.value.is_object() ? pr.value.find("url") : nullptr;
        const JsonValue* key_v = pr.ok && pr.value.is_object() ? pr.value.find("key_id") : nullptr;
        const JsonValue* ver_v = pr.ok && pr.value.is_object() ? pr.value.find("version") : nullptr;
        const std::string url = url_v != nullptr ? url_v->as_string("") : std::string();
        std::string key_id = key_v != nullptr ? key_v->as_string("") : std::string();
        if (key_id.empty()) key_id = env.default_key_id;
        const std::string ver = ver_v != nullptr ? ver_v->as_string("") : std::string();
        if (url.empty()) {
            // 任务文件本身坏 ⇒ 记失败，绝不留在原地造成触发循环
            JsonValue o = JsonValue::object();
            o.set("state", JsonValue::string("FAILED"));
            o.set("error", JsonValue::string("job_invalid"));
            o.set("detail", JsonValue::string("任务文件缺 url: " + f.filename().string()));
            write_status(env, o.dump());
            std::fprintf(stderr, "[ota] 任务文件不可用: %s: 缺 url\n", f.filename().c_str());
        } else {
            rc = run_update(env, url, key_id, ver);
            if (rc != 0) rc_all = 1;
        }
        // ★ 处理完必须移走：否则 path 单元会因文件仍在而再次触发
        const fs::path dest_dir = (rc == 0) ? done_dir : fail_dir;
        std::error_code e2;
        fs::create_directories(dest_dir, e2);
        const fs::path dest = dest_dir / (std::string(stamp) + "-" + f.filename().string());
        std::error_code e3;
        fs::rename(f, dest, e3);
        if (e3) {
            std::fprintf(stderr, "[warn] 任务文件归档失败（原地删除以防触发循环）: %s\n",
                         e3.message().c_str());
            std::error_code e4;
            fs::remove(f, e4);
        }
        if (rc != 0) rc_all = 1;
    }
    return rc_all;
}

}  // namespace ttbox::core::ota
