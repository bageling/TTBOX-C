# OTA 分发服务器接口契约（O11）

> 建立依据：`docs/交付前更新功能定案-2026-09-18.md` §2.8 / §三 H。
> 读者：业主侧搭建分发服务器的实现者；本仓盒子侧 `/api/update/check`（ttbox-web.py）
> 与 `tools/ota/fake_ota_server.py`（本地联调夹具）按本契约实现。

## 1. 形态

服务器**带后端程序**（不是静态目录）——为以后服务端做灰度、强制升级、版本下限留空间。
接口刻意最小：**一次查询，返回三个字段**，复杂度留在服务端。

## 2. 接口

### 请求

```
GET {OTA_SERVER_URL}/latest?current=<当前版本>
```

- `OTA_SERVER_URL` 写死在盒子里两处（必须同值，`ttbox.sh doctor` 校验）：
  `plugins/web/bin/ttbox-web.py` 的 `OTA_SERVER_URL`、`scripts/ttbox.sh` 的 `OTA_SERVER_URL`。
- `current` 为盒子当前版本（如 `1.4.5`）；首次安装可能为空。
- 盒子**只在面板点"检查更新"或运维 `ttbox.sh upgrade` 时查一次**；不轮询、不缓存。

### 响应（200，JSON）

现役服务器（2026-09-19 上线）：`OTA_SERVER_URL = https://cctv2.top:10086/ota`，
经七牛映射「外网 10046 → 内网 443」，**地址必须带端口**（外网 443 未映射）。

```json
{
  "latest_version": "1.5.0",
  "package_url": "https://cctv2.top:10086/ota/pkgs/ttbox-update-1.5.0.tgz",
  "sign_url": "https://cctv2.top:10086/ota/pkgs/ttbox-update-1.5.0.tgz.sign.json"
}
```

| 字段 | 必填 | 含义 |
|------|------|------|
| `latest_version` | 是 | 最新版本号（`[A-Za-z0-9._-]`，与 `RELEASE_MANIFEST.json` 的 `version` 一致） |
| `package_url` | 是 | 更新包（tgz）下载地址 |
| `sign_url` | 是 | 该包旁车签名文件地址 |

**硬约束（三样皆 https）**：

1. `package_url` 与 `sign_url` 必须 `https://`——更新器对非 https **不发起下载**即返回
   `scheme_rejected`（`ttbox_ota_updater.py` ①步，无例外）。
2. `sign_url` 必须**等于** `package_url + ".sign.json"`——更新器第②b步按旁车规则
   `<pkg>.tgz.sign.json` 拉签名，这是签名契约（防自指，见签名工具文件头），不容变通。

### 签名文件格式（`.sign.json`）

```json
{
  "sha256": "<整个 tgz 的 sha256，hex>",
  "version": "1.5.0",
  "built_at": 1769000000,
  "key_id": "ttbox-ota-2026b",
  "signature": "<base64(Ed25519 签名)，对前四字段 canonical JSON>"
}
```

签名工具：`tools/ota/ttbox_ota_sign.py sign <tgz> <version> --priv <私钥>`。
**当前有效 key_id = `ttbox-ota-2026b`**；`ttbox-ota-2026a` 已作废，盒子内只有 2026b 公钥。

## 3. 盒子侧行为（实现参考）

- `/api/update/check`（ttbox-web.py）：查询一次 → 与当前版本比较 → 返回
  `update_available / package_url / sign_url / key_id`。
- 安装：web 把 `{url: package_url, key_id}` 写进任务目录 → root 更新器消费：
  下载 → sha256 比对 → Ed25519 验签 → 展开复验 → **降级拒绝**（包版本必须高于当前）
  → 原子发布 → 30 秒健康检查（三服务 active + 模型已加载，不看授权）→ 失败自动回滚。

## 4. 错误与降级

| 场景 | 盒子侧行为 |
|------|-----------|
| 服务器不可达 / 非 200 | `/api/update/check` 返回 502 `ota_server_unreachable`（如实报错，无假结果） |
| 地址仍是占位值（example.com） | 返回 503 `ota_server_not_configured`（fail-closed） |
| 包版本 ≤ 当前版本 | 更新器返回 `downgrade_rejected`（**禁止降级，不留人工口子**；回滚路径除外） |
| 非 https / sha256 不符 / 验签失败 / key_id 不符 | 更新器对应 `scheme_rejected` / `sha256_mismatch` / `signature_invalid`，零残留 |

## 5. 本地联调夹具（无真服务器时跑通全链）

`tools/ota/fake_ota_server.py`（不入包）：

```sh
# 终端 1：起假服务器（自签证书）
python3 tools/ota/fake_ota_server.py --port 8443 --serve-dir /tmp/ota-fixture

# 终端 2：让更新器信任自签证书后跑全链
SSL_CERT_FILE=/tmp/ota-fixture/cert.pem python3 scripts/ttbox_ota_updater.py \
    https://127.0.0.1:8443/ttbox-update-9.9.9.tgz ttbox-ota-2026b
```

夹具自动生成自签证书与目录里的包/签名；仅限本地联调，**绝不部署**。
