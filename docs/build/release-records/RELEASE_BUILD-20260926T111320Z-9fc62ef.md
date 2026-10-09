# RELEASE_BUILD.md — 出货构建留档（T1.15）

> 本文件由 `scripts/ttbox_build_release.sh` **自动生成**（门禁全绿后才写）。
> 字段定义：`build-reproducibility.md` §10；单行记录格式：§5。**手工改动无效**（下次构建覆盖）。
> `commit` 的语义 = **构建时被编译的源码 commit**（脚本不会有未提交的 core 源码改动）；
> 本留档自身、以及其后追加的提交都**不改变**该值 —— 故 HEAD 与它不一致属**正常**，不是错。
> `vector_hash` 只覆盖**配置向量那一半**（不含 commit），与 `commit` 合成 (源码, 向量) 二元组。

## 留档字段

| 字段 | 值 |
|---|---|
| `commit` | `9fc62ef61287d58e94bcb0276c4fe08e249ae68a` |
| `toolchain` | /usr/bin/aarch64-linux-gnu-g++ (GCC 11.4.0) — aarch64-linux-gnu-g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 |
| `build_type` | `Release` |
| `generator` | `Ninja` |
| `sysroot` | `/root/sysroot` |
| `build_host` | Ubuntu 22.04.5 LTS @ PC-20260805NLLL |
| `build_time_utc` | 2026-09-26T11:13:20Z |
| `configure_args` | `cmake -S /mnt/c/ttbox-local/core -B /mnt/c/ttbox-local/build-aarch64-t148 -G Ninja -DTTBOX_SHIP=ON -DTTBOX_PROJECT_ROOT=/opt/ttbox -DTTBOX_CORE_BUILD_AUTH=OFF -DCMAKE_TOOLCHAIN_FILE=/mnt/c/ttbox-local/deploy/cmake/toolchain-aarch64.cmake -DCMAKE_SYSROOT=/root/sysroot -DTTBOX_CROSS_AARCH64=ON -DCMAKE_BUILD_TYPE=Release` |
| `product_path` | `/mnt/c/ttbox-local/build-aarch64-t148/ttbox_core_main` |
| `md5` | `108f41a43732866b4c5d32341c6d4a7a` |
| `sha256` | `27ec7bfddad3b39475ba09e7861db30a3d9215b74c468ebc25ced675e56008c3` |
| `size` | 1245304 B |
| `buildid` | `59ed4afe2da5659829f0100828d517ab041e87f7` |
| `strings_gate` | `third=0/ip=0/cred=0`（(a)第三方域名=0 硬门禁 PASS / (b)凭据字面量=0 硬门禁 PASS / (c)自有端点=0 登记〔M1 期望 0，不判 FAIL〕） |
| `selfcheck` | RUNPATH 逐段 PASS / NEEDED 覆盖 PASS（10 项 NEEDED 全部覆盖：随包 1 + 基础镜像白名单） / librknnrt 同源 PASS / lib 闭集 1（host 侧 `readelf` 静态；★ 交叉产物 host **不能** `ldd`，运行期 `ldd` 归板端 T1.13） |
| `usbproxy` | sha256 `4e09b0da6929b0eccf3555107f62b434b07908719dbfe1e71d011ae9d6f3b7ea`（= `.sha256` 声明值 ✅）；旧目录字面量=0 ✅；重建：重建成功 + sha256 **逐位一致** ✅（理想达成；4e09b0da6929b0eccf3555107f62b434b07908719dbfe1e71d011ae9d6f3b7ea） |
| `repro_verify` | 未执行（本脚本默认不做二次 clean build；设 TTBOX_RELEASE_VERIFY_REPRO=1 启用，判据见 build-reproducibility.md §13 V4） |
| `librknnrt_version` | `2.3.2` / sha256 `d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8`（须 = 2.3.2 / `d31fc19c…`；基准 = 链接期=模型=板端 三重一致） |
| `vector_hash` | `7e238b5b4c2677fa141a49362904c8112b6a3dd7fbe4b88923fb5e73d6723539`（**配置向量**指纹，**不含 commit**——可复现单元 = (源码 commit, 配置向量) 二元组；换任一向量项即变 ⇒ 与旧留档**不可比**） |
| `notes` | 上次留档对照：配置向量已变（vector_hash 18409151… → 7e238b5b…）⇒ 与上次产物**不同源、不可比**；另：源码 commit 已变（9c36677… → 9fc62ef…）；A4①=0（期望 0）/ A4②=1（期望 >=1）；CROSS_AARCH64=ON |

## §5 单行记录（供仓库外备份共用）

```
9fc62ef61287d58e94bcb0276c4fe08e249ae68a | /usr/bin/aarch64-linux-gnu-g++ (GCC 11.4.0) — aarch64-linux-gnu-g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 | /root/sysroot | /mnt/c/ttbox-local/build-aarch64-t148/ttbox_core_main | md5=108f41a43732866b4c5d32341c6d4a7a | BuildID=59ed4afe2da5659829f0100828d517ab041e87f7 | 2026-09-26T11:13:20Z | Ubuntu 22.04.5 LTS @ PC-20260805NLLL | third=0/ip=0/cred=0
```

<!-- RELEASE_BUILD_RECORD: commit=9fc62ef61287d58e94bcb0276c4fe08e249ae68a md5=108f41a43732866b4c5d32341c6d4a7a sha256=27ec7bfddad3b39475ba09e7861db30a3d9215b74c468ebc25ced675e56008c3 buildid=59ed4afe2da5659829f0100828d517ab041e87f7 vector_hash=7e238b5b4c2677fa141a49362904c8112b6a3dd7fbe4b88923fb5e73d6723539 -->
