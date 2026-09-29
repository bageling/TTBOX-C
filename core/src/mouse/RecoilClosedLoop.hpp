// RecoilClosedLoop.hpp — 开火期闭环纠偏引擎（压枪 v2「保持型积分修正」）
//
// 定位
// ----
// 这是「有实时偏移就纠偏，没有实时偏移就不猜」这句定案的直接实现：
// 开火后**每一帧**看实际偏了多少（control_y，像素），下一帧反向注入 count 把它拉回来，
// 连续纠偏，让弹道自己收敛在一个小区域里。不选枪、不录枪、不预采数据。
//
// 为什么必须由本模块补这一环（结构性理由）
// ----------------------------------------
// 瞄准 PID 的 Y 轴**没有积分**：AimThread.cpp:25 `pid_y_.init(25.0, 25.0, 0.0, ...)`
// 第 3 个实参是 predict，而它只乘在积分通道上
// （Pid1Controller.hpp:83 `ki_raw = (raw_velocity_input * predict) * integral_gain`）
// ⇒ predict_y = 0 把 K_i 整个乘成 0 ⇒ 默认配置下 Y 轴只有 P+D。
// 后坐力是**斜坡扰动**（枪口匀速上抬 ⇒ 目标框在画面里匀速下移），斜坡只有 I 项吃得掉：
// P 必留稳态误差、D 在稳态下不干活。本模块 = 把这一环补回来。
//
// ★★ v2 重做（2026-09-29 16:xx，实机「开始乱晃」定障）
// ----------------------------------------------------
// v1 上板（V1.0.02/V1.0.03 测试版）后业主实测「更垃圾、开始乱晃」。板端记录器数据定障，
// V1.0.03 测窗 74 个开火采样：
//   · add_y 合计 713.8 count，同口径是 V1.0.02 的 11.6 倍（每采样 9.65 vs 0.83）；
//   · 27% 采样顶死在 press_max_count=20（单帧 20 count，比瞄准 PID 整段输出
//     scheduler_input_y ≈ 1.4 还大 14 倍 ⇒ 闭环成了开火期的主导驱动）；
//   · i_term 最大 108，而单帧上限只有 20 ⇒ 积分自己就超上限 5 倍，积起来即永久顶格。
// 结构病根三条（与增益高低无关）：
//   ① **P 项与瞄准 PID 抢同一个执行器**。AimThread.cpp:819 `scaled_y += recoil_add_y`
//      与 PID 输出同域相加，而 PID 本来就在闭环地把 control_y 往 0 拉
//      ⇒ 同一个误差上叠两个控制器，等效 P 增益翻数倍 ⇒ 必然摆动。
//      v1 之所以"看起来没坏"，只是因为基线扣除前输出近 0（27 秒 17 count）。
//   ② **积分无抗饱和**：输出被限幅后积分继续积 ⇒ 顶格输出带拖尾，弹道压过头再被拉回。
//   ③ **输出无阻尼**：整段没有限速/微分，250Hz 下任何检测跳变都变成整帧猛踢。
// v2 改法（保留业主第一原则，只改结构）：
//   · kp 默认 **0** —— P 是瞄准 PID 的职责，闭环只补 PID 缺的积分；
//   · 积分抗饱和两件套：结构上限（积分项单独不得超单帧上限）+ 限幅后反算回写
//     （★ V1.0.06 去掉"饱和期停止累积"的粘滞开关 —— 它会把反向偏差也挡掉，见 update 注释）；
//   · 新增 slew_count_per_frame 限速（单帧输出变化上限）= 阻尼，防整帧猛踢；
//   · 量级整体下调：press_max_count 20→2（≈325 px/s 下压能力，按 0.65 px/count）、
//     gain 2.0→0.15、integral_max 100→10、底子 τ 1500→2000ms。
//
// 观测量：偏差减去慢基线（2026-09-29 15:3x，实机数据定案）
// ------------------------------------------------------
// 第一版直接拿 control_y 原始值当观测量，实机 27 秒只输出 17 count —— 数据定障：
//   · 瞄准系统常态带着静态负偏移（瞄准点取框顶 15%、Y 轴 PID 无 I 项 + 死区，
//     静止时 err_y ≈ -10~-20px）；
//   · 后坐力把偏差往正方向推，但被负底子抵住 ⇒「只在正偏差压」的门槛几乎永远过不去
//     （实测开火采样只有 41% 为正）。
// 修法：观测量换成 e_hp = control_y − baseline。baseline 是**没开火时**用慢 EMA 跟踪的
// 偏差底子（时间常数 baseline_tau_ms，默认 2s），几何含义是"开火前瞄点静止在哪"，
// 本模块因此成为一个**保持型**控制器：开火期间把瞄点稳在开火前那个位置上。
// baseline 开火期间冻结（防止长喷把后坐力学进底子）；换目标/换世代全量清；
// baseline 未就绪（刚换目标就开火）时以第一帧播种、该帧不压。
// baseline_tau_ms = 0 可退回"原始偏差"口径（留作对照/回退开关）。
//
// 为什么是闭环而不是前馈
// ---------------------
// 前馈（"测出后坐力速度再按固定比例外推整段"）必须假设"所有枪的 vy → count 比例相同"，
// 而不同枪的后坐力速度/方向/节奏/水平偏移都不同 ⇒ 那个比例根本不存在
// （业主 2026-09-29 指出）。闭环不需要它：枪是哪把、几倍镜、什么节奏，
// 全部由**实时实测的偏移**自己表达。
//
// 观测边界（诚实说明，勿在文档/面板上过度承诺）
// ---------------------------------------------
// e_hp 仍同时包含「目标自己移动」与「相机（枪口）运动」两个原因 —— 单看这一个数
// 不可分离。本模块不声称能识别后坐力，只做：① 观测合格性门控（obs_ok 构造见 AimThread）；
// ② 只在开火期、且偏移朝下（后坐力主方向）时输出，单向钳位。目标自身运动仍归 PID。
//
// 固有特性：闭环把 e_hp 压到 0 后观测量自己消失 —— 这是收敛的终态，不是缺陷。
//
// 与既有两套压枪引擎互斥（2026-09-29 业主令）：本引擎 enabled 时老引擎整段不跑
// （互斥逻辑在 AimThread 的 recoil_cl_takeover），关掉则完全走老引擎。
// 默认 enabled=false ⇒ 不开时输出链与加入前逐字节一致。
#pragma once

#include <cmath>
#include <limits>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class RecoilClosedLoop {
public:
    enum State : int {
        kOff = 0,        // 未开火 / 开关关 / 无实时观测（不猜）
        kObserving = 1,  // 观测中（连续帧数未达 start_frames，或偏移为 0 暂不下压）
        kPressing = 2,   // 正在下压
    };

    struct Output {
        float add_y = 0.0f;    // 本帧下压量（count，>=0，直接叠加进 scaled_y）
        bool active = false;   // 本帧是否真的在下压
    };

    // 没开火（或未按开火键）但有目标时每帧调用：慢 EMA 学"偏差底子"。
    // 开火期间不调用（baseline 冻结）。target 丢失期间也不调用（基线保持）。
    void track_baseline(float err_y_px, float dt_ms, const RecoilClConfig& cfg) {
        if (!(cfg.baseline_tau_ms > 0.0f)) return;   // 基线关闭：原始偏差口径
        // ★★ V1.0.06 几何门槛：只学「目标就在画面中心附近」的帧。
        // 定障（2026-09-29 20:57 实机，MR277 + 无框瞄具）：开火间隙转头/瞄地面时瞄准点跑到
        // 画面边缘，归一域偏差会冲到 ±300px；τ=2s 的慢 EMA 一旦被拉走，要 2 秒以上才回得来
        // ⇒ 下一次开火直接拿着跑偏的底子当零点（实测 13 个开火段的 base 从 -281 跨到 +0.7，
        //   顶格率随之 0% ↔ 88%，即业主所感「一会儿压得狠、一会儿压根不压」）。
        // 正常静态瞄偏实测量级为 -10 ~ -23px（全程 p50 = -21.9）⇒ 门槛取 40px。
        // 超门槛的帧**整帧不学**（而非钳位）——钳位会把 -281 学成 -40，仍偏离真实零点；
        // 整帧跳过则等待真正的静止帧来把 EMA 拉回。
        if (!(std::abs(err_y_px) <= kBaselineGatePx)) return;
        if (!std::isfinite(baseline_)) { baseline_ = err_y_px; return; }
        const float dt_s = dt_ms > 0.0f ? dt_ms / 1000.0f : 0.004f;
        const float alpha = dt_s / (cfg.baseline_tau_ms / 1000.0f);
        baseline_ += alpha * (err_y_px - baseline_);
    }

    // enabled / fire_active / obs_ok 任一不成立 ⇒ 清火控状态并返回零
    //（**不清 baseline** —— 底子是"当前目标几何"的测量，不是上一次开火的经验）。
    Output update(bool enabled, bool fire_active, bool obs_ok,
                  float error_y_px, float dt_ms, const RecoilClConfig& cfg) {
        Output out;
        if (!enabled || !fire_active || !obs_ok) {
            reset_fire();
            return out;
        }
        ++obs_frames_;
        if (obs_frames_ < cfg.start_frames) {
            add_y_ = 0.0f;   // 观察期：前几发不压（第一发子弹出膛时枪口还没抬，本就不该压）
            return out;
        }

        // ---- 观测量：减基线（第一版的教训，见文件头） ----
        float e = error_y_px;
        if (cfg.baseline_tau_ms > 0.0f) {
            if (!std::isfinite(baseline_)) {
                baseline_ = error_y_px;   // 刚换目标就开火：以本帧播种，本帧不压
                e = 0.0f;
            } else {
                e = error_y_px - baseline_;
            }
            if (e > 100.0f) e = 100.0f;   // 毛刺钳位（真信号 p90≈98px，+200px 级是检测跳变）
        }

        const float dt_s = dt_ms > 0.0f ? dt_ms / 1000.0f : 0.004f;
        const float press_cap = cfg.press_max_count > 0.0f
                                    ? cfg.press_max_count
                                    : std::numeric_limits<float>::infinity();

        // ---- 比例项（默认 0；P 是瞄准 PID 的职责，非 0 即与 PID 抢执行器）----
        float p_term = cfg.kp * e;
        if (p_term < 0.0f) p_term = 0.0f;

        // ---- 积分项（本模块存在的全部理由：Y 轴 PID 没有 I）----
        // 抗饱和两件套：
        //   ① 结构上限：积分项**单独**不得超过单帧上限（i_cap = press_cap）；
        //   ② 反算回写：限幅后把积分拉回"输出刚好等于上限"的位置，去掉多余卷绕。
        // ★★ V1.0.06（2026-09-29 21:xx）：**删掉"上一帧饱和就不再积"的粘滞开关**。
        //   板端配置恰好 integral_max × gain == press_max_count（12 × 0.25 = 3）⇒ 原判据
        //   用严格大于时回写永不执行（i_term 只会**等于** cap，实测 intg 只有 0 与 12 两个
        //   稳态）；改成"含等于"能把回写救活，但粘滞开关同时也被打开了 —— 积分一旦被
        //   回写到 cap/gain 就永久钉住，**连反向偏差也一起挡掉**（误差变号也降不下来），
        //   手感受体就是"压完不撒手"。Case12 的反向偏差段正是逮这个的。
        //   结论：只留反算回写（它本身就限住增长），不要粘滞开关。
        integral_ += e * dt_s;
        if (integral_ < 0.0f) integral_ = 0.0f;
        if (cfg.integral_max > 0.0f && integral_ > cfg.integral_max) integral_ = cfg.integral_max;

        float i_term = cfg.gain * integral_;
        if (i_term < 0.0f) i_term = 0.0f;

        float press = p_term + i_term;
        if (press < 0.0f) press = 0.0f;
        bool saturated = false;
        if (press >= press_cap) {
            press = press_cap;
            saturated = true;
            // ② 反算回写：把积分拉到"这个 P 项下输出刚好等于上限"的位置。
            // p_term 已经占掉多少预算，积分就只补剩下的（p_term 单独超上限 ⇒ 积分为 0）。
            if (cfg.gain > 0.0f) {
                const float cap_i = (press_cap - p_term) / cfg.gain;
                integral_ = cap_i > 0.0f ? cap_i : 0.0f;
                i_term = cfg.gain * integral_;
            }
        }

        // ---- 限速（阻尼）：单帧输出变化不超过 slew_count_per_frame ----
        // 无阻尼的闭环在 250Hz 上会把任何检测跳变变成整帧猛踢（v1 摆动的直接手感受体）。
        if (cfg.slew_count_per_frame > 0.0f) {
            const float lo = add_y_ - cfg.slew_count_per_frame;
            const float hi = add_y_ + cfg.slew_count_per_frame;
            if (press < lo) press = lo;
            if (press > hi) press = hi;
            if (press < 0.0f) press = 0.0f;
        }
        saturated_ = saturated;

        add_y_ = press;
        p_term_ = p_term;
        i_term_ = i_term;
        out.add_y = press;
        out.active = press > 0.0f;
        return out;
    }

    // 全量复位（换目标 / 换世代）：火控状态 + 基线一起清。
    void reset() {
        reset_fire();
        baseline_ = std::numeric_limits<float>::quiet_NaN();
    }

    // ---- 遥测 ----
    float integral_px_s() const { return integral_; }
    int obs_frames() const { return obs_frames_; }
    float add_y() const { return add_y_; }
    float p_term() const { return p_term_; }
    float i_term() const { return i_term_; }
    float baseline() const { return baseline_; }
    int state() const {
        if (obs_frames_ == 0) return kOff;
        return add_y_ > 0.0f ? kPressing : kObserving;
    }

private:
    // ---- V1.0.06 常量 ----
    // 基线学习的几何门槛（px，归一域）：超出即整帧不学。取值依据见 track_baseline 注释。
    static constexpr float kBaselineGatePx = 40.0f;

    void reset_fire() {
        integral_ = 0.0f;
        obs_frames_ = 0;
        add_y_ = 0.0f;
        p_term_ = 0.0f;
        i_term_ = 0.0f;
        saturated_ = false;
    }

    float integral_ = 0.0f;   // 累积偏移（px·s）
    int obs_frames_ = 0;      // 本次开火内的连续有效观测帧数
    float add_y_ = 0.0f;      // 上一帧下压量（count，已限幅）
    float p_term_ = 0.0f;     // 上一帧比例项（限幅前，count）
    float i_term_ = 0.0f;     // 上一帧积分项（限幅前，count）
    // 上一帧输出是否顶到单帧上限。**只作遥测**，不参与控制 —— V1.0.06 起抗饱和
    // 完全交给「反算回写」（粘滞开关会在饱和后把反向偏差也挡掉，见 update 注释）。
    bool saturated_ = false;
    float baseline_ = std::numeric_limits<float>::quiet_NaN();  // 偏差底子（px；NaN=未就绪）
};

}  // namespace ttbox::core::aim
