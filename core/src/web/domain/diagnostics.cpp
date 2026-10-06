// diagnostics.cpp — USB 代理诊断域（1 条路由，纯只读，不改设备状态）。
//
// 自 plugins/web/api/diagnostics.py 逐行为移植。ZIP 用 STORE（无压缩）手写，
// 规避引入 zlib 依赖（Python 用 ZIP_DEFLATED；诊断包体积极小，语义等价）。
#include "web/domain/domain_routes.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/sysinfo_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// CRC32（IEEE 802.3 多项式 0xEDB88320，表驱动）。
uint32_t crc32_of(const std::string& s) {
    static uint32_t table[256] = {0};
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : s) crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void put_u16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put_u32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// 手写 ZIP（STORE 无压缩）：local header + central directory + EOCD。
std::string build_zip_store(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out;
    std::string central;
    uint32_t offset = 0;
    for (const auto& [name, data] : entries) {
        const uint32_t crc = crc32_of(data);
        const uint32_t size = static_cast<uint32_t>(data.size());
        // Local File Header
        put_u32(out, 0x04034b50u);
        put_u16(out, 20);
        put_u16(out, 0);
        put_u16(out, 0);  // store
        put_u16(out, 0);
        put_u16(out, 0);
        put_u32(out, crc);
        put_u32(out, size);
        put_u32(out, size);
        put_u16(out, static_cast<uint16_t>(name.size()));
        put_u16(out, 0);
        out += name;
        out += data;
        // Central Directory entry
        put_u32(central, 0x02014b50u);
        put_u16(central, 20);
        put_u16(central, 20);
        put_u16(central, 0);
        put_u16(central, 0);
        put_u16(central, 0);
        put_u16(central, 0);
        put_u32(central, crc);
        put_u32(central, size);
        put_u32(central, size);
        put_u16(central, static_cast<uint16_t>(name.size()));
        put_u16(central, 0);
        put_u16(central, 0);
        put_u16(central, 0);
        put_u16(central, 0);
        put_u32(central, 0);
        put_u32(central, offset);
        central += name;
        offset += 30 + static_cast<uint32_t>(name.size()) + size;
    }
    const uint32_t cd_size = static_cast<uint32_t>(central.size());
    const uint32_t cd_offset = static_cast<uint32_t>(out.size());
    out += central;
    put_u32(out, 0x06054b50u);
    put_u16(out, 0);
    put_u16(out, 0);
    put_u16(out, static_cast<uint16_t>(entries.size()));
    put_u16(out, static_cast<uint16_t>(entries.size()));
    put_u32(out, cd_size);
    put_u32(out, cd_offset);
    put_u16(out, 0);
    return out;
}

}  // namespace

void register_diagnostics_routes(httplib::Server& svr, IpcClient&) {
    svr.Get("/api/diagnostics/usb-proxy.zip",
            [](const httplib::Request&, httplib::Response& res) {
                std::string service = run_quiet(
                    {"systemctl", "status", "ttbox-usbproxy", "--no-pager"});
                if (service.empty()) service = "ttbox-usbproxy service not active\n";

                std::vector<std::string> devs;
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(
                         "/dev", std::filesystem::directory_options::skip_permission_denied, ec)) {
                    const std::string n = e.path().filename().string();
                    if (n.rfind("hidg", 0) == 0) devs.push_back(n);
                }
                std::string hidg;
                for (const std::string& d : devs) hidg += d + "\n";

                std::vector<std::pair<std::string, std::string>> entries;
                entries.emplace_back("usbproxy_status.txt",
                                     "TTBOX usb-proxy diagnostic\n");
                entries.emplace_back("usbproxy_service.txt", service);
                entries.emplace_back("hidg_devices.txt", hidg);

                const std::string zip = build_zip_store(entries);
                res.status = 200;
                res.set_header("Content-Disposition",
                               "attachment; filename=usb-proxy.zip");
                res.set_content(zip, "application/zip");
            });
}

}  // namespace ttbox::core::web
