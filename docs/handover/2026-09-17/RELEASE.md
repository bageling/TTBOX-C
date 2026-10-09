# RELEASE.md — TTBOX 发卡 / 激活 / 换皮 / 排障 操作手册（M2 · v1.3.x）

> 适用范围：M2 批次（v1.3.0 及之后的 1.3.x）。读者 = **商户发卡方** + **板端现场运维**。
> 契约真源：`core/src/auth/`（LicenseCard / LicenseStateMachine / LicenseStore / LicenseShortCode）
> + `plugins/web/bin/ttbox-web.py`（投影层，零推导）+ `tools/license/ttbox_license_gen.py`（发卡）。
> 验收驱动：`scripts/ttbox_m2_license_accept.py`（B 系列，本文 §9）。

---

## 0. 一句话

M2 商业化三件套 = **特性级授权门控（features）** + **商业换皮（ui_brand）** + **卡号可读短码（对账）**。
三者都由**同一张离线 Ed25519 卡**下发；授权**唯一真相源 = core 的 `LicenseGate`**，
Web / 预览 / IPC 只**投影**、不派生、不臆造。**无卡 ⇒ AI 关（fail-closed）**。

> ★ 红线：短码（`TTB-XXXX-XXXX`）**只防抄错、不防伪造**，是商户对账用的**非安全边界**。
> 准入门槛永远是 **Ed25519 验签 + 设备绑定 + 有效期**。

---

## 1. 术语与契约速查

| 概念 | 定义 / 取值 | 真源 |
|---|---|---|
| 特性闭集 | `capture` / `inference` / `aim` / `ota` | `LicenseStateMachine.hpp::known_features()` |
| plan | `none` / `trial` / `subscription` / `permanent` | 同上 |
| device 绑定串 | 本板 `/proc/cpuinfo` 的 `Serial`（= `DeviceFingerprint.bind_string()`） | `DeviceFingerprint` |
| ui_brand | `[A-Za-z0-9_-]`，首字符为字母/数字，≤32 | `LicenseCard` 契约 |
| 卡 store | `/var/lib/ttbox/license/license.json`（信封原文）+ `state.json`（防回滚基线） | `LicenseStore` |
| 覆盖入口 | `/etc/ttbox/license.key`（**若存在则遮蔽 store**，覆盖优先，设计如此） | 恢复链 |
| 短码 | `TTB-XXXX-XXXX`（13 字符 = 7 位数据 + 1 位 Luhn-32 校验） | `LicenseShortCode.cpp` / `ttbox_license_gen.py` |

**恢复链优先级（覆盖优先）**：`--license` > `/etc/ttbox/license.key` > `config` > `store`。

---

## 2. 商户侧发卡（`tools/license/ttbox_license_gen.py`）

> 依赖：`pip install cryptography`。私钥落 `tools/license/.testkeys/`（gitignore，**不入库**），
> 仓库只存公钥 `tools/license/keys/<key_id>.pub.hex` + core 内嵌 `LicenseKeys.hpp`。

### 2.1 首次：生成密钥对（每个产线一次）

```sh
python tools/license/ttbox_license_gen.py gen-key --key-id ttbox-license-2026a
# 输出：私钥（不入库，务必备份到保险柜）/ 公钥（入库）
```

换钥（旧 key_id 卡自动失效）：
```sh
python tools/license/ttbox_license_gen.py gen-key --key-id ttbox-license-2027a
python tools/license/ttbox_license_gen.py emit-c-header --key-id ttbox-license-2027a
# 把输出的两行常量手工替换进 core/src/auth/LicenseKeys.hpp，然后重新构建 core。
# ★ 旧族卡的 key_id 不在新族 → core 侧 fail-closed（kInvalidCard）。
```

### 2.2 读设备绑定串

板端跑：`python tools/license/ttbox_license_gen.py fingerprint`
商户机无板 ⇒ 从设备 Web 管理页复制 `cpu Serial`，或用 `--device` 显式传入。

### 2.3 发一张卡

```sh
python tools/license/ttbox_license_gen.py issue-card \
  --device 9ecf266dc8154491 \
  --plan subscription --pro \
  --features capture,inference,aim,ota \
  --brand ttbox \
  --days 365 \
  > card-valid.json
# stdout = 卡信封 JSON（可直接落 license.key / 传板）
# stderr = short_code=TTB-006Z-0C33   ← M2.05：商户台账对账用，不污染 stdout
```

常用变体：

| 目的 | 命令要点 |
|---|---|
| 永久卡 | `--days 0`（= `expires_at=0`，永不过期） |
| 受限卡（仅采集） | `--features capture`（⇒ 能力位仅 capture，预览降级 + 水印） |
| 渠道换皮卡 | `--brand sample`（须板端品牌表含该渠道，见 §5） |
| 试用卡 | `--plan trial --days 30` |
| 指定卡号 | `--license-id ttbox-lic-20260917-0042` |

### 2.4 自检与短码对账

```sh
python tools/license/ttbox_license_gen.py verify card-valid.json      # PASS → exit 0
python tools/license/ttbox_license_gen.py short-code ttbox-lic-20260917-3842ff
#    → TTB-006Z-0C33
python tools/license/ttbox_license_gen.py verify-shortcode TTB-006Z-0C33   # PASS → exit 0
```

**跨语言契约锁**：短码算法（FNV-1a-32 → 7×Crockford-32 → Luhn mod 32）在 C++ 与 Python **两侧逐字节一致**。
黄金向量：`license_id "ttbox-lic-20260917-3842ff" → "TTB-006Z-0C33"`。
两侧单测（`core/tests/test_license_shortcode.cpp` 与 `tools/license/test_license_gen.py`）任一单方改动即红灯 ⇒ 逼对账。

> ★ 规格勘误（两处，**以实作为准**，主理人 2026-09-17 裁定）：
>  1. 短码长度 = **13 字符**（`TTB-` 3 + 分隔 1 + 4 + 分隔 1 + 4 = `TTB-XXXX-XXXX`）；
>     规格 §3.4 写的"11"系笔误，实作按 13。
>  2. `short-code` 子命令收**位置参数**：`short-code <license_id>`（如上方示例）；
>     规格 §2.12 写的 `--license-id <id>` 与实作不符，以实作为准。
>     （`issue-card` 的 `--license-id` 是发卡选项，与短码子命令无关，维持不变。）

> `verify-shortcode` **只查校验位、不查签名**：合法校验位 ≠ 真卡（构造出的合法校验短码也会被接受）。
> 这是**刻意**的 —— 短码只用于人对账，**绝不**作为授权准入判据。

---

## 3. 出货构建与刷机

**唯一出货入口**（T1.18 §5 门禁执法点，绕开 = 未执行门禁）：
```sh
bash scripts/ttbox_build_release.sh \
  -DCMAKE_TOOLCHAIN_FILE=deploy/cmake/toolchain-aarch64.cmake \
  -DCMAKE_SYSROOT=<sysroot> -DTTBOX_CROSS_AARCH64=ON -DCMAKE_BUILD_TYPE=Release
```
门禁全绿（依赖 / 产物路径锚 / 字符串表三档 / usbproxy 诚实性 / RUNPATH+NEEDED / librknnrt 同源）后，委派既有脚本完成部署：
```sh
bash scripts/ttbox_release_install.sh     # 浇筑到 /opt/ttbox（FHS 布局）
bash scripts/ttbox_release_verify.sh      # 校验 release 树（RUNPATH 闭集 / sha）
bash scripts/ttbox_release_selftest.sh    # 发布布局自测
```

服务（应 5/5 active）：`ttbox-core` / `ttbox-web` / `ttbox-preview` / `ttbox-usbproxy` / `ttbox-edid`。

---

## 4. 激活

### 4.1 途径 A：Web 端点（推荐）

```sh
# 卡信封内容作为 license_key 投递（登录后，或首次设置前 bootstrap 白名单可达）
curl -sS -X POST http://127.0.0.1:8000/api/license/activate \
  -H 'Content-Type: application/json' \
  --data "{\"license_key\": $(python -c 'import json,sys;print(json.dumps(open("card-valid.json").read()))')}"
# 成功：HTTP 200 {"ok":true,"data":{...activated:true...}}
# 失败：HTTP 400 {"ok":false,"error":"激活被拒绝：<原因>"}（fail-closed，原因逐条可辨）
```
其余可见端点：`GET /api/license`（**永久白名单**，登录页也能读授权态）、`POST /api/ota/install`（需登录）、`GET /api/state`（需登录）。

### 4.2 途径 B：落盘 / 覆盖

```sh
# 落 store（与 Web 激活等效；重启由 resolve 链恢复）
install -m 0600 card-valid.json /var/lib/ttbox/license/license.json
systemctl restart ttbox-core

# 应急覆盖（遮蔽 store，运维入口；fhs_init 不创建也不管理它）
install -m 0600 card-valid.json /etc/ttbox/license.key
systemctl restart ttbox-core
```

### 4.3 激活后的正确形态

```sh
curl -sS http://127.0.0.1:8000/api/license | python -m json.tool
#   activated=true / state=active / plan=subscription
#   features=[capture,inference,aim,ota]
#   capabilities={capture:true,inference:true,aim:true,ota:true}
#   short_code=TTB-006Z-0C33
#   ui_brand=<卡内 brand>
```

**复位为未激活（演示"未买"态）**：
```sh
rm -f /var/lib/ttbox/license/license.json && systemctl restart ttbox-core
```

---

## 5. 特性门控（M2.03）与换皮（M2.04）

三者执法层次（授权唯一真相源 = `LicenseGate`，其余层只投影/执行）：

| 层 | 位置 | 行为 |
|---|---|---|
| ① 会话边界 | `core/src/app/Application.cpp::run()` | 读一次 `LicenseGate` 快照 ⇒ 推导 `FeatureGates{capture,inference,aim}` ⇒ `CoreRuntime::set_feature_gates()` 按位启停 V4L2 / RKNN / AimThread |
| ② Web 端点可见性 | `plugins/web/bin/ttbox-web.py` | `capabilities` 投影到 `/api/license`；`/api/ota/install` 未授权 ⇒ **403 不调度** |
| ③ 预览降级 | `PreviewModule` | 全功能 ⇔ `capture∧inference∧aim` ⇒ 配置 fps、无水印；否则 `fps=min(配置值,5)` + 右下角水印 `"<BRAND大写> - LIMITED"` |

**换皮（ui_brand）落地**：卡内 `ui_brand` → core 下调 → web 品牌表投影到 `ui` 块。
板端品牌表 = `/opt/ttbox/plugins/web/config/ui_brands.json`（仓库源 = `plugins/web/config/ui_brands.json`，schema v2）。

v2 渠道条目示例（只增不改，v1 条目仍兼容）：
```json
"sample": {
  "brand_name": "SAMPLE", "brand_mark": "SM",
  "default_theme": "light", "allow_theme_switch": false,
  "brand_accent": "#E4572E", "brand_logo": "logos/sample.png",
  "theme": { "mode": "light", "accent": "#E4572E" },
  "template_dir": "sample", "static_dir": "sample"
}
```

> ★ **渠道白标 checklist（缺一即现场"用户找不到热点"）**：
> 1. 品牌表加渠道条目（`plugins/web/config/ui_brands.json`）；
> 2. **同一渠道名加入板端广播 SSID**：`scripts/wifi_manager.py` 的 `DEFAULT_SSIDS`
>    （来源环境变量 `TTBOX_WIFI_DEFAULT_SSIDS`，默认 `"TTBOX TTBOX-5G"`）——**面板显示与板端广播必须一起改**；
>    该自检由 `wifi_manager.verify_brand_ssid_consistency()` 覆盖（品牌表声明的 SSID ⊆ 实际广播集）。
> 3. 渠道静态皮肤放 `plugins/web/static/<id>/`（`static_dir` 前缀）；缺目录时静默回退默认模板（不白屏）。

---

## 6. OTA 门控

`POST /api/ota/install` 判据顺序：**401（鉴权，`before_request` 前置）> 403（feature `ota` 未授权）> 400（入参）**。
未授权卡（`capabilities.ota=false`）或 **core 不可达**（🟰 诚实默认四项全 False）⇒ **403 且不调度 updater**（fail-closed）。
Web 侧 scheme 初筛只是**提前提示**，权威判据在 updater（`scripts/ttbox_ota_updater.py`）。

---

## 7. 排障（F2 / F3 / F4 / F9 与本批修复）

| 症状 | 查因 | 处置 |
|---|---|---|
| **"卡在但被拒"但看不到原因**（F2） | `GET /api/license` → `message` 是否为空 | **已修**：解析拒绝与验签拒绝均写 `last_error`；空卡分支不再覆盖已有原因（仅 `last_error` 为空时才写 `card not set`）。若仍为空 ⇒ 升级 core 到含 F2 修复的版本 |
| 反复粘贴猜测卡 / 撞卡（F3） | 激活端点是否有独立限速 | **已修**：`/api/license/activate` 用**独立桶** `_RL_ACTIVATE`（30 次失败/10 分钟，与登录桶互不影响）；只计验签失败、成功清零、只认 `remote_addr`（不读 XFF）。命中 ⇒ 429 + `Retry-After`。**桶是 ttbox-web 进程内内存 ⇒ `systemctl restart ttbox-web` 即清空** |
| **换卡后"被拒"、日志 `downgrade rejected`**（F4） | `state.json` 的 `last_seen_issued_at` 基线 | **已修**：基线从**卡内嵌套 `license.issued_at`** 提取（此前只读顶层 ⇒ 恒 0 ⇒ 防回滚失效）。**重发同设备卡时新卡 `issued_at` 必须 ≥ 旧基线**；确需装"更旧"的卡 ⇒ 删 `state.json` 重建基线（`rm -f /var/lib/ttbox/license/state.json`） |
| 激活当次绿灯、**重启后回落未激活** | 是否有 `/etc/ttbox/license.key` 遮蔽 store | 检查 `test ! -e /etc/ttbox/license.key`；存在则备份改名（它是运维覆盖入口，优先于 store） |
| **云激活后重启 core 即永久丢激活**（D-D，M2.07；core < 1.4.1） | 症状：云端卡密激活成功 → `systemctl restart ttbox-core` → `GET /api/license.activated` 立即且**永久**变 `false`；core 日志先 `[LicenseDaemon] 云端授权恢复: state=2` 随后 `Application.cpp 授权未通过` | **已修（core 1.4.1）**：`resolve_license_card()` 不再把 cloud 形文档当离线卡交下（判定单一真源 = `StoreLoadResult.doc_is_cloud`）；`verify_now_blocking()` 与 `thread_loop` 非空分支均加 `cloud_license_` 守卫，云态下**绝不**走离线验签。升级 core 到 ≥1.4.1；板端回归锚 = 验收 **B24（云态纯重启保持，不经任何重新激活路径）** |
| `systemctl` 拒启 core（`start-limit-hit`） | `Restart=always` + `StartLimitBurst=5/5min` 被打满 | `systemctl reset-failed ttbox-core` 后重启 |
| 验收中 core 重启后读到"未激活"（假 FAIL） | web 在 IPC 未就绪时**回落**成"诚实未激活"默认块 | 判定前用**直连 `/run/ttbox/core.sock` 的 IPC PING** 作就绪探针，**禁止固定 sleep**（见 §9） |
| **全新板首次 `/setup` 提交密码即 500**（F9，`PermissionError: /etc/ttbox/web_credentials.json.tmp.*`） | `ls -ld /etc/ttbox` 的属组/权限——first-setup 的**原子写**（tmp + rename）要求 ttbox 用户对**目录**可写 | 应为 **`root:ttbox 0775`**。若为 `root:root 0755` ⇒ `chgrp ttbox /etc/ttbox && chmod 0775 /etc/ttbox` 立即恢复；根治 = 重跑含 F9 修复的 `ttbox_fhs_init.sh`（已含幂等补刀，会纠正存量目录） |
| **云端激活报「云端验证成功，但会话落盘失败」/ HTTP 500**（D-A，M2.07，F9 同族） | `ls -ld /opt/ttbox/config` 的属组/权限——web（User=ttbox）的云端会话**原子写**（`cloud_session.json.tmp.<pid>` + rename，见 `plugins/web/lib/cloud_session.py`）要求 ttbox 用户对**目录**可写 | 应为 **`root:ttbox 0775`**。若为 `root:root 0755` ⇒ `chgrp ttbox /opt/ttbox/config && chmod 0775 /opt/ttbox/config` 立即恢复；根治 = 重跑含 D-A 修复的 `ttbox_fhs_init.sh`（已含幂等补刀，会纠正存量目录） |
| **`default.json` 的 `cloud.app_secret` 世界可读**（F10；M2.07 遗留项，**M2.07.1 已根治**） | `ls -l /opt/ttbox/config/default.json` 应为 **`-rw-r----- root ttbox`（0640）** | **已根治（M2.07.1 / F10）**：收敛为 **`root:ttbox 0640`**（世界不可读、组可读）。此前收紧回退的根因**不是权限而是写侧丢属组**——以 root 重写该文件的原子写会把文件重建为 `root:root`，0640 下 `User=ttbox` 的 web EACCES ⇒ 凭据丢失（激活 **502「云端凭据未配置」**）。已修：① `scripts/ttbox_m207_accept.py::atomic_write_json` 在 `os.replace` 前把**原文件 uid/gid**（仅 posix 且 euid==0）应用回 tmp；② `ttbox_fhs_init.sh` 增**幂等** `chgrp ttbox` + `chmod 0640`（F10 补刀，纠正存量设备）。若见 0644 ⇒ 重跑含 F10 的 `ttbox_fhs_init.sh` |

---

## 8. 变更与回滚

- **升级**：`ttbox_release_install.sh` 浇筑新 release，`current` 软链切换；保留上一版 `releases/<ver>` 作回退点。
- **回滚**：统一走脚本（D14，2026-09-18 定案），不要手工 `ln -sfn`：
  ```sh
  ttbox_release_install.sh --rollback          # 回上一版本
  ttbox_release_install.sh --rollback <ver>    # 回指定版本
  ```
  脚本会切指针、重启服务并做健康检查；回滚后仍不健康 ⇒ 非零退出（不假绿）。
- **授权不受发布回滚影响**：卡在 `/var/lib/ttbox/license/`（数据目录），与 release 树解耦。

---

## 9. 板端验收驱动（B 系列）

```sh
install -d -m 0755 /root/m2-cards && cp card-*.json /root/m2-cards/
python3 scripts/ttbox_m2_license_accept.py [--restore-valid] [--password <PW>]
```

覆盖：B0 无卡基线 · B1 无签/坏签 · B2 他板卡 · B3 过期 · B8 篡改品牌 · B4 激活翻绿 ·
B9 一卡一设备 · B5 重启保持 · B7 投影恢复 · **B6 受限卡能力位（M2.03，含子断言
B6·ota OTA 端点门控 / B6·预览 预览降级）** · **B10 换皮投影（M2.04）** ·
**B11 短码投影（M2.05）** · **B12 F2 拒绝原因非空** · **B13 F3 激活限速** · **B14 F4 防降级**。

> ★ B 号唯一权威 = `ttbox-vs-yu-program/m2-acceptance-checklist.md`（主理人发号）。
>   ota 门控与预览降级不发新号，均为 B6 的能力门控半边（子断言 `B6·ota` / `B6·预览`）。

- 退出码：`0` = 无 FAIL；`1` = 有 FAIL（SKIP 不计 FAIL）。
- `--password <PW>`（opt-in）：进入**鉴权面**跑 B6·ota / B13。板端若未设管理员密码（bootstrap），
  脚本会用该值**首次设置**再登录 —— ⚠ 这会永久改变板端 web 密码；演示机选一个记得住的值。
  未给 `--password` 时 B6·ota / B13 标 **SKIP**（其 pytest 已覆盖，用注入桶无需板端鉴权）。
- 三个板端陷阱（脚本已内建规避）：① IPC PING 就绪探针（禁 sleep）；② 重启前 `reset-failed`；
  ③ B13 后用 `systemctl restart ttbox-web` 清限速桶。

**前置（干净基线）**：
```sh
test ! -e /etc/ttbox/license.key                 # 不得有遮蔽串
systemctl reset-failed ttbox-core                 # 防启动限速
```

---

## 10. 参考文件

| 文件 | 作用 |
|---|---|
| `tools/license/ttbox_license_gen.py` | 发卡 / 验卡 / 短码（商户侧） |
| `core/src/auth/{LicenseCard,LicenseStateMachine,LicenseDaemon,LicenseStore,LicenseShortCode}.{hpp,cpp}` | 授权唯一真相源与执法 |
| `plugins/web/bin/ttbox-web.py` | 投影层（`_license_block` / `_brand_payload` / `/api/ota/install` 门控 / 激活限速） |
| `plugins/web/config/ui_brands.json` | 品牌表（schema v2，含渠道白标） |
| `scripts/ttbox_m2_license_accept.py` | 板端 B 系列验收驱动 |
| `docs/handover/2026-09-17/M2.01-M2.02-板端验收-结果-2026-09-17.md` | M2.01/M2.02 板端实测结果 |
| `docs/handover/2026-09-17/evidence/m2/` | 卡证据（card-*.json）与验收日志 |
