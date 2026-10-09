# 出货构建留档归档（`docs/build/release-records/`）

本目录存放**每次出货构建**的留档快照，由 `scripts/ttbox_build_release.sh` 在门禁全绿后
**自动写入**。手工改动无效（下次构建不会覆盖已归档的旧份，但同一次构建的留档以脚本写出的为准）。

## 命名

```
RELEASE_BUILD-<构建UTC时间戳>-<源码commit前7位>.md
例：RELEASE_BUILD-20260923T150207Z-e111020.md
```

时间戳取自留档内的 `build_time_utc` 字段；commit 取自 `commit` 字段。

## 为什么归档在这里，而不是构建目录里

★ T1.49（2026-09-23）口径变更。旧做法把留档写在 `${BUILD_DIR}/RELEASE_BUILD.md`，即
`build-aarch64-t*/RELEASE_BUILD.md`，并靠 `.gitignore` 白名单放行入库。两个坏处：

1. **白名单放行的文件只是"未被忽略"，不等于"已入库"** —— 每轮构建新建的 `build-aarch64-t*`
   目录里那份留档都停在"未跟踪且未被忽略"状态，一次 `git add -A` 就会把一长串构建路径
   带进版本库。这就是"源码树里冒出 build 字符"的真实渠道。
2. 留档落在出货脚本 §3 clean 会 `rm -rf` 的目录内，脚本只能靠"赶在 clean 之前读一次"
   的时序保命；时序一变（比如改用新的构建目录）留档就静默丢失。

现改为归档到本目录（`docs/build/` 已在 `.gitignore` 白名单内），构建目录整体忽略。
出货脚本读"上一次留档"也改从这里取。

## 取最新一份的正确姿势

```bash
ls -1 docs/build/release-records/RELEASE_BUILD-*.md | sort | tail -1
```

**按文件名排序**，不要用 `ls -t` 按 mtime 排：留档一旦被编辑器重新格式化或复制过，
mtime 就不再等于构建时间（实测 t142 那份 mtime 是 22:49，而内部 `build_time_utc` 是 09:00:46）。

## 字段定义

留档各字段的含义、`§5` 单行记录格式、以及可复现性判据，见
[`../build-reproducibility.md`](../build-reproducibility.md) 的 §5 / §10 / §13。

## 历史

| 日期 | 事件 |
|---|---|
| 2026-09-17 | T1.15 建立留档机制，落在 `${BUILD_DIR}/RELEASE_BUILD.md` |
| 2026-09-23 | T1.49 迁至本目录，构建目录改为整体忽略；从 `build-aarch64-t1*` 归档 12 份（t114 / t138–t148） |

同期丢弃 1 份：`build-aarch64-deploy/RELEASE_BUILD.md`（2026-09-17T04:22:04Z）——
其 `configure_args` 指向已废弃的 `Desktop/TTBOX-Module-Edition-main` 构建路径，不属本仓产物链路。
