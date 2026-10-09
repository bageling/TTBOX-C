// LicenseConstants.hpp — 授权域时间/阈值单点真源（B-CONST-4）
//
// 心跳间隔 / 超时（秒）在客户端（TtboxLicenseClient）、状态机（LicenseStateMachine）、
// 门（LicenseGate）、守护线程（LicenseDaemon）曾各写一份 60/180；漂移即
// 「心跳周期不一致 ⇒ 状态抖动 / 误失效」。单一权威定义在本头，其余一律引用。
#pragma once

namespace ttbox::core::auth {

// 服务端未通过 app-info / 心跳响应下发时的默认心跳间隔（秒）。
inline constexpr int kHeartbeatIntervalSecDefault = 60;

// 服务端未下发时的默认心跳超时（秒）。
inline constexpr int kHeartbeatTimeoutSecDefault = 180;

}  // namespace ttbox::core::auth
