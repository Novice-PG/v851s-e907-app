# 第 35 章 性能与内存：64 MB 里的工程取舍

> 本章目标：
> 1. 会读 `/proc/meminfo`、`VmRSS`、`slabtop`，知道内存到底被谁用了。
> 2. 理解 64 MB 系统里“预留、缓存、匿名页、CMA”的关系。
> 3. 知道 Python daemon 的内存成本，以及怎么把它压下来。
> 4. 会用 `top -H`、`perf`、`strace -c` 找 CPU 热点。
> 5. 能判断“该优化”还是“该接受”，并写出取舍理由。

---

## 35.1 先看数字

### 这块板

```text
DDR:      64 MB
NAND:     128 MB
A7:       600 MHz（单核或双核视型号）
E907:     RV32 @ 600 MHz，独立 4 MB DRAM 窗口
```

### 内存去哪了

```text
0x40000000 ─┬─ Linux 可用内存
            │
            ├─ 内核代码/数据
            ├─ E907 预留 4 MB（0x43c00000）
            ├─ rpmsg buffer 256 KB（0x43000000）
            ├─ vring0 128 KB（0x43040000）
            ├─ vring1 128 KB（0x43060000）
            ├─ CMA / DMA 区（Wi-Fi、显示等）
            └─ 用户空间
```

实际 `MemTotal` 会明显小于 64 MB——内核、预留、CMA 都先扣掉。

```sh
grep -E 'MemTotal|MemFree|MemAvailable|CmaTotal|CmaFree' /proc/meminfo
```

### 一个真实的量级

```text
MemTotal:        ~60 MB（视配置）
内核 + 驱动:      ~15 MB
Python daemon:    ~15-25 MB
BlueZ + bluetoothd: ~3-5 MB
shell/ssh/其他:   ~3-5 MB
剩余:             几 MB 到十几 MB
```

> 这就是为什么这块板不能装 systemd、不能跑 Node.js、
> 不能随手 `pip install`。

---

## 35.2 读内存：五个概念

### `VSZ` vs `RSS`

```text
VSZ（虚拟）= 进程“声称”用了多少地址空间
RSS（常驻）= 实际占了多少物理页
```

```sh
ps -o pid,vsz,rss,comm -p $(pidof linkr-bee-daemon)
```

一个进程 VSZ 200 MB、RSS 20 MB 很正常——虚拟地址不等于物理内存。

> **判断真实占用看 RSS，不看 VSZ。**

### `VmRSS` 与共享页

```sh
grep -E 'VmRSS|RssAnon|RssFile|RssShmem' /proc/<pid>/status
```

```text
RssAnon   自己分配的（堆、栈）
RssFile   映射的文件页（代码、库、mmap 的文件）
RssShmem  共享内存
```

Python 进程的库代码（`libpython`、`libc`）可能被多个进程共享，
所以“所有进程 RSS 相加”会大于实际使用。

### `MemAvailable` 才是“还能用多少”

```sh
grep MemAvailable /proc/meminfo
```

`MemFree` 只是“完全没用的页”。页缓存（page cache）随时可以回收，
所以 `MemAvailable` 更接近真实可用量。

> 看到 `MemFree` 很小不用慌，看 `MemAvailable`。

### page cache

Linux 会把读过的文件缓存在内存里：

```sh
cat /proc/meminfo | grep -E 'Cached|Buffers'
```

好处：读文件快。
坏处：看起来“内存被吃满”。

回收：

```sh
sync
echo 3 > /proc/sys/vm/drop_caches    # 仅调试用
```

**不要在生产上定期 drop_caches**，那是把性能扔掉。

### slab

内核对象缓存：

```sh
slabtop -o | head -20
```

常见大头：

```text
dentry     目录项缓存
inode      索引节点缓存
kmalloc-*  内核小对象
```

slab 持续增长可能是内核泄漏。**用户态泄漏看 RSS，内核泄漏看 slab。**

---

## 35.3 OOM：当内存真的不够

### OOM killer 怎么选

```text
内核按 oom_score 选一个进程杀掉
分数和 RSS、是否 root、是否在跑关键服务有关
```

看：

```sh
cat /proc/<pid>/oom_score
cat /proc/<pid>/oom_score_adj
```

### 现象

```sh
dmesg | grep -i -E 'oom|killed process'
```

```text
Out of memory: Killed process 1234 (linkr-bee-daemo)
```

### 保护关键进程

```sh
echo -500 > /proc/$(pidof linkr-bee-daemon)/oom_score_adj
```

范围 `-1000..1000`。越低越不容易被杀。

但**不要滥用**：如果 daemon 真的泄漏，保护它只会让系统更糟。

> 正确的做法是**修泄漏**，不是调分数。

### overcommit

```sh
cat /proc/sys/vm/overcommit_memory
```

```text
0  启发式（默认）
1  总是允许（危险）
2  严格按 CommitLimit
```

这块板保持默认即可。**不要为了“能 malloc 成功”去设 1。**

---

## 35.4 Python daemon 的内存账

### 基础成本

```text
CPython 解释器本身：    ~5-8 MB
标准库模块：           按 import 增加
每个线程的栈：         默认 8 MB 虚拟，实际按需
```

### 这块板的实测

```sh
grep VmRSS /proc/$(pidof linkr-bee-daemon)/status
```

量级：**15～25 MB**。

### 大头在哪

```text
1. Python 对象（dict、str、list）
2. 每个 WebSocket 连接的缓冲
3. SerialWriter 的 256 KiB 队列
4. WebDAV 上传的临时缓冲
5. 日志字符串
```

### 一个真实的坑：字符串拼接

```python
# 慢且费内存
log = ""
for line in lines:
    log += line + "\n"

# 好
log = "\n".join(lines)
```

在 64 MB 系统上，`+=` 在大数据上会产生大量临时对象。

### 另一个坑：一次性读大文件

```python
# 128 MB 文件 → 直接 OOM
data = open(path, "rb").read()

# 分块
while True:
    chunk = f.read(64 * 1024)
    if not chunk:
        break
    process(chunk)
```

### 线程数

```python
threading.Thread(...).start()
```

每个线程有栈。默认栈大小可能很大（虚拟），实际按需。但
**线程多了，调度和内存都会变差**。

这块板的策略：

```text
- 每个 WS 连接一个线程（连接数有限）
- 每个 BLE 连接一个线程（通常 1 个）
- 固定的 rpmsg/serial-tx/HTTP/HTTPS 线程
```

几十个连接就不行了。**这是设计约束，不是 bug。**

### 观测

```sh
# 线程数
grep Threads /proc/$(pidof linkr-bee-daemon)/status

# 每个线程的栈（虚拟）
ls /proc/$(pidof linkr-bee-daemon)/task | wc -l

# Python 对象统计（如果装了 gc）
python3 -c "import gc; print(gc.get_count())"
```

### 压内存的手段

| 手段 | 效果 | 代价 |
|---|---|---|
| 减队列上限 | 直接省几 MB | 更容易丢数据 |
| 限制连接数 | 省线程和缓冲 | 并发变少 |
| 用 `__slots__` | 省对象开销 | 代码改动 |
| 避免大字符串 | 省峰值 | 重构 |
| 不用 `json` 大对象 | 省峰值 | 手写序列化 |
| 换 C 实现数据面 | 省很多 | 开发成本 |

> 先量，再改。**没有测量的优化是猜。**

---

## 35.5 CPU：600 MHz 能做什么

### 量级感

```text
600 MHz，单核
≈ 每秒 6 亿个周期
≈ 几百万条 Python 字节码/秒（乐观）
```

### 各任务的成本

| 任务 | 成本 |
|---|---|
| 115200 串口转发 | 很低 |
| rpmsg 收发 | 低 |
| WebSocket 广播（几 KB/s） | 低 |
| TLS 握手（RSA 2048） | 中（几百 ms） |
| BLE 通知节流 | 低 |
| Python JSON 大对象 | 中高 |
| 图像/压缩 | 高 |

### `top -H`：看线程

```sh
top -H -p $(pidof linkr-bee-daemon)
```

看哪个线程吃 CPU：

```text
serial-tx   写 rpmsg
rpmsg      读 /dev/rpmsgN
https       TLS
ble-*       BLE 桥
```

### `strace -c`：看系统调用

```sh
strace -c -p $(pidof linkr-bee-daemon)
```

输出按调用次数/耗时排序。常见：

```text
read/write   正常
ioctl        rpmsg
futex        线程同步（多说明锁竞争）
```

> `futex` 特别多，通常是“锁粒度太粗”或“线程太多”。

### `perf`：看热点

```sh
perf top -p $(pidof linkr-bee-daemon)
```

如果内核支持。没有的话：

```sh
perf record -g -p <pid> -- sleep 10
perf report
```

### 常见的 CPU 浪费

```text
1. 忙轮询（while True: pass）        → 改阻塞/事件
2. 每帧打日志                         → 限频
3. 大字符串重复拼接                   → join
4. 每帧 json.dumps                    → 只发变化
5. 太多线程上下文切换                 → 合并
6. TLS 每次请求重新握手               → 长连接
```

### 一个真实例子：日志拖慢系统

```python
for byte in data:
    log("rx %02x" % byte)
```

115200 下每秒上万字节 → 每秒上万行日志 → 写盘 + 格式化
占满 CPU，还可能把 UDISK 写满。

修法：

```python
if verbose:
    log("rx %d bytes: %s" % (len(data), data[:16].hex()))
```

> **日志是代码，不是注释。**它也有成本。

---

## 35.6 内存与性能的取舍

### 案例一：队列多大

```text
E907 RX/TX 环：64 KiB × 2 = 128 KiB（在 E907 的 4 MB 里，不计入 A7）
daemon SerialWriter：256 KiB（在 A7 的 64 MB 里）
```

为什么可以这么大：

```text
- 吸收突发（粘贴、日志爆发）
- 减少丢包
```

代价：

```text
- 常驻内存
- 队列越长，延迟越大（背压越晚生效）
```

> 取舍：**在内存允许时，队列大一点更好。** 但要有上限。

### 案例二：BLE 节流

```text
5 ms 一轮 × 4 包
```

- 更快：吞吐高，但容易丢包/断连。
- 更慢：稳，但慢。

> 这不是“调到最快”，是“调到不丢”。

### 案例三：squashfs 压缩算法

```text
xz：镜像小，解压慢、内存多
gzip：折中
lz4：快，镜像大
```

64 MB 内存下，**解压时的峰值内存**要算进去。

### 案例四：Python vs C

```text
控制面（HTTP、命令、配置）：Python 合适
数据面（高频字节搬运）：C 更合适
```

这块板的选择：

```text
daemon（Python）负责控制
linkr-ble（C）负责 BLE 数据面
E907（C/Zephyr）负责 UART 数据面
```

> 分工不是“语言偏好”，是**按频率和延迟要求分层**。

---

## 35.7 实验

### 实验 A：看内存全景

```sh
free -m
cat /proc/meminfo | grep -E 'MemTotal|MemFree|MemAvailable|Cached|Slab|Cma'
ps -eo pid,vsz,rss,comm --sort=-rss | head -10
```

### 实验 B：看 daemon

```sh
PID=$(pidof linkr-bee-daemon)
grep -E 'VmRSS|RssAnon|RssFile|Threads' /proc/$PID/status
ls /proc/$PID/fd | wc -l
```

### 实验 C：压测内存

```sh
# 连开 10 个 WebSocket 客户端，观察 RSS
for i in $(seq 1 10); do
    (python3 -c "
import socket,base64,os
s=socket.create_connection(('192.168.0.121',80))
k=base64.b64encode(os.urandom(16)).decode()
s.sendall(('GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n'%k).encode())
import time; time.sleep(60)
" &)
done
watch -n 2 'grep VmRSS /proc/$(pidof linkr-bee-daemon)/status'
```

观察 RSS 增长是否可回收。

### 实验 D：找 CPU 热点

```sh
top -H -p $(pidof linkr-bee-daemon)
```

在大流量粘贴时看哪个线程高。

### 实验 E：日志成本

把日志级别调高，重复实验 D，对比 CPU。

---

## 35.8 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 系统随机杀进程 | OOM | `dmesg` |
| 内存看着满了但还能跑 | page cache | `MemAvailable` |
| RSS 持续上涨 | 泄漏 | 长稳曲线 |
| slab 持续上涨 | 内核泄漏 | `slabtop` |
| CPU 100% 但吞吐低 | 忙轮询/日志 | `top -H`、`strace -c` |
| 延迟忽大 | 队列长/GC/锁 | 队列长度、`futex` |
| TLS 握手慢 | RSA/CPU | 换 ECDSA、长连接 |
| 连接多了就崩 | 线程/内存 | 限制连接数 |
| 镜像解压失败 | 内存不足 | 换压缩算法 |
| `fork` 失败 | overcommit/内存 | `MemAvailable` |

---

## 35.9 练习

### 练习 35.1（必做）

在板子上记录：

```text
MemTotal / MemAvailable
daemon 的 VmRSS / Threads / fd 数
最大的 5 个进程（按 RSS）
slab 前 5 名
```

画一张“64 MB 去哪了”的图。

### 练习 35.2（必做）

回答：

1. VSZ 和 RSS 的区别？
2. 为什么 `MemFree` 小不一定有问题？
3. OOM killer 按什么选进程？
4. Python 进程的内存大头通常在哪？
5. 为什么“每帧打日志”会拖慢系统？
6. 什么时候该用 C 替代 Python？

### 练习 35.3（必做）

用 `top -H` 找出大流量时最忙的线程，解释它在做什么。

### 练习 35.4（必做）

把 `SerialWriter` 的上限从 256 KiB 改成 64 KiB，测：

```text
RSS 变化
丢包变化
```

然后判断这个改动值不值。

### 练习 35.5（选做）

写一个脚本，每 10 秒记录一次 RSS/fd/线程数，跑 1 小时，
用数据判断有没有泄漏。

### 练习 35.6（选做）

比较 gzip 和 xz 的 squashfs 在板子上的：

```text
镜像大小
启动时间
解压峰值内存
```

---

## 35.10 本章验收

1. 会读 `meminfo`、`VmRSS`、`slabtop`，能说清内存去向。
2. 理解 page cache、`MemAvailable`、OOM 的行为。
3. 知道 Python daemon 的内存成本和大头。
4. 会用 `top -H`、`strace -c`、`perf` 找 CPU 热点。
5. 能做出“队列多大、节流多快、用 Python 还是 C”的取舍。
6. 知道“没测量不优化”。

### 自测题

1. 判断真实内存占用看哪个指标？
2. page cache 会导致 OOM 吗？
3. OOM 分数怎么调？
4. 一个线程的栈占多少物理内存？
5. 为什么 BLE 节流不能无限加快？
6. 64 MB 系统最不该装什么？

> 下一章把“能用”变成“敢交付”：威胁模型、加固、产品化。
