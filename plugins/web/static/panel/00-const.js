// ==========================================================================
// 00-const.js —— 面板「全部 59 个 const/let/var 声明」
// ==========================================================================
// ★ 加载顺序：**第一个**，排在 10-flow.js 之前。不可调整。
//   原因：JS 的 const/let **不hoist**（不存在函数声明那样的提升），
//   放后面会让 10-flow.js 与 01..09 取到它们时抛
//   ReferenceError: Cannot access 'X' before initialization（TDZ 暂时性死区）。
//
// ★ 本文件装什么：只有「状态与常量」，一行逻辑都没有。
//   1 状态对象 state（面板全部可变状态）
//   2 配置默认值（OVERVIEW_DEFAULTS / CONTROLLER_DEFAULTS / RECOIL_DEFAULTS 等）
//   3 枚举与映射表（热键、显示模式、裁剪尺寸、区间绑定等）
//   4 存储键与超时（THEME_STORAGE_KEY / *_TIMEOUT_MS）
//
// ★ 本段内部无任何函数调用（只有字面量、正则、内建 Math/Date/Set），
//   所以它不依赖 10-flow.js 里的任何函数，可以安全地排在前面。
//
// ★ 加新常量放哪：
//   · 全局共享的状态/默认值/表→ 本文件
//   · 只属于某个页签的常量    → 那个页签自己的文件
// ==========================================================================
const HOTKEYS = ["left", "right", "middle", "back", "forward", "auto"];
const POINTER_HOTKEYS = ["left", "right", "middle", "back", "forward"];
const OPTIONAL_POINTER_HOTKEYS = ["", ...POINTER_HOTKEYS];
// 鼠标五键位掩码 —— 必须与后端 HOTKEY_BITS（ttbox-web.py）和 core
// MouseTypes.hpp 的 AimHotkeyProfile 注释同域，否则档位重叠校验的口径会漂。
const AIM_HOTKEY_BITS = { left: 1, right: 2, middle: 4, back: 8, forward: 16 };
// 档位数上限，与后端 AIM_PROFILE_MAX 同值。
const AIM_PROFILE_MAX = 8;
const HOTKEY_LABELS = {
  left: "左键",
  right: "右键",
  middle: "中键",
  back: "后侧键",
  forward: "前侧键",
  auto: "自动",
  "": "不使用",
};
const STATUS_LABELS = {
  running: "运行中",
  stopped: "已停止",
  starting: "启动中",
  stopping: "停止中",
  reconnecting: "采集重连中",
  error: "异常",
  idle: "空闲",
  degraded: "采集降级",
  locked: "未激活",
};
const AIM_CLASS_MAX_COUNT = 30;
const AIM_CLASS_ALL_MASK = (1 << AIM_CLASS_MAX_COUNT) - 1;
const AIM_PROFILE_AXIS_OFFSET_MIN = 0;
const AIM_PROFILE_AXIS_OFFSET_MAX = 1;
const AIM_PROFILE_FOV_SCALE_MIN = 0.1;
const AIM_PROFILE_FOV_SCALE_MAX = 1;
// V1.0.12（2026-09-30）：倍镜倍率 / 本档 px/count 两个控件已删（不区分倍镜）。
// 总览「FOV 半径」= 内接圆倍率，下限与后端 FOV_FACTOR_MIN（ttbox-web.py）保持一致：
// 倍率 0 会让 core 的 fov.radius=0 撞上「必须在 (0,1]」校验，整个保存失败。
const OVERVIEW_FOV_FACTOR_MIN = 0.1;
const CROP_SIZE_OPTIONS = [192, 256, 320, 416, 640];
const CROP_SIZE_MIN = 1;
const CROP_SIZE_MAX = 1080;
// V1.0.13（2026-09-30）：CAPTURE_CROP_OFFSET_* / AIM_REFERENCE_OFFSET_MAX /
// DYNAMIC_OFFSET_INPUT_IDS 连同 dynamicNumericRangeLimitsForId /
// updateDynamicOffsetControlLimits 一起删除 —— 它们只服务于那四个已删的偏移控件
// （裁剪区偏移范围、准星偏移范围随截取尺寸伸缩）。现在裁剪区恒居中、准星恒在中心，
// 没有需要"动态改 min/max"的输入框了。
const AIM_OVERLAY_CONFIG_IDS = new Set([
  "capture_crop_size",
  "range_factor",
]);
const RANGE_BINDINGS = [
  ["video_detection_confidence", "video_detection_confidence_range"],
  ["video_detection_iou", "video_detection_iou_range"],
  ["sens", "sens_range"],
  ["range_factor", "range_factor_range"],
];
const NUMERIC_RANGE_LIMITS = {
  capture_crop_size: [CROP_SIZE_MIN, CROP_SIZE_MAX],
  sens: [0, 5],
  aim_profile_sensitivity: [0.1, 3],
  aim_profile_fov_scale: [AIM_PROFILE_FOV_SCALE_MIN, AIM_PROFILE_FOV_SCALE_MAX],
  aim_profile_offset_x: [AIM_PROFILE_AXIS_OFFSET_MIN, AIM_PROFILE_AXIS_OFFSET_MAX],
  aim_profile_offset_y: [AIM_PROFILE_AXIS_OFFSET_MIN, AIM_PROFILE_AXIS_OFFSET_MAX],
  range_factor: [OVERVIEW_FOV_FACTOR_MIN, 1],
  // ★ V1.0.13：kp/kd 改为**真实有效值**（旧的 15/25 是被「削弱强度」砍掉 99% 的名义值）。
  //   上限按离线扫频收敛（core/tools/pid_sim）：51ms 回路 + 144fps 下 kp_eff > 0.3 必自激。
  //   留到 1.0 是为了容纳低灵敏度玩家的 gain；一旦超过 0.3 就该往下调，不是往上。
  controller_kp: [0, 1],
  controller_kd: [0, 2],
  controller_predict: [0, 2],
  controller_predict_y: [0, 1],
  controller_rate: [0, 1],
  controller_output_deadzone: [0, 20],
  controller_pull_curve_strength: [0, 2],
  controller_pull_curve_min_distance: [0, 1280],
  controller_selector_lost_grace_ms: [0, 2000],
  hailo_pipeline_depth: [1, 4],
};
const OVERVIEW_DEFAULTS = {
  capture_crop_size: 320,
  range_factor: 1,
  video_detection_confidence: 0.25,
  video_detection_iou: 0.45,
};

const CONTROLLER_DEFAULTS = {
  // 2026-09-20 定案：pid1.cpp 的 P_PID 为唯一标准，面板只暴露它的这几个参数。
  // ★ 2026-09-24 修正：原先把 kp/kd/predict/rate/smooth 五项**同时写 _x 与 _y**，
  //   回填只取 X ⇒ 面板上调 X 的预判会把 Y 一起改成同一个值。而 pid1.cpp main() 里
  //   X/Y 的唯一差别恰恰就是 predict ⇒ predict_y 必须独立。
  //   现在：Kp / Kd / Rate 两轴共用（参考实现里两轴本来就同值），
  //         predict_x / predict_y 各自独立。
  // ★ V1.0.13（2026-09-30）：三项默认值改成与 core 结构体一致的**真实值**，并删掉 smooth：
  //   旧 kp/kd=25 是"被 smooth 削掉 99% 之后的名义值"（生效 0.25），旧 predict=3.0 是
  //   pid1 原版值、在我们这套 51ms 回路下会自激。现在面板显示的就是接进环路的那个数。
  kp: 0.25,
  kd: 0.25,
  predict: 1,
  predict_y: 0,
  rate: 0.3,
  output_deadzone: 1,
  pull_curve_enabled: true,
  pull_curve_strength: 0.8,
  pull_curve_min_distance: 80,
  selector_lost_grace_ms: 78,
};
// ---------------------------------------------------------------------------
// BB 对标模块表（2026-09-24 面板收敛）
// ★ 与 plugins/web/bin/ttbox-web.py 的 CTRL_BLOCKS /
//   CTRL_SELECTOR_FIELDS **同源**：下面这张表由后端表机械生成
//   字段名 / 类型 / Core 默认值逐项一致。改后端表必须同步重生成，
//   否则「面板回填显示的值」会与「Core 实际值」对不上（首次打开的设备尤其明显）。
// 每项 = [模块前缀, [[字段名, 类型, Core 默认值], ...]]
//   元素 id = 提交键名 = 前缀 + "_" + 字段名（后端按同样的规则取 body.ai.controller）。
//   类型：b = 勾选框，n = 小数，i = 整数，key = 按键字符串。
// ★ 表内**禁止写行注释**：tests/test_web_panel_bb_sync.py 会把这段当 JSON 解析来核对
//   前后端同源，注释会让解析直接炸（2026-09-26 实测）。要注释就写在本注释块里。
// ---------------------------------------------------------------------------
const BB_CTRL_MODULES = [
  ["trigger2", [
    ["enabled", "b", false],
    ["key1", "key", 16],
    ["key2", "key", 0],
    ["fire_button", "key", 1],
    ["with_aim", "b", true],
    ["with_simple_recoil", "b", false],
    ["confidence", "n", 0.5],
    ["first_err", "n", 30.0],
    ["first_delay", "n", 0.0],
    ["fire_interval", "n", 1.0],
    ["fire_random", "n", 0.0],
    ["fire_count", "i", 1],
    ["press_duration", "n", 50.0],
    ["move_throttle_frames", "i", 2],
    ["precision_enabled", "b", false],
    ["precision_range", "n", 10.0],
    ["precision_frames", "i", 5],
    ["retarget_reset_ms", "n", 1000.0],
    ["stop_detect_enabled", "b", false],
    ["stop_detect_color_id", "i", 2],
    ["stop_detect_tolerance", "n", 60.0],
    ["stop_detect_range", "n", 80.0],
    ["stop_detect_interval", "i", 10],
  ]],
  ["recoil", [
    ["only_when_target_visible", "b", true],
    ["target_lost_release_ms", "n", 300.0],
    ["trigger_delay_enabled", "b", false],
    ["trigger_delay_ms", "n", 120.0],
    ["strength", "n", 100.0],
    ["speed", "n", 1.0],
    ["curve_strength", "n", 0.6],
    ["roi_h", "n", 300.0],
  ]],
  ["selector", [
    ["lock_hold_ms", "n", 0.0],
    ["switch_hysteresis", "n", 0.5],
    ["switch_cooldown_ms", "n", 600.0],
    ["priority_scoring", "b", false],
    ["weight_dist", "n", 1.0],
    ["weight_size", "n", 0.3],
    ["stickiness", "n", 1.0],
    ["switch_threshold_px", "n", 60.0],
    ["head_body_stable", "b", false],
    ["hb_body1", "i", 0],
    ["hb_head1", "i", 1],
    ["hb_body2", "i", -1],
    ["hb_head2", "i", -1],
  ]],
];

// 每个分区管哪些模块（「恢复本页默认值」按这张表取值）。
const BB_SECTION_MODULES = {
  "assist-section-recoil": ["recoil"],
  "assist-section-trigger": ["trigger2"],
  "assist-section-selector": ["selector"],
};

const MOVEMENT_CONTROL_DEFAULTS = {
  sens: 1,
  controller: CONTROLLER_DEFAULTS,
};
const RECOIL_DEFAULTS = {
  // 压枪面板的共用参数（开关 + 触发键）；算法参数（strength/speed/roi_h/
  // curve_strength/门控）在 BB_CTRL_MODULES 的 recoil 块里，两边一起构成
  // mouse.recoil 这个子对象。
  enabled: false,
  hotkey: "left",
  hotkey2: "",
  hotkey_mode: "any",
};
const HOTKEY_GUARD_DEFAULTS = {
  enabled: false,
  toggle_hotkey: "middle",
};
const DISPLAY_NATIVE_MODES = [
  "",
  "1080p60compat",
  "1080p60",
  "1080p90",
  "1080p120compat",
  "1080p120",
  "1080p144",
  "1080p240compat",
  "1080p240",
  "1440p60",
  "1440p120",
  "1440p144",
  "2160p60",
];
const DISPLAY_NATIVE_MODE_LABELS = {
  "": "自动使用 EDID 模式",
  "1080p60compat": "1920x1080 @ 60 Hz 兼容",
  "1080p60": "1920x1080 @ 60 Hz",
  "1080p90": "1920x1080 @ 90 Hz",
  "1080p120compat": "1920x1080 @ 120 Hz 兼容",
  "1080p120": "1920x1080 @ 120 Hz",
  "1080p144": "1920x1080 @ 144 Hz",
  "1080p240compat": "1920x1080 @ 240 Hz 兼容",
  "1080p240": "1920x1080 @ 240 Hz",
  "1440p60": "2560x1440 @ 60 Hz",
  "1440p120": "2560x1440 @ 120 Hz",
  "1440p144": "2560x1440 @ 144 Hz",
  "2160p60": "3840x2160 @ 60 Hz",
};
const DISPLAY_IDENTITY_FIELD_IDS = ["display_name", "display_vendor", "display_product_id", "display_serial"];
const DISPLAY_EDID_MODE_FIELD_IDS = ["displayEdidModeWidth", "displayEdidModeHeight", "displayEdidModeRefresh"];
const DISPLAY_DYNAMIC_MODE_RE = /^(\d{3,4})x(\d{3,4})@(\d{2,3})$/;
const DISPLAY_DYNAMIC_MODE_MIN_WIDTH = 640;
const DISPLAY_DYNAMIC_MODE_MAX_WIDTH = 4095;
const DISPLAY_DYNAMIC_MODE_MIN_HEIGHT = 400;
const DISPLAY_DYNAMIC_MODE_MAX_HEIGHT = 4095;
const DISPLAY_DYNAMIC_MODE_MIN_REFRESH = 24;
const DISPLAY_DYNAMIC_MODE_MAX_REFRESH = 360;
const DISPLAY_DYNAMIC_MODE_MIN_PIXEL_CLOCK_KHZ = 25000;
const DISPLAY_DYNAMIC_MODE_MAX_PIXEL_CLOCK_KHZ = 600000;
const DISPLAY_DYNAMIC_MODE_H_BLANK = 48 + 32 + 80;
const DISPLAY_DYNAMIC_MODE_V_BLANK = 3 + 5 + 77;
const DISCLAIMER_STORAGE_KEY = "aiassistance_disclaimer_ack_v1";
const THEME_STORAGE_KEY = "aiassistance_theme";
// 2026-09-20：刷新/重新打开后回到上次停留的页签（原来没有持久化，刷新必回首页）
const ACTIVE_PAGE_STORAGE_KEY = "aiassistance_active_page";
const UI_BRAND_YU = "yu";
const UI_BRAND_XH = "xh";
const UI_BRAND_XCSH = "xcsh";
const MOUSE_MODE_SWITCH_SUPPRESS_MS = 180000;

const state = {
  data: null,
  config: null,
  configReady: false,
  isPopulating: false,
  isApplying: false,
  applyQueued: false,
  applyTimer: null,
  modelListSignature: "",
  modelCardsRenderSignature: "",
  // ★ 2026-10-03 性能 B：三个渲染守卫的签名（防止每 1.5s 轮询重建 DOM）
  modelGameFiltersSignature: "",
  modelBackendFiltersSignature: "",
  modelPanelSignature: "",
  brandRenderSignature: "",
  presetCleanupButtonSignature: "",
  // ★ 2026-10-03 交互延迟：启停请求在途时按钮显示的临时文字（空=不在途）
  runtimeControlBusyText: "",
  modelGameFilter: "all",
  modelBackendFilter: "all",
  modelGameOptions: [],
  modelGameSuggestionOpen: false,
  presetNames: [],
  presetSelectedName: "",
  presetListSignature: "",
  presetSuggestionOpen: false,
  presetAutoSaveInFlight: false,
  presetAutoSaveQueued: false,
  presetAutoSaveName: "",
  presetAutoSaveConfig: null,
  modelPresetBindingOpenId: "",
  modelClassNamesEditModelId: "",
  aimClassRenderSignature: "",
  updatePlan: null,
  updateStatus: null,
  updateStatusTimer: null,
  updateStatusInFlight: false,
  updateCompletionTimer: null,
  updateRefreshScheduled: false,
  // 安装已提交、更新器尚未写出 RUNNING 的窗口闩（毫秒时间戳）。期间端点回 idle 一律忽略。
  updateSessionDeadline: 0,
  // 本页打开时刻（秒）。板端 ota_status.json 装完后永久停在 SUCCESS，
  // 只有 finished_at 晚于本页打开时刻的 SUCCESS 才算「本次会话刚升完」，
  // 其余一律当 idle 处理（2026-09-22 业主定案：进度条只认本次会话的升级）。
  updatePageOpenedAtSec: Math.floor(Date.now() / 1000),
  licenseRecoveryInProgress: false,
  lastLicenseRecoveryMessage: "",
  displayRealMonitor: null,
  displayHardwarePayload: null,
  storageExpandBusy: false,
  systemHostname: "",
  webPort: 8080,
  currentVersion: "",
  activePageId: "home-page",
  activeControlSectionId: "control-section-pid",
  calibKeepPolling: false,
  activeAssistSectionId: "assist-section-recoil",
  navigationLockedToLicense: false,
  licenseStatusLoaded: false,
  mouseModeSwitchSuppressUntil: 0,
  uiBrand: UI_BRAND_YU,
  allowThemeSwitch: true,
  theme: "dark",
  livePollTimer: null,
  livePollInFlight: false,
  livePollErrorNotified: false,
  // 热键档位冲突的最近一次提示文案。自动保存会被反复触发，
  // 拿它去重，免得同一句「热键冲突」弹一屏。
  aimProfileConflictNotified: "",
  activationRedirecting: false,
};

const LICENSE_GATE_PAGE_ID = "license-page";


let authConfirmInFlight = false;
const RANGE_THUMB_SIZE_PX = 20;
const RANGE_THUMB_HIT_SLOP_PX = 14;

const ASSIST_SECTION_EXTRA_DEFAULTS = {
  "assist-section-recoil": () => ({
    recoil_enabled: RECOIL_DEFAULTS.enabled,
    recoil_hotkey: RECOIL_DEFAULTS.hotkey,
    recoil_hotkey2: RECOIL_DEFAULTS.hotkey2,
    recoil_hotkey_mode: RECOIL_DEFAULTS.hotkey_mode,
  }),
  "assist-section-lead": () => ({
    controller_pull_curve_enabled: CONTROLLER_DEFAULTS.pull_curve_enabled,
    controller_pull_curve_strength: CONTROLLER_DEFAULTS.pull_curve_strength,
    controller_pull_curve_min_distance: CONTROLLER_DEFAULTS.pull_curve_min_distance,
  }),
};

const HOSTNAME_PATTERN_FOR_UI = /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/;

const UPDATE_SESSION_TIMEOUT_MS = 180000;
let updateRenderKey = "";
const OTA_REFRESH_SEEN_PREFIX = "ota_refresh_seen_";
