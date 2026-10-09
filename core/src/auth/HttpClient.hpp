#pragma once

#include <string>

namespace ttbox::core::auth {

// 轻量 HTTPS POST 客户端（无第三方依赖；仅链接系统 libssl / libcrypto）
// 用法：
//   HttpsPostResult r = https_post_form("example.com", 443, "/path", "k=v");
struct HttpsPostResult {
    int status_code = 0;   // 0 = 网络错误
    std::string body;      // 响应 body
    std::string error;     // 错误描述（status_code == 0 时填充）
};

// 默认 10s 超时；返回响应体；不做证书 pinning（上层有能力后再叠加）
HttpsPostResult https_post_form(const std::string& host,
                                 int port,
                                 const std::string& path,
                                 const std::string& form_body);

// 方便 mock：可注入证书公钥 pin（SHA256，未启用时返回原样）
// 留接口，先不实现（原系统未见 pinning 证据，后续阶段 5 再叠加）
void https_set_pubkey_pins(const char** sha256_hex_pins, int count);

}  // namespace ttbox::core::auth
