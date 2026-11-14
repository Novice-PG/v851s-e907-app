# 第 13 章 rpmsg 协议：端点、命名服务与握手

> 本章目标：
> 1. 知道 rpmsg 消息头长什么样、地址怎么分配。
> 2. 理解“命名服务（NS）”和 `/dev/rpmsgN` 的创建过程。
> 3. 看懂 `LINKR1`、`LINKRCFG`、`LINKRSTAT` 三条控制线。
> 4. 能写一个最小 rpmsg 客户端。
> 5. 能排查“端点没出现、握手没回复、控制命令被当数据”的问题。

---

## 13.1 rpmsg 在 virtio 之上加了什么

virtio 只负责“搬 buffer”。rpmsg 在它上面加了三件事：

1. **消息边界**：一条消息就是一条，不会粘在一起。
2. **地址**：每个端点有编号，知道谁发给谁。
3. **命名服务**：设备可以宣告“我提供什么服务”，
   驱动可以创建对应端点。

```text
┌─────────────────────────────┐
│  应用数据（串口字节/控制行）  │
├─────────────────────────────┤
│  rpmsg 头（src/dst/len/...） │
├─────────────────────────────┤
│  virtio buffer + vring       │
├─────────────────────────────┤
│  共享内存 + mailbox           │
└─────────────────────────────┘
```

---

## 13.2 rpmsg 消息头

Linux 的 rpmsg 头是 16 字节：

```text
偏移  长度  字段
0     4     src（源端点地址）
4     4     dst（目标端点地址）
8     4     reserved（保留）
12    2     len（payload 长度）
14    2     flags（标志，比如 NS 公告）
16    N     payload
```

### 地址

每个端点有一个 32 位地址。常见约定：

- `RPMSG_ADDR_ANY`：让系统分配。
- 地址 `0x400` 以上通常是动态端点。
- 地址 `53`（0x35）是命名服务端点（不同实现可能不同）。

这块板的代码：

```c
ret = rpmsg_create_ept(&ns_ept, &rvdev.rdev, "rpmsg_chrdev",
                       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
                       bridge_cb, NULL);
```

- 名字：`rpmsg_chrdev`。
- 源/目标地址：让系统分配。
- 回调：`bridge_cb`。

A7 侧创建端点时指定名字和地址：

```python
class EndpointInfo(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 32),
        ("src", ctypes.c_uint32),
        ("dst", ctypes.c_uint32),
    ]

info.name = b"rpmsg_chrdev"
info.src = RPMSG_ADDR_ANY
info.dst = 0
fcntl.ioctl(ctrl, RPMSG_CREATE_EPT_IOCTL, bytes(info))
```

内核会：

1. 创建一个 rpmsg 端点。
2. 给 E907 发命名服务消息（name = `rpmsg_chrdev`）。
3. E907 调用 `ns_bind_cb`，创建对应端点。
4. 内核创建 `/dev/rpmsgN` 节点。

### 为什么名字必须是 `rpmsg_chrdev`

因为 Linux 的 `rpmsg_char` 驱动只暴露这个名字的端点。
E907 在初始化时宣告：

```c
rpmsg_create_ept(&ns_ept, &rvdev.rdev, "rpmsg_chrdev", ...);
```

A7 用同名创建，双方才能对上。

> 如果你自己写一个新服务，名字要双方约定一致。
> 名字不匹配，A7 侧不会出现 `/dev/rpmsgN`。

---

## 13.3 端点池：为什么是 4 个

E907 侧：

```c
#define LINKR_MAX_EPTS 4
static struct rpmsg_endpoint linkr_epts[LINKR_MAX_EPTS];
static bool linkr_ept_used[LINKR_MAX_EPTS];

static struct rpmsg_endpoint *linkr_ept_alloc(void)
{
    for (int i = 0; i < LINKR_MAX_EPTS; i++) {
        if (!linkr_ept_used[i]) {
            return &linkr_epts[i];
        }
    }
    return NULL;
}
```

为什么需要池？

- A7 的 daemon 可能反复打开/关闭 `/dev/rpmsgN`。
- 每次打开都会触发一次 NS bind。
- 如果只有一个静态端点，新连接会覆盖旧的，正在用的回调
  就被换掉了。

池子让旧端点还能安全存在，直到 unbind 释放。

### unbind

```c
static void linkr_ept_unbind(struct rpmsg_endpoint *ept)
{
    linkr_bridge_rpmsg_unbind(ept);
    rpmsg_destroy_ept(ept);
    linkr_ept_used[ept - linkr_epts] = false;
}
```

- 通知 bridge：如果这是当前端点，清空 `bridge_ept`。
- 销毁 rpmsg 端点。
- 释放池槽位。

> 用指针减法 `ept - linkr_epts` 算索引，前提是 `ept` 确实
> 指向数组里的元素。这是 C 里常见的“容器反查”技巧。

---

## 13.4 握手：LINKR1

### 为什么需要握手

rpmsg 端点建立后，双方都能收发，但 E907 还不知道：

- A7 的端点地址是多少（用于回发）。
- A7 是否准备好接收串口数据。
- 当前 UART 统计是多少。

所以 A7 主动发一行：

```text
LINKR1\n
```

E907 收到后：

```c
if (handshake_match(p, len)) {
    atomic_set(&bridge_ready, 1);
    n = snprintk(reply, sizeof(reply), "LINKR1 OK rx=%u\n",
                 (unsigned int)atomic_get(&uart_rx_total));
    return rpmsg_sendto(ept, reply, n, src);
}
```

回复：

```text
LINKR1 OK rx=0
```

- `bridge_ready = 1`：之后收到的数据才当串口字节。
- `rx`：E907 已经收到的字节数，用于诊断。

### 握手前是“回显模式”

```c
if (!atomic_get(&bridge_ready)) {
    /* Legacy echo mode: keeps rpmsg_test / rpmsg_bench usable. */
    return rpmsg_sendto(ept, data, len, src);
}
```

握手前，E907 把收到的任何数据原样发回。这样独立的
`rpmsg_test`/`rpmsg_bench` 工具还能工作。

握手后，数据才进入 UART 桥。

> 这是一个很好的兼容设计：**新协议不影响旧工具。**
> 代价是多一个状态位，收益是调试方便。

### 记录 A7 地址

```c
bridge_ept = ept;
bridge_dst = src;
```

之后 E907 主动发数据（UART → A7）时，用 `bridge_dst` 作为目标。

> 为什么不在握手消息里带地址？因为 rpmsg 头里已经有 `src`，
> 直接取即可。少一个字段，少一处出错。

---

## 13.5 控制线：LINKRCFG 与 LINKRSTAT

### LINKRCFG：改 UART 参数

格式：

```text
A7 → E907:  LINKRCFG <baud>,<data>,<parity>,<stop>,<flow>\n
E907 → A7:  LINKRCFG OK uart=<baud>,<data>,<parity>,<stop>,<flow>\n
            LINKRCFG ERR=<errno>\n
```

例子：

```text
LINKRCFG 115200,8,n,1,n
LINKRCFG OK uart=115200,8,n,1,n
```

查询：

```text
LINKRCFG ?
LINKRCFG OK uart=115200,8,n,1,n
```

E907 解析：

```c
ntok = split_commas(args, tok, 5);
if (ntok != 5) return snprintk(reply, ..., "LINKRCFG ERR=-22\n");

baud = strtoul(tok[0], NULL, 10);
if (baud < 300 || baud > 4000000)
    return snprintk(reply, ..., "LINKRCFG ERR=-22\n");
```

- 5 个字段，少一个就 `-EINVAL`。
- 波特率范围 300..4000000。
- 数据位 5..8。
- 校验 n/e/o。
- 停止位 1/2。
- 流控 n/rtscts。

### 为什么控制线不会被目标板看到

`linkr_bridge_rpmsg_recv()` 先匹配控制前缀，匹配到就直接
回复，不写 `uart_tx_ring`：

```c
if (atomic_get(&bridge_ready) && len > LINKRCFG_PREFIX_LEN &&
    !memcmp(p, LINKRCFG_PREFIX, LINKRCFG_PREFIX_LEN)) {
    ...
    return rpmsg_sendto(ept, reply, n, src);
}
```

所以目标板不会收到 `LINKRCFG ...` 这行文本。

> 如果控制线漏到串口，目标设备会看到一堆乱码命令。
> 这是“控制通道”和“数据通道”必须分开的原因。

### LINKRSTAT：查缓冲占用

格式：

```text
A7 → E907:  LINKRSTAT\n
E907 → A7:  LINKRSTAT rx=<used>/<size> tx=<used>/<size> drop=<rx>/<tx>\n
```

例子：

```text
LINKRSTAT rx=0/65536 tx=0/65536 drop=0/0
```

A7 的 daemon 解析后拼成 `@i?` 里的：

```text
@info uart rx=... tx=... drop=... buffer=0/65536
```

> `@i?` 里的数字不是编的，是 E907 真实统计。

---

## 13.6 数据路径的优先级

`linkr_bridge_rpmsg_recv()` 的判断顺序：

```text
1. 握手 "LINKR1"
2. 控制 "LINKRSTAT"
3. 控制 "LINKRCFG "
4. 未握手 → 回显
5. 已握手 → 放进 uart_tx_ring
```

为什么控制线要在数据之前？

- 控制线很短，匹配成本低。
- 如果先当数据写进环，就撤不回来了。
- 控制线不能被大流量饿死。

### 背压

如果 `uart_tx_ring` 满了：

```c
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
```

- 先尽量放。
- 放不下就等 TX 线程释放空间，最多等 100 ms。
- 还放不下才丢，并计数。

> 这就是“背压”：让上游慢下来，而不是静默丢数据。
> 100 ms 是上限，防止 UART 卡死时把 rpmsg 端点永久挂住。

---

## 13.7 写一个最小 rpmsg 客户端

### Python 版（和 daemon 同思路）

```python
import ctypes, fcntl, os, select, struct, time

RPMSG_ADDR_ANY = 0xFFFFFFFF
RPMSG_CREATE_EPT_IOCTL = 0x4008AF01   # 具体值以内核头为准
RPMSG_DESTROY_EPT_IOCTL = 0xAF02

class EndpointInfo(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 32),
        ("src", ctypes.c_uint32),
        ("dst", ctypes.c_uint32),
    ]

before = {n for n in os.listdir("/dev")
          if n.startswith("rpmsg") and n[5:].isdigit()}

ctrl = os.open("/dev/rpmsg_ctrl0", os.O_RDWR)
info = EndpointInfo()
info.name = b"rpmsg_chrdev"
info.src = RPMSG_ADDR_ANY
info.dst = 0
fcntl.ioctl(ctrl, RPMSG_CREATE_EPT_IOCTL, bytes(info))

path = None
deadline = time.time() + 5
while time.time() < deadline:
    new = {n for n in os.listdir("/dev")
           if n.startswith("rpmsg") and n[5:].isdigit()} - before
    if new:
        path = "/dev/" + sorted(new)[0]
        break
    time.sleep(0.05)

if not path:
    raise SystemExit("no /dev/rpmsgN appeared")

fd = os.open(path, os.O_RDWR)
os.write(fd, b"LINKR1\n")
if select.select([fd], [], [], 2)[0]:
    print("reply:", os.read(fd, 4096))
```

> ioctl 的具体数字以你的内核头文件为准。不要在不懂的情况下
> 随便填，错了会返回 `EINVAL` 或 `ENOTTY`。

### C 版（伪代码）

```c
int ctrl = open("/dev/rpmsg_ctrl0", O_RDWR);
struct rpmsg_endpoint_info info = {
    .name = "rpmsg_chrdev",
    .src = RPMSG_ADDR_ANY,
    .dst = 0,
};
ioctl(ctrl, RPMSG_CREATE_EPT_IOCTL, &info);

int fd = open("/dev/rpmsg0", O_RDWR);
write(fd, "LINKR1\n", 7);
char buf[128];
int n = read(fd, buf, sizeof(buf));
```

---

## 13.8 实验：看握手和统计

### 实验 A：看 daemon 日志

```sh
tail -20 /var/log/linkr-bee.log
```

预期：

```text
rpmsg endpoint /dev/rpmsg0 ready, handshake reply b'LINKR1 OK rx=...'
```

### 实验 B：看 E907 日志

```sh
tail -20 /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

预期：

```text
A7 handshake from addr 0x..., uart rx=...
```

### 实验 C：查统计

网页终端：

```text
@i?
@u?
```

对照 `LINKRSTAT` 的数字。

### 实验 D：改波特率

```text
@u=9600,8,n,1,n
@u?
```

观察 E907 回复和实际串口行为。

---

## 13.9 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 没有 `/dev/rpmsgN` | 名字不匹配、NS 没处理 | `trace0` 的 NS bind |
| 打开成功但读不到 | 没发握手、E907 没回 | daemon 日志 |
| 握手回复乱码 | 端点被复用/旧数据 | 重新创建端点 |
| 控制命令跑到目标板 | 匹配顺序错/没加前缀 | 看目标串口输出 |
| `EMSGSIZE` | 一次写超过 MTU | 切片 |
| 数据只去不回 | `bridge_dst` 没设置 | 握手是否成功 |
| 多个客户端互相覆盖 | 端点池太小/没 unbind | 看 unbind 日志 |
| 大流量后控制命令没反应 | 没有背压/队列无界 | 查 `SerialWriter` |

---

## 13.10 练习

### 练习 13.1（必做）

画出从 `os.write(fd, "abc")` 到 E907 `ring_buf_put` 的路径，
标出 rpmsg 头、vring、mailbox。

### 练习 13.2（必做）

回答：

1. 为什么要先发 `LINKR1`？
2. `LINKRSTAT` 为什么不能当普通数据？
3. 端点池为什么是 4 个而不是 1 个？
4. 背压最多等多久？为什么要有上限？

### 练习 13.3（选做）

用 Python 写一个最小客户端，连接后发：

```text
LINKRSTAT
```

打印回复。不要修改 daemon。

---

## 13.11 本章验收

1. 能说出 rpmsg 头的字段和地址分配方式。
2. 能解释命名服务、`rpmsg_chrdev`、`/dev/rpmsgN` 的关系。
3. 能画出握手流程，并说明握手前后的行为差异。
4. 能写出/读懂 `LINKRCFG`、`LINKRSTAT` 的格式。
5. 能解释控制线为什么不会出现在目标串口。

### 自测题

1. rpmsg 头和 payload 各占多少字节？
2. 为什么 E907 要记录 `bridge_dst`？
3. 握手前的回显模式有什么用？
4. `LINKRCFG` 有几个字段？波特率范围是多少？
5. 背压失败后数据去哪了？怎么发现？

> 下一章进入 Zephyr：线程、信号量、消息队列、环形缓冲。
