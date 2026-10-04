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
project(ttbox_core VERSION 1.0.20 LANGUAGES CXX)

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
    TTBOX_RKNN_API_HEADER="${TTBOX_RKNN_API_HEADER_PATH}")
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

  # 几何配对识头：落点取「头部小框」正中心（不依赖类别号，训练场单人双框场景）
  add_executable(test_aim_head_box tests/test_aim_head_box.cpp)
  target_link_libraries(test_aim_head_box PRIVATE ttbox_core)
  add_test(NAME test_aim_head_box COMMAND test_aim_head_box)

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

