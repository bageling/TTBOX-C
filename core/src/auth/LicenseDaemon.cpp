#include "auth/LicenseDaemon.hpp"
#include "auth/LicenseConstants.hpp"  // B-CONST-4：心跳 60/180 单点真源

#include <chrono>
#include <thread>

#include "auth/LicenseCard.hpp"  // M2.02：activate() 的结构校验
#include "common/Json.hpp"       // M2.07：activate_cloud 的 cloud 形 doc 构造/解析
#include "common/Logger.hpp"     // F2/F4：激活被拒原因日志

namespace ttbox::core::auth {

namespace {
// 取当前墙钟 Unix 毫秒。
int64_t now_unix_ms() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
}  // namespace

// 构造：绑定客户端引用、创建持久化 store、采集本机指纹。
LicenseDaemon::LicenseDaemon(ILicenseClient& client)
    : client_(client), store_(std::make_unique<LicenseStore>()) {
    fp_ = DeviceFingerprint::detect();
}

// 析构：停止后台线程（幂等）。
LicenseDaemon::~LicenseDaemon() {
    stop();
}

// 返回最近一次磁盘恢复结果（加锁拷贝）。
StoreLoadResult LicenseDaemon::store_load_result() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_load_;
}

// 设置内存中的完整卡号（加锁）。
void LicenseDaemon::set_card(const std::string& card_plain) {
    std::lock_guard<std::mutex> lk(mu_);
    card_plain_ = card_plain;
}

// 返回当前授权状态快照（加锁拷贝）。
LicenseStatus LicenseDaemon::status_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

// 是否允许运行 AI 主功能：仅 kValid / kFallback 为真。
bool LicenseDaemon::allow_run() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (status_.state == LicenseState::kValid ||
        status_.state == LicenseState::kFallback) {
        return true;
    }
    return false;
}

// 是否 Pro（武器扩展模型 / 高级 FOV 等）。
bool LicenseDaemon::is_pro() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_.is_pro;
}

// 启动后台心跳线程：加载磁盘授权文档/恢复云态，成功返回 true；已在运行则返回 false。
bool LicenseDaemon::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;
    }
    {
        // T1.09（§2.3）：启动前恢复磁盘授权文档，缓存供 LicenseGate 投影取 grace_until 等。
        // （深播种 verified_at/expire 见 T2.x；本轮仅缓存，不改 AUTH=ON 既有验卡语义。）
        std::lock_guard<std::mutex> lk(mu_);
        last_load_ = store_ ? store_->load() : StoreLoadResult{};
        // ★ M2.07：若磁盘文档是云端形（source:"cloud"），恢复到内存态（kValid/kExpired），
        //   并把 cloud_license_ 置 true，使 thread_loop 不误归一为 kInvalidCard。
        //   ★ D-D 订正（2026-09-17）：**单靠本调用并不能保证"重启后云授权仍在"**——
        //   Application::run() 随后的 verify_now_blocking() 会把 resolve_license_card()
        //   读回的 cloud 文档当离线卡验签并抹掉此处刚恢复的云态。真正成立需**同时**满足：
        //   （a）verify_now_blocking() 有 cloud_license_ 短路；（b）resolve_license_card()
        //   不把 cloud 文档当卡交下来；（c）thread_loop 非空分支不因 card_plain_ 非空去离线验签。
        //   三处缺一，重启后云态即丢（现场实证见验收 B24）。
        restore_cloud_doc_locked();
    }
    thread_ = std::thread(&LicenseDaemon::thread_loop, this);
    return true;
}

// 停止后台线程（等待 join，幂等；未运行时直接返回）。
void LicenseDaemon::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

// 同步触发一次验卡（--verify-only / 激活用）；云态下只回报当前态不走离线验签。
bool LicenseDaemon::verify_now_blocking(std::string* err_message) {
    std::string card;
    std::string bind;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // ★ M2.07（D-D）：云授权态下**绝不**走离线验签——`card_plain_` 里可能是 cloud 形
        //   文档（顶层 source:"cloud"，见 activate_cloud 落盘 / restore_cloud_doc_locked），
        //   它**不是** Ed25519 签名信封；离线验签必然失败，apply_check_result() 会把刚恢复的
        //   kValid 覆盖掉（现场：core 重启后 activated 由 true 变 false，B24 覆盖此点）。
        //   云态生命周期由 activate_cloud()/restore_cloud_doc_locked()/deactivate_cloud()
        //   全权维护 ⇒ 此处只回报当前态，不触发 verify_once / apply_check_result。
        if (cloud_license_) {
            status_.bind_device = fp_.bind_string();
            return status_.state == LicenseState::kValid ||
                   status_.state == LicenseState::kFallback;
        }
        card = card_plain_;
        bind = fp_.bind_string();
        status_.state = LicenseState::kChecking;
    }
    LicenseStatus out;
    bool ok = client_.verify_once(card, bind, out, err_message);
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 与 do_check_cycle_locked 共用同一纯迁移逻辑（T1.08）：同样避免失败时
        // 用 `status_ = out;` 抹掉上次成功验证留下的时间戳。
        // 本路径不参与心跳退避，故退避值用局部变量承载（不写回 cur_backoff_ms_，
        // 保持原有"一次性验卡不改动退避状态"的语义）。
        int64_t backoff_unused = cur_backoff_ms_;
        apply_check_result(status_, out, ok, now_unix_ms(), heartbeat_ms_,
                           backoff_base_ms_, backoff_cap_ms_, backoff_unused);
        status_.bind_device = bind;
    }
    return ok;
}

// 离线卡激活原子序：结构校验 → 防降级 → 装卡立即验签 → 仅 kValid 落盘；被拒时按需回滚云态。
bool LicenseDaemon::activate(const std::string& card_envelope, std::string* error) {
    // ★ F11（2026-09-17）：入口快照完整授权态。面板**免密**（M2.07 需求①）⇒ 局域网内
    //   任何人可 POST /api/license/activate。本函数三条拒绝路径（parse 失败 / 防降级 /
    //   验签失败）都会改写内存授权态 —— 未修前，一个空/非法 body 或坏签名卡即可把**云激活**
    //   打掉并关掉 AI（现场：before{activated,active,source=cloud} → POST card-badsig →
    //   after{unactivated,kInvalidCard,'offline card: signature mismatch'}，直到重启 core
    //   才由 restore_cloud_doc_locked() 恢复）。
    //   行为级契约（修法）：**调用前为云激活时**，任一被拒 ACTIVATE 都必须把
    //   cloud_license_ / status_ / card_plain_ **逐字段回滚**到调用前快照 ⇒ 调用后
    //   /api/license 与管线放行结果与调用前一致（activated/state/source/features.capture 不变）。
    //   反向护栏：**调用前非云激活时保持既有行为**（把拒绝原因写进状态，供现场排障 ——
    //   F2 意图不得丢）；合法离线卡（kValid）仍照常"接管"（cloud_license_=false + 落盘）。
    bool prev_cloud = false;
    LicenseStatus prev_status;
    std::string prev_card;
    {
        std::lock_guard<std::mutex> lk(mu_);
        prev_cloud = cloud_license_;
        prev_status = status_;
        prev_card = card_plain_;
    }
    auto rollback_cloud_guard = [&]() {
        // 仅当调用前是云激活态才回滚；非云态一律保持既有"写拒绝原因"行为。
        if (!prev_cloud) return;
        std::lock_guard<std::mutex> lk(mu_);
        cloud_license_ = true;
        status_ = prev_status;
        card_plain_ = prev_card;
    };

    // ① 结构校验先行（fail-closed；不碰内存态）
    //    （parse 只做格式/闭集/字符集，不验签——验签在 ② 的 verify_once 里。）
    LicenseCard parsed;
    std::string perr;
    if (!parse_license_card(card_envelope, &parsed, &perr)) {
        // ★ F2：解析拒绝也是"卡已存在但被拒"，必须把原因写进状态，
        //   否则 Web `/api/license` 的 message 为空（现场症状：只显示"卡在但被拒"，无原因）。
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_.state = LicenseState::kInvalidCard;
            status_.last_error = perr.empty() ? "offline card: parse rejected" : perr;
        }
        // ★ F11：调用前为云激活 ⇒ 上述写入整体回滚（一个空/非法 body 不得关掉 AI）。
        rollback_cloud_guard();
        if (error) *error = perr;
        return false;
    }

    // ①.5 ★ F4：防降级（LIC-17）——本次卡的 issued_at 不得早于已落盘基线。
    //   必须在装卡/验签之前判定：否则 kValid 已写入内存态却拒绝落盘 ⇒ 制造"半激活"。
    //   M2 卡的 issued_at 位于**嵌套** license.issued_at（见 LicenseCard.hpp canonical 定义）；
    //   LicenseStore::load() 已按此提取基线。store 缺失 ⇒ 基线 0 ⇒ 首次激活必然通过。
    if (store_) {
        const StoreLoadResult cur = store_->load();
        if (LicenseStore::is_downgrade(parsed.issued_at, cur.last_seen_issued_at)) {
            const std::string why =
                "license downgrade rejected (issued_at " + std::to_string(parsed.issued_at) +
                " < last_seen_issued_at " + std::to_string(cur.last_seen_issued_at) + ")";
            {
                std::lock_guard<std::mutex> lk(mu_);
                status_.state = LicenseState::kInvalidCard;
                status_.last_error = why;
            }
            // ★ F11：调用前为云激活 ⇒ 回滚（防降级拒绝亦不得踩云态）。
            rollback_cloud_guard();
            TTBOX_LOG_WARN("[LicenseDaemon] " + why);
            if (error) *error = "LICENSE_DOWNGRADE_REJECTED";
            return false;
        }
    }

    // ② 装卡 + 立即同步验签
    {
        std::lock_guard<std::mutex> lk(mu_);
        card_plain_ = card_envelope;
        // ★ M2.07：离线卡路径接管 ⇒ 关闭云态保护（两者互斥；见 impl-spec §0 D2/D7）。
        cloud_license_ = false;
        status_.state = LicenseState::kChecking;
    }
    std::string verr;
    (void)verify_now_blocking(&verr);
    const LicenseStatus st = status_snapshot();
    if (st.state != LicenseState::kValid) {
        // 权威拒绝（kInvalidCard / kBoundElsewhere / kExpired）：撤回内存卡，
        // 不落盘（防"半激活"：磁盘上留一张已知坏卡）。
        {
            std::lock_guard<std::mutex> lk(mu_);
            card_plain_.clear();
            // ★ F2：确保拒绝原因停留在状态里（verify 路径已写 last_error；此处兜底）。
            if (status_.last_error.empty()) {
                status_.last_error = verr.empty()
                                         ? ("activation rejected (state=" +
                                            std::to_string(static_cast<int>(st.state)) + ")")
                                         : verr;
            }
        }
        // ★ F11：调用前为云激活 ⇒ 整体回滚（含 cloud_license_/status_/card_plain_）；
        //   否则云激活被打掉、status 被写成 kInvalidCard ⇒ AI 被关。
        rollback_cloud_guard();
        if (error) {
            *error = st.last_error.empty()
                         ? (verr.empty() ? "激活被拒绝（state=" +
                               std::to_string(static_cast<int>(st.state)) + "）" : verr)
                         : st.last_error;
        }
        return false;
    }
    // ③ kValid 才落盘（license.json = 信封原文；重启后由 resolve 链恢复）
    if (store_ && !store_->save_doc(card_envelope, error)) {
        // 落盘失败：授权态在内存中仍有效（本会话可用），但持久化缺失要如实上报。
        return false;
    }
    // ★ F4：落盘成功后同步防回滚基线（issued_at = 本次卡），使下次更旧的卡被拒。
    //   其余基线字段（server_time_floor/grant/grace）沿用磁盘既有值，不在此清零。
    if (store_) {
        const StoreLoadResult cur = store_->load();
        std::string serr;
        if (!store_->save_state(parsed.issued_at, cur.server_time_floor_unix,
                                cur.grant_expires_at, cur.grace_until, &serr)) {
            // 基线落盘失败：不影响本会话授权（内存态已 kValid），但要如实记录。
            TTBOX_LOG_WARN("[LicenseDaemon] 防回滚基线落盘失败: " + serr);
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (parsed.issued_at > last_load_.last_seen_issued_at) {
            last_load_.last_seen_issued_at = parsed.issued_at;
        }
        last_load_.doc_ok = true;
        last_load_.doc_json = card_envelope;
    }
    if (error) error->clear();
    return true;
}

// ---------------------------------------------------------------------------
// M2.07：云端卡密激活（ACTIVATE_CLOUD 的 daemon 侧；D2 裁决）
// ---------------------------------------------------------------------------
// 云端卡密激活原子序：expire 校验 → features 收窄 → 落盘 cloud 形 doc → 内存置 kValid/cloud_license_。
bool LicenseDaemon::activate_cloud(int64_t expire_unix_ms,
                                   const std::vector<std::string>& features,
                                   const std::string& plan,
                                   const std::string& card_mask,
                                   std::string* error) {
    // ① fail-closed：expire 必须晚于当前时间
    if (expire_unix_ms <= 0 || expire_unix_ms <= now_unix_ms()) {
        if (error) *error = "expire_unix_ms 必须晚于当前时间";
        return false;
    }
    // ② features 收窄（闭集唯一权威 = LicenseStateMachine::known_features()）
    const std::vector<std::string> feats = normalize_features(features);
    const std::string pl = plan.empty() ? std::string("none") : plan;
    const int64_t now = now_unix_ms();

    // ③ 构造 cloud 形 license.json 并原子落盘（LicenseStore = 唯一落盘入口红线）
    ttbox::core::JsonValue doc = ttbox::core::JsonValue::object();
    doc.set("state", ttbox::core::JsonValue::string("valid"));
    doc.set("issued_at_ms", ttbox::core::JsonValue::number(static_cast<double>(now)));
    doc.set("expire_unix_ms",
            ttbox::core::JsonValue::number(static_cast<double>(expire_unix_ms)));
    ttbox::core::JsonValue feats_json = ttbox::core::JsonValue::array();
    for (const std::string& f : feats) {
        feats_json.push_back(ttbox::core::JsonValue::string(f));
    }
    doc.set("features", feats_json);
    doc.set("plan", ttbox::core::JsonValue::string(pl));
    doc.set("ui_brand", ttbox::core::JsonValue::string(default_ui_brand()));
    doc.set("card_mask", ttbox::core::JsonValue::string(card_mask));
    doc.set("source", ttbox::core::JsonValue::string("cloud"));
    if (store_) {
        std::string serr;
        if (!store_->save_doc(doc.dump(), &serr)) {
            // 落盘失败：不进入 kValid（防"内存有效、重启即丢"的半激活）
            if (error) *error = "license 落盘失败: " + serr;
            TTBOX_LOG_WARN("[LicenseDaemon] activate_cloud 落盘失败: " + serr);
            return false;
        }
        std::lock_guard<std::mutex> lk(mu_);
        last_load_.doc_ok = true;
        last_load_.doc_json = doc.dump();
        // ★ M2.07（D-D）：与已落盘的 cloud 形文档保持一致的缓存字段（单一真源）。
        last_load_.doc_is_cloud = true;
    }

    // ④ 内存态置 kValid（云端心跳归 web 进程 ⇒ daemon 不参与云心跳，
    //    next_check_ms 拉满仅表示"本线程无需验卡"；到期由 60s 扫描执法）
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.state = LicenseState::kValid;
        status_.verified_at_ms = now;
        status_.expire_unix_ms = expire_unix_ms;
        status_.features = feats;
        status_.plan = pl;
        status_.ui_brand = sanitize_ui_brand(default_ui_brand());
        status_.heartbeat_interval = kHeartbeatIntervalSecDefault;
        status_.heartbeat_timeout = kHeartbeatTimeoutSecDefault;
        status_.next_check_ms = now + 365LL * 24 * 3600 * 1000;  // 云态无 daemon 验卡
        status_.card = card_mask;                                 // 展示短码
        status_.license_id.clear();
        status_.last_error.clear();
        cloud_license_ = true;   // ★ M2.07：标记为云授权 ⇒ 空卡分支不归一 kInvalidCard
    }
    TTBOX_LOG_INFO("[LicenseDaemon] activate_cloud 成功: expire_unix_ms=" +
                   std::to_string(expire_unix_ms) + " features=" +
                   std::to_string(feats.size()) + " plan=" + pl);
    if (error) error->clear();
    return true;
}

// 云端否定（到期/禁用）快路径：内存置 kExpired + last_error，不落盘。
bool LicenseDaemon::deactivate_cloud(const std::string& reason) {
    std::lock_guard<std::mutex> lk(mu_);
    status_.state = LicenseState::kExpired;
    status_.last_error = reason.empty() ? std::string("云端授权已失效（到期或已禁用）")
                                        : reason;
    // 不落盘（D4）：本地 license.json 保留供对账；重启后 restore 会按 expire 判定。
    TTBOX_LOG_WARN("[LicenseDaemon] deactivate_cloud: " + status_.last_error);
    return true;
}

// 启动时从磁盘 cloud 形 license.json 恢复内存态（expire≤0 fail-closed 拒绝；非 cloud doc 不动）。
void LicenseDaemon::restore_cloud_doc_locked() {
    // 仅处理 cloud 形 doc；离线卡 doc 走既有 resolve 链，不在此恢复。
    // ★ M2.07（D-D）：cloud 形判定**唯一真源** = LicenseStore::load() 登记的
    //   last_load_.doc_is_cloud（本层不再另做 "source" 字符串匹配，避免双源漂移）。
    if (!last_load_.doc_ok || last_load_.doc_json.empty()) return;
    if (!last_load_.doc_is_cloud) return;
    const ttbox::core::JsonParseResult pr =
        ttbox::core::json_parse(last_load_.doc_json);
    if (!pr.ok || !pr.value.is_object()) return;

    int64_t expire = 0;
    if (const ttbox::core::JsonValue* v = pr.value.find("expire_unix_ms")) {
        expire = v->as_int(0);
    }
    std::vector<std::string> feats;
    if (const ttbox::core::JsonValue* v = pr.value.find("features")) {
        if (v->is_array()) {
            for (const ttbox::core::JsonValue& e : v->as_array()) {
                if (e.is_string()) feats.push_back(e.as_string());
            }
        }
    }
    std::string pl;
    if (const ttbox::core::JsonValue* v = pr.value.find("plan")) {
        if (v->is_string()) pl = v->as_string();
    }
    std::string mask;
    if (const ttbox::core::JsonValue* v = pr.value.find("card_mask")) {
        if (v->is_string()) mask = v->as_string();
    }

    const int64_t now = now_unix_ms();
    // ★ 2026-09-26 fail-closed：cloud 文档的 expire 必须 > 0（activate_cloud 明确
    //   拒绝 ≤0，云端语义没有"永久云卡"）。旧 restore 把 0（字段缺失/解析失败落 0
    //   都会到这）当"永久"放行 ⇒ 磁盘态异常时得到一张永远不过期、一年不复核的云授权。
    if (expire <= 0) {
        status_.state = LicenseState::kInvalidCard;
        status_.last_error = "云端授权文档异常：expire_unix_ms 缺失或非法，拒绝恢复";
        status_.expire_unix_ms = expire;
        status_.features = normalize_features(feats);
        status_.plan = pl.empty() ? std::string("none") : pl;
        status_.ui_brand = sanitize_ui_brand(default_ui_brand());
        status_.card = mask;
        cloud_license_ = true;
        TTBOX_LOG_WARN("[LicenseDaemon] 云端授权恢复被拒: expire_unix_ms=" +
                       std::to_string(expire));
        return;
    }
    if (now > expire) {
        status_.state = LicenseState::kExpired;
        status_.last_error = "卡密已到期";
    } else {
        status_.state = LicenseState::kValid;
        status_.verified_at_ms = now;
        status_.last_error.clear();
    }
    status_.expire_unix_ms = expire;
    status_.features = normalize_features(feats);
    status_.plan = pl.empty() ? std::string("none") : pl;
    status_.ui_brand = sanitize_ui_brand(default_ui_brand());
    status_.heartbeat_interval = kHeartbeatIntervalSecDefault;
    status_.heartbeat_timeout = kHeartbeatTimeoutSecDefault;
    status_.next_check_ms = now + 365LL * 24 * 3600 * 1000;
    status_.card = mask;
    cloud_license_ = true;   // ★ M2.07：磁盘 cloud 形文档 ⇒ 云授权来源
    TTBOX_LOG_INFO("[LicenseDaemon] 云端授权恢复: state=" +
                   std::to_string(static_cast<int>(status_.state)) +
                   " expire_unix_ms=" + std::to_string(expire));
}

// 注入心跳间隔（测试/mock 用）。
void LicenseDaemon::set_heartbeat_interval_ms(int64_t ms) {
    heartbeat_ms_ = ms;
}

// 注入指数退避基数（测试/mock 用）。
void LicenseDaemon::set_backoff_base_ms(int64_t ms) {
    backoff_base_ms_ = ms;
}

// 守护线程主体：按状态决定 sleep/验卡；空卡/云态/在线各有分支，分段 sleep 以响应 shutdown。
void LicenseDaemon::thread_loop() {
    while (running_.load()) {
        std::string card;
        std::string bind;
        int64_t sleep_ms = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            card = card_plain_;
            bind = fp_.bind_string();
            if (card.empty()) {
                const int64_t now = now_unix_ms();
                if (cloud_license_) {
                    // ★ M2.07（D4）：云授权（source=cloud）无本地卡号 —— 不得把云激活态
                    //   冲成 kInvalidCard/"card not set"。到期扫描（慢路径兜底，唯一强制点）：
                    //   state∈{kValid,kFallback} 且 expire>0 且 now>expire ⇒ kExpired
                    //   （Gate publish 由 Application 心跳循环周期性完成，本层只改内存态）。
                    if ((status_.state == LicenseState::kValid ||
                         status_.state == LicenseState::kFallback) &&
                        status_.expire_unix_ms > 0 && now > status_.expire_unix_ms) {
                        status_.state = LicenseState::kExpired;
                        status_.last_error = "卡密已到期";
                        TTBOX_LOG_WARN("[LicenseDaemon] 到期扫描：云端授权已到期 ⇒ kExpired");
                    }
                    // 仍有效/已到期：维持现状，短粒度轮询（60s 粒度兜底）。
                    sleep_ms = std::min(backoff_base_ms_, backoff_cap_ms_);
                } else {
                    // 无本地卡且非云授权态 ⇒ 归一 kInvalidCard（**既有语义，务必保留**：
                    // 离线卡被拒后 card_plain_ 已 clear，本分支是"无有效授权"的唯一归一；
                    // 也是 F2②「真因不得被泛化」的前提）。
                    // ★ F2：仅在无既有原因时写 "card not set"（保留真实拒绝原因）。
                    status_.state = LicenseState::kInvalidCard;
                    if (status_.last_error.empty()) {
                        status_.last_error = "card not set";
                    }
                    sleep_ms = std::min(backoff_base_ms_, backoff_cap_ms_);
                }
            } else if (cloud_license_) {
                // ★ M2.07（D-D）：云授权态**即使 card_plain_ 非空**（例如曾把 cloud 文档误当卡
                //   填入）也禁止走离线验签周期——否则 do_check_cycle_locked() 会拿该文档离线
                //   验签（必失败）并抹掉云态。云态无本地卡号，到期仅由 60s 扫描兜底（与上方
                //   空卡云态分支同语义）。本守卫与 verify_now_blocking()/resolve_license_card()
                //   的云态处置一起，构成 D-D 的三处闭合。
                const int64_t now = now_unix_ms();
                if ((status_.state == LicenseState::kValid ||
                     status_.state == LicenseState::kFallback) &&
                    status_.expire_unix_ms > 0 && now > status_.expire_unix_ms) {
                    status_.state = LicenseState::kExpired;
                    status_.last_error = "卡密已到期";
                    TTBOX_LOG_WARN("[LicenseDaemon] 到期扫描：云端授权已到期 ⇒ kExpired");
                }
                sleep_ms = std::min(backoff_base_ms_, backoff_cap_ms_);
            } else {
                const int64_t now = now_unix_ms();
                if (status_.state == LicenseState::kValid &&
                    now < status_.next_check_ms &&
                    (status_.expire_unix_ms == 0 ||
                     now < status_.expire_unix_ms)) {
                    // 有效期内且未到下一次心跳：睡到 next_check
                    sleep_ms = status_.next_check_ms - now;
                    // 若 expire 更早提前到期
                    if (status_.expire_unix_ms > 0) {
                        int64_t till_exp = status_.expire_unix_ms - now;
                        if (till_exp > 0 && till_exp < sleep_ms) sleep_ms = till_exp;
                    }
                } else {
                    // 需要本轮检查
                    do_check_cycle_locked(card, bind);
                    sleep_ms = cur_backoff_ms_ > 0
                                   ? cur_backoff_ms_
                                   : std::max<int64_t>(1000, heartbeat_ms_);
                }
            }
        }
        // 分段 sleep：响应 shutdown
        constexpr int64_t kSliceMs = 250;
        int64_t remain = sleep_ms;
        while (remain > 0 && running_.load()) {
            int64_t sl = remain < kSliceMs ? remain : kSliceMs;
            std::this_thread::sleep_for(std::chrono::milliseconds(sl));
            remain -= sl;
        }
    }
}

// 单次检查周期（调用方须持有 mu_）：验卡 + 状态迁移 + 脱敏卡号回填。
void LicenseDaemon::do_check_cycle_locked(const std::string& card_plain,
                                            const std::string& bind_device) {
    LicenseStatus out;
    std::string err;
    bool req_ok = client_.verify_once(card_plain, bind_device, out, &err);
    // 合并式状态迁移（T1.08）：失败时保留上次成功验证的时间戳，避免
    // `status_ = out;` 整体覆盖把 verified_at_ms / expire_unix_ms 清零，
    // 从而使 kFallback（网络错误 fail-open）永远不可达。
    apply_check_result(status_, out, req_ok, now_unix_ms(), heartbeat_ms_,
                       backoff_base_ms_, backoff_cap_ms_, cur_backoff_ms_);
    status_.bind_device = bind_device;
    if (!card_plain.empty()) {
        // 脱敏：前 8 位 + ******
        std::string s = card_plain;
        if (s.size() > 8) s = s.substr(0, 8) + "******";
        status_.card = s;
    }
}

}  // namespace ttbox::core::auth
