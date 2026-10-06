# ★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
#   本脚本只做「调 C++ / 生成输入 / 判阈值 / 出报告」，不含任何控制逻辑。
#   控制器与物理仿真全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
"""report_html.py — 自测看板 HTML 生成器（V1.0.46）

把 trace_replay 的回放结果渲染成**单文件自包含 HTML**：数据内嵌、双击即开、
不依赖网络/CDN（板端/内网环境常常没有外网）。

用法
----
    python report_html.py                     # 合成轨迹 → aim_report.html
    python report_html.py --trace x.csv       # 真实轨迹
    python report_html.py --out 看板.html
"""
from __future__ import annotations

import argparse
import html
import json
import os

import trace_replay as tr

# 结论阈值（与 trace_replay.replay 的判据同源，集中在这里便于调）
VERDICT_STABLE = "稳"
VERDICT_OSC = "振荡"


def _fmt(v, nd=2):
    if isinstance(v, float):
        return f"{v:.{nd}f}"
    return str(v)


def build(traces: list[tr.Trace]) -> dict:
    scenes = []
    for t in traces:
        p1 = tr.replay(t, tr.CtlCfg(kind="pid1"), "pid1")
        fi = tr.replay(t, tr.CtlCfg(kind="fitts"), "fitts")
        scenes.append({
            "name": t.name,
            "source": t.source,
            "stats": t.stats(),
            "rows": [p1.brief(), fi.brief()],
            "errs_pid1": p1.errs[:400],
            "errs_fitts": fi.errs[:400],
            "outs_pid1": p1.outs[:400],
            "outs_fitts": fi.outs[:400],
        })
    scan_tr = traces[-1]
    scans = [
        {"key": "fitts_ff_gain", "label": "速度前馈 ff_gain",
         "rows": tr.sweep(scan_tr, "fitts_ff_gain", [0.0, 0.3, 0.6, 0.85], tr.CtlCfg())},
        {"key": "fitts_deadzone_px", "label": "死区 deadzone_px",
         "rows": tr.sweep(scan_tr, "fitts_deadzone_px", [1.0, 2.0, 3.0, 5.0], tr.CtlCfg())},
        {"key": "fitts_a_ms", "label": "时间常数 A（快慢）",
         "rows": tr.sweep(scan_tr, "fitts_a_ms", [8.0, 12.0, 20.0, 30.0], tr.CtlCfg())},
        {"key": "fitts_b_ms", "label": "难度系数 B",
         "rows": tr.sweep(scan_tr, "fitts_b_ms", [10.0, 20.0, 30.0, 40.0], tr.CtlCfg())},
    ]
    return {
        "physics": {
            "gain": tr.GAIN_X_PX_PER_COUNT,
            "delay": tr.RESPONSE_DELAY_MS,
            "frame_ms": round(tr.FRAME_MS_144FPS, 3),
        },
        "scenes": scenes,
        "scans": scans,
    }


def render(data: dict) -> str:
    e = html.escape
    p = data["physics"]
    out: list[str] = []
    A = out.append

    A("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>")
    A("<meta name='viewport' content='width=device-width,initial-scale=1'>")
    A("<title>TTBOX 自测看板 · 轨迹闭环回放</title>")
    A("""<style>
:root{--bg:#0f1115;--card:#171a21;--line:#252a34;--fg:#e6e9ef;--dim:#98a1b3;
--ok:#3ddc97;--warn:#ffb020;--bad:#ff5c5c;--pid:#6aa9ff;--fit:#ff9f43}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.6 -apple-system,'Segoe UI','Microsoft YaHei',sans-serif}
.wrap{max-width:1180px;margin:0 auto;padding:24px 20px 60px}
h1{font-size:20px;margin:0 0 4px}
h2{font-size:16px;margin:28px 0 10px;padding-left:9px;border-left:3px solid var(--fit)}
.sub{color:var(--dim);font-size:12px;margin-bottom:6px}
.phys{display:flex;gap:8px;flex-wrap:wrap;margin:10px 0 4px}
.chip{background:var(--card);border:1px solid var(--line);border-radius:6px;
padding:3px 9px;font-size:12px;color:var(--dim)}
.chip b{color:var(--fg)}
table{width:100%;border-collapse:collapse;margin:8px 0 4px;font-size:13px}
th,td{padding:7px 9px;text-align:right;border-bottom:1px solid var(--line)}
th:first-child,td:first-child{text-align:left}
th{color:var(--dim);font-weight:600;font-size:12px}
tr:hover td{background:#1c2029}
.tag{display:inline-block;padding:1px 7px;border-radius:4px;font-size:11px}
.t-ok{background:rgba(61,220,151,.15);color:var(--ok)}
.t-warn{background:rgba(255,176,32,.15);color:var(--warn)}
.t-bad{background:rgba(255,92,92,.15);color:var(--bad)}
.t-pid{color:var(--pid);font-weight:600}
.t-fit{color:var(--fit);font-weight:600}
.delta{font-size:12px;color:var(--dim)}
.better{color:var(--ok)}.worse{color:var(--bad)}
canvas{display:block;width:100%;height:150px;background:var(--card);
border:1px solid var(--line);border-radius:8px;margin:6px 0 2px}
.legend{display:flex;gap:14px;font-size:12px;color:var(--dim);margin:2px 0 10px}
.lg{display:inline-flex;align-items:center;gap:5px}
.dot{width:9px;height:9px;border-radius:50%}
.note{background:var(--card);border:1px solid var(--line);border-left:3px solid var(--fit);
border-radius:6px;padding:10px 13px;margin:14px 0;font-size:13px;color:var(--dim)}
.warnbox{border-left-color:var(--bad)}
.warnbox b{color:var(--bad)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
@media(max-width:860px){.grid2{grid-template-columns:1fr}}
.hint{color:var(--dim);font-size:12px;margin-top:2px}
</style></head><body><div class='wrap'>""")

    A("<h1>TTBOX 自测看板 · 轨迹闭环回放</h1>")
    A(f"<div class='sub'>数据源：{e('trace_replay.py')} 闭环回放（板端实测物理量）· "
      f"准星位置由回放闭环驱动，误差 = 目标位置 − 准星位置</div>")
    A(f"<div class='phys'>"
      f"<span class='chip'>gain <b>{p['gain']}</b> px/count</span>"
      f"<span class='chip'>回路延迟 <b>{p['delay']}</b> ms</span>"
      f"<span class='chip'>帧间隔 <b>{p['frame_ms']}</b> ms（144fps）</span>"
      f"<span class='chip'>判定 <b>稳 / 振荡</b> 看末段幅度是否扩张</span>"
      f"</div>")

    A("<div class='note'><b>怎么读这张表：</b>"
      "「稳态/P95」越小越好（贴得准）；「过冲」越小越好（不冲过头）；"
      "「<b>翻转</b>」是输出方向反转次数 —— 这是「追着怪」的量化指标，"
      "几十次就是在抖；「卡死」是误差 &gt;15px 却没输出的帧数（跟不上）。"
      "pid1 是 V1.0.42 那套（BB927 实战值），fitts 是 V1.0.43 起的默认。</div>")

    # ── 各场景 ──
    for sc in data["scenes"]:
        st = sc["stats"]
        A(f"<h2>{e(sc['name'])} <span class='sub'>（{e(sc['source'])}，"
          f"{st.get('frames',0)} 帧 / 有目标 {st.get('target_frames',0)} 帧 / "
          f"均速 {st.get('mean_speed_px_s',0)} px·s⁻¹）</span></h2>")
        A("<table><tr><th>控制器</th><th>稳态中位</th><th>稳态P95</th>"
          "<th>过冲</th><th>翻转</th><th>卡死</th><th>输出帧</th><th>单帧最大</th><th>判定</th></tr>")
        base = None
        for r in sc["rows"]:
            if r["kind"] == "pid1":
                base = r
        for r in sc["rows"]:
            cls = "t-pid" if r["kind"] == "pid1" else "t-fit"
            osc = r["oscillate"]
            flip_col = "var(--bad)" if r["sign_flips"] > 30 else "var(--ok)"
            A(f"<tr><td class='{cls}'>{e(r['label'])}</td>"
              f"<td>{_fmt(r['settle_med'])} px</td>"
              f"<td>{_fmt(r['settle_p95'])} px</td>"
              f"<td>{_fmt(r['overshoot_px'])} px</td>"
              f"<td style='color:{flip_col}'>{r['sign_flips']}</td>"
              f"<td>{r['stuck_frames']}</td>"
              f"<td>{r['move_frames']}</td>"
              f"<td>{_fmt(r['max_step'])}</td>"
              f"<td><span class='tag {'t-ok' if not osc else 't-bad'}'>"
              f"{VERDICT_STABLE if not osc else VERDICT_OSC}</span></td></tr>")
        A("</table>")
        f1 = next(r for r in sc["rows"] if r["kind"] == "fitts")
        b1 = base
        if b1:
            dflip = b1["sign_flips"] - f1["sign_flips"]
            cls = "better" if dflip > 0 else "worse"
            A(f"<div class='delta'>fitts 相比 pid1：翻转 {b1['sign_flips']} → <b>{f1['sign_flips']}</b> "
              f"（<span class='{cls}'>{dflip:+d}</span>），"
              f"判定 <b>{'振荡→' if b1['oscillate'] else ''}{VERDICT_STABLE if not f1['oscillate'] else VERDICT_OSC}</b>。"
              f"</div>")

        # 曲线：误差 + 输出
        A("<div class='grid2'>")
        A("<div><canvas id='e_" + e(sc["name"].replace(" ", "_")) + "'></canvas>"
          "<div class='legend'>"
          "<span class='lg'><span class='dot' style='background:var(--pid)'></span>pid1 误差</span>"
          "<span class='lg'><span class='dot' style='background:var(--fit)'></span>fitts 误差</span>"
          "<span class='lg'>横轴=帧（144fps），纵轴=|误差| px</span></div></div>")
        A("<div><canvas id='o_" + e(sc["name"].replace(" ", "_")) + "'></canvas>"
          "<div class='legend'>"
          "<span class='lg'><span class='dot' style='background:var(--pid)'></span>pid1 输出</span>"
          "<span class='lg'><span class='dot' style='background:var(--fit)'></span>fitts 输出</span>"
          "<span class='lg'>纵轴=|count|，越高说明推得越猛</span></div></div>")
        A("</div>")

    # ── 参数扫描 ──
    A("<h2>参数扫描</h2>")
    A("<div class='note'>在同一段轨迹上逐个改一个参数。<b>稳态</b>与<b>翻转</b>要同时看："
      "只降稳态误差但翻转变多 ⇒ 变抖了，不是好事。</div>")
    for sc in data["scans"]:
        A(f"<h2 style='border-left-color:var(--dim)'>{e(sc['label'])}</h2>")
        A("<table><tr><th>取值</th><th>稳态中位</th><th>稳态P95</th><th>过冲</th>"
          "<th>翻转</th><th>卡死</th><th>判定</th></tr>")
        best = min(sc["rows"], key=lambda r: (r["sign_flips"] > 30, r["settle_med"]))
        for r in sc["rows"]:
            mark = " ★" if r is best else ""
            osc = r["oscillate"]
            A(f"<tr><td>{_fmt(r['value'],1)}{mark}</td>"
              f"<td>{_fmt(r['settle_med'])} px</td>"
              f"<td>{_fmt(r['settle_p95'])} px</td>"
              f"<td>{_fmt(r['overshoot_px'])} px</td>"
              f"<td>{r['sign_flips']}</td><td>{r['stuck_frames']}</td>"
              f"<td><span class='tag {'t-ok' if not osc else 't-bad'}'>"
              f"{VERDICT_STABLE if not osc else VERDICT_OSC}</span></td></tr>")
        A("</table>")
    A(f"<div class='hint'>★ = 该参数下的推荐值（先看翻转不抖，再看稳态误差最小）。"
      f"扫描用的是最后一个场景「{e(data['scenes'][-1]['name'])}」。</div>")

    # ── 数据 + 绘图 ──
    A("<script>const D=" + json.dumps(data, ensure_ascii=False).replace("</", "<\\/") + ";")
    A("""
function draw(id,series,colors,ymax){
 const cv=document.getElementById(id); if(!cv) return;
 const dpr=window.devicePixelRatio||1, w=cv.clientWidth, h=cv.clientHeight;
 cv.width=w*dpr; cv.height=h*dpr;
 const c=cv.getContext('2d'); c.setTransform(dpr,0,0,dpr,0,0);
 c.clearRect(0,0,w,h);
 const pad=6, iw=w-pad*2, ih=h-pad*2;
 const all=series.flat(); if(!all.length) return;
 const hi=Math.max(ymax||0, ...all.map(Math.abs))*1.05||1;
 // 网格
 c.strokeStyle='#252a34'; c.lineWidth=1;
 for(let i=0;i<=4;i++){const y=pad+ih*i/4;c.beginPath();c.moveTo(pad,y);c.lineTo(w-pad,y);c.stroke();}
 // 零线
 const y0=pad+ih/2; c.strokeStyle='#3a4252'; c.beginPath();c.moveTo(pad,y0);c.lineTo(w-pad,y0);c.stroke();
 series.forEach((s,si)=>{
   c.strokeStyle=colors[si]; c.lineWidth=1.2; c.beginPath();
   s.forEach((v,i)=>{const x=pad+iw*(i/(s.length-1||1)), y=y0-(v/hi)*ih/2; i?c.lineTo(x,y):c.moveTo(x,y);});
   c.stroke();
 });
}
const MAPS=[['e_','errs'],['o_','outs']];
for(const sc of D.scenes){
 const key=sc.name.replace(/ /g,'_');
 draw('e_'+key,[sc.errs_pid1,sc.errs_fitts],['#6aa9ff','#ff9f43']);
 draw('o_'+key,[sc.outs_pid1,sc.outs_fitts],['#6aa9ff','#ff9f43']);
}
window.addEventListener('resize',()=>{
 for(const sc of D.scenes){
  const key=sc.name.replace(/ /g,'_');
  draw('e_'+key,[sc.errs_pid1,sc.errs_fitts],['#6aa9ff','#ff9f43']);
  draw('o_'+key,[sc.outs_pid1,sc.outs_fitts],['#6aa9ff','#ff9f43']);
 }
});
</script>""")
    A("</div></body></html>")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", default="", help="板端 DetTrace CSV")
    ap.add_argument("--out", default="aim_report.html")
    args = ap.parse_args()

    if args.trace:
        traces = [tr.load_trace_csv(args.trace)]
    else:
        traces = [tr.make_synthetic(n, s) for n, s in
                  (("静止", 0.0), ("慢跑 50", 50.0), ("快跑 150", 150.0))]
    data = build(traces)
    html_text = render(data)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(html_text)
    print(f"看板已生成: {args.out}  ({os.path.getsize(args.out) / 1024:.0f} KB)")
    for sc in data["scenes"]:
        f1 = next(r for r in sc["rows"] if r["kind"] == "fitts")
        b1 = next(r for r in sc["rows"] if r["kind"] == "pid1")
        print(f"  {sc['name']:<12} 翻转 {b1['sign_flips']:>3d} → {f1['sign_flips']:>3d}  "
              f"稳态 {b1['settle_med']:.2f} → {f1['settle_med']:.2f}px  "
              f"{'振荡!' if f1['oscillate'] else '稳'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
