// pages.cpp — 页面路由域（/ /desktop /mobile /activate，URL 一字未改）。
//
// 自 plugins/web/api/pages.py 逐行为移植。无 Jinja2 运行时：仅做两种占位符替换
// （{{ url_for('static', filename=…) }} → /static/…；{{ app_title/default_theme/ui_skin }}
//  → 品牌投影），模板本身照读照发。激活 gate 对齐 before_request：需激活页面未激活
//  ⇒ 302 /activate；/activate 已激活 ⇒ 302 /。
#include "web/domain/domain_routes.hpp"

#include <cstdlib>
#include <mutex>
#include <string>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_payloads.hpp"
#include "web/domain/sysinfo_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// 模板目录（<TTBOX_ROOT>/templates；与 WebServer::template_dir_ 同口径）。
std::string template_dir() {
    const char* root = std::getenv("TTBOX_ROOT");
    const std::string r = (root != nullptr && *root != '\0') ? std::string(root) : std::string(".");
    return join_path(r, "templates");
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// {{ url_for('static', filename='X') }} → /static/X。
void substitute_url_for_static(std::string& body) {
    const std::string needle = "{{ url_for('static', filename='";
    const std::string closer = "') }}";
    size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        const size_t start = pos + needle.size();
        const size_t end = body.find(closer, start);
        if (end == std::string::npos) break;
        const std::string filename = body.substr(start, end - start);
        const std::string replacement = "/static/" + filename;
        body.replace(pos, end + closer.size() - pos, replacement);
        pos += replacement.size();
    }
}

// 渲染模板（品牌投影默认 ttbox；渠道皮肤 registry 下沉后续批次）。
std::string render_page(const std::string& name) {
    std::string body;
    if (!read_file(join_path(template_dir(), name), &body)) return "";
    const JsonValue ui = ui_block("ttbox");
    replace_all(body, "{{ app_title }}", json_field(ui, "app_title").as_string("TTBOX 控制台"));
    replace_all(body, "{{ default_theme }}", json_field(ui, "default_theme").as_string("dark"));
    replace_all(body, "{{ ui_skin }}", json_field(ui, "skin").as_string("yu"));
    substitute_url_for_static(body);
    return body;
}

// core 是否有效授权（_activation_ok，带 2s TTL 缓存）。
bool activation_ok(IpcClient& ipc) {
    static std::mutex mu;
    static double ts = 0.0;
    static bool ok = false;
    std::lock_guard<std::mutex> lk(mu);
    const double now = now_seconds();
    if (now - ts <= 2.0) return ok;
    ok = json_truthy(json_field(license_block(ipc), "activated"));
    ts = now;
    return ok;
}

void serve_html(httplib::Response& res, const std::string& body) {
    if (body.empty()) {
        res.status = 404;
        res.set_content(R"({"ok":false,"error":"not_found"})", "application/json");
        return;
    }
    res.set_content(body, "text/html; charset=utf-8");
}

}  // namespace

void register_pages_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/", [&ipc](const httplib::Request&, httplib::Response& res) {
        if (!activation_ok(ipc)) {
            res.set_redirect("/activate");
            return;
        }
        serve_html(res, render_page("index.html"));
    });
    svr.Get("/desktop", [&ipc](const httplib::Request&, httplib::Response& res) {
        if (!activation_ok(ipc)) {
            res.set_redirect("/activate");
            return;
        }
        serve_html(res, render_page("index.html"));
    });
    svr.Get("/mobile", [&ipc](const httplib::Request&, httplib::Response& res) {
        if (!activation_ok(ipc)) {
            res.set_redirect("/activate");
            return;
        }
        serve_html(res, render_page("index.html"));
    });
    svr.Get("/activate", [&ipc](const httplib::Request&, httplib::Response& res) {
        if (activation_ok(ipc)) {
            res.set_redirect("/");
            return;
        }
        serve_html(res, render_page("activate.html"));
    });
}

}  // namespace ttbox::core::web
