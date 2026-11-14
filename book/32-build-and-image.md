# 第 32 章 构建与镜像：从源码到能刷的文件

> 本章目标：
> 1. 知道一个“可发布镜像”由哪几块组成，各自刷到哪里。
> 2. 理解 squashfs、UBI、整片镜像的关系。
> 3. 会做可复现的构建：版本号、校验和、清单。
> 4. 知道 boot 分区和 rootfs 分区为什么要分开更新。
> 5. 能排查“构建成功但刷上去不对”“镜像太大”“版本号没变”。

---

## 32.1 一次发布的产物

这块板一次发布（`release-20261114`）产出四个文件：

| 文件 | 大小 | 刷到哪 | 作用 |
|---|---|---|---|
| `rootfs.squashfs` | ~37 MB | `ubi0_4` | 根文件系统（只读） |
| `boot-normal.img` | ~3.9 MB | `ubi0_3` | 内核 + 空 initramfs |
| `boot-rescue.img` | ~4.6 MB | `ubi0_3`（临时） | 救援内核 + initramfs |
| `v851s_linux_lizard_uart0.img` | ~44 MB | 整片 NAND | 完整镜像（含 boot 链） |

外加：

```text
SHA256SUMS        四个文件的校验和
README.md         版本说明、改动清单、刷机步骤、验收记录
```

### 为什么不是“一个文件搞定”

```text
整片镜像：适合“第一次刷”或“救砖”，但 44 MB，每次都要全写
分卷更新：只换 rootfs（37 MB）或只换 kernel（3.9 MB），更快
```

> 发布同时给“整片”和“分卷”，是给不同场景留选择。

### 版本号

```sh
cat /etc/rootfs_version
# v851s-lizard-e907-v12-20261114
```

命名规则：

```text
v851s-lizard-e907-<序号>-<日期>
        │      │      │
        │      │      └─ 构建日期
        │      └──────── E907 固件代号
        └─────────────── 产品名
```

**版本号必须进镜像**，否则“板子上跑的是哪一版”只能靠猜。

> 验收第一步永远是 `cat /etc/rootfs_version`。

---

## 32.2 squashfs：只读根文件系统

### 为什么用 squashfs

| 特性 | 好处 |
|---|---|
| 只读 | 不会被误改，掉电不会损坏 |
| 压缩 | 37 MB 装下几百 MB 的内容 |
| 随机访问 | 按块读取，不用解压整个镜像 |
| 成熟 | OpenWrt、Tina 都用 |

### 压缩算法

```text
gzip   兼容性最好，压缩率中等
xz     压缩率高，解压慢、内存多
zstd   折中，现代内核支持
lz4    最快，压缩率低
```

这块板 64 MB 内存，选 `xz` 要小心——解压需要额外内存。
实际用的是 gzip/或厂商默认。

### 制作过程（概念）

```sh
# 1. 准备一个目录树
rootfs/
  bin/  etc/  lib/  usr/  ...

# 2. 打包
mksquashfs rootfs/ rootfs.squashfs -comp gzip -noappend

# 3. 校验
unsquashfs -l rootfs.squashfs | head
```

### 只读的代价

```text
- /etc 不能直接改 → overlayfs 解决（第 8、18 章）
- 日志不能写 → /var 是 tmpfs
- 用户数据不能放根分区 → 放 UDISK
```

> squashfs 不是“限制”，是“把可变性集中到明确的地方”。

### 检查镜像内容

```sh
unsquashfs -l rootfs.squashfs | grep -E 'linkr|S5[34]'
unsquashfs -cat rootfs.squashfs /etc/rootfs_version
```

发布前应该确认：

```text
- 没有私钥
- 没有 Wi-Fi 凭据
- 没有 .git
- 没有测试脚本留下的临时文件
```

---

## 32.3 UBI 与 MTD

### 分层

```text
NAND 芯片
  │  MTD（原始闪存，有坏块）
  ▼
UBI（逻辑卷，处理坏块和磨损）
  │  ubi0_0 ... ubi0_4
  ▼
文件系统（squashfs 或 ubifs）
```

### 这块板的分卷

```text
ubi0_0   boot0 / SPL
ubi0_1   u-boot
ubi0_2   env
ubi0_3   boot（kernel + initramfs）
ubi0_4   rootfs（squashfs）
ubi0_5   UDISK（ubifs，可写数据）
```

具体编号以 `cat /proc/mtd` 和 `ubinfo -a` 为准。

### 为什么需要 UBI

```text
NAND 有坏块 → 不能像 SD 卡那样直接写
NAND 有寿命 → 需要磨损均衡
掉电风险   → 需要日志/恢复
```

UBI 负责这些。**不要绕过它直接写 MTD。**

### 查看

```sh
ubinfo -a
cat /proc/mtd
ubiattach -m 1     # 如果没自动 attach
```

### 写入（危险操作）

```sh
# 写之前先卸载
umount /mnt/UDISK

# 写一个卷
ubiupdatevol /dev/ubi0_4 rootfs.squashfs
```

> **不要在运行中的 rootfs 上原地写自己的卷。**
> 正确做法是用救援系统写（第 33 章）。

---

## 32.4 整片镜像与分卷的关系

```text
v851s_linux_lizard_uart0.img
  ├─ 头部（BROM 能识别的格式）
  ├─ boot0
  ├─ u-boot
  ├─ env
  ├─ boot_package
  ├─ sys 分区（kernel + rootfs）
  └─ UDISK 分区（空或模板）
```

它包含**启动链**。所以：

```text
整片刷 → 连 boot0/u-boot 一起换 → 能救砖，但风险大
分卷刷 → 只换 kernel/rootfs → 保留启动链 → 日常更新用这个
```

### 构建约束（很重要）

```text
保留 final7 启动链：不替换 boot0/u-boot/env/boot_package/sunxi
```

即：这次发布只换 `rootfs` 和 `boot` 卷，启动相关的东西不动。

> 为什么？启动链一旦刷坏，板子可能进不了 FEL，只能拆机上编程器。
> **能不动就不动。**

### 校验

```sh
sha256sum -c SHA256SUMS
```

发布页上的校验和必须和实际文件一致。**构建后立刻生成，不要手写。**

---

## 32.5 可复现构建

### 什么算“可复现”

```text
同样的源码 + 同样的工具链 + 同样的参数
→ 同样的产物（至少功能等价，最好字节一致）
```

现实里很难字节一致：

```text
- 时间戳
- 构建路径
- 并行顺序
- 工具链版本
```

所以目标通常是：

```text
1. 记录所有输入（版本、commit、工具链）
2. 产物可验证（功能 + 校验和）
3. 任何人按文档能重建出等价镜像
```

### 记录什么

```text
源码 commit
工具链版本
内核 config
Zephyr config
构建脚本版本
构建时间
```

写进：

```text
/etc/rootfs_version
/etc/build-info（可选）
发布 README
```

### 构建脚本骨架

```sh
#!/bin/sh
set -eu

VERSION="v851s-lizard-e907-v12-$(date +%Y%m%d)"
OUT="release-$VERSION"

mkdir -p "$OUT"

# 1. rootfs
./build-rootfs.sh "$OUT/rootfs.squashfs"

# 2. kernel
./build-boot.sh "$OUT/boot-normal.img"

# 3. rescue
./build-rescue.sh "$OUT/boot-rescue.img"

# 4. whole image
./pack-image.sh "$OUT"

# 5. checksums
(cd "$OUT" && sha256sum *.img *.squashfs > SHA256SUMS)

echo "built $VERSION"
```

要点：

- `set -eu`：失败就停。
- 版本号集中定义。
- 每一步是独立脚本，可单跑。
- 最后统一生成校验和。

> 一个 500 行的“一键构建”脚本，出问题时无法定位。
> **拆成能单独跑的步骤。**

### 幂等

同一个脚本跑两次，结果应该一致（允许时间戳差异）。

```sh
# 反例：往已有目录追加
cp -r overlay/* rootfs/

# 正例：每次从干净目录开始
rm -rf rootfs
mkdir rootfs
cp -a base/. rootfs/
```

> 构建脚本最大的敌人是“上次的残留”。

---

## 32.6 版本与校验和

### 校验和放哪

```text
SHA256SUMS         发布目录里
README.md          写明每个文件的用途
板子上             /etc/rootfs_version、/usr/bin/* 的 md5
```

### 为什么用 SHA-256 而不是 MD5

```text
MD5 已可构造碰撞，不适合做“完整性/真实性”证明
SHA-256 目前安全
```

但**板子上常用 md5sum**（BusyBox 自带，快）。所以：

```text
发布校验：SHA-256
板上自检：md5（够用，且工具现成）
```

### 验证脚本

```sh
#!/bin/sh
set -eu
cd "$(dirname "$0")"
sha256sum -c SHA256SUMS
```

### 板上验收清单

```sh
cat /etc/rootfs_version
md5sum /usr/bin/linkr-bee-daemon
md5sum /usr/bin/linkr-ble
md5sum /etc/init.d/S54bluetooth
cat /sys/class/remoteproc/remoteproc0/state
```

期望值写在发布 README 里，照着对。

> “刷完了”不等于“刷对了”。**必须逐项核对。**

---

## 32.7 只更新一部分

### 只换 daemon

```sh
/etc/init.d/S53linkr stop
scp -O linkr-bee-daemon root@<board>:/usr/bin/
ssh root@<board> '/etc/init.d/S53linkr start'
```

不需要重刷镜像。

### 只换网页

```sh
scp -O -r linkr-bee/web/* root@<board>:/usr/share/linkr-bee/web/
```

注意缓存版本号（第 26 章）。

### 只换 E907 固件

```sh
scp -O build/zephyr/zephyr_e907.elf root@<board>:/lib/firmware/e907_firmware.elf
ssh root@<board> '
  echo stop > /sys/class/remoteproc/remoteproc0/state
  sleep 8
  echo start > /sys/class/remoteproc/remoteproc0/state
'
```

### 什么时候必须重刷

```text
- 内核/驱动改动
- 启动脚本改动（S??）
- 系统库升级
- 文件系统结构变化
```

### 什么时候不能只换文件

```text
- 换了 rootfs 布局（如 /usr 合并）
- 换了 init 系统
- 换了库 ABI
```

> 判断标准：**“目标文件依赖的东西变了吗？”** 变了就得整包。

---

## 32.8 镜像大小

### 为什么在意

```text
- NAND 空间有限（128 MB）
- 刷机时间 ∝ 大小
- 内存里解压/缓存也受影响
```

### 这块板的数字

```text
rootfs.squashfs     37 MB
boot-normal.img    3.9 MB
boot-rescue.img    4.6 MB
整片               44 MB
```

### 怎么减小

```text
1. 去掉不需要的包（python 模块、文档、locale）
2. 删调试符号（但要保留一份带符号的 ELF 用于定位）
3. 用更好的压缩（xz，代价是内存/时间）
4. strip 二进制
5. 去掉重复文件（硬链接/去重工具）
```

### 不要减的

```text
- 证书模板（不是私钥）
- 启动脚本
- 恢复工具（如 ubi_recv）
- 文档里的关键说明
```

> 省 2 MB 但删掉救援工具，是典型的“省小钱赔大钱”。

### 检查大文件

```sh
unsquashfs -ll rootfs.squashfs | sort -k3 -n -r | head -20
```

---

## 32.9 实验

### 实验 A：看现有镜像

```sh
cd /path/to/release
sha256sum -c SHA256SUMS
unsquashfs -l rootfs.squashfs | head -20
unsquashfs -cat rootfs.squashfs /etc/rootfs_version
```

### 实验 B：找出镜像里的大文件

```sh
unsquashfs -ll rootfs.squashfs | sort -k3 -n -r | head -20
```

### 实验 C：确认没有敏感文件

```sh
unsquashfs -l rootfs.squashfs | grep -E 'linkr-bee\.key|wpa_supplicant\.conf|ws-token|id_rsa'
```

**应该没有任何输出。**

### 实验 D：构建一个最小 squashfs

```sh
mkdir -p /tmp/r/usr/bin /tmp/r/etc
echo "hello" > /tmp/r/usr/bin/hello
chmod +x /tmp/r/usr/bin/hello
echo "test" > /tmp/r/etc/motd
mksquashfs /tmp/r /tmp/test.squashfs -comp gzip -noappend
unsquashfs -l /tmp/test.squashfs
```

### 实验 E：验证版本号流程

给现有镜像加一个 `build-info`：

```sh
unsquashfs -d /tmp/rootfs rootfs.squashfs
cat > /tmp/rootfs/etc/build-info <<EOF
version=v851s-lizard-e907-v12-20261114
source=$(git rev-parse --short HEAD)
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
mksquashfs /tmp/rootfs /tmp/new.squashfs -comp gzip -noappend
```

**不要直接刷**，先检查内容。

---

## 32.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 刷完版本号没变 | 刷错卷/没重启 | `cat /etc/rootfs_version` |
| 镜像太大 | 调试符号/缓存 | `unsquashfs -ll` |
| 启动失败 | 换了启动链 | 对比 final7 |
| 文件系统只读报错 | 写 squashfs | 改到 UDISK/overlay |
| 校验和不匹配 | 传输损坏/构建失败 | 重新下载/构建 |
| 板上 md5 不对 | 刷的是旧文件 | 查卷、查路径 |
| 构建不可重复 | 有时间戳/随机 | 固定 `SOURCE_DATE_EPOCH` |
| 刷机后配置丢 | 只刷了 rootfs | UDISK 是否保留 |
| 私钥进了镜像 | 打包目录没清 | `unsquashfs -l \| grep key` |
| 空间不够 | 卷大小/镜像太大 | `ubinfo -a`、`df` |

### 关于 `SOURCE_DATE_EPOCH`

```sh
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
mksquashfs ... 
```

让时间戳固定，产物更接近可复现。

---

## 32.11 练习

### 练习 32.1（必做）

检查一个现有镜像：

1. 列出 `/etc/rootfs_version` 内容。
2. 找出最大的 10 个文件。
3. 确认没有私钥、Wi-Fi 配置、令牌。
4. 验证 `SHA256SUMS`。

### 练习 32.2（必做）

回答：

1. squashfs 为什么适合做根文件系统？
2. UBI 解决了 NAND 的哪些问题？
3. 整片镜像和分卷镜像各适合什么场景？
4. 为什么这次发布“只换 rootfs，不动启动链”？
5. 板上自检为什么用 md5 而不是 SHA-256？
6. 什么情况下必须重刷，而不能只替换文件？

### 练习 32.3（必做）

写一个 `verify-release.sh`，要求：

- 校验 `SHA256SUMS`。
- 检查四个文件是否都存在。
- 检查 squashfs 里没有 `*.key`、`ws-token`、`wpa_supplicant.conf`。
- 输出明确的 PASS/FAIL。

### 练习 32.4（选做）

从一份干净目录构建一个最小 squashfs，加上 `build-info`，
再用 `unsquashfs` 读回来验证。

### 练习 32.5（选做）

比较 `gzip` 和 `xz` 压缩同一份 rootfs 的大小和构建时间，
记录数据，判断这块板应该用哪个。

---

## 32.12 本章验收

1. 能说出一次发布的四个产物和各自的用途。
2. 理解 squashfs、UBI、MTD、整片镜像的层次关系。
3. 会生成和验证校验和，会把版本号放进镜像。
4. 知道为什么要保留启动链、什么时候只更新一部分。
5. 能检查镜像里有没有敏感文件。
6. 能排查版本没变、镜像太大、校验失败三类问题。

### 自测题

1. rootfs 刷到哪个卷？
2. squashfs 是不是可写？不可写怎么办？
3. UBI 和 MTD 谁在上面？
4. 整片镜像包含哪些部分？
5. `SHA256SUMS` 用来做什么？
6. 为什么构建脚本要 `set -eu`？

> 下一章讲怎么把镜像刷进去，以及刷坏了怎么救回来。
