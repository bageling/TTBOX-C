// core/src/auth/TtboxCanonical.hpp —— 授权请求 canonical 串构造（纯逻辑单元，T1.07）
//
// 契约（server-api-contract.md §2；已按云端 client_sign.go 的
//   clientSigningString = "%s\n%s\n%s\n%s\n%s" 实测校准，M2.07）：
//   签名原文 = METHOD \n PATH_WITH_QUERY \n TIMESTAMP \n NONCE \n BODY_JSON
// （LF 分隔，**末尾无换行**；GET 空 body ⇒ 以 body 前那个 "\n" 收尾，其后不再追加）。
// 实测：末尾多拼一个 "\n" ⇒ 云端 HMAC 校验失败返回 401；不带换行 ⇒ 200/业务码。
// 抽为纯函数以便在默认构建（AUTH=OFF，无 OpenSSL）下 host 单测
// （复用 T1.08 LicenseStateMachine 的抽取模式）。
#pragma once

#include <string>

namespace ttbox::core::auth {

// 构造 canonical 签名原文。**末尾无换行**（与云端 clientSigningString 逐字节一致）：
//   build_canonical("GET", "/api/client/app-info?app_key=ttbox", "1699999999", "abc", "")
//     == "GET\n/api/client/app-info?app_key=ttbox\n1699999999\nabc\n"
//   build_canonical("POST", "/api/client/card-login", "1699999999", "abc", "{\"a\":1}")
//     == "POST\n/api/client/card-login\n1699999999\nabc\n{\"a\":1}"
inline std::string build_canonical(const std::string& method,
                                   const std::string& path,
                                   const std::string& timestamp,
                                   const std::string& nonce,
                                   const std::string& body) {
    return method + "\n" + path + "\n" + timestamp + "\n" + nonce + "\n" + body;
}

}  // namespace ttbox::core::auth
