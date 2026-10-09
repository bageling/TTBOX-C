# OTA 1.5.22 远端发布说明（2026-09-20）

> ⚠ **本文档记录的 DTB 修复实际没有生效**：1.5.22 装上去后 DTB 一个字节都没改
> （更新器跑的是旧版安装脚本）。真正的修复在 **1.5.23**，见
> `docs/OTA-1.5.23-修1.5.22装了不生效-2026-09-20.md`。以下为当时的原始记录，保留备查。

面向已刷 **v3 镜像**的客户机，修 HDMI 采集整体不可用（客户报错：EDID 应用失败 · 未找到可写 HDMI-RX HPD 节点）。

## 一、发布了什么

| 项 | 值 |
| --- | --- |
| 版本 | `1.5.22` |
| 包名 | `ttbox-update-1.5.22.tgz` |
| 大小 | 3,296,711 B |
| **sha256** | `b4a99b3f60cd741fb6372eb05eb61441431a57abcb7f16537a86cfe49d6707b0` |
| 签名 | 服务器重签，`key_id=ttbox-ota-2026b`，自校验 `signature_ok=true` |
| 源 commit | `c03b2b5`（构建时所编译源码为 `35b71e5`） |
| core sha256 | `8fcafbffdcb0d10a8feac9eeede1a0a9583ee04a228d73c27dde2ec017b38580`（1,073,264 B） |
| 包内文件数 | 147 |

外网地址：

```
https://cctv2.top:10046/ota/latest
https://cctv2.top:10046/ota/pkgs/ttbox-update-1.5.22.tgz
https://cctv2.top:10046/ota/pkgs/ttbox-update-1.5.22.tgz.sign.json
```

## 二、修的是什么

出厂镜像的 `rk3588-orangepi-5-plus.dtb` 里 `hdmirx-controller@fdee0000` 是 `status="disabled"`，
而 `extlinux.conf` 没有任何 `fdtoverlays` ⇒ `rk3588-hdmirx.dtbo` 从未被应用 ⇒ `/dev/video0`
和 `/sys/class/hdmirx` 都不存在 ⇒ **HDMI 采集整体是废的**，EDID 报错只是第一个冒出来的症状。

修法：随包带上已验证可用的 DTB（sha256 `277d9de8…`，与出厂版唯一差异就是这一处 status），
由 `scripts/ttbox_dtb_fix.sh` 在装机阶段替换。

## 三、两条关键约束

**1. 必须重启才生效。** DTB 由 u-boot 在开机时读取（`extlinux.conf` 用 `fdtdir` 指向
`/lib/firmware/5.10.0-1012-rockchip/device-tree/`）。OTA 只会重启服务、不会重启机器，
只换文件不重启 ⇒ 修复永远不生效。故脚本在**确实发生替换**时执行 `shutdown -r +2`
（留 2 分钟缓冲让 activate/健康检查收尾，可用 `shutdown -c` 取消），并留标记
`/var/lib/ttbox/dtb-reboot-pending`。

**2. 指纹门禁，绝不乱换。** 只有当目标 DTB 的 sha256 **精确等于**已知的坏版本
`7b8cc892…` 时才替换；已是正确的版本、或任何不认识的版本一律不动。
所以：已经好的机器不会被换、不会被重启，重复装也不会反复重启。脚本恒退出 0，
绝不允许它把一次 OTA 判成失败。

## 四、验证到哪一步

- 交叉编译：出货门禁全绿（RUNPATH / NEEDED / librknnrt 同源 / 字符串三档 / usbproxy 诚实性），`BUILD_RC=0`
- 打包：7 道 fail-closed 断言全过，真钥签名，包内确认含 `payload/deploy/dtb/rk3588-orangepi-5-plus.dtb`
  与 `payload/scripts/ttbox_dtb_fix.sh`
- 脚本本地 5 分支测试：目标不存在 / 坏版本替换 / 已好不动 / 未知指纹不动 / 源缺失跳过 —— 全过
- 真板（Orange Pi 5 Plus，DTB 已是好的）实跑：正确定位路径 → 判定"无需改动" → 文件未变 → **未安排重启**
- 发布后外网自检：`/ota/latest` 指向 1.5.22；包下载 HTTP 200、字节数一致；签名 sidecar 可拉取

## 五、⚠ 尚未确认的一件事

发布前后反复查服务器访问日志（`/var/log/nginx/ttbox-10039.access.log`），
来源 IP **只有 `127.0.0.1` 和 `100.64.0.x`**（均为我方探测），**没有任何客户盒子来拉
`/ota/latest`**。也就是说：包已经挂上去了，但**没有证据显示客户的盒子联网或在轮询 OTA**。

需要客户侧确认：盒子通电联网、能访问 `https://cctv2.top:10046`。若盒子离线，这个 OTA 不会自己下去。
