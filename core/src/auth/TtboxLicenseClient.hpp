#pragma once

#include <cstdint>
#include <cstdlib>  // std::getenv（server_url_/app_key_/client_secret_ 默认值）
#include <string>
#include <utility>  // std::pair（fetch_app_info 返回类型）
#include <vector>
#include "auth/LicenseDaemon.hpp"

namespace ttbox::core::auth {

// TTBOX 新一代 License 客户端，对接 License-SaaS（HTTP(S) + HMAC-SHA256 签名）
// - app_key / client_secret 从环境变量或配置文件读取
// - card_key 登录模式（默认）+ JWT client_token 心跳续期
// - 继承 ILicenseClient；与 server-api-contract.md 的 /api/client/* 契约一致
class TtboxLicenseClient : public ILicenseClient {
public:
    TtboxLicenseClient() = default;
    ~TtboxLicenseClient() override = default;

    // 可注入服务端地址（用于测试 / Docker Compose）
    void override_server(const std::string& server_url);
    void override_credentials(const std::string& app_key,
                               const std::string& client_secret);

    // ILicenseClient
    bool verify_once(const std::string& card,
                     const std::string& bind_device,
                     LicenseStatus& out_status,
                     std::string* err_message = nullptr) override;

private:
    // 一次带签名请求的原始应答（status=HTTP 状态码；0=传输层失败）
    struct HttpReply {
        int status = 0;
        std::string body;
        std::string error;
    };

    // ★ 统一传输入口（2026-09-26 第三轮审计修复）：
    //   旧实现 api_get/api_post/do_heartbeat 三处各拷一份裸 TCP 明文 HTTP，
    //   默认端点却是 https ⇒ 对现役服务器永远打不通；且 parse_url_host 把
    //   base_url 的路径段（如 /ttbox）丢弃。现在：
    //     · 保留 server_url_ 的路径段并拼进请求行（对齐 Python cloud_client 的
    //       base_url + path 语义；HMAC canonical 仍签不含 base 的 path，同端同口径）；
    //     · https 走 OpenSSL（Linux/macOS，VERIFY_PEER + 系统 CA）；Windows 暂无
    //       TLS 实现 ⇒ 显式报错，绝不静默明文降级。
    bool signed_exchange(const std::string& method,
                         const std::string& path,
                         const std::string& body_json,
                         const std::vector<std::pair<std::string, std::string>>& extra_headers,
                         HttpReply& out,
                         std::string* err);

    // HMAC-SHA256 签名（T1.07：去掉 static——它读取成员 client_secret_，
    // 原 static 声明使本 TU **从未编译通过**；此缺陷于 7a 落码时暴露并修复）
    std::string sign_request(const std::string& method,
                                    const std::string& path,
                                    const std::string& timestamp,
                                    const std::string& nonce,
                                    const std::string& body);
    // 发送带签名的 HTTP 请求
    std::string api_get(const std::string& path);
    std::string api_post(const std::string& path, const std::string& body_json);
    // 解析 app-info（返回 heartbeat_interval / heartbeat_timeout）
    std::pair<int, int> fetch_app_info();
    // Card-login
    bool do_card_login(const std::string& card_key,
                       const std::string& bind_device,
                       LicenseStatus& out,
                       std::string* err);
    // Heartbeat（续期）
    bool do_heartbeat(const std::string& token,
                      const std::string& bind_device,
                      LicenseStatus& out,
                      std::string* err);

    // ★ 2026-09-26：旧服务器 38.127.133.6（七牛，NAT 10015/10039）已宕机，
    //   现役是阿里云 cctv2.top，HTTPS 主入口 10086。默认值必须跟到新服务器，
    //   否则任何未配置 cloud.license_base_url 的环境都会打到一个死地址。
    std::string server_url_ = std::getenv("TTBOX_LICENSE_SERVER")
                                  ? std::getenv("TTBOX_LICENSE_SERVER")
                                  : "https://cctv2.top:10086/ttbox";
    std::string app_key_ = std::getenv("TTBOX_APP_KEY")
                               ? std::getenv("TTBOX_APP_KEY")
                               : "ttbox";
    std::string client_secret_ = std::getenv("TTBOX_CLIENT_SECRET")
                                     ? std::getenv("TTBOX_CLIENT_SECRET")
                                     : std::string();   // 无默认密钥；空 ⇒ 运行时拒签（§3.3）
};

}  // namespace ttbox::core::auth
