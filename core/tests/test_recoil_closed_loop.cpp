// test_recoil_closed_loop.cpp — 开火期闭环纠偏（压枪 v1）单元测试
//
// 锁的是业主 2026-09-29 定案的原则与实现不变量：
//   ①「有实时偏移就纠偏，没有实时偏移就不猜」—— 开关关 / 未开火 / 观测不合格
//      ⇒ 立即清零，不留保持窗、不留跨开火记忆；
//   ②「前几发不压」是设计内代价 —— start_frames 观察期内零输出；
//   ③ 单向：只下压（积分下限钳 0），永远不会把准星往上推；
//   ④ 限幅：积分上限 + 单帧下压上限两道安全阀；
//   ⑤ 默认关 = 零输出（不开时输出链与加入前逐字节一致）；
//   ⑥ 真闭环仿真：后坐力持续把偏差推大时，注入能把偏差压回收敛区；
//   ⑦ 比例项（kp，2026-09-29 业主令「先做 1」）：短促点射里起压第一帧就有可观输出，
//      不依赖积分爬升 —— 纯积分在同场景下首帧输出只有它的十几分之一。
//   ⑧ 基线口径（2026-09-29 15:3x 实机数据定障）：观测量 = 偏差 − 没开火时学的底子。
//      实机：原始偏差开火时仅 41% 为正（被静态负偏移埋住）⇒ 第一版 27 秒只压 17 count；
//      减底线后 98% 为正、中位 +56px。Case1-9 锁的是 baseline_tau_ms=0 的原始口径（回归保护），
//      Case10/11 锁基线模式本体。
//
// 对应实现：core/src/mouse/RecoilClosedLoop.hpp
// 接线点：  core/src/aim/AimThread.cpp（obs_ok 构造 + control_y 作观测量）
#include <cstdio>

#include "mouse/RecoilClosedLoop.hpp"

using namespace ttbox::core::aim;

namespace {

int failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); failures++; }
    else { std::printf("  PASS: %s\n", msg); }
}

RecoilClConfig make_cfg() {
    RecoilClConfig c;
    c.enabled = true;
    c.kp = 0.5f;
    c.gain = 2.0f;
    c.integral_max = 100.0f;
    c.start_frames = 6;
    c.press_max_count = 20.0f;
    c.baseline_tau_ms = 0.0f;   // Case1-9：原始偏差口径（回归保护）
    return c;
}

constexpr float kDt = 16.0f;   // ~60fps

// Case1: 默认关 / 未开火 ⇒ 零输出，且不留任何状态
void test_disabled_and_no_fire() {
    std::printf("[Case1] enabled=false 与未开火 ⇒ 零输出、零状态\n");
    RecoilClosedLoop rc;
    RecoilClConfig cfg = make_cfg();
    cfg.enabled = false;
    const auto a = rc.update(false, true, true, 10.0f, kDt, cfg);
    check(a.add_y == 0.0f && !a.active, "enabled=false ⇒ 零输出");
    check(rc.state() == RecoilClosedLoop::kOff, "enabled=false ⇒ state=kOff");

    RecoilClosedLoop rc2;
    const auto cfg2 = make_cfg();
    const auto b = rc2.update(true, false, true, 10.0f, kDt, cfg2);
    check(b.add_y == 0.0f, "未开火 ⇒ 零输出");
    check(rc2.obs_frames() == 0, "未开火 ⇒ 观测计数为零（无跨开火记忆）");
}

// Case2: 观察期（前 start_frames-1 帧）不压，第 start_frames 帧开始压
void test_observation_window() {
    std::printf("[Case2] 观察期不压：前 5 帧零输出，第 6 帧起下压\n");
    RecoilClosedLoop rc;
    const auto cfg = make_cfg();   // start_frames = 6
    float first5 = 0.0f;
    for (int i = 0; i < 5; ++i) {
        const auto out = rc.update(true, true, true, 10.0f, kDt, cfg);
        first5 += out.add_y;
    }
    check(first5 == 0.0f, "观察期（前 5 帧）零输出");
    check(rc.obs_frames() == 5, "观察期内仍在计数（第 6 帧才会压）");
    const auto out6 = rc.update(true, true, true, 10.0f, kDt, cfg);
    check(out6.add_y > 0.0f, "第 6 帧（= start_frames）开始下压");
    check(out6.active, "第 6 帧 active=true");
    check(rc.state() == RecoilClosedLoop::kPressing, "状态 = 正在压");
}

// Case3: 观测中断 ⇒ 立即清零（不猜、不留保持窗）
void test_obs_drop_resets_immediately() {
    std::printf("[Case3] 观测中断 ⇒ 本帧零输出且积分立即清零\n");
    RecoilClosedLoop rc;
    const auto cfg = make_cfg();
    for (int i = 0; i < 20; ++i) rc.update(true, true, true, 10.0f, kDt, cfg);
    check(rc.integral_px_s() > 0.0f, "20 帧后积分 > 0（确实在压）");
    const auto out = rc.update(true, true, false, 10.0f, kDt, cfg);
    check(out.add_y == 0.0f, "观测不合格那一帧零输出");
    check(rc.integral_px_s() == 0.0f, "积分立即清零（不猜）");
    check(rc.obs_frames() == 0, "观测帧计数清零（下段观测重新累积）");
    check(rc.state() == RecoilClosedLoop::kOff, "状态回到 kOff");
}

// Case4: 单向 —— 负偏差时积分回落并钳在 0，输出永不为负
void test_one_way_down_only() {
    std::printf("[Case4] 单向：负偏差下积分钳 0，输出永不为负\n");
    RecoilClosedLoop rc;
    const auto cfg = make_cfg();
    for (int i = 0; i < 20; ++i) rc.update(true, true, true, 10.0f, kDt, cfg);
    check(rc.integral_px_s() > 0.0f, "先建立正积分");
    for (int i = 0; i < 500; ++i) {
        const auto out = rc.update(true, true, true, -100.0f, kDt, cfg);
        if (out.add_y < 0.0f) { check(false, "输出出现负值（把准星上推了）"); return; }
    }
    check(rc.integral_px_s() >= 0.0f, "负偏差下积分钳在 0");
    check(rc.p_term() == 0.0f, "负偏差下比例项也钳在 0（单向）");
    check(rc.add_y() >= 0.0f, "输出恒 >= 0");
}

// Case5: 限幅 —— 积分上限与单帧下压上限都生效
void test_limits() {
    std::printf("[Case5] 限幅：积分 <= integral_max，单帧输出 <= press_max_count\n");
    RecoilClosedLoop rc;
    const auto cfg = make_cfg();
    float max_out = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const auto out = rc.update(true, true, true, 1000.0f, kDt, cfg);
        if (out.add_y > max_out) max_out = out.add_y;
    }
    check(rc.integral_px_s() <= cfg.integral_max + 1e-3f, "积分不超过 integral_max");
    check(max_out <= cfg.press_max_count + 1e-3f, "单帧输出不超过 press_max_count");
    check(max_out > 0.0f, "限幅下仍在输出（不是被压死为 0）");
}

// Case6: reset 清空（换目标 / 换世代用）
void test_reset() {
    std::printf("[Case6] reset 清空积分 / 帧数 / 输出\n");
    RecoilClosedLoop rc;
    const auto cfg = make_cfg();
    for (int i = 0; i < 20; ++i) rc.update(true, true, true, 10.0f, kDt, cfg);
    check(rc.integral_px_s() > 0.0f, "复位前有积分");
    rc.reset();
    check(rc.integral_px_s() == 0.0f, "reset 后积分为 0");
    check(rc.obs_frames() == 0, "reset 后观测帧数为 0");
    check(rc.add_y() == 0.0f, "reset 后输出为 0");
    check(rc.state() == RecoilClosedLoop::kOff, "reset 后状态 kOff");
}

// Case7: 正偏差 ⇒ 持续累积、输出单调不减
void test_accumulates_on_positive_error() {
    std::printf("[Case7] 正偏差下输出随观测帧数单调累积\n");
    RecoilClosedLoop rc;
    const auto cfg = make_cfg();
    float prev = -1.0f;
    bool mono = true;
    for (int i = 0; i < 30; ++i) {
        const auto out = rc.update(true, true, true, 5.0f, kDt, cfg);
        if (out.add_y + 1e-6f < prev) mono = false;
        prev = out.add_y;
    }
    check(mono, "输出随帧数单调不减");
    check(prev > 0.0f, "稳态输出 > 0");
}

// Case8: 真闭环仿真 —— 后坐力持续推大偏差，注入把它压回收敛区
//
// 被控对象（与 aim_replay.py 同一量纲口径）：
//   偏差(py) 每帧 + recoil_px_per_s × dt        （枪口上抬 = 目标框在画面里下移）
//   注入 count × px_per_count 把它拉回          （gain 只乘一次，不重复折算）
void test_closed_loop_converges() {
    std::printf("[Case8] 闭环收敛：持续后坐力下偏差被压回小区域\n");
    const float ppc = 0.65f;              // px/count（与 AimThread.cpp:281 默认一致）
    const float recoil_px_per_s = 30.0f;  // 后坐力：每秒把偏差推大 30px
    const float dt_s = kDt / 1000.0f;     // 0.016s
    const int frames = 300;               // 4.8s

    RecoilClosedLoop rc;
    const auto cfg = make_cfg();
    float err = 1.0f;                     // 起始偏差 1px
    float max_err_after_ramp = 0.0f;
    for (int i = 0; i < frames; ++i) {
        err += recoil_px_per_s * dt_s;                                  // 后坐力
        const auto out = rc.update(true, true, true, err, kDt, cfg);     // 闭环观测+注入
        err -= out.add_y * ppc;                                          // 注入的反向位移
        if (err < 0.0f) err = 0.0f;
        if (i > 60 && err > max_err_after_ramp) max_err_after_ramp = err;
    }
    const float open_loop = recoil_px_per_s * dt_s * static_cast<float>(frames);  // 完全不压的累计
    std::printf("     开环累计 %.1fpx / 闭环末值 %.2fpx / 起压后峰值 %.2fpx\n",
                open_loop, err, max_err_after_ramp);
    check(err < open_loop * 0.10f, "闭环末值 < 开环的 10%");
    check(max_err_after_ramp < 0.5f * open_loop, "起压后峰值远小于开环累计");
    check(err >= 0.0f, "偏差不为负（单向注入没有把准星推过目标）");
}

// Case9: 比例项 —— 短促点射里起压第一帧就有输出（2026-09-29 业主令「先做 1」）
//
// 纯积分要"从 0 爬起来"：点射一次只按住 ~120ms，每次松手清零后都要重新起步，
// 累计下压量只有长按同长度的零头。比例项 = 看到多少偏移就立刻按比例补，不等累积。
void test_kp_immediate_output_for_tap_fire() {
    std::printf("[Case9] 比例项：点射起压第一帧就有输出（kp=0 vs kp=0.5）\n");

    struct R { float sum; float first_press; };
    auto run_tap = [](float kp) -> R {
        RecoilClosedLoop rc;
        RecoilClConfig cfg = make_cfg();
        cfg.kp = kp;
        const float err = 2.0f;   // 点射典型偏移：目标框在准星下方 2px
        R r{0.0f, 0.0f};
        bool got_first = false;
        for (int i = 0; i < 30; ++i) {   // 按住 120ms（主循环 4ms/帧 ⇒ 30 帧）
            const auto out = rc.update(true, true, true, err, 4.0f, cfg);
            r.sum += out.add_y;
            if (!got_first && out.add_y > 0.0f) { r.first_press = out.add_y; got_first = true; }
        }
        return r;
    };

    const R no_kp = run_tap(0.0f);      // 纯积分（改前）
    const R with_kp = run_tap(0.5f);    // 比例 + 积分（改后）
    std::printf("     kp=0 首帧 %.4f 累计 %.3f | kp=0.5 首帧 %.4f 累计 %.3f\n",
                no_kp.first_press, no_kp.sum, with_kp.first_press, with_kp.sum);
    check(with_kp.first_press > 0.5f, "kp=0.5：起压首帧输出就不小（不是从 0 爬）");
    check(with_kp.first_press > 10.0f * no_kp.first_press,
          "比例项让首帧输出比纯积分大 10 倍以上");
    check(with_kp.sum > 5.0f * no_kp.sum, "整段点射累计下压远大于纯积分（5 倍以上）");
    check(no_kp.sum >= 0.0f && with_kp.sum > 0.0f, "两种配置都不出现负输出");
}

// Case10: 基线口径 —— 学底子 → 开火用「偏差−底子」；底子冻结；毛刺钳位
void test_baseline_mode() {
    std::printf("[Case10] 基线口径：底子不挡路、开火中冻结、毛刺钳 100px\n");
    RecoilClosedLoop rc;
    RecoilClConfig cfg = make_cfg();
    cfg.baseline_tau_ms = 1500.0f;   // 基线开

    // ① 没开火：底子收敛到静态偏移 -15px（多帧 EMA）
    for (int i = 0; i < 300; ++i) rc.track_baseline(-15.0f, 4.0f, cfg);
    const float bl = rc.baseline();
    check(bl > -18.0f && bl < -12.0f, "底子收敛到 -15px 附近");

    // ② 开火：偏差 = 底子 + 后坐力残留 40px（原始值 +25，第一版会被负底子卡住）
    float sum = 0.0f;
    for (int i = 0; i < 30; ++i) {
        const auto out = rc.update(true, true, true, -15.0f + 40.0f, 4.0f, cfg);
        sum += out.add_y;
    }
    check(sum > 30.0f, "原始偏差 +25px 也能压（不被 -15 底子挡住）");

    // ③ 开火中底子冻结：长喷 500 帧后底子仍是开火前的值（没把后坐力学进去）
    for (int i = 0; i < 500; ++i) rc.update(true, true, true, 60.0f, 4.0f, cfg);
    check(rc.baseline() == bl, "开火期间底子冻结");

    // ④ 毛刺钳位：偏差突然 +300px（检测框跳变），e_hp 钳 100
    rc.reset();
    for (int i = 0; i < 300; ++i) rc.track_baseline(-15.0f, 4.0f, cfg);
    for (int i = 0; i < 8; ++i) rc.update(true, true, true, 285.0f, 4.0f, cfg);
    check(rc.p_term() <= 0.5f * 100.0f + 1e-3f, "毛刺被钳在 e_hp<=100（kp=0.5 → p_term<=50）");
    check(rc.add_y() <= cfg.press_max_count + 1e-3f, "单帧输出仍受安全阀约束");

    // ⑤ 偏差等于底子（无后坐力）⇒ 不压
    rc.reset();
    for (int i = 0; i < 300; ++i) rc.track_baseline(-15.0f, 4.0f, cfg);
    float sum0 = 0.0f;
    for (int i = 0; i < 30; ++i) sum0 += rc.update(true, true, true, -15.0f, 4.0f, cfg).add_y;
    check(sum0 == 0.0f, "偏差贴着底子（无变化）⇒ 零输出");

    // ⑥ 全量复位清底子：换目标后重新学
    rc.reset();
    check(!(rc.baseline() == rc.baseline()), "reset 后底子未就绪（NaN）");  // NaN != NaN
}

// Case11: 基线口径的闭环收敛仿真 —— 后坐力把偏差从底子上顶起来，闭环压回底子附近
void test_baseline_closed_loop() {
    std::printf("[Case11] 基线口径闭环：底子 -15px 起步，后坐力残留被压回\n");
    const float ppc = 0.65f;
    const float base = -15.0f;          // 静态瞄准偏移
    const float recoil_px_per_s = 30.0f;
    const float dt_ms = 4.0f;          // 主循环 250Hz
    RecoilClosedLoop rc;
    RecoilClConfig cfg = make_cfg();
    cfg.baseline_tau_ms = 1500.0f;
    cfg.kp = 0.5f;
    for (int i = 0; i < 400; ++i) rc.track_baseline(base, dt_ms, cfg);
    float err = base;                  // 原始偏差
    float peak_hp = 0.0f;
    for (int i = 0; i < 600; ++i) {    // 2.4s 连喷
        err += recoil_px_per_s * (dt_ms / 1000.0f);   // 后坐力顶
        const auto out = rc.update(true, true, true, err, dt_ms, cfg);
        err -= out.add_y * ppc;                        // 纠偏拉回
        if (err - base > peak_hp) peak_hp = err - base;
        if (err < base) err = base;                    // 不把准星推过目标
    }
    const float open_loop = recoil_px_per_s * 0.004f * 600.0f;
    std::printf("     开环累计 %.1fpx / 相对底子末值 %.2fpx / 峰值 %.2fpx\n",
                open_loop, err - base, peak_hp);
    check(err - base < open_loop * 0.10f, "相对底子的末值 < 开环 10%");
    check(peak_hp < 0.5f * open_loop, "峰值远小于开环累计");
}

}  // namespace

int main() {
    std::printf("=== test_recoil_closed_loop 开火期闭环纠偏（压枪 v1）===\n");
    test_disabled_and_no_fire();
    test_observation_window();
    test_obs_drop_resets_immediately();
    test_one_way_down_only();
    test_limits();
    test_reset();
    test_accumulates_on_positive_error();
    test_closed_loop_converges();
    test_kp_immediate_output_for_tap_fire();
    test_baseline_mode();
    test_baseline_closed_loop();
    std::printf("结果: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
