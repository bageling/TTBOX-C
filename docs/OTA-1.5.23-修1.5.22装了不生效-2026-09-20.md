# OTA 1.5.23 —— 修掉 1.5.22「装上了但一个字节都没改」

> 1.5.22 的发布说明见 `docs/OTA-1.5.22-远端发布说明-2026-09-20.md`。
> **1.5.22 的 DTB 修复实际没有执行**（原因见下），客户需要再吃一次 1.5.23 才真正生效。

## 一、1.5.22 的问题：装了，但没修

业主问「包里还有没有类似错误」⇒ 审计时发现 **1.5.22 的 DTB 修复被跳过了**。

**证据（开发板实测，非推测）**：

- 开发板**自己**把 1.5.22 拉下来装好了：`ota_status.json` = `state=SUCCESS`、
  sha256 与我发布的 `b4a99b3f…` 逐字节一致、`current -> releases/1.5.22`、`doctor` 全项通过。
- 但 `journalctl -u ttbox-ota | grep -c 'dtb-fix'` = **0**。
  而 `releases/1.5.22/scripts/ttbox_release_install.sh` 里明明有 **4 处** dtb_fix 引用。

**根因**：`ttbox_ota_updater.py:71`

```python
INSTALL_SCRIPT = "/opt/ttbox/current/scripts/ttbox_release_install.sh"
```

执行安装那一刻，`current` 还指向 **1.5.21** ⇒ 跑的是**旧版 install 脚本**，它根本不认识
1.5.22 新增的 dtb_fix 调用。也就是：**新增的安装逻辑要等下一次升级才会被执行**。

这正是"存在 ≠ 生效"的老毛病——文件在包里、脚本在 release 目录里、引用也在，
但**执行它的那一跳是旧代码**。

## 二、修法：改挂 EDID 入口

`ttbox-edid.service` 的 ExecStart 跑的是：

```
/bin/sh -c 'timeout 20s /opt/ttbox/current/scripts/edid/edid_apply.sh || echo ...'
```

关键差别：ensure 是在 **step5a 换链之后**才 `enable --now` edid 服务的，
那时 `current` **已经指向新版本** ⇒ 跑的是**新包**的 `edid_apply.sh`。

故把 DTB 修复挂到 `scripts/edid/edid_apply.sh` 最前面（没有 `/dev/video0` 时 EDID 本来必失败，
顺序上也合理）。install 脚本里的调用保留，两条路都留。

> 已排除的另一条路：`ttbox_ensure_services.sh:45` 的 `UNITS` 是**硬编码 7 个**，
> 靠"包里新增一个 service"来生效是自欺——新 unit 不会被拉起。

## 三、1.5.23 发布信息

| 项 | 值 |
| --- | --- |
| 版本 | `1.5.23` |
| **sha256** | `2e8ec7cd6aaa0ac6710844f4802b70772f1195be11611857540e73b6be2ba7e2` |
| 大小 | 3,297,163 B |
| 签名 | 服务器重签，`key_id=ttbox-ota-2026b`，自校验通过 |
| 源 commit | `ac0c81f` |
| 包内钩子 | `payload/scripts/edid/edid_apply.sh:19` |

`/ota/latest` 已指向 1.5.23，外网下载 HTTP 200。

## 四、⚠ 客户要做什么：升级要点两次

1. 客户刷的是 v3（内置 1.5.21）
2. 点升级 → 装 **1.5.22**（此时 DTB **不会**被改，因为跑的是 1.5.21 的 install 脚本）
3. 再点一次升级 → 装 **1.5.23**（此时跑的是 1.5.22 的脚本，且 edid 入口带 dtb_fix）
   ⇒ 替换 DTB ⇒ 自动 `shutdown -r +2` ⇒ 重启后 HDMI 采集可用

**想一步到位**：让客户直接重刷 **v4 镜像**（已内置修好的 DTB），省掉两次升级和一次重启。

## 五、另一个要提醒的：升级不是全自动

OTA 由 **web 面板**创建任务文件触发（`ttbox-web.py:2070` 写
`/var/lib/ttbox/ota/jobs/*.json` → `ttbox-ota.path` 感知 → 拉起 updater）。
**不存在定时轮询**。巡检 timer（`ttbox-ensure.timer`，每 10 分钟）只做服务自愈，不检查更新。

⇒ 客户**必须打开面板点升级**，否则盒子不会自己去拉。

## 六、验证结果（开发板实测）

| 检查 | 结果 |
| --- | --- |
| 1.5.23 装上 | `state=SUCCESS`，sha256 与发布值一致，`current -> releases/1.5.23` |
| dtb-fix **执行次数**（装 1.5.22 时） | **0** ← 问题现象 |
| dtb-fix **执行次数**（装 1.5.23 时） | **1** ← 走的是 1.5.22 的 install 脚本 |
| dtb-fix **执行次数**（手动 restart edid） | **2** ← edid 入口钩子也生效 |
| 实际输出 | `[ttbox-dtb-fix] rk3588-orangepi-5-plus.dtb 已是正确的 DTB，无需改动` |
| 是否被误安排重启 | 否（`/var/lib/ttbox/dtb-reboot-pending` 不存在） |

开发板 DTB 本来就是好的，故走"无需改动"分支、不替换、不重启 —— 说明**不会误伤**。
客户盒子是坏 DTB，会走替换分支并自动重启。

## 七、顺带发现：下载很慢

板端实测从 `cctv2.top:10046` 下载：**约 15 KB/s**，3.2 MB 的包要 3～4 分钟。
不是故障，但客户点完升级要耐心等几分钟，别以为卡死了。
