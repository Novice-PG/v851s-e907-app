# 第 37 章 开源、文档与许可证：让代码活得比你久

> 本章目标：
> 1. 知道一个可维护仓库应该长什么样，每个文档负责什么。
> 2. 会写“能让人一次跑起来”的 README 和 BUILD。
> 3. 理解许可证兼容性，知道哪些东西不能进仓库。
> 4. 会写提交信息、发布说明、贡献流程。
> 5. 能判断“这个代码能不能用、能不能发、该怎么引用”。

---

## 37.1 仓库结构：每个文件都有职责

### 现状

```text
v851s-e907-app/
├── README.md            英文总览
├── README.zh-CN.md      中文总览
├── LICENSE              Apache-2.0
├── NOTICE               第三方组件与版权声明
├── CMakeLists.txt       Zephyr 应用构建
├── prj.conf             Kconfig
├── src/                 E907 固件源码
├── firmware/            构建产物（ELF）
├── tools/               主机侧测试/基准
├── linkr-bee/           A7 侧 + 网页
├── docs/                BUILD / DEPLOY / PROTOCOL / EXPLAIN / REVIEW
├── book/                本书
└── MANIFEST.sha256      文件校验清单
```

### 每个文档回答一个问题

| 文件 | 回答 |
|---|---|
| `README.md` | 这是什么？能做什么？怎么跑起来？ |
| `BUILD.md` | 怎么从源码构建？需要什么？ |
| `DEPLOY.md` | 构建出来的东西怎么装到板子上？ |
| `PROTOCOL.md` | 协议长什么样？命令有哪些？ |
| `EXPLAIN.md` | 为什么这么设计？有什么坑？ |
| `NOTICE` | 哪些是别人的？什么许可证？ |
| `book/` | 从零教一遍 |

> 判断标准：**新人不问人，只读文档能不能跑通。**
> 跑不通，就是文档有缺口。

### 反模式

```text
- 只有 README，其他全在 wiki（会丢、会过期）
- BUILD 里写“参考 XXX”（指向不存在的链接）
- PROTOCOL 和代码不一致（代码是真相，文档要跟上）
- NOTICE 只写“见各文件”（等于没写）
```

---

## 37.2 README 的结构

### 一个好 README 的顺序

```text
1. 一句话说明这是什么
2. 架构图（10 行以内）
3. 目录结构（每个目录一句话）
4. 快速开始（最短路径）
5. 文档索引
6. 许可证
```

### 这块板的例子

````markdown
# v851s-e907-app

Zephyr firmware for the E907 RISC-V co-processor on the YuzukiPI-Lizard
(Allwinner V851S), plus the Linkr Bee changes made for this board.

```
browser ── HTTP(S)/WS ──┐
                        ├── linkr-bee-daemon ── /dev/rpmsgN ── E907 ── UART3
BLE central ── GATT ── linkr-ble ── Unix socket ──┘
```

## Quick start

```sh
export ZEPHYR_BASE=/path/to/zephyr   # branch v851s-e907-bsp
cmake -B build -GNinja -DBOARD=v851s_e907 .
ninja -C build
```

Artefact: `build/zephyr/zephyr_e907.elf`.

See [docs/BUILD.md](docs/BUILD.md) and [docs/DEPLOY.md](docs/DEPLOY.md).
````

> 快速开始必须**能复制粘贴就跑**。不要写“先安装依赖（略）”。

### 不要做的事

```text
- 一屏还没说清这是什么
- 徽章（build passing）比内容多
- 截图 5 张，命令 0 条
- “TODO: 补充文档”
```

---

## 37.3 BUILD 与 DEPLOY：可执行，不解释

### BUILD 要写什么

```text
- 前置条件（工具链、Zephyr 版本、主机系统）
- 一条能跑的命令
- 产物路径和名字
- 关键配置项（为什么这么设）
- 常见构建错误
```

### 一个真实片段

```sh
export ZEPHYR_BASE=/path/to/zephyr      # branch with the V851S BSP
cmake -B build -GNinja -DBOARD=v851s_e907 /path/to/v851s-e907-app
ninja -C build
```

产物：

```text
build/zephyr/zephyr_e907.elf
```

关键约束：

```text
- .resource_table 必须存在（否则 Linux 拒绝启动）
- CONFIG_KERNEL_BIN_NAME=zephyr_e907（否则名字对不上）
- XIP off（E907 从 DRAM 跑）
```

> BUILD 的读者是“想复现你的人”。**每个版本号、每个分支名都要写。**

### DEPLOY 要写什么

```text
- 文件放到哪（绝对路径）
- 服务怎么重启
- 怎么验证生效
- 怎么回滚
```

```sh
/etc/init.d/S53linkr stop
scp -O linkr-bee-daemon root@<board>:/usr/bin/
ssh root@<board> '/etc/init.d/S53linkr start'
```

验证：

```sh
ssh root@<board> 'md5sum /usr/bin/linkr-bee-daemon'
```

> “装好了”要有客观判据。**md5 是最便宜的那一种。**

### 一个常见缺口

```text
BUILD 写了怎么编译，没写“编出来的东西放哪”
DEPLOY 写了怎么放，没写“怎么确认放对了”
```

补上这两句，文档就从“参考”变成“可用”。

---

## 37.4 PROTOCOL：一行就是契约

### 结构

```text
1. 传输（WS / BLE / Unix socket）
2. 帧格式（二进制头）
3. 命令表
4. 事件格式
5. 错误码
6. 版本策略
```

### 命令表

| 命令 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `@c?` | — | `OK control ...` | 查所有权 |
| `@c claim` | `[idle]` | `OK ...` | 申请 |
| `@c release` | — | `OK ...` | 释放 |
| `@i?` | — | `@info ...` / `@info done` | 诊断 |
| `@u?` | — | `OK uart ...` | 查串口参数 |
| `@u=` | `baud,data,parity,stop,flow` | `OK`/`ERR` | 设置 |
| `@w?` | — | `@event wifi ...` | 查 Wi-Fi |
| `@w scan` | — | `@scan ...` / `@scan done` | 扫描 |
| `@s?` | — | `OK socket ...` | 查令牌状态 |
| `@s token=` | `<hex>` | `OK`/`ERR` | 设令牌 |
| `@h` | — | 命令清单 | 帮助 |

> 这张表**必须和 `@h` 的输出一致**。改一个就改两个。

### 帧格式

```text
Management（12 字节头）
0  2  'L','K'
2  1  version = 1
3  1  type
4  4  reqid (LE)
8  2  payload length (LE)
10 2  flags
12 N  payload
```

```text
Reliable UART（12 字节头）
0  2  'L','R'
2  1  version
3  1  reserved
4  4  seq (LE)
8  2  length (LE)
10 2  reserved
12 N  data
```

### 为什么要写这么细

因为**别人要用它写客户端**。手机 App、CLI、第三方工具都靠这份文档。

```text
文档不细 → 每个人自己猜 → 每个人实现不一样 → 互通失败
```

> 协议文档不是“说明”，是**接口定义**。

---

## 37.5 提交规范

### 一个提交只做一件事

```text
好：
  fix(ble): keep bond when MAC is derived from Wi-Fi
  docs(book): add chapter on flashing
  feat(cli): add --json output

坏：
  update stuff
  fix bug and add feature and reformat
```

### 格式

```text
<type>(<scope>): <subject>

<body: 为什么，不是做了什么>

<footer: 关联 issue、breaking change>
```

常见 type：

```text
feat      新功能
fix       修 bug
docs      文档
refactor  重构（不改行为）
test      测试
build     构建
chore     杂项
```

### 为什么“为什么”比“做了什么”重要

```text
diff 已经告诉你“做了什么”。
提交信息要告诉你“为什么”，否则半年后没人敢改。
```

```text
fix(ble): edit EATTChannels instead of Channels

BlueZ 5.54 in this rootfs ignores the [GATT] Channels key and reads
EATTChannels.  Without this the daemon fails to start with
"EATT channels must be configured".

Refs: BlueZ src/gatt-database.c
```

> 一个好的提交信息，能让 `git log` 变成设计文档。

### 签名

Zephyr 等上游要求：

```text
Signed-off-by: Name <email>
```

必须用真实身份，且和 `git config user.name/user.email` 一致。

```sh
git commit -s -m "..."
```

> 不是形式主义：`Signed-off-by` 是**开发者原创声明**（DCO）。

---

## 37.6 许可证：能用什么，能发什么

### 常见许可证

| 许可证 | 能商用 | 必须开源 | 能闭源链接 | 备注 |
|---|---|---|---|---|
| MIT | ✅ | ❌ | ✅ | 最宽松 |
| BSD-3 | ✅ | ❌ | ✅ | 类似 MIT + 禁止背书 |
| Apache-2.0 | ✅ | ❌ | ✅ | 有专利授权 |
| MPL-2.0 | ✅ | 改动的文件 | ✅ | 文件级 copyleft |
| LGPL-2.1/3 | ✅ | 库的改动 | ✅（动态） | 静态链接有争议 |
| GPL-2.0/3 | ✅ | ✅ | ❌ | 整个作品 |
| AGPL | ✅ | ✅（含网络） | ❌ | SaaS 也触发 |

### 这个仓库的组合

```text
本项目代码            Apache-2.0
Linkr Bee 前端        Apache-2.0
xterm / addon-fit     MIT
marked                MIT
dompurify             Apache-2.0 OR MPL-2.0
jszip                 GPL-3.0 OR MIT   ← 选了 MIT 分支
esptool-js            Apache-2.0
agent                 Apache-2.0
macaron-cursors       CC-BY-4.0
字体                   OFL-1.1
Zephyr                Apache-2.0
OpenAMP/libmetal      BSD-3-Clause
```

**全部兼容 Apache-2.0 分发。**

### 关键：多许可证要“选一个”

```text
jszip: GPL-3.0 OR MIT
```

你可以按 MIT 使用和分发。但**必须在 NOTICE 里写明选了哪个**。

```text
如果选了 GPL 分支，整个作品可能被迫 GPL。
```

> 多许可证不是“随便选”，是**有意识地选**，并留证据。

### 不能进仓库的东西

```text
- 全志 Tina SDK（Allwinner EULA，不可再分发）
- 厂商固件 blob
- 整片镜像
- datasheet
- 任何私有密钥/凭据
- 别人的代码但没有许可证
```

这不是“小气”，是**法律边界**。违反的后果是下架、索赔。

### NOTICE 要写什么

```text
1. 本项目原创部分 + 版权 + SPDX
2. 每个第三方组件 + 版本/commit + 许可证
3. 明确“不包含”的东西及原因
```

> NOTICE 是给**法务和下游**看的。写清楚，省很多事。

---

## 37.7 引用与致谢

### 什么时候要引用

```text
- 用了别人的代码（哪怕改过）
- 用了别人的协议设计
- 参考了别人的文档/博客
- 用了别人的数据/图片
```

### 怎么引用

```text
代码：文件头 SPDX + NOTICE
文档：链接 + 作者 + 日期
灵感：正文里写“参考 X 的实现”
```

### 一个文件头

```c
/*
 * Linkr Bee GATT server for V851S.
 *
 * Copyright (c) 2026 Abduwaris Nurmemet <daabu@qq.com>
 * SPDX-License-Identifier: Apache-2.0
 */
```

> 文件头是最不容易丢的归属信息。**新文件都加上。**

---

## 37.8 发布说明

### 要写什么

```text
1. 版本号 + 日期
2. 包含哪些文件 + 校验和
3. 相对上一版改了什么（分类）
4. 升级步骤
5. 已知问题
6. 回滚方法
```

### 一个真实结构

```markdown
# v851s-lizard v12 (2026-11-14)

## Files
| File | Size | MD5 |
...

## Changed since v11
- A7: S54bluetooth edits EATTChannels
- A7: bond storage bind-mounted from UDISK

## Upgrade
1. Backup UDISK
2. Flash rootfs via rescue initramfs (see docs)
3. Verify rootfs_version and md5

## Known issues
- BLE dual-central not supported (controller firmware)

## Rollback
Write back `boot-prev-*.img` and previous rootfs.
```

### 版本号规则

```text
MAJOR.MINOR.PATCH   或   日期 + 序号
```

这块板用：

```text
v851s-lizard-e907-<序号>-<日期>
```

关键是**单调、唯一、可比较**。

> “最新版”不是版本号。**必须有字符串能排序。**

---

## 37.9 贡献流程

### 一个最小 CONTRIBUTING

```markdown
## How to contribute

1. Fork and branch from `main`.
2. Keep commits small and focused.
3. Run `./linkr-bee/verify.sh` before pushing.
4. Update MANIFEST.sha256 if you changed tracked files.
5. Open a PR with: what, why, how tested.
```

### PR 描述模板

```markdown
## What
一句话。

## Why
问题是什么，不修会怎样。

## How tested
- [ ] build
- [ ] verify.sh
- [ ] on-board: H1, H4, H6
```

### 代码评审看什么

```text
1. 行为是否正确（最重要）
2. 边界是否处理
3. 是否破坏兼容
4. 是否有测试
5. 命名/风格是否一致
6. 提交信息是否说清“为什么”
```

> 评审不是“找茬”，是**共同承担后果**。

### 上游 vs 本地

```text
能上游的 → 提 PR（让别人也受益）
只对本板成立的 → 本地补丁 + 注释
```

第 27 章讲过的判断标准：

```text
“没有 V851S 的人会不会也遇到？”
```

---

## 37.10 实验

### 实验 A：审查自己的 README

找一个没看过这个项目的人，让他只读 README，试着构建。
记录他在哪一步卡住。

### 实验 B：检查文档一致性

```sh
# 命令表 vs 代码里的 @h
grep -n '@h' linkr-bee/a7/linkr-bee-daemon
# 人工对比 PROTOCOL.md 的命令表
```

找出不一致的地方。

### 实验 C：许可证扫描

```sh
# 找出所有第三方目录
find linkr-bee/web/vendor -maxdepth 1 -type d
# 每个目录里找 LICENSE
find linkr-bee/web/vendor -maxdepth 2 -iname 'LICENSE*'
```

对照 NOTICE，确认没有遗漏。

### 实验 D：写一个提交

挑一个真实的小改动，写一个符合规范的提交：

```text
type(scope): subject

body: 为什么

Signed-off-by: ...
```

### 实验 E：模拟发布

给当前状态打一个 tag，写一份发布说明，包含校验和。

```sh
git tag -a v0.1.0 -m "first public snapshot"
sha256sum firmware/zephyr_e907.elf
```

---

## 37.11 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 新人跑不起来 | README 缺前置条件 | 找人实测 |
| 文档和代码不一致 | 改代码没改文档 | 对照 `@h` |
| 许可证冲突 | 引入 GPL 代码 | `reuse lint`、人工审 |
| 仓库里有 SDK | 误提交 | `.gitignore` + `git ls-files` |
| 提交信息无信息 | “update” | 评审拦下 |
| NOTICE 过期 | 加了依赖没更新 | 发布前检查 |
| 版本号重复 | 手工写 | 用脚本生成 |
| 发布说明没写回滚 | 只写“新功能” | 模板强制 |
| 引用缺失 | 抄了没说 | 文件头 + NOTICE |
| PR 太大 | 一次改太多 | 拆提交 |

### 关于 `.gitignore`

```text
build/
*.o
*.pyc
__pycache__/
*.key
*.pem
/mnt/
```

**注意：`*.pem` 可能误伤证书模板。** 用更精确的规则：

```text
*.key
server.pem
!linkr-bee/a7/certs/san.cnf
```

> `.gitignore` 是最后一道防线，不是唯一防线。**提交前还是要看 `git status`。**

---

## 37.12 练习

### 练习 37.1（必做）

审查本仓库的 README，回答：

1. 它有没有一句话说清“这是什么”？
2. 快速开始能不能直接复制执行？
3. 文档索引是否完整？
4. 有没有过时的链接？

### 练习 37.2（必做）

回答：

1. README / BUILD / DEPLOY / PROTOCOL 各回答什么问题？
2. 为什么提交信息要写“为什么”？
3. `Signed-off-by` 的法律含义是什么？
4. Apache-2.0 和 GPL-3.0 最大的区别是什么？
5. 为什么全志 SDK 不能进仓库？
6. 多许可证组件应该怎么处理？

### 练习 37.3（必做）

给一个第三方组件补全 NOTICE 条目，包含：

```text
名称、来源、版本/commit、许可证、是否修改
```

### 练习 37.4（必做）

写一份发布说明，至少包含：

```text
版本号、文件清单、改动、升级步骤、已知问题、回滚
```

### 练习 37.5（选做）

用 `reuse`（FSFE 工具）给仓库加 SPDX 头，并跑 `reuse lint`。

### 练习 37.6（选做）

把 `book/` 里的某一章翻译成英文，检查技术术语是否一致。

---

## 37.13 本章验收

1. 能说出仓库里每个文档的职责，并判断缺了哪个。
2. 会写“复制就能跑”的 README/BUILD/DEPLOY。
3. 理解许可证兼容性，知道什么不能进仓库。
4. 会写规范的提交信息和发布说明。
5. 有贡献流程和评审清单。
6. 能解释“引用”和“致谢”的必要性。

### 自测题

1. PROTOCOL 文档和 `@h` 输出不一致，谁对？
2. 为什么 BUILD 要写产物路径？
3. `GPL-3.0 OR MIT` 应该怎么选？
4. 提交信息里 diff 已经说明的事还要写吗？
5. 全志 SDK 为什么不能放进来？
6. 发布说明必须包含哪三样“非功能”信息？

> 第 5 部分到此结束。下一部分把前面所有东西合成一个完整项目。
