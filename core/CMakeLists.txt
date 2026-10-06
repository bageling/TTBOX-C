# ttbox/core — TTBox C++ Core 阶段 A-1 地基
#
# 要求：
#   - C++17
#   - ARM64 / RK3588 可编译（板端 g++ 12 / cmake 3.25 已验证）
#   - Debug / Release
#   - warnings（-Wall -Wextra -Wpedantic / MSVC /W4）
#   - 可测试（内置零依赖测试框架）
#   - 不依赖 Python

cmake_minimum_required(VERSION 3.16)
# ★ 版本真源 = core/include/ttbox/core/version.hpp::kCoreVersion（B-CONST-3）；
#   此处仅为 CMake 记录的同一事实镜像，二者由 scripts/ttbox_conventions_gate.sh 断言对应关系。
# ★ 2026-09-30 起产品版本为 "V1.0.16"（带字母前缀，原因见 version.hpp 注释）；
#   CMake 的 project VERSION 只解析数字 ⇒ 这里写**去掉字母前缀的数字镜像**，
#   门禁 ⑥ 的判据 = 「kCoreVersion 去掉前导字母 == 本行」。出货包名/版本串取自 version.hpp。
#   V1.0.11 相对 V1.0.10：选靶照 yu 加量测跳变门控（拒绝坏量测，沿用上一帧）＋ continuity 免重置＋ 切靶确认窗＋ 开火期禁切靶。
#   V1.0.12 相对 V1.0.11：按业主口径「系统太臃肿、参考物太多导致功能紊乱」做**减法** ——
#                      彻底拆掉「按倍镜区分」：AimHotkeyProfile::{zoom_scale, gain_px_per_count}、
#                      TargetSelectorConfig::zoom_scale、PID 误差的 /zoom 归一、面板两控件全删；
#                      旧配置出现这些键一律**静默忽略**（等价 1.0/0，与删除前行为一致）。
#   V1.0.13 相对 V1.0.12：业主口径「功能太复杂、太多参考物干扰自瞄，落点与设置瞄点不一致」
#                       ⇒ 把「能移落点 / 能暗改增益」的并联入口全部删掉：
#                       · AimPointProfile::aim_offset_x/y（准星像素偏移）+ 面板两控件
#                       · CaptureProfile::offset_x/y（裁剪区偏移）+ 面板两控件
#                       · smooth_x/y（把 kp/kd 暗削 99% 的「削弱强度」）—— 折算进 kp/kd；
#                       · kp/kd 语义改为**真实生效值**（旧 25 实际只等于 0.25）、predict 默认 3.0→1.0。
#                       净效果：落点只剩「瞄点」一个入口；面板上「拉力/刹车」就是真实力气值。
#   V1.0.14 相对 V1.0.13：纯死代码清理（运行时行为 / 配置向量不变）：
#                       · 删「稳定边界层」空壳 ICoordinate/IMouse/CoreInterfaces/CoreInterface（零实现零引用）
#                       · 删 controller/PidController + IController（生产用 aim/Pid1Controller，此封装零引用）
#                       · 删 HumanizeConfig 死结构体
#                       测试改用 tests/test_pid_controller.hpp（生产同款 Pid1Controller + 真实参数）。
#   V1.0.15 相对 V1.0.14：业主令「去掉所有 BB/BK 的东西」—— 删 BB 对标第二批 8 模块：
#                       · 删 LeadPredictor/HumanizeShaper/SpeedFluctuation/AccuracySim/
#                         AntiOvershoot/SpeedAdaptiveKp/GlobalWave/BezierTrajectory（默认全关、零行为）
#                       · MouseTypes.hpp 删对应 8 配置结构体 + bezier enum + 7 成员
#                       · AimThread 删 21 处接线（continuous_lead 互斥简化为无条件）
#                       · RuntimeProfile 删序列化段；CMake 删 2 个测试目标
#                       · web 层删 BB 面板 8 段 + 拟人化分区（面板 5 分区 → 4 分区）
#                       保留：trigger2 自动扳机 / 切靶防抖 / 选靶四项（生产功能）。
#   V1.0.16 相对 V1.0.15：标定/落点三项改进（业主指令）——
#                       · 落点可改瞄「头部小框正中心」：mouse.aim_point.aim_at_head_box（默认关），
#                         resolve_head_box 纯几何配对（面积≤ref×0.5、中心落 ref 上半部、取最小），不靠类别号；
#                       · 标定参数面板热更新（web 层，core 无感）；
#                       · 标定摆动幅度 ±32→±96px + 闭环收敛（web 层，core 无感）。
#   V1.0.43 相对 V1.0.42（2026-10-06）：**新增 Fitts 定律控制器（治 pid1「追着怪/停不住」）**
#
#   ① ★核心：业主实测 V1.0.42（pid1+predict=0.5）仍「追着怪、停不住、瞄准有问题」。
#     定位为 pid1 **结构缺陷**（前馈主导对误差变化率敏感 → 追着怪；P/D 被 smooth
#     压 100 倍 → 停不住；死区绝对像素 → 瞄不准），非参数能修。新增 Fitts 定律控制器
#     （dat58/aimbot 思路）：MT = A + B×log2(2|err|/W+1)，近慢远快、只追误差不追变化率。
#     参数 A=20/B=20/dz=3px/ff_gain=0.6 为板端仿真定案。
#     **保留 pid1 可切换**（mouse.controller_type：fitts 默认 / pid1 备选）——
#     Fitts 上板不行改配置切回，不用再发版。
#   ② 速度前馈补固有滞后：Fitts 只对误差比例响应 ⇒ 移动目标滞后 ≈ 速度×MT
#     （err=100px 时 MT=108ms，叠加 51ms 回路延迟总滞后 ~110ms = 「追着慢」）。
#     ff = min(ff_gain,0.85) × vel × MT，喂 AimTracker 的 vx/vy（One-Euro 滤波后）。
#   ③ 三个安全护栏（算法核对时发现并修复）：
#     · **dt 实测传入**：Fitts 是时间模型，写死 dt ⇒ 帧率抖动时输出 ±50% 失配。
#     · **非有限输入拦截**：abs(NaN)<dz 恒假 ⇒ NaN 穿死区 ⇒ log2(NaN) 一路到 HID 输出
#       （int16 转 NaN 是 UB）。入口三参数 isfinite + 出口兜底。
#     · **前馈符号护栏**：反向前馈只钳幅度不翻符号（否则「减速」变「加速」）。
#   ④ 仿真验证（gain=0.65 + 51ms + 噪声±2px）：不规则目标翻转 10→4（降 60%）、
#     移动 50px/s 翻转 2→0、静态/急停全 0（停得住）。单测 10/10、ctest 43/43。
#
#   V1.0.42 相对 V1.0.40（2026-10-05）：**回滚 V1.0.41（SmoothAimController），
#     恢复 pid1（shuwu PID）+ predict=0.5（BB927 实战值）**
#
#   ① ★核心：V1.0.41 的 SmoothAimController（EMA+比例+限幅+框高死区）实测「太拉跨」，
#     整体回滚（git revert d85b41f）⇒ 恢复 Pid1Controller（= BB927 实战在用的 shuwu PID，
#     BB927_SOURCE/algo/pid_shuwu.lua 与其逐行一致）。
#   ② ★predict_x 定 0.5：考古发现 pid1.cpp main() 的 3.0 是**演示值**，BB927（shuwu PID
#     原发行方）实战配置 0.5（pid_shuwu.lua 注释「predict X=3 是过期信息，以 cfg 为准 0.5」）。
#     V1.0.38 误抄演示值 3.0 ⇒ 乱飞；V1.0.40 折中 1.0 ⇒ 移动抖。
#     本地仿真（gain=0.65+51ms+噪声±2px）：p0.5 静态幅0.0/翻0、移动50px/s 落后2.9px稳、
#     移动120px/s 落后6.0px稳 —— 三档里唯一全稳。
#   ③ 速度预测（AimTracker.predict / prediction_time_s）保持默认关（仿真实测全补偿会
#     放大框噪声 ⇒ 高频抖；BB927 的同类 lead.lua 默认也关）。
#   ④ 附带：usbproxy reset_device() 修（proxy.cpp:1518 加 reset_device_before_proxy 条件，
#     被控电脑 reset 时不再把物理星闪接收器打死 ⇒ 透传失效要拔插的问题）。
#
#   V1.0.40 相对 V1.0.39（2026-10-05）：**predict 前馈默认 3.0→1.0（修复 gain=0.65 下自激乱飞）
#     + predict_y 出厂 bug 修正（3→0）+ 转换依赖前置检查**
#
#   ① ★核心：predict_x 默认值 3.0 → 1.0（MouseTypes.hpp / RuntimeProfile.cpp / 出厂配置 / 面板 / 测试）
#      · 根因：pid1 原文 predict_x=3.0 是「gain=1（1count=1px）」假设下调的；板端实测 gain≈0.65
#        + 回路延迟 51ms，3.0 的前馈在该闭环下自激振荡（业主上板实测「乱飞」；仿真移动目标
#        50px/s：p3.0 稳态误差 29.7px/翻转 16 次 vs p1.0 6.0px/稳定）。
#      · 降到 1.0 是 V1.0.13 有 A/B 实测「16 轮全稳」的历史稳定值。
#      · Y 轴 pid1 原文就是 0（不带前馈），保持不变；出厂 10-device.json 的 predict_y 错写成 3，一并修 0。
#   ② 转换依赖前置检查：ONNX→RKNN 转换入口缺依赖时 fail-fast 诚实报错（板端三依赖全缺不再吐 errno 2）。
#
#   V1.0.39 相对 V1.0.38（2026-10-05）：**速度观测器接入 gain + 模型库磁盘泄漏修复 + 标定温和档口径回归**
#
#   ① ★核心（pid1 唯一改动，实质 2 行公式）：速度观测器的单位换算
#      · Pid1Controller：新增 set_gain() + gain_（**缺省 1.0 = pid1 原文**，
#        故不调用时 update() 与 pid1.cpp 逐字节一致，test_pid1 对拍仍钉得住）
#      · 主分支 `tv = error_diff + last_u * gain_`、近点减半分支 `last_u * gain_ * 0.5`
#        （后者最易漏：不改则该分支量纲差 1/g 倍，误差一进近点就掉一个数量级）
#      · AimThread：配置刷新块内 set_gain(gain_x/y_px_per_count)（复用标定已有字段，
#        不新增配置项；标定写回 gain 后热更新即生效）
#      · 动机（带 51ms 延迟 + gain 0.65 的闭环仿真实测）：pid1 原文 `+ last_u`
#        隐含假设 g=1（1count=1px），板端实测 g≈0.65 ⇒ 目标速度被系统性低估 35%
#        ⇒ 前馈力度恒不足 ⇒ 只能靠调低 predict 补偿（V1.0.13 的 3.0→1.0 即为此）
#        ⇒ 接入后 predict 恢复作者原意量级，换游戏灵敏度不必再动 predict
#      · 护栏：test_pid1 新增 4 条（缺省一致 / set_gain 生效 / 非法值忽略 / 近点分支已换算），
#        并做反向验证（改回`+ last_u` ⇒ 立刻变红）
#   ② 模型库泄漏修复（plugins/web/api/models.py）
#      · _incoming 上传落点：unlink 从「仅 r1 失败分支」移入 finally（无条件删除）。
#        原实现下校验失败/安装失败/**成功导入**三条路径都永久留 4~11MB 的 .rknn
#        （板端实测残留 5 文件 = 23.4 MB）。面板免密无鉴权（ttbox-web.py「D1 免密全拆」）
#        ⇒ 修复前属**未鉴权远程可触发磁盘耗尽**。
#      · 补 _incoming 总量配额 512 MB（超限 507 拒收，fail-closed）——原目录无配额
#        也无启动清理，"上传即删"只防新泄漏。
#   ③ 标定温和档口径回归（plugins/web/lib/calibration.py）
#      · CALIB_PID_KP_MAX 0.10 → **10.0**（随 PID 翻回名义域）。
#        不修则温和档写 0.10 名义 ⇒ 经 soft-limit 实际只发挥 0.001（比现役 kp=25
#        弱 250 倍）⇒ bias 阶跃推不动目标 ⇒ gain 测不准 ⇒「标定成功但参数是错的」。
#      · 新增 test_web_calibration_pid_domain.py（7 条）+ 修 test_web_calibration_gain_source
#        的区间断言（改在生效域做）。★ 顺带修掉一个测试盲区：部分 calibration 测试是
#        脚本式自测（顶层 sys.exit），pytest 收集时 INTERNALERROR 打断整个 session，
#        **全量 pytest 永远跑不到它们**；改成正常 pytest 用例后才第一次真正跑到
#        test_calib_gentle_pid_constants_are_sane，它立刻抓出了上面的口径错配。
#   ④ ModelRegistry::install() **不动**（净改动仅注释）：曾尝试 install 成功后清
#      staging，但 install 刻意支持**幂等重放**（test_model_registry.cpp:126
#      「同一 staging 内容的安装请求允许幂等重放，例如首次响应在网络中丢失」），
#      清掉会让重试失败。⇒ 板端 staging 残留（实测 2 套 = 15.1 MB）需**独立的
#      「陈旧 staging 回收」策略**，属独立需求，本版未做（磁盘已扩容 29G 不紧急）。
#
#   本地四道口径：门禁 PASS / ctest 42/42 / pytest 735 / check_links 0
#   V1.0.38 相对 V1.0.37（2026-10-05）：**PID 回归 pid1 原文完全移植**
#                       （业主令「pid 以 pid1 为准完全移植，不许有自己的变动」）。
#   · Pid1Controller.hpp **零改动**（git diff = 0 行），一切口径恢复都在配置层完成。
#   · MouseTypes：kp/kd 0.25→**25**（未折算原值）、predict_x 1.0→**3.0**（撤回 V1.0.13
#     的自激降级）、**恢复 smooth_x/y = 9900**（V1.0.13 曾删掉并折算进 kp）。
#   · RuntimeProfile：**删掉整段 smooth→kp 折算**（留着会双重缩放）、
#     **to_json 补回 smooth 序列化**（不补则面板调整不落盘）、**恢复 smooth∈[0,9999] 校验**。
#   · AimThread：configure() 第 5 参从**硬编码 0.0** 改回 smooth_x/y
#     ⇒ **pid1 的 soft-limit 复活**（V1.0.13~V1.0.37 期间它一直是关的；
#       实测大误差 200px 时 soft-limit 压掉 99% 输出）。
#   · ttbox_motion/calibration：derive_pid_params **返回前折回名义值**（÷SMOOTH_FACTOR）。
#     内部物理推导（7% 单帧误差 / 50ms→3.0×kp 阻尼比）与实测锚点全部不变。
#   · core/tests/test_pid_controller.hpp：测试桩补回 smooth（此前 configure 硬传 0，
#     导致所有 PID 行为测试都跑在「soft-limit 关闭」的错误口径上）。
#   · 迁移策略：**不写迁移，全员重跑标定**（业主 2026-10-05 定）。旧配置里
#     kp=0.25/predict=1.0 会覆盖新默认值，板端升级后必须重标。
#   · 护栏：test_mouse 3 条断言方向反转（钉 smooth 必须存在）+ 新增默认值断言；
#     test_pid1 逐点对拍仍 PASS（另跑 4 参数组 × 3 序列独立对账 0 处不一致）。
#   V1.0.37 相对 V1.0.36（2026-10-05）：**预览「瞄准范围」改常驻蓝圆、实时联动滑块**。
#                       · 起因：预览里有两个圈 —— 前端 HTML 白框（实时随滑块变）+ core 视频流蓝圆
#                         （要等保存才变），两者不同步、重叠，业主反馈「蓝圆不变、白框变」。
#                       · 改（蓝圆与选靶同源、实时）：前端 .aim-range-overlay 白框 → 常驻蓝圆，
#                         直径 = 截取尺寸 × 倍率（与 core 选靶半径 search_radius_px × fov_range 同源），
#                         拖动滑块/改截图尺寸实时变；core 删掉 draw_guides 里的 FOV 蓝圆（避免两圆重叠）。
#                       · 删（core）：PreviewModule::draw_guides 的 cv::circle 蓝圆、AimGuides::fov_radius /
#                         fov_circle_crop 字段、CoreRuntime 的 g.fov_radius 接线、AimThread::fov_radius_px 转发。
#                       · 护栏：test_preview_and_targeting_v31.py 改「core 不再带 FOV 半径」；
#                         test_web_fov_radius_semantics.py 的直径公式 / 越界带不变。
#   V1.0.36 相对 V1.0.35（2026-10-05）：**删除「瞄头部小框」功能**。
#                       · 业主令「删除 PID 页面里那个瞄头部小框功能」：移除
#                         mouse.aim_point.aim_at_head_box 开关 + resolve_head_box() 几何配对识头
#                         （大框(身体)+小框(头)时落点取小框正中心）整条链路。
#                       · 删（core）：AimThread.cpp 落点分支、AimPointProfile.hpp/.cpp 的
#                         resolve_head_box、MouseTypes.hpp 字段、RuntimeProfile.cpp 序列化往返、
#                         test_aim_head_box.cpp + CMake 测试目标。
#                       · 删（web）：profile_translate.py 两处、index.html 开关、00-const.js 默认值、
#                         10-flow.js 三处接线、test_web_aim_head_box.py。
#                       · 保留：head_aim（第3项头部瞄准约束，独立功能，未删）。
#   V1.0.35 相对 V1.0.34（2026-10-05）：**预览框角标显示模型置信度，不再显示类别号**。
#                       · 业主反馈「框上方应该是模型的置信度，不应该是类别」：V1.0.25 起
#                         角标画的是 `cls %d`（类别号），但板端现役是 7 类模型、class_names 为空
#                         ⇒ 类别号没有语义。置信度才是模型「这是目标」的直接输出（0~1）。
#                       · 修（4 处）：AimThread::Status 加 target_score 字段 + AimThread.cpp
#                         写 status_.target_score = selected.box.score；CoreRuntime.cpp 的
#                         aim_box_provider 回填 out->score = st.target_score（不再硬编码 0）；
#                         PreviewModule.cpp 角标 snprintf 改为 "%.0f%%"（box.score×100）。
#                       · 护栏：test_preview_and_targeting_v31.py 新增
#                         test_aim_box_label_is_confidence_not_class（源码级）。
#   V1.0.34 相对 V1.0.33（2026-10-05）：**修 FOV 预览圈与主页「瞄准范围」滑块不同步（差 2 倍）**。
#                       · 根因：V1.0.31 起 core 已把 fov_range = fov.radius × 2 改成
#                         fov.radius × fov_scale（AimThread.cpp，删掉 ×2），但 web 翻译层
#                         仍按旧口径写 fov.radius = range_factor / 2、回填倍率 = radius × 2，
#                         没跟着删 ⇒ 真实选靶半径 = search_radius_px × range_factor / 2
#                         （板端 1.0 时只有 160px），前端预览圈按 cropSize × range_factor
#                         画直径（= 320px 半径）⇒ 预览圈比真实圈大 2 倍、看起来「不同步」。
#                       · 修（纯 web 层，core 零改动）：fov.radius = range_factor（去掉 /2）、
#                         回填倍率 = radius（去掉 ×2）⇒ 选靶半径 = search_radius_px ×
#                         range_factor × fov_scale（1.0 ⇒ 内接圆 320px），与预览圈逐像素一致，
#                         符合业主 2026-09-24 钉死的「1.00 = 内接圆」口径。
#                       · 护栏：test_web_fov_radius_semantics.py 整套断言翻转（radius=k/2 →
#                         radius=k、越界带 >0.5 → >1.0、roundtrip 下限 0.05 → 0.1）。
#   V1.0.33 相对 V1.0.31（2026-10-05）：**回退 V1.0.32 的 FOV 基准改动**。
#                       · V1.0.32 曾把 FOV 半径基准从「截图半宽 search_radius_px」改成「全帧短边
#                         min(roi_w,roi_h)」想对齐 DecodeNMS，结果圈放大 4.5 倍（板端 fov.radius=0.195
#                         时 62px→281px，几乎填满 640 截图）、选靶范围同步放宽 4.5 倍 ⇒ 业主反馈
#                         「圈调不动/主页本来就有瞄准范围滑块，不该定死」⇒ 整体 revert。
#                       · 恢复：base_radius = search_radius_px（截图半宽，随截图缩放、由面板
#                         「瞄准范围」滑块经 fov_range 正常控制）；search_radius_px 未填时回退
#                         min(roi_w,roi_h)/2。与 DecodeNMS 的「整帧短边」口径差异是既有的、另行处理。
#                       · 版本号跳过 V1.0.32（已发布，本版等价 V1.0.31 + 更高版本号）。
#   V1.0.31 相对 V1.0.30（2026-10-04）：**预览按业界标准三层 + 选靶不再选中球 + FOV 真约束**。
#                       · 依据：GitHub 调研（sunone_aimbot，1.6k star，debug/overlay 窗口）。
#                         它的画法是三层：① show_boxes **全部检测框** ② 选中的框
#                         ③ show_target_line 中心→目标连线 + FOV 圆；框是**模型原框**，不改几何。
#                       · 起因：业主 20:12 看到"框罩在头盔、大小不随远近变"⇒ 板端实测选中的那个
#                         框是 35×31、h/w=0.88 —— 按 sunone 类别表（6=hideout_target_balls）
#                         那是**训练场的球**；板端 class_filter=[0..6] 全选 ⇒ 挑中了它。
#                       · ① 预览三层：细=全部检测框、粗=选中框、红线=中心→落点、蓝圈=FOV。
#                         圆半径**直接取选靶本帧实际用的值**（经 AimThread::fov_radius_px 转发），
#                         避免"画出来的圆 ≠ 真正约束的圆"这种调试期骗人。
#                       · ② 选靶几何兜底：**相对**判据 —— 同位置群内"明显更竖长(≥1.5×)"的才算人，
#                         圆/方块物后排且排在 priority 之前。★ 不用绝对阈值：绝对阈值在远处/
#                         小目标上不成立，本机回归当场打回 3 个既有测试（test_target_selector/
#                         test_pipeline/test_selector_clip_guard 集体变红）⇒ 相对判据零扰动。
#                       · ③ FOV 曾形同虚设：fov.radius 被多乘 2（把半径当直径算）⇒ 板端 0.5
#                         算成 1.0 ⇒ 半径 320 = 640 画面半宽 ⇒ 圆等于全屏 ⇒ 零约束。
#                         现在 fov.radius 就是"相对画面半宽的比例"（0.5 ⇒ 半径 160px）。
#                       · ④ **裁框整体退役**（业主 17:56 定调「框完整，只偏移落点」，即业界
#                         body_y_offset 的做法：框画完整，落点在框内由 offset_y 定位，0.24≈胸口）。
#                         V1.0.24~30 的上半身裁剪（垂直砍半 + 收窄到肩宽）全部删除。
#                       · 护栏：core/tests/test_preview_and_targeting_v31.py（8 条源码级，
#                         **反向验证 6/6 全红**）；5 条选靶用例并入 test_target_selector.cpp
#                         （新建独立 target 时链接失败，collect2 不给详情，故并入既有文件）。

#   V1.0.30 相对 V1.0.28（2026-10-04）：补上漏网的**字符串常量型**锚点（升级仍失败）。
#                       · V1.0.28 修了 4 个对象型锚点，但漏了 `kAppVersion` ——
#                         它在 lib/settings.py 是**字符串**（'2026.08.03.1'），
#                         入口 import 进来后仍是 str ⇒ `hub.call` 调用它 ⇒
#                         TypeError: 'str' object is not callable ⇒ web 启动即崩
#                         ⇒ 健康检查失败 ⇒ 自动回切 ⇒ 依然升不上去。
#                       · 同时修**审计器自己的错判**：imported 锚点原先"扫 lib 找第一个 def"，
#                         而那通常是**转发函数**不是真实定义处 ⇒ 把 str 误判成 function
#                         ⇒ 判据放行了 hub.call。现在按**入口 import 语句**追到确切模块。
#                       · 审计器扫源码时同时剥 `#` 注释与**三引号 docstring**
#                         （讲事故的 docstring 里写了 hub.call(...) 示例 ⇒ 连续两轮假阳性）。
#                       · 现状：全仓「对象型 + hub.call」**违规 0 处**。
#   V1.0.28 相对 V1.0.27（2026-10-04）：**修 hub 转发语义用错 ⇒ 升级死锁**（业主报"升不上去"）。
#                       · 事故：web 换写法(S9/S10)后，入口的 `_CLOUD_CLIENT`/`_CLOUD_SESSION`/
#                         `_ACTIVATION_CACHE`/`_HEARTBEAT_START_LOCK` 这些**对象型**锚点被
#                         `hub.call`（**调用**语义）取 ⇒ TypeError ⇒ web 启动即崩
#                         （'CloudLicenseClient' object is not callable，Main process exited）
#                         ⇒ **面板「检查更新」必然失败**；手动装新版后 web 仍起不来
#                         ⇒ 健康检查失败 ⇒ 安装脚本自动回切 ⇒ **永远升不上（死锁）**。
#                       · 修：对象型锚点一律改 `hub.get`（取值语义）。全仓扫过：违规 2 处
#                         （cloud_hooks 的 _CLOUD_CLIENT、api/aim.py 的 `_aim_trace`），
#                         另有 _CLOUD_SESSION/_ACTIVATION_CACHE/_HEARTBEAT_START_LOCK 同类，
#                         一并改（共 5 个锚点）。
#                       · 审计器 + 护栏：plugins/web/tests/test_web_hub_anchor_audit.py
#                         （列出每个锚点在入口的真实类型）+ test_web_hub_object_anchors.py
#                         （禁止对象型锚点用 hub.call）。**"分不清函数还是对象"是今天
#                         连踩 4 次的根因，现在由脚本判定而不是靠人猜。**
#                       · 连带含 V1.0.26 的「不要胳膊」框算法与 V1.0.27 的 usb-proxy 竞态修复。
#   V1.0.27 相对 V1.0.26（2026-10-04）：**修升级后物理鼠标输入链永久断开**（业主报"出错了"）。
#                       · 事故：core 与 ttbox-usbproxy **同秒启动**（unit 之间无依赖），
#                         core 去 connect usb-proxy 的 event.sock 时它还没建 ⇒ ENOENT ⇒
#                         上层按「不阻塞 AI 流水线」放弃 PhysicalMouseReader
#                         ⇒ **鼠标输入永久断且不自动重连**（手工 restart core 即恢复）。
#                       · 修 1（主）：`PhysicalMouseReader::start_event_socket` 遇失败**重试**
#                         12 次 × 300ms ≈ 最长等 3.6s，覆盖 usb-proxy 的设备枚举时间。
#                       · 修 2（保险）：ttbox-core.service 加 `After=/Wants=ttbox-usbproxy.service`。
#                       · 护栏：core/tests/test_usbproxy_socket_race.py（3 条源码级 ——
#                         该文件走 `#if defined(_WIN32)` 分流，host 编译的是 Windows 分支，
#                         本机 ctest 对它零覆盖；真正验证靠交叉编译 + 板端实跑）。
#                       · 连带含 V1.0.26 的「不要胳膊」框算法。
#   V1.0.27 相对 V1.0.26（2026-10-04）：**修升级后物理鼠标输入链永久断开**（业主报"出错了"）。
#                       · 事故：core 与 ttbox-usbproxy **同秒启动**（unit 之间无依赖），
#                         core 去 connect usb-proxy 的 event.sock 时它还没建 ⇒ ENOENT ⇒
#                         上层按「不阻塞 AI 流水线」放弃 PhysicalMouseReader
#                         ⇒ **鼠标输入永久断且不自动重连**（手工 restart core 即恢复）。
#                       · 修 1（主）：`PhysicalMouseReader::start_event_socket` 遇失败**重试**
#                         12 次 × 300ms ≈ 最长等 3.6s，覆盖 usb-proxy 的设备枚举时间。
#                       · 修 2（保险）：ttbox-core.service 加 `After=/Wants=ttbox-usbproxy.service`。
#                       · 护栏：core/tests/test_usbproxy_socket_race.py（3 条源码级 ——
#                         该文件走 `#if defined(_WIN32)` 分流，host 编译的是 Windows 分支，
#                         本机 ctest 对它零覆盖；真正验证靠交叉编译 + 板端实跑）。
#                       · 连带含 V1.0.26 的「不要胳膊」框算法。
#   V1.0.26 相对 V1.0.25（2026-10-04）：**补上"不要胳膊"—— 框水平收窄到肩宽**。
#                       · 业主完整定义（17:56）：「上半身 = 从胯到头 + 不要胳膊」。
#                         垂直砍到身高 50%（Y 轴偏移 0.5）V1.0.24 已做；
#                         **水平收窄是这版补的** —— V1.0.24/25 只缩高度、宽度原样，
#                         框里一直含着两侧胳膊（业主明确纠正："扯淡，我说的上半身不是这样"）。
#                       · 收窄到肩宽 = 0.23 × 身高（人体测量：肩峰宽/身高 ≈ 0.22~0.24），
#                         **只收窄不放宽**，**以中线对称收**。
#                       · ★ 关键性质：对称收窄**不动中线** ⇒ 落点横向完全不受影响
#                         （落点取框内比例点）⇒ 落点公式不用改，只有框变窄变干净。
#                       · 近身（框底被画面切）时 h 残缺 ⇒ 肩宽算得偏小 ⇒ 框比理想更窄，
#                         属**保守方向**且不影响落点，故不引入反推身高的复杂度。
#                       · 护栏：test_upper_body_wiring.py 加 test_shrink_narrows_to_
#                         shoulder_width_symmetrically（系数域/只收窄/对称/用原始 h）。
#   V1.0.26 相对 V1.0.25（2026-10-04）：**补上"不要胳膊"—— 框水平收窄到肩宽**。
#                       · 业主完整定义（17:56）：「上半身 = 从胯到头 + 不要胳膊」。
#                         垂直砍到身高 50%（Y 轴偏移 0.5）V1.0.24 已做；
#                         **水平收窄是这版补的** —— V1.0.24/25 只缩高度、宽度原样，
#                         框里一直含着两侧胳膊（业主明确纠正："扯淡，我说的上半身不是这样"）。
#                       · 收窄到肩宽 = 0.23 × 身高（人体测量：肩峰宽/身高 ≈ 0.22~0.24），
#                         **只收窄不放宽**，**以中线对称收**。
#                       · ★ 关键性质：对称收窄**不动中线** ⇒ 落点横向完全不受影响
#                         （落点取框内比例点）⇒ 落点公式不用改，只有框变窄变干净。
#                       · 近身（框底被画面切）时 h 残缺 ⇒ 肩宽算得偏小 ⇒ 框比理想更窄，
#                         属**保守方向**且不影响落点，故不引入反推身高的复杂度。
#                       · 护栏：test_upper_body_wiring.py 加 test_shrink_narrows_to_
#                         shoulder_width_symmetrically（系数域/只收窄/对称/用原始 h）。
#   V1.0.25 相对 V1.0.24（2026-10-04）：**预览 = 实际瞄准的框**（业主定口径：
#                       「预览里本来应该要画的就是实际瞄准的框」）。
#                       · 旧：预览喂 `status().detection_boxes` = 本帧**全部**检测框，
#                         按 class 上色（红=0 / 绿=1 加粗 / 黄=其余），两个框叠在一起
#                         看着像"又大又含头" —— 那是候选框集合，预览在骗人；
#                         且「class 1 = 头」是出厂 2 类模型(EP)的老约定，现役 7 类模型
#                         class_names 为空 ⇒ 那个映射是瞎猜。
#                       · 新：PreviewModule 只画**一个**框（`set_aim_box_provider`），
#                         数据源 = `target_*`（控制链这一帧真在用的框）；固定醒目亮绿、
#                         不再按 class 分派颜色、角标只留类别号（Status 无该目标置信度）。
#                       · 删除：多框平滑 `smooth_boxes`（框已 One-Euro，再平滑只会更滞后）
#                         与"丢失后保留 3 帧旧框"（那是画出已经不瞄的框）。
#                       · 护栏：`core/tests/test_preview_aim_box.py`（7 条源码级 ——
#                         PreviewModule 依赖 opencv，**host 不编译**，本机 ctest 零覆盖）。
#   V1.0.25 相对 V1.0.24（2026-10-04）：**预览 = 实际瞄准的框**（业主定口径：
#                       「预览里本来应该要画的就是实际瞄准的框」）。
#                       · 旧：预览喂 `status().detection_boxes` = 本帧**全部**检测框，
#                         按 class 上色（红=0 / 绿=1 加粗 / 黄=其余），两个框叠在一起
#                         看着像"又大又含头" —— 那是候选框集合，预览在骗人；
#                         且「class 1 = 头」是出厂 2 类模型(EP)的老约定，现役 7 类模型
#                         class_names 为空 ⇒ 那个映射是瞎猜。
#                       · 新：PreviewModule 只画**一个**框（`set_aim_box_provider`），
#                         数据源 = `target_*`（控制链这一帧真在用的框）；固定醒目亮绿、
#                         不再按 class 分派颜色、角标只留类别号（Status 无该目标置信度）。
#                       · 删除：多框平滑 `smooth_boxes`（框已 One-Euro，再平滑只会更滞后）
#                         与"丢失后保留 3 帧旧框"（那是画出已经不瞄的框）。
#                       · 护栏：`core/tests/test_preview_aim_box.py`（7 条源码级 ——
#                         PreviewModule 依赖 opencv，**host 不编译**，本机 ctest 零覆盖）。
#   V1.0.24 相对 V1.0.23（2026-10-04）：**去掉 V1.0.23 的开关/配置项**，
#   改成无条件生效的算法校正（业主口径：「不要开关，要校正算法/数学/几何」）。
#                       · core：控制链（贴边判定/冻结/身高自校准/落点/框高/拟人化
#                         目标半径）继续用上半身虚拟框 (y1, y1+k·h)，比例改为
#                         **算法常量 kUpperBodyRatio=0.5**（人体几何依据：头 0.13 +
#                         颈肩到髋 0.40），不进配置、不进面板；
#                         **落点等效换算**（相对框高的比例量 ÷k）保持落点物理位置不变；
#                         ★ **显示框不再做多框并集** —— 直接画控制链在用的那个框。
#                         旧并集把身体框与头框拼起来，正是「框是全身加头部」的来源。
#                       · 删除：core 两个配置键、web 两键透传、面板开关卡片、web 测试。
#   V1.0.22 相对 V1.0.21（2026-10-04）：启停"首次点击卡顿 + 动画生硬"两半都修。
#                       · core：800ms 格式稳定等待补第二条跳过判据 ——
#                         除"热重开（距上次 close < 5s）"外，新增"盒子已启动 >= 60s"。
#                         依据 = uptime 4.6 天的盒子上连续 40 次**全新 open**，
#                         读到的格式全部 2560/1440 BGR3，一次都没变 ⇒ 早已稳定。
#                         兜底分支（开机 < 60s / 读不到 /proc/uptime）**逐字节不变**，
#                         仍睡满 800ms。实测冷路径 start 1.07s → 见发布记录。
#                       · web 面板：.power-button 补 transition（原先一条都没有 ⇒
#                         悬停位移/配色切换/忙态全是硬跳）；忙态改为"按压 scale +
#                         外环加速 0.9s + 光晕呼吸"，不再用 opacity 调暗（会把文字弄糊）；
#                         按下（:active）即反馈；忙态提到授权恢复 await 之前；
#                         新增最短忙态 260ms（防短请求下按钮"抖一下"）。
#   V1.0.21 相对 V1.0.20（2026-10-04）：
#                       · 启停性能：V4L2 打开的 800ms 格式稳定等待改为**只在冷打开时等**
#                         （停→启时设备未断电、格式早已稳定）；实测 start997ms → ~200ms。
#                       · WorkerPool 新增 stop_keep_workers()：停线程但保留 worker
#                         （模型/NPU 上下文常驻，下次 start_loops 省掉模型加载 ~200ms）。
#                         ★ 能力已备，CoreRuntime::stop()暂不切换（"停止"要否释放 NPU
#                           由产品语义定，见 WorkerPool.hpp 注释）。
#   V1.0.20 相对 V1.0.19（2026-10-04）：
#                       · core 结构：Application.cpp 按职责拆成 4 个编译单元
#                         （2341 → 1086行，只搬定义不改逻辑，36 个成员逐字节一致）；
#                         共享辅助与 shutdown 标志提到头文件（拆 TU 的硬前提）。
#                       · core 体检（cppcheck 首跑）：修ConfigManager 空指针保护写成
#                         崩溃入口、ModelRegistry 变量遮蔽、3 处析构调虚函数加防误改注释；
#                         标注 5 处死函数 + 1 组假覆盖测试（ModelRuntimeState 已被
#                         model_ready()取代，生产 0 引用）。
#                       · web 层：修 hub 绑定在动态加载路径下 KeyError
#                         （framework 两条用例自 S9/S10 起一直红，stash 验证过）。
#   V1.0.19 相对 V1.0.18（2026-10-03）：
#                       · web 层：轮询路径加签名守卫（静止 DOM 变更 -75%）、
#                         连接状态三处误报修复（阈值/超时/首屏文案）。
#   V1.0.18 相对 V1.0.17（2026-10-03）：
#                       · web 层：面板与后端按域拆分 + 静态资源 gzip（-72% 传输）
#                       · core：去写死的板级拓扑（风扇 hwmon / UDC 名改运行时解析）、
#                         日志三文件分流（报错与客户操作永不被轮转吃掉）。
project(ttbox_core VERSION 1.0.55 LANGUAGES CXX)

option(TTBOX_CORE_BUILD_TESTS "构建 C++ Core 单元测试" ON)
option(TTBOX_CORE_BUILD_TOOLS "构建 IPC 命令行工具" ON)
# 硬件测试（需要 /dev/video0，仅板端启用；host 上默认关闭）
option(TTBOX_CORE_BUILD_HW_TESTS "构建 V4L2 硬件验收测试（RK3588）" OFF)
option(TTBOX_CORE_BUILD_AUTH "构建在线授权层（需要 OpenSSL 开发文件）" OFF)
option(TTBOX_CORE_WITH_ONNX "启用 ONNX Runtime CPU 后端（Windows/宿主机）" OFF)
# 交叉编译 aarch64（RK3588）时置 ON：开启"依赖缺失即失败"的硬门禁，
# 防止 RKNN/RGA 探测失败只打 WARNING、导致 ttbox_core_main 被静默跳过。
# Windows 本机开发必须保持 OFF（本机无 RKNN 属正常，不应 FATAL）。
option(TTBOX_CROSS_AARCH64 "交叉编译 aarch64：依赖缺失时 FATAL_ERROR" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 项目根 (ttbox2/): core 位于 <root>/ttbox/core
# 交叉编译时用 -DTTBOX_PROJECT_ROOT=/opt/ttbox 覆盖为板端运行根。
# 注意：-D 创建的是 CACHE 变量，若此处无条件覆盖为普通变量会把 -D 静默忽略，
# 因此仅在未定义时推导默认值（不加 -D 时行为与改动前完全一致）。
#
# ★ S1 / T1.15 ⑥（2026-09-17）：本宏**被编进目标码**（Application.cpp 的
#   kDefaultConfigPath = TTBOX_PROJECT_ROOT "/config/default.json"，编译期定值），
#   故它对产物字节有直接影响，必须显式固定。**未固定**的后果（build-reproducibility.md
#   §13「配置向量」两个后果）：
#     甲 · 跨检出路径 ⇒ sha 漂移（默认值 = 检出路径被编进产物）；
#     乙 · 产物内泄漏构建机绝对路径 ⇒ 板端 kDefaultConfigPath 指向不存在的构建机路径。
#   ⇒ 交叉编译（= 出货形态）下**未显式给出即 FATAL**（fail-closed，拒绝产出不可出货的二进制）。
#      本机开发（TTBOX_CROSS_AARCH64=OFF）保持旧行为（推导默认值），不影响 Windows 日常开发。
if(NOT DEFINED TTBOX_PROJECT_ROOT)
  if(TTBOX_CROSS_AARCH64)
    message(FATAL_ERROR
      "交叉编译（TTBOX_CROSS_AARCH64=ON）未显式固定 -DTTBOX_PROJECT_ROOT ⇒ 拒绝配置。\n"
      "  为什么硬失败：该宏被编进目标码（Application.cpp 的 kDefaultConfigPath），\n"
      "  不固定时会回退到检出路径 '${CMAKE_CURRENT_SOURCE_DIR}/..' ⇒\n"
      "    · 产物内泄漏构建机绝对路径（板端 kDefaultConfigPath 指向不存在的路径）；\n"
      "    · 换检出路径即产物 sha 漂移（同源不再可比）。\n"
      "  修法：configure 显式传 -DTTBOX_PROJECT_ROOT=/opt/ttbox\n"
      "        （出货构建请走 scripts/ttbox_build_release.sh —— 它已强制传入并会复核。）\n"
      "  依据：build-reproducibility.md §13 配置向量表 / tasks.md T1.15 待补 ⑥。")
  endif()
  get_filename_component(TTBOX_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()

# ---- 编译警告 ----
if(MSVC)
  # /utf-8：源码为 UTF-8（中文注释），GB 代码页下会被错读导致 #pragma once 失效
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- 源码路径归一（A4 附加断言 ①：产物内不得含构建机绝对路径）----
# 背景（2026-09-17 实测）：`TTBOX_LOGx` 宏经 core/src/common/Logger.hpp 把 `__FILE__` 编进产物，
#   而 CMake/ninja 传给编译器的是**绝对源码路径** ⇒ `.rodata` 内出现 11 条构建机路径
#   （`/mnt/c/.../core/src/**.cpp`），违反 m1-acceptance-checklist.md A4 附加断言 ①（期望 0）。
#   ★ 这是**独立于 `TTBOX_PROJECT_ROOT` 的第二个绝对路径源** —— 只 pin 前者不足以满足 A4①，
#     且会让"产物路径锚"门禁拒绝每一次出货构建（门禁不可达成）。
# 处置：-ffile-prefix-map 把编译期记录的源码绝对路径映射为**仓库内相对路径**
#   ⇒ 日志仍可读（`./core/src/app/Application.cpp`），且产物不再含构建机路径；
#   副作用：产物字节与 BuildID 改变 ⇒ 指纹锚须**保留旧锚 + 追加带时点的新锚**（不得偷删）。
# MSVC 不支持该选项（其等价物是 /d1trimfile：），故仅非 MSVC 生效。
if(NOT MSVC)
  get_filename_component(TTBOX_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  add_compile_options("-ffile-prefix-map=${TTBOX_SOURCE_ROOT}=.")
endif()

# ---- 核心库 ----
set(CORE_SOURCES
  # ★ 条件源闭包（P0-1）：src/app/Application.cpp / src/runtime/CoreRuntime.cpp /
  #   src/runtime/HardwareRunner.cpp **已移出本无条件列表** —— 它们无条件引用
  #   WorkerPool::*/RKNNEngine，必须与 WorkerPool.cpp 共用同一编译条件
  #   （见下方 "条件源闭包（P0-1）" 块）。放错位置 = 无 RKNN 的 host 链接期缺符号。
  src/aim/AimThread.cpp
  src/input/PhysicalMouseReader.cpp
  src/output/FifoHidOutput.cpp
  src/output/AiboxHidOutput.cpp
  src/output/OutputBackend.cpp
  src/output/LocalHidBackend.cpp
  src/output/MouseControlClient.cpp
  src/common/Logger.cpp
  # §4.2 行数规矩：FileSink（轮转 / 环形缓冲 / §5.2 三分流）拆成独立源，
  #   Logger.cpp 只留「格式与分发」。test_license_daemon 只做符号隔离用，不需本文件。
  src/common/LoggerFileSink.cpp
  src/common/CpuAffinity.cpp
  src/common/RtSched.cpp
  src/common/Json.cpp
  src/config/ConfigManager.cpp
  src/ipc/IpcServer.cpp
  src/model/ModelAdapter.cpp
  src/model/ModelRegistry.cpp
  src/model/ModelManagement.cpp
  src/model/RuntimeProfile.cpp
  src/model/IModelSource.cpp
  src/model/backend/ModelBackend.cpp
  src/rknn/Preprocess.cpp
  src/rknn/DecodeNMS.cpp
  src/rknn/DetectionGeometryFilter.cpp
  src/hid/HidForwarder.cpp
  src/hid/HidParser.cpp
  src/hid/HidPackageManifest.cpp
  src/hid/HidPackageConfig.cpp
  src/hid/HidPackageRegistry.cpp
  src/hid/HidRuntime.cpp
  src/hid/IHidPackageSource.cpp
  # A10：AI 鼠标注入（纯 C++ 算法模块，host 可测）
  src/mouse/TargetSelector.cpp
  src/mouse/AimPointProfile.cpp
  src/mouse/AimTracker.cpp
  src/mouse/CoordinateTransform.cpp
  src/mouse/MouseRouter.cpp
  src/mouse/AimStateMachine.cpp
  src/mouse/PersonalMotion.cpp
  # T1.09：授权持久化（原子写 + 自愈 + 防降级）。纯 C++/POSIX 文件 I-O + 仓库内置 Json，
  #        **无 OpenSSL / 无网络** ⇒ 无条件列入：AUTH=ON/OFF 两种构建都要编
  #        （规格 §2.8.1；M2 起 DisabledAuth 桩已删，本注释为历史沿革）。
  src/auth/LicenseStore.cpp
  # T1.10：授权执法快照（LicenseGate）。同上**无 OpenSSL / 无网络** ⇒ 无条件列入
  #        （AUTH=ON/OFF 两种构建都要编；会话边界/IPC 投影/板端诊断三处只是它的投影）。
  src/auth/LicenseGate.cpp
  # M2.01/M2.02：离线签名卡授权链（全部无 OpenSSL / 无网络 ⇒ 无条件列入）。
  #   · LicenseCard：卡信封解析 + canonical + 归一化（纯逻辑）
  #   · ed25519_verify：TweetNaCl verify-only 提取（公有域，自包含）
  #   · DeviceFingerprint / LicenseDaemon：从 AUTH=ON 列表移入——真 daemon 本就
  #     无 OpenSSL 依赖（在线客户端才有）；M2 起出货向量（AUTH=OFF）必须走真验签
  #     ⇒ DisabledAuth fail-open 桩退役（文件已删，ODR 单一定义 = LicenseDaemon.cpp）
  src/auth/LicenseCard.cpp
  src/auth/ed25519/ed25519_verify.cpp
  src/auth/DeviceFingerprint.cpp
  src/auth/LicenseDaemon.cpp
  src/auth/OfflineCardClient.cpp
  # M2.05：卡号可读短码（FNV-1a-32 + Crockford-Base32 + Luhn mod 32）。**纯逻辑、无平台分支**、
  #   无 OpenSSL / 无网络 / 无文件 I-O ⇒ 无条件列入（AUTH=ON/OFF 两种构建都编；
  #   Windows(MinGW) 与 Linux 双可编译）。被 LicenseGate::to_snapshot 调用派生 short_code。
  src/auth/LicenseShortCode.cpp
)
if(TTBOX_CORE_BUILD_AUTH)
  # 在线授权层（TtboxLicenseClient）使用 OpenSSL 的 HMAC/SSL。
  # 此处仅做依赖探测；对 ttbox_core 的 include/link 注入必须放在
  # add_library(ttbox_core ...) 之后（见下方"授权层 OpenSSL 注入"），
  # 否则 CMake 会因 target 尚不存在而在 configure 阶段直接报错。
  # ★ M2 注：本开关语义收窄为"在线客户端层"（T2.03 在线接入用）；
  #   离线验签（OfflineCardClient）不受其门控、两构建都编。
  find_package(OpenSSL QUIET)
  if(NOT OPENSSL_FOUND)
    message(WARNING
      "TTBOX_CORE_BUILD_AUTH=ON 但未找到 OpenSSL 开发文件（HMAC/SSL）："
      "在线授权层将无法编译链接，请安装 libssl-dev 或关闭该开关。")
  endif()
  list(APPEND CORE_SOURCES
    src/auth/HttpClient.cpp
    src/auth/TtboxLicenseClient.cpp
  )
endif()
set(TTBOX_CORE_HAS_RGA FALSE)
set(TTBOX_CORE_HAS_RKNN FALSE)
if(UNIX AND NOT APPLE)
  list(APPEND CORE_SOURCES
    src/capture/DmaBuf.cpp
    src/capture/V4L2Capture.cpp
    src/preview/PreviewModule.cpp
  )
  # RGA（librga，阶段 A-3）：板端已装 /usr/local；缺失则不编译 RGA 源
  # RGA（librga）：TTBOX 独立库位于 /opt/ttbox/lib
  find_path(RGA_INCLUDE_DIR im2d.h PATHS /opt/ttbox/lib/include /usr/local/include /usr/include /usr/include/rga)
  find_library(RGA_LIBRARY rga PATHS /opt/ttbox/lib /usr/local/lib)
  if(RGA_INCLUDE_DIR AND RGA_LIBRARY)
    list(APPEND CORE_SOURCES src/rga/RgaProcessor.cpp)
    set(TTBOX_CORE_HAS_RGA TRUE)
    message(STATUS "librga found: include=${RGA_INCLUDE_DIR} lib=${RGA_LIBRARY}")
  else()
    message(WARNING "librga 未找到（include=${RGA_INCLUDE_DIR} lib=${RGA_LIBRARY}），RgaProcessor 不编译")
  endif()

  # RKNN（librknnrt）：TTBOX 独立库。
  # ★ T1.18：删除 `if(EXISTS /opt/ttbox/lib/librknnrt.so)` host 绝对路径旁路
  #   （权威文本 lib-scope-ruling.md §6「EXISTS 旁路」）——该旁路走 host 绝对路径直查、
  #   不吃 toolchain-aarch64.cmake 的 FIND_ROOT_PATH_MODE_LIBRARY ONLY，交叉编译时若**构建机**
  #   恰有 /opt/ttbox/lib/librknnrt.so，会**静默链上非 sysroot 的库**（与 §7 的 JPEG fallback
  #   死代码同族："用 host 存在性代替 sysroot 探测"）。统一走 find_library（受 sysroot re-root
  #   管辖）；出货前由 scripts/ttbox_fhs_init.sh 的 rknnrt_gate（同源 sha256）硬门禁把守。
  find_path(RKNN_INCLUDE_DIR rknn_api.h PATHS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rknn)
  find_library(RKNNRT_LIBRARY rknnrt PATHS /opt/ttbox/lib /usr/lib /usr/local/lib)
  if(RKNNRT_LIBRARY AND RKNN_INCLUDE_DIR)
    list(APPEND CORE_SOURCES
      src/rknn/RKNNEngine.cpp
      src/rknn/NpuMonitor.cpp
      src/rknn/WorkerPool.cpp
        )
    set(TTBOX_CORE_HAS_RKNN TRUE)
    message(STATUS "librknnrt found: lib=${RKNNRT_LIBRARY} include=${RKNN_INCLUDE_DIR}")
  else()
    message(WARNING "librknnrt 未找到（lib=${RKNNRT_LIBRARY} include=${RKNN_INCLUDE_DIR}），RKNNEngine 不编译")
  endif()
endif()

# ---- 条件源闭包（P0-1）：WorkerPool 的消费端与 WorkerPool **共用同一编译条件** ----
# 缺陷（UNIX host 实测）：Application.cpp / CoreRuntime.cpp / HardwareRunner.cpp **无条件**引用
#   WorkerPool::{start,stop,total_processed,total_errors} 与 InferenceWorker::~InferenceWorker()
#   （HardwareRunner 另引用 RKNNEngine），而 WorkerPool.cpp / RKNNEngine.cpp **仅在 RKNN 可用时**
#   编入 ⇒ 无 RKNN 的 UNIX host 链接期 `undefined reference to WorkerPool::stop()` 等（M1 收口阻断项）。
# 修法（team-lead 裁决，选项 b）：**不给 WorkerPool 加软依赖**，而是让消费端与其**共用同一条件**。
#   选择 (b) 而非 (a) 的理由：WorkerPool.cpp **确有 RKNN 硬依赖**（构造 RKNNEngine、调 rknn_* API），
#   (a)"UNIX 下无条件编 WorkerPool.cpp" 只把 undefined 从 WorkerPool 下沉到 RKNNEngine/rknn_* —— 不成立。
#   WIN32 一并保留（WorkerPool.cpp 在 _WIN32 是空 TU；且 Windows 侧无 target 引用这三者、不进链接），
#   使 Windows 编译面与改动前**逐字一致**（零行为变化）。
if(TTBOX_CORE_HAS_RKNN OR WIN32)
  list(APPEND CORE_SOURCES
    src/app/Application.cpp
    # ★ 2026-10-04（S1）：Application.cpp 按职责拆成 4 个 TU。
    #   **必须与 Application.cpp 同条件编入** —— 三个新文件里的成员定义同样无条件引用
    #   WorkerPool / RKNNEngine（build_runtime_params / switch_active_model_runtime 等），
    #   少编一个就是链接期 undefined reference。
    #   ApplicationRuntime.cpp 依赖 build_runtime_params/switch_active_model_runtime
    #   等带 RKNN 面的成员；ApplicationLicense/Ipc 依赖 RuntimeProfile + IO。
    src/app/ApplicationIpc.cpp
    src/app/ApplicationLicense.cpp
    src/app/ApplicationRuntime.cpp
    src/runtime/CoreRuntime.cpp
    src/runtime/HardwareRunner.cpp
  )
endif()

# ---- 交叉编译硬门禁（仅 -DTTBOX_CROSS_AARCH64=ON 时生效）----
# 背景：ttbox_core_main 的构建被 TTBOX_CORE_HAS_RKNN 门禁（见下方 add_executable 处）。
# RKNN 探测失败时上面只打 WARNING，会导致"cmake 成功 + make 成功 + 退出码 0，
# 但没有生成 ttbox_core_main"的静默降级。交叉编译时必须在 configure 阶段就失败。
# Windows 本机开发不开启本开关（本机无 RKNN 属正常，不应 FATAL）。
if(TTBOX_CROSS_AARCH64)
  if(NOT TTBOX_CORE_HAS_RKNN)
    message(FATAL_ERROR
      "交叉编译：librknnrt 未找到 —— ttbox_core_main 将被静默跳过，产物不可用。\n"
      "  当前探测值：RKNNRT_LIBRARY='${RKNNRT_LIBRARY}' RKNN_INCLUDE_DIR='${RKNN_INCLUDE_DIR}'\n"
      "  请检查：\n"
      "    1. CMAKE_SYSROOT / CMAKE_FIND_ROOT_PATH 是否设置，且 sysroot 是否镜像板端目录结构\n"
      "       （需要 <sysroot>/opt/ttbox/lib 与 <sysroot>/usr/local/lib）\n"
      "    2. 或显式指定 -DRKNNRT_LIBRARY=<librknnrt.so> -DRKNN_INCLUDE_DIR=<含 rknn_api.h 的目录>")
  endif()
  if(NOT TTBOX_CORE_HAS_RGA)
    message(WARNING
      "交叉编译：librga 未找到 —— RgaProcessor 不编译，产物将缺失 RGA 硬件缩放。\n"
      "  当前探测值：RGA_LIBRARY='${RGA_LIBRARY}' RGA_INCLUDE_DIR='${RGA_INCLUDE_DIR}'\n"
      "  若非预期，请检查 sysroot（需要 <sysroot>/usr/local/include/rga 与 librga.so）")
  endif()
  message(STATUS "交叉编译硬门禁已启用（TTBOX_CROSS_AARCH64=ON）：RKNN=找到，RGA=${TTBOX_CORE_HAS_RGA}")
endif()

if(TTBOX_CORE_WITH_ONNX)
  if(NOT WIN32)
    message(FATAL_ERROR "TTBOX_CORE_WITH_ONNX 当前仅支持 Windows x64")
  endif()
  set(TTBOX_ONNX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
  if(NOT EXISTS "${TTBOX_ONNX_ROOT}/include/onnxruntime_cxx_api.h" OR
     NOT EXISTS "${TTBOX_ONNX_ROOT}/lib/x64/onnxruntime.lib")
    message(FATAL_ERROR "ONNX Runtime 未找到：core/third_party/onnxruntime/include 与 lib/x64")
  endif()
  list(APPEND CORE_SOURCES src/model/backend/OnnxBackend.cpp)
endif()

# ---- EDID 核心（V1.0.53：自 scripts/edid/*.py 移植；业主令「去掉所有 Python 代码」）----
# 无条件源：纯算法（时序表 / 256B 构造 / 合规校验），零平台依赖（只用 common/Json）
# ⇒ host 与板端都能编译 ⇒ 逐字节对拍测试可在本机跑（core/tests/test_edid_golden.cpp）。
# ★ 必须位于 add_library(ttbox_core ...) 之前（同 ONNX/Preview 块的顺序约束）。
list(APPEND CORE_SOURCES
  src/edid/EdidTiming.cpp
  src/edid/EdidBuilder.cpp
  src/edid/EdidValidator.cpp
  src/edid/EdidConfig.cpp
  src/edid/EdidApply.cpp)

# ---- OTA 核心（V1.0.56：自 scripts/ttbox_ota_updater.py 移植）----
# 同样是纯逻辑优先：版本比较 / canonical(验签序列化) / 标识符消毒 —— 零平台依赖，host 可编可对拍。
# ★ canonical 单列出来自己实现、**不复用** common/Json.hpp 的 dump()：它是签名协议的一部分，
#   必须锁死字节格式；与通用 dump 混用会在"顺手优化通用函数"时静默打断 OTA 验签。
list(APPEND CORE_SOURCES
  src/common/Shell.cpp
  src/ota/OtaCanonical.cpp
  src/ota/OtaVersion.cpp
  # 验签与摘要：复用 auth/ed25519（TweetNaCl 自包含，零外部依赖）+ 自实现 SHA-256。
  # ★ 不引 OpenSSL：出货向量强制 AUTH=OFF，且随包 ELF 的运行期依赖闭集已冻结。
  src/ota/OtaCrypto.cpp
  src/ota/OtaExtract.cpp
  src/ota/OtaUpdater.cpp)

# ---- Phase2：Preview 源闭包（libjpeg + OpenCV 双端，板端 UNIX）源选择 ----
# 顺序硬约束：本块必须位于 add_library(ttbox_core ...) 之前。
#   add_library 一旦执行，CORE_SOURCES 是否包含 PreviewModule.cpp 就被固化；
#   之后再 list(REMOVE_ITEM ...) 是**空操作** —— 表现为"声明不编译、实际仍编译"，
#   而 PreviewModule.cpp 依赖 jpeglib.h + opencv2/*，缺任一编译就会直接失败。
#   ★ T1.18：OpenCV 探测也必须在 add_library **之前**（原先它在 add_library 之后，
#     结构上根本无法据其缺失 REMOVE_ITEM 源——权威文本 libjpeg-linkage-ruling.md §1.2）。
#   ★ T1.18：无 libjpeg / 无 OpenCV 时仅 REMOVE_ITEM **不够**——消费端
#     （Application.cpp:654 / CoreRuntime.cpp:111-127）无条件引用
#     PreviewModule::{start,stop,snapshot}，必须**同时** APPEND 上 stub，
#     否则链接期缺符号（§4.2(2)）。
#   因此：find_package 探测 + 条件 list(REMOVE_ITEM)/list(APPEND) 全部在本块完成；
#   对应的 target_link_libraries / target_include_directories 注入放到 add_library 之后。
#
# ★ 出货门禁开关（裁决 1(a)：主保险放 configure 期，执行者不可能跳过）：
#   TTBOX_SHIP=ON 时，PreviewModule 必须走**真实现**（jpeg ∧ opencv 齐备）；
#   否则 configure 期硬失败（见下方 endif() 之后的 FATAL_ERROR）。
#   非 UNIX（Windows/msys2）下 HAS_JPEG/HAS_OPENCV **从不被赋值**（保持 FALSE）
#   ⇒ 在 Windows 上 -DTTBOX_SHIP=ON 必然硬失败，而非静默产出 stub 链接的产物。
option(TTBOX_SHIP "发布/出货构建：缺 libjpeg/OpenCV 即 configure 期硬失败" OFF)
set(TTBOX_CORE_HAS_JPEG   FALSE)
set(TTBOX_CORE_HAS_OPENCV FALSE)
if(UNIX AND NOT APPLE)
  # JPEG：只留 find_package —— §7 的 find_library fallback（PATHS /usr/lib）在交叉构建下是
  #   死代码（被 sysroot re-root、且 sysroot 无 usr/lib/libjpeg.so）⇒ 删除，避免"静默救不了"。
  find_package(JPEG QUIET)
  if(JPEG_FOUND)
    set(TTBOX_CORE_HAS_JPEG TRUE)
    message(STATUS "libjpeg found: ${JPEG_LIBRARIES}")
  endif()
  # ★ OpenCV 探测【上移到此】（原在 add_library 之后的 OpenCV 块）
  find_package(OpenCV QUIET COMPONENTS core imgproc)
  if(OpenCV_FOUND)
    set(TTBOX_CORE_HAS_OPENCV TRUE)
    message(STATUS "OpenCV found: ${OpenCV_VERSION} (${OpenCV_LIBS})")
  endif()

  # ★ 配置位生产者：发布门禁（scripts/ttbox_build_release.sh，libjpeg-linkage-ruling.md §5）
  #   从 configure 日志按 `-- TTBOX_CORE_HAS_*=TRUE|FALSE` 解析，非 TRUE 即拒出货。
  message(STATUS "TTBOX_CORE_HAS_JPEG=${TTBOX_CORE_HAS_JPEG}")
  message(STATUS "TTBOX_CORE_HAS_OPENCV=${TTBOX_CORE_HAS_OPENCV}")

  # ★ 条件源闭包：仅当 jpeg AND opencv 都齐，才编译真实现；否则换 stub（三组合全覆盖：
  #   缺 jpeg / 缺 opencv〔有 jpeg〕/ 双缺）。
  if(NOT (TTBOX_CORE_HAS_JPEG AND TTBOX_CORE_HAS_OPENCV))
    # 注：此处消息不得再写 "不编译 / 无画框仍可工作"——真正发生的是"改用 stub 空实现"，
    #     否则就是"警告承诺优雅降级、实际链接失败"的假话（T1.18 §4.2(4)）。
    message(STATUS "PreviewModule 链接 PreviewModule_stub.cpp"
                   "（缺 libjpeg=${TTBOX_CORE_HAS_JPEG} / 缺 OpenCV=${TTBOX_CORE_HAS_OPENCV}；预览不可用，流水线不受影响）")
    list(REMOVE_ITEM CORE_SOURCES src/preview/PreviewModule.cpp)
    list(APPEND     CORE_SOURCES src/preview/PreviewModule_stub.cpp)   # ★ T1.18
  endif()
endif()

# ★ 裁决 1(a)：出货构建 configure 期硬失败门禁（在 UNIX 块 **之外**评估）。
#   为何放在 endif() 之外：非 UNIX（Windows/msys2）下 TTBOX_CORE_HAS_JPEG/HAS_OPENCV
#   **从不被赋值** ⇒ 恒为 FALSE ⇒ -DTTBOX_SHIP=ON 在此处必然 FATAL，
#   而不是静默产出一个"预览走 stub"的产物。这是"缺行==FALSE==拒出货"语义在配置层的等价物。
if(TTBOX_SHIP AND NOT (TTBOX_CORE_HAS_JPEG AND TTBOX_CORE_HAS_OPENCV))
  message(FATAL_ERROR "TTBOX_SHIP=ON 但 TTBOX_CORE_HAS_JPEG=${TTBOX_CORE_HAS_JPEG} / TTBOX_CORE_HAS_OPENCV=${TTBOX_CORE_HAS_OPENCV} ⇒ PreviewModule 走 stub ⇒ 预览/画框不可用 ⇒ 拒绝出货（libjpeg-linkage-ruling.md §5）")
endif()

add_library(ttbox_core STATIC ${CORE_SOURCES})
target_include_directories(ttbox_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# 授权层 OpenSSL 注入：必须位于 add_library(ttbox_core ...) 之后，
# 否则 target_include_directories/target_link_libraries 会因 target 不存在而失败。
# 仅在 AUTH=ON 且确实找到 OpenSSL 时注入（与上面的探测一致）。
if(TTBOX_CORE_BUILD_AUTH AND OPENSSL_FOUND)
  target_include_directories(ttbox_core PUBLIC ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(ttbox_core PUBLIC ${OPENSSL_LIBRARIES})
endif()
if(TTBOX_CORE_HAS_RGA)
  target_include_directories(ttbox_core PUBLIC ${RGA_INCLUDE_DIR})
  target_link_libraries(ttbox_core PUBLIC ${RGA_LIBRARY})
  # ★ 条件源闭包（P0-1）：把 RGA 可用性暴露为编译位，供 Preprocess.cpp 的 RGA 分支
  #   与 RgaProcessor.cpp 的编译条件**逐字一致**（原为 `__unix__` 隐式假设 ⇒ host 链接失败）。
  target_compile_definitions(ttbox_core PUBLIC TTBOX_CORE_HAS_RGA=1)
endif()
if(TTBOX_CORE_WITH_ONNX)
  target_include_directories(ttbox_core PUBLIC "${TTBOX_ONNX_ROOT}/include")
  target_link_libraries(ttbox_core PUBLIC "${TTBOX_ONNX_ROOT}/lib/x64/onnxruntime.lib")
  target_compile_definitions(ttbox_core PUBLIC TTBOX_CORE_HAS_ONNX=1)
endif()
if(TTBOX_CORE_HAS_RKNN)
  target_include_directories(ttbox_core PUBLIC ${RKNN_INCLUDE_DIR})
  target_link_libraries(ttbox_core PUBLIC ${RKNNRT_LIBRARY})
endif()
# 注入项目根（默认配置文件路径 / 测试定位真实 default.json）
target_compile_definitions(ttbox_core PUBLIC TTBOX_PROJECT_ROOT="${TTBOX_PROJECT_ROOT}")
if(TTBOX_CORE_HAS_RKNN)
  target_compile_definitions(ttbox_core PUBLIC TTBOX_CORE_HAS_RKNN=1)
endif()
if(WIN32)
  target_link_libraries(ttbox_core PUBLIC ws2_32)
endif()
# Phase2：Preview 链接注入（libjpeg / OpenCV，板端 UNIX）
# 注：探测（find_package JPEG/OpenCV）+ 条件源选择（REMOVE_ITEM/APPEND）必须发生在
# add_library(ttbox_core ...) 之前（见上方"Preview 源闭包"块）；此处的 target_*
# 注入必须位于 add_library 之后。两半分离、不可合并，是本文件的核心顺序约束。
# ★ T1.18：OpenCV 探测已上移到 add_library 之前（此处不再重复 find_package）；原先此处
#   那句 `message(WARNING "OpenCV 未找到，预览检测框绘制不编译（PreviewModule 无画框仍可工作）")`
#   两个方向都与事实相反（源码侧 PreviewModule.cpp 无条件 include <opencv2/*> ⇒ 缺 ocv 是
#   编译失败；产物侧 libopencv_core/imgproc 是硬 NEEDED ⇒ 缺 ocv 是整程序加载失败），
#   已删除——其语义并入上方"PreviewModule 链接 PreviewModule_stub.cpp"的 STATUS。
if(UNIX AND NOT APPLE)
  if(TTBOX_CORE_HAS_JPEG)
    target_link_libraries(ttbox_core PUBLIC ${JPEG_LIBRARIES})
    target_include_directories(ttbox_core PUBLIC ${JPEG_INCLUDE_DIRS})
    # §4.2(3)：暴露配置位（与 TTBOX_CORE_HAS_ONNX/HAS_RKNN 同款；本次不散点使用）
    target_compile_definitions(ttbox_core PUBLIC TTBOX_CORE_HAS_JPEG=1)
  endif()
  if(TTBOX_CORE_HAS_OPENCV)
    target_link_libraries(ttbox_core PUBLIC ${OpenCV_LIBS})
    target_include_directories(ttbox_core PUBLIC ${OpenCV_INCLUDE_DIRS})
    target_compile_definitions(ttbox_core PUBLIC TTBOX_CORE_HAS_OPENCV=1)
  endif()
endif()

# =============================================================================
# 受闸源闭包登记表（P0-1 / 与 T1.18 同族）：把"条件编译的源 × 无条件消费点"登记为**可门禁计数**。
#   ① 数量漂移即 FAIL —— 任何人新增/删除一组条件源，configure 期立刻暴露（不靠注释、不靠自觉）；
#   ② 每组"条件生效性"当场断言 —— 防"list(REMOVE_ITEM) 又被写成空操作 / 条件改回隐式假设"。
#   ★ 引用纪律：本表是**唯一**的受闸源组清单；新增/修改条件源时**必须**同步此表与 EXPECTED。
#   组 1 覆盖 team-lead 点名的 "jpeg / opencv" 双端（T1.18 已裁定其为**同一闭包**：PreviewModule）；
#   组 3 = "RKNN-WorkerPool"；组 2 = P0-1 实测发现的**第三处**同源缺陷（RGA↔Preprocess）。
# =============================================================================
set(TTBOX_GATED_SOURCE_GROUPS "")
macro(ttbox_gate_register name)
  list(APPEND TTBOX_GATED_SOURCE_GROUPS "${name}")
endmacro()

# 组 1：Preview（libjpeg ∧ OpenCV 条件源闭包；消费端 Application/CoreRuntime）—— T1.18 落地（stub 兜底）
ttbox_gate_register("preview_jpeg_opencv")
# 组 2：RGA（RgaProcessor.cpp ← Preprocess.cpp；消费端条件已统一为 TTBOX_CORE_HAS_RGA）—— P0-1
ttbox_gate_register("rga_preprocess")
# 组 3：RKNN（WorkerPool/RKNNEngine/NpuMonitor ← Application/CoreRuntime/HardwareRunner）—— P0-1
ttbox_gate_register("rknn_workerpool")

# ★ 期望值：修改此常量 = 显式承认"受闸源组数"发生变化（团队评审点）。目前 3 组。
set(TTBOX_GATED_SOURCE_GROUPS_EXPECTED 3)
list(LENGTH TTBOX_GATED_SOURCE_GROUPS _ttbox_gate_n)
if(NOT _ttbox_gate_n EQUAL TTBOX_GATED_SOURCE_GROUPS_EXPECTED)
  message(FATAL_ERROR
    "受闸源闭包登记表数量漂移：登记 ${_ttbox_gate_n} 组，期望 ${TTBOX_GATED_SOURCE_GROUPS_EXPECTED} 组。\n"
    "  当前登记：${TTBOX_GATED_SOURCE_GROUPS}\n"
    "  若确为新增/删除条件源组，请显式更新 TTBOX_GATED_SOURCE_GROUPS_EXPECTED（禁止静默）。")
endif()

# 条件生效性断言（防"REMOVE_ITEM 变空操作 / 条件回退成隐式假设"）：
if(NOT TTBOX_CORE_HAS_RGA AND "src/rga/RgaProcessor.cpp" IN_LIST CORE_SOURCES)
  message(FATAL_ERROR "受闸源组 rga_preprocess 条件失效：RGA 未找到，但 RgaProcessor.cpp 仍在 CORE_SOURCES")
endif()
if(TTBOX_CORE_HAS_RGA AND NOT "src/rga/RgaProcessor.cpp" IN_LIST CORE_SOURCES)
  message(FATAL_ERROR "受闸源组 rga_preprocess 条件失效：RGA 已找到，但 RgaProcessor.cpp 不在 CORE_SOURCES")
endif()
if(NOT TTBOX_CORE_HAS_RKNN AND "src/rknn/WorkerPool.cpp" IN_LIST CORE_SOURCES)
  message(FATAL_ERROR "受闸源组 rknn_workerpool 条件失效：RKNN 未找到，但 WorkerPool.cpp 仍在 CORE_SOURCES")
endif()
if(TTBOX_CORE_HAS_RKNN AND NOT "src/rknn/WorkerPool.cpp" IN_LIST CORE_SOURCES)
  message(FATAL_ERROR "受闸源组 rknn_workerpool 条件失效：RKNN 已找到，但 WorkerPool.cpp 不在 CORE_SOURCES")
endif()
# 组 1（preview）：libjpeg ∧ OpenCV 条件源闭包条件生效性断言 —— T1.18 落地。
#   Windows / 非 UNIX：CORE_SOURCES 从不含 preview 源（该源仅在 UNIX 块加入），
#   故断言整体置于 UNIX 分支内评估，避免误判。
if(UNIX AND NOT APPLE)
  set(_ttbox_preview_real FALSE)
  if(TTBOX_CORE_HAS_JPEG AND TTBOX_CORE_HAS_OPENCV)
    set(_ttbox_preview_real TRUE)
  endif()
  if(_ttbox_preview_real AND NOT "src/preview/PreviewModule.cpp" IN_LIST CORE_SOURCES)
    message(FATAL_ERROR "受闸源组 preview_jpeg_opencv 条件失效：jpeg+opencv 已找到，但 PreviewModule.cpp 不在 CORE_SOURCES")
  endif()
  if(NOT _ttbox_preview_real AND "src/preview/PreviewModule.cpp" IN_LIST CORE_SOURCES)
    message(FATAL_ERROR "受闸源组 preview_jpeg_opencv 条件失效：缺 jpeg 或 opencv，但 PreviewModule.cpp 仍在 CORE_SOURCES")
  endif()
  if(NOT _ttbox_preview_real AND NOT "src/preview/PreviewModule_stub.cpp" IN_LIST CORE_SOURCES)
    message(FATAL_ERROR "受闸源组 preview_jpeg_opencv 条件失效：缺 jpeg 或 opencv，但 stub 不在 CORE_SOURCES")
  endif()
  if(_ttbox_preview_real AND "src/preview/PreviewModule_stub.cpp" IN_LIST CORE_SOURCES)
    message(FATAL_ERROR "受闸源组 preview_jpeg_opencv 条件失效：jpeg+opencv 已找到，但 stub 误入 CORE_SOURCES")
  endif()
endif()

# ---- 新架构硬件入口（默认 NullHidOutput，不接管真实鼠标）----
if(UNIX AND NOT APPLE AND TTBOX_CORE_HAS_RKNN)
  add_executable(hardware_runner_main tools/hardware_runner_main.cpp)
  target_link_libraries(hardware_runner_main PRIVATE ttbox_core)
endif()

# ---- 主程序 ----
# V4L2/RKNN 仅在板端编译；Windows 下 CoreRuntime 会引用未编译符号，
# 因此 ttbox_core_main 仅在 UNIX 且 RKNN 可用时构建。
if(UNIX AND NOT APPLE AND TTBOX_CORE_HAS_RKNN)
  add_executable(ttbox_core_main src/main.cpp)
  target_link_libraries(ttbox_core_main PRIVATE ttbox_core)
  # T1.16 / P0-C：自包含 RUNPATH（形态 A = 仅 $ORIGIN/../lib 一段）
  # 背景（design §B1.5）：出货 payload 是 **build-tree 产物**——ttbox_release_install.sh
  # 用 tar 管道拷 payload，全脚本从不调用 `cmake --install`。⇒ 真正写进产物动态段的
  # 是 **BUILD_RPATH**，只改 INSTALL_RPATH 是"源码看着改了、产物纹丝不动"的静默假修。
  # 形态 B（$ORIGIN/../lib:/opt/ttbox/lib）= 假回滚（尾段跨版本共享目录缺库时静默回退）；
  # 形态 C（/opt/ttbox/lib）= 当前 bug——两者均禁止。$ORIGIN 必须转义 "\$ORIGIN"，
  # 否则生成器命令行会被 shell 把 $ORIGIN 展开为空 ⇒ 变成错值 /../lib。
  # 唯一权威判据是构建后 readelf -d（禁只比文本）。
  set_target_properties(ttbox_core_main PROPERTIES
    BUILD_RPATH   "\$ORIGIN/../lib"
    INSTALL_RPATH "\$ORIGIN/../lib"
    # 原为 TRUE：会把链接期发现的设备库绝对路径重新追加进 RUNPATH
    # （把形态 A 悄悄污染回形态 B）——必须 FALSE。
    INSTALL_RPATH_USE_LINK_PATH FALSE)
endif()

# ---- Web 面板后端（C++ 迁移 T01：httplib + IPC 客户端纯转发骨架）----
# 与 ttbox_core_main 不同：ttbox_web 是纯 HTTP 壳 + IPC 客户端，业务只调
# ttbox::core::ipc_request / ipc_ping / JsonValue（均为无条件源），零 RKNN/ONNX/
# OpenCV/JPEG/RGA 依赖 ⇒ 不做 TTBOX_CORE_HAS_RKNN 门禁，host（本机/CI）也能构建
# 冒烟验证；板端 aarch64 与 ttbox_core_main 同树编出（httplib 单头已 vendor）。
# 线程库说明：httplib 用 std::thread（与 core 既有 IpcServer.cpp 同款），板端 g++ 12 /
# glibc≥2.34 已把 libpthread 并入 libc，本文件亦无 find_package(Threads) 先例 ⇒ 不引入。
add_executable(ttbox_web
  src/web/main.cpp
  src/web/WebServer.cpp
  src/web/infra/ipc_client.cpp
  src/web/translate/hotkeys.cpp
  src/web/translate/controller_params.cpp
  src/web/translate/profile_translate_fov.cpp
  src/web/translate/profile_translate_aim.cpp
  src/web/translate/profile_translate_body.cpp
  src/web/translate/profile_translate_body_mouse.cpp
  src/web/translate/profile_translate_web.cpp
  src/web/domain/control.cpp
  src/web/domain/state.cpp
  src/web/domain/state_snapshot.cpp
  src/web/domain/models.cpp
  src/web/domain/models_meta.cpp
  src/web/domain/presets.cpp
  src/web/domain/hardware.cpp
  src/web/domain/license.cpp
  src/web/domain/ota.cpp
  src/web/domain/ota_impl.cpp
  src/web/domain/ota_query.cpp
  src/web/domain/system.cpp
  src/web/domain/diagnostics.cpp
  src/web/domain/brand.cpp
  src/web/domain/pages.cpp
  src/web/domain/aim.cpp
  src/web/domain/preview.cpp
  src/web/domain/calib.cpp
  src/web/domain/motion.cpp
  src/web/infra/calibration_controller.cpp
  src/web/infra/calibration_api.cpp
  src/web/infra/calibration_worker.cpp
  src/web/infra/calibration_worker_sample.cpp
  src/web/infra/calibration_worker_finish.cpp
  src/web/infra/motion_store.cpp
  src/web/infra/motion_store_session.cpp
  src/web/infra/motion_store_validate.cpp
)
target_link_libraries(ttbox_web PRIVATE ttbox_core)
target_include_directories(ttbox_web PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib
)
# 云端 card-login（license.cpp）复用 core 的 TtboxLicenseClient（在线授权层）。
# 仅在 TTBOX_CORE_BUILD_AUTH=ON 且 OpenSSL 找到时启用；默认构建（AUTH=OFF）诚实报 501。
if(TTBOX_CORE_BUILD_AUTH AND OPENSSL_FOUND)
  target_compile_definitions(ttbox_web PRIVATE TTBOX_WEB_CLOUD_AUTH=1)
endif()
# RUNPATH 与 ttbox_core_main 同口径：$ORIGIN/../lib 就近解析设备库（librknnrt.so 等），
# 形态 A（仅一段）且 INSTALL_RPATH_USE_LINK_PATH=FALSE，避免被链接期绝对路径污染。
set_target_properties(ttbox_web PROPERTIES
  BUILD_RPATH   "\$ORIGIN/../lib"
  INSTALL_RPATH "\$ORIGIN/../lib"
  INSTALL_RPATH_USE_LINK_PATH FALSE)

# ★ V1.0.49：httplib 开 SSL —— 访问云端 https（OTA check / announcement）必需。
#   旧版没开 SSL ⇒ httplib::Client 构造 https 时抛异常 ⇒ /api/update/check 返回 500
#   （V1.0.48 板端实测故障）。这里单独 find OpenSSL（不受 AUTH=OFF 影响）。
#
#   ★ 交叉编译要点：toolchain-aarch64.cmake 只设了 CMAKE_SYSROOT、**没设**
#     CMAKE_FIND_ROOT_PATH ⇒ FindOpenSSL 只会搜 host 默认路径（/usr/include 等），
#     找不到 sysroot 里的 aarch64 OpenSSL ⇒ OPENSSL_FOUND=FALSE、SSL 静默没开
#     （V1.0.49 首编实测踩到：configure 打出「未找到 OpenSSL」）。
#     修法：把 sysroot 显式喂给 find_package（OPENSSL_ROOT_DIR / _INCLUDE_DIR /
#     _SSL_LIBRARY / _CRYPTO_LIBRARY），交叉时它们优先于默认搜索。
find_package(OpenSSL QUIET)
if(NOT OPENSSL_FOUND AND CMAKE_CROSSCOMPILING AND CMAKE_SYSROOT)
  # aarch64 sysroot 的多架构布局：头在 <sysroot>/usr/include，库在 <sysroot>/usr/lib/< triplet>
  find_path(TTBOX_SSL_INCLUDE_DIR openssl/ssl.h
            PATHS "${CMAKE_SYSROOT}/usr/include" NO_DEFAULT_PATH)
  find_library(TTBOX_SSL_LIBRARY NAMES ssl
            PATHS "${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
                  "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu"
            NO_DEFAULT_PATH)
  find_library(TTBOX_CRYPTO_LIBRARY NAMES crypto
            PATHS "${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
                  "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu"
            NO_DEFAULT_PATH)
  if(TTBOX_SSL_INCLUDE_DIR AND TTBOX_SSL_LIBRARY AND TTBOX_CRYPTO_LIBRARY)
    set(OPENSSL_FOUND TRUE)
    set(OPENSSL_INCLUDE_DIR "${TTBOX_SSL_INCLUDE_DIR}")
    set(OPENSSL_SSL_LIBRARY "${TTBOX_SSL_LIBRARY}")
    set(OPENSSL_CRYPTO_LIBRARY "${TTBOX_CRYPTO_LIBRARY}")
    if(NOT TARGET OpenSSL::SSL)
      add_library(OpenSSL::SSL UNKNOWN IMPORTED)
      set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION "${TTBOX_SSL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TTBOX_SSL_INCLUDE_DIR}")
    endif()
    if(NOT TARGET OpenSSL::Crypto)
      add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
      set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION "${TTBOX_CRYPTO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TTBOX_SSL_INCLUDE_DIR}")
    endif()
  endif()
endif()

if(OPENSSL_FOUND)
  target_compile_definitions(ttbox_web PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(ttbox_web PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "ttbox_web: httplib SSL 已启用（ssl=${OPENSSL_SSL_LIBRARY} crypto=${OPENSSL_CRYPTO_LIBRARY}）")
else()
  message(WARNING "ttbox_web: 未找到 OpenSSL ⇒ 云端 https 请求会失败（/api/update/check 将 500）")
endif()

# ---- EDID 应用入口（V1.0.53：替代 scripts/edid/edid_apply.sh 与其中的 4 段内嵌 python）----
# 与 ttbox_web 同类：纯算法 + popen/sysfs，不依赖 RKNN/RGA/OpenCV/JPEG/OpenSSL
#   ⇒ 不做 TTBOX_CORE_HAS_RKNN 门禁，host 也能构建（本机只验编译；真正注入需要 Linux 的
#     sysfs 与 v4l2-ctl，本机跑不了 —— 这是有意的，便于快速验证编译）。
# ★ 刻意不用 fork+execvp：那会引入 sys/wait.h，使本机 MSYS **编不了**（system.cpp 的前车之鉴）。
# ★ 也刻意不加平台条件：无条件构建 ⇒ 板端与 host 同源编译，减少"只在板端才编到"的盲区。
add_executable(ttbox_edid src/edid/main.cpp)
target_link_libraries(ttbox_edid PRIVATE ttbox_core)
# 自包含 RUNPATH（形态 A = 仅 $ORIGIN/../lib 一段，与 ttbox_core_main / ttbox_web 同规格）。
# $ORIGIN 必须转义 "\$ORIGIN"，否则生成器命令行会被 shell 把 $ORIGIN 展开为空 ⇒ 变成 /../lib。
set_target_properties(ttbox_edid PROPERTIES
  BUILD_RPATH   "\$ORIGIN/../lib"
  INSTALL_RPATH "\$ORIGIN/../lib"
  INSTALL_RPATH_USE_LINK_PATH FALSE)

# ---- OTA 更新入口（V1.0.56：替代 scripts/ttbox_ota_updater.py）----
# 无条件构建、零新增依赖：HTTPS 走系统 curl（板端有 7.81）、解包走系统 tar、
# 验签走自包含 TweetNaCl（auth/ed25519）、SHA-256 自实现。
# ★ 刻意**不**引 OpenSSL：仅为下载而给升级通道增加 libssl/libcrypto 依赖不划算
#   （低频操作，且会动"随包 ELF 运行期依赖闭集"契约）。
add_executable(ttbox_ota src/ota/main.cpp)
target_link_libraries(ttbox_ota PRIVATE ttbox_core)
set_target_properties(ttbox_ota PROPERTIES
  BUILD_RPATH   "\$ORIGIN/../lib"
  INSTALL_RPATH "\$ORIGIN/../lib"
  INSTALL_RPATH_USE_LINK_PATH FALSE)

# ---- IPC 命令行工具 ----
if(TTBOX_CORE_BUILD_TOOLS)
  add_executable(ipc_ping tools/ipc_ping.cpp)
  target_link_libraries(ipc_ping PRIVATE ttbox_core)
  # 板端单图 GT 推理工具：真实 RKNN + TTBOX DecodeNMS（HDMI 真实链路验证用）
  if(UNIX AND NOT APPLE AND TTBOX_CORE_HAS_RKNN)
    add_executable(imgdetect tools/imgdetect.cpp)
    target_link_libraries(imgdetect PRIVATE ttbox_core)
  endif()
  # Windows 本地 Core 宿主：真实 ModelRegistry + IpcServer(TCP)，Web 模型库后端
  if(WIN32)
    add_executable(win_core_main tools/win_core_main.cpp)
    target_link_libraries(win_core_main PRIVATE ttbox_core)
  endif()
endif()

# ---- 测试 ----
if(TTBOX_CORE_BUILD_TESTS)
  enable_testing()
  set(TEST_SOURCES
    tests/test_main.cpp
    tests/test_logger.cpp
    tests/test_config.cpp
    tests/test_json.cpp         # 嵌套深度护栏（2026-09-23 审查复核 #20）
    tests/test_ipc.cpp
    tests/test_ipc_control.cpp  # Phase 1：SET_CONFIG / RUNTIME_CONTROL 验收
    tests/test_ipc_model.cpp    # v0.3：模型管理 IPC 验收
    # ★ test_application 由下方 P0-1 条件块统一管辖（UNIX ∧ NOT APPLE ∧ TTBOX_CORE_HAS_RKNN）。
    #   此处**不得**再留无条件/仅按平台排除的条目 —— 否则无 RKNN 的 host 上该 TU 仍被编入，
    #   而 Application.cpp 已随 P0-1 移出无条件 CORE_SOURCES ⇒ `undefined reference to
    #   ttbox::core::Application::*` ⇒ ttbox_core_tests 链接失败 ⇒ 全部 host 测试无法运行。
    tests/test_decode.cpp
    tests/test_model_metadata.cpp
    tests/test_decoder_dispatch.cpp
    tests/test_runtime_profile.cpp
    tests/test_model_registry.cpp
    tests/test_fov_roi.cpp
    tests/test_hid.cpp
    tests/test_hid_package.cpp
    tests/test_mouse.cpp  # A10：AI 鼠标注入模块测试
    tests/test_license_state_machine.cpp  # T1.08：授权降级状态机（纯函数，无 OpenSSL 依赖）
    tests/test_input_quant.cpp  # T1.14：输入量化守卫 + XOR 搬运（纯函数，Windows 可测；
                                #        引擎接线层仅在 TTBOX_CORE_HAS_RKNN 下启用）
    tests/test_input_path_summary.cpp  # T1.15：输入通路诊断（快照/聚合/文案/契约名，
                                       #        header-only 纯函数 ⇒ host 与板端同跑一份断言）
    tests/test_ttbox_canonical.cpp  # T1.07：canonical 拼接逐字节（header-only，无 OpenSSL）
    tests/test_license_store.cpp  # T1.09：原子写/掉电残留自愈/防降级（纯文件 I-O，无 OpenSSL；host 可跑）
    tests/test_license_gate.cpp  # T1.10：决策表（fail-open/fail-closed）+ wire 映射（纯函数，无 OpenSSL）
    tests/test_license_features.cpp  # M2：签名卡内容（features/plan/ui_brand）投影 + 可信态门控
                                     #     + 归一化/字符集闸门 + 网络失败保留 vs 权威否定清空（无 OpenSSL）
    tests/test_ed25519.cpp  # M2.01：Ed25519 验签原语（RFC 8032 官方向量 + 篡改负控；
                            #        TweetNaCl 提取路径的正确性锚，AUTH 无关）
    tests/test_license_card.cpp  # M2.01：离线卡解析/canonical/归一化（fail-closed 表）
                                 #        + OfflineCardClient 全链（坏卡/他板卡/过期卡拒绝）
    tests/test_license_shortcode.cpp  # M2.05：短码派生/校验位（FNV-1a-32 + Crockford32 + Luhn mod 32）
                                      #        正例 + 单字符替换/相邻换位负控 + 跨语言向量
                                      #        + pipeline_allowed/capabilities 投影（无 OpenSSL）
  )
  if(UNIX AND NOT APPLE)
    # ★ Windows/msys2 平台（UNIX=FALSE）此 TU 缺席 3 例（预期，非缺陷）：
    #   本文件恰有 3 个 TEST()，仅在此条件成立时被追加 ⇒ 造成 ttbox_core_tests 用例数的
    #   平台差（Linux/WSL host 见 145，Windows host 见 142）。报计数必须带平台与配置，
    #   勿把该差额当漂移。（"编了不等于跑了"同族：把差异明示，而非让人再追一遍。）
    list(APPEND TEST_SOURCES tests/test_latestframe.cpp)
  endif()
  # P0-1：test_application 依赖 Application→CoreRuntime→WorkerPool/RKNN；与 WorkerPool 共用条件
  #   （原先用 `$<NOT:$<PLATFORM_ID:Windows>>` ⇒ 无 RKNN 的 UNIX host 上仍编入 ⇒ 链接失败）。
  if(UNIX AND NOT APPLE AND TTBOX_CORE_HAS_RKNN)
    list(APPEND TEST_SOURCES tests/test_application.cpp)
  endif()
  if(TTBOX_CORE_HAS_RGA)
    list(APPEND TEST_SOURCES tests/test_rga.cpp)
  endif()
  if(TTBOX_CORE_WITH_ONNX)
    list(APPEND TEST_SOURCES tests/test_win_e2e.cpp)
  endif()
  # 真实模型 + 方向语义测试（无 ONNX 也跑层B 纯算法部分）
  list(APPEND TEST_SOURCES tests/test_real_model.cpp)
  # EDID C++ 移植的逐字节对拍（V1.0.53）。基准 = tools/edid_golden_gen.py 在删 Python 前
  # 冻结的现役实现输出；host 可跑（EDID 核心零平台依赖）。
  list(APPEND TEST_SOURCES tests/test_edid_golden.cpp)
  # OTA C++ 移植的逐值对拍（V1.0.56）。基准 = tools/ota_golden_gen.py 在删 Python 前冻结的
  # 现役实现输出（114 例）。OTA 是唯一升级通道，等价性必须钉死。
  list(APPEND TEST_SOURCES tests/test_ota_golden.cpp)
  add_executable(ttbox_core_tests ${TEST_SOURCES})
  target_include_directories(ttbox_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  # D1：把真实 rknn_api.h 的**绝对路径**注入测试（host/board 通用），使
  # input_quant_enum_mirror_against_real_header 能在 host 上运行时解析真实头文件，
  # 成为"改 rknn_api.h 即 FAIL"的真漂移锁（不再仅依赖板端编译期 static_assert）。
  # 路径为源树绝对路径，configure 时确定。
  set(TTBOX_RKNN_API_HEADER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rknn/rknn_api.h")
  # configure 期断言：头文件必须存在，否则真对锁会指向空路径而静默失效。
  # 与 test_input_quant.cpp 顶部 `#ifndef → #error` 双保险：
  #   这里保证**路径有效**，那里保证**宏已定义**（缺一即失败，绝不静默全绿）。
  if(NOT EXISTS "${TTBOX_RKNN_API_HEADER_PATH}")
    message(FATAL_ERROR
      "D1 host 枚举真对锁失效：未找到 ${TTBOX_RKNN_API_HEADER_PATH}。"
      "test_input_quant.cpp 的 input_quant_enum_mirror_against_real_header 依赖该文件。")
  endif()
  target_compile_definitions(ttbox_core_tests PRIVATE
    TTBOX_RKNN_API_HEADER="${TTBOX_RKNN_API_HEADER_PATH}"
    # ★ V1.0.53：EDID 对拍基准路径（configure 期确定；见下方存在性断言）
    TTBOX_EDID_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/edid_golden.json"
    # ★ V1.0.56：OTA 对拍基准路径
    TTBOX_OTA_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ota_golden.json"
    # ★ V1.0.56：真实历史签名 fixture（验证 C++ 验签与 Python cryptography 结论一致）
    TTBOX_OTA_REAL_SIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ota_real_signature.json")
  # EDID 对拍基准必须存在，否则 test_edid_golden 会静默失效（同 TTBOX_RKNN_API_HEADER 的
  # 「守卫存在=假象」纪律）。它是**入库文件**（tools/edid_golden_gen.py 产出）。
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/edid_golden.json")
    message(FATAL_ERROR
      "EDID 对拍基准缺失：core/tests/fixtures/edid_golden.json。"
      "请先运行 tools/edid_golden_gen.py（需现役 Python 实现在场）重新冻结。")
  endif()
  # OTA 对拍基准同理（OTA 是唯一升级通道，等价性靠它钉死）
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ota_golden.json")
    message(FATAL_ERROR
      "OTA 对拍基准缺失：core/tests/fixtures/ota_golden.json。"
      "请先运行 tools/ota_golden_gen.py（需现役 Python 实现在场）重新冻结。")
  endif()
  target_link_libraries(ttbox_core_tests PRIVATE ttbox_core)
  add_test(NAME ttbox_core_tests COMMAND ttbox_core_tests)

  # T1.08：真实 LicenseDaemon 端到端测试。自行编译 LicenseDaemon.cpp +
  # DeviceFingerprint.cpp（二者均不依赖 OpenSSL），故在默认构建（AUTH=OFF）下即可
  # 运行；且不链接 ttbox_core（避免与 ttbox_core 内同名符号冲突——M2 起
  # LicenseDaemon.cpp 已入 CORE_SOURCES）。
  # T1.09：LicenseDaemon 自 T1.09 起持有 LicenseStore（持久化卡文档/状态）。LicenseStore
  #   依赖 Logger（STATE_* 诊断）与 Json（解析 state.json），故本目标同编 LicenseStore.cpp /
  #   Logger.cpp / Json.cpp（三者均无 OpenSSL；亦不引入 ttbox_core，保持符号隔离）。
  # M2.02：LicenseDaemon::activate() 引用 parse_license_card ⇒ 增编 LicenseCard.cpp。
  add_executable(test_license_daemon
    tests/test_main.cpp
    tests/test_license_daemon.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/LicenseDaemon.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/LicenseStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/LicenseCard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/DeviceFingerprint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common/Logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common/Json.cpp)
  target_include_directories(test_license_daemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME test_license_daemon COMMAND test_license_daemon)

  # A11：Worker -> AimThread 无锁目标邮箱测试（不需要硬件）
  add_executable(test_aim_target_mailbox tests/test_aim_target_mailbox.cpp)
  target_link_libraries(test_aim_target_mailbox PRIVATE ttbox_core)
  add_test(NAME test_aim_target_mailbox COMMAND test_aim_target_mailbox)

  # 选靶静默行为回归：FOV 半径/置信度/贴边剔除/轨迹宽限/reset/优先级（不需要硬件）
  add_executable(test_target_selector tests/test_target_selector.cpp)
  target_link_libraries(test_target_selector PRIVATE ttbox_core)
  add_test(NAME test_target_selector COMMAND test_target_selector)

  # 深合并语义 + 空指针契约（2026-10-04 体检后P1 修复的护栏；不需要硬件）
  add_executable(test_config_deep_merge tests/test_config_deep_merge.cpp)
  target_link_libraries(test_config_deep_merge PRIVATE ttbox_core)
  add_test(NAME test_config_deep_merge COMMAND test_config_deep_merge)

  # 瞬时帧率计（滚动窗口）回归：面板"检测帧率缓慢爬升"是累计平均口径错，非性能问题
  add_executable(test_frame_rate_meter tests/test_frame_rate_meter.cpp)
  target_link_libraries(test_frame_rate_meter PRIVATE ttbox_core)
  add_test(NAME test_frame_rate_meter COMMAND test_frame_rate_meter)

  # S6：StatsCollector 滑动窗口回归（内存泄漏修复，不需要硬件）
  add_executable(test_stats_window tests/test_stats_window.cpp)
  target_link_libraries(test_stats_window PRIVATE ttbox_core)
  add_test(NAME test_stats_window COMMAND test_stats_window)

  # 第11阶段：合成 DFL-dist 坐标闭环测试（模型坐标 → 解码 → 原图映射，host 可跑）
  add_executable(test_decode_coords tests/test_decode_coords.cpp)
  target_link_libraries(test_decode_coords PRIVATE ttbox_core)
  add_test(NAME test_decode_coords COMMAND test_decode_coords)

  # A11：AimThread 线程生命周期与邮箱消费测试
  add_executable(test_aim_thread tests/test_aim_thread.cpp)
  target_link_libraries(test_aim_thread PRIVATE ttbox_core)
  add_test(NAME test_aim_thread COMMAND test_aim_thread)


  # P_PID 对照测试（与参考实现输出逐点一致）
  add_executable(test_pid1 tests/test_pid1.cpp)
  target_link_libraries(test_pid1 PRIVATE ttbox_core)
  add_test(NAME test_pid1 COMMAND test_pid1)

  # V1.0.43：Fitts 定律控制器单测（近慢远快/尺寸死区/px→count/纯函数/MT 钳位）
  add_executable(test_fitts_aim tests/test_fitts_aim.cpp)
  target_link_libraries(test_fitts_aim PRIVATE ttbox_core)
  add_test(NAME test_fitts_aim COMMAND test_fitts_aim)

  # V1.0.46：逐帧检测框记录器单测（CSV 列一致 / 框序列 / 关闭零开销 / 缓冲批量写）
  add_executable(test_det_trace tests/test_det_trace.cpp)
  target_link_libraries(test_det_trace PRIVATE ttbox_core)
  add_test(NAME test_det_trace COMMAND test_det_trace)

  # V1.0.46：离线回放器（★ 直接链接产品控制器头文件，不做二次翻译）
  #   之前用「Python 精确移植」回放，那是二次翻译：Python 与 C++ 两套独立实现，
  #   双方可能同时误解原始算法（V1.0.43 的 τ=MT 错误就是两边犯一样的错）。
  #   现在回放器 include 的就是上板那份 FittsAimController.hpp / Pid1Controller.hpp。
  add_executable(ttbox_replay tools/replay/replay_main.cpp)
  target_link_libraries(ttbox_replay PRIVATE ttbox_core)
  # 注意：刻意**不** install 进产品 payload（纯开发工具）。

  # V3 阶段 5：拟人化抖动前馈扣除（延迟对齐环形缓冲 + 两条链抖动上报）
  add_executable(test_jitter_feedforward tests/test_jitter_feedforward.cpp)
  target_link_libraries(test_jitter_feedforward PRIVATE ttbox_core)
  add_test(NAME test_jitter_feedforward COMMAND test_jitter_feedforward)

  # Hotkey Gate 最终输出安全边界测试（7 个验收场景）
  add_executable(test_hotkey_gate tests/test_hotkey_gate.cpp)
  target_link_libraries(test_hotkey_gate PRIVATE ttbox_core)
  add_test(NAME test_hotkey_gate COMMAND test_hotkey_gate)

  # 热键保护（hotkey_guard）测试：按 toggle 键切换「全部挂起」（9 个场景 + JSON 往返）
  add_executable(test_hotkey_guard tests/test_hotkey_guard.cpp)
  target_link_libraries(test_hotkey_guard PRIVATE ttbox_core)
  add_test(NAME test_hotkey_guard COMMAND test_hotkey_guard)

  # 拉枪曲线注入 AimThread 输出链测试（4 个验收场景）
    add_executable(test_pull_curve_aimthread tests/test_pull_curve_aimthread.cpp)
    target_link_libraries(test_pull_curve_aimthread PRIVATE ttbox_core)
    add_test(NAME test_pull_curve_aimthread COMMAND test_pull_curve_aimthread)

    # 拟人化整形引擎（第 1 项）单元测试
      add_executable(test_personal_trajectory_shader tests/test_personal_trajectory_shader.cpp)
      target_link_libraries(test_personal_trajectory_shader PRIVATE ttbox_core)
      add_test(NAME test_personal_trajectory_shader COMMAND test_personal_trajectory_shader)

      # 目标锁定确认（第 2 项：ENTER/HOLD/instant-enter）单元测试
            add_executable(test_lock_confirm tests/test_lock_confirm.cpp)
            target_link_libraries(test_lock_confirm PRIVATE ttbox_core)
            add_test(NAME test_lock_confirm COMMAND test_lock_confirm)

            # 头部瞄准约束（第 3 项）单元测试
                        add_executable(test_head_aim_constraint tests/test_head_aim_constraint.cpp)
                        target_link_libraries(test_head_aim_constraint PRIVATE ttbox_core)
                        add_test(NAME test_head_aim_constraint COMMAND test_head_aim_constraint)

                        # ByteTrack 增强（第4项：轨迹生命周期/最大上限/卡尔曼速度）单元测试
                        add_executable(test_bytetrack_upgrade tests/test_bytetrack_upgrade.cpp)
                        target_link_libraries(test_bytetrack_upgrade PRIVATE ttbox_core)
                        add_test(NAME test_bytetrack_upgrade COMMAND test_bytetrack_upgrade)

                        # 压枪引擎（recoil assist：触发/延迟/目标门控/残差/ramp/微动）单元测试
                        add_executable(test_recoil tests/test_recoil.cpp)
                        target_link_libraries(test_recoil PRIVATE ttbox_core)
                        add_test(NAME test_recoil COMMAND test_recoil)


    # 热键完全可配置化验收（默认/换键/侧键/双键/any-all/免重启生效/未按键零输出）
  add_executable(test_hotkey_config tests/test_hotkey_config.cpp)
  target_link_libraries(test_hotkey_config PRIVATE ttbox_core)
  add_test(NAME test_hotkey_config COMMAND test_hotkey_config)

  # 瞄准档位（多热键）数据层验收：命中判定 / 互斥不变量 / 闸门键位并集 /
  # 多档序列化往返 / 老配置（只有平铺 key）合成单档 / 数组优先于平铺 key
  add_executable(test_aim_profiles tests/test_aim_profiles.cpp)
  target_link_libraries(test_aim_profiles PRIVATE ttbox_core)
  add_test(NAME test_aim_profiles COMMAND test_aim_profiles)

  # 贝塞尔移动轨迹（一代分段自适应/二代随机弧线/抽签时机/弓高上限）单元测试

  # 准星找色（CrosshairProbe：RGB/BGR/RGBA/NV12 取色 + 容差 + 半径 + 红色环绕 + fail-closed）
  add_executable(test_crosshair_probe tests/test_crosshair_probe.cpp)
  target_link_libraries(test_crosshair_probe PRIVATE ttbox_core)
  add_test(NAME test_crosshair_probe COMMAND test_crosshair_probe)

  # C6：TargetSelector 防横跳压力测试（交叉/抖动/消失/排序翻转）
  add_executable(test_selector_stress tests/test_selector_stress.cpp)
  target_link_libraries(test_selector_stress PRIVATE ttbox_core)
  add_test(NAME test_selector_stress COMMAND test_selector_stress)

  # V1.0.07：贴裁剪区边界剔除（防"锁到画面外半截人猛拉一把"）+ 锁定尺寸一致性
  add_executable(test_selector_clip_guard tests/test_selector_clip_guard.cpp)
  target_link_libraries(test_selector_clip_guard PRIVATE ttbox_core)
  add_test(NAME test_selector_clip_guard COMMAND test_selector_clip_guard)

  # V1.0.11：选靶量测门控 / continuity / 切靶确认窗（治「喨上后还晃」）
  add_executable(test_selector_gating tests/test_selector_gating.cpp)
  target_link_libraries(test_selector_gating PRIVATE ttbox_core)
  add_test(NAME test_selector_gating COMMAND test_selector_gating)

  # V1.0.08/09：框底被裁剪区截断时的落点外推（治「走近目标落点跑到头上」）
  add_executable(test_aim_point_clip tests/test_aim_point_clip.cpp)
  target_link_libraries(test_aim_point_clip PRIVATE ttbox_core)
  add_test(NAME test_aim_point_clip COMMAND test_aim_point_clip)

  # C10/C11/C14：生命周期压力 + RuntimeConfig 并发读写压测
  add_executable(test_lifecycle_stress tests/test_lifecycle_stress.cpp)
  target_link_libraries(test_lifecycle_stress PRIVATE ttbox_core)
  add_test(NAME test_lifecycle_stress COMMAND test_lifecycle_stress)

  # R5/R6：启动意图裁决（更新后停不停跑）纯函数测试
  add_executable(test_startup_intent tests/test_startup_intent.cpp)
  target_link_libraries(test_startup_intent PRIVATE ttbox_core)
  add_test(NAME test_startup_intent COMMAND test_startup_intent)

  # 预览裁剪矩形：跟随 RuntimeProfile::capture（面板「截取尺寸」）的纯函数测试
  add_executable(test_preview_roi tests/test_preview_roi.cpp)
  target_link_libraries(test_preview_roi PRIVATE ttbox_core)
  add_test(NAME test_preview_roi COMMAND test_preview_roi)

  # A11：FOV 分辨率与方向换算测试
  add_executable(test_fov_angle tests/test_fov_angle.cpp)
  target_link_libraries(test_fov_angle PRIVATE ttbox_core)
  add_test(NAME test_fov_angle COMMAND test_fov_angle)

  add_executable(test_detection_geometry_filter tests/test_detection_geometry_filter.cpp)
  target_link_libraries(test_detection_geometry_filter PRIVATE ttbox_core)
  add_test(NAME test_detection_geometry_filter COMMAND test_detection_geometry_filter)

  add_executable(test_coordinate_transform tests/test_coordinate_transform.cpp)
  target_link_libraries(test_coordinate_transform PRIVATE ttbox_core)
  add_test(NAME test_coordinate_transform COMMAND test_coordinate_transform)

  # 第13阶段：完整 Pipeline 单元测试（无检测/单/多/越界/NaN/低置信度/坐标/Controller/连续帧/消失）
  add_executable(test_pipeline tests/test_pipeline.cpp)
  target_link_libraries(test_pipeline PRIVATE ttbox_core)
  add_test(NAME test_pipeline COMMAND test_pipeline)

  # 第15阶段：AimTracker + PidController 运动场景测试（静止/匀速/加速/减速/急停/反转/切换/短暂丢失/完全丢失）
  add_executable(test_tracker tests/test_tracker.cpp)
  target_link_libraries(test_tracker PRIVATE ttbox_core)
  add_test(NAME test_tracker COMMAND test_tracker)

  # 第13阶段：Pipeline 模块分项耗时基准（Selector/Coordinate/Controller，host+板端均可跑）
  add_executable(pipeline_bench tests/pipeline_bench.cpp)
  target_link_libraries(pipeline_bench PRIVATE ttbox_core)

  # OutputBackend 统一接口测试（纯 C++，host 可测；Gate/选择/生命周期/零移动）
  add_executable(test_output_backend tests/test_output_backend.cpp)
  target_link_libraries(test_output_backend PRIVATE ttbox_core)
  add_test(NAME test_output_backend COMMAND test_output_backend)

  add_executable(test_mouse_control_client tests/test_mouse_control_client.cpp)
  target_link_libraries(test_mouse_control_client PRIVATE ttbox_core)
  add_test(NAME test_mouse_control_client COMMAND test_mouse_control_client)

  add_executable(test_personal_motion tests/test_personal_motion.cpp)
  target_link_libraries(test_personal_motion PRIVATE ttbox_core)
  add_test(NAME test_personal_motion COMMAND test_personal_motion)

  add_executable(test_preprocess tests/test_preprocess.cpp)
  target_link_libraries(test_preprocess PRIVATE ttbox_core)
  # 独立最小链路测试（Capture frame → Preprocess，host 可跑，无需硬件）。
  # ★ item 3：已实测 host PASS（`frame=1 detect_size=2x2 ...` + `test_preprocess: PASS`，exit 0）
  #   ⇒ 纳入 CTest 验收集合。
  # 旧注释"保持既有 CTest 验收集合 14 项不变"口径作废：该数字随清单演进必然陈旧
  # （架构师"去硬编码计数"规则，勿以注释锁死计数）——验收集合以 CTest **实际注册**为准。
  add_test(NAME test_preprocess COMMAND test_preprocess)

  add_executable(test_model_backend tests/test_model_backend.cpp)
  target_link_libraries(test_model_backend PRIVATE ttbox_core)
  add_test(NAME test_model_backend COMMAND test_model_backend)
  add_executable(test_model_runtime_state tests/test_model_runtime_state.cpp)
  target_include_directories(test_model_runtime_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(test_model_runtime_state PRIVATE ttbox_core)
  add_test(NAME test_model_runtime_state COMMAND test_model_runtime_state)

  # RKNN Engine 失败注入（纯状态门+hook，不依赖真硬件，双平台可跑）
  add_executable(test_rknn_failure_injection tests/test_rknn_failure_injection.cpp)
  target_link_libraries(test_rknn_failure_injection PRIVATE ttbox_core)
  add_test(NAME test_rknn_failure_injection COMMAND test_rknn_failure_injection)

  # G1 metrics 测试：实例化 CoreRuntime（依赖 V4L2Capture/WorkerPool，仅板端编译）
  if(UNIX AND NOT APPLE AND TTBOX_CORE_HAS_RKNN)
    add_executable(test_metrics_g1 tests/test_metrics_g1.cpp)
    target_link_libraries(test_metrics_g1 PRIVATE ttbox_core)
    add_test(NAME test_metrics_g1 COMMAND test_metrics_g1)
  endif()

  # 面板字段真源回归：只依赖 IpcServer::system_status_to_json 与 Metrics（无硬件依赖）
  add_executable(test_preprocess_metric tests/test_preprocess_metric.cpp)
  target_link_libraries(test_preprocess_metric PRIVATE ttbox_core)
  add_test(NAME test_preprocess_metric COMMAND test_preprocess_metric)

  # ---- 硬件验收测试（板端：-DTTBOX_CORE_BUILD_HW_TESTS=ON）----
  if(TTBOX_CORE_BUILD_HW_TESTS AND UNIX AND NOT APPLE)
    add_executable(test_capture_hw tests/test_capture_hw.cpp)
    target_include_directories(test_capture_hw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(test_capture_hw PRIVATE ttbox_core)
    add_test(NAME test_capture_hw COMMAND test_capture_hw)

    if(TTBOX_CORE_HAS_RGA)
      add_executable(test_rga_hw tests/test_rga_hw.cpp)
      target_include_directories(test_rga_hw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_rga_hw PRIVATE ttbox_core)
      add_test(NAME test_rga_hw COMMAND test_rga_hw)
    endif()

    if(TTBOX_CORE_HAS_RKNN)
      add_executable(test_rknn_hw tests/test_rknn_hw.cpp)
      target_include_directories(test_rknn_hw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_rknn_hw PRIVATE ttbox_core)
      # ctest 项使用短帧数快速回归；完整吞吐测试见 A-4 验收（--frames 300）
      add_test(NAME test_rknn_hw COMMAND test_rknn_hw --mode raw --model /opt/ttbox2/models/yolo261n-rk3588.rknn --frames 30)

      # A11：旧独立 Worker 入口已移除。硬件链路由 HardwareRunner 验收入口承载。

      # A11：HardwareRunner 只构建验证生命周期，不自动访问设备。
      add_executable(test_hardware_runner tests/test_hardware_runner.cpp)
      target_include_directories(test_hardware_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_hardware_runner PRIVATE ttbox_core)

      add_executable(test_decode_align tests/test_decode_align.cpp)
      target_include_directories(test_decode_align PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_decode_align PRIVATE ttbox_core)

      # A-7 ModelAdapter / Decoder / Runtime 生命周期测试
      add_executable(test_model_adapter tests/test_model_adapter.cpp)
      target_include_directories(test_model_adapter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_model_adapter PRIVATE ttbox_core)
      add_test(NAME test_model_adapter COMMAND test_model_adapter
               --model /opt/ttbox2/models/yolo261n-rk3588.rknn --color bgr --expect single --classes 80)

      add_executable(test_model_runtime tests/test_model_runtime.cpp)
      target_include_directories(test_model_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_model_runtime PRIVATE ttbox_core)
      # ★ item 12：A-7 模型加载生命周期（板端 RKNN）——本次纳入 CTest 注册。
      #   用法 `--models "model:color:expect:classes,..."`；模型与 test_model_adapter 一致。
      add_test(NAME test_model_runtime COMMAND test_model_runtime
               --models /opt/ttbox2/models/yolo261n-rk3588.rknn:bgr:single:80)

      # =========================================================================
      # ★ item 12 处置表：本文件所有"有 add_executable 但（曾）无 add_test"的目标。
      #   逐项给出【是否注册 / 理由 / 域】，杜绝"有二进制却从不跑、且一声不响"的静默。
      #   下列目标均位于 TTBOX_CORE_BUILD_HW_TESTS AND UNIX AND NOT APPLE 或 tools/ ⇒
      #   host（Windows/msys2）不编译，故不进入 host CTest 计数。
      #   · test_model_runtime.cpp    → ✅ 本次注册      A-7 加载/卸载生命周期              域=板端
      #   · test_hardware_runner.cpp  → ❌ 故意不注册    注释明文"只构建验证生命周期、不自动
      #                                              访问设备"，无自检退出语义           域=板端
      #   · test_decode_align.cpp     → ❌ 故意不注册    需真实 .rknn 模型 + 板端 RKNN     域=板端
      #   · test_model_switch_hw.cpp  → ❌ 故意不注册    A-8 模型仓切换/热更新、需设备/模型仓 域=板端
      #   · test_rga_roi_hw.cpp       → ❌ 故意不注册    A-8 RGA ROI 裁剪、需 RGA 设备      域=板端
      #   · test_hid_forward_hw.cpp   → ❌ 故意不注册    A-9 HID 透传、需 hidraw→hidg 设备   域=板端
      #   · test_hid_load_sim.cpp     → ❌ 故意不注册    A-9 合成高频负载模拟（压测，非断言单测）域=板端
      #   · test_hid_loopback.cpp     → ❌ 故意不注册    A-9 转发器回环、需 fifo+hidg 设备   域=板端
      #   非 test_ 命名的目标（一并登记，避免同类静默）：
      #   · pipeline_bench.cpp        → ❌ 非断言      分项耗时基准（Selector/Coord/Controller）域=host+板端基准
      #   · tools/ttbox_hid_test.cpp / ttbox_hid_health.cpp / ttbox_hid_pkg.cpp /
      #     tools/ttbox_hid_forward.cpp → ❌ 非测试：CLI 工具（tools/）              域=工具
      # =========================================================================

      # A-8 模型仓库生命周期 + 切换 + 热更新
      add_executable(test_model_switch_hw tests/test_model_switch_hw.cpp)
      target_include_directories(test_model_switch_hw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_model_switch_hw PRIVATE ttbox_core)

      # A-8 RGA ROI 硬件裁剪
      add_executable(test_rga_roi_hw tests/test_rga_roi_hw.cpp)
      target_include_directories(test_rga_roi_hw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_rga_roi_hw PRIVATE ttbox_core)

      # A-9 HID 透传（hidraw → hidg）
      add_executable(test_hid_forward_hw tests/test_hid_forward_hw.cpp)
      target_include_directories(test_hid_forward_hw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_hid_forward_hw PRIVATE ttbox_core)

      # A-9 独立测试程序 ttbox-hid-test
      add_executable(ttbox-hid-test tests/ttbox_hid_test.cpp)
      target_include_directories(ttbox-hid-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(ttbox-hid-test PRIVATE ttbox_core)

      # A-9 合成 HID 高频负载模拟（CPU affinity / AI 隔离实验）
      add_executable(test_hid_load_sim tests/test_hid_load_sim.cpp)
      target_include_directories(test_hid_load_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_hid_load_sim PRIVATE ttbox_core)

      # A-9 转发器回环测试（fifo → hidg）
      add_executable(test_hid_loopback tests/test_hid_loopback.cpp)
      target_include_directories(test_hid_loopback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      target_link_libraries(test_hid_loopback PRIVATE ttbox_core)

      # A-9-P2 HID Package 健康检查工具
      add_executable(ttbox-hid-health tools/ttbox_hid_health.cpp)
      target_include_directories(ttbox-hid-health PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
      target_link_libraries(ttbox-hid-health PRIVATE ttbox_core)

      # A-9-P2 HID Package Registry CLI
      add_executable(ttbox-hid-pkg tools/ttbox_hid_pkg.cpp)
      target_include_directories(ttbox-hid-pkg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
      target_link_libraries(ttbox-hid-pkg PRIVATE ttbox_core)

      # A-9-P2 常驻 HID 透传 daemon（systemd: ttbox-hid-forward.service）
      add_executable(ttbox-hid-forward tools/ttbox_hid_forward.cpp)
      target_include_directories(ttbox-hid-forward PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
      target_link_libraries(ttbox-hid-forward PRIVATE ttbox_core)
    endif()
  endif()
endif()

