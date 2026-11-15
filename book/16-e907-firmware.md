# 第 16 章 写 E907 固件：从 hello 到完整桥接

> 本章目标：
> 1. 知道 E907 固件的四个源文件各负责什么。
> 2. 能按正确顺序把“hello world”扩展成“UART + rpmsg 桥”。
> 3. 理解启动顺序：`SYS_INIT` → `main` → 建线程 → OpenAMP 初始化。
> 4. 能读懂 `main.c`、`board_early.c`、`linkr_bridge.c`、`openamp_slave.c`。
> 5. 知道每个模块失败时，系统表现成什么样。

---

## 16.1 固件的四个文件

```text
src/
  main.c            启动横幅、初始化桥、启动 OpenAMP、心跳
  board_early.c     UART3 引脚/时钟早期初始化（PIO 部分实测不生效）
  linkr_bridge.c    UART3 ↔ 环形缓冲 ↔ rpmsg，两个线程
  openamp_slave.c   OpenAMP 初始化、IPM、端点池、rpmsg 回调
```

依赖关系：

```text
main.c
  ├─ linkr_bridge_init()     启动 UART 线程
  └─ e907_openamp_start()    启动 OpenAMP（内部建线程）

board_early.c  ── SYS_INIT ── 比 main 早，配好 UART3

openamp_slave.c
  └─ bridge_cb() → linkr_bridge_rpmsg_recv()
```

> 结构很清晰：**板级胶水、传输层、协议层、入口**四层分开。
> 不要把所有东西塞进 `main.c`。

---

## 16.2 第一步：hello world

先什么都不要接，只验证“固件能加载、日志能出来”。

### `prj.conf`（最小）

```conf
CONFIG_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_RAM_CONSOLE=y
CONFIG_UART_CONSOLE=n
CONFIG_SERIAL=y
CONFIG_UART_NS16550=y
CONFIG_XIP=n
CONFIG_KERNEL_BIN_NAME="zephyr_e907"
```

### `main.c`（最小）

```c
#include <zephyr/kernel.h>

int main(void)
{
    printk("hello from E907\n");
    while (1) {
        k_msleep(1000);
    }
    return 0;
}
```

### 验证

1. 构建出 `build/zephyr/zephyr_e907.elf`。
2. 复制到板子 `/lib/firmware/`。
3. 启动 remoteproc。
4. 读日志：

```sh
cat /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

看到 `hello from E907` 就说明：**加载、启动、RAM console
全通了。** 这是后面一切的地基。

### 没看到怎么办

按顺序查：

| 步骤 | 检查 |
|---|---|
| ELF 有没有 `.resource_table` | `readelf -S` |
| remoteproc 有没有加载 | `dmesg`、`state` |
| 固件路径/名字对不对 | `/lib/firmware/` |
| RAM console 配了吗 | `.config` 里 `CONFIG_RAM_CONSOLE=y` |
| 入口地址对吗 | remoteproc 日志 |

---

## 16.3 第二步：让 UART3 能收发

### 问题

UART3 的引脚和时钟没人配。直接 `DEVICE_DT_GET` 会失败，或者
寄存器读写全 0。

### 解决：`board_early.c`

```c
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>

#define PIO_CFG_PE12_PE13   0x020000C4u
#define CCU_UART_GATE       0x0200190Cu
#define PE12_PE13_MUX7      0x00770000u
#define UART3_GATE_EN       (1u << 19)
#define UART3_RESET_DEASSERT (1u << 3)

static int uart3_early_init(void)
{
    uint32_t v;

    v = sys_read32(PIO_CFG_PE12_PE13);
    v = (v & ~PE12_PE13_MUX7) | PE12_PE13_MUX7;
    sys_write32(v, PIO_CFG_PE12_PE13);

    v = sys_read32(CCU_UART_GATE);
    v |= (UART3_GATE_EN | UART3_RESET_DEASSERT);
    sys_write32(v, CCU_UART_GATE);

    return 0;
}

SYS_INIT(uart3_early_init, PRE_KERNEL_1, 0);
```

> 实测提醒：这段代码确实会执行，但 **E907 写 PIO 不生效**。
> PE12/PE13 真正被切成 UART3，是 A7 侧 `start_e907` 先
> `insmod uart3pin.ko` 的结果（见 6.4）。如果你换了一个没有这个
> 模块的 rootfs，UART3 不会通，要在 A7 侧补 mux，而不是改这里。

### 验证：回环/打印

临时把 console 切到 UART3（只用于 bring-up）：

```c
/* 临时：直接 poll_out 一个字符 */
#include <zephyr/drivers/uart.h>
static const struct device *u = DEVICE_DT_GET(DT_NODELABEL(uart3));

if (device_is_ready(u)) {
    uart_poll_out(u, 'O');
    uart_poll_out(u, 'K');
    uart_poll_out(u, '\n');
}
```

用 USB-TTL 接 PE12(TX)/PE13(RX)/GND，115200 8N1。

看到 `OK` 说明引脚 mux 和时钟都对了——其中 mux 来自 A7 的
`uart3pin.ko`，不是来自上面的 `board_early.c`。

> 这一步没通，不要往下做。桥接的所有问题都会表现成
> “没数据”，你会分不清是 UART 还是 rpmsg。

---

## 16.4 第三步：环形缓冲 + 线程

### 为什么需要缓冲

UART 是低速字节流，rpmsg 是消息。两者速度不匹配：

- UART 一个字节一个字节来。
- rpmsg 一次发一条消息（最多 256 字节）。
- A7 可能瞬间塞一大段（粘贴），UART 要好几秒才发完。

没有缓冲：

- 要么丢数据。
- 要么阻塞 rpmsg 端点。

### 两个环 + 三个信号量

```c
#define RX_RING_SIZE 65536
#define TX_RING_SIZE 65536
#define RX_CHUNK     256

RING_BUF_DECLARE(uart_rx_ring, RX_RING_SIZE);
RING_BUF_DECLARE(uart_tx_ring, TX_RING_SIZE);

K_SEM_DEFINE(uart_rx_sem, 0, 1);
K_SEM_DEFINE(uart_tx_sem, 0, 1);
K_SEM_DEFINE(uart_tx_space_sem, 0, 1);
```

数据流：

```text
UART3 RX ──ISR──► uart_rx_ring ──uart_rx 线程──► rpmsg_sendto ──► A7
A7 ──rpmsg 回调──► uart_tx_ring ──uart_tx 线程──► uart_poll_out ──► UART3 TX
```

### RX 路径

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
        rx_push(buf, n);
    }
    k_sem_give(&uart_rx_sem);
}
```

`rx_push` 用 spinlock 保护，因为 ISR 和轮询线程都会写：

```c
static struct k_spinlock rx_lock;

static void rx_push(const uint8_t *buf, uint32_t len)
{
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    uint32_t put = ring_buf_put(&uart_rx_ring, buf, len);
    k_spin_unlock(&rx_lock, key);

    atomic_add(&uart_rx_total, put);
    if (put < len) {
        atomic_add(&uart_rx_dropped, len - put);
    }
}
```

### 为什么还要轮询

```c
for (;;) {
    (void)k_sem_take(&uart_rx_sem, K_MSEC(20));
    uart_poll_rx();
    /* 取环里的数据，发 rpmsg */
}
```

注释写得很清楚：

> “Wait for the ISR, but also poll so a missing CLIC IRQ only
> costs latency instead of losing the bridge entirely.”

即：如果 CLIC 中断路由有问题，信号量等不到，但 20 ms 后
轮询仍能把 FIFO 里的数据捞出来。**只损失延迟，不丢功能。**

`uart_poll_rx` 用 `irq_lock()` 保护，因为 ns16550 驱动不保证
线程和 ISR 同时调 `uart_fifo_read` 是安全的。

### TX 路径

```c
static void uart_tx_thread(void *a, void *b, void *c)
{
    uint8_t buf[RX_CHUNK];

    for (;;) {
        uint32_t got;

        k_sem_take(&uart_tx_sem, K_FOREVER);
        while ((got = ring_buf_get(&uart_tx_ring, buf, sizeof(buf))) > 0) {
            for (uint32_t i = 0; i < got; i++) {
                uart_poll_out(uart_dev, buf[i]);
            }
            k_sem_give(&uart_tx_space_sem);
        }
    }
}
```

注意：**发送用轮询 `uart_poll_out`，不用中断。**

为什么？UART3 的 TX 中断在这个 SoC 上没完全验证；轮询发送
简单可靠。代价是 TX 线程会忙一会儿，但 256 字节 115200 下
约 22 ms，可以接受。

> 工程取舍：**RX 用中断 + 轮询兜底，TX 用纯轮询。**
> 先把功能做对，再优化延迟。

---

## 16.5 第四步：rpmsg 回调与握手

### 回调入口

```c
static int bridge_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
                     uint32_t src, void *priv)
{
    return linkr_bridge_rpmsg_recv(ept, data, len, src);
}
```

### 收数据的判断顺序

```c
int linkr_bridge_rpmsg_recv(...)
{
    bridge_ept = ept;
    bridge_dst = src;

    /* 1. 握手 */
    if (handshake_match(p, len)) { ... }

    /* 2. 统计 */
    if (bridge_ready && len >= 9 && !memcmp(p, "LINKRSTAT", 9)) { ... }

    /* 3. 配置 */
    if (bridge_ready && len > 8 && !memcmp(p, "LINKRCFG ", 9)) { ... }

    /* 4. 没握手 → 回显 */
    if (!bridge_ready) { return rpmsg_sendto(ept, data, len, src); }

    /* 5. 正常数据 → TX 环 */
    put = ring_buf_put(&uart_tx_ring, p, len);
    ...
}
```

顺序很重要：控制线必须在数据前匹配，否则会被写进 UART。

### 背压

```c
put = ring_buf_put(&uart_tx_ring, p, len);
if (put < len) {
    uint32_t left = len - put;
    p += put;
    while (left > 0) {
        if (k_sem_take(&uart_tx_space_sem, K_MSEC(100)) != 0) {
            atomic_add(&uart_tx_dropped, left);
            break;
        }
        put = ring_buf_put(&uart_tx_ring, p, left);
        p += put;
        left -= put;
    }
}
k_sem_give(&uart_tx_sem);
```

逻辑：

1. 先尽量放。
2. 放不下就等 TX 线程腾空间，最多 100 ms。
3. 还放不下才丢，并计数。

100 ms 是“防止 UART 卡死把 rpmsg 端点永久挂住”的上限。

> 这就是“有界背压”：既不让上游无脑丢，也不让它无限等。

---

## 16.6 第五步：OpenAMP 初始化

### 顺序

```c
static int openamp_slave_init(void)
{
    /* 1. metal 库 */
    metal_init(&metal_params);

    /* 2. 共享内存窗口 */
    metal_io_init(&shm_io, (void *)SHM_START_ADDR, &shm_physmap,
                  SHM_SIZE, -1, 0, NULL);

    /* 3. resource table */
    rsc_table_get(&rsc_table, &rsc_size);
    rsc_physmap = (metal_phys_addr_t)(uintptr_t)rsc_table;
    metal_io_init(&rsc_io, rsc_table, &rsc_physmap, rsc_size, -1, 0, NULL);

    /* 4. 创建 virtio 设备 */
    vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID,
                                    rsc_table_to_vdev(rsc_table), &rsc_io,
                                    NULL, mailbox_notify, NULL);

    /* 5. 等 Linux DRIVER_OK */
    rproc_virtio_wait_remote_ready(vdev);

    /* 6. 初始化两个 vring */
    vring_rsc = rsc_table_get_vring0(rsc_table);
    rproc_virtio_init_vring(vdev, 0, vring_rsc->notifyid,
                            (void *)(uintptr_t)vring_rsc->da, &shm_io,
                            vring_rsc->num, vring_rsc->align);
    vring_rsc = rsc_table_get_vring1(rsc_table);
    rproc_virtio_init_vring(vdev, 1, vring_rsc->notifyid,
                            (void *)(uintptr_t)vring_rsc->da, &shm_io,
                            vring_rsc->num, vring_rsc->align);

    /* 7. rpmsg */
    rpmsg_init_vdev(&rvdev, vdev, ns_bind_cb, &shm_io, NULL);

    /* 8. 最后才开中断 */
    ipm_register_callback(ipm_dev, ipm_rx_cb, NULL);
    ipm_set_enabled(ipm_dev, 1);

    /* 9. 宣告端点 */
    rpmsg_create_ept(&ns_ept, &rvdev.rdev, "rpmsg_chrdev",
                     RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, bridge_cb, NULL);

    return 0;
}
```

### 为什么要等 `DRIVER_OK`

Linux 加载固件后，会：

1. 解析 resource table。
2. 填入 vring 地址。
3. 设置 `status = DRIVER_OK`。
4. 才真正启动 E907（写入口地址）。

E907 启动后如果不等，可能读到 vring 地址还是 0 的旧表。

> 这是“双方签字才能交易”的契约。第 12 章详细讲过。

### 为什么中断最后开

如果先开 IPM 中断，A7 可能在 vring 还没初始化时按门铃，
`rproc_virtio_notified()` 访问未初始化结构 → 崩溃或丢消息。

---

## 16.7 第六步：IPM 中断与消息队列

### ISR

```c
static void ipm_rx_cb(const struct device *dev, void *ctx, uint32_t id,
                      volatile void *data)
{
    uint32_t notifyid = VRING1_ID;

    if (data != NULL) {
        notifyid = *(volatile uint32_t *)data;
    }

    if (k_msgq_put(&ipm_msgq, &notifyid, K_NO_WAIT) != 0) {
        atomic_set(&ipm_overflow, 1);
    }
}
```

只有两行逻辑：读 notify id、进队列。队列满就标记溢出。

### 线程

```c
static void ipm_rx_thread(void *a, void *b, void *c)
{
    uint32_t notifyid;

    for (;;) {
        k_msgq_get(&ipm_msgq, &notifyid, K_FOREVER);

        do {
            (void)rproc_virtio_notified(rvdev.vdev, notifyid);
        } while (k_msgq_get(&ipm_msgq, &notifyid, K_NO_WAIT) == 0);

        if (atomic_cas(&ipm_overflow, 1, 0)) {
            (void)rproc_virtio_notified(rvdev.vdev, VRING0_ID);
            (void)rproc_virtio_notified(rvdev.vdev, VRING1_ID);
        }
    }
}
```

逻辑：

1. 阻塞等一个 notify id。
2. 处理它。
3. 把队列里剩下的都处理掉（批量）。
4. 如果溢出过，重扫两个 vring。

> 第 4 步是“丢通知的补偿”：队列满时丢了一条 kick，
> 但数据还在 vring 里，重扫就能捞回来。

### 发送方 notify

```c
static int mailbox_notify(void *priv, uint32_t id)
{
    static uint32_t notify_id;

    notify_id = id;
    return ipm_send(ipm_dev, 0, id, &notify_id, sizeof(notify_id));
}
```

用 `static` 而不是栈变量：注释说得很清楚——

> “a driver that defers the transfer must not get a pointer into
> this stack frame.”

即：如果 mailbox 驱动异步发送，栈变量早就没了。用 static 保底。

---

## 16.8 完整启动顺序

把上面拼起来：

```text
上电
 │
 ├─ boot0 / U-Boot / Linux 启动（A7）
 │
 ├─ Linux remoteproc 加载 zephyr_e907.elf
 │   ├─ 读 .resource_table
 │   ├─ 填 vring 地址
 │   └─ 写入口地址 → E907 开始执行
 │
 ├─ E907: SYS_INIT(PRE_KERNEL_1, 0)  board_early
 │   └─ 配 PE12/PE13、开 UART3 时钟
 │
 ├─ E907: 驱动初始化（ns16550 等）
 │
 ├─ E907: main()
 │   ├─ printk 横幅
 │   ├─ linkr_bridge_init()
 │   │   ├─ 检查 UART3 ready
 │   │   ├─ 清 FIFO
 │   │   ├─ 建 uart_rx 线程（优先级 7）
 │   │   ├─ 建 uart_tx 线程（优先级 8）
 │   │   └─ 开 RX 中断
 │   └─ e907_openamp_start()
 │       └─ 建 openamp 线程（优先级 6）
 │           ├─ openamp_slave_init()
 │           │   ├─ 等 DRIVER_OK
 │           │   ├─ init vring0/1
 │           │   ├─ rpmsg_init_vdev
 │           │   ├─ 开 IPM 中断
 │           │   └─ 宣告 rpmsg_chrdev
 │           └─ 建 openamp_rx 线程（优先级 6）
 │
 ├─ A7: 看到 NS 公告 → 创建 /dev/rpmsg0
 │
 ├─ A7 daemon: 打开 /dev/rpmsg0，发 "LINKR1\n"
 │
 ├─ E907: 握手成功，bridge_ready = 1
 │
 └─ 数据双向流动
```

### 关键时间点

| 事件 | 谁 | 标志 |
|---|---|---|
| UART3 可用 | E907 | `device_is_ready` |
| rpmsg 端点宣告 | E907 | `OpenAMP rpmsg_chrdev ready` |
| `/dev/rpmsg0` 出现 | A7 | 内核 rpmsg_char |
| 桥接就绪 | 双方 | `LINKR1 OK` |

> 调试时按这个表从上往下查。哪一步没到，问题就在那一步。

---

## 16.9 实验：逐层验证

### 实验 A：只验证加载

用最小 hello 固件，确认 `trace0` 有输出。

### 实验 B：验证 UART3

在 `main` 里直接 `uart_poll_out` 几个字节，用 USB-TTL 看。
**不要同时开 console 到 UART3 和桥接。**

### 实验 C：验证 rpmsg 回显

不启动 daemon，用独立 `rpmsg_test` 工具发数据。握手前
E907 是回显模式，应该原样返回。

```text
host/board: rpmsg_test /dev/rpmsg0 "hello"
E907: 回 "hello"
```

### 实验 D：验证握手

发 `LINKR1\n`，应收到：

```text
LINKR1 OK rx=0
```

### 实验 E：验证数据路径

握手后，从 A7 发 `abc`，目标板串口应收到 `abc`；
目标板发 `xyz`，A7 应收到 `xyz`。

### 实验 F：验证控制线

```text
LINKRSTAT
→ LINKRSTAT rx=0/65536 tx=0/65536 drop=0/0

LINKRCFG ?
→ LINKRCFG OK uart=115200,8,n,1,n
```

---

## 16.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 固件加载但无日志 | RAM console 没配/固件名错 | `.config`、`trace0` |
| UART3 无输出 | 引脚/时钟没配 | `board_early.c`、示波器 |
| UART3 有乱码 | 波特率/时钟频率不对 | `clock-frequency`、实测 |
| rpmsg 端点不出现 | NS 名字不匹配 | `trace0` 的 NS bind |
| 握手没回复 | `bridge_ready` 逻辑/端点错 | daemon 日志 |
| 数据单向 | `bridge_dst` 没设 | 握手是否成功 |
| 大流量丢数据 | 环太小/背压失败 | `LINKRSTAT`、drop 计数 |
| 系统崩溃 | 栈溢出/vring 未初始化就开中断 | 栈用量、初始化顺序 |
| 偶发丢通知 | IPM 队列满 | `ipm_overflow` 日志 |
| UART TX 卡住 | 对端没读/流控 | 降速、查接线 |

---

## 16.11 练习

### 练习 16.1（必做）

按 16.9 的 A～F 顺序，在你自己的板上逐层验证，记录每步的
命令和输出。**跳步会让你在出问题时无法定位。**

### 练习 16.2（必做）

回答：

1. 为什么 TX 用轮询而 RX 用中断？
2. `uart_rx_sem` 等不到时，20 ms 轮询起什么作用？
3. 为什么 `mailbox_notify` 用 `static uint32_t`？
4. `ipm_overflow` 为什么能“丢通知但不丢数据”？
5. 握手前为什么保留回显模式？

### 练习 16.3（必做）

画出 `main.c` 启动后创建的四个线程，标出优先级和职责：

```text
main / openamp / openamp_rx / uart_rx / uart_tx
```

解释为什么 `openamp_rx` 优先级比 `uart_rx` 高。

### 练习 16.4（选做）

把 `RX_RING_SIZE` 改成 4096，构建并测大流量粘贴，观察
`LINKRSTAT` 的 drop 计数变化。然后改回 65536。

---

## 16.12 本章验收

1. 能说出四个源文件各自的职责和依赖关系。
2. 能按“hello → UART → 环 → rpmsg → 握手”的顺序搭出固件。
3. 能解释完整启动顺序，并据此定位问题。
4. 能读懂 `board_early.c` 的寄存器操作。
5. 能解释背压、溢出补偿、回显兼容三个设计。
6. 能独立完成六层验证。

### 自测题

1. 为什么 `board_early.c` 用 `SYS_INIT` 而不是在 `main` 里配？
2. RX 环和 TX 环各由谁写、谁读？
3. `bridge_ready` 什么时候变 1？
4. `rproc_virtio_wait_remote_ready` 等的是什么？
5. IPM 队列满了，数据会丢吗？为什么？
6. 为什么先建 `openamp` 线程再建 `openamp_rx` 线程？

> 下一章讲调试：当这些都没按预期工作时，怎么一层层查。
