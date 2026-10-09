// HidPackageConfig.cpp — HID Package 独立配置实现
#include "hid/HidPackageConfig.hpp"

#include <cstdio>
#include <fstream>

namespace ttbox::core {

namespace {

// 取对象下的子节点（不存在返回 nullptr）
const JsonValue* obj(const JsonValue& v, const char* key) {
    return v.find(key);
}
// 取字符串字段（类型不符/缺失返回 def）
std::string str(const JsonValue& o, const char* key, const std::string& def) {
    const JsonValue* v = o.find(key);
    return (v && v->is_string()) ? v->as_string(def) : def;
}
// 取整数字段（类型不符/缺失返回 def）
int64_t i64(const JsonValue& o, const char* key, int64_t def) {
    const JsonValue* v = o.find(key);
    return (v && v->is_number()) ? v->as_int(def) : def;
}

}  // namespace

// 序列化配置为 JSON（device/mouse/keyboard/thread/gadget/descriptor 分组）
JsonValue HidPackageConfig::to_json() const {
    JsonValue root = JsonValue::object();
    JsonValue dev = JsonValue::object();
    dev.set("keyboard_hidraw", JsonValue::string(keyboard_hidraw));
    dev.set("mouse_hidraw", JsonValue::string(mouse_hidraw));
    dev.set("report_rate_hz", JsonValue::number(static_cast<double>(report_rate_hz)));
    root.set("device", std::move(dev));

    JsonValue ms = JsonValue::object();
    ms.set("enabled", JsonValue::boolean(mouse_enabled));
    ms.set("buttons_enabled", JsonValue::boolean(mouse_buttons_enabled));
    ms.set("wheel_enabled", JsonValue::boolean(mouse_wheel_enabled));
    root.set("mouse", std::move(ms));

    JsonValue kb = JsonValue::object();
    kb.set("enabled", JsonValue::boolean(keyboard_enabled));
    root.set("keyboard", std::move(kb));

    JsonValue th = JsonValue::object();
    th.set("cpu_affinity", JsonValue::number(static_cast<double>(cpu_affinity)));
    th.set("queue_size", JsonValue::number(static_cast<double>(queue_size)));
    root.set("thread", std::move(th));

    JsonValue g = JsonValue::object();
    g.set("name", JsonValue::string(gadget_name));
    g.set("udc", JsonValue::string(udc));
    g.set("keyboard_hidg", JsonValue::string(keyboard_hidg));
    g.set("mouse_hidg", JsonValue::string(mouse_hidg));
    root.set("gadget", std::move(g));

    JsonValue d = JsonValue::object();
    d.set("keyboard_descriptor", JsonValue::string(keyboard_descriptor));
    d.set("mouse_descriptor", JsonValue::string(mouse_descriptor));
    root.set("descriptor", std::move(d));
    return root;
}

// 从 JSON 反序列化配置（非对象/缺字段时用默认值）
HidPackageConfig HidPackageConfig::from_json(const JsonValue& v) {
    HidPackageConfig c;
    if (!v.is_object()) return c;
    if (const JsonValue* d = obj(v, "device"); d && d->is_object()) {
        c.keyboard_hidraw = str(*d, "keyboard_hidraw", c.keyboard_hidraw);
        c.mouse_hidraw = str(*d, "mouse_hidraw", c.mouse_hidraw);
        c.report_rate_hz = static_cast<int>(i64(*d, "report_rate_hz", c.report_rate_hz));
    }
    if (const JsonValue* m = obj(v, "mouse"); m && m->is_object()) {
        if (const JsonValue* e = m->find("enabled"); e && e->is_bool())
            c.mouse_enabled = e->as_bool(c.mouse_enabled);
        if (const JsonValue* b = m->find("buttons_enabled"); b && b->is_bool())
            c.mouse_buttons_enabled = b->as_bool(c.mouse_buttons_enabled);
        if (const JsonValue* w = m->find("wheel_enabled"); w && w->is_bool())
            c.mouse_wheel_enabled = w->as_bool(c.mouse_wheel_enabled);
    }
    if (const JsonValue* k = obj(v, "keyboard"); k && k->is_object()) {
        if (const JsonValue* e = k->find("enabled"); e && e->is_bool())
            c.keyboard_enabled = e->as_bool(c.keyboard_enabled);
    }
    if (const JsonValue* t = obj(v, "thread"); t && t->is_object()) {
        c.cpu_affinity = static_cast<int>(i64(*t, "cpu_affinity", c.cpu_affinity));
        c.queue_size = static_cast<int>(i64(*t, "queue_size", c.queue_size));
    }
    if (const JsonValue* g = obj(v, "gadget"); g && g->is_object()) {
        c.gadget_name = str(*g, "name", c.gadget_name);
        c.udc = str(*g, "udc", c.udc);
        c.keyboard_hidg = str(*g, "keyboard_hidg", c.keyboard_hidg);
        c.mouse_hidg = str(*g, "mouse_hidg", c.mouse_hidg);
    }
    if (const JsonValue* d = obj(v, "descriptor"); d && d->is_object()) {
        c.keyboard_descriptor = str(*d, "keyboard_descriptor", c.keyboard_descriptor);
        c.mouse_descriptor = str(*d, "mouse_descriptor", c.mouse_descriptor);
    }
    return c;
}

// 从文件加载配置（解析失败回填 error 并返回默认配置）
HidPackageConfig HidPackageConfig::load(const std::string& path, std::string* error) {
    auto res = json_parse_file(path);
    if (!res.ok) {
        if (error) *error = "HID 配置解析失败(" + path + "): " + res.error;
        return {};
    }
    return from_json(res.value);
}

// 写入配置文件（成功返回 true）
bool HidPackageConfig::save(const std::string& path, std::string* error) const {
    const std::string text = to_json().dump();
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        if (error) *error = "无法写入 HID 配置: " + path;
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0) {
        if (error) *error = "写 HID 配置失败: " + path;
        return false;
    }
    return ok;
}

}  // namespace ttbox::core
