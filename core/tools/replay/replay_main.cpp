// replay_main.cpp — 自瞄控制器离线回放器（V1.0.46）
//
// ★ 为什么这个工具必须是 C++（V1.0.46 的教训）
//   上板跑的是 C++ 控制器。之前的做法是"Python 精确移植一份 → 在 Python 里回放"，
//   那是**二次翻译**：Python 与 C++ 是两套独立实现，若两边都误解了原始算法
//   （例如都把 Fitts 的 MT 含义搞错），互验也发现不了 —— 事实正是如此：
//   V1.0.43 的 τ=MT 错误在 Python 与 C++ 里犯了**一模一样**的错。
//   现在改成：回放器直接 #include 产品头文件，链接的就是上板那份代码。
//   Python 只负责"跑多个场景 + 判阈值 + 出报告"，不碰控制逻辑。
//
// 输入
//   --trace <csv>       板端 DetTrace 录制的 CSV（真实检测序列）。不给则跑内置合成轨迹。
//   --kind fitts|pid1   控制器
//   --frames N          合成模式下生成多少帧
//   --out <json>        指标输出（给 selftest.py 判定用）
//   Fitts 参数：--fitts-a / --fitts-b / --fitts-dz / --fitts-ratio / --fitts-ff / --fitts-tau
//   pid1  参数：--kp / --kd / --predict / --rate / --smooth
//   物理量：--gain（px/count）、--delay-ms（回路延迟）、--frame-ms
//
// 闭环口径（与 AimThread.cpp 逐项对齐）
//   · 准星位置由本回放闭环驱动（从 0 起），误差 = 目标位置 − 准星位置
//   · 输出 count → ×gain 得 px 位移 → 经 --delay-ms 延迟队列才生效
//   · 死区判 **count 域**（AimThread 用 scaled，不是 px）
//   · remainder 跨帧携带 + 朝零截断（int16 量化）
//   · 检测噪声直接来自录制数据，不叠加
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aim/FittsAimController.hpp"
#include "aim/Pid1Controller.hpp"

using ttbox::core::aim::FittsAimController;
using ttbox::core::aim::Pid1Controller;

namespace {

struct Frame {
    double t_ms = 0.0;
    double box_w = 0.0;
    double box_h = 0.0;
    double target_x = 0.0;   // 目标**绝对位置**（准星在原点起步 ⇒ 初始误差即 target_x）
    int has_target = 1;
};

struct Cfg {
    std::string kind = "fitts";
    // fitts
    double f_a = 20.0, f_b = 20.0, f_dz = 3.0, f_ratio = 0.02, f_ff = 0.85, f_tau = 51.0;
    // pid1
    double kp = 25.0, kd = 25.0, predict = 0.5, rate = 0.3, smooth = 9900.0;
    // 公共
    double gain = 0.686;
    double delay_ms = 51.0;
    double frame_ms = 1000.0 / 144.0;
    double deadzone_count = 1.0;
    int frames = 1200;
    std::string trace;
    std::string out;
};

// ── CSV 解析 ──
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\r' && c != '\n') { cur.push_back(c); }
    }
    out.push_back(cur);   // ★ 尾部字段即使是空串也要保留（DetTrace 的空框占位）
    return out;
}

double num(const std::string& s) { return s.empty() ? 0.0 : std::atof(s.c_str()); }

std::vector<Frame> load_trace(const std::string& path, const Cfg& cfg) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "打不开 trace: %s\n", path.c_str()); return {}; }
    std::string line;
    if (!std::getline(f, line)) return {};
    // 列名索引
    auto h = split(line);
    auto idx = [&](const char* k) -> int {
        for (size_t i = 0; i < h.size(); ++i) if (h[i] == k) return static_cast<int>(i);
        return -1;
    };
    const int i_ts = idx("timestamp_us"), i_mv = idx("move_x"), i_cx = idx("ctrl_x");
    const int i_ax1 = idx("aim_x1"), i_ax2 = idx("aim_x2");
    const int i_ay1 = idx("aim_y1"), i_ay2 = idx("aim_y2");
    const int i_ty = idx("target_id");
    if (i_ts < 0 || i_cx < 0) {
        std::fprintf(stderr, "trace 缺 timestamp_us/ctrl_x 列（不是 DetTrace 文件）\n");
        return {};
    }
    std::vector<Frame> out;
    out.reserve(4096);
    // ★ 板端录的 ctrl_x 是**误差**；要还原目标绝对位置需加回板端准星已走的距离：
    //     target_x[i] = ctrl_x[i] + gain × Σ_{k<i} move_x[k]
    double cross = 0.0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto c = split(line);
        auto at = [&](int i) -> double { return (i >= 0 && i < static_cast<int>(c.size())) ? num(c[static_cast<size_t>(i)]) : 0.0; };
        Frame fr;
        fr.t_ms = at(i_ts) / 1000.0;
        fr.box_w = at(i_ax2) - at(i_ax1);
        fr.box_h = at(i_ay2) - at(i_ay1);
        fr.target_x = at(i_cx) + cross;
        fr.has_target = 1;
        (void)i_ty; (void)i_mv;
        cross += cfg.gain * at(i_mv);
        out.push_back(fr);
    }
    return out;
}

// ── 合成轨迹（无 trace 时的兜底；3D 场景由 Python 侧生成后走 --trace）──
std::vector<Frame> gen_synthetic(const Cfg& cfg) {
    std::vector<Frame> out;
    out.reserve(static_cast<size_t>(cfg.frames));
    unsigned st = 12345u;
    auto nxt = [&st]() { st = st * 1103515245u + 12345u; return static_cast<double>(st >> 8) / 8388608.0; };
    double pos = -40.0, vel = 0.0;
    for (int i = 0; i < cfg.frames; ++i) {
        vel += (nxt() * 2.0 - 1.0) * 8.0;
        vel = std::max(-150.0, std::min(150.0, vel));
        pos += vel * (cfg.frame_ms / 1000.0);
        Frame fr;
        fr.t_ms = i * cfg.frame_ms;
        fr.box_w = 80.0; fr.box_h = 160.0;
        fr.target_x = pos + (nxt() * 2.0 - 1.0) * 2.0;   // ±2px 检测噪声
        out.push_back(fr);
    }
    return out;
}

struct Metrics {
    double settle_med = 0.0, settle_p95 = 0.0, overshoot = 0.0;
    int flips = 0, move_frames = 0, stuck = 0, settle_frames = 0;
    double drift = 0.0, max_step = 0.0, jerk = 0.0;
    int frames = 0;
};

Metrics replay(const std::vector<Frame>& tr, const Cfg& cfg) {
    Metrics m;
    m.frames = static_cast<int>(tr.size());
    if (tr.empty()) return m;

    FittsAimController fitts;
    Pid1Controller pid1;
    if (cfg.kind == "fitts") fitts.configure(static_cast<float>(cfg.f_a), static_cast<float>(cfg.f_b),
                                               static_cast<float>(cfg.f_dz), static_cast<float>(cfg.f_ratio),
                                               static_cast<float>(cfg.f_ff), static_cast<float>(cfg.f_tau));
    else pid1.init(cfg.kp, cfg.kd, cfg.predict, cfg.rate, cfg.smooth);

    const int delay_n = std::max(0, static_cast<int>(std::lround(cfg.delay_ms / cfg.frame_ms)));
    std::vector<double> queue(static_cast<size_t>(delay_n), 0.0);
    std::size_t qhead = 0;
    double rem = 0.0, cross = 0.0, vel = 0.0, prev_tgt = 0.0;
    int sign_prev = 0, over = 0;
    bool crossed = false;
    std::vector<double> errs, outs;
    errs.reserve(tr.size()); outs.reserve(tr.size());

    for (size_t i = 0; i < tr.size(); ++i) {
        const Frame& fr = tr[i];
        const double e = fr.target_x - cross;   // 闭环观测误差
        if (i > 0) {
            const double dt = (fr.t_ms - tr[i - 1].t_ms) / 1000.0;
            if (dt > 1e-6) {
                const double v = (fr.target_x - tr[i - 1].target_x) / dt;  // ★跟目标位置（同 AimTracker）
                vel += 0.5 * (v - vel);
            }
        }
        double u = 0.0;
        if (cfg.kind == "fitts")
            u = fitts.update(static_cast<float>(e), static_cast<float>(fr.box_h),
                             static_cast<float>(cfg.gain), static_cast<float>(vel),
                             static_cast<float>(cfg.frame_ms));
        else
            u = pid1.update(e);
        if (!std::isfinite(u)) u = 0.0;
        if (std::fabs(u) < cfg.deadzone_count) u = 0.0;
        rem += u;
        double mv = 0.0;
        if (std::fabs(rem) >= 1.0) { mv = std::trunc(rem); rem -= mv; }
        mv = std::max(-32768.0, std::min(32767.0, mv));
        // ★ 延迟队列：先**读**队头（上一帧排队的位移）再写新值 —— 顺序反了就是零延迟
        //   （V1.0.46 第一版写成"先写再读同一格"，applied 立刻拿到本帧值
        //     ⇒ 51ms 延迟完全失效，回放结果与真机不符）。
        const double applied = (delay_n > 0) ? queue[qhead] : mv * cfg.gain;
        queue[qhead] = mv * cfg.gain;
        qhead = (qhead + 1) % static_cast<size_t>(std::max(1, delay_n));
        cross += applied;
        errs.push_back(e);
        outs.push_back(mv);

        if (!crossed && i > 3 && std::fabs(e) < 4.0) crossed = true;
        if (crossed && i > 3 && ((e < 0) != (errs[i - 1] < 0))) {
            over = std::max(over, static_cast<int>(std::fabs(e)));
        }
        const int s = mv > 0 ? 1 : (mv < 0 ? -1 : 0);
        if (s && sign_prev && s != sign_prev) ++m.flips;
        if (s) sign_prev = s;
        if (std::fabs(e) > 15.0 && mv == 0.0) ++m.stuck;
    }
    (void)prev_tgt;

    const size_t n = errs.size();
    const size_t tail_n = std::max<size_t>(30, n / 3);
    std::vector<double> tail;
    tail.reserve(tail_n);
    for (size_t i = n - tail_n; i < n; ++i) tail.push_back(std::fabs(errs[i]));
    std::sort(tail.begin(), tail.end());
    m.settle_med = tail.empty() ? 0.0 : tail[tail.size() / 2];
    m.settle_p95 = tail.empty() ? 0.0 : tail[std::min(tail.size() - 1, tail.size() * 95 / 100)];
    for (size_t i = n - tail_n; i < n; ++i) {
        if (std::fabs(outs[i]) >= 1.0) ++m.move_frames;
        m.max_step = std::max(m.max_step, std::fabs(outs[i]));
    }
    // 收敛帧数 + 末段漂移
    const double dz_base = (cfg.kind == "pid1") ? cfg.deadzone_count * cfg.gain : 3.0;
    for (size_t i = 0; i < n; ++i)
        if (std::fabs(errs[i]) > std::max(dz_base, 2.0)) m.settle_frames = static_cast<int>(i + 1);
    const size_t q = std::max<size_t>(1, n / 4);
    if (n > 8) {
        const size_t seg0 = n - q, seg1 = n - 2 * q;
        double h1 = 0.0, h2 = 0.0, ym = 0.0, xm = static_cast<double>(q - 1) / 2.0, num_ = 0.0, den = 0.0;
        for (size_t i = seg1; i < seg0; ++i) h1 = std::max(h1, std::fabs(errs[i]));
        for (size_t i = seg0; i < n; ++i) { h2 = std::max(h2, std::fabs(errs[i])); ym += errs[i]; }
        ym /= static_cast<double>(q);
        for (size_t i = 0; i < q; ++i) {
            const double d = static_cast<double>(i) - xm;
            num_ += d * (errs[seg0 + i] - ym);
            den += d * d;
        }
        if (den > 1e-9) m.drift = num_ / den * 1000.0;
        // jerk（参考值，判据里不用）
        double jm = 0.0;
        for (size_t i = n - q + 2; i < n; ++i)
            jm += std::fabs(errs[i] - 2.0 * errs[i - 1] + errs[i - 2]);
        m.jerk = jm / static_cast<double>(q);
    }
    m.overshoot = over;
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    Cfg cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto nxt = [&]() { return (i + 1 < argc) ? std::atof(argv[++i]) : 0.0; };
        if (a == "--trace") cfg.trace = argv[++i];
        else if (a == "--kind") cfg.kind = argv[++i];
        else if (a == "--frames") cfg.frames = static_cast<int>(nxt());
        else if (a == "--out") cfg.out = argv[++i];
        else if (a == "--fitts-a") cfg.f_a = nxt();
        else if (a == "--fitts-b") cfg.f_b = nxt();
        else if (a == "--fitts-dz") cfg.f_dz = nxt();
        else if (a == "--fitts-ratio") cfg.f_ratio = nxt();
        else if (a == "--fitts-ff") cfg.f_ff = nxt();
        else if (a == "--fitts-tau") cfg.f_tau = nxt();
        else if (a == "--kp") cfg.kp = nxt();
        else if (a == "--kd") cfg.kd = nxt();
        else if (a == "--predict") cfg.predict = nxt();
        else if (a == "--rate") cfg.rate = nxt();
        else if (a == "--smooth") cfg.smooth = nxt();
        else if (a == "--gain") cfg.gain = nxt();
        else if (a == "--delay-ms") cfg.delay_ms = nxt();
        else if (a == "--frame-ms") cfg.frame_ms = nxt();
    }

    std::vector<Frame> tr = cfg.trace.empty() ? gen_synthetic(cfg) : load_trace(cfg.trace, cfg);
    if (tr.empty()) { std::fprintf(stderr, "没有轨迹可回放\n"); return 2; }
    const Metrics m = replay(tr, cfg);

    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "{\n \"frames\": %d,\n \"settle_med\": %.3f,\n \"settle_p95\": %.3f,\n"
        " \"overshoot_px\": %d,\n \"sign_flips\": %d,\n \"stuck_frames\": %d,\n"
        " \"settle_frames\": %d,\n \"drift_px\": %.1f,\n \"max_step\": %.2f,\n"
        " \"jerk\": %.3f,\n \"move_frames\": %d\n}\n",
        m.frames, m.settle_med, m.settle_p95, m.overshoot, m.flips, m.stuck,
        m.settle_frames, m.drift, m.max_step, m.jerk, m.move_frames);
    if (cfg.out.empty()) std::fputs(buf, stdout);
    else {
        std::ofstream f(cfg.out);
        f << buf;
    }
    return 0;
}
