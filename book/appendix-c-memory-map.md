# 附录 C：内存地图与寄存器速查

> 所有地址来自这块板的实际配置。**换板子/换芯片一定要重新核对。**
> 地址写错不会“报错”，只会“行为诡异”。

---

## C.1 整体地址空间

```text
0x0000_0000 ┬─ BROM / SRAM（芯片内部）
            │
0x0200_0000 ┬─ PIO（GPIO / 引脚复用）
0x0200_1000 ├─ CCU（时钟）
0x0250_0000 ├─ UART0..3
            │
0x4000_0000 ┬─ DRAM 起始（64 MB → 0x4000_0000..0x43FF_FFFF）
            │
0x4300_0000 ├─ rpmsg 共享缓冲（256 KiB）
0x4304_0000 ├─ vring0（128 KiB）
0x4306_0000 ├─ vring1（128 KiB）
            │
0x43C0_0000 ├─ E907 固件运行区（4 MiB → 0x43C0_0000..0x43FF_FFFF）
0x4400_0000 ┴─ DRAM 结束
```

### 关键约束

```text
1. 无 IOMMU：设备地址 == 物理地址（DA == PA）
2. reserved-memory 必须 no-map，否则 Linux 会把它当普通内存
3. DTS 里不能写 iommus（没有这个硬件）
4. E907 只能访问自己的 4 MiB 窗口 + 共享区
```

### 对应 DTS 片段（形状）

```dts
reserved-memory {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges;

    e907_fw: e907-firmware@43c00000 {
        reg = <0x43c00000 0x400000>;
        no-map;
    };

    rpmsg_buf: rpmsg-buffer@43000000 {
        reg = <0x43000000 0x40000>;   /* 256 KiB */
        no-map;
    };

    vring0: vring0@43040000 {
        reg = <0x43040000 0x20000>;   /* 128 KiB */
        no-map;
    };

    vring1: vring1@43060000 {
        reg = <0x43060000 0x20000>;
        no-map;
    };
};
```

> `no-map` 的含义：**Linux 不建立映射，也不把它算进可用内存。**

---

## C.2 各区域用途

| 地址 | 大小 | 用途 | 谁访问 |
|---|---|---|---|
| `0x4300_0000` | 256 KiB | rpmsg 消息缓冲 | A7 + E907 |
| `0x4304_0000` | 128 KiB | vring0（A7→E907 或反向） | A7 + E907 |
| `0x4306_0000` | 128 KiB | vring1 | A7 + E907 |
| `0x43C0_0000` | 4 MiB | E907 固件（代码 + 数据 + 栈 + 堆） | E907 |

### 为什么 vring 要 128 KiB

```text
vring 大小 ≈ 描述符表 + avail 环 + used 环
每项几十字节，环深度几百到几千
128 KiB 足够放较大的环，减少“环满”导致的丢包
```

具体计算（概念）：

```text
desc[n]    16 字节/项
avail      2 + 2*n 字节
used       2 + 8*n 字节（含对齐）

n = 1024 → 16K + ~2K + ~8K ≈ 26 KiB
```

128 KiB 是“留足余量”的选择，不是最小值。

---

## C.3 E907 内存布局

```text
0x43C0_0000 ┬─ .text（代码）
            ├─ .rodata
            ├─ .data
            ├─ .bss
            ├─ 堆（k_malloc / sys_heap）
            ├─ 栈（每个线程）
            └─ resource table（在 .resource_table 段）
0x4400_0000 ┴─ 结束（4 MiB）
```

### 检查固件段

```sh
$ readelf -S build/zephyr/zephyr_e907.elf | \
    grep -E 'text|rodata|data|bss|resource_table|noinit'
$ readelf -l build/zephyr/zephyr_e907.elf
```

**必须看到 `.resource_table`**，否则 Linux 拒绝启动。

### 栈与堆的预算

```text
4 MiB 总量
- 代码/常量      ~200-600 KiB
- 数据/bss        ~50-200 KiB
- 堆（缓冲）      ~1-3 MiB
- 栈（每线程）    4-16 KiB × 线程数
```

> 缓冲“能大就大”的前提是**算过总量**。加一个大数组前先看 map 文件。

```sh
$ arm-zephyr-eabi-size build/zephyr/zephyr_e907.elf
$ grep -E '^\s+0x' build/zephyr/zephyr_e907.map | tail -20
```

---

## C.4 UART3

```text
基地址：0x0250_0C00
```

### 16550 兼容寄存器（偏移）

| 偏移 | 名称 | 读 | 写 |
|---|---|---|---|
| `0x00` | RBR / THR / DLL | 收字节 | 发字节 / 波特率低 |
| `0x01` | IER / DLM | 中断使能 | 波特率高 |
| `0x02` | IIR / FCR | 中断标识 | FIFO 控制 |
| `0x03` | LCR | 线控 | 数据位/停止位/校验/DLAB |
| `0x04` | MCR | 调制解调控制 | RTS/DTR/loopback |
| `0x05` | LSR | 线状态 | — |
| `0x06` | MSR | 调制解调状态 | — |
| `0x07` | SCR | 暂存 | 暂存 |

### LSR 关键位

```text
bit0  Data Ready（有数据可读）
bit5  THR Empty（发送保持寄存器空）
bit6  Transmitter Empty（发送器空）
bit1  Overrun Error（溢出，说明读太慢）
```

### 常用初始化顺序

```text
1. 配引脚复用（见 C.5）
2. 开时钟（见 C.6）
3. 解除复位（见 C.6）
4. 设 LCR.DLAB=1，写 DLL/DLM 设波特率
5. 设 LCR：8 数据位、无校验、1 停止位，DLAB=0
6. 设 FCR：使能 FIFO、清空
7. 设 IER：使能 RX 中断
```

### 波特率计算

```text
divisor = 时钟频率 / (16 × 波特率)
```

例：时钟 24 MHz，115200：

```text
divisor = 24_000_000 / (16 × 115200) = 13.0208...
→ 13（实际 115384，误差 0.16%，可接受）
```

> 误差超过 2% 就可能收到乱码。**一定要用实际时钟算，不要抄别处的值。**

---

## C.5 引脚复用：PE12 / PE13

```text
寄存器：0x0200_00C4（PIO + 0xC4）
功能：PE12、PE13 的 mux
字段：bits 16-19（PE12）、bits 20-23（PE13）
值：  0x7 = UART3
```

### 读出-改-写

```c
#define PIO_PE_CFG   0x020000C4u

uint32_t v = readl(PIO_PE_CFG);
v &= ~(0xFu << 16);          /* 清 PE12 */
v &= ~(0xFu << 20);          /* 清 PE13 */
v |=  (0x7u << 16);          /* PE12 = mux7 */
v |=  (0x7u << 20);          /* PE13 = mux7 */
writel(v, PIO_PE_CFG);
```

结果：`0x0077_0000`（如果其他位为 0）。

### 常见错误

```text
- 只写不读：把别的引脚配置清掉
- 移位写错：16-19 是 4 位，掩码 0xF
- 忘了开时钟：写了也没用
- 引脚方向：UART 由外设控制，不需要设 GPIO 方向
```

---

## C.6 CCU：时钟与复位

```text
CCU 基地址：0x0200_1000
UART_GATE：0x0200_190C
```

### UART_GATE 位定义（这块板）

```text
bit19  UART3 时钟门控（1 = 开）
bit3   UART3 复位释放（1 = 释放）
```

### 顺序

```c
/* 1. 开门控 */
uint32_t g = readl(0x0200190C);
g |= (1u << 19);
writel(g, 0x0200190C);

/* 2. 释放复位（有些实现是“写 1 释放”） */
g = readl(0x0200190C);
g |= (1u << 3);
writel(g, 0x0200190C);

/* 3. 等一会儿，再初始化 UART */
```

> 位定义在不同 SoC 上可能相反（“写 1 复位” vs “写 1 释放”）。
> **以手册和实测为准，不要凭印象。**

### 检查是否真的开了

```sh
# 板子上（如果 debugfs 可用）
# cat /sys/kernel/debug/clk/clk_summary | grep uart
```

没有 debugfs 时，用“能不能收到字节”来间接验证。

---

## C.7 PIO 其他常用寄存器

```text
PIO 基地址：0x0200_0000

+0x00  PB_CFG0
+0x04  PB_CFG1
...
+0x40  PD_CFG0
...
+0xC0  PE_CFG0
+0xC4  PE_CFG1     ← PE12/PE13 在这里
...
+0x10  数据输出（PIO_DAT）
+0x20  数据方向（PIO_DDR）
+0x30  数据输入（PIO_DAT_IN）
```

每组配置寄存器管 8 个引脚，每个引脚 4 位：

```text
bits [4k .. 4k+3] = pin (base + k) 的 mux
```

### 计算某个引脚在哪个寄存器

```text
pin 号 / 8 = 组内寄存器索引
pin 号 % 8 = 组内位置
```

例：PE12 → `12 / 8 = 1`，`12 % 8 = 4` → `PE_CFG1` 的 bits 16-19。

> 公式比背表可靠。**换一个引脚就重算一次。**

---

## C.8 rpmsg / virtio 内存

### 消息缓冲

```text
基地址：0x4300_0000
大小：  256 KiB
用途：  rpmsg 消息载荷（vring 只放描述符，不放数据）
```

### vring

```text
vring0：0x4304_0000，128 KiB
vring1：0x4306_0000，128 KiB
```

### 一个方向一个 vring

```text
vring0：A7 → E907
vring1：E907 → A7
```

（具体方向以 DTS/驱动为准；不要假设。）

### 描述符结构（virtio split ring）

```c
struct vring_desc {
    uint64_t addr;    /* 数据缓冲物理地址 */
    uint32_t len;
    uint16_t flags;   /* NEXT / WRITE / INDIRECT */
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[N];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[N];
};
```

### 一致性

无 IOMMU 时：

```text
desc.addr 必须是物理地址
共享区不能进 cache，或必须显式 flush/invalidate
```

这块板的共享区由 DTS 的 `no-map` + 驱动处理，应用层不要自己算。

---

## C.9 常见“地址类”错误

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 写寄存器没反应 | 时钟没开 | 查 CCU |
| 读回值不对 | 地址错/位错 | 用 devmem 读 |
| 系统随机崩 | 覆盖了保留内存 | 查 DTS `no-map` |
| rpmsg 收不到 | vring 地址不一致 | 对比两端 DTS |
| 固件加载失败 | 固件区大小/地址错 | `readelf -l`、DTS |
| Linux 启动内存变少 | reserved 太大 | `/proc/meminfo` |
| 引脚功能不对 | mux 值/寄存器错 | 读 PIO |
| UART 乱码 | 时钟/分频错 | 重新算 divisor |

### 用 `devmem` 直接读（板子上）

```sh
# 读 32 位
# devmem 0x020000C4 32

# 写（危险）
# devmem 0x020000C4 32 0x00770000
```

> `devmem` 是“手术刀”，不是“锤子”。**先读、再算、最后写。**

---

## C.10 一页速查

```text
DRAM           0x40000000  64 MiB
rpmsg buffer   0x43000000  256 KiB
vring0         0x43040000  128 KiB
vring1         0x43060000  128 KiB
E907 firmware  0x43C00000  4 MiB
PIO            0x02000000
CCU            0x02001000
UART_GATE      0x0200190C  bit19 时钟 / bit3 复位
UART3          0x02500C00  115200 8N1
PE12/PE13 mux  0x020000C4  bits16-19 / 20-23 = 0x7
```

> 把这一页抄在便利贴上，比每次翻手册快。
