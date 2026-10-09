// OtaUpdater.hpp — OTA 更新主流程与任务队列消费（自 ttbox_ota_updater.py 移植）。
//
// ★ 这是唯一升级通道的**全部**逻辑。对拍基准 A~F 段覆盖其中可纯函数化的部分；
//   其余（下载/解包/装/健康检查）依赖真实环境，由板端端到端升级作最终判据。
//
// 流程（与现役 Python 逐步对应，括号内为 Python 的 state 名）：
//   ① scheme 白名单（仅 https）                    scheme_rejected
//   ② 下载包 + 旁车签名（120s × 3 次重试）
//   ③ 包 sha256 == 签名记录里的 sha256              sha256_mismatch
//   ④ Ed25519 验签（canonical 取 SIGNED_FIELDS）    signature_invalid
//   ⑤ 版本消毒（_check_safe_id）                   unsafe_field
//   ⑥ 解包（成员名先过安全校验）                   unsafe_member / manifest_missing
//   ⑥b 增量合并（base_version 必须 == 当前版本）    delta_base_mismatch
//   ⑦ 全量 manifest 复验                            manifest_mismatch
//   ⑦b 降级拒绝（包必须**新于**当前）                downgrade_rejected
//   ⑧ 调 release_install.sh 原子发布                install_failed
//   ⑨ 健康检查（失败**自动回滚**）                   health_check_failed
//   ⑩ 写 SUCCESS 状态
//   finally：临时目录与 staging **成败皆清**
#pragma once

#include <string>
#include <vector>

namespace ttbox::core::ota {

struct OtaEnv {
    std::string prefix = "/opt/ttbox";  // TTBOX_PREFIX
    std::string default_key_id = "ttbox-ota-2026b";
    int health_timeout_sec = 30;
};

// 当前运行版本：读 current 软链指向的 releases/<ver> 目录名；
// 读不到回退 <prefix>/state/version；都拿不到返回空串（= 未知，降级判定放行）。
std::string current_version(const std::string& prefix);

// 任务目录消费（web 写 *.json，本进程以 root 跑）。
// ★ 每个任务处理完**必须移走**（成功 → processed/，失败 → failed/），
//   否则 path 单元会因文件仍在而反复触发。返回：全部成功 0，任一失败 1。
int process_jobs(const OtaEnv& env, const std::string& jobs_dir);

// 跑一次升级。返回 0 成功，非 0 失败（并已写 ota_status.json）。
int run_update(const OtaEnv& env, const std::string& url, const std::string& key_id,
                const std::string& version);

// ---- 供单测/复用的细粒度构件 ----

// 下载（curl；仅 https 由调用方先拦）。120s 读超时 × 3 次重试，每次重写目标文件。
// 理由（2026-09-19 板端实测）：七牛隧道到板端仅 ~12KB/s，且偶发 >60s 的读停顿，
// 旧的 60s 超时一卡就整包失败。
bool fetch_file(const std::string& url, const std::string& dest, std::string* err);

// 完整校验：sha256 + Ed25519。signs_text 是旁车 .sign.json 的原文。
bool verify_package(const std::string& tgz_path, const std::string& signs_text,
                    const std::string& key_id, const OtaEnv& env, std::string* out_sha256,
                    std::string* fail_state, std::string* fail_detail);

// 写 ota_status.json（原子：临时文件 + rename）
void write_status(const OtaEnv& env, const std::string& json_body);

// 健康检查：三服务 active **且** core IPC 能应答（对齐 Python default_health）。
bool health_check(const OtaEnv& env, int timeout_sec);

}  // namespace ttbox::core::ota
