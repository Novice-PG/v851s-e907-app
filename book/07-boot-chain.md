# 第 7 章 启动链：从按下电源到网页打开

> 本章目标：
> 1. 能按顺序说出 BROM → boot0 → U-Boot → 内核 → init → S 脚本。
> 2. 能看懂启动日志，判断“卡在哪一级”。
> 3. 理解 U-Boot 环境变量、`bootargs`、`bootcmd`。
> 4. 知道 FEL、rescue、U-Boot 命令行各自什么时候用。
> 5. 知道哪些分区绝对不能乱刷。

---

## 7.1 一条时间轴

```text
上电/复位
  │
  ├─ BROM（芯片内固化的 ROM 代码）
  │    ├─ 初始化最基本的时钟
  │    ├─ 从 NAND/SPI/SD 找第一级引导
  │    └─ 校验并跳转到 boot0
  │
  ├─ boot0（mtd0，1 MB）
  │    ├─ 初始化 DRAM
  │    ├─ 初始化更多时钟/电源
  │    ├─ 从 mtd1 读 U-Boot
  │    └─ 跳转到 U-Boot
  │
  ├─ U-Boot（mtd1，3 MB）
  │    ├─ 读环境变量（env，ubi0_2）
  │    ├─ 按 bootcmd 执行
  │    ├─ 从 ubi0_3 读内核 + 设备树
  │    └─ 启动内核
  │
  ├─ Linux 4.9.191
  │    ├─ 解析设备树
  │    ├─ 保留 E907 的 4 MiB 和 rpmsg/vring 内存
  │    ├─ 挂载 rootfs（ubi0_4，squashfs + overlay）
  │    ├─ 执行 /init
  │    └─ 启动 busybox init
  │
  ├─ /etc/init.d/rcS
  │    └─ rc.final：按文件名顺序执行 S?? 脚本
  │
  ├─ S30persist → S31hostname → S45wifi-conf → S50wifideamon
  ├─ S52e907（加载 uart3pin.ko，启动 E907）
  ├─ S53linkr（启动 Python daemon）
  ├─ S54bluetooth（XR829 蓝牙 + linkr-ble）
  └─ S99postnet（DHCP / NTP）
       │
       └─ 浏览器打开 https://<板子 IP>:8443/
```

> 记住“接力赛”这个比喻：每一棒只负责把下一棒叫醒，
> 然后交出自己的控制权。任何一棒失败，后面的都不会发生。

---

## 7.2 每一级到底在做什么

### BROM

BROM 是芯片出厂时固化在 ROM 里的代码，**改不了、刷不掉**。
它只做最少的事：

- 根据启动介质选择（NAND、SPI、SD、USB FEL）找引导。
- 读一小段到内部 SRAM。
- 校验签名/校验和（如果启用安全启动）。
- 跳过去。

因为 SRAM 很小，第一级引导（boot0）必须也很小。

### boot0

boot0 在 `mtd0`，1 MB。它负责：

- 初始化 DRAM 控制器（之后才能用 64 MB 内存）。
- 初始化时钟、电源、部分引脚。
- 从 `mtd1` 读 U-Boot 到 DRAM。
- 跳转到 U-Boot。

如果 boot0 坏了，现象通常是：**串口完全没有输出**，
或者只输出一行就停。这时只能靠 FEL 救。

### U-Boot

U-Boot 是一个“小型操作系统”，提供：

- 命令行（可以 `printenv`、`setenv`、`md`、`mw`、`fatload`…）。
- 网络启动（TFTP）、USB 启动、NAND/UBI 读写。
- 环境变量（`bootcmd`、`bootargs`）。
- 启动脚本（`boot_normal`、`boot_rescue` 之类）。

这块板的启动逻辑大致是：

```text
U-Boot
  └─ 读 env
      └─ bootcmd
          └─ run boot_normal
              ├─ 从 ubi0_3 读内核 + 设备树
              └─ bootm / booti
```

### 内核

Linux 4.9.191 启动后：

1. 解析设备树，知道有哪些设备、内存怎么分。
2. 保留 E907 的 4 MiB、rpmsg buffer、vring。
3. 挂载 rootfs。
4. 执行 `/init`（busybox init）。
5. init 读 `/etc/inittab`，启动 `rcS`。

### init 脚本

`rcS` 最后会执行 `rc.final`，后者按文件名顺序跑：

```sh
for i in /etc/init.d/S??*; do
    $i start
done
```

所以 `S30` 一定在 `S52` 前，`S99` 最后。

> 注意：这个镜像里 `load_script.conf` 是空的，真正排序的是
> `rc.final` 的 `S??*` 循环。不同镜像可能用不同机制，
> 看脚本内容比看文档可靠。

---

## 7.3 读启动日志：定位“卡在哪”

### 接线

- 串口：`/dev/ttyACM0`，115200 8N1。
- 工具：`picocom -b 115200 /dev/ttyACM0`。

### 正常启动的关键行

```text
[Boot] ... boot0 ...
U-Boot 20xx.xx ...
DRAM:  64 MiB
...
Loading Kernel Image ...
Starting kernel ...
[    0.000000] Booting Linux on physical CPU 0x0
...
[    1.234567] Run /init as init process
...
Starting linkr-bee: ok
Starting bluetooth: ...
```

你不需要记住每一行，只要记住**四个分界点**：

```text
1. 有没有 boot0 输出？
2. 有没有 U-Boot 输出？
3. 有没有 "Starting kernel"？
4. 有没有跑到 S 脚本 / 登录提示？
```

### 失败位置对照表

| 最后看到 | 卡在哪 | 可能原因 | 怎么救 |
|---|---|---|---|
| 什么都没有 | BROM/boot0 | boot0 坏、NAND 坏、供电 | FEL 重刷 boot0 |
| boot0 一行后停 | boot0/DRAM | DRAM 参数、供电、boot0 不匹配 | FEL 重刷 |
| U-Boot 提示符 | U-Boot | env 错、内核/DTB 读不到 | 修 env、重刷 boot |
| `Starting kernel` 后无输出 | 内核早期 | DTB 错、console 参数错、内核不匹配 | 改 bootargs、重刷 boot |
| 内核日志到一半停 | 驱动/rootfs | rootfs 挂不上、驱动卡死 | rescue、检查 UBI |
| 到 init 后停 | 用户态 | S 脚本死循环、关键文件缺失 | 串口登录、单步跑脚本 |
| 有登录但网页打不开 | 服务/网络 | daemon 没起、Wi-Fi 没连 | `ps`、日志、`@i?` |

### 一个真实例子：console 参数错了

如果 `bootargs` 里写的是 `console=ttyS0`，但实际调试口是
`ttyS0` 之外的 UART，内核日志会“消失”，看起来像“内核没启动”。

**不是没启动，是日志没送到你看的串口。**

排查：

```text
1. 确认 bootargs 的 console= 和你的物理串口一致
2. 确认波特率一致
3. 临时在 U-Boot 里改 bootargs，再 boot
```

---

## 7.4 U-Boot 环境变量：启动的“配置文件”

### 看

进入 U-Boot 命令行后：

```text
printenv
printenv bootargs
printenv bootcmd
```

### 关键变量

| 变量 | 作用 | 例子 |
|---|---|---|
| `bootcmd` | 上电自动执行的命令 | `run boot_normal` |
| `bootargs` | 传给内核的参数 | `ubi.mtd=sys console=ttyS0,115200` |
| `boot_normal` | 正常启动脚本 | 读内核、bootm |
| `boot_rescue` | 救援启动脚本 | 读 rescue 镜像 |

### `bootargs` 里常见项

```text
ubi.mtd=sys                 根文件系统在哪个 MTD 上
console=ttyS0,115200        内核日志输出到哪个串口
root=/dev/ubi0_4            根文件系统设备
rootfstype=squashfs         根文件系统类型
rw                          可写（overlay 场景不一定需要）
loglevel=7                  日志级别
```

> 改 `bootargs` 是“临时实验”的常用手段，但**改完要确认
> 能恢复**。在 U-Boot 里 `setenv` 只改内存，`saveenv` 才写回。
> 不确定就先 `printenv` 抄下来。

### 进 U-Boot 的时机

上电后串口会快速滚屏。看到 U-Boot 提示时，通常按一个键
（常见是空格、回车或 `Ctrl-C`，取决于配置）可以打断自动启动。

如果错过了，就等它启动完，或者复位重来。

> 不同 U-Boot 配置不一样。**先看屏幕上的提示**，不要盲按。

---

## 7.5 FEL：芯片级别的“救砖模式”

### 什么是 FEL

BROM 里有一段 USB 恢复代码。当启动介质找不到有效引导，
或者你主动进入 FEL 模式时，芯片会通过 USB OTG 等待主机
发送代码。

### 怎么进

- 按住某个键/短接某个点再上电（看板子原理图）。
- 或者 boot0 校验失败时自动进入。

### 主机工具

全志常用 `sunxi-fel`（开源）或全志的 `xfel`：

```sh
sunxi-fel ver
sunxi-fel read 0x40000000 0x1000 dump.bin
```

> FEL 是最后一道门。能进 FEL，板子基本就“没死透”。
> 但 FEL 操作有风险，**不要在没备份的情况下写 NAND**。

### 这块板的救砖路径

1. 进 FEL。
2. 用官方/仓库提供的工具重刷 boot0 + U-Boot。
3. 再用 rescue initramfs 恢复 rootfs/boot。

具体命令见 `docs/DEPLOY.md` 和仓库的刷机记录。
**不要凭记忆敲 FEL 写命令。**

---

## 7.6 rescue：不改引导链，只修应用层

### 为什么需要 rescue

正常系统起不来时，如果直接重刷整片，会连 boot0/U-Boot/env
一起动，风险大。rescue 的思路是：

```text
临时把 boot 分区（ubi0_3）换成一个“救援 initramfs”
  → 启动到内存里的最小系统
  → 在救援系统里重写 ubi0_4（rootfs）和 ubi0_3（正常 boot）
  → 重启
```

引导链（boot0/U-Boot/env）完全不动。

### 流程（概念版）

```text
1. 把 boot-rescue.img 写到 ubi0_3
2. 在 U-Boot 里设置临时 bootargs：
      ubi.mtd=sys console=ttyS0,115200 loglevel=7
3. run boot_normal（或对应的救援启动命令）
4. 进入救援 shell
5. 写 rootfs.squashfs → ubi0_4
6. 写 boot-normal.img → ubi0_3
7. sync; reboot -f
```

### 写 UBI 卷的工具

仓库里有 `ubi_stream.py` 之类的脚本，负责把镜像流式写入
指定卷：

```sh
ubi_stream.py --src boot-rescue.img --vol /dev/ubi0_3
```

> 写之前先用 `ubiinfo` / `cat /proc/mtd` 确认卷号和大小。
> 写错卷 = 覆盖别的数据。

### 刷完必须回读校验

```sh
# 读回来算 md5，和主机上的镜像对比
md5sum /dev/ubi0_3   # 概念示例，实际用工具读
```

这块板的发布记录里，每次刷机都记录了：

- `boot-rescue.img` 回读 md5
- `rootfs.squashfs` 回读 md5
- `boot-normal.img` 回读 md5
- 整包索引 0–24 与 final7 一致

**没有回读校验的刷机，等于没刷。**

---

## 7.7 哪些东西绝对不能乱动

### 可以相对放心改的

| 层 | 位置 | 恢复方式 |
|---|---|---|
| rootfs 内容 | `ubi0_4` | 重刷 rootfs |
| 应用脚本 | `/etc/init.d/S??` | 重刷 rootfs 或 overlay 删除 |
| 内核 + DTB | `ubi0_3` | 重刷 boot |
| 用户数据 | `ubi0_5`（UDISK） | 备份恢复 |

### 不要动的

| 层 | 位置 | 为什么 |
|---|---|---|
| boot0 | `mtd0` | 坏了只能 FEL |
| U-Boot | `mtd1` | 坏了可能连 FEL 都难 |
| env | `ubi0_2` | bootargs 错会起不来 |
| boot_package / sunxi | 引导相关分区 | 厂商打包，不属于应用层 |

### 一条实用规则

> **只改你能“从串口/rescue 恢复”的东西。**
> 改 boot0 之前，先确认你会用 FEL。

---

## 7.8 启动脚本：最后一棒

### 顺序与依赖

| 脚本 | 做什么 | 依赖 |
|---|---|---|
| `S30persist` | 把 `/etc`、`/root` 等 bind 到 UDISK | UDISK 已挂载 |
| `S31hostname` | 恢复主机名 | `S30` |
| `S45wifi-conf` | 准备 Wi-Fi 配置 | UDISK |
| `S50wifideamon` | 启动 `wpa_supplicant` | 配置、驱动 |
| `S52e907` | 加载 `uart3pin.ko`，启动 E907 | debugfs、固件 |
| `S53linkr` | 启动 Python daemon | `/dev/rpmsg_ctrl0` |
| `S54bluetooth` | XR829 蓝牙 + `bluetoothd` + `linkr-ble` | Wi-Fi 固件、dbus |
| `S99postnet` | DHCP / NTP | 网络接口 |

### `S52e907` 的关键动作

```sh
# 概念版
mount -t debugfs none /sys/kernel/debug 2>/dev/null
insmod /lib/modules/.../uart3pin.ko
echo e907_firmware.elf > /sys/kernel/debug/remoteproc/remoteproc0/firmware
echo start > /sys/kernel/debug/remoteproc/remoteproc0/state
```

- `debugfs` 不挂，remoteproc 的控制文件就不存在。
- `uart3pin.ko` 不插，PE12/PE13 可能还不是 UART3。
- 固件名要和 `/lib/firmware/` 里的文件一致。
- `state` 写 `start` 才会真正释放 E907。

### 为什么 E907 不自己从 NAND 启动

- E907 没有 NAND 驱动、没有文件系统。
- 固件在 A7 的 `/lib/firmware/` 里。
- 必须 A7 先启动 Linux，再由 remoteproc 加载。

> 所以看到 `state` 是 `offline (0)`，先查 `S52` 有没有跑、
> 固件在不在、`uart3pin.ko` 有没有加载。

---

## 7.9 实验：完整观察一次启动

### 准备

```sh
picocom -b 115200 /dev/ttyACM0
```

给板子断电再上电，从头看到尾。

### 记录四件事

1. boot0 版本/日期。
2. U-Boot 版本、DRAM 大小。
3. 内核版本、DTB 名称。
4. 最后一个成功的 S 脚本。

### 启动完成后验证

```sh
cat /etc/rootfs_version
uname -a
cat /sys/kernel/debug/remoteproc/remoteproc0/state
ps w | grep -E 'linkr|bluetoothd|wpa'
curl -sk -o /dev/null -w '%{http_code}\n' https://127.0.0.1:8443/
```

预期：

```text
v851s-lizard-e907-v12-20261114
Linux ... 4.9.191 ...
running (2)
200
```

### 用 `dmesg` 找时间线

```sh
dmesg | grep -E 'Booting|remoteproc|rpmsg|ubi0|squashfs|overlay'
```

你会看到内核视角的启动顺序，和串口日志互相印证。

---

## 7.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 串口无任何输出 | 选错口、Type-C 插反、boot0 坏 | 换口、量电平、FEL |
| 卡在 U-Boot | env 的 bootcmd 错 | `printenv`、手动 `run boot_normal` |
| `Starting kernel` 后停 | console 参数、DTB 不匹配 | 改 bootargs、换 boot 镜像 |
| 内核找不到 rootfs | `ubi.mtd`/`root=` 错、卷坏 | rescue、检查 UBI |
| 到 init 停 | S 脚本死循环、缺文件 | 串口登录、`sh -x` 单步 |
| E907 offline | 固件缺失、uart3pin 没加载 | `ls /lib/firmware`、`lsmod` |
| 网页 502/打不开 | daemon 没起、证书缺 | `ps`、日志、`curl 127.0.0.1` |
| 刷完还是旧版本 | 写错卷、没 sync、overlay 旧文件 | 回读 md5、`cat /etc/rootfs_version` |
| 刷完起不来 | 动了 boot0/U-Boot/env | rescue / FEL，对照发布记录 |

---

## 7.11 练习

### 练习 7.1（必做）

完整记录一次启动日志，标出四个分界点，并回答：

1. boot0 和 U-Boot 各输出了什么版本信息？
2. 内核是什么时候接管串口的？
3. 第一个执行的 S 脚本是哪个？

### 练习 7.2（必做）

在 U-Boot 命令行里：

```text
printenv bootargs
printenv bootcmd
```

把输出抄下来，解释每一项的含义。**不要 `saveenv`。**

### 练习 7.3（必做）

在板子上模拟“E907 没起来”：

1. `cat state` 记录当前值。
2. 停掉相关服务（或直接不启动）。
3. 观察 `state`、`dmesg`、`trace0` 的变化。
4. 恢复。

> 如果你不确定怎么恢复，不要做这个练习。

### 练习 7.4（选做）

画一张“失败位置 → 恢复手段”的流程图，至少包含：

- 无输出 → FEL
- 卡 U-Boot → 修 env / 重刷 boot
- 卡 init → rescue / 单用户
- 服务没起 → 前台跑脚本

---

## 7.12 本章验收

1. 能按顺序说出启动链的六级，并说明每级职责。
2. 能根据串口最后一行判断卡在哪一级。
3. 能解释 `bootcmd`、`bootargs`、`boot_normal` 的关系。
4. 知道 FEL、rescue、U-Boot 命令行分别什么时候用。
5. 能说出哪些分区绝对不能乱刷，以及为什么。
6. 能独立完成一次 rescue 刷 rootfs + boot 的流程（在备份/可救的板子上）。

### 自测题

1. 为什么 E907 不能自己从 NAND 启动？
2. `Starting kernel` 之后没有日志，一定是内核没启动吗？
3. `setenv` 和 `saveenv` 有什么区别？
4. rescue 为什么不改 boot0/U-Boot？
5. 为什么刷完必须回读 md5？
6. `load_script.conf` 和 `rc.final` 哪个真正决定脚本顺序？

> 下一章进入存储：NAND、MTD、UBI、squashfs、overlay。
> 你会明白“为什么 rootfs 是只读的”“UDISK 为什么能保住配置”。
