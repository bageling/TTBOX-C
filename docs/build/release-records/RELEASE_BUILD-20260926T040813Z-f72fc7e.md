# RELEASE_BUILD.md — 出货构建留档（T1.15）

> 本文件由 `scripts/ttbox_build_release.sh` **自动生成**（门禁全绿后才写）。
> 字段定义：`build-reproducibility.md` §10；单行记录格式：§5。**手工改动无效**（下次构建覆盖）。
> `commit` 的语义 = **构建时被编译的源码 commit**（脚本不会有未提交的 core 源码改动）；
> 本留档自身、以及其后追加的提交都**不改变**该值 —— 故 HEAD 与它不一致属**正常**，不是错。
> `vector_hash` 只覆盖**配置向量那一半**（不含 commit），与 `commit` 合成 (源码, 向量) 二元组。

## 留档字段

| 字段 | 值 |
|---|---|
| `commit` | `f72fc7ec1bca89fa384287a326c4297e86e6cd51` |
| `toolchain` | /usr/bin/aarch64-linux-gnu-g++ (GCC 11.4.0) — aarch64-linux-gnu-g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 |
| `build_type` | `Release` |
| `generator` | `Ninja` |
| `sysroot` | `/root/sysroot` |
| `build_host` | Ubuntu 22.04.5 LTS @ PC-20260805NLLL |
| `build_time_utc` | 2026-09-26T04:08:13Z |
| `configure_args` | `cmake -S /mnt/g/WORKBUDDY工作区/TTBOX-最终源码-2026-09-18/core -B /mnt/g/WORKBUDDY工作区/TTBOX-最终源码-2026-09-18/build-aarch64-t148 -G Ninja -DTTBOX_SHIP=ON -DTTBOX_PROJECT_ROOT=/opt/ttbox -DTTBOX_CORE_BUILD_AUTH=OFF -DCMAKE_TOOLCHAIN_FILE=/mnt/g/WORKBUDDY工作区/TTBOX-最终源码-2026-09-18/deploy/cmake/toolchain-aarch64.cmake -DCMAKE_SYSROOT=/root/sysroot -DTTBOX_CROSS_AARCH64=ON -DCMAKE_BUILD_TYPE=Release` |
| `product_path` | `/mnt/g/WORKBUDDY工作区/TTBOX-最终源码-2026-09-18/build-aarch64-t148/ttbox_core_main` |
| `md5` | `a20cd4c16d513410825041c1ee85f59d` |
| `sha256` | `46278ad6fff55d682fb58571ad61b41116fb0ed3901d1d1df2c01f62cc425586` |
| `size` | 1240768 B |
| `buildid` | `7b0b2475768e3c294343bd3be60cf79b97b1f894` |
| `strings_gate` | `third=0/ip=0/cred=0`（(a)第三方域名=0 硬门禁 PASS / (b)凭据字面量=0 硬门禁 PASS / (c)自有端点=0 登记〔M1 期望 0，不判 FAIL〕） |
| `selfcheck` | RUNPATH 逐段 PASS / NEEDED 覆盖 PASS（10 项 NEEDED 全部覆盖：随包 1 + 基础镜像白名单） / librknnrt 同源 PASS / lib 闭集 1（host 侧 `readelf` 静态；★ 交叉产物 host **不能** `ldd`，运行期 `ldd` 归板端 T1.13） |
| `usbproxy` | sha256 `4e09b0da6929b0eccf3555107f62b434b07908719dbfe1e71d011ae9d6f3b7ea`（= `.sha256` 声明值 ✅）；旧目录字面量=0 ✅；重建：未执行（本脚本默认不做 usbproxy 重建；设 TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD=1 启用，判据见 build-reproducibility.md §11②） |
| `repro_verify` | 未执行（本脚本默认不做二次 clean build；设 TTBOX_RELEASE_VERIFY_REPRO=1 启用，判据见 build-reproducibility.md §13 V4） |
| `librknnrt_version` | `2.3.2` / sha256 `d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8`（须 = 2.3.2 / `d31fc19c…`；基准 = 链接期=模型=板端 三重一致） |
| `vector_hash` | `18409151d8caebe709db20ccd46d2cf8b01eadd8d834d4e0092157fa0d73f95d`（**配置向量**指纹，**不含 commit**——可复现单元 = (源码 commit, 配置向量) 二元组；换任一向量项即变 ⇒ 与旧留档**不可比**） |
| `notes` | 上次留档对照：配置向量相同，但源码 commit 已变（c49ca40… → f72fc7e…） ⇒ 本次 md5=a20cd4c16d513410825041c1ee85f59d 与上次的差异属**预期改变**（非非确定性）；A4①=0（期望 0）/ A4②=1（期望 >=1）；CROSS_AARCH64=ON |

## §5 单行记录（供仓库外备份共用）

```
f72fc7ec1bca89fa384287a326c4297e86e6cd51 | /usr/bin/aarch64-linux-gnu-g++ (GCC 11.4.0) — aarch64-linux-gnu-g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 | /root/sysroot | /mnt/g/WORKBUDDY工作区/TTBOX-最终源码-2026-09-18/build-aarch64-t148/ttbox_core_main | md5=a20cd4c16d513410825041c1ee85f59d | BuildID=7b0b2475768e3c294343bd3be60cf79b97b1f894 | 2026-09-26T04:08:13Z | Ubuntu 22.04.5 LTS @ PC-20260805NLLL | third=0/ip=0/cred=0
```

<!-- RELEASE_BUILD_RECORD: commit=f72fc7ec1bca89fa384287a326c4297e86e6cd51 md5=a20cd4c16d513410825041c1ee85f59d sha256=46278ad6fff55d682fb58571ad61b41116fb0ed3901d1d1df2c01f62cc425586 buildid=7b0b2475768e3c294343bd3be60cf79b97b1f894 vector_hash=18409151d8caebe709db20ccd46d2cf8b01eadd8d834d4e0092157fa0d73f95d -->
