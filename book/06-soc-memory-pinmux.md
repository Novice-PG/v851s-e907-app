# 第 6 章 SoC、内存地图与引脚复用

> 本章目标：
> 1. 知道一颗 SoC 里有什么，CPU 和外设怎么连。
> 2. 能背下这块板的关键地址：DRAM、E907 窗口、rpmsg、vring、UART3。
> 3. 理解 MMIO：为什么“写一个地址”就是在操作硬件。
> 4. 会从引脚号推导出寄存器地址和位域。
> 5. 知道 `reserved-memory` 和“没有 IOMMU”意味着什么。

---

## 6.1 SoC 不是“一块 CPU”

V851S 是一颗片上系统（System on Chip）。把封装打开，里面有：

```text
┌─────────────────────────────────────────────┐
│                  V851S                      │
│                                             │
│  ┌──────────┐   ┌──────────┐   ┌────────┐  │
│  │ Cortex-A7│   │  E907    │   │  NPU   │  │
│  │  (Linux) │   │ (Zephyr) │   │ (闲置) │  │
│  └────┬─────┘   └────┬─────┘   └────────┘  │
│       │              │                      │
│  ┌────▼──────────────▼───────────────────┐  │
│  │        总线 / 互联（AXI、AHB、APB）    │  │
│  └──┬───────┬────────┬────────┬──────────┘  │
│     │       │        │        │             │
│  ┌──▼──┐ ┌──▼──┐  ┌──▼──┐  ┌──▼──────┐     │
│  │ DDR │ │ NAND│  │ UART│  │ Wi-Fi   │     │
│  │ 控制器│ │控制器│ │ ×4  │  │ XR829   │     │
│  └─────┘ └─────┘  └─────┘  └─────────┘     │
│                                             │
│  ┌──────────┐ ┌──────────┐ ┌────────────┐  │
│  │ PIO 引脚 │ │ CCU 时钟 │ │ Mailbox    │  │
│  │ 控制器   │ │ 控制器   │ │ 门铃       │  │
│  └──────────┘ └──────────┘ └────────────┘  │
└─────────────────────────────────────────────┘
```

关键点：

- CPU 和外设通过**总线**通信。
- 外设通过**寄存器**暴露能力：写寄存器 = 配置硬件。
- 两个 CPU 共享同一片 DRAM，但各自跑自己的程序。
- 有些外设只给 A7（如 NAND、Wi-Fi），有些只给 E907（如 mailbox 的从端）。

### 本板参数

| 部件 | 参数 | 备注 |
|---|---|---|
| Cortex-A7 | 最高约 912 MHz | 跑 Tina Linux 4.9.191 |
| E907 | 约 600 MHz | RV32IMAC，跑 Zephyr 4.5.0 |
| DDR2 | 64 MB | 和 SoC 封装在一起 |
| SPI NAND | 128 MB | 启动链和 rootfs |
| Wi-Fi/BT | XR829 | 2.4 GHz + BLE |
| NPU | 有 | 本项目没用 |
| MIPI | 有 | 本项目没用 |

> 你不需要背主频。需要记住的是：**A7 和 E907 是两个独立
> 处理器，共享内存，但各有各的程序和栈。**

---

## 6.2 地址空间：一张必须记住的地图

CPU 访问内存和外设，用的是**物理地址**。这块板没有 IOMMU，
所以“设备看到的地址”和“CPU 看到的地址”是同一个（DA == PA）。

### 关键区域

```text
0x4000_0000 ┌──────────────────────────────┐
            │  DDR 64 MB                   │
            │                              │
0x4300_0000 │  ┌────────────────────────┐  │
            │  │ rpmsg buffer 256 KiB   │  │
0x4304_0000 │  ├────────────────────────┤  │
            │  │ vring0 128 KiB         │  │
0x4306_0000 │  ├────────────────────────┤  │
            │  │ vring1 128 KiB         │  │
            │  ├────────────────────────┤  │
            │  │ （空闲）                │  │
0x43c0_0000 │  ├────────────────────────┤  │
            │  │ E907 固件 4 MiB        │  │
0x4400_0000 └──┴────────────────────────┴──┘

0x0200_0000   PIO（引脚控制器）
0x0200_1000   CCU（时钟控制器）
0x0250_0c00   UART3
```

### 表格版

| 区域 | 起始地址 | 大小 | 用途 |
|---|---|---|---|
| DDR | `0x40000000` | 64 MiB | 全部内存 |
| rpmsg buffer | `0x43000000` | 256 KiB | 每条消息的 512 B 缓冲 |
| vring0 | `0x43040000` | 128 KiB | 一个方向的描述符环 |
| vring1 | `0x43060000` | 128 KiB | 另一个方向 |
| E907 固件窗口 | `0x43c00000` | 4 MiB | 代码、数据、栈、`.bss` |
| PIO | `0x02000000` | — | 引脚复用 |
| CCU | `0x02001000` | — | 时钟/复位 |
| UART3 | `0x02500c00` | 0x400 | ns16550 串口 |

> 这些数字不是“背下来考试用”。调试时你会反复看它们：
> `reserved-memory` 对不对、固件 entry 对不对、vring 地址对不对。
> 一个数字错了，现象往往是“E907 起不来”或“rpmsg 超时”。

### 为什么 E907 的 4 MiB 在 `0x43c00000`

因为它在 DDR 的高端，避开 Linux 常用的低地址。Linux 在设备树里
用 `reserved-memory` 把这段标成 `no-map`，自己就不会分配它。

```dts
reserved-memory {
    e907_dram: e907_dram@43c00000 {
        reg = <0x43c00000 0x00400000>;   /* 4 MiB */
        no-map;
    };
    vdev0buffer: vdev0buffer@43000000 {
        reg = <0x43000000 0x00040000>;   /* 256 KiB */
        no-map;
    };
    vdev0vring0: vdev0vring0@43040000 {
        reg = <0x43040000 0x00008000>;   /* 128 KiB */
        no-map;
    };
    vdev0vring1: vdev0vring1@43060000 {
        reg = <0x43060000 0x00008000>;
        no-map;
    };
};
```

`no-map` 的意思是：**Linux 不要给这段建立线性映射，也不要把它
当普通内存分配。** 这段只给 E907 和 rpmsg 用。

### 为什么不能有 `iommus`

V851S 没有 IOMMU。设备树里如果写了 `iommus = <...>`，
remoteproc 会以为需要 IOMMU，启动时报：

```text
can't attach iommu device: -22
```

所以这块板的 DTS 特意不写。**“没有 IOMMU”是硬件事实，
不是配置选项。**

---

## 6.3 MMIO：写地址就是操作硬件

CPU 访问外设寄存器，和访问内存用的是同一套指令：

```c
/* 读 UART3 的一个寄存器 */
uint32_t v = *(volatile uint32_t *)(0x02500c00 + 0x00);

/* 写 */
*(volatile uint32_t *)(0x02500c00 + 0x00) = v;
```

这叫 **MMIO**（Memory-Mapped I/O）。外设寄存器被映射到
地址空间里，读写地址就是读写寄存器。

### 为什么必须 `volatile`

编译器看到“写一个值，然后再也没读”，可能把写优化掉。
`volatile` 告诉它：**这个地址的读写有副作用，不能删、不能合并、不能重排。**

### Zephyr 的封装

```c
#include <zephyr/sys/sys_io.h>

uint32_t v = sys_read32(0x020000C4u);
sys_write32(v, 0x020000C4u);
```

比裸指针安全一点（有类型、有内存屏障语义），但本质一样。

### Linux 的封装

```c
void __iomem *m = ioremap(0x020000C4u, 4);
u32 v = readl(m);
writel(v, m);
iounmap(m);
```

内核不能直接解引用物理地址（要经过页表映射），所以用
`ioremap` 把物理地址映射成内核虚拟地址，再用 `readl/writel`。

`uart3pin.ko` 就是这么做的：

```c
m = ioremap(PIO_PE_CFG1, 4);
v = readl(m);
v = (v & ~0x00FF0000u) | PE12_13_UART3;
writel(v, m);
iounmap(m);
```

### 读-改-写

配置寄存器时几乎总是三步：

```c
v = read(reg);          /* 1. 读出现有值 */
v &= ~mask;             /* 2. 清掉要改的位 */
v |= value;             /* 3. 写入新值 */
write(reg, v);
```

如果直接 `write(reg, value)`，会把同一个寄存器里其他引脚/其他
功能的配置一起冲掉。

---

## 6.4 引脚复用：一个引脚，多种身份

### 为什么需要 pinmux

芯片引脚数量有限，一个物理引脚可以当：

- GPIO（软件控制高低）
- UART TX/RX
- I2C SCL/SDA
- PWM
- SPI
- I2S
- …

上电默认通常是 GPIO 或某个默认功能。你要用 UART3，就得把
PE12/PE13 切到“UART3 功能”，这个动作叫 **pinmux**。

### PIO 寄存器的组织

全志的 PIO 控制器按“组（bank）”组织：

```text
PIO base = 0x02000000
每组占 0x30 字节
PA = bank 0 → 0x00
PB = bank 1 → 0x30
PC = bank 2 → 0x60
PD = bank 3 → 0x90
PE = bank 4 → 0xC0
PF = bank 5 → 0xF0
...
```

每组内部：

```text
+0x00  CFG0：pin 0..7
+0x04  CFG1：pin 8..15
+0x08  CFG2：pin 16..23
+0x0C  CFG3：pin 24..31
```

每个引脚占 4 个 bit（0..15 表示 16 种功能）。

### 推导 PE12/PE13

```text
PE = bank 4
bank 偏移 = 4 × 0x30 = 0xC0
PE12/PE13 属于 pin 8..15 → CFG1 → +0x04
所以寄存器地址 = 0x02000000 + 0xC0 + 0x04 = 0x020000C4
```

在 CFG1 这个 32 位寄存器里：

```text
pin 8  → bits 0..3
pin 9  → bits 4..7
pin 10 → bits 8..11
pin 11 → bits 12..15
pin 12 → bits 16..19
pin 13 → bits 20..23
pin 14 → bits 24..27
pin 15 → bits 28..31
```

UART3 的功能号是 **7**，所以：

```text
PE12: bits 16..19 = 7
PE13: bits 20..23 = 7

二进制：0000 0000 0111 0111 0000 0000 0000 0000
十六进制：0x00770000
```

这就是 `uart3pin.c` 里那个“魔法数字”的来源。（`board_early.c`
里也有一份同样的写入，但实测不生效，见 6.4。）

### 为什么注释要写推导

半年后你只看到：

```c
#define PE12_PE13_UART3 0x00770000u
```

根本不知道它代表哪两个引脚、为什么是 7。所以代码里把
“bank → 偏移 → CFG → bit → mux”整条链写进注释。

> 好的寄存器注释不是翻译代码，是记录**推导过程和依据**
> （哪份手册、哪个驱动、哪个引脚表）。

### A7 和 E907 都写了一遍？

v12 里有两处写同一个寄存器：

1. `uart3pin.ko`（A7 内核模块）：启动 E907 前由 `start_e907`（`S52e907` 调用）插入。
2. `board_early.c`（E907 固件）：`SYS_INIT(PRE_KERNEL_1, 0)`。

看起来是“两边都可能第一个摸到引脚，所以各写一遍”。但实机测试
（v12 镜像，2026-11）证明：**只有 A7 那一处生效**。

测试方法：

**第一步**：把 `/usr/bin/start_e907` 里 `insmod uart3pin.ko` 那一行
注释掉，重启板子。

**第二步**：手动启动 E907，确认 `state=running (2)`、`trace0` 有
心跳——说明 `board_early.c` 已经跑过。

**第三步**：在 A7 上读 debugfs：

```sh
echo PE12 > /sys/kernel/debug/sunxi_pinctrl/sunxi_pin
cat /sys/kernel/debug/sunxi_pinctrl/sunxi_pin_configure
echo PE13 > /sys/kernel/debug/sunxi_pinctrl/sunxi_pin
cat /sys/kernel/debug/sunxi_pinctrl/sunxi_pin_configure
```

结果仍是 `func f`（`io_disabled`，复位默认态），不是 7。

**第四步**：手动 `insmod uart3pin.ko`，同一时刻再读，变成 `func 7`。

也就是说 E907 侧对 PIO（`0x02000000`）的写被静默忽略：不报错，
也不生效。CCU 里的 UART3 门控写没有单独验证，但能确定它不是
“引脚能被切到 UART3”的原因。

> 这类“写成功但没效果”最容易被误判成“已经配好了”。判断依据
> 必须是**读回**，而不是“代码执行到这一行、没有 fault”。

结论：

- pinmux 的唯一真实路径是 A7 的 `uart3pin.ko`（或等价的 A7 侧配置）。
- `board_early.c` 里的 PIO/CCU 写入是 no-op。保留它不会有害，但
  不能把它当成“E907 单独复位时的兜底”。
- 调试 E907 时 UART3 不通，先查 A7 有没有加载 `uart3pin.ko`，
  不要先怀疑 E907 的初始化顺序。

> “两处都写、幂等”听起来稳妥，但前提是两处都真的能写。
> 这次实测说明：**没验证过的兜底，等于没有兜底。**

---

## 6.5 时钟门控与复位

外设不是上电就能用，还要：

1. **开时钟**：没时钟，寄存器读写可能挂死或无效。
2. **释放复位**：复位信号拉着，外设一直处于初始状态。

### UART3 的时钟寄存器

```text
CCU base = 0x02001000
UART_GATE = 0x090C → 0x0200190C
bit 19 = UART3 时钟使能
bit 3  = UART3 复位释放（低有效，写 1 = 不复位）
```

代码：

```c
#define V851S_CCU_UART_GATE       0x0200190Cu
#define UART3_GATE_EN             (1u << 19)
#define UART3_RESET_DEASSERT      (1u << 3)

v = sys_read32(V851S_CCU_UART_GATE);
v |= (UART3_GATE_EN | UART3_RESET_DEASSERT);
sys_write32(v, V851S_CCU_UART_GATE);
```

### 为什么顺序重要

如果先让 UART 驱动初始化，再开时钟：

- 驱动读寄存器可能返回全 0 或总线错误。
- 配置写不进去。
- 串口没输出，但代码看起来“没报错”。

所以 `board_early.c` 用：

```c
SYS_INIT(v851s_e907_uart3_early_init, PRE_KERNEL_1, 0);
```

`PRE_KERNEL_1` 是内核启动早期阶段，优先级 0 比串口驱动
（通常 50+）更早执行。

> Zephyr 的 `SYS_INIT` 是“编译期注册的初始化函数”，
> 不是运行时动态加载。它的顺序由阶段 + 优先级决定，
> 适合做“驱动之前必须完成”的板级动作。

### 时钟树的一句话

```text
晶振/PLL → 分频器 → 外设时钟 → 门控 → 外设
```

门控（gate）就是开关。没开开关，外设收不到时钟，
所有寄存器操作都可能是无效的。

---

## 6.6 缓存、一致性与共享内存

### 两个 CPU 各有缓存

A7 有 L1/L2 cache，E907 也可能有。它们看到的内存不一定是
同一时刻的最新值：

```text
A7 写 shared[0] = 1   → 先进 A7 cache
E907 读 shared[0]     → 可能读到旧值 0
```

### 这块板怎么避免

- rpmsg 的共享内存区在 DTS 里是 `no-map`，Linux 不会把它
  当普通缓存内存使用。
- virtio/OpenAMP 在写描述符和门铃之间有**内存屏障**，
  保证顺序。
- E907 侧的 OpenAMP 库负责 cache 维护（如果配置了 cache）。

> 你写业务代码时，只要通过 `rpmsg_sendto()` 发数据、
> 通过回调收数据，就不要自己直接读写 vring。**绕过框架
> 直接操作共享内存，是双核系统最容易翻车的地方。**

### 门铃不是数据

mailbox 只传一个编号（中断），不传数据。数据在共享内存里。
顺序必须是：

```text
1. 把数据写好
2. 内存屏障（确保数据先可见）
3. 按门铃
```

如果先按门铃再写数据，对方可能读到半截。

---

## 6.7 内存预算：64 MB 怎么花

### 大致分配

```text
64 MiB DDR
├─ Linux 内核、页表、驱动        （几十 MiB，按需）
├─ 用户态进程（python、bluez）    （十几 MiB）
├─ rpmsg buffer                  256 KiB
├─ vring0 + vring1               256 KiB
└─ E907 窗口                     4 MiB
   ├─ 代码 + 只读数据            ~57 KiB（镜像）
   ├─ .bss（两个 64 KiB 环）     128 KiB
   ├─ 栈（主线程 + 两个 UART 线程）几 KiB
   └─ 剩余空闲
```

E907 的 4 MiB 窗口非常宽裕，所以环形缓冲“能大就大”没有代价。
真正紧张的是 A7 侧的 Linux 用户态。

### 为什么 `.bss` 不占固件文件

ELF 文件里 `.bss` 只记录“需要多大、启动清零”，不存实际内容。
所以把环从 16 KiB 加到 64 KiB，`firmware/zephyr_e907.elf`
几乎不变，变的是运行时内存占用。

### 实验 6.1：看内存

在板子上：

```sh
free
cat /proc/meminfo | head -5
```

在主机上看固件段：

```sh
arm-zephyr-eabi-size build/zephyr/zephyr_e907.elf
# 或
riscv64-unknown-elf-size build/zephyr/zephyr_e907.elf
```

关注 `text`、`data`、`bss` 三列。

---

## 6.8 实验：读一个寄存器

### 在 A7 上（不推荐直接 devmem）

很多 Tina 内核默认关闭 `CONFIG_DEVMEM`，`/dev/mem` 不存在。
即使存在，直接读写外设寄存器也可能和驱动打架。

正确做法：写一个小内核模块，像 `uart3pin.ko` 那样
`ioremap` + `readl`，在 `insmod` 时打印。

### 在 E907 上

```c
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(regdump, LOG_LEVEL_INF);

static int dump_regs(void)
{
    uint32_t cfg = sys_read32(0x020000C4u);
    uint32_t gate = sys_read32(0x0200190Cu);

    LOG_INF("PE_CFG1=0x%08x (PE12=%u PE13=%u)",
            cfg, (cfg >> 16) & 0xF, (cfg >> 20) & 0xF);
    LOG_INF("UART_GATE=0x%08x", gate);
    return 0;
}

SYS_INIT(dump_regs, APPLICATION, 0);
```

看 `trace0`：

```sh
tail -5 /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

预期能看到 `PE12=7 PE13=7`。

> 注意：E907 能不能访问 PIO/CCU 取决于总线权限。v12 上的实测是
> **写不生效**：E907 启动完成后从 A7 侧看，PE12/PE13 仍是
> `func f`；只有 A7 `insmod uart3pin.ko` 才变成 7。所以这个实验
> 适合确认“寄存器读得到什么”，但不能用来证明“E907 能配置引脚”。
> 引脚复用请在 A7 侧做。

---

## 6.9 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 写寄存器没效果 | 时钟没开、复位没释放 | 读回寄存器、查 CCU |
| 读寄存器全 0 | 地址错、总线没权限 | 对照手册、换 A7 侧读 |
| 改了引脚导致别的功能坏 | 没先清位、掩码太宽 | 检查 `& ~mask` |
| E907 起不来 | reserved-memory 地址/大小不对 | 对比 DTS 与固件链接地址 |
| rpmsg 超时 | vring 地址不一致 | 看 resource table 和 DTS |
| `can't attach iommu` | DTS 多了 `iommus` | 删除该属性 |
| 共享数据偶尔错 | 缓存/顺序问题 | 不要绕过 OpenAMP |
| 固件变大很多 | 大数组有初值（`.data`） | 改未初始化或放 `.bss` |
| 内存不够 | 环太大/线程栈太大 | `size`、`free` |

---

## 6.10 练习

### 练习 6.1（必做）

不看本章，写出：

1. PE12/PE13 的 CFG 寄存器地址。
2. 它们在寄存器里的 bit 范围。
3. mux 7 对应的 32 位值。

然后用 3.5 的实验程序验证。

### 练习 6.2（必做）

回答：

1. 为什么 E907 的 4 MiB 要放在 `0x43c00000`，而不是 `0x40000000`？
2. `no-map` 去掉会发生什么？
3. 为什么 DTS 不能写 `iommus`？

### 练习 6.3（必做）

在板子上执行：

```sh
cat /proc/iomem | head -40
dmesg | grep -i -E 'reserved|remoteproc|rpmsg'
```

找出 E907 的保留内存和 rpmsg 相关日志。

### 练习 6.4（选做）

把 `board_early.c` 里的 `PE12_PE13_MUX7` 改成 `0x00700000`
（只改 PE12），观察 UART3 还能不能收。解释现象。

> 警告：改之前确认你有串口/救砖手段。不要在没有备份的板子上
> 做“让系统起不来”的实验。

---

## 6.11 本章验收

1. 能画出 V851S 的简化框图，标出 A7、E907、DDR、UART、PIO、CCU。
2. 能默写关键地址：DDR、rpmsg、vring0/1、E907 窗口、UART3。
3. 能解释 MMIO 和 `volatile`，并写出读-改-写三步。
4. 能从“PE12”推导到 `0x020000C4` 的 bits 16..19。
5. 能解释时钟门控和复位为什么必须在驱动之前。
6. 能说清 `no-map`、无 IOMMU、共享内存一致性三件事的关系。

### 自测题

1. 为什么写外设寄存器要 `volatile`？
2. `0x00770000` 里两个 7 分别在哪几位？
3. 为什么 A7 和 E907 都做了一遍 pinmux？
4. E907 的 `.bss` 为什么不在固件文件里占空间？
5. rpmsg 的 mailbox 传的是数据还是通知？
6. 如果没有 `reserved-memory`，Linux 可能对 E907 的内存做什么？

> 下一章沿着时间轴走：从按下电源到网页打开，每一级引导
> 到底做了什么，失败时停在哪里。
