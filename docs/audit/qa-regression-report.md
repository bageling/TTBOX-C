# QA 回归测试报告 —— TTBOX Web 面板迁 C++

- 日期：2026-10-06
- QA：Edward（严过关）
- 基线：`main-335f4114`（V1.0.46「代码全 C++」）
- 对象：`plugins/web` Flask 后端 → `core/src/web/` cpp-httplib 服务
- 结论速览：**编译 PASS / 86 路由黑盒 86/86 PASS / 金丝雀 PASS**；契约抽查发现 2 处**轻微契约漂移**（core 离线错误形状），均不影响前端功能，但违反「JSON 契约一字不改」铁律，建议交 Engineer 修复或由主理人拍板。

---

## 一、测试执行摘要

| 项 | 结果 | 证据 |
|---|---|---|
| 编译（WSL `cmake --build build-web --target ttbox_web`） | **PASS** | `[100%] Built target ttbox_web`，GNU 11.4.0，TTBOX_CORE_BUILD_TESTS=OFF / AUTH=OFF |
| 86 路由黑盒（`scripts/web_route_blackbox.py`） | **86/86 PASS** | 输出「86 通过 / 0 失败，URL 零漂移」 |
| 标定拟合数学金丝雀（`calibration_math_canary.cpp`） | **PASS** | `calibration_math_canary: ALL PASS`，exit 0 |

> 说明：`core/tests/` 下原 C++ 单测不在本次回归范围（web 构建用 `-DTTBOX_CORE_BUILD_TESTS=OFF`，且 team-lead 的「复现清单」仅列编译 + 黑盒 + 金丝雀三项）。原 C++ 单测与本迁移无直接交集，未跑。

---

## 二、关键契约抽查结果（core 离线实测）

抽查方法：起 `ttbox_web`（`TTBOX_IPC_SOCKET` 指向不存在路径 ⇒ core 离线），对关键端点打 HTTP 并逐字段核对返回体（完整返回体见本报告附注与 `qa_contract_check.py` 运行日志）。

### 1. `GET /api/health` → HTTP 200
```json
{"data": {"backend": "cpp", "core_reachable": false, "core_status": 3}, "ok": true}
```
- **判定：符合契约**。`{ok:true, data:{backend:"cpp", core_reachable, core_status}}` 三字段齐全；core 离线时 `core_reachable=false`（诚实）。
- 注：`/api/health` 是 C++ 新增探针（86 条路由快照之外），属**增量**，不构成契约破坏。

### 2. `GET /api/license`（core 离线）→ HTTP 200
返回 `data.license` 关键字段：
```json
"license": {"activated": false, "state": "unactivated", "valid": false,
            "capabilities": {"ota": false, "aim": false, ...}, ...}
```
- **判定：符合契约，不假绿**。`activated:false / state:"unactivated" / valid:false` 三要素正确；`capabilities.ota=false` 顺带解释了 `/api/ota/install` 的 403（见下）。

### 3. `GET /api/system` → HTTP 200
返回 `data` 包含：`hostname` / `lan_ipv4` / `load_average[3]` / `memory` / `storage` / `os:"Orange Pi 1.2.0"` / `web_port` / `temperature` 等。
- **判定：符合契约**。逐字段与 `plugins/web/lib/sysinfo.py::collect_system_stats`（sysinfo.py:161-177）一一对应，`os` 值一致（"Orange Pi 1.2.0"），`app_version` 值一致（"ttbox-0.1.0"）。

### 4. 信封规范 `{ok:bool, data:..., error?:str, code?:str}`
抽查 14 个端点（health/license/system/storage/state/events/config/ota·install/update·install/reboot/poweroff/storage·expand/reactivate/calibration·start）：
- **判定：基本符合**。成功与失败响应均含 `ok` + `data`，错误路径含 `error`，`code` 仅在有错误码时出现。
- **唯一例外**：core 离线错误用了 `code:"core_offline"`（字符串），而 Python 原版用 `core_offline:true`（布尔，无 `data`/`code`）——见「四、发现的问题」#1。

### 5. 其它抽查要点
- `GET /api/state`（core 离线）→ 503 `{ok:false, data:{}, error:"Core 未运行…", code:"core_offline"}`：**诚实 fail-loud**，无静默回落。
- `POST /api/ota/install`（core 离线）→ 403 `"feature 'ota' not licensed"`：来自 handler 的授权能力校验，**非** D1 来源校验误杀（见第三节）。
- `POST /api/models/device-code` → `data.code="AIMK1_…"` 结构：与 `plugins/web/api/models.py:209-214` 一致（`AIMK1_` 前缀 + 去 `-` 截断 40）。
- 前端 `10-flow.js:670` `api()` 返回 `data.data`（解信封），故 `result.code` 取的是内层 `code`，C++ 侧 `models_meta.cpp:42` 正确落在 `data.code`。

---

## 三、安全回归结论（D1 来源校验）

### 3.1 代码审查结论：**逻辑正确**

`core/src/web/WebServer.cpp:141-159` `is_trusted_source`：
- `::1` → 放行（IPv6 loopback）
- `::ffff:x.x.x.x` → 先还原为 IPv4 再判
- `127.0.0.0/8` → `(ip & 0xFF000000) == 0x7F000000` ✅
- `10.0.0.0/8` → `(ip & 0xFF000000) == 0x0A000000` ✅
- `172.16.0.0/12` → `(ip & 0xFFF00000) == 0xAC100000` ✅（覆盖 172.16~172.31）
- `192.168.0.0/16` → `(ip & 0xFFFF0000) == 0xC0A80000` ✅
- 其余（公网 IPv4 / 其它 IPv6 / 非法地址）→ 拒绝 ✅

`parse_ipv4`（WebServer.cpp:21-47）逐字节校验：非法字符、>255 的段、非 4 段、空段均返回 false，无注入/绕过面。

特权端点登记（WebServer.cpp:67-73）与方案 §5.4 清单**完全一致**的 7 条：
`POST /api/ota/install`、`/api/update/install`、`/api/system/reboot`、`/api/system/poweroff`、`/api/system/storage/expand`、`/api/system/reactivate`、`/api/control/calibration/start`。

`enforce_gate`（WebServer.cpp:165-175）：命中特权端点且非可信来源 → 403 `{"ok":false,"error":"forbidden_source"}`，否则放行。`set_pre_routing_handler` 在 `start()` 中先于 `register_routes()` 设置（start 内 register_routes 才填充 `privileged_paths_`），但 lambda 按引用捕获 `this` 并在**请求时**读取集合，此时集合已填充完毕，无时序问题。

### 3.2 实测确认 loopback 正确放行（D1 未误杀）

黑盒/契约实测中，7 个特权端点里 3 个（`storage/expand`→409、`reactivate`→409、`calibration/start`→400）返回的是 **handler 自身响应**，而非 D1 的 403 `forbidden_source`；另 4 个（`ota/install`→403、`update/install`→403、`reboot`→403、`poweroff`→403）的 403 也均为 handler 自身原因（`ota/install`/`update/install` = `feature 'ota' not licensed`；`reboot`/`poweroff` = WSL 无 systemd-logind 故 `CanReboot/CanPowerOff` 不可用）。**无一条是 D1 的 `forbidden_source`**，证明 loopback(127.0.0.1) 被正确放行。

### 3.3 Host 测试限制（如实说明）

D1 的**拒绝分支无法在 host 上直接触发**：httplib 的 `remote_addr` 取自 socket 对端（非 `X-Forwarded-For`，无伪造面），而 host 上所有连接要么走 127.0.0.1，要么走 WSL NAT 的 172.26.x（落在 172.16/12 私有网段）——两者都被放行。要触发 403 需从公网源访问，本机不具备该条件。**结论：D1 逻辑正确性由代码审查确认，拒绝分支的行为由单元语义推理确认，未做真机公网实测**（需板端/公网环境补测，标注「需确认」）。

---

## 四、发现的问题（按严重程度排序）

### #1（低 · 契约漂移）core 离线错误形状由 `core_offline:true` 改为 `code:"core_offline"`，且新增 `data:{}`

- **Python 原版**：`plugins/web/lib/error_handlers.py:53` `{'ok':False,'error':str(exc),'core_offline':True}` + 503；`api/state.py:110-111` `{'ok':False,'core_offline':True,'error':...}` + 503。均**无** `data`、**无** `code`，用**布尔** `core_offline`。
- **C++ 现版**：`domain_internal.hpp:29-38` `send_json` 恒写 `data`（空对象也写）+ 用 `code` 字符串；`state.cpp:21-22`、`state.cpp:35/84`、`control.cpp:33-34`、`hardware.cpp:56/107` 等统一输出 `{ok:false, data:{}, error:..., code:"core_offline"}`。
- **影响**：已 grep `plugins/web/static/**`，前端**没有任何** `core_offline` 消费点（`core_offline` 仅出现在 `api/`、`bin/`、`lib/`、`tests/`），故**无功能断裂**；但违反「返回 JSON 结构一字不改」铁律，且与方案 §4 钩子表「503 + core_offline:true」自相矛盾（方案附 D 又写 `code?:str` 信封，两处口径不一）。
- **建议**：需主理人拍板——① 恢复 `core_offline:true` 布尔（严格一字不改）；或 ② 接受 `code:"core_offline"`（统一信封，前端不依赖）。**此项标注「需确认」**。

### #2（低 · 契约漂移）`POST /api/control/stop` 在 core 离线时返回 200 而非 503

- **Python**：`api/control.py:60-65` `stop_control()` 先 `ipc_request('RUNTIME_CONTROL')`（离线返回 status=3，不抛），随后 `collect_web_state()` 内部 `_get_runtime_profile()` 抛 `CoreUnavailableError` → 503 `{core_offline:true}`。
- **C++**：`control.cpp:58-64` `stop` 调 `collect_web_state(ipc)` 得到 `JsonValue::null()` 哨兵，经 `state_data()` 转空对象后 `send_json(200, false, {}, "", "")` → **HTTP 200 `{ok:false, data:{}}`**（无 error）。
- **影响**：core 离线时，前端 `stop` 失败会拿到 `data.error` 为空 → `10-flow.js:655` 拼出错误文案 `"HTTP 200"`（无意义）。正常运行（core 在线）不受影响。轻微 UX/契约偏差。
- **建议**：`stop` 与 `start` 对齐——先 `get_runtime_profile` 失败即 503 `core_offline`。

### #3（信息 · 已确认正确）其余核查项无异常

- 86 路由无遗漏：黑盒 86/86 命中，与 `scripts/web_route_snapshot.txt` 一一对应。
- `core/CMakeLists.txt:864-902` `ttbox_web` 源文件列表完整：37 个 `core/src/web/**/*.cpp` 全部在列，无漏编。
- 路由注册顺序正确：`motion.cpp:108-122` 静态 `/api/motion-profiles/active` 先于动态 `<profile_id>`；`system.cpp` `POST /api/themes/redeem`（:222）先于 `POST /api/themes/<id>/install`（:234）。
- 5 条诚实占位路由确认返回 `{ok:false, code:"NOT_IMPLEMENTED", error:"…"}` 而非假数据：`PUT /api/hardware/mouse`、`GET/PUT /api/hardware/display`（hardware.cpp:127-141）、`POST /api/models/import`、`POST /api/models/import-onnx`（models_meta.cpp:49-74）。
- `license_block` 默认投影（domain_internal.hpp:169-190）`valid=activated`、未激活 `valid:false`，授权语义零推导、不假绿。
- 未发现 `TODO/FIXME/假数据` 残留（仅「下沉 T2.x / 后续批次」等诚实标注）。

---

## 五、智能路由判定

**Send To: Engineer（Alex）**

- 测试代码无 bug（三轮复现测试全部按预期通过，未发现断言错误）。
- 发现 **2 处源码侧轻微契约漂移**（#1、#2），均属「实现偏离 Python 原版 JSON 契约」而非测试问题，按判定规则应回 Engineer。
- 但严重程度均为**低**：#1 需主理人先定「恢复布尔 or 接受 code 字符串」口径（可先确认再改）；#2 是明确的 3 行内小修。
- 若主理人裁定 #1 按「接受 `code` 信封」处理、#2 一并接受，则本报告可降级为 **NoOne（全部通过）** 并附「已知轻微偏差」说明。

### 附：复现命令（供 Engineer / 后续回归）

```bash
# WSL Ubuntu-22.04
cd /mnt/c/.../main-335f4114/core
cmake --build build-web --target ttbox_web -j"$(nproc)"
cd ..
python3 scripts/web_route_blackbox.py --binary core/build-web/ttbox_web --port 18080
g++ -std=c++17 -I core/src core/tools/calibration_math_canary.cpp -o /tmp/canary && /tmp/canary
```
