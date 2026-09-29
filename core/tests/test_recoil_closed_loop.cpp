// test_recoil_closed_loop.cpp — 开火期闭环纠偏（压枪 v2「保持型积分修正」）单元测试
//
// 锁的是业主 2026-09-29 定案的原则与实现不变量：
//   ①「有实时偏移就纠偏，没有实时偏移就不猜」—— 开关关 / 未开火 / 观测不合格
//      ⇒ 立即清零，不留保持窗、不留跨开火记忆；
//   ②「前几发不压」是设计内代价 —— start_frames 观察期内零输出；
//   ③ 单向：只下压（积分下限钳 0），永远不会把准星往上推；
//   ④ 限幅：积分上限 + 单帧下压上限两道安全阀；
//   ⑤ 默认关 = 零输出（不开时输出链与加入前逐字节一致）；
//   ⑥ 真闭环仿真：后坐力持续把偏差推大时，注入能把偏差压回收敛区；
//   ⑦ 比例项（kp）：短促点射里起压第一帧就有可观输出，不依赖积分爬升 ——
//      Case9 锁的是**机制**（kp 关/开对比），注意 v2 已把 kp 默认值改成 0，见 Case14。
//   ⑧ 基线口径（2026-09-29 15:3x 实机数据定障）：观测量 = 偏差 − 没开火时学的底子。
//      实机：原始偏差开火时仅 41% 为正（被静态负偏移埋住）⇒ 第一版 27 秒只压 17 count；
//      减底线后 98% 为正、中位 +56px。Case1-9 锁的是 baseline_tau_ms=0 的原始口径（回归保护），
//      Case10/11 锁基线模式本体。
//   ⑨ ★ v2 新增不变量（2026-09-29 16:xx 实机「开始乱晃」定障后补）：
//      抗饱和（积分项单独不得超单帧上限 + 限幅后反算回写 + 饱和期停止累积）、
//      输出限速（单帧变化上限 = 阻尼）、以及"v1 那套参数再也积不到 5 倍上限"的回归保护。
//   ⑩ ★ V1.0.06 新增不变量（2026-09-29 20:57 实机「一会儿压得狠、一会儿压根不压」定障）：
//      基线学习的**几何门槛** —— |err_y| > 40px 的帧整帧不学，防开火间隙转头/瞄地面把零点带跑；
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

// Case1-11 用它锁**机制**（P+I 数学、限幅、单向、基线口径）。
// v1 那套数值（kp=0.5/gain=2.0/integral_max=100/press_max_count=20）刻意保留：
// 它同时充当"老参数在新代码下不再失控"的回归场景。
// slew_count_per_frame = 0 ⇒ 这些用例测的是无限速的裸机制（限速单独由 Case13 锁）。
RecoilClConfig make_cfg() {
    RecoilClConfig c;
    c.enabled = true;
    c.kp = 0.5f;
    c.gain = 2.0f;
    c.integral_max = 100.0f;
    c.start_frames = 6;
    c.press_max_count = 20.0f;
    c.slew_count_per_frame = 0.0f;   // Case1-11：无限速，锁裸机制
    c.baseline_tau_ms = 0.0f;        // Case1-9：原始偏差口径（回归保护）
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
// ★ 注意：v2 已把 kp 默认改成 0（P 归瞄准 PID，见 Case14）。本用例锁的是
//   「kp 这个旋钮存在且行为如描述」，不是"默认该开"。
void test_kp_immediate_output_for_tap_fire() {
    std::printf("[Case9] 比例项机制：点射起压第一帧就有输出（kp=0 vs kp=0.5）\n");

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

    const R no_kp = run_tap(0.0f);      // 纯积分
    const R with_kp = run_tap(0.5f);    // 比例 + 积分
    std::printf("     kp=0 首帧 %.4f 累计 %.3f | kp=0.5 首帧 %.4f 累计 %.3f\n",
                no_kp.first_press, no_kp.sum, with_kp.first_press, with_kp.sum);
    check(with_kp.first_press > 0.5f, "kp=0.5：起压首帧输出就不小（不是从 0 爬）");
    check(with_kp.first_press > 10.0f * no_kp.first_press,
          "比例项让首帧输出比纯积分大 10 倍以上");
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

// Case15: 基线学习的几何门槛（V1.0.06 新增）—— 只有"目标就在画面中心附近"的帧才学底子
//
// 2026-09-29 20:57 实机定障：`track_baseline` 以 τ=2000ms 慢学"没开火时瞄在哪"当零点，但**没有几何门槛**
// ⇒ 开火间隙转头/瞄地面时瞄点跑到画面边缘，归一域偏差冲到 ±300px，EMA 被拉走且 2 秒回不来
// ⇒ 下一段开火直接拿着跑偏的零点压（实测 13 段 base 从 -281 跨到 +0.7，顶格率 0% ↔ 88%，
//   即业主所感「一会儿压得狠、一会儿压根不压」）。
// 修法：|err_y| > 40px 的帧**整帧不学**（不是钳位 —— 钳位会把 -281 学成 -40，仍偏离真实零点）。
void test_baseline_geometry_gate() {
    std::printf("[Case15] 基线几何门槛：大偏差帧整帧不学（含边界 40px）\n");
    RecoilClConfig cfg = make_cfg();
    cfg.baseline_tau_ms = 1500.0f;

    // ① 全程大偏差 ⇒ 底子始终未就绪（NaN），不会被带跑
    {
        RecoilClosedLoop rc;
        for (int i = 0; i < 400; ++i) rc.track_baseline(300.0f, 4.0f, cfg);
        check(!(rc.baseline() == rc.baseline()), "全是大正偏差 ⇒ 底子仍未就绪（NaN != NaN）");
        for (int i = 0; i < 400; ++i) rc.track_baseline(-300.0f, 4.0f, cfg);
        check(!(rc.baseline() == rc.baseline()), "反方向大偏差同样不学");
    }

    // ② 门槛内正常学
    RecoilClosedLoop rc;
    for (int i = 0; i < 300; ++i) rc.track_baseline(-15.0f, 4.0f, cfg);
    check(rc.baseline() > -18.0f && rc.baseline() < -12.0f, "门槛内（15px）照常收敛到 -15px 附近");

    // ③ 底子已就绪后再来大偏差 ⇒ 底子一动不动（复现开火间隙转头那一下）
    const float bl = rc.baseline();
    for (int i = 0; i < 300; ++i) rc.track_baseline(280.0f, 4.0f, cfg);
    check(rc.baseline() == bl, "已就绪后的大偏差不改变底子");
    for (int i = 0; i < 300; ++i) rc.track_baseline(-280.0f, 4.0f, cfg);
    check(rc.baseline() == bl, "两个方向的大偏差都不改变底子");

    // ④ 边界：恰好 40px 学进去，40.1px 不学
    {
        RecoilClosedLoop r_in;
        r_in.track_baseline(40.0f, 4.0f, cfg);
        check(r_in.baseline() == 40.0f, "恰好 40px：学（门槛含等于）");
        RecoilClosedLoop r_out;
        r_out.track_baseline(40.1f, 4.0f, cfg);
        check(!(r_out.baseline() == r_out.baseline()), "40.1px：不学（整帧跳过）");
    }

    // ⑤ 门槛不影响"开火中冻结"：门槛只是不学，不改冻结语义
    {
        RecoilClosedLoop r2;
        for (int i = 0; i < 300; ++i) r2.track_baseline(-15.0f, 4.0f, cfg);
        const float b0 = r2.baseline();
        for (int i = 0; i < 50; ++i) r2.update(true, true, true, 60.0f, 4.0f, cfg);
        check(r2.baseline() == b0, "开火（update）期间底子不变");
    }
}

// Case12: 抗饱和（v2 新增）—— 积分项单独不得超单帧上限 + 反算回写 + 饱和期停止累积
//
// v1 的实机故障：integral_max=100、gain=2.0 ⇒ 积分项最大 200，而单帧上限只有 20
// ⇒ 积起来就是 10 倍超限的顶格输出（实测 i_term 冲到 108 才被压回去，压过头即摆动）。
// v2 用结构上限 + 反算回写堵死：**即使配置仍是 v1 那套值**，积分也积不过单帧上限。
void test_anti_windup() {
    std::printf("[Case12] 抗饱和：v1 老参数下积分项也不得超单帧上限（反算回写）\n");
    RecoilClosedLoop rc;
    RecoilClConfig cfg = make_cfg();   // ★ 故意用 v1 老参数：integral_max=100 / gain=2.0
    cfg.kp = 0.0f;                     // 只看积分项，隔离 P 的干扰
    cfg.slew_count_per_frame = 0.0f;   // 隔离限速，只看抗饱和

    for (int i = 0; i < 5000; ++i) rc.update(true, true, true, 200.0f, 4.0f, cfg);
    const float cap_i = cfg.press_max_count / cfg.gain;   // 20/2 = 10 px·s
    std::printf("     积分 %.2f（结构上限 %.2f，配置 integral_max %.1f）i_term %.2f\n",
                rc.integral_px_s(), cap_i, cfg.integral_max, rc.i_term());
    check(rc.integral_px_s() <= cap_i + 1e-3f,
          "积分被反算回写到 <= 单帧上限/gain（配置 integral_max 再大也无效）");
    check(rc.i_term() <= cfg.press_max_count + 1e-3f, "积分项单独不超过单帧上限");
    check(rc.add_y() <= cfg.press_max_count + 1e-3f, "单帧输出仍受安全阀约束");

    // 反向偏差 ⇒ 积分单调回落，且在有限帧内归零（不留长期拖尾）
    const float before = rc.integral_px_s();
    int frames_to_zero = 0;
    for (int i = 0; i < 5000; ++i) {
        rc.update(true, true, true, -20.0f, 4.0f, cfg);
        ++frames_to_zero;
        if (rc.integral_px_s() <= 0.0f) break;
    }
    check(rc.integral_px_s() <= before, "反向偏差下积分单调回落");
    check(rc.integral_px_s() == 0.0f, "反向偏差下积分最终归零");
    check(rc.add_y() == 0.0f, "积分归零 ⇒ 输出为 0（不留拖尾）");
    std::printf("     归零用 %d 帧（%.2fs）\n", frames_to_zero, frames_to_zero * 0.004f);
}

// Case13: 输出限速（v2 新增，阻尼）—— 单帧变化不超过 slew_count_per_frame
//
// v1 无阻尼：250Hz 下任何一跳检测跳变都变成整帧猛踢（业主手感「乱晃」的直接来源）。
void test_slew_limit() {
    std::printf("[Case13] 限速：单帧输出变化 <= slew（软启动 + 软回零）\n");
    RecoilClosedLoop rc;
    RecoilClConfig cfg = make_cfg();
    cfg.kp = 5.0f;                    // 故意给一个很大的 P，制造"想猛踢"的原始需求
    cfg.press_max_count = 20.0f;
    cfg.slew_count_per_frame = 0.3f;

    float prev = 0.0f;
    float max_delta = 0.0f;
    bool monotonic_up = true;
    int rise_frames = 0;
    for (int i = 0; i < 400; ++i) {
        const auto out = rc.update(true, true, true, 50.0f, 4.0f, cfg);
        const float d = out.add_y - prev;
        if (d > max_delta) max_delta = d;
        if (d < -1e-6f) monotonic_up = false;
        prev = out.add_y;
        ++rise_frames;
        if (out.add_y >= cfg.press_max_count - 1e-3f) break;
    }
    std::printf("     单帧最大涨幅 %.4f（上限 %.2f）/ 爬到顶用 %d 帧（%.2fs）\n",
                max_delta, cfg.slew_count_per_frame, rise_frames, rise_frames * 0.004f);
    check(max_delta <= cfg.slew_count_per_frame + 1e-4f, "单帧涨幅不超过 slew");
    check(monotonic_up, "限速下输出单调爬升（无跳变）");
    check(prev >= cfg.press_max_count - 1e-3f, "限速只是限速，最终还是能爬到上限");

    // 软回零：偏差转负后输出逐帧下降，单帧跌幅同样不超过 slew
    float max_drop = 0.0f;
    for (int i = 0; i < 400; ++i) {
        const auto out = rc.update(true, true, true, -50.0f, 4.0f, cfg);
        const float d = prev - out.add_y;
        if (d > max_drop) max_drop = d;
        prev = out.add_y;
        if (out.add_y <= 0.0f) break;
    }
    std::printf("     单帧最大跌幅 %.4f / 末值 %.4f\n", max_drop, prev);
    check(max_drop <= cfg.slew_count_per_frame + 1e-4f, "单帧跌幅不超过 slew");
    check(prev == 0.0f, "最终回到 0");

    // slew = 0 ⇒ 不限速（回到裸机制）。注意要先过 start_frames 观察期。
    RecoilClosedLoop rc2;
    RecoilClConfig cfg2 = make_cfg();
    cfg2.kp = 5.0f;
    cfg2.slew_count_per_frame = 0.0f;
    auto fast = rc2.update(true, true, true, 50.0f, 4.0f, cfg2);
    for (int i = 0; i < 8; ++i) fast = rc2.update(true, true, true, 50.0f, 4.0f, cfg2);
    check(fast.add_y > 0.3f, "slew=0 ⇒ 过了观察期一帧就顶到限幅值（不受限速）");
}

// Case14: v2 默认值不变量（面板/后端/结构体三处同源，这里锁结构体这端）
void test_v2_defaults() {
    std::printf("[Case14] v2 默认值：kp=0（P 归瞄准 PID）、量级下调、限速默认开\n");
    const RecoilClConfig c;   // 默认构造 = 出厂默认
    std::printf("     kp=%.2f gain=%.2f integral_max=%.1f press_max=%.1f slew=%.2f tau=%.0f\n",
                c.kp, c.gain, c.integral_max, c.press_max_count,
                c.slew_count_per_frame, c.baseline_tau_ms);
    check(c.enabled == false, "默认关（不开时行为零变化）");
    check(c.kp == 0.0f, "默认 kp=0：P 归瞄准 PID，闭环不与它抢执行器（v1 摆动真凶）");
    check(c.gain > 0.0f && c.gain <= 0.5f, "默认积分增益 <= 0.5（慢修正，不是猛踢）");
    check(c.press_max_count > 0.0f && c.press_max_count <= 4.0f,
          "默认单帧上限 <= 4 count（约 650 px/s 以内，远小于 v1 的 20）");
    check(c.integral_max > 0.0f && c.gain * c.integral_max <= c.press_max_count + 1e-3f,
          "默认下积分项自己就吃不掉整个单帧预算（gain×integral_max <= press_max）");
    check(c.slew_count_per_frame > 0.0f, "默认启用限速（阻尼）");
    check(c.baseline_tau_ms > 0.0f, "默认启用基线（开火前静止位置 = 保持目标）");
    // ★ V1.0.05（2026-09-29 19:xx）：本引擎天生单轴（Output 只有 add_y），接管期间老引擎整段
    //   不跑 ⇒ 横向补偿归零（业主实机「弹道偏左偏出人身」）。keep_horiz 默认必须为 true，
    //   否则等于把那个缺陷又装回去。
    check(c.keep_horiz == true,
          "默认保留横向补偿（接管时老引擎只出横向）—— 关掉就等于横向没人管");
}

}  // namespace

int main() {
    std::printf("=== test_recoil_closed_loop 开火期闭环纠偏（压枪 v2 保持型积分修正）===\n");
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
    test_anti_windup();
    test_slew_limit();
    test_v2_defaults();
    test_baseline_geometry_gate();
    std::printf("结果: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
