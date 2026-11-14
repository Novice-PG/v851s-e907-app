# 第 3 章 C 补课：嵌入式真正需要的 20%

> 本章目标：
> 1. 看懂 `linkr_bridge.c` 里每一行 C 为什么那样写。
> 2. 分清栈、堆、`.bss`，知道 64 KiB 环形缓冲放在哪。
> 3. 掌握嵌入式最常用的六样东西：指针、结构体、位运算、
>    `volatile`/原子、环形缓冲、字节序。
> 4. 能自己写一个在主机上可运行、逻辑和固件一致的环形缓冲。

---

## 3.1 先说实话：你不需要学完整本 C 教材

嵌入式代码里 90% 的 C 只有这几类：

| 类型 | 例子 | 用在哪 |
|---|---|---|
| 指针 + 长度 | `rx_push(buf, n)` | 收发数据 |
| 结构体 | `struct uart_config` | 配置、协议头 |
| 位运算 | `0x00770000` | 寄存器 |
| 原子/`volatile` | `atomic_t` | 中断与线程共享 |
| 数组当环 | `ring_buf_put()` | 缓冲 |
| 大小端 | 12 字节 BLE 帧头 | 协议 |

你在课本里学的链表、红黑树、函数指针数组、复杂宏，
在这份代码里出现得很少。**先把上面六样练熟，比刷题有用。**

> 本章的代码都可以在主机上编译运行。不要只在板子上试，
> 主机上改一行编译一次，比烧录快一百倍。

---

## 3.2 内存：一个变量到底住在哪

### 三种寿命

```c
int global_counter;              /* .bss：程序启动就是 0，活到程序结束 */
static int file_counter = 3;     /* .data：有初值，活到程序结束 */

void f(void)
{
    int local = 0;               /* 栈：函数返回就没了 */
    char *p = malloc(100);       /* 堆：直到 free 才没 */
    ...
    free(p);
}
```

对嵌入式来说，关键不是“会不会用”，而是**知道它占谁的内存**：

| 区域 | 谁分配 | 大小何时定 | 风险 |
|---|---|---|---|
| `.bss` / `.data` | 编译器 | 编译时 | 太大撑爆 RAM |
| 栈 | 内核/RTOS | 创建线程时 | 递归、大数组导致溢出 |
| 堆 | `malloc` | 运行时 | 碎片、失败、泄漏 |

### 看一个真实例子

`src/linkr_bridge.c` 顶部：

```c
#define RX_RING_SIZE     65536
#define TX_RING_SIZE     65536

RING_BUF_DECLARE(uart_rx_ring, RX_RING_SIZE);
RING_BUF_DECLARE(uart_tx_ring, TX_RING_SIZE);
```

`RING_BUF_DECLARE` 展开后大意是：

```c
static uint8_t uart_rx_ring_buf[65536];
static struct ring_buf uart_rx_ring = { .buf = uart_rx_ring_buf, ... };
```

没有初值，所以 128 KiB 全部落在 `.bss`：

- 不占固件文件大小（ELF 里只记“这一段要 128 KiB，启动清零”）。
- 占 E907 的 4 MiB 内存窗口。
- 不需要 `free`，也不会碎片化。

源码注释写得很直白：

```c
/* The E907 image uses ~57 KiB of its 4 MiB DRAM window, so the rings can be
 * generous without trade-offs: 64 KiB holds ~5.7 s at 115200 ... They are
 * .bss, so the extra 120 KiB costs no NAND space in the firmware file. */
```

> 这就是“会看内存地图”的价值：同样一句 `65536`，
> 在 PC 上无所谓，在 64 MB 的板子上要算清楚。

### 实验 3.1：亲眼看到 `.bss`

在主机上写 `mem.c`：

```c
#include <stdio.h>

static unsigned char big[1024 * 1024];   /* 1 MiB，未初始化 */
static unsigned char data[16] = {1};     /* 有初值 */

int main(void)
{
    printf("big[0]=%d data[0]=%d\n", big[0], data[0]);
    return 0;
}
```

```sh
gcc mem.c -o mem
size mem
```

你会看到类似：

```text
   text    data     bss     dec     hex filename
   1234     576 1048608 1049418 10034a mem
```

`bss` 差不多是 1 MiB。现在把 `big` 改成局部变量：

```c
void f(void) { unsigned char big[1024 * 1024]; big[0] = 1; }
```

再 `size`，`bss` 会变小，但程序运行时栈会爆。**这就是“内存没消失，只是换了个地方”。**

> 嵌入式第一条纪律：**大数组不要放栈上。**
> Zephyr 线程栈默认只有一两 KiB，放一个 4 KiB 数组就溢出。

---

## 3.3 指针：不是“难”，是“地址 + 类型”

### 一句话

指针就是一个数字（地址），类型决定“一次读几个字节、怎么解释”。

```c
uint8_t  *p;   /* p 里存地址；*p 读 1 字节 */
uint32_t *q;   /* q 里存地址；*q 读 4 字节 */
```

### 真实代码里的三种用法

**用法一：函数传数组**

```c
static void rx_push(const uint8_t *buf, uint32_t len)
{
    ...
    put = ring_buf_put(&uart_rx_ring, buf, len);
    ...
}
```

C 里的数组传参，实际传的是首元素地址。`const` 表示
“这个函数保证不改 `buf` 指向的内容”，是给读代码的人看的承诺。

**用法二：指针移动**

`linkr_bridge.c` 的背压路径：

```c
uint32_t left = len - put;

p += put;                       /* 跳过已经放进环的部分 */
while (left > 0) {
    ...
    put = ring_buf_put(&uart_tx_ring, p, left);
    p += put;
    left -= put;
}
```

`p += put` 不是“加一个数”，是“地址往后挪 `put` 个字节”。
如果 `p` 是 `uint32_t *`，挪的是 `put * 4` 个字节。**类型决定步长。**

**用法三：字符串就是指针**

```c
const uint8_t *p = data;

if (len >= 9 && !memcmp(p, "LINKRSTAT", 9)) { ... }
```

`"LINKRSTAT"` 在内存里是 10 个字节：9 个字符 + `'\0'`。
`memcmp` 只比 9 个，因为 rpmsg 消息不一定带结尾 0。

### 实验 3.2：打印地址，消除恐惧

```c
#include <stdio.h>

int main(void)
{
    int a[4] = {10, 20, 30, 40};
    int *p = a;

    printf("a     = %p\n", (void *)a);
    printf("&a[0] = %p\n", (void *)&a[0]);
    printf("p     = %p\n", (void *)p);
    printf("*p    = %d\n", *p);
    printf("*(p+2)= %d\n", *(p + 2));
    printf("p+2   = %p\n", (void *)(p + 2));
    return 0;
}
```

观察：`a == &a[0] == p`，而 `p + 2` 的地址比 `p` 大 `2 * sizeof(int)`。

### 空指针和野指针

```c
if (!ept || !data) {
    return -EINVAL;
}
```

这是防御式写法：调用者可能传 `NULL`。嵌入式里宁可多一行判断，
也不要让设备在现场崩溃。

野指针是“没初始化的指针”，它指向随机地址。规则：

- 指针定义时就初始化，不知道就写 `NULL`。
- `free` 之后立刻置 `NULL`。
- 不要返回局部数组的地址。

---

## 3.4 结构体：把一组相关字段绑在一起

### 定义与初始化

```c
static struct uart_config active_cfg = {
    .baudrate = 115200,
    .parity = UART_CFG_PARITY_NONE,
    .stop_bits = UART_CFG_STOP_BITS_1,
    .data_bits = UART_CFG_DATA_BITS_8,
    .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};
```

`.字段 = 值` 叫“指定初始化器”。好处：

- 顺序无关，漏写自动为 0。
- 加字段时不用改所有初始化点。
- 读代码时一眼看出每个值对应哪个字段。

> 对比 `{115200, 0, 0, 3, 0}`：后者过三个月你自己都看不懂。

### 结构体指针用 `->`

```c
active_cfg.baudrate      /* 结构体变量用 . */
cfg->baudrate            /* 结构体指针用 -> */
```

`cfg->x` 就是 `(*cfg).x` 的简写。

### 跨语言的结构体：`EndpointInfo`

A7 侧 Python 要调用内核 ioctl，需要一个和 C 完全一致的内存布局：

```python
class EndpointInfo(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 32),
        ("src", ctypes.c_uint32),
        ("dst", ctypes.c_uint32),
    ]
```

对应内核头文件里大概是：

```c
struct rpmsg_endpoint_info {
    char name[32];
    __u32 src;
    __u32 dst;
};
```

两边必须**字段顺序、宽度、对齐**完全一致，否则内核读到的
`src` 会是名字的一部分。

> 这就是“协议”的另一个层面：不只是网络包，内存里的结构体
> 也是协议。跨语言、跨 CPU 时，字段宽度要用 `uint32_t` 这种
> 明确宽度的类型，不要用 `int`、`long`。

### 位域？先别用

C 支持：

```c
struct flags {
    unsigned ready : 1;
    unsigned mode  : 3;
};
```

但位域的排布依赖编译器和字节序，跨平台协议里**不要用**。
协议字段老老实实用 `uint8_t`/`uint16_t` 加移位。

---

## 3.5 位运算：寄存器就是这么读写的

### 四个运算

| 运算 | 写法 | 用途 |
|---|---|---|
| 与 | `x & mask` | 取某几位 |
| 或 | `x \| mask` | 置某几位为 1 |
| 异或 | `x ^ mask` | 翻转某几位 |
| 取反 | `~mask` | 清零某几位（配合与） |

常见组合：

```c
x |= (1u << 5);          /* 把第 5 位置 1 */
x &= ~(1u << 5);         /* 把第 5 位清 0 */
x ^= (1u << 5);          /* 翻转第 5 位 */
y = (x >> 4) & 0x7;      /* 取第 4..6 位 */
```

### 真实例子：引脚复用

`src/board_early.c`：

```c
#define V851S_PIO_CFG_PE12_PE13   0x020000C4u
#define PE12_PE13_MUX7            0x00770000u
```

注释里写了推导过程：

```text
PE bank = pin 128.. -> bank 4 -> 4*0x30 = 0xC0
pins 8..15 in CFG1 at 0xC4; pin12 -> bits16..19, pin13 -> bits20..23
```

也就是：

- PE12 的功能选择占 bit16..19；
- PE13 占 bit20..23；
- 每个字段写 7，表示“复用为 UART3”。

```text
bit:  23..20  19..16  15..0
值:   7        7       0
hex:  0x00770000
```

写寄存器时通常要“先清零，再或”：

```c
reg &= ~(0xFu << 16);       /* 清 PE12 的 4 位 */
reg |=  (7u   << 16);       /* 设成 7 */
```

如果直接 `reg |= 0x00770000`，原来的值可能还留着，
结果就是“7 或 旧值”，不一定是 7。

> 读寄存器代码时，先找“掩码”和“移位”，再看值。
> `0x00770000` 不是魔法，是“两个 7 放在特定位置”。

### 实验 3.3：手工拆一个寄存器值

```c
#include <stdio.h>

int main(void)
{
    unsigned int v = 0x00770000u;

    printf("PE12 field = 0x%x\n", (v >> 16) & 0xF);
    printf("PE13 field = 0x%x\n", (v >> 20) & 0xF);
    printf("bits set   =");
    for (int i = 31; i >= 0; i--) {
        putchar((v >> i) & 1 ? '1' : '0');
    }
    putchar('\n');
    return 0;
}
```

---

## 3.6 `volatile`、原子操作、内存序

### 问题：编译器会“自作聪明”

```c
int ready = 0;

void isr(void)      { ready = 1; }
void worker(void)   { while (!ready) { } }
```

`worker` 里的 `ready` 每次循环都从内存读吗？不一定。
优化开启后，编译器可能把它提到寄存器里，变成死循环。

`volatile` 告诉编译器：**这个变量可能被程序之外的东西改，
每次都老老实实从内存读/写。**

```c
volatile int ready;
```

什么时候需要：

- 中断服务程序改、主循环读的简单标志。
- 硬件寄存器映射到内存地址（`*(volatile uint32_t *)0x...`）。

什么时候不够：

- 多线程/多核共享的计数器，`x++` 不是一步完成的。
- 需要“读-改-写”原子性的地方。

> `volatile` 不是锁，也不保证原子性。它只解决“别优化掉”。

### Zephyr 的 `atomic_t`

`linkr_bridge.c`：

```c
static atomic_t bridge_ready;
static atomic_t uart_rx_total;
static atomic_t uart_rx_dropped;
static atomic_t uart_tx_dropped;
```

使用：

```c
atomic_set(&bridge_ready, 1);
if (atomic_get(&bridge_ready)) { ... }
atomic_add(&uart_rx_dropped, len - put);
```

这些函数保证“加”“设”“读”是原子的，不会被中断或其他线程
插在中间。计数器在 ISR 和线程里同时更新，所以必须用原子类型。

### 为什么普通 `int` 会丢计数

假设 `counter = 5`，两个执行流同时做 `counter++`：

```text
线程 A：读 5
线程 B：读 5
线程 A：写 6
线程 B：写 6      <- 少加了一次
```

结果应该是 7，实际是 6。`atomic_add` 在硬件层面用
原子指令完成“读-改-写”，不会出现这个窗口。

### 自旋锁：保护一整段操作

```c
static struct k_spinlock rx_lock;

key = k_spin_lock(&rx_lock);
put = ring_buf_put(&uart_rx_ring, buf, len);
k_spin_unlock(&rx_lock, key);
```

为什么这里不只用原子？因为 `ring_buf_put` 要同时改
`head`、`tail` 和“已用空间”，中间状态不能给别人看到。
自旋锁把这一段变成“要么全做，要么没做”。

规则：

- 临界区越短越好，不要在锁里做 `printk`、`k_msleep`、I/O。
- ISR 和线程共用锁时用 `k_spin_lock`（关中断），不要用会睡眠的互斥量。
- 锁的获取顺序要固定，避免死锁。

### 内存序（先知道有这回事）

多核系统里，CPU 可能重排读写顺序。`atomic_set` 之后
另一个核看到的顺序不一定和代码一致。Zephyr/Linux 提供
`memory barrier` 和带内存序的原子操作。

这块板是 A7 + E907 双核，rpmsg 的 vring 就有严格的内存序要求。
好消息是：**OpenAMP 库已经帮你处理了**，你写业务代码时
只要不绕过它直接操作共享内存，就不用担心。

> 本章只需要记住：**中断/线程共享的简单标志用 `volatile`，
> 共享的计数器用 `atomic_*`，共享的数据结构用锁。**

---

## 3.7 环形缓冲：嵌入式最重要的一个数据结构

### 为什么不用“普通数组 + memmove”

串口每秒来 11500 字节。如果每来一个字节就把后面所有数据往前挪，
CPU 全花在搬运上。环形缓冲让“读”和“写”各自移动一个下标，
不搬数据，O(1)。

### 原理图

```text
size = 8

初始： head=0, tail=0
[ . . . . . . . . ]
  ^head
  ^tail

写入 3 个字节 A B C： tail 前进
[ A B C . . . . . ]
  ^head    ^tail

读取 2 个字节： head 前进，返回 A B
[ A B C . . . . . ]
      ^head  ^tail

再写 6 个字节 D E F G H I：tail 绕回
[ A B C D E F G H ]   -> 实际只能再写 5 个，I 放不下
      ^head  ^tail
```

- `head`：读位置。
- `tail`：写位置。
- 已用 = `(tail - head + size) % size`。
- 空：`head == tail`。
- 满：`(tail + 1) % size == head`（留一个空位区分空和满）。

### 用位与代替取模

如果 `size` 是 2 的幂：

```c
next = (tail + 1) & (size - 1);    /* 等价于 % size，但快得多 */
```

65536 是 2 的 16 次方，所以 `& 0xFFFF` 就行。
这就是为什么环形缓冲大小几乎总是 2 的幂。

### 一个能跑的最小实现

```c
/* ring.h */
#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>

struct ring {
    uint8_t *buf;
    uint32_t size;   /* 必须是 2 的幂 */
    uint32_t head;   /* 读 */
    uint32_t tail;   /* 写 */
};

static inline uint32_t ring_used(const struct ring *r)
{
    return (r->tail - r->head) & (r->size - 1);
}

static inline uint32_t ring_space(const struct ring *r)
{
    return r->size - 1 - ring_used(r);
}

static inline uint32_t ring_put(struct ring *r, const uint8_t *src, uint32_t len)
{
    uint32_t i;

    if (len > ring_space(r)) {
        len = ring_space(r);
    }
    for (i = 0; i < len; i++) {
        r->buf[r->tail] = src[i];
        r->tail = (r->tail + 1) & (r->size - 1);
    }
    return len;
}

static inline uint32_t ring_get(struct ring *r, uint8_t *dst, uint32_t len)
{
    uint32_t i;

    if (len > ring_used(r)) {
        len = ring_used(r);
    }
    for (i = 0; i < len; i++) {
        dst[i] = r->buf[r->head];
        r->head = (r->head + 1) & (r->size - 1);
    }
    return len;
}

#endif
```

```c
/* ring_test.c */
#include <stdio.h>
#include <string.h>
#include "ring.h"

static uint8_t storage[8];
static struct ring r = { .buf = storage, .size = 8 };

int main(void)
{
    uint8_t out[16];

    printf("put ABC -> %u, used=%u\n",
           ring_put(&r, (const uint8_t *)"ABC", 3), ring_used(&r));
    printf("get 2   -> %u, used=%u\n",
           ring_get(&r, out, 2), ring_used(&r));
    out[2] = '\0';
    printf("out=%s\n", out);

    /* 绕回测试 */
    ring_put(&r, (const uint8_t *)"DEFGH", 5);
    printf("after wrap used=%u space=%u\n", ring_used(&r), ring_space(&r));
    return 0;
}
```

编译：

```sh
gcc -Wall -Wextra ring_test.c -o ring_test
./ring_test
```

### 和固件的差别

固件的 `ring_buf_put` 比这个版本多做几件事：

1. 加锁（`k_spin_lock`），因为 ISR 和线程同时访问。
2. 分两段拷贝，处理“写到末尾要绕回”的情况，不用逐字节循环。
3. 提供 `ring_buf_size_get()` 查询占用。

逻辑是一样的。**你先把这个最小版本跑通，再去看 Zephyr 的实现，
就不会被 API 吓到。**

### 为什么是 64 KiB

| 波特率 | 每秒字节（约） | 64 KiB 能吸收 |
|---|---|---|
| 115200 | 11520 | 5.7 s |
| 1.5 Mbaud | 150000 | 437 ms |
| 4 Mbaud | 400000 | 164 ms |

突发越长、波特率越高，越需要大缓冲。代价只是 `.bss`。

> 注意：缓冲不是万能的。如果生产者长期比消费者快，
> 再大的缓冲也会满。这时需要**背压**：让上游慢下来。
> 固件里用 `uart_tx_space_sem` 等 100 ms，就是这个思路。

---

## 3.8 字节序与协议：12 字节帧头

### 大端与小端

一个 `uint32_t x = 0x11223344` 在内存里：

```text
小端（x86、ARM 默认）：44 33 22 11
大端（部分网络协议）：  11 22 33 44
```

A7（ARM）和 E907（RISC-V）默认都是小端，所以这块板上
两边直接传结构体不会翻车。但**跨架构、跨语言、过网络时
不能假设小端**。

### 手工拼一个小端 32 位

```c
static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}
```

不要写 `*(uint32_t *)p`：

- 可能不对齐（某些架构直接 fault）。
- 受字节序影响。
- 违反 strict aliasing，优化后行为不可预测。

### BLE 帧头

`docs/PROTOCOL.md` 里的 12 字节头：

```text
偏移  长度  含义
0     2     magic
2     1     version
3     1     type / 0
4     4     seq / reqid（小端）
8     2     payload length（小端）
10    2     flags（小端）
12    N     payload
```

注意偏移：magic 占 0..1，version 在 2，type 在 3。
写解析代码时，**先检查长度，再取字段**：

```c
if (len < 12) return -EINVAL;
if (p[0] != 'L' || p[1] != 'R') return -EPROTO;
seq = get_le32(p + 4);
plen = (uint16_t)(p[8] | (p[9] << 8));
if (12 + plen > len) return -EMSGSIZE;
```

### 实验 3.4：手工解析

```c
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    /* magic 'L','R', version 1, type 0, seq 0x01020304, len 5, flags 0 */
    uint8_t f[12] = {'L', 'R', 1, 0,
                     0x04, 0x03, 0x02, 0x01,
                     0x05, 0x00, 0x00, 0x00};

    uint32_t seq = (uint32_t)f[4] | ((uint32_t)f[5] << 8)
                 | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
    uint16_t len = (uint16_t)(f[8] | (f[9] << 8));

    printf("magic=%c%c ver=%u type=%u seq=0x%08x len=%u\n",
           f[0], f[1], f[2], f[3], seq, len);
    return 0;
}
```

预期：`seq=0x01020304 len=5`。

---

## 3.9 字符串与格式化：最容易出错的地方

### 字符串以 `'\0'` 结尾

```c
char s[4] = "abc";     /* a b c \0 */
```

`s[3]` 是 0。`strlen(s)` 是 3，`sizeof(s)` 是 4。

### 三个常见事故

**事故一：忘了留结尾 0**

```c
char buf[4];
memcpy(buf, "abcd", 4);   /* buf 里没有 \0 */
printf("%s\n", buf);      /* 越界读，可能打印垃圾或崩溃 */
```

**事故二：`strncpy` 不保证结尾**

```c
char dst[4];
strncpy(dst, "abcdef", sizeof(dst));   /* dst = 'a','b','c','d'，没有 0 */
```

正确写法：

```c
snprintf(dst, sizeof(dst), "%s", src);
```

**事故三：`snprintf` 返回值不是“实际写入长度”**

`snprintf` 返回“如果空间够，本来想写多少”，可能大于缓冲区。
所以判断截断要看返回值：

```c
int n = snprintf(buf, sizeof(buf), "%s", long_string);
if (n < 0) { /* 编码错误 */ }
else if ((size_t)n >= sizeof(buf)) { /* 被截断 */ }
```

固件里这段就是这样：

```c
n = snprintk(reply, sizeof(reply), "LINKR1 OK rx=%u\n", ...);
if (n < 0) {
    n = sizeof("LINKR1 OK\n") - 1;
    memcpy(reply, "LINKR1 OK\n", n);
}
return rpmsg_sendto(ept, reply, n, src);
```

`snprintk` 是 Zephyr 的 `snprintf`，返回负值表示失败。
失败时要给一个最小可用回复，不能把负值当长度传给 `rpmsg_sendto`。

### `sizeof` 的坑

```c
void f(char buf[100])
{
    printf("%zu\n", sizeof(buf));   /* 是指针大小，不是 100 */
}
```

数组传参后退化成指针。**长度必须单独传。**

---

## 3.10 宏、编译期常量、条件编译

### `#define` 不是变量

```c
#define RX_CHUNK 256
```

编译前文本替换，没有类型、没有地址、调试器看不到。
好处是零开销；坏处是出错时信息难懂。

需要类型时用：

```c
static const uint32_t rx_chunk = 256;
```

### 带参数的宏要加括号

```c
#define MAX(a, b) ((a) > (b) ? (a) : (b))
```

每个参数、整个表达式都要括号，否则 `MAX(x, y+1) * 2` 会算错。
嵌入式里更推荐 `static inline` 函数，调试友好。

### 条件编译

```c
#ifdef CONFIG_LOG
    LOG_INF("...");
#endif
```

Zephyr 的 `CONFIG_*` 来自 `prj.conf` 和 Kconfig。编译时决定
“这段代码在不在”，不占运行时开销。

### 真实例子：`K_THREAD_STACK_DEFINE`

```c
K_THREAD_STACK_DEFINE(uart_rx_stack, 1536);
static struct k_thread uart_rx_thread_data;
```

展开后是一个对齐的 `uint8_t` 数组 + 一些元数据。
1536 字节是栈大小，不是“函数大小”。如果线程里调用层级深、
有大局部数组，就要加大。

### `ARG_UNUSED`

```c
ARG_UNUSED(user_data);
```

线程函数签名固定是 `(void *a, void *b, void *c)`，但这里不用参数。
`ARG_UNUSED` 告诉编译器“我知道没用，别警告”。

---

## 3.11 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 程序跑飞、重启 | 栈溢出、野指针 | 加大栈、开 `CONFIG_STACK_SENTINEL` |
| 计数少了几次 | 普通 `int` 被并发修改 | 改 `atomic_t` |
| 循环永远不退出 | 共享标志没 `volatile` | 加 `volatile` 或用原子 |
| 串口数据错位 | 结构体/协议字段宽度不一致 | 用固定宽度类型，打印偏移 |
| 打印乱码 | 字符串没结尾、长度算错 | `xxd` 看原始字节 |
| 大数组导致启动失败 | `.bss` 超过 RAM | `size`、链接 map 文件 |
| `snprintf` 后字符串被截断 | 返回值当成了写入长度 | 比较返回值和缓冲区大小 |
| 寄存器写了没效果 | 没先清零、时钟没开 | 读回寄存器、查手册 |
| 中断里死机 | ISR 里睡眠/打印/加互斥量 | ISR 只做最少的事 |

---

## 3.12 练习

### 练习 3.1（必做）

把 3.7 的 `ring.h` / `ring_test.c` 跑通，然后：

1. 把 `size` 改成 16，观察 `ring_space` 的变化。
2. 写一个循环，持续 `put` 1 字节直到满，打印 `used` 和 `space`。
3. 故意把 `size` 改成 10（不是 2 的幂），观察 `& (size-1)` 出错。
   然后用 `% size` 修正，比较两者结果。

### 练习 3.2（必做）

写 `frame.c`，实现：

```c
int frame_parse(const uint8_t *buf, size_t len,
                uint32_t *seq, uint16_t *plen);
```

要求：

- `len < 12` 返回 `-1`。
- magic 不是 `"LR"` 返回 `-2`。
- `12 + plen > len` 返回 `-3`。
- 成功返回 0，并填 `seq`、`plen`。

用 3.8 的实验数据测试。

### 练习 3.3（选做）

给 `ring.h` 加一个 `ring_peek()`：读出数据但不移动 `head`。
提示：先算 `used`，再逐字节取 `(head + i) & (size - 1)`。

### 练习 3.4（选做）

读 `src/linkr_bridge.c` 的 `rx_push()`，回答：

1. 为什么 `ring_buf_put` 要放在 `k_spin_lock` 里？
2. 为什么 `atomic_add(&uart_rx_total, put)` 放在锁外面也可以？
3. 如果去掉锁，最坏会发生什么？

---

## 3.13 本章验收

1. 能解释 `.bss`、栈、堆的区别，并说出 64 KiB 环形缓冲在哪。
2. 能手算 `0x00770000` 对应的两个引脚和字段值。
3. 能说清 `volatile`、`atomic_t`、`k_spinlock` 各自解决什么问题，
   以及为什么不能互相替代。
4. 能默写环形缓冲的“空/满/绕回”判断。
5. 能手工解析一个 12 字节小端帧头。
6. 练习 3.1 和 3.2 的代码能编译、能跑、结果正确。

### 自测题

1. 为什么 `RING_BUF_DECLARE(..., 65536)` 不增加固件文件大小？
2. `p += put` 移动了多少字节？如果 `p` 是 `uint32_t *` 呢？
3. `volatile int x` 能保证 `x++` 原子吗？为什么？
4. 为什么环形缓冲大小通常是 2 的幂？
5. 直接 `*(uint32_t *)p` 读协议字段有哪三个风险？
6. `snprintf` 返回 100 但缓冲区只有 32 字节，说明什么？

> 下一章换到 Linux 侧：进程、文件描述符、socket、权限、挂载。
> 这些是看懂 daemon 的前提。
