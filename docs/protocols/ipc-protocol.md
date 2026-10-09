# ttbox_core IPC 协议 (阶段 A-1)

> 版本: 0.3 · 状态: 基础版 + 配置写入/运行控制/模型管理（PING / GET_STATUS / GET_CONFIG / SET_CONFIG / RUNTIME_CONTROL / MODEL_*）· 对应代码 `ttbox/core/src/ipc/IpcServer.{hpp,cpp}`

## 1. 传输

- **Unix (RK3588 / Linux)**：AF_UNIX SOCK_STREAM，默认 socket 路径 `/tmp/ttbox_core.sock`（`--ipc <path>` 可覆盖；启动时自动清理残留文件，chmod 0666 允许非 root 客户端）
- **Windows（预留 host 构建）**：TCP loopback，路径格式 `tcp:<port>`

## 2. 帧格式

- 请求/响应均为**一行 JSON**，以 `\n` 结尾（NDJSON）
- 单请求最大 64 KiB（超长将被拒绝）
- 客户端发送一行请求后等待一行响应，然后可关闭连接（当前协议为短连接）

## 3. 请求

```json
{
  "id": "<可选回显标识，字符串>",
  "type": "PING | GET_STATUS | GET_CONFIG | SET_CONFIG | RUNTIME_CONTROL",
  "params": { }   // SET_CONFIG / RUNTIME_CONTROL 必填
}
```

| type | 说明 |
|---|---|
| `PING` | 存活探测，恒成功 |
| `GET_STATUS` | 返回系统运行状态 |
| `GET_CONFIG` | 返回已加载配置（扁平键值） |
| `SET_CONFIG` | 原子更新 RuntimeProfile（校验→热更新→落盘），v0.2 新增 |
| `RUNTIME_CONTROL` | start/stop/restart AI 流水线，v0.2 新增 |

### SET_CONFIG（v0.2）

```json
{"type":"SET_CONFIG","params":{"profile":{ ...完整 RuntimeProfile JSON... }}}
→ 成功 {"status":0,"data":{"applied":true,"persisted":true}}
→ 校验失败 {"status":1,"error":"profile 校验失败: confidence 必须在 [0,1]"}
```

原子序：JSON 解析 → `RuntimeProfile::validate` → `RuntimeConfig.update`（内存原子替换）→ 写回配置文件（仅替换宿主 JSON 的 `runtime_profile` 键，其余键保留）。任一步失败当前运行配置不被污染；`persisted=false` 表示内存已生效但落盘失败。

### RUNTIME_CONTROL（v0.2）

```json
{"type":"RUNTIME_CONTROL","params":{"action":"start|stop|restart"}}
→ 成功 {"status":0,"data":{"action":"start"}}
```

start/stop 幂等；复用 CoreRuntime 启停，不影响平台 RuntimeController 状态机。

### MODEL_*（v0.3）

| 消息 | params | 说明 |
|---|---|---|
| `MODEL_LIST` | 无 | 返回 `{models:[manifest...], active:"<id>"}` |
| `MODEL_IMPORT` | `src_path`（必须位于收件目录 `models/_incoming/`）, `model_id`, `label?` | 收件目录文件 → staging |
| `MODEL_VALIDATE` | `model_id` | staging 校验（validator 注入；文件级或板端 RKNN 加载） |
| `MODEL_INSTALL` | `model_id` | staging → installed（需先 validate 通过） |
| `MODEL_ACTIVATE` | `model_id` | 设置激活模型；**需重启 AI 流水线生效**（Core 无模型热加载） |
| `MODEL_REMOVE` | `model_id` | 删除 installed 模型（激活中拒绝） |

`model_id` 仅允许 `[A-Za-z0-9_-]`（1~64 字符，防路径穿越）。大文件不走红 IPC：先由 Gateway 落盘到收件目录，再发 `MODEL_IMPORT` 引用路径。

## 4. 响应

```json
{
  "id": "<回显请求 id，无则空串>",
  "type": "<回显请求 type>",
  "status": <错误码 0~4>,
  "data": { ... },
  "error": "<可选，仅错误时存在>"
}
```

### 错误码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | `OK` | 成功 |
| 1 | `BAD_REQUEST` | 请求不是合法 JSON / 缺少 `type` / 结构错误 |
| 2 | `NOT_FOUND` | 目标不存在（预留） |
| 3 | `INTERNAL` | 服务端内部错误（如 status provider 未注册） |
| 4 | `UNSUPPORTED` | 未知的 `type` |

## 5. 各请求的 data 结构

### PING

```json
{ "pong": true, "server": "ttbox_core" }
```

### GET_STATUS

```json
{
  "running": true,
  "app_name": "ttbox_core",
  "version": "0.1.0",
  "uptime_ms": 1234.5,
  "ipc_socket": "/tmp/ttbox_core.sock",
  "config_file": "/path/to/config/default.json",
  "metrics": {
    "fps": 0.0, "capture_ms": 0.0, "resize_ms": 0.0,
    "infer_ms": 0.0, "decode_ms": 0.0, "aim_ms": 0.0, "e2e_ms": 0.0,
    "detect_count": 0, "dropped_frames": 0, "frames_total": 0
  }
}
```

> 阶段 A-1 中视觉链路未接入，`metrics` 全部为占位值。

### GET_CONFIG

```json
{ "<key>": "<字符串值>", ... }
```

值一律转为字符串（数字/布尔按字面，嵌套结构按紧凑 JSON）。

## 6. 错误示例

```json
{"type":"DO_NOTHING"}
→ {"id":"","type":"DO_NOTHING","status":4,"data":{},"error":"unsupported request type: DO_NOTHING"}

{ not json
→ {"id":"","type":"","status":1,"data":{},"error":"invalid JSON request: JSON 语法错误 @ ..."}
```

## 7. 客户端

- C++：`ttbox/core/src/ipc/IpcServer.hpp` 提供 `ipc_request()` / `ipc_ping()`
- 命令行：构建产物 `ipc_ping`（`--socket <path> --type PING|GET_STATUS|GET_CONFIG`）
- 手工验证：`printf '{"type":"PING"}\n' | nc -U /tmp/ttbox_core.sock`

## 8. 兼容性约定

- 协议只增不改：新请求类型以新 `type` 追加；错误码 `status` 保持稳定
- 未来升级（版本协商、二进制帧、长连接）向后兼容，不破坏现有字段
