# 第 19 章 A7 串口 daemon：线程、队列与背压

> 本章目标：
> 1. 知道 daemon 为什么用 Python 写、又为什么不能“随便写”。
> 2. 看懂 `RpmsgBridge`、`SerialWriter`、`Session` 三个核心对象。
> 3. 理解多线程 + 有界队列 + 背压的组合。
> 4. 知道 Python GIL 对吞吐的影响，以及怎么规避。
> 5. 能排查“daemon 占满 CPU”“串口越用越慢”“控制命令卡住”。

---

## 19.1 daemon 在整个系统里的位置

```text
浏览器 ──ws──┐
             ├──► linkr-bee-daemon ──rpmsg──► E907 ──UART3──► 目标板
手机 App ─BLE┘         │
                       ├── 静态网页（web/）
                       ├── 会话/令牌/证书
                       ├── Wi-Fi 控制
                       └── WebDAV 上传
```

它同时是：

- HTTP 服务器（80/8443）。
- WebSocket 服务器（`/ws`）。
- BLE 桥（Unix socket）。
- rpmsg 客户端（`/dev/rpmsgN`）。
- 状态管理（令牌、绑定、目标）。
- 系统命令代理（Wi-Fi、上传）。

### 为什么用 Python

| 因素 | 说明 |
|---|---|
| 开发速度 | 网络、JSON、线程、SSL 标准库齐全 |
| 板子资源 | 64 MB 内存够跑一个 Python 进程 |
| 无需编译 | 改完重启即可 |
| 标准库 | 不需要 pip 装东西（板子不一定联网） |

代价：

- 内存比 C 大（几十 MB）。
- 吞吐不如 C。
- GIL 限制 CPU 并行。

> 这块板的定位是“家用/教学级串口桥”，Python 的性能足够。
> 如果要做 10 Mbaud 持续吞吐，应该把数据面改成 C。

---

## 19.2 线程模型

daemon 启动后至少有这些线程：

```text
main           accept HTTP 连接
rpmsg          RX：读 /dev/rpmsgN → session.on_bridge_data
serial-tx      TX：SerialWriter 队列 → bridge.send
https          监听 8443
ble-socket     监听 /var/run/linkr-bee.sock
ble bridge N   每个 BLE 连接一个
ws client N    每个 WebSocket 连接一个
http conn N    每个 HTTP 连接一个
```

### 一条数据的路径

**目标板 → 浏览器：**

```text
E907 rpmsg
  → rpmsg 线程 os.read(fd)
  → session.on_bridge_data(data)
  → 去掉控制行
  → session._deliver(data)
  → broadcast(data)
  → 每个 Client.send_binary(data)
```

**浏览器 → 目标板：**

```text
WebSocket Client.run()
  → session.handle_binary(data)
  → serial_writer.write(data)   ← 只入队，立刻返回
  → serial-tx 线程取队列
  → bridge.send(chunk)          ← 按 MTU 切片
  → os.write(fd)
```

### 为什么 RX 和 TX 要分开

- RX 是“收到就广播”，不能阻塞。
- TX 受 UART 波特率限制，可能很慢。
- 如果 RX 线程同步写 rpmsg，大流量会把控制命令饿死。

```text
错误：一个线程既读又写
  读 → 处理 → 写（等 UART）→ 才能读下一条
  → 控制命令 @c? 排在 100 KB 粘贴后面

正确：读写分离
  读线程：只入队/广播
  写线程：按自己的节奏发
```

---

## 19.3 `RpmsgBridge`：端点管理与切片

### 创建端点

```python
def _create_endpoint(self):
    before = _rpmsg_devices()

    ctrl = os.open("/dev/rpmsg_ctrl0", os.O_RDWR)
    info = EndpointInfo()
    info.name = b"rpmsg_chrdev"
    info.src = RPMSG_ADDR_ANY
    info.dst = 0
    fcntl.ioctl(ctrl, RPMSG_CREATE_EPT_IOCTL, bytes(info))

    # 等新的 /dev/rpmsgN 出现
    deadline = time.time() + 5.0
    while time.time() < deadline:
        new = _rpmsg_devices() - before
        if new:
            path = "/dev/" + sorted(new)[0]
            break
        time.sleep(0.05)

    fd = os.open(path, os.O_RDWR)
    os.write(fd, b"LINKR1\n")
    ...
```

要点：

1. **先记录已有设备，再创建**，用差集找出新节点。
2. **轮询等待**最多 5 秒。不要假设 ioctl 返回时节点已出现。
3. **握手**在创建后立刻发，避免 E907 处于回显模式太久。

### 重连循环

```python
def _run(self):
    while True:
        try:
            self._create_endpoint()
        except Exception as exc:
            log("rpmsg setup failed: %s" % exc)
            time.sleep(2.0)
            continue

        try:
            while True:
                data = os.read(self.fd, 4096)
                if not data:
                    raise OSError("endpoint closed")
                self.on_data(data)
        except OSError as exc:
            log("rpmsg RX stopped: %s" % exc)
        finally:
            self._drop_endpoint()

        time.sleep(1.0)
```

设计要点：

- **永远重连**：E907 重启、remoteproc 重载都能自愈。
- **回调异常不杀线程**：`on_data` 包在 try 里（代码里有）。
- **先 destroy 再 close**：避免内核端点泄漏。

### 发送切片

```python
def send(self, data):
    with self.send_lock:
        if self.fd is None:
            raise OSError("rpmsg endpoint not ready")
        view = memoryview(data)
        sent = 0
        while sent < len(view):
            chunk = view[sent:sent + self.max_chunk]
            n = os.write(self.fd, chunk)
            if n <= 0:
                break
            sent += n
        return sent
```

- `max_chunk=256`，和 E907 `RX_CHUNK` 对齐。
- 锁保证多个线程不会交错写。
- `memoryview` 避免复制。

> 为什么不能“一次写 100 KB”？rpmsg_char 会返回
> `EMSGSIZE`。切片是硬要求，不是优化。

---

## 19.4 `SerialWriter`：有界队列 + 背压

### 完整代码

```python
class SerialWriter:
    def __init__(self, bridge, limit=262144, chunk=256):
        self.bridge = bridge
        self.limit = limit
        self.chunk = chunk
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.dropped = 0
        self.thread = threading.Thread(target=self._run, name="serial-tx",
                                       daemon=True)

    def write(self, data):
        with self.cond:
            room = self.limit - len(self.buf)
            if room <= 0:
                self.dropped += len(data)
                return 0
            if len(data) > room:
                self.dropped += len(data) - room
                data = data[:room]
            self.buf += data
            self.cond.notify()
            return len(data)

    def _run(self):
        while True:
            with self.cond:
                while not self.buf:
                    self.cond.wait()
                chunk = bytes(self.buf[:self.chunk])
                del self.buf[:self.chunk]
            try:
                self.bridge.send(chunk)
            except OSError as exc:
                log("rpmsg TX failed: %s" % exc)
                time.sleep(0.2)
```

### 为什么是 256 KiB

- 115200 下 256 KiB 约 22 秒。
- 4 Mbaud 下约 0.5 秒。
- 足够容纳一次大粘贴，又不至于吃光 64 MB 内存。

> 有界是重点。无界队列在“浏览器疯狂发、串口很慢”时会把
> 内存吃光，然后 OOM。

### 背压 vs 丢弃

这块板的选择是**有界 + 丢弃 + 计数**：

```text
写入方（WS/BLE）：
  write() 立刻返回，返回实际接受字节数
队列满：
  多出来的字节丢弃，dropped 累加
```

为什么不阻塞写入方？

- 阻塞会让 WebSocket 读线程卡住，控制命令也读不到。
- 丢弃 + 计数让上层知道“慢了”，可以提示用户。

> 对比 E907 侧：E907 用“有界 + 阻塞 100 ms + 丢弃”。
> 两侧策略一致：**先尽量缓冲，实在不行才丢，并且计数。**

### 日志限频

```python
def _log_drops(self):
    mark = self.dropped // 8192
    if mark != self.drop_logged:
        self.drop_logged = mark
        log("serial queue full: dropped %d bytes so far (limit %d)" %
            (self.dropped, self.limit))
```

每丢 8 KiB 才打一行。否则大流量时日志本身会拖垮系统。

---

## 19.5 Python GIL：什么会卡，什么不会

### GIL 是什么

CPython 同一时刻只有一个线程执行 Python 字节码。

```text
线程 A: 执行 Python 代码
线程 B: 等 GIL
```

但**阻塞的系统调用会释放 GIL**：

- `os.read` / `os.write`
- `socket.recv` / `send`
- `select`
- `time.sleep`

所以：

| 操作 | 是否释放 GIL |
|---|---|
| 纯 Python 循环 | 否 |
| `os.read` | 是 |
| `socket.sendall` | 是 |
| `json.dumps` 大对象 | 否（CPU 密集） |
| `bytearray` 切片 | 否（短） |

### 对 daemon 的影响

- RX/TX 主要时间在 `os.read`/`os.write`，GIL 不是瓶颈。
- 真正的瓶颈是 UART 波特率和 rpmsg 往返。
- 但**大 JSON、大字符串处理**会占 GIL，表现为“偶尔卡顿”。

> 如果发现 CPU 100% 且吞吐上不去，先看是不是在 Python 里
> 做大块数据转换，而不是 I/O。

### 线程不是越多越好

每个 WS 客户端一个线程、每个 BLE 连接一个线程。连接数
不多时没问题；几十个客户端时，线程切换和内存会成为负担。

这块板的目标场景是“几个人用”，不是“公网服务器”。

---

## 19.6 `Session`：状态与广播

### 它保存什么

```python
self.clients = {}       # 连接的客户端
self.owner = 0          # 当前控制者
self.managed = set()    # 主动管理过的客户端
self.ws_token = ...     # LAN 令牌
self.webdav_url = ...   # 上传目标
self.target = ...       # 目标标识
self.bond_labels = ...  # BLE 绑定备注
self.uart_config = ...  # UART 参数缓存
```

### 广播

```python
def _deliver(self, data):
    if not data:
        return
    self.upload_bytes += len(data)
    self.webdav.feed(data)
    self.broadcast(data)
```

一条串口数据会：

1. 累加统计。
2. 喂给 WebDAV 上传器（如果配置了）。
3. 广播给所有 WebSocket/BLE 客户端。

### 控制回复的剥离

```python
CTL_PREFIXES = (b"LINKRCFG ", b"LINKRSTAT ")

def on_bridge_data(self, data):
    ...
    if data.startswith(CTL_PREFIXES):
        idx = data.find(b"\n")
        ...
        self._handle_ctl_reply(data[:idx])
        self._deliver(data[idx + 1:])
        return
    self._deliver(data)
```

E907 的 `LINKRCFG OK ...` / `LINKRSTAT ...` 是**给 daemon 的**，
不能出现在目标板终端里。

难点：控制回复可能和目标数据**粘在同一条 rpmsg 消息里**。代码
在“有请求在飞”时按前缀切分：

```python
if self._ctl_waiting and self._ctl_pending is None:
    idx = _ctl_prefix_index(data)
    if idx < 0:
        self._deliver(data)
        return
    if idx:
        self._deliver(data[:idx])
        data = data[idx:]
```

> 这是真实世界的“协议粘包”问题。任何“控制和数据共用一条流”
> 的设计都要处理它。

---

## 19.7 实验：观察线程与队列

### 实验 A：看线程

```sh
# 板子上
ls /proc/$(pidof linkr-bee-daemon)/task | wc -l
cat /proc/$(pidof linkr-bee-daemon)/status | grep -E 'Threads|VmRSS'
```

### 实验 B：制造队列压力

在网页终端粘贴一大段文本（比如 200 KB），同时观察：

```sh
tail -f /var/log/linkr-bee.log
```

可能看到：

```text
serial queue full: dropped N bytes so far (limit 262144)
```

### 实验 C：控制命令是否被饿死

在大粘贴进行时，立刻输入：

```text
@c?
```

如果很快回复，说明 TX 线程和控制通道没被阻塞。
如果卡住，检查是不是有人把写操作改成了同步。

### 实验 D：看 rpmsg 重连

```sh
echo stop > /sys/class/remoteproc/remoteproc0/state
echo start > /sys/class/remoteproc/remoteproc0/state
tail -f /var/log/linkr-bee.log
```

应该看到 `rpmsg RX stopped`，然后自动重连、握手。

---

## 19.8 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| daemon CPU 100% | 忙轮询/大 JSON | `top -H`、py-spy |
| 串口越用越慢 | 队列积压/对端不读 | 日志 drop、`LINKRSTAT` |
| 控制命令卡住 | 同步写阻塞读线程 | 改回队列 |
| 内存持续增长 | 无界队列/未清理客户端 | `VmRSS`、客户端数 |
| 重连失败 | 端点泄漏/名字错 | `ls /dev/rpmsg*`、日志 |
| 数据错位/粘包 | 控制回复未剥离 | 看目标终端是否出现 `LINKR*` |
| 大包发送失败 | 没切片 | `EMSGSIZE` |
| 多线程写交错 | 没加 send 锁 | 数据错乱 |
| 掉电丢令牌 | 没 rename/sync | 查写文件函数 |
| 关服务慢 | 线程没退出 | `stop` 逻辑、daemon 线程 |

---

## 19.9 练习

### 练习 19.1（必做）

阅读 `RpmsgBridge`，回答：

1. 为什么要先记录 `/dev/rpmsg*` 再创建端点？
2. `max_chunk=256` 的依据是什么？
3. `_run` 为什么用 `while True` 包住创建和读取？
4. `send_lock` 保护什么？

### 练习 19.2（必做）

阅读 `SerialWriter`，回答：

1. 队列上限是多少？为什么有界？
2. `write()` 为什么立刻返回而不是阻塞？
3. 丢弃时怎么让用户知道？
4. 日志为什么要限频？

### 练习 19.3（必做）

画出“浏览器 → 目标板”的完整线程/队列路径，标出：

- WebSocket 线程
- `SerialWriter.write`
- `serial-tx` 线程
- `RpmsgBridge.send`
- `os.write`
- E907

### 练习 19.4（选做）

写一个小脚本，向 `/dev/rpmsgN` 连续写 1 MB 数据，测量：

- 总耗时。
- 是否出现 `EMSGSIZE`。
- `LINKRSTAT` 的 drop 计数。

**注意别把目标板刷死**，先用回环或假目标。

---

## 19.10 本章验收

1. 能画出 daemon 的线程模型和数据路径。
2. 能解释 `RpmsgBridge` 的创建、握手、切片、重连。
3. 能解释 `SerialWriter` 的有界队列和丢弃策略。
4. 知道 GIL 在哪释放、哪里可能成为瓶颈。
5. 能解释控制回复为什么要从数据流里剥离。
6. 能排查 CPU、内存、卡顿、粘包四类问题。

### 自测题

1. 为什么 RX 和 TX 要分两个线程？
2. rpmsg 一次最多写多少？为什么？
3. `SerialWriter` 队列满时会发生什么？
4. `os.read` 会释放 GIL 吗？
5. `LINKRSTAT` 回复为什么不能出现在目标终端？
6. daemon 怎么从 E907 重启中恢复？

> 下一章从零写一个最小 WebSocket/HTTP 服务器。
