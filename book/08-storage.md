# 第 8 章 存储：NAND、MTD、UBI、squashfs、overlay

> 本章目标：
> 1. 看懂 128 MB NAND 是怎么分区的。
> 2. 理解 MTD、UBI、UBIFS、squashfs、overlayfs 各是什么。
> 3. 知道为什么 rootfs 是只读的、配置为什么必须放 UDISK。
> 4. 能排查“刷了没生效”“重启丢配置”“UDISK 满了”。
> 5. 知道掉电时哪些数据危险，怎么写出“原子保存”。

---

## 8.1 先建立直觉：一块 NAND 的五层

同样一个文件，从“物理闪存”到“你看到的路径”，要经过五层：

```text
物理 NAND 芯片
  │  擦除块、页、坏块
  ▼
MTD（Memory Technology Device）
  │  把闪存抽象成“可擦可写”的设备
  ▼
UBI（Unsorted Block Images）
  │  坏块管理、磨损均衡、逻辑卷
  ▼
文件系统
  │  UBIFS（可写） / squashfs（只读）
  ▼
挂载点
/、/mnt/UDISK、/rom
```

生活比喻：

| 层 | 比喻 |
|---|---|
| NAND | 一本只能整页撕、不能单字改的笔记本 |
| MTD | 规定“第几页到第几页归谁” |
| UBI | 图书馆管理员：坏页换新页、把书分散放、记账 |
| 文件系统 | 目录和索引 |
| 挂载点 | 图书馆在你家地图上的位置 |

> 关键区别：**NAND 不能“覆盖写”**。要改一个字节，
> 必须把整块擦掉再写。所以需要 UBI 这种“搬来搬去 +
> 逻辑映射”的层。

---

## 8.2 MTD：四块地

板子上的 128 MB NAND 被分成 4 个 MTD：

| MTD | 大小 | 内容 | 能不能动 |
|---|---|---|---|
| `mtd0` | 1 MB | boot0 | 不要动 |
| `mtd1` | 3 MB | U-Boot | 不要动 |
| `mtd2` | 1 MB | secure_storage | 不要动 |
| `mtd3` | 123 MB | `sys`，UBI 容器 | 应用层在这里 |

查看：

```sh
cat /proc/mtd
```

典型输出：

```text
dev:    size   erasesize  name
mtd0: 00100000 00020000 "boot0"
mtd1: 00300000 00020000 "uboot"
mtd2: 00100000 00020000 "secure_storage"
mtd3: 07b00000 00020000 "sys"
```

- `size` 是字节数（十六进制）。
- `erasesize` 是擦除块大小，这里是 `0x20000` = 128 KiB。
- 你改一个字节，底层至少要擦 128 KiB。

> 这就是为什么“频繁写小文件”对 NAND 不友好：
> 每次都可能触发擦除 + 搬移。日志、数据库要控制写入频率。

---

## 8.3 UBI：在坏块上建“逻辑卷”

### UBI 解决三个问题

1. **坏块**：NAND 出厂就有坏块，使用中还会增加。
2. **磨损**：每个块擦写次数有限，要均匀使用。
3. **分区**：一块大 UBI 容器里再切成多个逻辑卷。

### 两个关键术语

| 术语 | 含义 |
|---|---|
| PEB | Physical Erase Block，物理擦除块 |
| LEB | Logical Erase Block，逻辑擦除块 |
| volume | 逻辑卷，类似“分区里的分区” |
| `ubi0_N` | 第 N 个卷的设备节点 |

UBI 维护一张“逻辑块 → 物理块”的映射表。写逻辑块时，
UBI 找一个空闲物理块写进去，再更新映射。旧块之后被擦除、
回收，参与磨损均衡。

### 这块板的 UBI 卷

| 卷 | 设备 | 内容 | 可写 |
|---|---|---|---|
| `boot-res` | `ubi0_1` | 启动资源（logo 等） | 一般不动 |
| `env` | `ubi0_2` | U-Boot 环境变量 | 谨慎 |
| `boot` | `ubi0_3` | 内核 + 设备树 | 可重刷 |
| `rootfs` | `ubi0_4` | 只读 squashfs | 重刷 |
| `UDISK` | `ubi0_5` | 用户数据 | 经常写 |

查看：

```sh
ubinfo -a
```

或者：

```sh
ls -l /dev/ubi*
cat /proc/mtd
```

> `ubi0_3` 不是“第三个分区”，是“UBI 设备 0 的第 3 号卷”。
> 卷号从 0 或 1 开始取决于工具，写之前一定用 `ubinfo` 确认。

### 为什么刷 rootfs 不会清 UDISK

因为它们是**不同的卷**。写 `ubi0_4` 只动 rootfs 那部分，
`ubi0_5` 里的 Wi-Fi 密码、证书、蓝牙绑定都还在。

> 这是“应用层升级”的基础：只换系统，不丢用户数据。
> 但整片刷机（包含 UBI 容器重建）会清掉所有卷，另当别论。

---

## 8.4 squashfs：只读，但很省空间

### 特点

- 只读，创建后不能改。
- 压缩（常用 xz、gzip、lzo），省 NAND。
- 随机读取性能好，适合 rootfs。
- 文件系统本身很紧凑。

制作：

```sh
mksquashfs rootfs_dir rootfs.squashfs \
    -root-owned -no-progress -comp xz -noappend -b 262144
```

- `-comp xz`：压缩算法。
- `-b 262144`：块大小 256 KiB。
- `-noappend`：覆盖旧文件，不追加。

### 为什么 rootfs 用只读

| 好处 | 说明 |
|---|---|
| 抗掉电 | 运行时不写，掉电不会写坏系统文件 |
| 易校验 | 整个镜像一个哈希，刷完能回读比对 |
| 易回滚 | 换一个 squashfs 就是换一个系统 |
| 防误删 | 手滑 `rm -rf /usr` 重启就恢复（overlay 另说） |
| 无碎片 | 只读文件系统不需要整理 |

代价：**不能直接改**。要改就得靠 overlay 或重做镜像。

---

## 8.5 overlayfs：只读层 + 可写层

### 结构

```text
你看到的 /
  ├─ lowerdir = /rom              squashfs，只读
  ├─ upperdir = /mnt/UDISK/overlay/upper   可写
  └─ workdir  = /mnt/UDISK/overlay/work
```

读文件：

```text
先看 upper 有没有
  ├─ 有 → 读 upper
  └─ 没有 → 读 lower
```

写文件：

```text
要改 lower 里的文件
  → 先把它复制到 upper（copy-up）
  → 在 upper 里修改
  → lower 永远不动
```

删除 lower 里的文件：

```text
在 upper 里放一个“白障”（whiteout）
  → 看起来文件没了
  → lower 里其实还在
```

### 生活比喻

想象一张**透明桌布**铺在不能写的桌子上：

- 原来的桌子（lower）不动。
- 你在桌布上写字（upper）。
- 想“删掉”桌上的图案，就在桌布上贴一块白纸（whiteout）。
- 换一张新桌子（重刷 rootfs），桌布还在，白纸也还在。

### 后果一：刷机后“旧文件盖住新文件”

镜像里更新了 `/etc/foo`，但板子的 upper 里还留着旧版 `/etc/foo`。
启动时 overlay 优先读 upper，于是**你看到的是旧内容**。

排查：

```sh
ls -l /mnt/UDISK/overlay/upper/etc/foo
```

如果存在，而你想用新镜像的版本：

```sh
rm /mnt/UDISK/overlay/upper/etc/foo
sync
reboot
```

> 注意：删 upper 里的文件，等于“恢复 lower 版本”，不是“删除文件”。
> 如果 lower 也没有，那才是真的没有。

### 后果二：UDISK 满了，写就失败

upper 在 UDISK 上。UDISK 满时：

- 改 `/etc` 下的文件失败。
- 日志写不进去。
- 令牌、证书保存失败。

查看：

```sh
df -h /mnt/UDISK
du -sh /mnt/UDISK/overlay/upper
```

### 后果三：`/var` 是内存盘

```sh
ls -ld /var
# /var -> /tmp
```

`/tmp` 是 tmpfs，重启清空。所以：

- 蓝牙绑定不能放 `/var/lib/bluetooth`。
- 运行时 socket、PID 放 `/var/run` 没问题（本来就不需要持久）。
- 需要保存的东西必须放 `/mnt/UDISK/persist/`。

---

## 8.6 UDISK：用户数据的家

### 哪些东西放这里

| 路径 | 内容 |
|---|---|
| `/mnt/UDISK/wpa_supplicant.conf` | Wi-Fi 凭据 |
| `/mnt/UDISK/persist/ws-token` | LAN 令牌（600） |
| `/mnt/UDISK/persist/certs/` | HTTPS 证书/私钥 |
| `/mnt/UDISK/persist/bluetooth/` | BLE 绑定 |
| `/mnt/UDISK/persist/etc/` | passwd/shadow 等 |
| `/mnt/UDISK/persist/ble-name` | 广播名 |
| `/mnt/UDISK/persist/webdav-url` | 上传目标 |

### 为什么不在 rootfs 里

- rootfs 是公开镜像，任何人都能解压。
- 密码、私钥、令牌进镜像 = 泄露。
- 每板一份的东西，不能固化在通用镜像里。

### 挂载与 bind mount

`/mnt/UDISK` 是 UBIFS，可写。但很多程序只认固定路径，
比如 BlueZ 只认 `/etc/lib/bluetooth`。于是用 bind mount：

```sh
mount --bind /mnt/UDISK/persist/bluetooth /etc/lib/bluetooth
```

效果：访问 `/etc/lib/bluetooth` 实际读写 UDISK 上的目录。
重刷 rootfs 后，绑定信息还在。

### 实验 8.1：看挂载

```sh
cat /proc/mounts | grep -E 'overlay|UDISK|bluetooth|ubi'
df -h
```

重点确认：

1. `/` 是 overlay。
2. `/mnt/UDISK` 是 ubifs。
3. `/etc/lib/bluetooth` 有 bind mount。

---

## 8.7 掉电安全：NUL 文件的故事

### 真实事故

这块板 v11 之前遇到过一个经典问题：

> 掉电后，`/etc/passwd` 文件大小正常，但内容全是 `NUL`（0x00）。
> 于是 `dbus-daemon` 报：
>
> ```text
> Could not get UID and GID for username "root"
> ```
>
> 蓝牙起不来，网页 daemon 也受影响，但串口还能登录。

为什么？

- 文件从只读 rootfs 复制到 UDISK 时，先分配 inode/大小，
  再写数据。
- 如果在“分配了大小、还没写数据”时掉电，就留下一个
  “大小对、内容全 0”的文件。
- UBIFS 认为这是合法文件，不会自动修复。

### 修复思路

启动脚本在 bind mount 前**校验内容**：

```sh
# 概念版
if ! grep -q '^root:' /mnt/UDISK/persist/etc/passwd; then
    cp /rom/etc/passwd /mnt/UDISK/persist/etc/passwd
fi
sync
```

Wi-Fi 配置同理：没有 `ssid=` 就重新播种模板。

> 这是嵌入式存储的通用教训：**不要假设“文件存在”等于
> “内容有效”。** 掉电、坏块、写到一半，都可能留下坏文件。

### 怎么写才安全

**原则一：写临时文件 + rename**

```sh
printf '%s\n' "$new_token" > /mnt/UDISK/persist/.ws-token.tmp
sync
mv /mnt/UDISK/persist/.ws-token.tmp /mnt/UDISK/persist/ws-token
sync
```

`rename` 在同一文件系统内是原子的：要么旧文件，要么新文件，
不会出现半个。

**原则二：写完 `sync`**

```sh
sync
```

把页缓存刷到 NAND。不加 `sync`，掉电可能丢最后一段。

**原则三：不要频繁写**

每次写都可能触发擦除。日志、计数器要合并写、限频。

**原则四：保留一份可恢复的副本**

关键配置至少留 `.bak`，启动时校验失败就回退。

---

## 8.8 擦除块、写放大与寿命

### 写放大

你写 1 字节，底层可能：

1. 读整个 128 KiB 块。
2. 改 1 字节。
3. 擦一个新块。
4. 写回去。
5. 更新映射。

这叫**写放大**。写 1 字节可能实际写 128 KiB。

### 对设计的启示

- 不要把高频计数器直接写文件。
- 日志用环形/限频，或者放 tmpfs 定期落盘。
- 配置变更才写，不要“每秒保存一次”。
- UDISK 留足空间，满了会触发更多搬移。

### 坏块

NAND 允许一定比例的坏块。UBI 会自动跳过坏块，
但**坏块是消耗品**：坏块越多，可用空间越少。

查看 UBI 状态：

```sh
ubinfo -a
dmesg | grep -i ubi
```

> 如果看到大量 `bad PEB`、`ECC error`，说明闪存开始老化。
> 备份数据，准备换板。

---

## 8.9 刷写一个 UBI 卷

### 概念流程

```text
主机                        板子
  │                           │
  │  ubi_stream.py            │
  │  --src rootfs.squashfs    │
  │  --vol /dev/ubi0_4        │
  ├──────────────────────────►│ 写入卷
  │                           │ sync
  │                           │ 回读校验 md5
  │◄──────────────────────────┤
```

### 写之前

1. `ubinfo -a` 确认卷号和大小。
2. 确认镜像大小不超过卷容量。
3. 确认板子供电稳定（刷机时断电最危险）。
4. 备份 UDISK（如果这次操作可能动到它）。

### 写之后

1. `sync`。
2. 回读并算 md5。
3. 和主机上的镜像对比。
4. 再重启。

> 发布记录里之所以有“回读 md5”这一栏，就是因为
> **“写成功”是工具说的，“写对了”要自己验。**

### 不要做的事

- 不要在正常系统里直接 `dd` 到 `/dev/ubi0_3`（可能破坏 UBI 元数据）。
- 不要写错卷号。
- 不要在没有 rescue/FEL 手段时动 boot0/U-Boot。
- 不要在刷机过程中拔电。

---

## 8.10 实验：持久化与 overlay

### 实验 A：验证 UDISK 持久化

```sh
echo hello > /mnt/UDISK/persist/hello.txt
sync
reboot
```

重启后：

```sh
cat /mnt/UDISK/persist/hello.txt
```

应该还在。

### 实验 B：验证 `/var` 不持久

```sh
echo temp > /var/hello.txt
sync
reboot
```

重启后：

```sh
ls /var/hello.txt
```

应该不存在（或 `/var` 已重置）。

### 实验 C：观察 overlay copy-up

```sh
# 找一个 rootfs 里的文件
ls -l /etc/hostname

# 改它
echo test > /etc/hostname

# 看 upper 里有没有副本
ls -l /mnt/UDISK/overlay/upper/etc/hostname
```

你应该看到 upper 里出现了 `hostname`。原文件在 `/rom/etc/hostname`
仍然不变。

### 实验 D：恢复 lower 版本

```sh
rm /mnt/UDISK/overlay/upper/etc/hostname
sync
cat /etc/hostname
```

应该恢复成镜像里的原始内容。

> 做实验 C/D 前确认你知道怎么恢复。改 hostname 影响不大，
> 但不要拿 `/etc/passwd` 做实验。

---

## 8.11 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 刷了 rootfs 没变化 | 写错卷、没 sync、overlay 旧文件 | 回读 md5、查 upper |
| 重启丢配置 | 写到了 `/var` 或 tmpfs | `df`、`ls -ld /var` |
| 配置变成全 NUL | 掉电时写到一半 | `xxd`、重新播种 |
| UDISK 满 | 日志/上传/overlay 占满 | `df -h`、`du -sh` |
| 文件删了还在 | overlay whiteout | 查 upper |
| 刷机后空间变小 | 坏块增加 | `ubinfo`、`dmesg` |
| `ubi0_x` 打不开 | 卷没 attach / 号错 | `ubinfo -a` |
| 写文件很慢 | NAND 擦除、写放大 | 减少小写、加缓冲 |
| 大量 ECC 错误 | 闪存老化/干扰 | `dmesg`、备份换板 |

---

## 8.12 练习

### 练习 8.1（必做）

在板子上执行并解释每一行：

```sh
cat /proc/mtd
ubinfo -a
df -h
cat /proc/mounts | grep -E 'overlay|ubifs'
```

回答：

1. `sys` 的擦除块多大？
2. `ubi0_5` 挂在哪？
3. overlay 的 upper 在哪个卷上？

### 练习 8.2（必做）

完成 8.10 的实验 A/B/C/D，并写一段话解释：

- 为什么 UDISK 能持久，`/var` 不能。
- 为什么改 `/etc/hostname` 会在 upper 里出现副本。

### 练习 8.3（必做）

写一个 shell 函数 `safe_write`：

```sh
safe_write() {
    path=$1
    # 从 stdin 读内容，写临时文件、sync、rename、sync
    ...
}
```

要求：

- 目标目录不存在时创建。
- 失败时不留半个文件。
- 注释说明为什么需要两次 `sync`。

### 练习 8.4（选做）

在虚拟机/备份板上模拟“UDISK 满”：

1. 用 `dd` 填满剩余空间（注意不要填满到系统无法启动）。
2. 尝试写一个配置文件。
3. 观察错误信息。
4. 删除填充文件，确认恢复。

> 不要在主力板上做，除非你有 rescue 手段。

---

## 8.13 本章验收

1. 能画出 NAND → MTD → UBI → 文件系统 → 挂载点五层。
2. 能说出四个 MTD 和五个 UBI 卷的用途。
3. 能解释 squashfs 为什么只读、overlay 的 lower/upper 怎么工作。
4. 能说出哪些数据必须放 UDISK，为什么。
5. 能解释“刷了没生效”和“配置全 NUL”两个真实问题。
6. 会写“临时文件 + rename + sync”的原子保存。
7. 能独立完成一次 UBI 卷刷写 + 回读校验。

### 自测题

1. 为什么 NAND 不能像内存一样直接改一个字节？
2. PEB 和 LEB 有什么区别？
3. 重刷 `ubi0_4` 会清掉 `ubi0_5` 吗？为什么？
4. overlay 删除文件时，lower 里的文件真的没了吗？
5. 为什么 `/var` 里的东西重启就没了？
6. 掉电为什么会产生“大小正常、内容全 0”的文件？
7. 写 1 字节为什么可能实际写 128 KiB？

> 下一章讲设备树：内核怎么知道“板子上有什么”。
> 这是从“会跑命令”到“会改板级配置”的分水岭。
