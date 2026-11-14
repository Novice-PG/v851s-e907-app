# 第 11 章 AMP、SMP、共享内存与 mailbox

> 本章目标：
> 1. 分清 SMP 和 AMP，知道这块板为什么是 AMP。
> 2. 理解两个 CPU 通信的物理基础：共享内存 + 门铃。
> 3. 知道内存屏障为什么不能省。
> 4. 能看懂 `/dev/rpmsg*` 和 E907 日志的关系。
> 5. 能判断“通信失败”是内存问题、通知问题还是协议问题。

---

## 11.1 SMP 和 AMP 的区别

### SMP：对称多处理

```text
      ┌─────────────┐
      │  一个 Linux  │
      │  内核镜像    │
      └──┬───────┬──┘
         │       │
      ┌──▼──┐ ┌──▼──┐
      │ CPU0│ │ CPU1│   同样的架构、同样的内存视图
      └─────┘ └─────┘   同一个调度器
```

特点：

- 所有 CPU 跑同一个操作系统。
- 共享全部内存，硬件保证缓存一致（cache coherent）。
- 任务可以在任意 CPU 上迁移。

### AMP：非对称多处理

```text
      ┌─────────────┐        ┌─────────────┐
      │  Linux      │        │  Zephyr     │
      │  (A7)       │        │  (E907)     │
      └──┬──────────┘        └──────────┬──┘
         │                              │
         └──────── 共享内存 ────────────┘
                  + mailbox 门铃
```

特点：

- 两个 CPU 跑**不同的操作系统**。
- 各自有各自的代码、栈、驱动。
- 只有一块约定好的内存共享。
- 没有硬件缓存一致性保证（或需要额外维护）。

> 这块板是 AMP：A7 跑 Linux，E907 跑 Zephyr。
> 它们不是“一个系统的两个核”，而是“两个系统住在一块芯片里”。

### 为什么选 AMP

| 需求 | SMP 方案 | AMP 方案 |
|---|---|---|
| 实时串口 | Linux 上很难保证微秒级 | RTOS 专职处理 |
| 网络/网页 | Linux 生态成熟 | RTOS 做起来很痛苦 |
| 隔离 | 一个内核崩全崩 | 一个核崩，另一个还能救 |
| 启动 | 一个系统 | 需要加载从核固件 |

这块板的选择：

- A7：网络、文件、蓝牙、Python、网页。
- E907：只盯 UART3，保证不丢字节。

代价：要自己设计通信协议、共享内存布局、启动顺序。

---

## 11.2 共享内存：两个人共用的桌子

### 物理事实

两个 CPU 访问的是同一片 DDR。地址 `0x43000000` 对 A7 和
E907 是同一个物理位置（这块 SoC 没有 IOMMU，DA == PA）。

```text
A7 视角                       E907 视角
0x43000000 ─┐                ┌─ 0x43000000
            ├── 同一块 DRAM ──┤
0x43040000 ─┘                └─ 0x43040000
```

### 但不能随便共享

如果两个 CPU 同时写同一块内存：

```text
A7: 读 x=5
E907: 读 x=5
A7: 写 x=6
E907: 写 x=6      ← 谁赢？不确定
```

所以共享内存必须：

1. **划分清楚**：哪段归谁写、哪段只读。
2. **有协议**：谁先写、谁后读、怎么通知。
3. **有同步**：原子操作、屏障、门铃。

### 这块板的共享区

| 区域 | 地址 | 谁写 | 用途 |
|---|---|---|---|
| rpmsg buffer | `0x43000000` | 双方按协议 | 每条消息的数据 |
| vring0 | `0x43040000` | 双方按 virtio 规则 | 描述符环 A→B/D→H |
| vring1 | `0x43060000` | 双方按 virtio 规则 | 反方向 |
| E907 DRAM | `0x43c00000` | E907 为主 | 代码/数据/栈 |

这些区域在 Linux 设备树里是 `reserved-memory` + `no-map`，
Linux 不会拿它们当普通内存分配。

> 如果你在 Linux 里 `malloc` 到 `0x43000000` 附近，说明
> `reserved-memory` 没生效，迟早踩坏 rpmsg。

---

## 11.3 mailbox：只按门铃，不搬东西

### 为什么需要门铃

共享内存只是“桌子”。A7 把数据放桌上后，E907 怎么知道
“有新东西”？

- 轮询：E907 不停地看桌子。浪费 CPU，延迟不可控。
- 中断：A7 按一下门铃，E907 中断处理。

mailbox 就是门铃。

### 门铃传什么

通常只传一个很小的编号（notify id），不传数据本身：

```text
A7: 把消息写进 vring
A7: mailbox 发送 notify id = 0
        │
        ▼
E907: 收到中断，读 notify id
E907: 去 vring 里找描述符
E907: 按描述符找到共享内存里的数据
```

### 为什么不能只靠门铃

如果门铃带数据：

- 一次只能传几个字节。
- 没有缓冲，容易丢。
- 大块数据要拆很多次。

所以：**数据走共享内存，门铃只负责通知。**

### 中断上下文要短

E907 的 mailbox 中断回调：

```c
static void ipm_rx_cb(const struct device *dev, void *ctx, uint32_t id,
                      volatile void *data)
{
    uint32_t notifyid = VRING1_ID;
    ...
    if (k_msgq_put(&ipm_msgq, &notifyid, K_NO_WAIT) != 0) {
        atomic_set(&ipm_overflow, 1);
    }
}
```

它只做一件事：把 notify id 放进消息队列，然后返回。
真正的 rpmsg 处理在 `ipm_rx_thread` 里做。

为什么？

- 中断里不能睡眠。
- rpmsg 处理可能比较久，会阻塞其他中断。
- 消息队列满时不能丢通知，所以用 `ipm_overflow` 标记，
  让线程稍后重扫两个 vring。

> 这是嵌入式通信的通用模式：**ISR 只搬运通知，线程做重活。**

---

## 11.4 内存屏障：顺序不能乱

### 一个危险的顺序

```c
/* 错误 */
mailbox_send(notify);   /* 先按门铃 */
shared->value = 42;     /* 再写数据 */
```

E907 收到门铃，去读 `shared->value`，可能还是旧值。

正确顺序：

```c
shared->value = 42;     /* 1. 写数据 */
memory_barrier();       /* 2. 确保写完成且对其他核可见 */
mailbox_send(notify);   /* 3. 再按门铃 */
```

### 编译器和 CPU 都会重排

- 编译器为了优化，可能调整指令顺序。
- CPU 为了性能，可能乱序执行、延迟写回。
- 缓存可能让另一个核看到旧值。

内存屏障（barrier）告诉它们：**到这里必须停一下，
之前的写对其他人可见，之后的读不能提前。**

### 谁负责这件事

好消息：**OpenAMP/virtio 库已经帮你做了。**
你调用 `rpmsg_sendto()`，它内部会：

1. 把数据放进 buffer。
2. 更新 vring 的 available 环。
3. 执行必要的屏障。
4. 调用 notify（mailbox）。

所以业务代码不要自己直接操作 vring。**绕过框架 = 自己负责
所有屏障和缓存维护 = 极易出错。**

---

## 11.5 缓存一致性：为什么这块板“看起来没问题”

严格来说，AMP 系统需要处理 cache：

- A7 写的数据可能在 A7 的 cache 里，E907 看不到。
- E907 写的数据可能在 E907 的 cache 里，A7 看不到。

这块板的处理：

1. 共享区在 Linux 侧是 `no-map`，不作为普通缓存内存使用。
2. OpenAMP 在关键位置做 cache 维护/屏障。
3. 数据流是“写 buffer → 更新环 → 通知”，有明确顺序。

> 这并不意味着你可以随便共享。**任何自己新加的共享内存，
> 都要明确：谁写、谁读、什么时候刷 cache、用什么屏障。**
> 不确定就通过 rpmsg 发消息，不要直接共享裸内存。

---

## 11.6 一次完整的“按门铃”旅程

A7 发 3 个字节 `abc` 给 E907：

```text
1. daemon: os.write(/dev/rpmsg0, "abc")
2. Linux rpmsg_char:
   ├─ 分配一个 512 B buffer
   ├─ 填 rpmsg 头（src/dst/len/flags）
   ├─ 拷贝 "abc"
   ├─ 把 buffer 描述符放进 vring0 的 available 环
   ├─ 内存屏障
   └─ 写 mailbox 寄存器（notify id）
3. E907 mailbox 中断:
   ├─ 读 notify id
   ├─ 放进 ipm_msgq
   └─ 返回
4. E907 ipm_rx_thread:
   ├─ rproc_virtio_notified(vdev, id)
   ├─ 从 vring 取描述符
   ├─ 找到 buffer，读出 "abc"
   ├─ 调用 bridge_cb()
   └─ 把 buffer 还给 vring（used 环）
5. bridge_cb → linkr_bridge_rpmsg_recv():
   ├─ 不是握手/控制命令
   └─ ring_buf_put(uart_tx_ring, "abc")
6. uart_tx 线程:
   └─ uart_poll_out('a'/'b'/'c')
```

反方向类似，只是 vring1 和另一个 notify id。

### 为什么有“还 buffer”这一步

buffer 数量有限（这块板每个 vring 8 个）。如果 E907 只取不还，
很快就没有 buffer 可用，A7 会阻塞或丢消息。

virtio 用 used 环记录“哪些 buffer 用完了，可以复用”。

> 这就像图书馆的座位：坐下（取 buffer），离开时要登记
> （还 buffer），否则别人没位置。

---

## 11.7 实验：观察通信是否活着

### 实验 A：看设备节点

```sh
ls -l /dev/rpmsg*
```

预期：

```text
/dev/rpmsg_ctrl0
/dev/rpmsg0
```

- `rpmsg_ctrl0` 是控制节点，用来创建/销毁端点。
- `rpmsg0` 是实际的数据端点。

### 实验 B：看 E907 日志

```sh
tail -20 /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

预期能看到：

```text
OpenAMP rpmsg_chrdev ready (addr 0x...)
rpmsg NS bind: 'rpmsg_chrdev' dest=...
```

### 实验 C：看内核日志

```sh
dmesg | grep -i -E 'rpmsg|remoteproc|mailbox|msgbox'
```

重点：

- `remoteproc0: remote processor ... is now up`
- `rpmsg ... created`
- 有没有 `timeout`、`failed`、`not ready`

### 实验 D：看统计

在网页终端输入：

```text
@i?
```

关注：

```text
@info uart rx=... tx=... drop=0/0 buffer=0/65536
```

如果 `drop` 一直涨，说明某一侧处理不过来。

---

## 11.8 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 没有 `/dev/rpmsg0` | E907 没起、NS 没绑定 | `state`、`trace0`、`dmesg` |
| `/dev/rpmsg0` 打不开 | 端点被销毁/权限 | `ls -l`、重新握手 |
| 写返回 `EMSGSIZE` | 超过 MTU（256 限制） | 切片 |
| 写成功但对方没收到 | 门铃没发、vring 没更新 | 看 `trace0` 有没有 `notified` |
| 偶尔丢消息 | 通知队列满/没重扫 | `ipm_overflow`、加大队列 |
| 数据错乱 | 共享内存布局错/越界 | 对照地址表 |
| 系统随机崩 | reserved-memory 没生效 | `/proc/iomem` |
| 延迟很大 | 轮询/优先级/中断没开 | 看线程优先级、IRQ 统计 |

---

## 11.9 练习

### 练习 11.1（必做）

画出 A7 发 1 个字节到 E907 的完整路径，标出：

- 用户态
- 内核 rpmsg
- 共享内存
- mailbox
- E907 中断
- E907 线程
- UART

### 练习 11.2（必做）

回答：

1. 为什么 mailbox 不直接传数据？
2. 为什么 ISR 只把 notify id 放进队列？
3. 如果去掉内存屏障，最坏会发生什么？
4. 为什么 buffer 要“还”？

### 练习 11.3（选做）

在板子上连续发送大量数据，同时观察：

```sh
cat /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

看有没有丢通知/重扫的日志，解释原因。

---

## 11.10 本章验收

1. 能说清 SMP 和 AMP 的区别，以及这块板为什么选 AMP。
2. 能画出共享内存 + mailbox 的通信模型。
3. 能解释“数据走共享内存、门铃只传通知”。
4. 能说出内存屏障和缓存一致性为什么重要。
5. 能用 `state`、`trace0`、`dmesg`、`@i?` 判断通信是否正常。

### 自测题

1. AMP 系统和 SMP 系统最大的区别是什么？
2. 共享内存为什么不能随便分配？
3. mailbox 一次通常传多少数据？
4. 为什么 ISR 里不能直接处理 rpmsg？
5. 如果 A7 写成功但 E907 没收到，先查哪一层？

> 下一章拆开 vring 和 resource table：通信的“合同”到底写了什么。
