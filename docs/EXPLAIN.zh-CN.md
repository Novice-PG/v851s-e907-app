# 从零看懂这台 V851S：硬件、启动、Zephyr、Linkr Bee 与全部代码

> 这份文档写给**完全没接触过嵌入式、也没写过驱动**的人。
> 你不需要先懂 Linux 内核、RISC-V、蓝牙协议；文中遇到新词会先解释，
> 再告诉你它在哪个文件、哪一行、为什么这么写。
>
> 事实基准：本仓库的 v12 版本（2026-11-13），板子为 YuzukiPI-Lizard
> （全志 V851S / sun8iw21p1），A7 跑 Tina Linux 4.9.191，E907 跑
> Zephyr 4.5.0。不同日期的镜像可能在版本号、地址上有差异，但架构不变。

---

## 0. 先记住三句话

1. **A7 是“经理”**：跑 Linux，负责网络、网页、蓝牙、文件、升级。
2. **E907 是“专职接线员”**：跑 Zephyr，只负责一根串口（UART3），
   把串口字节原样转给 A7，把 A7 的字节原样写到串口。
3. **Linkr Bee 是“前台”**：把这条串口包装成网页终端和 BLE 串口，
   让浏览器或手机能像坐在设备旁边一样敲命令。

三条数据流：

```text
浏览器 ──HTTP(S)/WebSocket──┐
                            ├── linkr-bee-daemon ── rpmsg ── E907 ── UART3 ── 目标板
手机 ──BLE GATT── linkr-ble ┘
```

另一条控制流：

```text
浏览器/手机 ── "@u=115200,8,n,1,n" ── daemon ── "LINKRCFG ..." ── E907
                                                              └── 重新配置 UART3
```

---

## 1. 硬件：这台板子到底有什么

### 1.1 一块 SoC，两个 CPU

V851S 是一颗“片上系统”（SoC）。你可以把它理解成一块芯片里塞进了：

| 部件 | 通俗解释 | 本板实测/配置 |
|---|---|---|
| Cortex-A7 | 主 CPU，跑完整 Linux | 最高 912 MHz（可调 408/600/720/912） |
| E907 | 第二个 CPU，RISC-V 小核，跑 RTOS | 600 MHz，RV32IMAC + 玄铁扩展 |
| DDR2 | 内存，程序运行的地方 | 64 MB，和 SoC 封装在一起（SIP） |
| SPI NAND | 闪存，断电后数据还在 | 128 MB |
| NPU | 神经网络加速器 | 0.5 TOPS，本项目的终端功能没用到 |
| XR829 | Wi-Fi + 蓝牙二合一芯片 | 2.4 GHz Wi-Fi + BT/BLE |
| MIPI CSI/DSI | 摄像头/显示屏接口 | 本板有，本终端功能没用到 |

> “SoC”“SIP”“RTOS”这些词不用背。你只需要知道：
> **A7 和 E907 是同一块芯片里的两个独立处理器，共享内存和外设。**

### 1.2 A7 和 E907 的分工

**A7 侧（Linux）**

- 有文件系统、进程、网络协议栈、Python、BlueZ、Web 服务器。
- 负责：Wi-Fi、HTTP/HTTPS、WebSocket、BLE GATT、SSH、日志、升级。
- 跑的是 Linux 4.9.191（全志 Tina BSP），用户态是 Tina/OpenWrt 风格。

**E907 侧（Zephyr）**

- 没有文件系统、没有进程、没有 MMU；只有线程、信号量、环形缓冲。
- 负责：UART3 的收发、rpmsg 端点、运行时改串口参数、统计缓冲占用。
- 跑的是 Zephyr 4.5.0，一个很小的实时操作系统（RTOS）。

**为什么不让 A7 直接管 UART3？**

技术上可以，但这里有三个现实原因：

1. **引脚归属**：这块板的 UART3 在 PE12/PE13 上，是复用引脚；
   BSP 把 UART3 设计成 E907 的外设，Linux 的 `uart3` 节点是关掉的。
2. **实时性**：Linux 会被网络、Python、蓝牙抢占；串口在高波特率下
   需要及时把 FIFO 读走，否则丢字节。E907 只做这一件事，延迟稳定。
3. **隔离**：网页/蓝牙再忙，也不会把目标串口堵死；E907 的 64 KiB
   环形缓冲可以吸收突发数据。

> 这不是“唯一正确”的设计，而是这块板 + 这个 BSP 下最省事、最稳的设计。
> 如果换一块 A7 直接引出 UART 的板子，完全可以让 Linux 直接管串口。

### 1.3 引脚：为什么串口不是一个“洞”

芯片的引脚大多可以切换功能，叫**引脚复用（pinmux）**。
同一个物理引脚，可以当 GPIO、I2C、PWM、UART……

本板相关的引脚：

| 信号 | SoC 引脚 | 用途 | 注意 |
|---|---|---|---|
| UART0 TX/RX | PH9 / PH10 | A7 调试控制台 | 板载 CH343P USB 转串口 |
| UART2 TX/RX | PD18 / PD19 | XR829 蓝牙 | 还有 RTS/CTS：PD20/PD21 |
| BT_RSTN | PE8 | 蓝牙复位 | 由 sunxi-bt 驱动控制 |
| UART3 TX/RX | PE12 / PE13 | **E907 的目标串口** | 多功能引脚，默认没启用 |
| Wi-Fi SDIO | SDC1 | XR829 Wi-Fi | 和蓝牙共用一颗芯片 |
| USB OTG | USB0 | Type-C | 反面是 SoC USB，正面是 CH343P |

**关键坑：PE12 不能接到 UART0 的 TX0（PH9）。**

因为 UART0 的 TX 已经在输出调试日志，PE12 再输出 E907 的串口数据，
两个输出脚接在一起就会“对打”（一个拉高、一个拉低），轻则乱码，重则
损坏。正确做法是：E907 的 UART3 只接独立的 USB-TTL，或者接目标板的
UART0（并且目标板那一端不能再被别的 TX 驱动）。

### 1.4 Type-C 的两个“面”

这块板的 Type-C 口很特殊：

- 一面是 **CH343P USB-UART**，接 A7 的 UART0，主机上出现 `/dev/ttyACM0`
  （或 `ttyACM1`），这是板子的 Linux 控制台。
- 另一面是 **SoC 的 USB OTG**，用于 FEL 刷机、USB 主机/从机。

所以“串口不见了”经常不是软件问题，而是 **Type-C 插反了**。
主机上看到 `1a86:55d3 QinHeng USB Single Serial` 才是 CH343P 那一面。

另外，板上还有一个 CH340（`1a86:7523`，`/dev/ttyUSB0`），
它接的是 E907 的 UART3（PE12/PE13），用于单独看目标串口。

### 1.5 存储：128 MB NAND 怎么分

板内是 SPI NAND，Linux 看到 4 个 MTD：

| MTD | 大小 | 内容 |
|---|---|---|
| `mtd0` boot0 | 1 MB | 全志 BROM 之后的第一级引导 |
| `mtd1` uboot | 3 MB | U-Boot |
| `mtd2` secure_storage | 1 MB | 安全存储 |
| `mtd3` sys | 123 MB | UBI 容器，里面再分卷 |

`sys` 上的 UBI 卷：

| UBI 卷 | 设备 | 内容 |
|---|---|---|
| `ubi0_1` boot-res | `/dev/by-name/boot-res` | 启动 logo/资源 |
| `ubi0_2` env | `/dev/by-name/env` | U-Boot 环境变量 |
| `ubi0_3` boot | `/dev/by-name/boot` | Linux 内核 + 设备树 |
| `ubi0_4` rootfs | `/dev/by-name/rootfs` | **只读 squashfs 根文件系统** |
| `ubi0_5` UDISK | `/dev/by-name/UDISK` | **可写数据分区**，放配置/证书/蓝牙绑定 |

板内实际挂载大致是：

```text
/rom                 squashfs，只读，约 35.8 MB
/                    overlayfs，可写层在 /mnt/UDISK/overlay
/mnt/UDISK           ubifs，约 40.7 MB 可用
/var                 tmpfs（内存），重启就没了
/etc/passwd 等       从 /mnt/UDISK/persist/etc 绑定的文件
```

**为什么要这么分？**

- 根文件系统是只读的，升级/掉电不容易坏。
- 真正需要保存的东西（Wi-Fi 密码、SSH key、BLE 绑定、HTTPS 私钥）
  放 UDISK；镜像里永远没有真实凭据。
- `/var` 是内存盘，所以蓝牙绑定、PID、socket 这些要么重建，
  要么显式绑定到 UDISK。

### 1.6 电源与刷机

- 正常启动：boot0 → U-Boot → Linux。
- 刷机：可以进 FEL（USB OTG）用 PhoenixSuit 整片刷；
  也可以从 U-Boot 用 XMODEM 载入 rescue 镜像，再从 rescue 里写 UBI 卷。
- **不要替换 final7 的 boot0/u-boot/env/boot_package/sunxi**，
  这些是板子能启动的根。应用层只动 `ubi0_3`（boot）和 `ubi0_4`（rootfs）。

---

## 2. 从按下电源到网页打开：完整启动链

### 2.1 一张总图

```text
上电
 │
 ├─ BROM（芯片内固化代码）
 │    └─ 从 NAND 读 boot0
 │
 ├─ boot0（第一级引导）
 │    └─ 初始化 DRAM、时钟，读 U-Boot
 │
 ├─ U-Boot（mtd1）
 │    ├─ 读 env（bootargs、bootcmd）
 │    ├─ 从 ubi0_3 读 Linux 内核 + 设备树
 │    └─ 启动内核
 │
 ├─ Linux 内核 4.9.191
 │    ├─ 解析设备树：保留 E907 的 4 MiB 内存、vring/共享内存
 │    ├─ 挂载 rootfs（ubi0_4，squashfs + overlay）
 │    ├─ 执行 /init
 │    └─ 启动 busybox init
 │
 ├─ /etc/init.d/rcS
 │    ├─ 挂载 /mnt/UDISK
 │    ├─ 加载 xradio Wi-Fi 模块
 │    └─ rc.final：按文件名顺序执行所有 S?? 脚本
 │
 ├─ S30persist      把 /etc/passwd 等绑定到 UDISK
 ├─ S31hostname     恢复主机名
 ├─ S45wifi-conf    准备 Wi-Fi 配置（凭据只在 UDISK）
 ├─ S50wifideamon   启动 wpa_supplicant
 ├─ S52e907         加载 uart3pin.ko，启动 E907
 ├─ S53linkr        启动 Python daemon（HTTP/HTTPS/WS/BLE socket）
 ├─ S54bluetooth    XR829 蓝牙 + bluetoothd + linkr-ble
 └─ S99postnet      DHCP / NTP
 │
 └─ 浏览器打开 https://<board-ip>:8443/
```

> 注意：这个镜像里 `load_script.conf` 是空的，真正按顺序跑脚本的是
> `rc.final` 里的 `for i in /etc/init.d/S??*; do $i start; done`。
> `S52e907` 同时实现了 `start()` 和 `boot()`，所以从哪条路径调用都能工作。

### 2.2 为什么 A7 必须先启动，E907 后启动

这是整个项目最重要的架构点之一。顺序不能反，原因有 6 条：

1. **E907 的固件存在 A7 的文件系统里。**
   E907 没有 NAND 驱动、没有文件系统。它的固件是
   `/lib/firmware/e907_firmware.elf`，只有 Linux 能读出来。

2. **E907 的启动地址、时钟、复位由 A7 写寄存器控制。**
   `sunxi_rproc_boot.c` 里，A7 把入口地址写到 `0x06010204`，
   再操作 CCU 的时钟门控、复位、mailbox 复位寄存器，最后释放 CPU。
   E907 自己没有一个“上电自动从 NAND 启动”的路径。

3. **共享内存的“合同”要先由 A7 准备好。**
   A7 的设备树在 `reserved-memory` 里保留了：
   - E907 代码/数据：`0x43c00000`，4 MiB
   - rpmsg 缓冲：`0x43000000`，256 KiB
   - vring0：`0x43040000`，128 KiB
   - vring1：`0x43060000`，128 KiB

   这些区域标了 `no-map`，Linux 不会拿去当普通内存。E907 启动后
   直接按这些地址读写；如果 A7 没先保留，Linux 可能把数据写进去，
   两边就踩内存了。

4. **资源表（resource table）要由 Linux 填好。**
   E907 的 ELF 里有一个 `.resource_table` 段，描述“我有一个 rpmsg
   设备、两个 vring”。Linux 的 remoteproc 会解析 ELF、把各段拷贝到
   目标地址、把 vring 的实际地址写回资源表，然后才启动 E907。

5. **UART3 的引脚复用要先由 A7 做。**
   `S52e907` 先 `insmod uart3pin.ko`，把 PE12/PE13 切成 UART3
   （mux 7）。E907 固件里的 `board_early.c` 也会写同样的 PIO/CCU
   寄存器作为兜底，但 A7 这一次先发生。
   （厂商文档对“E907 能不能写 PIO”说法不一致，所以代码里两边都做，
   以先执行的 A7 为准；这是典型的“硬件不确定，软件做双保险”。）

6. **Linux 要先建立 rpmsg 字符设备。**
   E907 启动后会宣告一个叫 `rpmsg_chrdev` 的端点，Linux 的
   `rpmsg_char` 驱动据此创建 `/dev/rpmsg_ctrl0`。daemon 要等这个
   节点出现，才能创建 `/dev/rpmsgN` 并握手。

一句话：**E907 是“被 A7 加载和放行的从核”，不是独立上电的第二个系统。**

### 2.3 remoteproc 启动 E907 的详细过程

对应代码：`S52e907` → `/usr/bin/start_e907` → 内核
`drivers/remoteproc/sunxi_rproc.c`、`sunxi_rproc_boot.c`。

```text
S52e907
  │
  ├─ mount debugfs
  ├─ insmod uart3pin.ko          # PE12/PE13 -> UART3
  ├─ echo e907_firmware.elf > /sys/kernel/debug/remoteproc/remoteproc0/firmware
  └─ echo start > .../state
        │
        ▼
Linux remoteproc
  ├─ request_firmware("e907_firmware.elf")
  ├─ 解析 ELF，把 PT_LOAD 段拷到 0x43c00000
  ├─ 找到 .resource_table
  ├─ 把 vdev0vring0/vdev0vring1 的地址、大小写进资源表
  ├─ 把资源表写回 E907 内存
  └─ sunxi_rproc_start()
        ├─ 设置入口地址 0x06010204 = bootaddr
        ├─ 设置 600 MHz
        ├─ 复位/释放时钟、CPU、mailbox
        └─ E907 开始执行
```

E907 启动后：

```text
Zephyr 启动
  ├─ PRE_KERNEL_1: board_early.c 配 UART3 引脚/时钟
  ├─ 驱动初始化：ns16550、mailbox、IPM
  ├─ main(): 打印 banner，初始化 UART 桥，创建 openamp 线程
  └─ openamp 线程:
       ├─ 创建 virtio device（VIRTIO_DEV_DEVICE）
       ├─ 等 Linux 在资源表里写 DRIVER_OK
       ├─ 初始化 vring0 / vring1
       ├─ rpmsg_init_vdev()
       ├─ 注册 mailbox 回调
       └─ 宣告 "rpmsg_chrdev"
```

> 为什么 E907 要“等 DRIVER_OK”？
> 因为 virtio 是双方协议：A7 先把 vring 地址、状态准备好，E907 看到
> `DRIVER_OK` 才能安全地读写队列。提前读会读到还没初始化的内存。

### 2.4 启动后有哪些进程/设备

```text
A7 Linux
 ├─ /usr/bin/linkr-bee-daemon   Python，80/8443，/var/run/linkr-bee.sock
 ├─ /usr/bin/bluetoothd -n      BlueZ，D-Bus，GATT 数据库
 ├─ /usr/bin/linkr-ble          C/GLib，GATT server
 ├─ wpa_supplicant              Wi-Fi
 └─ sshd                        SSH

设备节点
 ├─ /dev/rpmsg_ctrl0            rpmsg 控制节点
 ├─ /dev/rpmsgN                 daemon 创建的端点
 ├─ /dev/ttyS2                  XR829 蓝牙 UART
 ├─ /dev/ttyS0                  A7 调试控制台
 └─ /dev/ubi0_*                 UBI 卷

E907 Zephyr
 ├─ uart_rx 线程                 UART3 -> rpmsg
 ├─ uart_tx 线程                 rpmsg -> UART3
 └─ openamp_rx 线程              mailbox -> virtqueue
```

---

## 3. 核间通信：OpenAMP / rpmsg / mailbox 到底是什么

### 3.1 三个概念，先用生活比喻

- **共享内存**：两个人在同一张桌子上放纸条。
- **vring（virtqueue）**：桌子上的“收件盘”和“发件盘”，每个盘子有
  固定数量的格子（descriptor），格子里写“数据在哪个地址、多长”。
- **mailbox（信箱/门铃）**：一个人放好纸条后，按一下对方的门铃，
  说“有新东西”。门铃只传一个数字（notify id），不传数据本身。

所以一次发送是：

```text
发送方：把数据放进共享内存 -> 在 vring 里放一个 descriptor
      -> 按 mailbox 门铃（notify id）
接收方：门铃中断 -> 看 vring -> 按 descriptor 去共享内存取数据
```

### 3.2 内存地图

| 区域 | 地址 | 大小 | 用途 |
|---|---|---|---|
| E907 固件 | `0x43c00000` | 4 MiB | 代码、数据、栈、`.bss` |
| rpmsg buffers | `0x43000000` | 256 KiB | 每条消息的 512 B 缓冲 |
| vring0 | `0x43040000` | 128 KiB | 一个方向的 virtqueue |
| vring1 | `0x43060000` | 128 KiB | 另一个方向 |
| UART3 | `0x02500c00` | 0x400 | ns16550 串口 |
| E907 CLIC | `0x30800000` | 0x5000 | 中断控制器 |
| mailbox RX/TX | `0x06020000` / `0x03003000` | 0x1000 | 门铃寄存器 |
| E907 入口寄存器 | `0x06010204` | 4 B | A7 写入启动地址 |

> `DA == PA`：这块 SoC 没有 IOMMU，设备地址和物理地址一样，
> 所以 E907 可以直接用这些数值当指针。设备树里不能写
> `iommus`，否则 Linux 会以为要走 IOMMU，启动失败。

### 3.3 一次“A7 发串口字节”的完整过程

```text
浏览器输入 "ls\n"
  │
  ├─ WebSocket 二进制帧 -> daemon
  ├─ daemon 的 SerialWriter 队列
  ├─ RpmsgBridge.send() 按 256 B 切片
  ├─ write(/dev/rpmsgN) -> Linux rpmsg_char
  ├─ Linux 把数据放进 vring0 的 buffer
  ├─ Linux sunxi_rproc_kick() -> mailbox 写 notify id
  │
  ▼  （E907 的 mailbox 中断）
E907 ipm_rx_cb()
  ├─ 把 notify id 放进 k_msgq
  └─ openamp_rx 线程: rproc_virtio_notified()
        ├─ 从 vring 取 descriptor
        ├─ bridge_cb() -> linkr_bridge_rpmsg_recv()
        └─ ring_buf_put(uart_tx_ring)
              └─ uart_tx 线程 -> uart_poll_out() -> UART3 TX
```

反方向（目标板输出）：

```text
UART3 RX 中断
  ├─ uart_isr() 读 FIFO -> rx_push() -> uart_rx_ring
  └─ uart_rx 线程
        ├─ 每 20 ms 也会轮询一次（防止中断没路由）
        ├─ ring_buf_get() 最多 256 B
        └─ rpmsg_sendto() -> E907 的 vring1 -> mailbox
              └─ A7 收到 -> daemon -> WebSocket / BLE
```

### 3.4 mailbox 代码

E907 侧：`src/openamp_slave.c`

```c
static int mailbox_notify(void *priv, uint32_t id)
{
    static uint32_t notify_id;   /* 必须是静态存储 */
    notify_id = id;
    return ipm_send(ipm_dev, 0, id, &notify_id, sizeof(notify_id));
}
```

为什么 `notify_id` 要 `static`？
`ipm_send()` 可能只是把指针记下来，等驱动稍后再拷贝。如果指向栈上的
局部变量，函数返回后那块栈可能被覆盖，驱动读到的就是垃圾。注释里
专门写了这一点，这是“注释解释为什么”的典型例子。

接收侧：

```c
static void ipm_rx_cb(...)
{
    uint32_t notifyid = VRING1_ID;
    if (data != NULL)
        notifyid = *(volatile uint32_t *)data;

    if (k_msgq_put(&ipm_msgq, &notifyid, K_NO_WAIT) != 0)
        atomic_set(&ipm_overflow, 1);
}
```

中断里不能做太多事，所以只把 notify id 塞进队列，由线程处理。
如果队列满，不能假装没发生——置一个 `ipm_overflow` 标志，线程处理完
队列后再把两个 vring 都扫一遍。这样“丢一次门铃”最多多扫一次，
不会把 virtqueue 永久卡死。

### 3.5 Linux 侧：`/dev/rpmsg_ctrl0` 和 `/dev/rpmsgN`

Linux 的 `rpmsg_char` 驱动提供：

- `/dev/rpmsg_ctrl0`：控制节点，用来创建/销毁端点。
- `/dev/rpmsgN`：创建出来的端点，读写它就是收发 rpmsg 消息。

daemon 里的 `RpmsgBridge` 做了三件事：

```python
# 1. 打开控制节点
ctrl = os.open("/dev/rpmsg_ctrl0", os.O_RDWR)

# 2. ioctl 创建端点，名字必须是 "rpmsg_chrdev"
info.name = b"rpmsg_chrdev"
info.src = 0xFFFFFFFF   # RPMSG_ADDR_ANY
info.dst = 0
fcntl.ioctl(ctrl, RPMSG_CREATE_EPT_IOCTL, bytes(info))

# 3. 扫描 /dev，找到新出现的 /dev/rpmsgN
#    然后写 "LINKR1\n" 握手
```

`RPMSG_CREATE_EPT_IOCTL` 的值是 `0x4028B501`，这是 Linux 4.9 的
`_IOW(0xb5, 0x1, struct rpmsg_endpoint_info)`。结构体是：

```c
struct rpmsg_endpoint_info {
    char name[32];
    uint32_t src;
    uint32_t dst;
};
```

### 3.6 为什么是 256 字节一片

Linux rpmsg 的默认缓冲区是 512 字节，其中 16 字节是 rpmsg 头。
`rpmsg_char` 对超过 MTU 的写会返回 `EMSGSIZE`（errno 90）。
所以 daemon 用 256 字节：

```python
self.max_chunk = 256
```

E907 侧的 `RX_CHUNK` 也是 256，UART 线程一次最多搬 256 字节。
这样两边都不需要处理“超长消息拆分”的复杂逻辑。

### 3.7 rpmsg 上的控制协议

普通串口字节是“透明”的：收到什么就转发什么。
但桥本身还需要控制，所以保留了几个文本前缀：

| 方向 | 内容 | 作用 |
|---|---|---|
| A7 → E907 | `LINKR1\n` | 握手，告诉 E907 A7 的端点地址 |
| E907 → A7 | `LINKR1 OK rx=<n>\n` | 握手回复 |
| A7 → E907 | `LINKRCFG <baud>,<data>,<parity>,<stop>,<flow>\n` | 改 UART3 |
| A7 → E907 | `LINKRCFG ?\n` | 查询当前配置 |
| E907 → A7 | `LINKRCFG OK uart=...\n` | 改成功 |
| E907 → A7 | `LINKRCFG ERR=<errno>\n` | 改失败 |
| A7 → E907 | `LINKRSTAT\n` | 查询环形缓冲 |
| E907 → A7 | `LINKRSTAT rx=used/size tx=used/size drop=rx/tx\n` | 统计 |

**握手前是 echo 模式**：这样 `rpmsg_test` / `rpmsg_bench` 这些独立
测试工具还能用；握手后才进入“串口桥”模式。

**控制行为什么用文本？**
因为它可以直接用 `rpmsg_test` 发、用眼睛看、用 shell 脚本测，
不需要为调试再写一个二进制解析器。代价是：如果目标板自己打印的
一行以 `LINKRCFG ` 开头，会被桥吞掉。这是已知取舍，文档里写明了。

---

## 4. E907 固件：每个文件在干什么

### 4.1 `src/main.c`

```c
int main(void)
{
    printk("*** YuzukiPI-Lizard V851S E907 co-processor ***\n");
    ...
    (void)linkr_bridge_init();
    e907_openamp_start();

    while (1) {
        printk("e907 heartbeat %u cyc %llu\n", n++, k_cycle_get_64());
        k_msleep(30000);
    }
}
```

- `printk` 输出到 **RAM console**，不是 UART3。因为 UART3 已经被
  串口桥占用；如果调试日志也写到 UART3，会把目标板的数据搅乱。
  A7 可以通过 `cat /sys/kernel/debug/remoteproc/remoteproc0/trace0`
  看到这些日志。
- `linkr_bridge_init()` 创建 UART 收发线程。
- `e907_openamp_start()` 创建 OpenAMP 线程。
- 主线程每 30 秒打一次心跳，用来确认 E907 还活着。

### 4.2 `src/board_early.c`

这个文件只做两件事：

1. 把 PE12/PE13 复用成 UART3（mux 7）。
2. 打开 UART3 的时钟门控、释放复位。

它用 `SYS_INIT(..., PRE_KERNEL_1, 0)` 在串口驱动初始化之前执行。
注释里把每个寄存器值的来源写得很清楚：

```c
/* PE12 = uart3_tx, PE13 = uart3_rx, mux 7
 * PE bank = pin 128.. -> bank 4 -> 4*0x30 = 0xC0
 * pins 8..15 in CFG1 at 0xC4; pin12 -> bits16..19, pin13 -> bits20..23
 */
#define V851S_PIO_CFG_PE12_PE13   0x020000C4u
#define PE12_PE13_MUX7            0x00770000u
```

为什么注释要写这么细？
因为半年后你再看 `0x00770000`，根本不知道它代表哪两个引脚。
注释记录的是**推导过程**，不是重复代码。

### 4.3 `src/linkr_bridge.c`

这是 E907 的核心。它维护两个 64 KiB 环形缓冲：

```c
RING_BUF_DECLARE(uart_rx_ring, 65536);   // UART3 -> A7
RING_BUF_DECLARE(uart_tx_ring, 65536);   // A7 -> UART3
```

**为什么是 64 KiB？**

- 115200 波特率下，64 KiB 约等于 5.7 秒的缓冲。
- 1.5 Mbaud 下约 437 ms。
- 4 Mbaud 下约 164 ms。
- 它们是 `.bss`，不占 NAND 空间；E907 的 4 MiB 窗口只用约 180 KiB，
  所以“能变大就变大”没有代价。

**RX 路径：**

```c
static void uart_isr(...)
{
    while (uart_irq_is_pending(dev)) {
        n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) break;
        rx_push(buf, n);
    }
    k_sem_give(&uart_rx_sem);
}
```

中断只负责把 FIFO 搬进环形缓冲，然后唤醒线程。

**轮询兜底：**

```c
(void)k_sem_take(&uart_rx_sem, K_MSEC(20));
uart_poll_rx();
```

为什么中断都开了还要轮询？
因为这块板的 CLIC 中断路由曾经不稳定。轮询让“中断没来”只变成
20 ms 的延迟，而不是整条桥失效。`uart_poll_rx()` 里用 `irq_lock()`
保护 FIFO 读，因为 ns16550 驱动不会在“线程读 FIFO”和“自己的 ISR
读 FIFO”之间做串行化。

**TX 路径：**

```c
k_sem_take(&uart_tx_sem, K_FOREVER);
while ((got = ring_buf_get(&uart_tx_ring, buf, sizeof(buf))) > 0) {
    for (i = 0; i < got; i++)
        uart_poll_out(uart_dev, buf[i]);
    k_sem_give(&uart_tx_space_sem);
}
```

`uart_poll_out()` 是忙等写一个字节。115200 下没问题；如果以后要
4 Mbaud，建议改成中断/DMA 发送，否则会占用大量 CPU。

**背压：**

如果 A7 一次粘贴 100 KB，而 UART 只有 115200，TX 环很快满。
`linkr_bridge_rpmsg_recv()` 不会静默丢掉，而是：

1. 先尽量放进环；
2. 放不下就等 `uart_tx_space_sem`，最多等 100 ms 一段；
3. 等待超时才丢弃，并累加 `uart_tx_dropped`。

这样 A7 的写会变慢，浏览器看到的是“发送慢”，而不是“数据少了”。

**运行时改串口：**

`LINKRCFG` 解析出 5 个参数，调用 `uart_configure()`，然后重新打开
RX 中断。支持 300–4,000,000 波特率、5–8 数据位、n/e/o 校验、1/2
停止位、无/硬流控。

### 4.4 `src/openamp_slave.c`

这个文件负责“E907 作为 rpmsg 从设备”的初始化和 mailbox 回调。
关键点：

- `rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, ...)`：E907 是 device。
- `rproc_virtio_wait_remote_ready(vdev)`：等 Linux 写 DRIVER_OK。
- `rproc_virtio_init_vring()`：用资源表里的地址初始化两个 vring。
- `rpmsg_init_vdev()`：建立 rpmsg 层。
- `rpmsg_create_ept(..., "rpmsg_chrdev", ...)`：宣告端点。

端点池 `LINKR_MAX_EPTS = 4` 是为了 A7 反复打开/关闭 `/dev/rpmsgN`
时不会把正在用的端点覆盖掉。

### 4.5 `prj.conf` 和 `CMakeLists.txt`

`prj.conf` 是 Zephyr 的 Kconfig 配置：

| 配置 | 作用 |
|---|---|
| `CONFIG_KERNEL_BIN_NAME="zephyr_e907"` | 生成 ELF 的名字 |
| `CONFIG_UART_CONSOLE=n` | 不把 console 放到 UART3 |
| `CONFIG_RAM_CONSOLE=y` | 调试日志进内存，A7 用 trace0 看 |
| `CONFIG_RING_BUFFER=y` + `CONFIG_RING_BUFFER_LARGE=y` | 支持 64 KiB 环（32 位索引） |
| `CONFIG_XIP=n` | 整个镜像从 DRAM 跑，不从 flash 执行 |
| `CONFIG_MBOX=y` / `CONFIG_MBOX_SUNXI_MSGBOX=y` | mailbox 驱动 |
| `CONFIG_IPM=y` | IPM 层（mailbox 之上） |
| `CONFIG_OPENAMP=y` / `CONFIG_OPENAMP_SLAVE=y` | OpenAMP 从设备 |
| `CONFIG_OPENAMP_RSC_TABLE=y` | 生成 `.resource_table` |
| `CONFIG_OPENAMP_RSC_TABLE_NUM_RPMSG_BUFF=8` | 每个 vring 8 个缓冲 |

`CMakeLists.txt` 只做一件事：把 4 个 `.c` 编进 `app`。
Zephyr 的构建系统会自动处理链接脚本、资源表、设备树。

### 4.6 构建 E907 固件

```sh
export ZEPHYR_BASE=/path/to/zephyr   # 带 v851s BSP 的分支
west build -b v851s_e907 apps/e907_bringup
```

产物：

- `build/zephyr/zephyr.elf`：remoteproc 加载它。
- `.resource_table` 段必须存在，否则 Linux 拒绝启动。
- 板内复制成两个名字：`/lib/firmware/e907_firmware.elf`（启动脚本用）
  和 `/lib/firmware/zephyr.elf`（设备树 `firmware-name` 用）。

### 4.7 注释风格：这份代码为什么这样写注释

这份项目的注释大致分四类：

1. **硬件事实**：寄存器地址、位域、引脚、时钟。
   例：`board_early.c` 里的 PIO/CCU 推导。
2. **顺序约束**：为什么必须先做 A 再做 B。
   例：`S54bluetooth` 里“wlan0 出现后再 hciattach”。
3. **失败模式**：不这样写会出什么错。
   例：`linkr_ble.c` 里“BlueZ 会静默截断到 MTU-3”。
4. **取舍**：为什么选 64 KiB、为什么 256 字节、为什么文本协议。
   例：`linkr_bridge.c` 的环形缓冲注释。

好的注释不是把代码翻译一遍，而是回答：
**“如果我不知道背景，为什么会写出这一行？”**

---

## 5. A7 侧：Linux 怎么把串口变成网页和 BLE

### 5.1 进程拓扑

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

为什么拆成两个进程？

- `linkr-ble` 需要 GLib/D-Bus，用 C 写更贴近 BlueZ，也能处理 BLE 时序。
- daemon 用 Python 写业务逻辑（命令、Wi-Fi、会话、文件），开发快。
- 两者用 Unix socket 解耦：BLE 进程崩了，LAN 还能用；daemon 重启，
  BLE 可以重连。
- 代价：没有自动重启（见“已知限制”）。

### 5.2 `linkr-bee-daemon` 的结构

文件很长（约 1900 行），但可以按块读：

| 区块 | 作用 |
|---|---|
| `EndpointInfo` / `RpmsgBridge` | 打开 `/dev/rpmsg_ctrl0`，创建端点，收发热数据 |
| `SerialWriter` | 有界队列（256 KiB），把 WS 的写和慢速 UART 解耦 |
| `ws_recv_message` | WebSocket 帧解析（支持分片、掩码） |
| `Session` | 会话所有者、令牌、命令分发、持久化 |
| `Client` | 一个 WebSocket 客户端 |
| `BleClient` | 一个来自 `linkr-ble` 的 Unix socket 连接 |
| `serve_file` / `handle_connection` | HTTP 静态文件 + WS 升级 |
| `serve_tls` | HTTPS 监听 |
| `serve_ble_socket` | BLE Unix socket 监听 |
| `main` | 启动所有线程 |

**`SerialWriter` 为什么单独开线程？**

如果 WebSocket 读线程直接写 rpmsg，而 UART 只有 115200，那么一次
粘贴 100 KB 会让这个线程阻塞好几秒；这期间 `@c?`、`@i?` 这些控制
命令都排在后面，网页会“卡住”。所以读线程只入队，发送线程按 UART
速度慢慢写。队列满时丢弃并计数，保证控制命令不被大流量饿死。

### 5.3 WebSocket 协议

`GET /ws` 升级后：

- **二进制帧**：原始串口字节。
- **文本帧**：`@...` 控制命令。
- 连接建立后，服务端先发：

```text
@ws auth=required    # 配置了令牌
@ws auth=none        # 没配令牌
```

有令牌时，客户端必须在 3 秒内发一行文本令牌；否则断开。
令牌通过 `@s token` 管理，存在 `/mnt/UDISK/persist/ws-token`
（权限 600）。

**为什么令牌是文本帧而不是 HTTP Header？**
因为网页的 WebSocket API 不方便自定义 Header；用第一帧文本最简单，
也方便脚本测试。代价是：如果走明文 `ws://`，令牌在局域网里是明文。

### 5.4 命令语言

所有控制命令都以 `@` 开头，LAN 和 BLE 共用同一套：

| 命令 | 作用 |
|---|---|
| `@c?` | 查询当前会话所有者 |
| `@c claim` / `@c release` | 申请/释放控制权 |
| `@c yield <id>` | 把控制权让给某个客户端 |
| `@i?` | 诊断：固件、运行时间、UART 环、Wi-Fi、上传 |
| `@u?` / `@u=115200,8,n,1,n` | 查询/修改 UART3 |
| `@w?` / `@w scan` / `@w=ssid,pass` / `@w off` | Wi-Fi |
| `@s?` / `@s token` / `@s token=<32hex>` / `@s token off` | LAN 令牌 |
| `@n?` / `@n=<name>` / `@n default` | BLE 广播名 |
| `@d?` / `@d=<url>[,user,pass]` | WebDAV 上传目标 |
| `@h` | 帮助 |

`@i?` 里的 UART 数据不是假的：daemon 向 E907 发 `LINKRSTAT`，
解析回复，缓存 2 秒，再拼成 `@info ...` 行。这样诊断面板刷新不会
每次都阻塞在 rpmsg 上。

### 5.5 多客户端与会话所有权

一条串口只能有一个“正在敲键盘”的人，否则两个终端会互相插字。
daemon 用“会话票据”解决：

- 第一个写串口的客户端自动成为 owner。
- 显式 `@c claim` 也能成为 owner。
- owner 3 秒不写，别人可以接管。
- `@c release` 后，这个客户端不会被自动重新 claim（release 就是 release）。
- BLE 是共享桥：`linkr-ble` 把所有 BLE 主机复用一个 Unix socket，
  所以 BLE 的 release 语义和 LAN 不同，代码里用 `transport == "ble"`
  区分。

这不是安全机制，只是“避免互相抢键盘”的礼貌规则。

### 5.6 HTTP / HTTPS / 证书

| 监听 | 行为 |
|---|---|
| `http://<board>/`（80） | 页面请求 301 到 HTTPS；`/ws` 仍可用明文 |
| `https://<board>:8443/` | 页面、`wss://.../ws`、安全上下文 |

为什么需要 HTTPS？
**Web Bluetooth 只在安全上下文（secure context）里可用**。
`http://` 页面里 `navigator.bluetooth` 是 `undefined`，网页会报
“Bluetooth LE transport is not available”。所以主入口是 8443。

证书：

- 自签名，每板一份。
- 优先 `/mnt/UDISK/persist/certs/`，其次 `/usr/share/linkr-bee/certs/`。
- 私钥权限 600，**不进镜像、不进 git**。
- SAN 里写板子的 IP、`localhost`、`linkr-bee.local`。
- 浏览器第一次会警告，接受一次即可。

`S53linkr` 的逻辑：

```sh
if 证书和私钥都存在; then
    tls=8443
else
    tls=0      # 只跑 HTTP
fi
```

### 5.7 Wi-Fi 控制

板子上没有 NetworkManager（64 MB 内存，busybox init），所以有一个
小脚本 `/usr/bin/wctl`，封装 `wpa_supplicant` / `wpa_cli`：

```text
wctl status
wctl list
wctl connect SSID password PASS
wctl disconnect
wctl saved
wctl forget SSID
```

凭据只写 `/mnt/UDISK/wpa_supplicant.conf`，`/etc/wifi/...` 只是符号
链接。`S45wifi-conf` 负责创建模板和链接，`S50wifideamon` 启动
`wpa_supplicant`，`S99postnet` 负责 DHCP 和 NTP。

> 为什么 NTP 要在网络起来后跑？
> 板子没有电池 RTC，启动时是 1970 年。TLS 证书有“生效时间”，
> 时间不对浏览器会报证书错误。`S99postnet` 在拿到 IP 后 `ntpd -q`
> 校时，再写硬件时钟。

### 5.8 持久化：哪些东西放哪里

| 路径 | 内容 | 为什么 |
|---|---|---|
| `/mnt/UDISK/wpa_supplicant.conf` | Wi-Fi 凭据 | 不能进镜像 |
| `/mnt/UDISK/persist/ws-token` | LAN 令牌（600） | 每板不同 |
| `/mnt/UDISK/persist/certs/` | HTTPS 证书/私钥 | 私钥不能进镜像 |
| `/mnt/UDISK/persist/ble-name` | BLE 广播名 | 重启保持 |
| `/mnt/UDISK/persist/bluetooth/` | BLE 绑定/LTK | `/var` 是 tmpfs |
| `/mnt/UDISK/persist/etc/passwd` 等 | 账号 | 根文件系统只读 |
| `/mnt/UDISK/persist/webdav-url` | WebDAV 目标 | 用户配置 |
| `/mnt/UDISK/persist/target` | 目标标识 | 用户配置 |

`S30persist` 做两件事：

1. 如果 UDISK 上的 `passwd/shadow/group` 缺失、为空、或没有 `^root:`，
   就从只读 rootfs 重新拷贝一份。
2. 用 `mount --bind` 把它们挂到 `/etc/...`。

为什么需要第 1 步？
掉电/全盘格式化可能留下“文件长度正常，但内容全是 NUL”的坏文件。
`passwd` 全 NUL 会让 `dbus-daemon` 报
`Could not get UID and GID for username "root"`，然后蓝牙、daemon
全部起不来。校验 `^root:` 是最便宜的“内容健康检查”。

### 5.9 启动脚本顺序与原因

| 脚本 | 做什么 | 为什么在这个位置 |
|---|---|---|
| `S30persist` | 绑定 `/etc`、`/root` 到 UDISK | 后面所有服务都要用账号 |
| `S31hostname` | 恢复主机名 | init 已经先用了 rootfs 里的名字 |
| `S45wifi-conf` | 准备 Wi-Fi 配置/符号链接 | `wpa_supplicant` 之前 |
| `S50wifideamon` | 启动 Wi-Fi | 蓝牙要等 `wlan0` |
| `S52e907` | 加载 uart3pin，启动 E907 | daemon 需要 rpmsg |
| `S53linkr` | 启动 Python daemon | E907 已就绪 |
| `S54bluetooth` | XR829 BT + bluetoothd + linkr-ble | Wi-Fi 固件已加载 |
| `S99postnet` | DHCP / NTP | 最后，网络已可用 |

`S54bluetooth` 的顺序尤其讲究：

1. `start_dbus()`：bluetoothd 需要系统 D-Bus。
2. `wait_for_wifi()`：只等 `wlan0` 出现，不等联网。因为 XR829 的
   Wi-Fi 固件加载会复位整颗芯片；BT 在复位前 attach 是白费。
3. 写 `/etc/bluetooth/xr_bt.conf`：固定 BLE MAC。
4. `try_attach()`：`hciattach -n /dev/ttyS2 xradio`。
   - 先 `killall hciattach`：半开的 attach 会让内核保持 `HCI_RUNNING`，
     后续帧报 `-49`。
   - 用 `script` 给 hciattach 一个 pty：stdout 行缓冲，失败时日志完整。
   - `timeout 35`：冷启动时芯片可能卡在固件握手，不能永远等。
5. `start_services()`：写 `EATTChannels = 1`，启动 bluetoothd，
   再启动 linkr-ble。
6. `le_only()`：`hciconfig hci0 noscan`，关掉经典蓝牙扫描。

**为什么不能碰 `/sys/class/rfkill`？**
厂商 `sunxi-bt` 驱动的 `set_block()` 逻辑是反的；在 hciattach 前多写
一次“unblock”会让 BT 核进入一个固件握手超时的状态。hciattach 自己
的电源时序已经够了。

### 5.10 注释与错误处理风格

A7 侧代码的注释主要解释：

- **协议字段**：`FRAME_CTL_REQ` 的 `reqid`、`flags` 是什么。
- **兼容性**：为什么 `@linkr` 和 `@` 两种拼写都接受。
- **失败模式**：短写、SIGPIPE、粘包、半包。
- **取舍**：为什么 BLE release 和 LAN release 不一样。

错误处理的原则：

- 能重试就重试（rpmsg 端点、BLE socket、hciattach）。
- 不能重试就记录并继续（单个 WS 客户端断开不影响别人）。
- 不静默丢数据：能计数就计数（`uart_rx_dropped`、`serial queue full`）。
- 面向用户的错误用 `ERR ...` 文本，方便网页显示。

---

## 6. BLE：从 GATT 到字节流

### 6.1 先懂 5 个词

| 词 | 通俗解释 |
|---|---|
| GAP | 广播、扫描、连接这些“怎么找到对方”的规则 |
| GATT | 连接后“有哪些服务/特征值”的规则 |
| Service | 一组功能，比如“串口服务” |
| Characteristic | 服务里的一个数据点，可读/写/通知 |
| MTU | 一次 ATT 数据最大多少字节；默认 23，可协商到 517 |
| Notify | 服务端主动推数据，不需要客户端确认 |
| Indicate | 服务端主动推数据，客户端要确认（更可靠，但更慢） |

一条 BLE 数据不是“直接写串口”，而是：

```text
手机写 characteristic
  -> ATT Write Request
  -> BlueZ (bluetoothd)
  -> D-Bus GattCharacteristic1.WriteValue
  -> linkr-ble 的 char_method_call()
  -> Unix socket -> daemon -> rpmsg -> E907 -> UART3
```

### 6.2 三个 GATT 服务

`linkr_ble.c` 注册了三个服务：

#### 1) Nordic UART Service（NUS）

标准 UUID，很多串口 App 都认识：

| 特征 | UUID | 属性 | 方向 |
|---|---|---|---|
| RX | `6e400002-...` | write / write-without-response | 手机 → 板子 |
| TX | `6e400003-...` | notify | 板子 → 手机 |

这是“兼容模式”：不装 Linkr Bee App 也能用通用 BLE 串口工具。

#### 2) Linkr Management Service v1

| 特征 | UUID | 属性 | 作用 |
|---|---|---|---|
| Protocol | `4c4b0002-...` | read | 版本、最大载荷、能力位 |
| Device ID | `4c4b0003-...` | read | 16 字节设备标识（含 MAC） |
| Command | `4c4b0004-...` | write | 发 `@...` 命令 |
| Response | `4c4b0005-...` | indicate | 回命令结果/异步事件 |

#### 3) Linkr Reliable UART Service v1

| 特征 | UUID | 属性 | 作用 |
|---|---|---|---|
| RX | `4c4b0011-...` | write | 手机 → 板子，带序号 |
| TX | `4c4b0012-...` | indicate | 板子 → 手机，带序号 |
| State | `4c4b0013-...` | read | 期望序号、下一个发送序号、已确认序号 |

“Reliable”不是 TCP 那种重传协议；它利用 BLE 的
**Write Request / Indicate 本身有 ATT 确认**，再加一个 32 位序号，
用来发现重复和缺口。真正的吞吐优化在下一节。

### 6.3 12 字节帧格式

Management 和 Reliable UART 都用一个 12 字节头：

```text
偏移  长度  含义
0     2     magic
3     1     version
4     4     seq / reqid（小端）
8     2     payload length（小端）
10    2     flags（小端）
12    N     payload
```

Management 的 magic 是 `"LK"`，version=1，type 在 byte 3：
1=请求，2=响应，3=异步事件。
Reliable UART 的 magic 是 `"LR"`，version=1，byte 3=0，
seq 在 4..7，长度在 8..9。

为什么固定 12 字节？
因为 BLE 的最小 ATT MTU 是 23，一次写最多 20 字节；12 字节头 + 8 字节
数据还能放进第一片。代码要求“第一片必须带完整头”，这是唯一的
实际限制（见 REVIEW）。

### 6.4 MTU：为什么不能固定发 20 字节

BLE 4.0 默认 ATT MTU=23，一次只能发 20 字节有效数据。
但现代手机/系统会协商更大的 MTU：

- Android 常见 512
- HarmonyOS 常见 247
- iOS 常见 185

BlueZ 在发通知时，如果数据超过 `MTU-3`，会**静默截断**。
所以 `linkr-ble` 不能写死一个“很大”的值，必须知道对端协商结果。

问题：BlueZ 的 D-Bus GATT API 不暴露“当前连接的 MTU”。
解决：给 `bluetoothd` 打一个小补丁，把每个 peer 的 MTU 写到：

```text
/var/run/linkr-bt-mtu/<bdaddr>
```

`linkr-ble` 每秒读一次目录，取所有文件里的**最小值**，然后：

```c
att_chunk = mtu - 3;      // 20 .. 514
```

这样：

- MTU=23 的对端拿到 20 字节一片，不会截断。
- MTU=517 的对端拿到 514 字节一片，接近满速。
- 多个连接时取最小值，保证谁都不丢。

> 为什么目录在 `/var/run`？
> 它是 tmpfs，重启自动清空；MTU 是“当前连接”的状态，不应该持久化。

### 6.5 吞吐优化：从 20 B 到 232 B

BLE 串口慢，通常不是“蓝牙带宽不够”，而是**发送节奏**和**帧大小**。

修复前：

- 每个 ATT 值 20 字节；
- Reliable 帧只有 64 字节，拆成 4 个 20 字节的 confirmed indication；
- 每片等 10 ms，每帧等 30 ms；
- 有时缓冲区只有 30 字节就起一帧。

修复后：

- 分片大小 = `MTU-3`（20..514）；
- Reliable 帧载荷 232 字节（12+232=244，一个 MTU≥247 的 indication
  就能装下）；
- 发送节奏：每片 2 ms，每帧 15 ms；
- 起帧前最多等 10 ms，尽量凑满 232 字节，避免“小碎片”浪费
  每个 indication 的确认往返。

为什么是 232，不是 512？
因为帧头 12 字节，244 字节正好能放进 MTU=247 的单个 indication；
再大就要拆成多个 indication，反而增加确认次数。232 是一个在
“兼容性”和“效率”之间的折中。

**实际速度受什么限制？**

- 对端的 connection interval（连接间隔），常见 15–30 ms。
- Indicate 需要 ATT 确认，每个确认要一个连接事件。
- 所以理论上限大约是“每个连接事件能发多少字节 / 连接间隔”。
- 手机 App 可以通过 `requestConnectionPriority('high')` 请求更短间隔；
  板子侧无法单方面决定。

### 6.6 广播数据：4.9 内核的坑

BlueZ 通过 `MGMT Add Advertising` 把广播数据交给内核，但全志 4.9
内核把这两个 HCI 命令注释掉了：

- `HCI_OP_LE_SET_ADV_DATA`（0x0008）
- `HCI_OP_LE_SET_SCAN_RSP_DATA`（0x0009）

结果：BlueZ 以为注册成功，但空中广播是空的。Web Bluetooth 的选择框
按 Service UUID 过滤，就找不到板子。

解决：`ble-adv-data.py` 在每次注册广播后，用 `hcitool` 直接发原始
HCI 命令，把 AD（flags + Management UUID）和 scan response（名字）
写进控制器。

```text
AD:  02 01 06  11 07 <128-bit UUID>
SRD: <len> 09 <name>
```

这是“内核不给你接口，就绕过内核直接和控制器说话”的典型 workaround。

### 6.7 配对、EATT、单连接

**配对（pairing）**

`linkr-ble` 注册了一个 `NoInputNoOutput` 的 pairing agent，
也就是 Just Works：没有 PIN，手机点“配对”就完成。适合没有屏幕/
键盘的板子。

**EATT 为什么必须关**

EATT（Enhanced ATT）允许并行 ATT 通道。Linux 4.9 没有 EATT PSM 支持。
BlueZ 5.54 默认 `EATTChannels=3`，会向对端宣告 EATT；Windows 连上来
后走 EATT，结果连不上/看不到特征。

更坑的是 BlueZ 5.54 的解析器读的是 `Channels`，不是 `EATTChannels`，
所以只改配置不生效。项目里：

1. 给 `bluetoothd` 打补丁，正确解析 `EATTChannels`；
2. `main.conf` 写 `EATTChannels = 1`；
3. `S54bluetooth` 启动前再兜底改一次。

**为什么只能连一个中心设备**

XR829 的控制器在已有 LE 连接时，`LE Set Advertise Enable` 返回成功，
AD 数据也接受，但空中没有广播。第二个手机永远看不到板子。
这是控制器固件行为，软件改不了。多终端请走 LAN/WSS（可同时接多个
WebSocket 客户端）。

### 6.8 `linkr_ble.c` 的结构

| 区块 | 作用 |
|---|---|
| UUID / 路径宏 | 服务、特征、D-Bus 对象路径 |
| `chars[]` / `services[]` | 声明 3 个服务、9 个特征 |
| `emit_value()` | 发 `PropertiesChanged`，等于 notify/indicate |
| `char_method_call()` | 处理 ReadValue / WriteValue / StartNotify |
| `mgmt_write()` / `mgmt_dispatch()` | 解析 12 字节头，转发给 daemon |
| `rel_rx_write()` | Reliable UART 接收、序号检查、粘包处理 |
| `mgmt_send_tick()` / `rel_send_tick()` / `nus_send_tick()` | 三个发送节奏器 |
| `refresh_att_chunk()` | 读 MTU 目录，算分片大小 |
| `register_gatt()` / `register_adv()` / `register_agent()` | D-Bus 注册 |
| `main()` | 连 D-Bus，注册服务，起 GLib 主循环 |

**粘包处理**：一次 BLE 写可能包含两个完整帧（Write Without Response
批量发送）。`mgmt_write()` / `rel_rx_write()` 用 `while (len > 0)` 循环，
处理完一帧继续处理剩余字节，不会丢第二帧。

**SIGPIPE**：Unix socket 对端关闭时，默认会杀死进程。代码忽略
`SIGPIPE`，短写重试，避免 daemon 重启把 `linkr-ble` 带走。

### 6.9 BLE 数据流（完整）

```text
手机写 REL_RX
  -> ATT Write Request
  -> bluetoothd -> D-Bus WriteValue
  -> linkr-ble rel_rx_write()
       ├─ 检查 "LR" 头
       ├─ 收满 12+len
       ├─ 检查序号
       └─ send_frame(FRAME_DATA) -> Unix socket
  -> daemon BleClient
       └─ handle_binary() -> SerialWriter -> rpmsg -> E907 -> UART3

UART3 收到数据
  -> E907 uart_rx_ring
  -> rpmsg -> daemon on_bridge_data()
  -> broadcast() -> BleClient.send_binary()
  -> Unix socket FRAME_DATA
  -> linkr-ble serial_in
  -> rel_send_tick()
       ├─ 凑 232 字节
       ├─ 加 12 字节头
       ├─ 按 att_chunk 分片
       └─ emit_value(REL_TX) -> indicate -> 手机
```

Management 命令：

```text
手机写 MGMT_CMD（LK 头 + reqid + 文本）
  -> linkr-ble mgmt_write()
  -> FRAME_CTL_REQ(reqid, text)
  -> daemon handle_command()
  -> FRAME_CTL_RSP(reqid, text)
  -> linkr-ble append_mgmt_rsp()
  -> MGMT_RSP indicate（LK 头 + reqid + flags）
```

异步事件（Wi-Fi 扫描结果、连接完成）：

```text
daemon emit_event()
  -> FRAME_EVENT(reqid, flags, text)
  -> linkr-ble append_mgmt_event()
  -> MGMT_RSP indicate，flags 带 ASYNC 位
```

---

## 7. 网页前端：为什么有 5 个补丁

`linkr-bee/web/` 来自 [radxa/linkr-bee](https://github.com/radxa/linkr-bee)
的 `d9b2e00`，不是从零写的。V851S 的差异用 5 个补丁解决：

| 补丁 | 问题 | 解决 |
|---|---|---|
| 0001 | `crypto.randomUUID` 只在安全上下文存在 | 用 `getRandomValues` 生成 UUID |
| 0002 | 管理面板假设 BLE 传输 | LAN 模式下显示正确提示 |
| 0003 | `crypto.subtle` 在 HTTP 页面不存在 | 纯 JS SHA-256 兜底 |
| 0004 | HTTPS 页面需要 `wss://` | 根据页面 origin 推导 WS URL |
| 0005 | LAN 页面不自动查诊断 | 连接后自动 `@i?`/`@w?`/`@s?` |

为什么这些补丁不直接提给上游？
因为它们是“V851S 的 LAN 桥”特有的；上游可能不想为 HTTP 明文场景
维护 fallback。项目把补丁留在本仓库，fork 保持干净。

`apply-web.sh` 可以把补丁打到任意 `linkr-bee` 检出；
`snapshot/web/` 是打完补丁的关键文件（md5 固定），用于验证。

---

## 8. 代码包之间的关系

### 8.1 两个仓库

| 仓库 | 作用 | 是否公开 |
|---|---|---|
| `Novice-PG/v851s-e907-app` | E907 固件源码 + A7 侧 Linkr Bee 实现 + web 补丁 + 文档 | public |
| `Novice-PG/v851s-lizard-tina` | 板级 DTS、rootfs 脚本、SDK 补丁、镜像组装 | private |

为什么分开？

- `v851s-e907-app` 是“应用源码”，可以公开、可以给别人复用。
- `v851s-lizard-tina` 涉及全志 SDK 的构建补丁、板级文件、镜像，
  受全志 EULA 约束，不公开分发。
- 两边通过 `apply.sh` 连接：lizard 仓库从 `v851s-e907-app` 取
  `linkr-bee/a7/` 和 `firmware/e907_firmware.elf`，装进 SDK 的
  staging rootfs。

### 8.2 依赖图

```text
radxa/linkr-bee (web 前端, d9b2e00)
        │ 5 个补丁
        ▼
linkr-bee/web/  ──────────────┐
                              │
Zephyr v4.5.0                 │
  + Novice-PG/zephyr BSP      │
  (v851s-e907-bsp)            │
        │                     │
        ▼                     │
src/*.c, prj.conf             │
        │ west build          │
        ▼                     │
firmware/zephyr_e907.elf      │
        │                     │
        ├─────────────────────┤
        │                     ▼
        │            v851s-lizard-tina
        │            (board.dts, rootfs, SDK patches)
        │                     │
        │                     ▼
        │            Tina SDK build + pack
        │                     │
        └──────────────► 整片镜像 .img
                              │
                              ▼
                          V851S 板子
```

A7 侧运行时：

```text
Python 3.9 (板内)
  └─ linkr-bee-daemon
       ├─ /dev/rpmsg_ctrl0  (Linux rpmsg_char)
       ├─ /var/run/linkr-bee.sock
       └─ /usr/share/linkr-bee/web

BlueZ 5.54 + GLib
  └─ linkr-ble (由 linkr_ble.c 交叉编译)
       └─ D-Bus org.bluez.GattManager1
```

### 8.3 哪些是原创，哪些是依赖

| 组件 | 来源 | 本仓库是否包含 |
|---|---|---|
| E907 应用（`src/`） | 本项目原创 | 是 |
| A7 daemon（`linkr-bee-daemon`） | 本项目原创 | 是 |
| GATT server（`linkr_ble.c`） | 本项目原创 | 是 |
| 启动脚本 | 本项目原创 | 是 |
| web 前端 | radxa/linkr-bee + 5 个补丁 | 是（补丁后的树） |
| Zephyr | 上游 | 否（依赖） |
| V851S BSP | Novice-PG/zephyr fork | 否（依赖） |
| OpenAMP/libmetal | Zephyr module | 否（依赖） |
| BlueZ | 上游 + 两个小补丁 | 否（板内二进制/镜像） |
| 全志 SDK/固件 | 全志 | 否（EULA） |

### 8.4 构建产物怎么对应到板子

| 源码 | 产物 | 板内路径 |
|---|---|---|
| `src/*.c` + Zephyr | `firmware/zephyr_e907.elf` | `/lib/firmware/e907_firmware.elf`、`zephyr.elf` |
| `linkr-bee/a7/linkr-bee-daemon` | 无需编译 | `/usr/bin/linkr-bee-daemon` |
| `linkr-bee/a7/linkr_ble.c` | `linkr-bee/a7/linkr-ble` | `/usr/bin/linkr-ble` |
| `linkr-bee/a7/S53linkr` | 脚本 | `/etc/init.d/S53linkr` |
| `linkr-bee/a7/S54bluetooth` | 脚本 | `/etc/init.d/S54bluetooth` |
| `linkr-bee/a7/main.conf` | 配置 | `/etc/bluetooth/main.conf` |
| `linkr-bee/a7/ble-adv-data.py` | 脚本 | `/usr/bin/ble-adv-data.py` |
| `linkr-bee/web/*` | 静态文件 | `/usr/share/linkr-bee/web/` |
| `linkr-bee/a7/certs/san.cnf` | 模板 | `/usr/share/linkr-bee/certs/san.cnf` |

---

## 9. 想改代码？先看这张表

| 你想改什么 | 改哪里 | 注意 |
|---|---|---|
| 改 UART3 默认波特率 | `linkr_bridge.c` 的 `active_cfg`，或设备树 `current-speed` | 运行时可用 `@u=` 覆盖 |
| 加一个 `@` 命令 | `linkr-bee-daemon` 的 `handle_command()` | BLE/LAN 共用，返回 `OK/ERR` 文本 |
| 改 BLE 分片/节奏 | `linkr_ble.c` 的 `att_chunk`、`rel_send_tick()` | 先测对端 MTU/连接间隔 |
| 改广播名 | `@n=<name>` 或 `/mnt/UDISK/persist/ble-name` | 最长 29 字节 UTF-8 |
| 改网页 | `linkr-bee/web/`，并更新补丁/快照 | 跑 `verify.sh` |
| 改启动顺序 | `S??` 脚本 + `rc.final` 的排序 | 不要破坏 `S30persist`→`S52`→`S53`→`S54` |
| 改 E907 内存布局 | `board.dts` 的 `reserved-memory` + `prj.conf` | 地址必须和 Linux 资源表一致 |
| 换 Wi-Fi 凭据 | `/mnt/UDISK/wpa_supplicant.conf` | 不要写进镜像 |
| 换 HTTPS 证书 | `/mnt/UDISK/persist/certs/` | 私钥 600 |

### 9.1 加一个命令的完整路径

1. 在 daemon 的 `handle_command()` 里加分支，返回文本。
2. 如果命令要访问 E907，用 `self.bridge.send()` 发控制行，并在
   `on_bridge_data()` 里解析回复。
3. 如果 BLE 也要用，不需要改 `linkr_ble.c`：Management 命令是
   透传文本的。
4. 如果网页要按钮，改 `web/` 里对应模块。
5. 更新 `docs/PROTOCOL.md` 和 `verify.sh` 的 md5。

### 9.2 改 UART 参数

不要在 E907 里硬编码。用：

```text
@u=1500000,8,n,1,n
```

daemon 会发 `LINKRCFG 1500000,8,n,1,n`，E907 调 `uart_configure()`，
回复 `LINKRCFG OK uart=...`。如果失败，返回 `ERR uart_configure=<errno>`。

### 9.3 改 BLE 吞吐

先测量，不要盲调：

1. 手机/主机看协商 MTU。
2. 看 `/var/run/linkr-bt-mtu/` 是否有文件。
3. 看 daemon 日志有没有 `reliable RX sequence gap`。
4. 看 `@i?` 的 `uart dropped` / `tx_dropped`。
5. 再调 `rel_send_tick()` 的 232/15ms/2ms。

---

## 10. 调试手册

### 10.1 三个入口

| 入口 | 用途 |
|---|---|
| `/dev/ttyACM0`（115200） | A7 Linux 控制台 |
| `/dev/ttyUSB0`（115200） | E907 UART3，直接看目标串口 |
| SSH `root@<board-ip>` | 网络正常时最方便 |

### 10.2 常用命令

```sh
# 系统
cat /etc/rootfs_version
uname -a
free
df -h

# E907
cat /sys/kernel/debug/remoteproc/remoteproc0/state    # running (2)
tail -20 /sys/kernel/debug/remoteproc/remoteproc0/trace0

# 服务
ps | grep -E 'linkr|bluetoothd|wpa'
cat /var/log/linkr-bee.log
cat /var/log/bluetooth-init.log
cat /var/log/hciattach.log

# 蓝牙
hciconfig hci0
btmgmt info
cat /var/run/linkr-bt-mtu/* 2>/dev/null

# rpmsg
ls -l /dev/rpmsg*
./rpmsg_test        # 如果板内有
./rpmsg_bench 1000 32

# 网页
curl -sk -o /dev/null -w '%{http_code}\n' https://127.0.0.1:8443/
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1/

# 诊断
# 通过 WS 发 @i? / @c? / @u?
```

### 10.3 故障树

**网页打不开**

1. `ping <board-ip>` 通吗？
2. `wctl status` 有 IP 吗？
3. `ps | grep linkr-bee-daemon` 在吗？
4. `curl http://127.0.0.1/` 返回 200 吗？
5. 8443 没起来：看 `/mnt/UDISK/persist/certs/` 是否成对存在。
6. 浏览器证书警告：接受；时间不对会报 not yet valid。

**BLE 找不到/连不上**

1. `hciconfig hci0` 是否 `UP RUNNING`？
2. `hciconfig hci0 noscan` 是否执行？
3. `ps | grep linkr-ble` 在吗？
4. `cat /var/log/linkr-ble.log` 有 `advertising as` 吗？
5. 广播为空：`ble-adv-data.py` 是否执行成功？
6. Windows 连不上：确认 `EATTChannels = 1`，bluetoothd 是打过补丁的。
7. 只能连一个：正常，XR829 限制。

**BLE 很慢**

1. `/var/run/linkr-bt-mtu/` 里的 MTU 是多少？
2. 对端 connection interval 是多少（`btmon`）？
3. `@i?` 的 `uart dropped` 是否为 0？
4. Reliable UART 是否被订阅？没有订阅会走 NUS 兼容路径。

**E907 没起来**

1. `cat .../state` 是 `offline (0)` 还是 `running (2)`？
2. `trace0` 有 banner 吗？
3. `lsmod | grep uart3pin`。
4. `/lib/firmware/e907_firmware.elf` 存在吗？
5. 资源表 / 内存地址是否和 DTS 一致？

**串口乱码**

1. 波特率/校验/停止位是否一致？
2. PE12/PE13 是否被别的功能占用？
3. 是否把 PE12 接到了 TX0（双输出对打）？
4. 地线是否共地？

### 10.4 日志位置

| 日志 | 内容 |
|---|---|
| `/var/log/linkr-bee.log` | daemon：rpmsg、HTTP、WS、会话 |
| `/var/log/bluetooth-init.log` | S54 的 attach 过程、BT MAC |
| `/var/log/hciattach.log` | xradio 固件下载、HCI 初始化 |
| `/var/log/bluetoothd.log` | BlueZ |
| `/var/log/linkr-ble.log` | GATT 注册、广播、发送错误 |
| `trace0` | E907 的 printk |

### 10.5 验证

```sh
./linkr-bee/verify.sh                 # 仓库 md5
./linkr-bee/verify.sh /path/to/rootfs # 板内/镜像文件
```

注意：`verify.sh` 用 `MANIFEST.sha256` 做逐文件 md5。不要手工改
已安装文件而不更新清单，否则会报 `DIFF`。

---

## 11. 安全性：这套东西的边界在哪

### 11.1 已经做的

- 镜像里没有 Wi-Fi 密码、没有 HTTPS 私钥、没有令牌。
- 私钥/令牌文件权限 600。
- HTTP 文件服务做了路径规范化，阻止 `../` 目录穿越。
- 令牌可选；设置后 WS 必须 3 秒内认证。
- BLE 用 Just Works 配对；绑定存 UDISK。
- 没有把全志 SDK/固件/镜像提交到公开仓库。

### 11.2 需要你知道的风险

1. **默认没有 LAN 令牌**。新 UDISK 上 `auth=none`，同一局域网的人
   可以直接连 `/ws` 控制串口。建议设 `@s token`。
2. **令牌在明文 `ws://` 上是明文传输**。如果在意，走
   `wss://<board>:8443/ws`。
3. **HTTP 静态文件不需要令牌**。令牌只保护 WebSocket。
4. **BLE 没有应用层认证**。靠近板子的人可以配对；Just Works 没有
   PIN。物理安全 = 蓝牙安全边界。
5. **daemon 以 root 运行**。因为它要操作 `/dev/rpmsg*`、蓝牙文件、
   `wctl`。这是嵌入式常见做法，但也意味着 Web 服务的漏洞影响很大。
6. **默认 root/tina 密码**。正式使用请改密码，并只保留自己的 SSH key。
7. **自签名证书**。浏览器会警告；不要在生产环境忽略来源不明的证书。

### 11.3 建议的最小加固

```sh
# 设 LAN 令牌
# 通过网页或 WS 发：@s token

# 改 root 密码
passwd

# 确认私钥/令牌权限
ls -l /mnt/UDISK/persist/certs /mnt/UDISK/persist/ws-token
```

---

## 12. 已知限制与设计取舍

这些不是“忘了做”，而是有明确原因的取舍：

| 限制 | 原因 | 影响/替代 |
|---|---|---|
| BLE 只能一个中心 | XR829 连接时不广播 | 多终端走 LAN/WSS |
| 没有 BLE 自动重启 | 没有 systemd/看护进程 | 手动 `S54bluetooth restart` |
| daemon 没有自动重启 | busybox init，无 supervisor | 手动 `S53linkr restart` |
| `LINKRCFG` 前缀被保留 | 文本控制行简单可调试 | 目标板输出该前缀会被吞 |
| Reliable 头必须第一片 | 12 字节头 > 最小 MTU 的剩余空间 | 实际 MTU≥23 都满足 |
| 令牌明文 over ws | WebSocket 浏览器 API 限制 | 用 wss |
| 默认无令牌 | 出厂易用 | 建议设置 |
| HTTPS 自签名 | 没有公共 CA/域名 | 接受一次警告 |
| opkg 只有 19.07 两源 | 21.02 索引能载入但下载卡住 | 自建本地 feed |
| `@c?` 的 `ble_max=2` | 协议/UI 容量字段，不是实际双连接 | 以 README 的单连接说明为准 |

---

## 13. 全量 review 结论（2026-11-14）

### 13.1 已验证

- `src/` 4 个 C 文件、`prj.conf`、`CMakeLists.txt` 结构一致，能对应到
  v12 固件（`41e41bc6...`）。
- `linkr_ble.c` 的三个服务、UUID、12 字节帧、MTU 分片、节奏器、
  广播 workaround、pairing agent 都能在代码里找到对应实现。
- daemon 的 rpmsg、串口队列、WS、TLS、令牌、BLE socket、命令分发、
  持久化路径完整。
- `S53linkr` / `S54bluetooth` 的顺序、重试、日志、MAC 固定逻辑完整。
- `verify.sh` 可校验仓库和板内文件。
- 板内 v12 运行状态：WiFi 已连、HTTPS 200、token 生效、E907
  `running (2)`、BLE MAC 固定、opkg 回到 19.07.10。

### 13.2 发现并已修复

1. **`MANIFEST.sha256` 包含被 `.gitignore` 忽略的 `__pycache__/*.pyc`。**
   在干净 clone 上 `verify.sh` 会失败。已重新生成清单并排除
   `__pycache__`、`build/` 与证书私钥。
2. **文档里的 `Channels = 1` 与实际 `EATTChannels = 1` 不一致。**
   已更新 `docs/REVIEW.md` 和 `linkr-bee/README.md`。
3. **`S54bluetooth` 的 `sed` 只匹配旧键名 `Channels`。** 已改为
   `EATTChannels`，键不存在时追加到 `[GATT]`。
4. **Bond 持久化挂错了目录。** BlueZ 以 `--localstatedir=/etc` 构建，
   实际使用 `/etc/lib/bluetooth`，脚本却把 UDISK bind 到
   `/var/lib/bluetooth`（`/var` 是 tmpfs）。现在改挂
   `/etc/lib/bluetooth`；首次启动会把 overlay 里的旧状态复制到
   UDISK，已有 UDISK 数据不会被覆盖。
5. **`@i?` 无统计时回退 16384**，而实际环是 65536；已改为 65536。
6. **控制回复残留。** `LINKRCFG`/`LINKRSTAT` 超时后 `_ctl_pending`
   里的半行会拼到下一次回复前面；现在请求结束和开始都清空。
7. **TLS 慢客户端会阻塞串口转发。** TLS 监听没有 `SO_SNDTIMEO`，而
   `broadcast()` 跑在 rpmsg RX 线程里。现在与 HTTP 一样 10 s，并给
   TLS 握手加 10 s 上限，避免静默客户端卡住 accept 循环。
8. **rpmsg RX 线程会被异常杀死。** 原来只捕获 `OSError`；现在
   `on_data()` 的其他异常记录后继续运行。
9. **`MAX_CLIENTS=4` 从未生效。** 已删除，`PROTOCOL.md` 改为
   “所有连接客户端共享会话”。
10. **`apply-web.sh` 的 `app.js` 期望 md5 过期。** 已更新为
    `839bea52e730f36393657a2c4e994e1c`。

### 13.3 发现但不打算改的部分

1. `@c?` 返回 `ble_max=2`，这是协议/网页容量字段；实际 BLE 只能
   一个中心。已在 README 和本文档中说明，避免误解。
2. `serve_file()` 会把整个静态文件读进内存（最大约 1.3 MB）。
   单客户端没问题，但 64 MB 内存下多个并发大文件请求要留意。
3. `S53linkr` / `S54bluetooth` 没有进程看护；进程退出后不会自动拉起。

### 13.4 建议的下一步

1. 用手机/Windows 做 30 分钟 BLE 长稳 + 重连 + 绑定持久化测试。
2. 如果 BLE 仍慢，抓对端 connection interval 和 `@i?` 的丢包计数，
   再决定是否调 232/15 ms。
3. 给 daemon/linkr-ble 加一个简单的看护脚本（busybox 下可用
   `while true; do ...; sleep 5; done`），但要注意不要和 `S54` 的
   attach 状态打架。
4. 需要更多 opkg 包时，用 Tina SDK 自建本地 feed，而不是硬切
   21.02 在线源。
5. 如果要做产品化，补上 LAN 令牌默认策略、HTTP 静态文件鉴权、
   以及 BLE 应用层配对确认。

---

## 14. 术语表

| 术语 | 解释 |
|---|---|
| SoC | 片上系统，一颗芯片里含 CPU、内存控制器、外设 |
| A7 | ARM Cortex-A7，主 CPU，跑 Linux |
| E907 | 玄铁 RISC-V 小核，跑 Zephyr |
| RTOS | 实时操作系统，任务切换延迟可预测 |
| Zephyr | 一个开源 RTOS，支持多种架构 |
| rpmsg | 核间消息传递协议，基于 virtio |
| OpenAMP | 实现 rpmsg/virtio 的开源框架 |
| virtqueue/vring | 共享内存里的描述符环 |
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

---

## 15. 一页速查

```text
板子：YuzukiPI-Lizard / V851S / sun8iw21p1
A7：Linux 4.9.191，912 MHz
E907：Zephyr 4.5.0，600 MHz，RV32IMAC
内存：64 MB DDR2；E907 保留 0x43c00000 +4 MiB
共享：0x43000000 256 KiB；vring0 0x43040000；vring1 0x43060000
串口：UART0 PH9/PH10 控制台；UART2 PD18-21 蓝牙；UART3 PE12/PE13 目标
存储：mtd0 boot0；mtd1 uboot；mtd3 sys(UBI)；ubi0_3 boot；ubi0_4 rootfs；ubi0_5 UDISK
启动：boot0 -> U-Boot -> Linux -> rcS -> rc.final S30..S99
E907：S52e907 insmod uart3pin + remoteproc start
网页：80 -> 301 -> 8443；/ws 可明文；wss://:8443/ws
令牌：/mnt/UDISK/persist/ws-token（600）
证书：/mnt/UDISK/persist/certs/linkr-bee.{crt,key}
BLE：NUS + Management v1 + Reliable UART v1
BLE MAC：22:22:<WiFi MAC 后 4 字节>
BLE 限制：一个中心；XR829 连接时不广播
opkg：19.07.10 两源（base + packages）
```

---

## 16. 最后：怎么继续学

如果你只记住一条，请记住：

> **先分清“谁在做什么”**：A7 做网络/网页/蓝牙，E907 做串口，
> rpmsg 是两者之间的桥，daemon/linkr-ble 是桥两端的翻译官。

然后按这个顺序读代码：

1. `src/main.c` → 知道 E907 启动后做什么。
2. `src/linkr_bridge.c` → 知道串口字节怎么进出。
3. `src/openamp_slave.c` → 知道 rpmsg 怎么建立。
4. `linkr-bee-daemon` 的 `RpmsgBridge` / `SerialWriter` → 知道 A7 怎么接。
5. `linkr-bee-daemon` 的 `Session.handle_command()` → 知道命令怎么走。
6. `linkr_ble.c` 的 `char_method_call()` / `rel_send_tick()` → 知道 BLE 怎么走。
7. `S52e907` / `S53linkr` / `S54bluetooth` → 知道开机怎么把这些串起来。

读的时候不要一行一行抠语法，先问三个问题：

1. 这段代码在**哪个 CPU** 上跑？
2. 它读/写的是**内存、寄存器、文件、socket 还是网络**？
3. 如果它失败了，**谁会先发现**（日志、超时、丢包计数、网页报错）？

把这三个问题回答出来，你就已经看懂这套系统了。
