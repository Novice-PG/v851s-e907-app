# V851S + Linkr Bee 小白讲解稿

> 面向听众：会一点 C（知道变量、指针、结构体、函数），
> 会用几条 Linux 命令（`ls`、`cat`、`ps`、`ssh`、`curl`），
> 但没有做过嵌入式，也没写过驱动。
>
> 事实基准：本仓库 `v12`（2026-11-14），板子为 YuzukiPI-Lizard
> （全志 V851S / sun8iw21p1），A7 跑 Tina Linux 4.9.191，
> E907 跑 Zephyr 4.5.0。
>
> 用法：这不是一份“读完就能背下来”的教材，而是一份**可以照着讲的稿子**。
> 每一节都按“一句话 → 口播 → 板书/比喻 → 演示 → 可能被问到”组织。
> 你可以只讲第 0～4 节（20 分钟），也可以讲完整版（45～60 分钟）。

---

## 0. 开场：先让听众知道“这东西有什么用”

### 一句话

**这块板子是一根“能上网、能蓝牙”的串口延长线。**

### 口播

> 大家先不要想 Linux、不要想 Zephyr。先想象一个很常见的场景：
> 你有一块设备，它只有一个串口，想看它的启动日志、想给它敲命令，
> 怎么办？
>
> 传统办法是：拿一台电脑，装 USB 转串口驱动，打开串口助手，
> 选对 COM 口，设置 115200、8、N、1，然后坐在设备旁边。
>
> 这台 V851S 做的事情是：把“串口助手”搬到浏览器和手机里。
> 设备还放在现场，你在办公室打开网页，就能看到它的输出、给它发命令；
> 手机靠近板子，也能通过蓝牙看到同一路串口。
>
> 所以你可以把它理解成一根“串口延长线”，只不过这根线
> 中间经过了 Wi-Fi、网页、蓝牙，还有两个 CPU。

### 板书

```text
浏览器 / 手机
      │  Wi-Fi / BLE
      ▼
   V851S 板子
      │  一根普通串口线（TX/RX/GND）
      ▼
   目标设备
```

### 演示（1 分钟）

打开 `https://<板子 IP>:8443/`，在页面终端里输入：

```text
@i?
```

让听众看返回的几行：

```text
@info fw version=v851s-lizard-e907-v12 ...
@info uart dropped=0 ... buffer=0/65536 ...
@info wifi state=connected ip=...
```

> 这几行不是网页编出来的，是板子里的程序真的去问了 E907、
> 问了 Wi-Fi，再把结果拼出来的。后面我们会一步一步拆开。

### 可能被问到

**问：为什么不直接买个 Wi-Fi 串口模块？**

> 可以，而且很多人这么干。但这块板子还能同时做网页终端、
> BLE 串口、文件上传、Wi-Fi 配置、远程诊断，而且源码全在手里，
> 适合做“可定制的远程串口”。这不是唯一方案，是一个能讲清楚
> 每一层的教学方案。

---

## 1. 一张图看懂全部数据流

### 一句话

**两条入口，一条串口，中间靠两个进程和两个 CPU 接力。**

### 口播

> 整个系统只有三条主要链路。第一条是浏览器到板子，走 HTTP/HTTPS
> 和 WebSocket；第二条是手机到板子，走 BLE；第三条是板子到目标设备，
> 走真正的串口。前两条最后都汇到同一个地方：`linkr-bee-daemon`。
>
> 你可以把 daemon 想成“总机”：浏览器打进来、手机打进来，它都接；
> 接完以后，它把字节交给 E907；E907 再把字节从 UART3 发出去。

### 板书

```text
浏览器 ──HTTPS/WebSocket──┐
                          ├── linkr-bee-daemon ── rpmsg ── E907 ── UART3 ── 目标板
手机 ────BLE GATT─────────┘
                          ▲
                          │
                     linkr-ble（BLE 翻译官）
```

三条链路的细节：

| 链路 | 走什么 | 谁负责 |
|---|---|---|
| 浏览器 ↔ daemon | HTTP、HTTPS、WebSocket | Python daemon |
| 手机 ↔ linkr-ble | BLE GATT | C + BlueZ |
| daemon ↔ E907 | rpmsg（共享内存 + 门铃） | Linux 驱动 + Zephyr |
| E907 ↔ 目标板 | UART3，TX/RX 两根线 | E907 固件 |

### 关键概念：串口不是网络

> 串口和网络最大的区别是：串口没有 IP 地址、没有端口号、没有“包”。
> 它就是一根线，按时间顺序把 bit 一位一位送出去。双方事先约好
> “每秒多少位、有没有校验位、有没有停止位”，这叫**波特率**和**帧格式**。
>
> 所以串口最怕两件事：**参数不对**（乱码）和**接收方来不及取数据**
> （丢字节）。后面的环形缓冲、背压、流控，都是围绕这两个问题。

### 可能被问到

**问：为什么手机和浏览器要分成两个进程？**

> 因为 BLE 要跟 BlueZ、D-Bus、GLib 打交道，用 C 写更贴近系统；
> 网页、Wi-Fi、命令解析这些业务逻辑用 Python 写更快。
> 两个进程通过 Unix socket 连接，坏一个不至于全挂。

---

## 2. 硬件：一块芯片里有两个 CPU

### 一句话

**A7 是经理，E907 是专职接线员，共享内存是两人的桌子，mailbox 是门铃。**

### 口播

> V851S 是一颗 SoC，片上系统。你可以把它想成一栋小楼：
> 楼里有大办公室、有小前台、有仓库、有电话交换机。
>
> 大办公室是 Cortex-A7，跑完整的 Linux，负责网络、网页、蓝牙、
> 文件、SSH。小前台是 E907，跑 Zephyr，只负责一件事：看着 UART3。
>
> 两个人不在同一个房间，怎么传话？楼里有一张共享的桌子（共享内存），
> 桌子上有收件盘和发件盘（vring），放好东西以后按一下对方的门铃
> （mailbox），说“有新东西”。门铃只传一个编号，不传数据本身。

### 硬件清单

| 部件 | 通俗解释 | 本板情况 |
|---|---|---|
| Cortex-A7 | 主 CPU，跑 Linux | 最高 912 MHz |
| E907 | 第二个 CPU，RISC-V，跑 RTOS | 600 MHz |
| DDR2 | 运行内存 | 64 MB，和 SoC 封装在一起 |
| SPI NAND | 断电不丢的闪存 | 128 MB |
| XR829 | Wi-Fi + 蓝牙二合一 | 2.4 GHz Wi-Fi + BLE |
| NPU | 神经网络加速器 | 本项目没用到 |
| MIPI | 摄像头/屏幕接口 | 本项目没用到 |

> 你不需要背这些数字。只需要记住：**A7 和 E907 是同一颗芯片里的
> 两个独立处理器，它们共享内存，但各自跑自己的程序。**

### 引脚：为什么串口不是一个“洞”

芯片的引脚大多可以切换功能，叫**引脚复用（pinmux）**。
同一个物理引脚，可以当 GPIO、I2C、PWM、UART。

本板相关的引脚：

| 信号 | 引脚 | 用途 |
|---|---|---|
| UART0 TX/RX | PH9 / PH10 | A7 调试控制台 |
| UART2 TX/RX | PD18 / PD19 | XR829 蓝牙 |
| UART3 TX/RX | PE12 / PE13 | E907 的目标串口 |

**必须提醒的坑：PE12 不能接到 UART0 的 TX0（PH9）。**

> 因为 PH9 已经在输出 Linux 调试日志了。你把 E907 的 TX 也接上去，
> 两个输出脚会“对打”：一个想拉高，一个想拉低，轻则乱码，重则损坏。
> 正确做法是：E907 的 UART3 只接独立的 USB-TTL，或者接目标板的
> 接收脚，并且目标板那一端不能再被别的 TX 驱动。

### Type-C 的两个“面”

这块板的 Type-C 口很特殊：

- 一面是 **CH343P USB 转串口**，接 A7 的 UART0，电脑上出现
  `/dev/ttyACM0`，这是 Linux 控制台。
- 另一面是 **SoC 的 USB OTG**，用于 FEL 刷机、USB 主机/从机。

> 所以“串口不见了”经常不是软件问题，而是 Type-C 插反了。
> 另外板上还有一个 CH340，接 E907 的 UART3，电脑上是 `/dev/ttyUSB0`，
> 用来单独看目标串口。

### 存储：128 MB NAND 怎么分

Linux 看到 4 个 MTD：

| MTD | 大小 | 内容 |
|---|---|---|
| `mtd0` boot0 | 1 MB | 第一级引导 |
| `mtd1` uboot | 3 MB | U-Boot |
| `mtd2` secure_storage | 1 MB | 安全存储 |
| `mtd3` sys | 123 MB | UBI 容器，里面再分卷 |

`sys` 上的 UBI 卷：

| 卷 | 设备 | 内容 |
|---|---|---|
| `ubi0_1` boot-res | 启动资源 | logo 等 |
| `ubi0_2` env | U-Boot 环境变量 | `bootargs` 等 |
| `ubi0_3` boot | Linux 内核 + 设备树 | 可单独更新 |
| `ubi0_4` rootfs | 只读 squashfs | 根文件系统 |
| `ubi0_5` UDISK | 可写数据分区 | 配置、证书、蓝牙绑定 |

> 为什么根文件系统要只读？
> 因为升级、掉电、误删都不容易把它弄坏。真正需要保存的东西，
> 放 UDISK。这样镜像里可以永远没有 Wi-Fi 密码和私钥。
>
> 根文件系统上面还盖了一层 overlayfs：读的时候先看只读层，
> 写的时候写到 UDISK 上的可写层。你可以理解成“透明桌布”：
> 原来的桌子不动，你在上面铺一张可以写的布。重启后布还在，
> 但重新刷 rootfs 时，布不会跟着镜像走。

### 可能被问到

**问：E907 为什么不直接从 NAND 启动？**

> 因为 E907 没有 NAND 驱动、没有文件系统，它的固件在 A7 的
> `/lib/firmware/e907_firmware.elf` 里。必须由 A7 先启动 Linux，
> 再由 Linux 的 remoteproc 把固件读出来、放到 E907 的内存里，
> 最后释放 E907。E907 是“被 A7 加载和放行的从核”。

---

## 3. 从按下电源到网页打开

### 一句话

**启动是一场接力赛：BROM → boot0 → U-Boot → Linux → init → 各个 S 脚本。**

### 口播

> 按下电源以后，芯片里固化的 BROM 先跑，它从 NAND 读第一级引导
> boot0；boot0 初始化内存和时钟，读 U-Boot；U-Boot 读环境变量，
> 从 `ubi0_3` 读 Linux 内核和设备树，启动内核。
>
> 内核挂载 `ubi0_4` 的根文件系统，执行 `/init`，然后 busybox init
> 按文件名顺序跑 `/etc/init.d/S??*`。这些 S 脚本就是接力赛的最后一棒：
> 挂 UDISK、起 Wi-Fi、起 E907、起网页 daemon、起蓝牙。

### 板书

```text
上电
 │
 ├─ BROM（芯片内固化代码）
 │    └─ 读 boot0
 ├─ boot0
 │    └─ 初始化 DRAM/时钟，读 U-Boot
 ├─ U-Boot（mtd1）
 │    ├─ 读 env
 │    ├─ 从 ubi0_3 读内核 + 设备树
 │    └─ 启动内核
 ├─ Linux 4.9.191
 │    ├─ 解析设备树，保留 E907 的 4 MiB 内存
 │    ├─ 挂载 rootfs（squashfs + overlay）
 │    └─ 执行 /init
 ├─ /etc/init.d/rcS → rc.final
 │    └─ 按顺序执行 S30、S31、S45、S50、S52、S53、S54、S99
 └─ 浏览器打开 https://<板子 IP>:8443/
```

启动脚本的顺序和原因：

| 脚本 | 做什么 | 为什么在这个位置 |
|---|---|---|
| `S30persist` | 把 `/etc`、`/root` 绑定到 UDISK | 后面所有服务都要用账号 |
| `S31hostname` | 恢复主机名 | init 已经先用了 rootfs 里的名字 |
| `S45wifi-conf` | 准备 Wi-Fi 配置/符号链接 | `wpa_supplicant` 之前 |
| `S50wifideamon` | 启动 Wi-Fi | 蓝牙要等 `wlan0` |
| `S52e907` | 加载 `uart3pin.ko`，启动 E907 | daemon 需要 rpmsg |
| `S53linkr` | 启动 Python daemon | E907 已就绪 |
| `S54bluetooth` | XR829 蓝牙 + bluetoothd + linkr-ble | Wi-Fi 固件已加载 |
| `S99postnet` | DHCP / NTP | 最后，网络已可用 |

### 为什么 A7 必须先启动

只讲三条，听众就能记住：

1. **固件在 A7 的文件系统里。** E907 自己读不到 NAND。
2. **内存和引脚要先由 A7 准备好。** A7 在设备树里保留 E907 的
   4 MiB 内存，还要把 PE12/PE13 切成 UART3。
3. **rpmsg 的“合同”要先由 A7 填好。** A7 把 vring 地址、状态写进
   资源表，E907 看到 `DRIVER_OK` 才能安全读写队列。

> 一句话：E907 不是“独立上电的第二个系统”，它是被 A7 加载的从核。

### `S52e907` 到底做了什么

```text
S52e907
  ├─ mount debugfs
  ├─ insmod uart3pin.ko          # PE12/PE13 -> UART3
  ├─ echo e907_firmware.elf > .../remoteproc0/firmware
  └─ echo start > .../remoteproc0/state
```

> `remoteproc` 是 Linux 的一个框架：你告诉它“固件叫什么、什么时候启动”，
> 它负责解析 ELF、拷贝代码、填资源表、释放 CPU。你不需要自己写
> 启动 E907 的汇编。

### 演示（2 分钟）

```sh
cat /etc/rootfs_version
cat /sys/kernel/debug/remoteproc/remoteproc0/state
tail -5 /sys/kernel/debug/remoteproc/remoteproc0/trace0
ps w | grep -E 'linkr|bluetoothd|wpa'
```

预期看到：

```text
v851s-lizard-e907-v12-20261114
running (2)
e907 heartbeat ...
```

> `running (2)` 是 remoteproc 的状态；`trace0` 是 E907 的日志。
> 注意 E907 的日志没有走 UART3，而是走内存控制台，因为 UART3
> 要留给目标设备。

### 可能被问到

**问：为什么 E907 的 `printk` 不直接打到串口？**

> 因为 UART3 是目标设备的串口。如果调试日志也写进去，目标设备
> 会收到一堆无关字符。所以 E907 用 RAM console，A7 通过 debugfs
> 的 `trace0` 去读。

---

## 4. 两个 CPU 怎么传字节：rpmsg 入门

### 一句话

**数据放桌上，vring 是收件盘，mailbox 是门铃。**

### 口播

> 假设 A7 要发一个字节给 E907。它不能直接调用 E907 的函数，
> 因为两个 CPU 各自跑各自的程序。它们只能通过共享内存传数据。
>
> 流程是三步：
> 第一步，A7 把数据放到共享内存的某个缓冲区；
> 第二步，A7 在 vring 里放一个“描述符”，写清楚“数据在哪个地址、多长”；
> 第三步，A7 按一下 mailbox 门铃，告诉 E907“有新东西”。
>
> E907 收到门铃中断，去看 vring，按描述符找到数据，处理完再把
> 缓冲区还给 A7。反方向也一样。

### 三个词

| 词 | 比喻 | 作用 |
|---|---|---|
| 共享内存 | 两个人共用的桌子 | 真正放数据的地方 |
| vring / virtqueue | 桌上的收件盘、发件盘 | 放“数据在哪、多长”的描述符 |
| mailbox | 门铃 | 只传一个通知号，不传数据 |

### 内存地图

| 区域 | 地址 | 大小 | 用途 |
|---|---|---|---|
| E907 固件 | `0x43c00000` | 4 MiB | 代码、数据、栈、`.bss` |
| rpmsg buffers | `0x43000000` | 256 KiB | 每条消息的 512 B 缓冲 |
| vring0 | `0x43040000` | 128 KiB | 一个方向 |
| vring1 | `0x43060000` | 128 KiB | 另一个方向 |
| UART3 | `0x02500c00` | 0x400 | ns16550 串口 |

> 这些地址不是随便写的，Linux 设备树的 `reserved-memory` 里
> 必须先保留它们，否则 Linux 会把普通数据写进去，两边踩内存。
> 这块 SoC 没有 IOMMU，所以设备地址就是物理地址。

### 一个字节的完整旅程

A7 → E907 → 目标板：

```text
浏览器输入 "ls\n"
  │
  ├─ WebSocket 二进制帧 -> daemon
  ├─ daemon 的发送队列
  ├─ daemon 按 256 B 切片，write(/dev/rpmsgN)
  ├─ Linux rpmsg_char 把数据放进 vring
  ├─ Linux 按 mailbox 门铃
  ▼
E907 mailbox 中断
  ├─ 把通知号放进消息队列
  ├─ openamp 线程取 descriptor
  ├─ bridge_cb() -> ring_buf_put(uart_tx_ring)
  └─ uart_tx 线程 -> UART3 TX
```

目标板 → E907 → A7：

```text
UART3 RX 中断
  ├─ 读 FIFO -> uart_rx_ring
  └─ uart_rx 线程
        ├─ ring_buf_get() 最多 256 B
        └─ rpmsg_sendto() -> E907 的 vring -> mailbox
              └─ A7 daemon -> WebSocket / BLE
```

### 为什么是 256 字节一片

> rpmsg 不是“想发多大就发多大”。Linux 的 rpmsg 缓冲是 512 字节，
> 减去 16 字节头部，实际能放的数据有限。`rpmsg_char` 对超过端点 MTU
> 的写会返回 `EMSGSIZE`（错误码 90）。
>
> 所以 daemon 把大块数据切成 256 字节一片。256 不是魔法数字，
> 是“留足余量、又不会太碎”的折中。

### 为什么要 64 KiB 环形缓冲

> 串口 115200 波特率，大约每秒 11.5 KB。64 KiB 能吸收约 5.7 秒的突发。
> 如果只给 4 KiB，网页一次粘贴一大段，或者目标板突然打印很多日志，
> 缓冲区很快就满，只能丢数据。
>
> 64 KiB 放在 E907 的 `.bss` 里，不占 NAND；E907 的 4 MiB 窗口
> 总共只用了约 180 KiB，所以“能变大就变大”没有代价。
> 1.5 Mbaud 下 64 KiB 约 437 ms，4 Mbaud 下约 164 ms。

### 控制协议：文本行

除了串口数据，A7 还要控制 E907：

| 方向 | 行 | 作用 |
|---|---|---|
| A7 → E907 | `LINKR1\n` | 握手，告诉 E907 A7 的端点地址 |
| A7 → E907 | `LINKRCFG 115200,8,n,1,n\n` | 改 UART3 参数 |
| A7 → E907 | `LINKRSTAT\n` | 查询缓冲占用和丢包 |
| E907 → A7 | `LINKRCFG OK uart=...` | 改参数成功 |
| E907 → A7 | `LINKRSTAT rx=... tx=... drop=...` | 统计结果 |

> 为什么用文本，不用二进制？
> 因为文本可以直接在日志里看懂，出问题时用 `cat`、`printf` 就能调试。
> 代价是解析稍微多几行代码，但在这个项目里完全值得。

### C 语言小课堂：环形缓冲

如果你只会一点 C，环形缓冲是最好的练习：

```c
/* 简化版：只演示“收” */
while (used > 0) {
    ch = ring[head];          /* 取一个字节 */
    head = (head + 1) % size; /* 头指针绕回去 */
    used--;
}
```

- `ring` 是一个数组，`size` 是 65536。
- `head` 是“读到哪里”，`tail` 是“写到哪里”。
- `% size` 让指针走到数组末尾后回到 0，所以叫“环”。
- 中断里只改 `tail`，线程里只改 `head`，各改各的，减少打架。
- 但“各改各的”不等于绝对安全：还需要原子变量或关中断保护，
  否则一个字节可能被覆盖。Zephyr 的 `ring_buf_put/get` 已经帮你做了。

> 学 C 的时候，指针、数组、结构体、位运算、`volatile`、原子操作，
> 这六个东西在这份代码里都会出现。你不需要一次全懂，先知道
> “它们是为了解决并发和硬件寄存器的问题”。

### 背压：满了怎么办

> 如果 A7 一次粘贴 100 KB，而串口只有 115200，TX 环很快满。
> E907 不会静默丢掉，而是先尽量放；放不下就等 `uart_tx_space_sem`，
> 最多等 100 ms；还放不下才丢，并累加 `uart_tx_dropped`。
>
> 这样浏览器看到的是“发送变慢”，而不是“数据悄悄少了”。
> 这就是背压：让上游慢下来，而不是让下游崩掉。

### 可能被问到

**问：mailbox 为什么不直接传数据？**

> 因为 mailbox 只是一个通知通道，通常只能传一个很小的编号。
> 传大块数据要走共享内存；mailbox 只负责说“去看桌子”。

---

## 5. E907 固件：四个 C 文件

### 一句话

**`main.c` 是入口，`board_early.c` 管引脚，`linkr_bridge.c` 管串口，`openamp_slave.c` 管 rpmsg。**

### 口播

> E907 的代码只有四个 `.c` 文件，非常适合作为嵌入式入门。
> 你不需要先懂 Zephyr 的全部 API，先看每个文件“负责哪一件事”。

| 文件 | 作用 |
|---|---|
| `src/main.c` | 打印 banner，初始化桥，启动 OpenAMP，主循环心跳 |
| `src/board_early.c` | 把 PE12/PE13 复用成 UART3，开时钟、释放复位 |
| `src/linkr_bridge.c` | 两个 64 KiB 环形缓冲，UART 收发线程，协议解析 |
| `src/openamp_slave.c` | E907 作为 rpmsg 从设备，mailbox 回调，端点池 |

### `main.c`：入口长什么样

```c
int main(void)
{
    printk("*** YuzukiPI-Lizard V851S E907 co-processor ***\n");
    (void)linkr_bridge_init();
    e907_openamp_start();

    while (1) {
        printk("e907 heartbeat %u cyc %llu\n", n++, k_cycle_get_64());
        k_msleep(30000);
    }
}
```

逐行解释：

- `printk` 输出到 RAM console，不是 UART3。A7 用 `trace0` 看。
- `linkr_bridge_init()` 创建 UART 收发线程。
- `e907_openamp_start()` 创建 OpenAMP 线程。
- 主循环每 30 秒打一次心跳，用来确认 E907 还活着。

> 在 Linux 里，“主线程”通常很忙；在 RTOS 里，主线程可以很闲，
> 真正干活的是它创建出来的线程。这是思维方式的差别。

### `board_early.c`：为什么满屏十六进制

```c
#define V851S_PIO_CFG_PE12_PE13   0x020000C4u
#define PE12_PE13_MUX7            0x00770000u
```

> 这不是“魔法数字”。它是在算：PE 这一组引脚的配置寄存器在
> `0x020000C0` 附近，PE12/PE13 在 `CFG1` 的 bit16..23，
> 把这两位都写成 7（UART3 的复用功能），就得到 `0x00770000`。
>
> 注释里把推导过程写清楚，是因为半年后你只看到 `0x00770000`
> 根本不知道它代表哪两个引脚。好的注释不是翻译代码，是记录
> “为什么是这个值”。

`SYS_INIT(..., PRE_KERNEL_1, 0)` 的意思是：在串口驱动初始化之前
执行这段代码。顺序很重要，引脚没切好，驱动就找不到 UART。

### `linkr_bridge.c`：中断只做最少的事

```c
static void uart_isr(...)
{
    while (uart_irq_is_pending(dev)) {
        n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) break;
        rx_push(buf, n);       /* 搬进环形缓冲 */
    }
    k_sem_give(&uart_rx_sem);  /* 唤醒线程 */
}
```

> 中断服务程序（ISR）要短。它只把 FIFO 里的字节搬进环形缓冲，
> 然后唤醒线程；真正处理数据、发 rpmsg 的事情交给线程。
> 如果 ISR 里做太多事，其他中断会被延迟，串口就可能丢字节。

为什么还要轮询兜底？

```c
(void)k_sem_take(&uart_rx_sem, K_MSEC(20));
uart_poll_rx();
```

> 这块板的 CLIC 中断路由曾经不稳定。轮询让“中断没来”只变成
> 20 ms 的延迟，而不是整条桥失效。这是典型的“硬件不确定，
> 软件做双保险”。

TX 路径：

```c
k_sem_take(&uart_tx_sem, K_FOREVER);
while ((got = ring_buf_get(&uart_tx_ring, buf, sizeof(buf))) > 0) {
    for (i = 0; i < got; i++)
        uart_poll_out(uart_dev, buf[i]);
    k_sem_give(&uart_tx_space_sem);
}
```

> `uart_poll_out()` 是忙等写一个字节。115200 下没问题；如果以后
> 要 4 Mbaud，建议改成中断或 DMA 发送，否则 CPU 会一直忙着等。

### `openamp_slave.c`：E907 是“从设备”

关键顺序：

1. 创建 virtio device（E907 是 device，A7 是 driver）。
2. 等 A7 在资源表里写 `DRIVER_OK`。
3. 用资源表里的地址初始化 vring0/vring1。
4. 初始化 rpmsg，注册 mailbox 回调。
5. 宣告端点 `rpmsg_chrdev`。

> 为什么必须等 `DRIVER_OK`？
> 因为 virtio 是双方协议。A7 先把 vring 地址、状态准备好，
> E907 看到 `DRIVER_OK` 才能安全读写。提前读会读到还没初始化的内存。

端点池 `LINKR_MAX_EPTS = 4` 是为了 A7 反复打开、关闭 `/dev/rpmsgN`
时，不会把正在用的端点覆盖掉。

### 构建

```sh
export ZEPHYR_BASE=/path/to/zephyr   # 带 V851S BSP 的分支
cmake -B build -GNinja -DBOARD=v851s_e907 .
ninja -C build
```

产物：

```text
build/zephyr/zephyr_e907.elf
```

仓库里保存为 `firmware/zephyr_e907.elf`，板内安装成
`/lib/firmware/e907_firmware.elf`（启动脚本写的名字）。
`.resource_table` 段必须存在，否则 Linux 拒绝启动 E907。

### 怎么读这份代码

1. 先看 `main.c`：知道 E907 启动后做什么。
2. 再看 `linkr_bridge.c`：知道字节怎么进出。
3. 最后看 `openamp_slave.c`：知道 rpmsg 怎么建立。
4. `board_early.c` 可以最后看，它只和硬件寄存器有关。

> 不要一行一行抠语法。先问三个问题：
> 这段代码在哪个 CPU 上跑？它读写的是内存、寄存器、文件还是 socket？
> 如果它失败了，谁会先发现？

---

## 6. A7 侧：Python daemon 和 BLE 翻译官

### 一句话

**daemon 是总机，linkr-ble 是 BLE 翻译官，两者用 Unix socket 连接。**

### 口播

> A7 侧有两个主要进程。`linkr-bee-daemon` 是 Python 写的，负责网页、
> WebSocket、HTTP/HTTPS、命令解析、Wi-Fi、文件上传。`linkr-ble`
> 是 C 写的，负责和 BlueZ 打交道，把 BLE 的写请求翻译成 daemon
> 能懂的帧。
>
> 你可能会问：为什么一个用 Python，一个用 C？
> 因为业务逻辑用 Python 改起来快；BLE 要跟 D-Bus、GLib、BlueZ
> 的时序打交道，用 C 更稳。两者通过 `/var/run/linkr-bee.sock`
> 解耦：BLE 崩了，网页还能用；daemon 重启，BLE 可以重连。

### 进程拓扑

```text
linkr-bee-daemon (Python)
 ├─ rpmsg 线程        /dev/rpmsgN <-> E907
 ├─ serial-tx 线程    限速写 rpmsg，防止粘贴淹没串口
 ├─ BLE socket 线程   /var/run/linkr-bee.sock <-> linkr-ble
 ├─ HTTP 线程         80
 ├─ HTTPS 线程        8443
 └─ 每个 WS 客户端一个线程

linkr-ble (C/GLib)
 ├─ D-Bus 连接        org.bluez / GattManager1
 ├─ GATT 服务注册     NUS / Management / Reliable UART
 └─ Unix socket 客户端 连到 daemon
```

### daemon 里值得认识的几个名字

| 名字 | 作用 |
|---|---|
| `EndpointInfo` / `RpmsgBridge` | 打开 `/dev/rpmsg_ctrl0`，创建端点，收发热数据 |
| `SerialWriter` | 有界队列，把网页的写和慢速 UART 解耦 |
| `ws_recv_message` | WebSocket 帧解析 |
| `Session` | 会话所有者、令牌、命令分发、持久化 |
| `Client` | 一个 WebSocket 客户端 |
| `BleClient` | 一个来自 linkr-ble 的连接 |
| `serve_file` / `handle_connection` | HTTP 静态文件 + WS 升级 |
| `serve_tls` | HTTPS 监听 |
| `serve_ble_socket` | BLE Unix socket 监听 |

### 为什么要有 `SerialWriter`

> 假设网页一次粘贴 100 KB，而串口只有 115200。如果 WebSocket
> 读线程直接写 rpmsg，它会阻塞好几秒。这期间你发 `@c?`、`@i?`
> 这些控制命令，全都排在后面，网页看起来像“卡死”。
>
> 所以读线程只负责把数据放进队列，发送线程按串口速度慢慢写。
> 队列满时丢弃并计数，保证控制命令不被大流量饿死。
> 这跟银行取号一样：不是让每个人挤到窗口，而是先取号排队。

### WebSocket：网页和 daemon 的语言

`GET /ws` 升级后：

- **二进制帧**：原始串口字节。
- **文本帧**：`@...` 控制命令。
- 连接建立后，服务端先发：

```text
@ws auth=required    # 配置了令牌
@ws auth=none        # 没配令牌
```

有令牌时，客户端必须在 3 秒内发一行文本令牌，否则断开。
令牌存在 `/mnt/UDISK/persist/ws-token`，权限 600。

常用命令：

| 命令 | 作用 |
|---|---|
| `@c?` | 查询当前会话所有者 |
| `@c claim` / `@c release` | 申请/释放控制权 |
| `@i?` | 诊断：固件、运行时间、UART 环、Wi-Fi、上传 |
| `@u?` / `@u=115200,8,n,1,n` | 查询/修改 UART3 |
| `@w?` / `@w scan` / `@w=ssid,pass` | Wi-Fi |
| `@s?` / `@s token` | LAN 令牌 |
| `@n?` / `@n=<name>` | BLE 广播名 |
| `@d?` / `@d=<url>[,user,pass]` | WebDAV 上传目标 |
| `@h` | 帮助 |

> `@i?` 里的 UART 数据不是假的：daemon 向 E907 发 `LINKRSTAT`，
> 解析回复，缓存 2 秒，再拼成 `@info ...` 行。这样诊断面板刷新时
> 不会每次都阻塞在 rpmsg 上。

### HTTP / HTTPS：为什么主入口是 8443

| 监听 | 行为 |
|---|---|
| `http://<board>/`（80） | 页面请求 301 到 HTTPS；`/ws` 仍可用明文 |
| `https://<board>:8443/` | 页面、`wss://.../ws`、安全上下文 |

> 为什么需要 HTTPS？
> 因为 Web Bluetooth 只在**安全上下文**里可用。在 `http://` 页面里，
> `navigator.bluetooth` 是 `undefined`，网页会报“Bluetooth LE
> transport is not available”。所以主入口是 8443。
>
> 证书是自签名、每板一份，优先放
> `/mnt/UDISK/persist/certs/`。私钥权限 600，不进镜像、不进 git。
> 浏览器第一次会警告，接受一次即可。

### 一个值得讲的工程故事：慢客户端

> 之前有一个隐藏问题：HTTPS 连接建立后，如果客户端很慢、甚至
> 一直不读数据，`sendall()` 可能永久阻塞。而 `broadcast()` 是跑在
> rpmsg 接收线程里的，结果就是：一个慢网页，把整条串口转发拖死。
>
> 修复分两步：给连接设 10 秒 `SO_SNDTIMEO`；把 TLS 握手放进
> 独立线程并限时 10 秒，避免一个只连接不说话的客户端卡住
> accept 循环。这个故事说明：**网络代码里，任何“等”都要有上限。**

### 会话所有权：一条串口只能一个人敲键盘

> 两个终端同时往串口打字，会互相插字。daemon 用“会话票据”解决：
> 第一个写串口的客户端自动成为 owner；也可以显式 `@c claim`；
> owner 3 秒不写，别人可以接管；`@c release` 后不会自动重新 claim。
>
> 这不是安全机制，只是“避免互相抢键盘”的礼貌规则。

### 持久化：哪些东西放 UDISK

| 路径 | 内容 |
|---|---|
| `/mnt/UDISK/wpa_supplicant.conf` | Wi-Fi 凭据 |
| `/mnt/UDISK/persist/ws-token` | LAN 令牌（600） |
| `/mnt/UDISK/persist/certs/` | HTTPS 证书/私钥 |
| `/mnt/UDISK/persist/ble-name` | BLE 广播名 |
| `/mnt/UDISK/persist/bluetooth/` | BLE 绑定/LTK |
| `/mnt/UDISK/persist/etc/passwd` 等 | 账号 |
| `/mnt/UDISK/persist/webdav-url` | WebDAV 目标 |
| `/mnt/UDISK/persist/target` | 目标标识 |

> `/var` 是内存盘，重启就没了，所以蓝牙绑定不能放 `/var`。
> BlueZ 这个版本用 `--localstatedir=/etc` 构建，绑定实际在
> `/etc/lib/bluetooth`；启动脚本把 UDISK 的 `persist/bluetooth`
> bind mount 到这个目录。这样重刷 rootfs 不会丢配对信息。

### 演示（3 分钟）

在网页终端依次输入：

```text
@i?
@u?
@c claim
@c?
@c release
```

或者用脚本发 WebSocket。重点让听众看：

- `@i?` 的 `buffer=0/65536` 是 E907 的真实环大小。
- `@u?` 返回当前 UART 参数。
- `@c claim` 后 `owner` 变成自己的 id。

---

## 7. 网页前端：它只是另一个客户端

### 一句话

**网页不是魔法，它就是一个会发 WebSocket 的 JavaScript 客户端。**

### 口播

> 网页来自 radxa 的 linkr-bee，不是我们从零写的。它本来是为另一个
> 板子准备的，我们打了 5 个补丁，让它在 V851S 的“HTTP/HTTPS 混合”
> 环境里也能用。

| 补丁 | 问题 | 解决 |
|---|---|---|
| 0001 | `crypto.randomUUID` 只在安全上下文存在 | 用 `getRandomValues` 生成 UUID |
| 0002 | 管理面板假设 BLE 传输 | LAN 模式下显示正确提示 |
| 0003 | `crypto.subtle` 在 HTTP 页面不存在 | 纯 JS SHA-256 兜底 |
| 0004 | HTTPS 页面需要 `wss://` | 根据页面 origin 推导 WS URL |
| 0005 | LAN 页面不自动查诊断 | 连接后自动 `@i?`/`@w?`/`@s?` |

> 这些补丁不是“炫技”，而是因为浏览器有安全限制：
> 在 `http://` 页面里，一些加密 API 不可用。网页要么降级，
> 要么报错。我们选择降级，让没有证书的板子也能用基本功能。

### 想改网页时

1. 改 `linkr-bee/web/` 里的文件。
2. 更新 `linkr-bee/patches/` 和快照。
3. 跑 `./linkr-bee/verify.sh`，确认 md5 对得上。
4. 不要直接改板内文件而不更新清单，否则下次验证会报 `DIFF`。

---

## 8. BLE：从 GATT 到字节流

### 一句话

**BLE 不是“无线串口”，它是一棵服务/特征值树；我们把串口字节塞进树的一个叶子里。**

### 先懂 6 个词

| 词 | 通俗解释 |
|---|---|
| GAP | 广播、扫描、连接这些“怎么找到对方”的规则 |
| GATT | 连接后“有哪些服务/特征值”的规则 |
| Service | 一组功能，比如“串口服务” |
| Characteristic | 服务里的一个数据点，可读/写/通知 |
| MTU | 一次 ATT 数据最大多少字节；默认 23，可协商到 517 |
| Notify / Indicate | 服务端主动推数据；Indicate 需要客户端确认，更可靠但更慢 |

> 你可以把 GATT 想成一个文件柜：Service 是抽屉，Characteristic
> 是抽屉里的文件夹。手机要发数据，就是往某个文件夹里写；
> 板子要推数据，就是往某个文件夹里放通知。

### 数据不是“直接写串口”

```text
手机写 characteristic
  -> ATT Write Request
  -> BlueZ (bluetoothd)
  -> D-Bus GattCharacteristic1.WriteValue
  -> linkr-ble 的 char_method_call()
  -> Unix socket -> daemon -> rpmsg -> E907 -> UART3
```

### 三个 GATT 服务

| 服务 | 用途 |
|---|---|
| Nordic UART Service（NUS） | 兼容通用 BLE 串口 App |
| Linkr Management Service v1 | 发 `@...` 命令、收结果/事件 |
| Linkr Reliable UART Service v1 | 带序号的可靠字节流 |

> NUS 是“兼容模式”：不装 Linkr Bee App，用任何通用 BLE 串口工具
> 也能连。Management 是“控制通道”，用来发 `@c?`、`@i?` 这些命令。
> Reliable UART 是“数据通道”，带 32 位序号，能发现重复和缺口。

### 12 字节帧头

Management 和 Reliable UART 都用一个 12 字节头：

```text
偏移  长度  含义
0     2     magic（"LK" 或 "LR"）
3     1     version / type
4     4     seq / reqid（小端）
8     2     payload length（小端）
10    2     flags（小端）
12    N     payload
```

> 为什么固定 12 字节？
> 因为 BLE 最小 ATT MTU 是 23，一次写最多 20 字节有效数据；
> 12 字节头 + 8 字节数据刚好能放进第一片。代码要求“第一片必须
> 带完整头”，这是唯一的实际限制。

### MTU：为什么不能固定发 20 字节

> BLE 4.0 默认 ATT MTU=23，一次只能发 20 字节有效数据。但现代手机
> 会协商更大的 MTU：Android 常见 512，HarmonyOS 常见 247，
> iOS 常见 185。
>
> 坑在于：BlueZ 发通知时，如果数据超过 `MTU-3`，它会**静默截断**。
> 所以 linkr-ble 不能写死一个“很大”的值，必须知道对端协商结果。
>
> 但 BlueZ 的 D-Bus API 不暴露当前 MTU。解决方法是给 bluetoothd
> 打一个小补丁，把每个 peer 的 MTU 写到
> `/var/run/linkr-bt-mtu/<bdaddr>`。linkr-ble 每秒读一次目录，
> 取最小值，然后按 `MTU-3` 分片。

### 吞吐优化：从 20 B 到 232 B

修复前：

- 每个 ATT 值 20 字节；
- Reliable 帧只有 64 字节，拆成 4 个 confirmed indication；
- 每片等 10 ms，每帧等 30 ms。

修复后：

- 分片大小 = `MTU-3`（20..514）；
- Reliable 帧载荷 232 字节（12+232=244，一个 MTU≥247 的 indication
  就能装下）；
- 每片 2 ms，每帧 15 ms；
- 起帧前最多等 10 ms，尽量凑满 232 字节。

> 为什么是 232，不是 512？
> 因为帧头 12 字节，244 字节正好能放进 MTU=247 的单个 indication；
> 再大就要拆成多个 indication，反而增加确认次数。232 是兼容性和
> 效率之间的折中。
>
> 实际速度还受对端 connection interval 限制，常见 15～30 ms。
> 手机 App 可以请求更短间隔，板子侧不能单方面决定。

### 广播数据：4.9 内核的坑

> BlueZ 通过 `MGMT Add Advertising` 把广播数据交给内核，但全志
> 4.9 内核把 `HCI_OP_LE_SET_ADV_DATA` 和
> `HCI_OP_LE_SET_SCAN_RSP_DATA` 两个命令注释掉了。结果 BlueZ 以为
> 注册成功，空中广播却是空的。
>
> 解决：`ble-adv-data.py` 在每次注册广播后，用 `hcitool` 直接发
> 原始 HCI 命令，把 AD（flags + Management UUID）和 scan response
> （名字）写进控制器。这是“内核不给你接口，就绕过内核直接和
> 控制器说话”的典型 workaround。

### EATT 和单连接

- **EATT 必须关**：Linux 4.9 没有 EATT PSM 支持。BlueZ 5.54 默认
  `EATTChannels=3`，会向对端宣告 EATT；Windows 连上来后走 EATT，
  结果连不上/看不到特征。配置写 `EATTChannels = 1`，启动脚本还会
  兜底改一次。
- **只能连一个中心设备**：XR829 控制器在已有 LE 连接时，
  `LE Set Advertise Enable` 返回成功，AD 数据也接受，但空中没有
  广播。第二个手机永远看不到板子。这是控制器固件行为，软件改不了。
  多终端请走 LAN/WSS。

### 演示（3 分钟）

```sh
hciconfig hci0
cat /var/log/linkr-ble.log
cat /var/run/linkr-bt-mtu/* 2>/dev/null
```

预期看到：

```text
UP RUNNING
GATT application registered
advertising as "Linkr Bee V851S"
```

手机用通用 BLE 串口 App 连 NUS，发一个字符；网页终端应能看到。
如果 Windows 连不上，先检查 `EATTChannels = 1`。

---

## 9. 安全与持久化：边界在哪里

### 一句话

**镜像是公开的，秘密只放 UDISK；局域网不是保险箱。**

### 口播

> 这套东西的设计原则是：镜像可以随便给，里面没有 Wi-Fi 密码、
> 没有私钥、没有令牌。真正的秘密在板子的 UDISK 分区里，每板一份。
>
> 但这不代表它“安全”。局域网里的网页能控制串口，令牌默认可能
> 是关闭的，HTTPS 是自签名，BLE 是 Just Works 配对。它适合
> 可信局域网和教学，不适合直接暴露到公网。

### 已经做的

- 镜像里没有 Wi-Fi 密码、没有 HTTPS 私钥、没有令牌。
- 私钥/令牌文件权限 600。
- HTTP 静态文件服务做了路径规范化，阻止 `../` 目录穿越。
- 根文件系统只读，掉电不容易坏。
- 令牌可选：设置后 WebSocket 必须带令牌。

### 需要你知道的风险

- 令牌走明文 `ws://` 时，局域网里可被看到；用 `wss://` 更好。
- 自签名证书只能防“被动偷看”，不能防“中间人”。
- BLE Just Works 没有 PIN，附近的人可能配对。
- 没有失败登录锁、没有入侵检测。
- 默认可能没有令牌；建议设置：

```text
@s token
```

### 持久化与刷机

- 重刷 rootfs 不会清 UDISK，所以 Wi-Fi、证书、绑定、SSH key 都还在。
- 但刷机前仍然建议备份 `/mnt/UDISK`。
- 蓝牙绑定在 `/mnt/UDISK/persist/bluetooth`，bind mount 到
  `/etc/lib/bluetooth`。不要手工删这个目录，否则配对信息会丢。
- `/var` 是 tmpfs，重启就清空；不要把需要保存的东西放 `/var`。

### 最小加固建议

1. 设置 LAN 令牌。
2. 改 root 密码。
3. 不要把 80/8443 直接映射到公网。
4. 不需要 BLE 时，关掉广播或停掉 `S54bluetooth`。
5. 定期备份 UDISK。

---

## 10. 演示脚本：5 分钟版和 20 分钟版

### 5 分钟版（适合开场或路演）

1. 亮板子，说明“串口延长线”。
2. 打开 `https://<板子 IP>:8443/`。
3. 输入 `@i?`，解释 `buffer=0/65536` 和 `wifi state=connected`。
4. 输入 `@u?`，解释当前串口参数。
5. 在终端里敲几个字符，让目标串口（或 USB-TTL）收到。
6. 手机连 BLE，发一个字符，网页能看到。
7. 收尾：“A7 管网络，E907 管串口，rpmsg 是桥。”

### 20 分钟版（适合课程）

| 时间 | 内容 | 演示 |
|---|---|---|
| 0–3 min | 开场、三条链路 | 打开网页 |
| 3–7 min | 硬件、两个 CPU、存储 | `cat /etc/rootfs_version` |
| 7–12 min | 启动链、S 脚本 | `cat state`、`trace0` |
| 12–17 min | rpmsg、环形缓冲 | `@i?`、`@u?` |
| 17–20 min | BLE、安全、总结 | `hciconfig`、手机 |

### 演示前的检查清单

```sh
# 1. 板子在线
ping <board-ip>

# 2. 服务在
ps w | grep -E 'linkr-bee-daemon|bluetoothd|linkr-ble'

# 3. 网页在
curl -sk -o /dev/null -w '%{http_code}\n' https://<board-ip>:8443/

# 4. E907 在
cat /sys/kernel/debug/remoteproc/remoteproc0/state

# 5. 蓝牙在
hciconfig hci0
```

### 演示时不要做的事

- 不要现场刷机。
- 不要现场改 `main.conf` 或重启 `S54bluetooth`，除非你留了后路。
- 不要把真实 Wi-Fi 密码投到屏幕上。
- 不要让手机和网页同时成为 owner，否则听众会看到“互相插字”。

---

## 11. 常见问题：听众最可能问什么

### 网页打不开

1. `ping <board-ip>` 通吗？
2. `wctl status` 有 IP 吗？
3. `ps w | grep linkr-bee-daemon` 在吗？
4. `curl http://127.0.0.1/` 返回 301 吗？
5. 8443 没起来：看 `/mnt/UDISK/persist/certs/` 是否成对存在。
6. 浏览器证书警告：接受；时间不对会报 not yet valid。

### BLE 找不到 / 连不上

1. `hciconfig hci0` 是否 `UP RUNNING`？
2. `cat /var/log/linkr-ble.log` 有 `advertising as` 吗？
3. `ble-adv-data.py` 是否执行成功？
4. Windows 连不上：确认 `EATTChannels = 1`。
5. 只能连一个：正常，XR829 限制。

### E907 没起来

1. `cat .../remoteproc0/state` 是 `offline (0)` 还是 `running (2)`？
2. `trace0` 有 banner 吗？
3. `lsmod | grep uart3pin`。
4. `/lib/firmware/e907_firmware.elf` 存在吗？
5. 资源表 / 内存地址是否和 DTS 一致？

### 串口乱码

1. 波特率、数据位、校验位、停止位是否一致？
2. 目标板是不是 3.3 V？TX/RX 是否交叉？
3. 有没有把 PE12 接到 PH9（UART0 TX）？绝对不要。
4. 地线接了吗？

### BLE 很慢

1. `/var/run/linkr-bt-mtu/` 里的 MTU 是多少？
2. 对端 connection interval 是多少（`btmon`）？
3. `@i?` 的 `uart dropped` 是否为 0？
4. Reliable UART 是否被订阅？没有订阅会走 NUS 兼容路径。

### 令牌忘了 / 想关掉

令牌文件在 `/mnt/UDISK/persist/ws-token`，权限 600。
可以通过网页/WS 发 `@s?` 查询，`@s token off` 关闭。
不要在公开场合投屏令牌。

### 刷机失败怎么办

- 先确认没有替换 final7 的 boot0/u-boot/env/boot_package/sunxi。
- 应用层只动 `ubi0_3`（boot）和 `ubi0_4`（rootfs）。
- rescue initramfs 里 `reboot` 不生效时，用 `reboot -f`。
- 刷机前备份 UDISK；刷 rootfs 不会清 UDISK，但操作失误可能。

---

## 12. 最后：怎么继续学

### 先记住一句话

> **先分清“谁在做什么”**：A7 做网络/网页/蓝牙，E907 做串口，
> rpmsg 是两者之间的桥，daemon/linkr-ble 是桥两端的翻译官。

### C 需要补什么

| 主题 | 在这份代码里的例子 |
|---|---|
| 指针、数组 | 环形缓冲、`ring_buf_get` |
| 结构体 | `EndpointInfo`、`uart_config` |
| 位运算 | 引脚复用寄存器 `0x00770000` |
| `volatile` / 原子 | 中断和线程共享的计数器 |
| 信号量 | `k_sem_take/give`，背压 |
| 大小端 | BLE 12 字节帧头 |

### Linux 需要补什么

| 主题 | 在这份代码里的例子 |
|---|---|
| 文件和权限 | `/dev/rpmsgN`、令牌 600 |
| socket | TCP 80/8443、Unix socket |
| 进程和线程 | daemon 的多个线程 |
| 日志 | `/var/log/linkr-bee.log` |
| 挂载 | overlayfs、bind mount |
| 设备树 | `reserved-memory`、UART3 |
| 系统启动 | busybox init、`S??` 脚本 |

### 推荐阅读顺序

1. `src/main.c` → E907 启动后做什么。
2. `src/linkr_bridge.c` → 串口字节怎么进出。
3. `src/openamp_slave.c` → rpmsg 怎么建立。
4. `linkr-bee-daemon` 的 `RpmsgBridge` / `SerialWriter` → A7 怎么接。
5. `linkr-bee-daemon` 的 `Session.handle_command()` → 命令怎么走。
6. `linkr_ble.c` 的 `char_method_call()` / `rel_send_tick()` → BLE 怎么走。
7. `S52e907` / `S53linkr` / `S54bluetooth` → 开机怎么把这些串起来。

### 读代码时问三个问题

1. 这段代码在**哪个 CPU** 上跑？
2. 它读/写的是**内存、寄存器、文件、socket 还是网络**？
3. 如果它失败了，**谁会先发现**（日志、超时、丢包计数、网页报错）？

> 把这三个问题回答出来，你就已经看懂这套系统了。
> 剩下的，是经验。

---

## 附录 A：关键文件地图

```text
v851s-e907-app/
├─ src/                         E907 固件（Zephyr）
│  ├─ main.c                    入口、心跳
│  ├─ board_early.c             引脚复用、时钟
│  ├─ linkr_bridge.c            UART + 环形缓冲 + 协议
│  └─ openamp_slave.c           rpmsg 从设备
├─ prj.conf                     Zephyr 配置
├─ CMakeLists.txt               构建入口
├─ firmware/zephyr_e907.elf     编译产物（板内 e907_firmware.elf）
├─ linkr-bee/a7/
│  ├─ linkr-bee-daemon          Python daemon
│  ├─ linkr_ble.c               GATT server 源码
│  ├─ linkr-ble                 编译后的 GATT server
│  ├─ S53linkr                  daemon 启动脚本
│  ├─ S54bluetooth              蓝牙启动脚本
│  ├─ main.conf                 BlueZ 配置
│  └─ ble-adv-data.py           广播数据 workaround
├─ linkr-bee/web/               网页静态文件
├─ linkr-bee/patches/           5 个前端补丁
└─ docs/
   ├─ EXPLAIN.zh-CN.md          完整技术讲解
   ├─ TALK.zh-CN.md             本讲解稿
   ├─ BUILD.md                  构建
   ├─ DEPLOY.md                 部署
   ├─ PROTOCOL.md               协议
   └─ REVIEW.md                 review 结论
```

## 附录 B：命令速查

```sh
# 系统
cat /etc/rootfs_version
uname -a
free
df -h

# E907
cat /sys/kernel/debug/remoteproc/remoteproc0/state
tail -20 /sys/kernel/debug/remoteproc/remoteproc0/trace0
lsmod | grep uart3pin

# 服务
ps w | grep -E 'linkr|bluetoothd|wpa'
cat /var/log/linkr-bee.log
cat /var/log/bluetooth-init.log
cat /var/log/hciattach.log

# 蓝牙
hciconfig hci0
cat /var/run/linkr-bt-mtu/* 2>/dev/null

# rpmsg
ls -l /dev/rpmsg*
cat /proc/mounts | grep bluetooth

# 网页
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1/
curl -sk -o /dev/null -w '%{http_code}\n' https://127.0.0.1:8443/

# 诊断（通过网页/WS）
@i?
@u?
@c?
@s?
```

## 附录 C：自测题（讲完可以问听众）

1. A7 和 E907 谁先启动？为什么？
2. `LINKR1` 是做什么的？
3. 为什么 daemon 要把数据切 256 字节？
4. 为什么 E907 的环形缓冲要 64 KiB？
5. `@i?` 里的 `buffer=0/65536` 是谁提供的？
6. 为什么主入口是 8443，不是 80？
7. Web Bluetooth 为什么在 HTTP 页面不可用？
8. BLE 的 MTU 是谁协商的？BlueZ 为什么要打补丁？
9. 蓝牙绑定为什么不能放 `/var`？
10. 为什么 PE12 不能接 PH9？

参考答案都在正文里。如果听众能答出 6 题以上，说明这套架构已经听懂了。

## 附录 D：术语表

| 术语 | 解释 |
|---|---|
| SoC | 片上系统，一颗芯片里含 CPU、内存控制器、外设 |
| A7 | ARM Cortex-A7，主 CPU，跑 Linux |
| E907 | 玄铁 RISC-V 小核，跑 Zephyr |
| RTOS | 实时操作系统，任务切换延迟可预测 |
| Zephyr | 一个开源 RTOS，支持多种架构 |
| rpmsg | 核间消息传递协议，基于 virtio |
| OpenAMP | 实现 rpmsg/virtio 的开源框架 |
| virtqueue / vring | 共享内存里的描述符环 |
| mailbox | 核间“门铃”，只传通知号 |
| remoteproc | Linux 加载/启动/停止从核的框架 |
| resource table | 从核 ELF 里描述资源（vring 等）的表 |
| UBI | NAND 上的卷管理系统 |
| squashfs | 只读压缩文件系统 |
| overlayfs | 只读根 + 可写层的联合文件系统 |
| GATT | BLE 的服务/特征值协议 |
| NUS | Nordic UART Service，通用 BLE 串口服务 |
| MTU | ATT 最大传输单元 |
| notify | 无需确认的推送 |
| indicate | 需要确认的推送 |
| EATT | Enhanced ATT，4.9 内核不支持 |
| hciattach | 把 UART 上的蓝牙控制器接进 BlueZ 的工具 |
| wpa_supplicant | Wi-Fi 认证客户端 |
| WebSocket | 浏览器和服务器的双向字节流 |
| 安全上下文 | HTTPS/localhost 等浏览器认为可信的环境 |
