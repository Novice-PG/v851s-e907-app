# 第 12 章 virtio、vring 与 resource table

> 本章目标：
> 1. 知道 virtio 的“设备/驱动”角色怎么分。
> 2. 看懂 vring 的三个数组：desc、avail、used。
> 3. 理解 resource table 里写了什么，Linux 为什么要改它。
> 4. 能算一条消息从 buffer 到队列的完整生命周期。
> 5. 能判断“卡在 DRIVER_OK”“vring 地址不对”这类问题。

---

## 12.1 virtio 是什么

virtio 是一套“虚拟设备”协议：**一端是驱动，一端是设备**。

```text
┌──────────────┐        共享内存 + 通知        ┌──────────────┐
│  virtio 驱动 │ ◄──────────────────────────► │  virtio 设备 │
│   (Linux A7) │                              │  (E907)      │
└──────────────┘                              └──────────────┘
```

在这块板上：

| 角色 | 谁 | 做什么 |
|---|---|---|
| driver | Linux A7 | 分配 buffer、填队列、按门铃 |
| device | E907 | 初始化 vring、收通知、取数据、还 buffer |

> 注意：这里的“设备”不是硬件，是 E907 上的软件。
> E907 假装自己是一个 virtio 网卡/串口类的设备。

### 为什么用 virtio，而不是自己定协议

自己定协议当然可以，但 virtio 已经解决了：

- 描述符环（怎么告诉对方数据在哪）。
- 缓冲区回收（怎么知道对方用完了）。
- 通知机制（什么时候按门铃）。
- 特性协商（双方支持什么）。

OpenAMP 在 virtio 之上再做 rpmsg（消息传递），
我们直接站在成熟协议上。

---

## 12.2 vring：三个数组

一个 vring（virtqueue）由三部分组成：

```text
┌─────────────────────────────────────────────┐
│ desc[]   描述符表：数据在哪、多长、下一个是谁 │
├─────────────────────────────────────────────┤
│ avail[]  驱动 → 设备：我放了哪些描述符        │
├─────────────────────────────────────────────┤
│ used[]   设备 → 驱动：我用完了哪些描述符      │
└─────────────────────────────────────────────┘
```

### 描述符 desc

```c
struct vring_desc {
    uint64_t addr;    /* 数据在共享内存里的物理地址 */
    uint32_t len;     /* 多长 */
    uint16_t flags;   /* NEXT / WRITE / INDIRECT */
    uint16_t next;    /* 下一个描述符的索引（链式） */
};
```

一条消息可以用一个描述符，也可以用链：

```text
desc[2] ──► buffer A（头）
   │ next
   ▼
desc[5] ──► buffer B（数据）
```

这块板的 rpmsg 消息通常一个描述符就够（512 B buffer）。

### avail 环

```c
struct vring_avail {
    uint16_t flags;
    uint16_t idx;      /* 驱动写：我放到第几个 */
    uint16_t ring[];   /* 描述符索引数组 */
};
```

驱动（A7）把“可用的描述符索引”写进 `ring[]`，然后 `idx++`。

### used 环

```c
struct vring_used_elem {
    uint32_t id;       /* 描述符索引 */
    uint32_t len;      /* 实际用了多少字节 */
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;      /* 设备写：我用完到第几个 */
    struct vring_used_elem ring[];
};
```

设备（E907）处理完，把索引和长度写进 `used.ring[]`，
然后 `idx++`。驱动看到 `used.idx` 变化，就知道 buffer 可以回收。

### 生活比喻

```text
desc[]  = 盘子（每个盘子写着“菜在哪”）
avail[] = 厨房窗口的取餐号（驱动：这些菜好了）
used[]  = 回收箱（设备：这些盘子我用完了）
```

- 驱动把菜放盘子，报号。
- 设备按号取菜，吃完把盘子放回收箱。
- 盘子数量固定，不回收就没盘子用。

---

## 12.3 一次消息的完整生命周期

以 A7 发 3 字节 `abc` 为例：

```text
初始：desc[0..7] 空闲，avail.idx=0，used.idx=0

1. A7 找空闲 desc，比如 desc[0]
2. A7 把 "abc" 写进 buffer（0x43000000 区域）
3. desc[0].addr = buffer 地址
   desc[0].len  = 3
4. avail.ring[0] = 0
   avail.idx = 1
5. 内存屏障
6. A7 按门铃（mailbox notify）

7. E907 中断 → 线程
8. E907 读 avail.idx=1，知道有 1 个新描述符
9. 取 avail.ring[0]=0 → desc[0]
10. 按 desc[0].addr/len 读 "abc"
11. 处理（放进 uart_tx_ring）
12. used.ring[0].id = 0
    used.ring[0].len = 3
    used.idx = 1
13. E907 按门铃（如果驱动需要回收通知）

14. A7 看到 used.idx=1，回收 buffer 0
```

### 为什么需要“回收”

buffer 只有 8 个。如果 A7 一直发、E907 不还，第 9 条消息就
没 buffer 可用。`used` 环就是“我还给你了”的凭证。

### 通知不是必须每步都发

- 驱动放数据后通常要通知设备。
- 设备还 buffer 后，驱动可能不需要立刻知道（可以批量回收）。
- 具体看 flags（`VIRTQ_AVAIL_F_NO_INTERRUPT` 等）。

> 这块板的实现由 OpenAMP 处理，你不需要手写这些。
> 但看懂生命周期，才能在“消息卡住”时知道查哪。

---

## 12.4 resource table：给 Linux 的“合同”

### 它在哪

E907 固件 ELF 里有一个 `.resource_table` 段。Linux 加载固件时：

1. 读 ELF。
2. 找到 `.resource_table`。
3. 解析里面的 vdev、vring 信息。
4. 把 vring 地址填进去。
5. 写回内存。
6. 启动 E907。

### 里面有什么

概念结构：

```c
struct resource_table {
    uint32_t ver;
    uint32_t num;
    uint32_t reserved[2];
    uint32_t offset[];       /* 每个资源项的偏移 */
};

/* 资源项 1：一个 virtio 设备 */
struct fw_rsc_vdev {
    uint32_t type;
    uint32_t id;
    uint32_t notifyid;
    uint32_t dfeatures;
    uint32_t gfeatures;
    uint32_t config_len;
    uint8_t status;
    uint8_t num_of_vrings;
    uint8_t reserved[2];
    struct fw_rsc_vdev_vring vring[2];
};

struct fw_rsc_vdev_vring {
    uint32_t da;        /* device address，Linux 填 */
    uint32_t align;
    uint32_t num;       /* 描述符数量 */
    uint32_t notifyid;
    uint32_t reserved;
};
```

### 关键字段

| 字段 | 含义 |
|---|---|
| `num_of_vrings` | 这块板是 2（双向） |
| `vring[0].num` | 描述符数量（8 的倍数，常见 256） |
| `vring[0].align` | 对齐（这块板 16） |
| `vring[0].da` | vring 的物理地址，Linux 填 |
| `status` | 设备状态位，`DRIVER_OK` 是关键 |

### 为什么 Linux 要改它

E907 固件编译时不知道 Linux 会把 vring 放在哪。
所以 resource table 里先留空，Linux 根据设备树的
`vdev0vring0`/`vdev0vring1` carveout 填入地址。

```dts
vdev0vring0: vdev0vring0@43040000 {
    reg = <0x43040000 0x00008000>;
    no-map;
};
```

Linux 把这 128 KiB 的起始地址写进 `vring[0].da`。

### `DRIVER_OK`

virtio 的状态机：

```text
0 → ACKNOWLEDGE → DRIVER → DRIVER_OK → 运行
```

E907 在初始化 vring 前必须等 `DRIVER_OK`：

```c
rproc_virtio_wait_remote_ready(vdev);
```

为什么？

- 如果 Linux 还没填好 vring 地址，E907 就去读，会读到 0 或垃圾。
- 如果 Linux 还没分配 buffer，E907 取描述符会越界。

> 这就是“合同”的比喻：双方都签字（DRIVER_OK）后，
> 才能开始交易。

---

## 12.5 E907 侧的初始化顺序

`openamp_slave.c` 的顺序：

```c
1. metal_init()
2. metal_io_init(shm_io, 0x43000000, 256 KiB)
3. rsc_table_get() / metal_io_init(rsc_io)
4. rproc_virtio_create_vdev(DEVICE, ...)
5. rproc_virtio_wait_remote_ready(vdev)     ← 等 DRIVER_OK
6. rproc_virtio_init_vring(vdev, 0, ...)
7. rproc_virtio_init_vring(vdev, 1, ...)
8. rpmsg_init_vdev(&rvdev, vdev, ns_bind_cb, &shm_io, NULL)
9. ipm_register_callback() / ipm_set_enabled(1)
10. rpmsg_create_ept("rpmsg_chrdev", ...)
```

顺序不能乱：

- 先有 vdev，才能等 DRIVER_OK。
- 先 init vring，才能 init rpmsg。
- 先 init rpmsg，才能创建端点。
- 最后才开中断，避免“中断来了但队列还没准备好”。

### 为什么要开中断在最后

如果先开中断，A7 可能立刻按门铃，而 E907 的 vring 还没初始化，
`rproc_virtio_notified()` 会访问未初始化的结构，轻则丢消息，
重则崩溃。

---

## 12.6 buffer 大小与 MTU

### 512 字节的来历

Linux rpmsg 每个 buffer 默认 512 字节：

```text
┌────────────────┬──────────────────────────┐
│ rpmsg 头 16 B  │ payload 最多 496 B       │
└────────────────┴──────────────────────────┘
```

但实际可用 payload 还受端点 MTU 限制。

### 为什么 daemon 只发 256

`RpmsgBridge.send()` 的注释：

```text
rpmsg_char rejects writes larger than the endpoint MTU
(EMSGSIZE / errno 90). The Linux rpmsg buffer is 512 bytes,
minus the 16-byte rpmsg header, so 256 leaves plenty of headroom.
```

- 512 − 16 = 496。
- 取 256，留一半余量。
- 256 也是 E907 `RX_CHUNK` 的大小，两边对齐。

### 写大了会怎样

```text
OSError: [Errno 90] Message too long
```

在 C 里就是 `write()` 返回 -1，`errno == EMSGSIZE`。

> 不要“试出来”再改。协议设计时就把 MTU 定好，
> 上层按 MTU 切片。

---

## 12.7 实验：看 vring 和 resource table

### 实验 A：看固件段

```sh
# 主机上
readelf -S firmware/zephyr_e907.elf | grep -i resource
readelf -x .resource_table firmware/zephyr_e907.elf | head -20
```

你应该看到 `.resource_table` 段存在且非空。**没有它，
Linux 会拒绝启动 E907。**

### 实验 B：看 vring 地址

```sh
# 板子上
cat /proc/iomem | grep -i -E 'vdev|rpmsg|43c0|4300'
```

### 实验 C：看 E907 日志

```sh
tail -20 /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

预期能看到 `OpenAMP rpmsg_chrdev ready` 和 vring 初始化成功。

### 实验 D：故意写大

在网页终端或脚本里，尝试一次发 1024 字节。观察：

- 是否出现 `EMSGSIZE`。
- daemon 日志有没有报错。
- 数据有没有丢。

> 正常实现会切片，不会让你看到错误。这个实验是让你
> 理解“为什么必须切片”。

---

## 12.8 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| E907 卡在启动 | 等不到 `DRIVER_OK` | Linux remoteproc 日志、DTS |
| `failed to init vring` | 地址/大小/对齐错 | resource table、DTS carveout |
| `rpmsg_init_vdev failed` | vring 没准备好 | 上一步日志 |
| 没有 `/dev/rpmsg0` | NS 没绑定 | `trace0` 的 NS bind |
| 写大包失败 | 超过 MTU | 切片 |
| 消息只发出一条 | buffer 没回收 | used 环、驱动日志 |
| 系统随机崩 | vring 地址和 Linux 不一致 | 对照 DTS 和固件 |
| 重复消息 | 通知重发/没清状态 | 看 notify id、used.idx |

---

## 12.9 练习

### 练习 12.1（必做）

画出 vring 的 desc/avail/used 三个数组，并标出：

- 驱动写哪些。
- 设备写哪些。
- 哪些是“索引”，哪些是“数据”。

### 练习 12.2（必做）

回答：

1. 为什么 E907 要等 `DRIVER_OK`？
2. 为什么 buffer 要回收？
3. `vring[0].da` 是谁填的？依据是什么？
4. 为什么 daemon 一次只写 256 字节？

### 练习 12.3（选做）

用 `readelf -x .resource_table` 找到 vdev 的 `num_of_vrings`
和 `notifyid`，和代码里的 `VRING0_ID`/`VRING1_ID` 对照。

---

## 12.10 本章验收

1. 能解释 virtio 的 driver/device 角色。
2. 能画出 vring 三个数组，并说明消息生命周期。
3. 能说出 resource table 的关键字段和 Linux 为什么改它。
4. 知道 E907 初始化顺序，以及为什么中断最后开。
5. 能解释 512/496/256 这三个数字的关系。

### 自测题

1. 这块板上谁是 virtio 驱动，谁是设备？
2. `avail.idx` 和 `used.idx` 分别由谁更新？
3. 为什么 `DRIVER_OK` 之前不能碰 vring？
4. 一个 rpmsg buffer 有多大？payload 最多多少？
5. 如果 buffer 不回收，第 9 条消息会怎样？

> 下一章讲 rpmsg 协议本身：端点、命名服务、握手和控制命令。
