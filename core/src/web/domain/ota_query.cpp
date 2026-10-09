// ota_query.cpp — OTA 状态与更新检查实现（install 见 ota_impl.cpp）。
//
// 自 plugins/web/api/ota.py 与 lib/ota.py 逐行为移植。URL 一字未改。
#include "web/domain/ota_internal.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "common/Paths.hpp"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_routes.hpp"

namespace ttbox::core::web {

namespace {

// 更新器安装结果文件路径（<state>/ota_status.json）。
std::string ota_status_file() {
    return join_path(state_dir(), ttbox::core::paths::kOtaStatusFileName);
}

// 文件 mtime（unix 秒）；读不到 → 0。
int64_t file_mtime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch());
    return static_cast<int64_t>(sec.count());
}

// OTA 分发服务器。★ V1.0.52：原与 plugins/web/bin/ttbox-web.py 同值，该文件随 Python
// 清理移除 ⇒ 现只与 scripts/ttbox.sh::OTA_SERVER_URL 保持同值（仍是 env 可覆盖）。
std::string ota_server_url() {
    const char* v = std::getenv("TTBOX_OTA_SERVER_URL");
    return (v != nullptr && *v != '\0') ? std::string(v) : "https://47.104.18.178:10086/ota";
}

// 版本号切段（数字段按值、其余按字典序；对齐 ota._ota_ver_key）。
// 版本号切出的一个段（数字段按值比较、其余按字典序）。
struct VerSeg {
    bool numeric = false;
    int64_t num = 0;
    std::string str;
};

// 把版本号按数字段/非数字段切成序列，供逐段比较（对齐 _ota_ver_key 的解析）。
std::vector<VerSeg> ota_ver_segs(const std::string& v) {
    std::vector<VerSeg> out;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return;
        VerSeg s;
        s.numeric = true;
        for (char c : cur) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                s.numeric = false;
                break;
            }
        }
        if (s.numeric) {
            s.num = std::strtoll(cur.c_str(), nullptr, 10);
        } else {
            s.str = cur;
        }
        out.push_back(s);
        cur.clear();
    };
    for (char c : v) {
        if (c == '.' || c == '-' || c == '_' || c == '+') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

// 版本比较：逐段按「数字段 < 字符串段」规则，数值/字典序分别比较。
int ota_ver_key_cmp(const std::string& a, const std::string& b) {
    const std::vector<VerSeg> sa = ota_ver_segs(a);
    const std::vector<VerSeg> sb = ota_ver_segs(b);
    const size_t n = sa.size() < sb.size() ? sa.size() : sb.size();
    for (size_t i = 0; i < n; ++i) {
        const VerSeg& x = sa[i];
        const VerSeg& y = sb[i];
        if (x.numeric != y.numeric) return x.numeric ? -1 : 1;  // 数字段 < 字符串段
        if (x.numeric) {
            if (x.num != y.num) return x.num < y.num ? -1 : 1;
        } else {
            if (x.str != y.str) return x.str < y.str ? -1 : 1;
        }
    }
    if (sa.size() != sb.size()) return sa.size() < sb.size() ? -1 : 1;
    return 0;
}

// 把 OTA_SERVER_URL 拆成 (scheme://host:port, base_path)。
bool ota_split_url(const std::string& url, std::string* shp, std::string* base) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    const size_t slash = url.find('/', scheme + 3);
    if (slash == std::string::npos) {
        *shp = url;
        *base = "";
    } else {
        *shp = url.substr(0, slash);
        *base = url.substr(slash);
    }
    return true;
}

}  // namespace

// 读 ota_status.json 并投影为面板状态（idle/running/success/failed，含超时判死）。
void ota_status_impl(httplib::Response& res) {
    const std::string path = ota_status_file();
    JsonValue doc = JsonValue::object();
    std::string text;
    if (read_file(path, &text)) {
        const JsonParseResult r = ttbox::core::json_parse(text);
        if (r.ok && r.value.is_object()) doc = r.value;
    }
    std::string state = json_field(doc, "state").as_string("");
    for (char& c : state) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    JsonValue data = JsonValue::object();
    if (state == "RUNNING") {
        int64_t progress = json_field(doc, "progress").as_int(10);
        if (progress < 1) progress = 1;
        if (progress > 99) progress = 99;
        const bool stale = (std::time(nullptr) - file_mtime(path)) > 1800;
        if (stale) {
            data.set("status", JsonValue::string("failed"));
            data.set("progress", JsonValue::number(100.0));
            data.set("error", JsonValue::string("更新进程中断（状态超过30分钟无进展）"));
        } else {
            data.set("status", JsonValue::string("running"));
            data.set("progress", JsonValue::number(static_cast<double>(progress)));
            data.set("message", JsonValue::string(
                !json_field(doc, "phase").as_string("").empty()
                    ? json_field(doc, "phase").as_string("")
                    : "更新进行中"));
            data.set("version", JsonValue::string(json_field(doc, "version").as_string("")));
        }
    } else if (state == "SUCCESS") {
        data.set("status", JsonValue::string("success"));
        data.set("progress", JsonValue::number(100.0));
        data.set("version", JsonValue::string(json_field(doc, "version").as_string("")));
        data.set("finished_at", JsonValue::number(static_cast<double>(file_mtime(path))));
    } else if (state == "FAILED") {
        data.set("status", JsonValue::string("failed"));
        data.set("progress", JsonValue::number(100.0));
        const std::string detail = json_field(doc, "detail").as_string("");
        const std::string err = json_field(doc, "error").as_string("");
        data.set("error", JsonValue::string(
            !detail.empty() ? detail : (!err.empty() ? err : "更新失败")));
        data.set("finished_at", JsonValue::number(static_cast<double>(file_mtime(path))));
    } else {
        data.set("status", JsonValue::string("idle"));
    }
    send_json(res, 200, true, data, "", "");
}

// 向 OTA 服务器拉 latest 并比对当前版本，回报是否有更新 + 包/签名地址。
void ota_check_impl(IpcClient& ipc, httplib::Response& res) {
    const std::string server = ota_server_url();
    if (server.find("example.com") != std::string::npos ||
        server.compare(0, 8, "https://") != 0) {
        send_json(res, 503, false, JsonValue::object(), "ota_server_not_configured",
                  "OTA_SERVER_URL 必须是 https");
        return;
    }
    const std::string cur = ota_current_version(ipc);
    std::string shp;
    std::string base;
    if (!ota_split_url(server, &shp, &base)) {
        send_json(res, 502, false, JsonValue::object(), "ota_server_unreachable",
                  "OTA 服务器地址非法");
        return;
    }
    httplib::Client cli(shp);
    cli.set_connection_timeout(6, 0);
    cli.set_read_timeout(6, 0);
    const std::string path = base + "/latest?current=" + cur;
    const auto resp = cli.Get(path.c_str());
    if (!resp) {
        send_json(res, 502, false, JsonValue::object(), "ota_server_unreachable",
                  "OTA 服务器不可达");
        return;
    }
    const JsonParseResult pr = ttbox::core::json_parse(resp->body);
    if (!pr.ok || !pr.value.is_object() ||
        json_field(pr.value, "latest_version").as_string("").empty()) {
        send_json(res, 502, false, JsonValue::object(), "ota_server_unreachable",
                  "服务器响应缺 latest_version");
        return;
    }
    const std::string ver = json_field(pr.value, "latest_version").as_string("");
    const std::string pkg = json_field(pr.value, "package_url").as_string("");
    const std::string sig =
        !json_field(pr.value, "sign_url").as_string("").empty()
            ? json_field(pr.value, "sign_url").as_string("")
            : (pkg.empty() ? "" : pkg + ".sign.json");
    const bool update_available =
        !ver.empty() && !pkg.empty() && (cur.empty() || ota_ver_key_cmp(ver, cur) > 0);
    JsonValue data = JsonValue::object();
    data.set("update_available", JsonValue::boolean(update_available));
    data.set("current_version", JsonValue::string(cur));
    data.set("latest_version", JsonValue::string(ver));
    data.set("package_url", JsonValue::string(pkg));
    data.set("sign_url", JsonValue::string(sig));
    data.set("full_package_url", JsonValue::string(pkg));
    data.set("full_sign_url", JsonValue::string(sig));
    data.set("delta", JsonValue::boolean(false));  // 增量包探测下沉后续批次
    data.set("key_id", JsonValue::string(ota_default_key_id()));
    send_json(res, 200, true, data, "", "");
}

}  // namespace ttbox::core::web
