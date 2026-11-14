# 第 14 章 Zephyr 入门：线程、同步、内存

> 本章目标：
> 1. 知道 Zephyr 是什么、和 Linux 有什么本质差别。
> 2. 会创建线程、设置优先级，知道什么在中断里能做什么。
> 3. 会用 `k_sem`、`k_msgq`、`k_mutex`、`k_spinlock`、`atomic`、`ring_buf`。
> 4. 理解 `SYS_INIT` 的初始化顺序。
> 5. 知道这块板的日志为什么走 RAM console，怎么看。

---

## 14.1 Zephyr 不是“小 Linux”

很多人第一次接触 Zephyr，会拿它和 Linux 比，然后得出错误的结论：

> “Linux 有 fork，Zephyr 没有；Linux 有文件系统，Zephyr 没有；
> 所以 Zephyr 是个残缺的系统。”

反过来才对：**Zephyr 不是残缺的 Linux，它是另一类系统。**

| 对比项 | Linux | Zephyr |
|---|---|---|
| 类型 | 通用操作系统 | 实时操作系统（RTOS） |
| 地址空间 | 进程隔离，每个进程独立 | 所有线程共享一个地址空间 |
| 隔离单位 | 进程 | 线程（没有内存保护，除非开 MPU） |
| 启动时间 | 秒级 | 毫秒级 |
| 代码量 | 几千万行 | 可裁剪到几十 KB |
| 调度 | CFS 等复杂调度器 | 固定优先级抢占调度 |
| 内存分配 | `malloc`/`free` 灵活 | 尽量静态分配 |
| 驱动模型 | 设备树 + 总线 | 设备树 + 编译期生成 |
| 出错后果 | 进程崩，系统还在 | 线程崩，整个固件崩 |

### 一句话区别

- Linux：**让很多程序公平地共享一台机器。**
- Zephyr：**让几个任务确定地在确定时间内跑完。**

这块板上：

- A7 跑 Linux，因为要网络、文件、蓝牙协议栈、网页。
- E907 跑 Zephyr，因为要**确定地**在几十微秒内响应 UART。

> 把实时任务放到 Linux 上不是不行（PREEMPT_RT），但代价大、
> 调优复杂。用一颗小核专职做实时，是更常见的工程选择。

---

## 14.2 一个 Zephyr 程序长什么样

最小的 Zephyr 应用只有两个文件：

```text
my_app/
  CMakeLists.txt
  prj.conf
  src/main.c
```

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app)
target_sources(app PRIVATE src/main.c)
```

```conf
# prj.conf
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
```

```c
#include <zephyr/kernel.h>

int main(void)
{
    printk("hello zephyr\n");
    return 0;
}
```

构建：

```sh
export ZEPHYR_BASE=/path/to/zephyr
cmake -B build -GNinja -DBOARD=<your_board> .
ninja -C build
```

### `main` 也是一个线程

在 Zephyr 里，`main()` 不是“程序的开始和结束”，它是**主线程的入口**。

- `main` 返回后，主线程结束。
- 其他线程继续跑。
- 如果所有线程都结束，系统进入 idle。

所以固件里常见的写法是：

```c
int main(void)
{
    /* 初始化 */
    while (1) {
        /* 或者干脆在这里睡，把工作交给别的线程 */
        k_msleep(1000);
    }
    return 0;
}
```

> 不要以为 `main` 返回 = 程序退出。嵌入式没有“退出”，
> 只有“继续跑”或“复位”。

---

## 14.3 线程：比 pthread 更轻，也更危险

### 创建线程

静态方式（推荐）：

```c
#include <zephyr/kernel.h>

void my_thread(void *a, void *b, void *c)
{
    while (1) {
        printk("tick\n");
        k_msleep(1000);
    }
}

K_THREAD_STACK_DEFINE(my_stack, 1024);
static struct k_thread my_thread_data;

int main(void)
{
    k_thread_create(&my_thread_data, my_stack,
                    K_THREAD_STACK_SIZEOF(my_stack),
                    my_thread, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    k_thread_name_set(&my_thread_data, "my_thread");
    return 0;
}
```

逐段：

- `K_THREAD_STACK_DEFINE`：静态分配栈。**栈大小要自己估。**
- `struct k_thread`：线程控制块，静态分配。
- `k_thread_create`：参数是“控制块、栈、栈大小、入口、
  三个参数、优先级、选项、启动延迟”。
- `K_PRIO_PREEMPT(5)`：抢占式优先级。
- `K_NO_WAIT`：立刻可运行。
- `k_thread_name_set`：给调试器/日志一个名字。

### 优先级：数字越小越优先

Zephyr 的优先级规则和 Linux 相反：

```text
数字小 = 优先级高
```

范围通常是：

```text
-16 .. -1   协作式（cooperative），不会被抢占
 0 .. 15    抢占式（preemptive），按优先级抢占
```

这块板实际用的：

| 线程 | 优先级 | 作用 |
|---|---|---|
| `openamp` | 6 | OpenAMP 初始化 |
| `openamp_rx` | 6 | 处理 mailbox 通知、rpmsg |
| `uart_rx` | 7 | UART 收到字节 → rpmsg |
| `uart_tx` | 8 | rpmsg 收到字节 → UART |
| `main` | 默认 | 心跳、初始化 |

数字上 `openamp_rx`(6) 比 `uart_rx`(7) 高，所以门铃处理
不会被 UART 线程挡住。

> 优先级设计不是“越高越好”。所有线程都设最高，
> 等于没有优先级，还会饿死低优先级线程。

### 协作式 vs 抢占式

```c
K_PRIO_COOP(2)    /* 协作式：只有它主动让出/yield/睡眠，别人才跑 */
K_PRIO_PREEMPT(2) /* 抢占式：更高优先级线程就绪会立刻抢占 */
```

协作式适合“很短、必须完整执行”的关键段。用错会导致
系统看起来“卡住”。

### 和 pthread 对比

| 概念 | Linux pthread | Zephyr |
|---|---|---|
| 创建 | `pthread_create` | `k_thread_create` |
| 栈 | 默认 8 MB，可调 | 必须显式指定，常 1～4 KB |
| 优先级 | `pthread_setschedparam` | 创建时指定 |
| 睡眠 | `sleep`/`usleep` | `k_sleep`/`k_msleep` |
| 让出 | `sched_yield` | `k_yield` |
| 退出 | `pthread_exit` | 函数返回 |
| 隔离 | 同进程内共享地址空间 | 全局共享地址空间 |

### 栈溢出

Zephyr 没有 MMU 保护（除非开 MPU/用户模式）。栈溢出会
**悄悄写坏别的内存**，表现为“随机崩溃”。

预防：

```conf
CONFIG_THREAD_STACK_INFO=y
CONFIG_INIT_STACKS=y
```

运行时看栈用量：

```c
size_t unused = k_thread_stack_space_get(&my_thread_data);
```

或者用 `CONFIG_THREAD_ANALYZER`。经验值：

- 简单线程：1 KB 起。
- 调用 printf/浮点/递归：2～4 KB。
- 这块板 UART 线程 1536 字节，OpenAMP 线程 2048 字节。

> 栈按 1 KB 估，宁大勿小。多 1 KB 的 RAM 通常比“随机崩溃”便宜。

---

## 14.4 中断：能做什么，不能做什么

### 中断上下文的三条铁律

1. **不能睡眠**：不能 `k_sleep`、不能等信号量（除非 `K_NO_WAIT`）。
2. **不能调用可能阻塞的 API**：`k_mutex_lock`、`k_msgq_get(K_FOREVER)` 都不行。
3. **要快**：中断里做太久，其他中断和线程都被耽误。

### 这块板的 UART ISR

```c
static void uart_isr(const struct device *dev, void *user_data)
{
    uint8_t buf[64];

    uart_irq_update(dev);
    while (uart_irq_is_pending(dev)) {
        int n;
        if (!uart_irq_rx_ready(dev)) {
            break;
        }
        n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        rx_push(buf, n);          /* 只放进环形缓冲 */
    }
    k_sem_give(&uart_rx_sem);     /* 叫醒线程 */
}
```

ISR 只做两件事：**把字节搬进 ring buffer，给信号量**。
真正的 rpmsg 发送在 `uart_rx_thread` 里做。

### 为什么不能在 ISR 里直接 `rpmsg_sendto`

- `rpmsg_sendto` 可能要等 buffer、做内存操作、甚至阻塞。
- 它执行时间不固定，会拉长中断延迟。
- 它可能调用别的可能睡眠的 API。

> 通用模式：**ISR 只搬运，线程做重活。** 第 11 章已经讲过。

### 关中断 vs 自旋锁

Zephyr 里保护“ISR 和线程都会访问”的数据，用 `k_spinlock`：

```c
static struct k_spinlock rx_lock;

static void rx_push(const uint8_t *buf, uint32_t len)
{
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    uint32_t put = ring_buf_put(&uart_rx_ring, buf, len);
    k_spin_unlock(&rx_lock, key);
    ...
}
```

`k_spin_lock` 在单核上通常是关中断，在多核上是自旋 +
关本地中断。它**不能被长时间持有**，里面不能睡眠。

> 这块板是单核 RISC-V，`k_spin_lock` 基本等价于短暂关中断。
> 临界区越短越好。

---

## 14.5 同步原语：五个够用一辈子

### `k_sem`：信号量

用途：**通知“有事情发生了”**，或者**限制并发数量**。

```c
K_SEM_DEFINE(my_sem, 0, 1);   /* 初值 0，最大 1 */

/* 线程 A：等 */
k_sem_take(&my_sem, K_FOREVER);

/* 线程 B / ISR：给 */
k_sem_give(&my_sem);
```

这块板的用法：

| 信号量 | 谁给 | 谁等 | 含义 |
|---|---|---|---|
| `uart_rx_sem` | UART ISR | `uart_rx` 线程 | 收到字节了 |
| `uart_tx_sem` | rpmsg 回调 | `uart_tx` 线程 | 有数据要发 |
| `uart_tx_space_sem` | `uart_tx` 线程 | rpmsg 回调 | TX 环腾出空间了 |

### `k_msgq`：消息队列

用途：**在中断和线程之间传小消息**，有固定容量。

```c
K_MSGQ_DEFINE(my_q, sizeof(uint32_t), 8, 4);

/* ISR */
uint32_t v = 42;
k_msgq_put(&my_q, &v, K_NO_WAIT);

/* 线程 */
uint32_t got;
k_msgq_get(&my_q, &got, K_FOREVER);
```

这块板用它传 mailbox 的 notify id：

```c
K_MSGQ_DEFINE(ipm_msgq, sizeof(uint32_t), 8, 4);

static void ipm_rx_cb(...)
{
    uint32_t notifyid = VRING1_ID;
    if (data != NULL) {
        notifyid = *(volatile uint32_t *)data;
    }
    if (k_msgq_put(&ipm_msgq, &notifyid, K_NO_WAIT) != 0) {
        atomic_set(&ipm_overflow, 1);   /* 队列满，标记稍后重扫 */
    }
}
```

### `k_mutex`：互斥锁

用途：**保护一段临界区，可以睡眠**。

```c
K_MUTEX_DEFINE(my_mutex);

k_mutex_lock(&my_mutex, K_FOREVER);
/* 访问共享资源，可能耗时 */
k_mutex_unlock(&my_mutex);
```

和 `k_spinlock` 的区别：

| | `k_spinlock` | `k_mutex` |
|---|---|---|
| 能在 ISR 用 | 能 | 不能 |
| 持有期间能睡眠 | 不能 | 能 |
| 开销 | 很小 | 较大 |
| 适用 | 极短临界区、ISR 共享 | 较长临界区、线程间 |

> 规则：**ISR 会碰的数据用 spinlock；只在线程间用的用 mutex。**
> 不确定时，优先把临界区缩短，而不是换更重的锁。

### `atomic_t`：原子计数

用途：**单个整数的无锁读写/加减**。

```c
static atomic_t uart_rx_total;

atomic_add(&uart_rx_total, n);
uint32_t v = atomic_get(&uart_rx_total);
```

这块板用它统计 RX/TX 字节和丢包数。这些统计：
- 不需要“读-改-写”整体一致，只要单个操作原子。
- 不加锁，开销小。
- ISR 和线程都能安全操作。

比较并交换：

```c
if (atomic_cas(&ipm_overflow, 1, 0)) {
    /* 之前是 1，现在清成 0，说明确实溢出过 */
}
```

### `ring_buf`：环形缓冲

用途：**字节流的高效缓冲**，生产者写、消费者读。

```c
RING_BUF_DECLARE(my_ring, 1024);

/* 生产者 */
uint32_t put = ring_buf_put(&my_ring, data, len);

/* 消费者 */
uint32_t got = ring_buf_get(&my_ring, buf, sizeof(buf));
```

这块板用 64 KiB 的 RX/TX 环：

```c
#define RX_RING_SIZE 65536
RING_BUF_DECLARE(uart_rx_ring, RX_RING_SIZE);
RING_BUF_DECLARE(uart_tx_ring, TX_RING_SIZE);
```

需要 `CONFIG_RING_BUFFER_LARGE=y`，因为索引超过 16 位。

> 64 KiB 在 115200 下能缓 ~5.7 秒，在 4 Mbaud 下能缓
> ~164 ms。这是“背压”的物质基础。

### 五个原语怎么选

```text
只是通知一件事         → k_sem
传一个小消息           → k_msgq
保护较长临界区         → k_mutex
ISR 也要用的极短临界区 → k_spinlock
单个计数器             → atomic
字节流缓冲             → ring_buf
```

---

## 14.6 `SYS_INIT`：在 `main` 之前跑

### 为什么需要

UART 控制台驱动需要在 `main` 之前初始化。但这块板的
UART3 引脚和时钟没人配：

- Linux 侧 `&uart3` 是 `disabled`。
- A7 内核没开 `CONFIG_DEVMEM`。
- E907 必须自己配。

### 这块板的 `board_early.c`

```c
static int v851s_e907_uart3_early_init(void)
{
    uint32_t v;

    /* 1. PE12/PE13 → UART3（mux 7） */
    v = sys_read32(0x020000C4);
    v = (v & ~0x00770000u) | 0x00770000u;
    sys_write32(v, 0x020000C4);

    /* 2. 开 UART3 时钟、解复位 */
    v = sys_read32(0x0200190C);
    v |= (1u << 19) | (1u << 3);
    sys_write32(v, 0x0200190C);

    return 0;
}

SYS_INIT(v851s_e907_uart3_early_init, PRE_KERNEL_1, 0);
```

### `SYS_INIT` 的三个参数

```c
SYS_INIT(函数, 级别, 子优先级);
```

| 级别 | 何时 | 通常放什么 |
|---|---|---|
| `EARLY` | 最早，内核还没起来 | 极底层、时钟、内存 |
| `PRE_KERNEL_1` | 内核基本结构就绪 | 引脚、时钟门、简单驱动 |
| `PRE_KERNEL_2` | 更多子系统就绪 | 依赖前一级的驱动 |
| `POST_KERNEL` | 内核完全就绪 | 大多数驱动 |
| `APPLICATION` | `main` 之前最后 | 应用级初始化 |

这块板选 `PRE_KERNEL_1, 0`：

- 比 `main` 早。
- 比 ns16550 串口驱动（`CONFIG_SERIAL_INIT_PRIORITY`，
  通常 50+，在 `POST_KERNEL`）早。
- 所以驱动初始化时，时钟已经开、引脚已经 mux。

> 顺序错会怎样：驱动去读写 UART 寄存器，但时钟没开，
> 读回来全是 0 或总线错误。**“驱动没反应”先查初始化顺序。**

### 实验：看初始化顺序

构建时打开：

```conf
CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
```

或者看 `build/zephyr/zephyr.map` 里 `__init_*` 段的顺序。

---

## 14.7 日志与 RAM console

### 为什么这块板不把日志打到 UART3

UART3 是**目标板的串口**，用户要看的是目标板输出，不是
E907 的调试日志。如果 E907 把日志也写 UART3，会污染数据流。

所以配置：

```conf
CONFIG_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_UART_CONSOLE=n
CONFIG_RAM_CONSOLE=y
CONFIG_SERIAL=y
CONFIG_UART_NS16550=y
CONFIG_UART_INTERRUPT_DRIVEN=y
```

- `prj.conf` 关掉 UART console。
- 打开 RAM console。
- `printk` 写进一小块内存。

### Linux 侧怎么读

```sh
cat /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

remoteproc 驱动把 E907 的 RAM console 映射成 `trace0`。
这就是为什么你能看到：

```text
*** YuzukiPI-Lizard V851S E907 co-processor ***
Zephyr 3.x.x | RV32IMAC + xthead @ 600 MHz
...
OpenAMP rpmsg_chrdev ready (addr 0x...)
```

### 日志级别

```c
LOG_MODULE_REGISTER(linkr_bridge, LOG_LEVEL_INF);

LOG_ERR("...");
LOG_WRN("...");
LOG_INF("...");
LOG_DBG("...");
```

运行时可调：

```conf
CONFIG_LOG_DEFAULT_LEVEL=3   /* INF */
```

或者在代码里用 `LOG_MODULE_REGISTER(name, LOG_LEVEL_DBG)`。

> 不要把大流量数据 `LOG_INF` 每一帧。日志本身会拖慢系统，
> 还会把 RAM console 刷爆。

### `printk` vs `LOG_*`

| | `printk` | `LOG_*` |
|---|---|---|
| 需要 `CONFIG_PRINTK` | 是 | 否（但常一起开） |
| 带模块/级别 | 否 | 是 |
| 可运行时过滤 | 否 | 是 |
| 适合 | 启动横幅、panic | 正常运行日志 |

---

## 14.8 实验：写第一个多线程程序

### 目标

写一个程序，两个线程：

1. 生产者：每 500 ms 往消息队列放一个递增数字。
2. 消费者：从队列取，打印。

### 代码

```c
#include <zephyr/kernel.h>

K_MSGQ_DEFINE(q, sizeof(uint32_t), 8, 4);

void producer(void *a, void *b, void *c)
{
    uint32_t n = 0;
    while (1) {
        if (k_msgq_put(&q, &n, K_NO_WAIT) != 0) {
            printk("queue full, drop %u\n", n);
        } else {
            printk("put %u\n", n);
        }
        n++;
        k_msleep(500);
    }
}

void consumer(void *a, void *b, void *c)
{
    uint32_t n;
    while (1) {
        k_msgq_get(&q, &n, K_FOREVER);
        printk("got %u\n", n);
    }
}

K_THREAD_STACK_DEFINE(prod_stack, 1024);
K_THREAD_STACK_DEFINE(cons_stack, 1024);
static struct k_thread prod_data, cons_data;

int main(void)
{
    k_thread_create(&prod_data, prod_stack,
                    K_THREAD_STACK_SIZEOF(prod_stack),
                    producer, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    k_thread_create(&cons_data, cons_stack,
                    K_THREAD_STACK_SIZEOF(cons_stack),
                    consumer, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
    return 0;
}
```

### 观察

- 队列没满时，`put` 和 `got` 交替出现。
- 如果把消费者改成 `k_msleep(2000)`，队列会满，出现 `queue full`。
- 如果把消费者优先级改成比生产者高（数字更小），
  消费者会先跑，能看到调度顺序变化。

### 再实验：去掉同步

把消费者改成“轮询一个全局变量”：

```c
static volatile int flag;
static uint32_t value;

/* producer */
value = n;
flag = 1;

/* consumer */
while (!flag) { }
printk("got %u\n", value);
```

在单核上可能“碰巧对”，但这是错的：

- 编译器可能把 `value` 读取提到 `flag` 之前。
- 换成多核/优化等级就出错。
- 用 `k_sem` 或 `atomic` 才是正确做法。

> 能跑的代码不等于正确的代码。同步原语不是“多余”，
> 是让正确性有保证。

---

## 14.9 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 系统启动就崩 | `SYS_INIT` 顺序/时钟没开 | 看初始化日志、`zephyr.map` |
| 随机崩溃 | 栈溢出 | 开 `INIT_STACKS`、量栈用量 |
| 中断里卡死 | ISR 调了会睡眠的 API | 检查 ISR 代码 |
| 数据偶尔丢 | 临界区没保护 | 加 spinlock/atomic |
| 高优先级线程饿死别人 | 优先级设计错 | 看线程状态、加延时 |
| `k_msgq_put` 总失败 | 队列太小/消费者太慢 | 加大队列、加背压 |
| 日志看不到 | UART console 没关/没开 RAM console | 查 `trace0`、`.config` |
| `printk` 卡住 | 输出太多/中断里打印 | 降频率、别在 ISR 打印 |
| 线程名字是空 | 没调 `k_thread_name_set` | 加一行 |
| 设备 `not ready` | 驱动没初始化/时钟没开 | 检查 `SYS_INIT` 和 DTS |

---

## 14.10 练习

### 练习 14.1（必做）

写一个三线程程序：

- 线程 A 每秒给信号量。
- 线程 B 等信号量，收到后给消息队列放一个数。
- 线程 C 从队列取，打印。

要求：

- 栈各 1024 字节。
- 优先级自选，解释为什么。
- 用 `k_thread_name_set` 命名。

### 练习 14.2（必做）

回答：

1. 为什么 ISR 里不能 `k_sleep`？
2. `k_spinlock` 和 `k_mutex` 各适合什么场景？
3. `atomic_cas` 的返回值是什么意思？
4. `SYS_INIT` 的 `PRE_KERNEL_1` 和 `POST_KERNEL` 差在哪？
5. 为什么这块板的日志不直接打 UART3？

### 练习 14.3（必做）

阅读 `src/linkr_bridge.c`，回答：

1. `uart_rx_sem` 谁给、谁等？
2. `rx_lock` 保护什么？为什么 ISR 路径“自己不需要锁”？
3. `uart_tx_space_sem` 为什么要有？没有会怎样？

### 练习 14.4（选做）

故意把 `uart_rx_stack` 改成 256 字节，构建烧录，观察现象。
**用备用板做，或者确认能恢复。** 记录你看到的错误。

---

## 14.11 本章验收

1. 能说清 Zephyr 和 Linux 的本质差别。
2. 能独立创建线程、设优先级、命名、估栈大小。
3. 知道 ISR 的三条铁律，能判断哪些 API 不能在 ISR 用。
4. 会选 `k_sem`/`k_msgq`/`k_mutex`/`k_spinlock`/`atomic`/`ring_buf`。
5. 能解释 `SYS_INIT` 的级别和这块板为什么用 `PRE_KERNEL_1`。
6. 知道 RAM console 怎么配、Linux 侧怎么读。

### 自测题

1. Zephyr 的优先级数字越大越高还是越低？
2. 协作式和抢占式线程有什么区别？
3. `k_spinlock` 持有期间能睡眠吗？
4. `ring_buf` 满了，`ring_buf_put` 返回什么？
5. `SYS_INIT(func, PRE_KERNEL_1, 0)` 比 `main` 早还是晚？
6. 为什么这块板 `CONFIG_UART_CONSOLE=n`？

> 下一章讲怎么把 Zephyr 移植到一块新板：board、DTS、defconfig。
