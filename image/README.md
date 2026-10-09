# image/ — 出厂镜像烘焙链

把 TTBOX 内嵌进厂商 RK3588/Ubuntu 整机镜像时用的一整套脚本与素材。
本目录此前**从未入库**，整条交付链只在开发机存了一份 —— 机器没了就重做不了。
（2026-09-23 补入库；`image/factory/` 下两个敏感/二进制件仍按纪律排除，见文末。）

## 文件组织

| 路径 | 作用 |
|---|---|
| `00_recon.sh` … `99_fsck.sh` | 编号主链：侦察 → 准备 → 挂载 → 灌装 → 回写 → 校验 |
| `10_wsl_prep.sh` / `11_stage_image.sh` | 在 WSL 里 loop 挂载厂商 `.img` 并 chroot 进 qemu 环境 |
| `20_mount.sh` / `29_umount.sh` / `98_writeback.sh` | 挂载/卸载/回写镜像 |
| `prepare_stage.sh` / `phase1.sh` / `run_in_img.sh` | 阶段编排 |
| `v5_bake.sh` / `v5_verify.sh` | V5 出厂镜像的烘焙与验证入口 |
| `steps/` | **板内**执行的步骤脚本（`01_install_deps` → `16_final_audit`） |
| `artifacts/` | 交付文档、板端自检输出快照、客户排查记录 |
| `factory/` | 出厂素材：配置、模型、设备树 |

## ★ `factory/` 与配置分层的关系（改配置键时最容易漏的一处）

TTBOX 的配置真源按层叠加，板端落 `/etc/ttbox/config.d/`，`00-factory` 是基线、`10-device` 覆盖。
但 `10-device` 有**两份**，职责不同 —— 实测对比：

| 文件 | 键数 | `runtime_profile` | 角色 |
|---|---|---|---|
| `deploy/config/10-device.json` | 24 | 基本为空（`capture` 0×0、`model_id` `''`、`confidence` 0） | **骨架模板**：只声明键与默认，随仓分发 |
| `image/factory/10-device.json` | 139 | 完整出厂值（320×320、`model_id` `EP`、`confidence` 0.57） | **烘焙灌装值**：出厂镜像时真正写进板子的那份 |

⇒ **`image/factory/10-device.json` 是配置真源的第四处**，且在改动前不被任何文档提及。
它的风险最高：不在版本库里时 `git status` 看不见它，改键的人根本不会想起它 ——
于是出现"代码改了、包发了、出厂镜像灌的还是旧值"。

两处 `10-device.json` 的键集合关系是**真包含**（image 版 ⊇ deploy 版），值差异 5 处，集中在
`runtime_profile`：`capture.width/height`（0→320）、`fov.radius`（0.5→1）、
`inference.confidence`（0→0.57）、`model_id`（`''`→`EP`）。

> 另注一处待业主判定的口径：`factory/10-device.json` 里 `model_label` = `jwdl_sjzv11`
> 而 `runtime_profile.model_id` = `EP`。两者若语义上是"默认标签 vs 当前激活"，不算矛盾；
> 若非，则出厂默认与灌装模型不一致。已记录待确认。

## 出厂素材里不入库的两件（有纪律依据，不是遗漏）

| 文件 | 原因 |
|---|---|
| `factory/cloud.default.json` | 内含 `cloud.app_secret`（全网盒子共用的 HMAC 应用级凭据）。见同目录 `.gitignore` 与 `config/README.md` 的「凭据纪律」：真实云端凭据唯一落点是板端 `/opt/ttbox/config/default.json` 与本地密钥库，**严禁入库** |
| `factory/model_EP/model.rknn` | 4.0 MB 模型二进制，由根 `.gitignore` 的 `*.rknn` 排除。模型的**元信息**（`manifest.json` / `metadata.json` / `active.json` / `validation/ok.json`）已入库 |

## 历史

| 日期 | 事件 |
|---|---|
| 2026-09-20 | V3/V4 出厂镜像交付、客户排查记录落 `artifacts/` |
| 2026-09-21 | V5 交付标准定稿 |
| 2026-09-23 | 补入库（54 文件 / 0.51 MB，排除上述两件） |
