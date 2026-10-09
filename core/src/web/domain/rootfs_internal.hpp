// rootfs_internal.hpp — 根分区扩容可行性探测（自 plugins/web/lib/rootfs.py 逐行为移植）。
//
// 两个问题分开回答（不挤进同一布尔）：
//   expandable        = 物理上有无未分配尾空间（读分区表算）
//   action_available  = 本仓有无"改分区表 + resize2fs"执行通道（无特权 worker ⇒ false）
#pragma once

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "common/Json.hpp"
#include "web/domain/sysinfo_internal.hpp"

namespace ttbox::core::web {

// 尾部未分配空间低于此值（64 MiB）即判定不可扩容（对齐 rootfs._TAIL_MIN_BYTES）。
inline constexpr int64_t kRootfsTailMinBytes = 64LL * 1024 * 1024;

// 字节 → 人类可读（对齐 rootfs._human_bytes）。
inline std::string human_bytes(int64_t n) {
    if (n < 0) return "未知";
    double value = static_cast<double>(n);
    const char* units[] = {"B", "KB", "MB", "GB"};
    for (int i = 0; i < 4; ++i) {
        if (value < 1024.0 || i == 3) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[i]);
            return buf;
        }
        value /= 1024.0;
    }
    return "0 B";
}

// 探测（无缓存；findmnt / lsblk / sysfs 全真读数，无预置结论）。
inline JsonValue rootfs_expand_probe_uncached() {
    JsonValue probe = JsonValue::object();
    probe.set("ok", JsonValue::boolean(true));
    probe.set("supported", JsonValue::boolean(true));
    probe.set("expandable", JsonValue::boolean(false));
    probe.set("reason", JsonValue::string(""));
    probe.set("message", JsonValue::string(""));
    probe.set("method", JsonValue::string("growpart"));
    probe.set("missing_tools", JsonValue::array());
    probe.set("action_available", JsonValue::boolean(false));
    probe.set("action_reason", JsonValue::string("expand_worker_not_implemented"));

    JsonValue root = JsonValue::object();
    const std::string src = run_quiet({"findmnt", "-no", "SOURCE", "/"});
    const std::string fstype = run_quiet({"findmnt", "-no", "FSTYPE", "/"});
    root.set("device", JsonValue::string(src));
    root.set("label", JsonValue::string("根分区"));
    root.set("disk", JsonValue::string(""));
    root.set("fstype", JsonValue::string(fstype));
    probe.set("root", root);

    if (src.rfind("/dev/", 0) != 0) {
        probe.set("supported", JsonValue::boolean(false));
        probe.set("reason", JsonValue::string("unsupported_root"));
        probe.set("message", JsonValue::string(
            "根文件系统不是块设备分区（" + (src.empty() ? "未知" : src) + "），不支持在线扩容"));
        return probe;
    }

    const std::string part = src.substr(src.find_last_of('/') + 1);
    std::string disk;
    {
        const std::string out = run_quiet({"lsblk", "-no", "PKNAME", src});
        const size_t nl = out.find('\n');
        disk = nl == std::string::npos ? out : out.substr(0, nl);
        while (!disk.empty() && std::isspace(static_cast<unsigned char>(disk.back())) != 0) {
            disk.pop_back();
        }
    }
    if (!disk.empty()) {
        root.set("disk", JsonValue::string("/dev/" + disk));
        probe.set("root", root);
    }

    const int64_t p_start = sysfs_int("/sys/class/block/" + part + "/start");
    const int64_t p_size = sysfs_int("/sys/class/block/" + part + "/size");
    const int64_t d_size = disk.empty() ? -1 : sysfs_int("/sys/class/block/" + disk + "/size");
    int64_t tail = -1;
    if (p_start >= 0 && p_size >= 0 && d_size >= 0) {
        tail = (d_size - (p_start + p_size)) * 512;
        root.set("free_after_partition", JsonValue::number(static_cast<double>(tail)));
        probe.set("root", root);
    }

    JsonValue missing = JsonValue::array();
    if (run_quiet({"which", "growpart"}).empty()) missing.push_back(JsonValue::string("growpart"));
    if (run_quiet({"which", "resize2fs"}).empty()) missing.push_back(JsonValue::string("resize2fs"));
    probe.set("missing_tools", missing);

    if (!fstype.empty() && fstype != "ext2" && fstype != "ext3" && fstype != "ext4") {
        probe.set("reason", JsonValue::string("unsupported_filesystem"));
        probe.set("message", JsonValue::string("根分区文件系统是 " + fstype + "，本仓只支持 ext4 在线扩容"));
        return probe;
    }
    const bool has_missing = run_quiet({"which", "growpart"}).empty() ||
                             run_quiet({"which", "resize2fs"}).empty();
    if (has_missing) {
        std::string names;
        if (run_quiet({"which", "growpart"}).empty()) names += "growpart";
        if (run_quiet({"which", "resize2fs"}).empty()) {
            if (!names.empty()) names += "、";
            names += "resize2fs";
        }
        probe.set("reason", JsonValue::string("missing_tools"));
        probe.set("message", JsonValue::string("缺少扩容工具：" + names));
        return probe;
    }
    if (tail < 0) {
        probe.set("ok", JsonValue::boolean(false));
        probe.set("reason", JsonValue::string("probe_failed"));
        probe.set("message", JsonValue::string(
            "读不到 " + (root.find("disk") ? root.find("disk")->as_string("") : part) +
            " 的分区表，扩容可行性未知"));
        return probe;
    }
    if (tail < kRootfsTailMinBytes) {
        probe.set("reason", JsonValue::string("no_tail_space"));
        probe.set("message", JsonValue::string(
            "磁盘尾部只剩 " + human_bytes(tail) + "，不足 " +
            human_bytes(kRootfsTailMinBytes) + "，无法扩容"));
        return probe;
    }

    probe.set("expandable", JsonValue::boolean(true));
    probe.set("reason", JsonValue::string("ok"));
    probe.set("message", JsonValue::string("磁盘尾部有 " + human_bytes(tail) + " 未分配空间，可扩容"));
    return probe;
}

// 5 秒缓存探测（findmnt/lsblk 各一次，别被轮询打成热点）。
inline JsonValue rootfs_expand_probe(bool force = false) {
    static std::mutex mu;
    static double cache_ts = 0.0;
    static JsonValue cache_data = JsonValue::null();
    std::lock_guard<std::mutex> lk(mu);
    const double now = now_seconds();
    if (!force && !cache_data.is_null() && now - cache_ts < 5.0) {
        return cache_data;
    }
    JsonValue probe = rootfs_expand_probe_uncached();
    cache_ts = now;
    cache_data = probe;
    return probe;
}

// 探测 + df 容量拼成 rootfs 契约对象（对齐 rootfs._rootfs_expand_payload）。
inline JsonValue rootfs_expand_payload(const std::string& action, bool force = false) {
    const JsonValue s = storage_payload();
    JsonValue payload = rootfs_expand_probe(force);
    const JsonValue* expandable = payload.find("expandable");
    payload.set("action", JsonValue::string(action));
    payload.set("can_expand", expandable != nullptr ? *expandable : JsonValue::boolean(false));
    const JsonValue* percent = s.find("percent");
    const JsonValue* total = s.find("total");
    const JsonValue* used = s.find("used");
    const JsonValue* free = s.find("free");
    payload.set("percent", percent != nullptr ? *percent : JsonValue::number(0.0));
    payload.set("total", total != nullptr ? *total : JsonValue::number(0.0));
    payload.set("used", used != nullptr ? *used : JsonValue::number(0.0));
    payload.set("free", free != nullptr ? *free : JsonValue::number(0.0));
    return payload;
}

}  // namespace ttbox::core::web
