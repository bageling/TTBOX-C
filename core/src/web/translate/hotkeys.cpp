// hotkeys.cpp — 见 .hpp 头注释。
#include "web/translate/hotkeys.hpp"

#include <cctype>

namespace ttbox::core::web {

namespace {

// 热键名 ↔ 位掩码表项。
struct HotkeyEntry {
    const char* name;
    int64_t bits;
};

// 顺序即错误文案里的枚举顺序；位值与 core/src/mouse/MouseTypes.hpp 的位掩码定义一致。
constexpr HotkeyEntry kHotkeyTable[] = {
    {"left", 1}, {"right", 2}, {"middle", 4}, {"back", 8}, {"forward", 16},
};

// strip + lower（对齐 Python str.strip().lower()）。
std::string normalize_name(const std::string& raw) {
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) {
        --end;
    }
    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(raw[i]))));
    }
    return out;
}

}  // namespace

// 判定掩码是否为合法单键位（左/右/中/侧1/侧2 之一）。
bool is_valid_hotkey_bit(int64_t bits) {
    for (const HotkeyEntry& e : kHotkeyTable) {
        if (e.bits == bits) return true;
    }
    return false;
}

// Web 热键字符串（或数字）→ 位掩码；未识别回退 default_bits。
int64_t hotkey_to_bits(const JsonValue& value, int64_t default_bits) {
    if (value.is_string()) {
        const std::string name = normalize_name(value.as_string());
        for (const HotkeyEntry& e : kHotkeyTable) {
            if (name == e.name) return e.bits;
        }
        return default_bits;
    }
    if (value.is_number()) return value.as_int();
    return default_bits;
}

// 位掩码 → Web 热键字符串（未识别返回空串）。
std::string bits_to_hotkey(int64_t bits) {
    for (const HotkeyEntry& e : kHotkeyTable) {
        if (e.bits == bits) return e.name;
    }
    return "";
}

// 位掩码 → 英文名（供错误文案；与 bits_to_hotkey 同源）。
std::string hotkey_name(int64_t bits) {
    return bits_to_hotkey(bits);
}

// Core hotkey_guard → 前端控件值（缺字段补 enabled=false / toggle_hotkey=middle）。
JsonValue hotkey_guard_to_web(const JsonValue& hotkey_guard) {
    JsonValue out = JsonValue::object();
    const JsonValue* enabled =
        hotkey_guard.is_object() ? hotkey_guard.find("enabled") : nullptr;
    const JsonValue* toggle =
        hotkey_guard.is_object() ? hotkey_guard.find("toggle_hotkey") : nullptr;

    out.set("enabled", JsonValue::boolean(enabled != nullptr && enabled->as_bool()));
    const int64_t bits = toggle != nullptr ? toggle->as_int(4) : 4;
    const std::string name = bits_to_hotkey(bits);
    out.set("toggle_hotkey", JsonValue::string(name.empty() ? "middle" : name));
    return out;
}

}  // namespace ttbox::core::web
