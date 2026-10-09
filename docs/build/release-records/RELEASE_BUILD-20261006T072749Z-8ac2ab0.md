# RELEASE_BUILD.md — 出货构建留档（T1.15）

> 本文件由 `scripts/ttbox_build_release.sh` **自动生成**（门禁全绿后才写）。
> 字段定义：`build-reproducibility.md` §10；单行记录格式：§5。**手工改动无效**（下次构建覆盖）。
> `commit` 的语义 = **构建时被编译的源码 commit**（脚本不会有未提交的 core 源码改动）；
> 本留档自身、以及其后追加的提交都**不改变**该值 —— 故 HEAD 与它不一致属**正常**，不是错。
> `vector_hash` 只覆盖**配置向量那一半**（不含 commit），与 `commit` 合成 (源码, 向量) 二元组。

## 留档字段

| 字段 | 值 |
|---|---|
| `commit` | `8ac2ab01f8107c6f171a68902ff4a4b478b411a6` |
| `toolchain` | /usr/bin/aarch64-linux-gnu-g++ (GCC 11.4.0) — aarch64-linux-gnu-g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 |
| `build_type` | `Release` |
| `generator` | `Ninja` |
| `sysroot` | `/root/sysroot` |
| `build_host` | Ubuntu 22.04.5 LTS @ PC-20260805NLLL |
| `build_time_utc` | 2026-10-06T07:27:49Z |
| `configure_args` | `cmake -S /mnt/c/ttbox-local/core -B /mnt/c/ttbox-local/build-aarch64-t148 -G Ninja -DTTBOX_SHIP=ON -DTTBOX_PROJECT_ROOT=/opt/ttbox -DTTBOX_CORE_BUILD_AUTH=OFF -DCMAKE_SYSROOT=/root/sysroot -DCMAKE_TOOLCHAIN_FILE=/mnt/c/ttbox-local/deploy/cmake/toolchain-aarch64.cmake` |
| `product_path` | `/mnt/c/ttbox-local/build-aarch64-t148/ttbox_core_main` |
| `md5` | `fce995554811a97ffeb4e36fce5e5647` |
| `sha256` | `1111649d73dae597f20215f242eb9d741938b8afdd906545dc727954e137e35c` |
| `size` | 1290816 B |
| `buildid` | `e703d40ee5077605a86614b4bac53c94df939e7a` |
| `strings_gate` | `third=0/ip=0/cred=0`（(a)第三方域名=0 硬门禁 PASS / (b)凭据字面量=0 硬门禁 PASS / (c)自有端点=0 登记〔M1 期望 0，不判 FAIL〕） |
| `selfcheck` | RUNPATH 逐段 PASS / NEEDED 覆盖 PASS（10 项 NEEDED 全部覆盖：随包 1 + 基础镜像白名单） / librknnrt 同源 PASS / lib 闭集 1（host 侧 `readelf` 静态；★ 交叉产物 host **不能** `ldd`，运行期 `ldd` 归板端 T1.13） |
| `usbproxy` | sha256 `d9549da1d57311b974e3f13f1c564cb46871f9d57713323712265df2d9651452`（= `.sha256` 声明值 ✅）；旧目录字面量=0 ✅；重建：未执行（本脚本默认不做 usbproxy 重建；设 TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD=1 启用，判据见 build-reproducibility.md §11②） |
| `repro_verify` | 未执行（本脚本默认不做二次 clean build；设 TTBOX_RELEASE_VERIFY_REPRO=1 启用，判据见 build-reproducibility.md §13 V4） |
| `librknnrt_version` | `2.3.2` / sha256 `d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8`（须 = 2.3.2 / `d31fc19c…`；基准 = 链接期=模型=板端 三重一致） |
| `vector_hash` | `65802e9db79665df8e022931f9773304dfc6abf2414a0b9ddaf5b9e66b74ae67`（**配置向量**指纹，**不含 commit**——可复现单元 = (源码 commit, 配置向量) 二元组；换任一向量项即变 ⇒ 与旧留档**不可比**） |
| `notes` | 上次留档对照：配置向量相同，但源码 commit 已变（5dc542f… → 8ac2ab0…） ⇒ 本次 md5=fce995554811a97ffeb4e36fce5e5647 与上次的差异属**预期改变**（非非确定性）；A4①=0（期望 0）/ A4②=1（期望 >=1）；CROSS_AARCH64=OFF |

## §5 单行记录（供仓库外备份共用）

```
8ac2ab01f8107c6f171a68902ff4a4b478b411a6 | /usr/bin/aarch64-linux-gnu-g++ (GCC 11.4.0) — aarch64-linux-gnu-g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 | /root/sysroot | /mnt/c/ttbox-local/build-aarch64-t148/ttbox_core_main | md5=fce995554811a97ffeb4e36fce5e5647 | BuildID=e703d40ee5077605a86614b4bac53c94df939e7a | 2026-10-06T07:27:49Z | Ubuntu 22.04.5 LTS @ PC-20260805NLLL | third=0/ip=0/cred=0
```

<!-- RELEASE_BUILD_RECORD: commit=8ac2ab01f8107c6f171a68902ff4a4b478b411a6 md5=fce995554811a97ffeb4e36fce5e5647 sha256=1111649d73dae597f20215f242eb9d741938b8afdd906545dc727954e137e35c buildid=e703d40ee5077605a86614b4bac53c94df939e7a vector_hash=65802e9db79665df8e022931f9773304dfc6abf2414a0b9ddaf5b9e66b74ae67 -->
