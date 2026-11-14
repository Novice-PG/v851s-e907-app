# 第 4 章 Linux 补课：进程、文件描述符、socket、权限

> 本章目标：
> 1. 理解“进程”和“线程”在 daemon 里怎么分工。
> 2. 知道 `read()`/`write()` 的真实语义，不再假设“写多少就发多少”。
> 3. 能看懂并写出一个最小 TCP 服务器。
> 4. 搞清 `chmod 600`、`/var` 是内存盘、overlayfs 这些事为什么影响系统。
> 5. 能自己写一个 systemd 之外的“板级启动脚本”。

---

## 4.1 从 C 程序到进程

你在主机上运行 `./hello`，内核做三件事：

```text
1. 读 ELF 头，找到入口
2. 建立虚拟地址空间，把代码/数据映射进去
3. 创建一个 task（进程），开始执行
```

进程拥有：

- 独立的虚拟地址空间（一个进程崩了，不会直接改坏另一个）。
- 一组文件描述符（fd）。
- 至少一个线程（主线程）。

线程共享：

- 同一个地址空间（所以能直接读同一个全局变量，也所以会打架）。
- 同一组文件描述符。
- 但有自己的栈和寄存器。

> 一句话：**进程是资源边界，线程是执行边界。**
> 需要隔离就开进程，需要共享内存就开线程。

### daemon 里有哪些执行流

`linkr-bee-daemon` 启动后大致是这样：

```text
主线程
  └─ accept() 循环，等 HTTP 连接

rpmsg 线程
  └─ os.read(/dev/rpmsgN) 循环，收 E907 数据

serial-tx 线程
  └─ 从队列取数据，write(/dev/rpmsgN)

ble-socket 线程
  └─ accept() Unix socket，等 linkr-ble

https 线程
  └─ accept() 8443

每个 WebSocket 客户端
  └─ 一个线程

每个 BLE 客户端
  └─ 一个线程

webdav 线程
  └─ 上传任务
```

它们共享 `Session` 对象（会话、令牌、命令状态），所以需要锁。

### 实验 4.1：看线程

在板子上：

```sh
ps w | grep linkr-bee-daemon
ls /proc/$(pidof linkr-bee-daemon)/task | wc -l
cat /proc/$(pidof linkr-bee-daemon)/status | grep Threads
```

`Threads` 就是线程数。多开几个网页，数字会变。

> 线程不是越多越好。每个线程有栈（Linux 默认 8 MB 虚拟，
> 实际按需分配），上下文切换也有成本。64 MB 的板子上
> 要克制。

---

## 4.2 文件描述符：Linux 的“万能把手”

### 一切皆文件

```text
普通文件   /etc/rootfs_version
目录       /mnt/UDISK
设备       /dev/rpmsg0、/dev/ttyS2
管道       |
socket     TCP 连接、Unix socket
```

打开它们都返回一个**非负整数**，叫 fd：

```c
int fd = open("/dev/rpmsg0", O_RDWR);
if (fd < 0) {
    perror("open");
    return;
}
```

每个进程启动时默认有：

| fd | 名字 | 默认指向 |
|---|---|---|
| 0 | stdin | 终端/管道 |
| 1 | stdout | 终端/日志 |
| 2 | stderr | 终端/日志 |

### 读写

```c
ssize_t n = read(fd, buf, sizeof(buf));
ssize_t m = write(fd, buf, len);
close(fd);
```

`ssize_t` 是“有符号大小”，因为可能返回 `-1`。

### daemon 里的真实用法

`RpmsgBridge._create_endpoint()`：

```python
ctrl = os.open("/dev/rpmsg_ctrl0", os.O_RDWR)
...
fd = os.open(path, os.O_RDWR)
os.write(fd, b"LINKR1\n")
```

Python 的 `os.open/read/write/close` 就是系统调用的薄封装。
理解 C 的 fd，才能理解 Python 这段在干什么。

### 为什么 rpmsg 是“文件”

Linux 把很多设备抽象成文件，好处是：

- 能用同一套 `open/read/write/close`。
- 能用 `select/poll/epoll` 等统一等待。
- 权限、命名、挂载点都复用文件系统规则。

代价是：**有些“文件”不支持普通文件的操作**。
比如 `/dev/rpmsg0` 上 `lseek` 没意义，`write` 超过 MTU 会
返回 `EMSGSIZE`。

---

## 4.3 `read()`/`write()` 的真相

### 短读、短写

课本常说“read 读 n 个字节”，真实语义是：

- `read(fd, buf, n)` 返回**最多** n 个字节，可能更少。
- `write(fd, buf, n)` 返回**最多** n 个字节，可能更少。
- 返回 0 表示 EOF（对 read）或写不进（罕见）。
- 返回 -1 表示错误，要看 `errno`。

所以正确写法是循环：

```c
static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;            /* 被信号打断，重试 */
            }
            return -1;
        }
        if (n == 0) {
            return -1;               /* 对端关闭或无法写 */
        }
        off += (size_t)n;
    }
    return 0;
}
```

### 这块板上的两个“短写”来源

**来源一：rpmsg MTU**

`RpmsgBridge.send()`：

```python
view = memoryview(data)
sent = 0
while sent < len(view):
    chunk = view[sent:sent + self.max_chunk]
    n = os.write(self.fd, chunk)
    if n <= 0:
        break
    sent += n
```

注释写得很清楚：

```text
rpmsg_char rejects writes larger than the endpoint MTU
(EMSGSIZE / errno 90). The Linux rpmsg buffer is 512 bytes,
minus the 16-byte rpmsg header, so 256 leaves plenty of headroom.
```

**来源二：TCP 发送缓冲**

`sendall()` 内部会循环，但如果对端一直不读，缓冲满后
它会**永久阻塞**。所以 daemon 给每个连接设了：

```python
conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO,
                struct.pack("ll", 10, 0))
```

10 秒写不出去就报错，而不是把整个进程拖死。

### `EINTR`：被信号打断

慢系统调用（`read`、`accept`、`select`）可能因为收到信号
返回 -1，`errno == EINTR`。这不是错误，重试即可。

```c
do {
    n = read(fd, buf, sizeof(buf));
} while (n < 0 && errno == EINTR);
```

Python 3 会自动重试大部分 `EINTR`，但 C 里必须自己处理。

### 阻塞与非阻塞

```c
fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
```

非阻塞模式下：

- `read` 没数据返回 -1，`errno == EAGAIN`。
- `write` 缓冲满返回 -1，`errno == EAGAIN`。
- 需要配合 `select`/`poll`/`epoll` 才知道什么时候可读可写。

这块板的 daemon 用的是“阻塞 fd + 每连接一个线程”，
简单直接，适合客户端不多的场景。

### 实验 4.2：观察短读

```c
/* short.c */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(void)
{
    int fd = open("/etc/hostname", O_RDONLY);
    char buf[8];
    ssize_t n;

    if (fd < 0) { perror("open"); return 1; }

    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read");
            break;
        }
        printf("got %zd bytes\n", n);
    }
    close(fd);
    return 0;
}
```

把 `sizeof(buf)` 改成 1、2、8，观察每次返回多少。
普通文件通常“要多少给多少”，但管道、socket、串口常常更少。

---

## 4.4 socket：从“打开文件”到“打开连接”

### TCP 服务器的五个动作

```c
int s = socket(AF_INET, SOCK_STREAM, 0);

int one = 1;
setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(8080),
    .sin_addr.s_addr = htonl(INADDR_ANY),
};
bind(s, (struct sockaddr *)&addr, sizeof(addr));

listen(s, 16);

for (;;) {
    int c = accept(s, NULL, NULL);
    /* 处理 c */
}
```

- `socket`：创建“电话”。
- `bind`：分配号码（IP + 端口）。
- `listen`：开机接听。
- `accept`：等一个来电，返回新的 fd。
- `read/write`：通话。

### `SO_REUSEADDR` 为什么必须有

进程退出后，端口会进入 `TIME_WAIT` 一段时间。没有
`SO_REUSEADDR`，重启服务会报 `Address already in use`。

daemon 里每个监听 socket 都设了：

```python
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
```

### 字节序

网络字节序是大端：

```c
htons(port)      /* host to network short */
htonl(addr)      /* host to network long */
```

接收端再 `ntohs`/`ntohl` 转回来。ARM 是小端，所以这一步不能省。

### 一个能跑的最小 TCP 服务器

```c
/* echo_server.c */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in addr = {0};
    char buf[256];

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(s, (struct sockaddr *)&addr, sizeof(addr));
    listen(s, 4);
    printf("listening on 8080\n");

    for (;;) {
        int c = accept(s, NULL, NULL);
        ssize_t n;

        while ((n = read(c, buf, sizeof(buf))) > 0) {
            write(c, buf, (size_t)n);     /* 回显 */
        }
        close(c);
    }
}
```

```sh
gcc -Wall echo_server.c -o echo_server
./echo_server
# 另开一个终端
printf 'hello\n' | nc 127.0.0.1 8080
```

### 为什么 daemon 每个连接开一个线程

```python
threading.Thread(target=handle_connection,
                 args=(conn, addr, ...), daemon=True).start()
```

因为 `read` 是阻塞的：一个客户端连上来不发数据，主线程
如果直接 `read`，其他客户端就都进不来。

> 另一种做法是 `select`/`epoll` 单线程事件循环，省线程但代码复杂。
> 客户端少的时候，“每连接一线程”更直观。

---

## 4.5 线程、锁、条件变量

### 共享数据必须保护

`SerialWriter` 的队列同时被两个线程访问：

- WebSocket 读线程调用 `write()` 入队。
- `serial-tx` 线程调用 `_run()` 出队。

```python
self.lock = threading.Lock()
self.cond = threading.Condition(self.lock)
```

入队：

```python
with self.cond:
    ...
    self.buf += data
    self.cond.notify()
```

出队：

```python
with self.cond:
    while not self.buf:
        self.cond.wait()
    chunk = bytes(self.buf[:self.chunk])
    del self.buf[:self.chunk]
```

### 为什么用 `while not self.buf` 而不是 `if`

`Condition.wait()` 可能被“假唤醒”（spurious wakeup），
也可能醒来后发现数据又被别的消费者拿走了。必须重新检查条件。

### 为什么不在锁里发数据

注意 `bridge.send(chunk)` 在 `with self.cond` **外面**：

```python
with self.cond:
    ...
    chunk = bytes(self.buf[:self.chunk])
    del self.buf[:self.chunk]
try:
    self.bridge.send(chunk)
```

如果拿着锁去写 rpmsg（可能阻塞），入队线程就会被卡住。
**锁只保护共享状态，不保护慢操作。**

### 有界队列与背压

队列上限 256 KiB：

```python
def __init__(self, bridge, limit=262144, chunk=256):
```

队列满时：

```python
room = self.limit - len(self.buf)
if room <= 0:
    self.dropped += len(data)
    ...
    return 0
```

不是无限增长。嵌入式设备内存有限，**队列必须有界**，
满了要么丢、要么让上游等，不能把内存吃光。

### 实验 4.3：观察竞态

```c
/* race.c */
#include <pthread.h>
#include <stdio.h>

static long counter;

static void *worker(void *arg)
{
    (void)arg;
    for (long i = 0; i < 1000000; i++) {
        counter++;
    }
    return NULL;
}

int main(void)
{
    pthread_t a, b;

    pthread_create(&a, NULL, worker, NULL);
    pthread_create(&b, NULL, worker, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("counter = %ld (expect 2000000)\n", counter);
    return 0;
}
```

```sh
gcc -O2 race.c -o race -pthread
./race
./race
./race
```

多跑几次，结果会小于 2000000。把 `counter++` 换成
`__sync_fetch_and_add(&counter, 1)` 或加互斥锁，就稳定了。

> 这个实验在 x86 上也可能“看起来正常”，换 `-O2`、多核、
> 多次运行才暴露。**并发 bug 不是每次都能复现，这才是它可怕的地方。**

---

## 4.6 Unix domain socket：本机进程之间的“电话”

`linkr-ble` 和 daemon 不需要走 TCP，它们在同一台机器上：

```python
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind("/var/run/linkr-bee.sock")
os.chmod("/var/run/linkr-bee.sock", 0o600)
server.listen(4)
```

好处：

- 不占端口，不会被局域网访问。
- 用文件权限控制谁能连（600 = 只有 root）。
- 比 TCP 少一层协议开销。

客户端（`linkr-ble` 一侧）：

```c
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
struct sockaddr_un addr = { .sun_family = AF_UNIX };
strncpy(addr.sun_path, "/var/run/linkr-bee.sock",
        sizeof(addr.sun_path) - 1);
connect(fd, (struct sockaddr *)&addr, sizeof(addr));
```

> 看到 `/var/run/xxx.sock`，就知道这是本机进程通信。
> 它和 `/dev/rpmsg0` 的区别：一个是“连接到另一个进程”，
> 一个是“和内核里的驱动说话”。

---

## 4.7 权限：为什么令牌必须是 600

### 三个数字

```text
rwx rwx rwx
所有者 组 其他人
```

```text
600 = rw-------   只有所有者能读写
644 = rw-r--r--   所有人能读
755 = rwxr-xr-x   所有人能进目录/执行
```

### 真实代码

```python
os.chmod(WS_TOKEN_FILE, 0o600)
os.chmod(BLE_SOCK_PATH, 0o600)
```

- 令牌文件 600：网页进程能读，其他用户不能。
- BLE socket 600：只有 root（或同用户）能连，防止局域网里
  别的进程冒充 `linkr-ble` 往串口写数据。

### `umask`

新建文件的默认权限会被 `umask` 削掉：

```sh
umask          # 常见 022
touch a.txt    # 实际权限 644，不是 666
```

敏感文件不要依赖默认值，创建后显式 `chmod`。

### 镜像里没有秘密

设计原则：**镜像可以公开，秘密只在 UDISK。**

| 内容 | 位置 | 权限 |
|---|---|---|
| Wi-Fi 密码 | `/mnt/UDISK/wpa_supplicant.conf` | 600 |
| LAN 令牌 | `/mnt/UDISK/persist/ws-token` | 600 |
| HTTPS 私钥 | `/mnt/UDISK/persist/certs/linkr-bee.key` | 600 |
| BLE 绑定 | `/mnt/UDISK/persist/bluetooth/` | 目录 |
| root 密码 | `/mnt/UDISK/persist/etc/shadow` | 600 |

> 私有 key 一旦进了镜像，就等于公开。刷机包会被人解压、
> 上传、镜像站缓存。**永远不要把私钥打进 rootfs。**

### 实验 4.4：验证权限

```sh
ls -l /mnt/UDISK/persist/ws-token
stat -c '%a %U:%G %n' /mnt/UDISK/persist/ws-token
```

预期 `600 root:root`。如果不对：

```sh
chmod 600 /mnt/UDISK/persist/ws-token
```

---

## 4.8 守护进程：没有终端的程序

### 前台 vs 后台

```sh
./program            # 前台，Ctrl-C 结束
./program &          # 后台，终端关了可能收到 SIGHUP
nohup ./program &    # 忽略 SIGHUP，输出重定向到 nohup.out
```

板子上的 `S53linkr`：

```sh
nohup python3 -u "$DAEMON" --port "$PORT" ... >>"$LOGFILE" 2>&1 &
echo $! >"$PIDFILE"
```

三个细节：

- `nohup`：父 shell 退出后不被挂断。
- `-u`：Python 不缓冲 stdout，日志实时落盘。
  不加 `-u`，崩溃前的最后几行可能还在缓冲区里，永远看不到。
- `>>"$LOGFILE" 2>&1`：stdout 和 stderr 都进日志。
- `$!` 是刚启动进程的 PID，写进 pidfile 供 `stop()` 使用。

### 为什么不用 systemd

Tina 用的是 busybox init，没有 systemd。它的“服务管理”
就是 `/etc/init.d/S??` 脚本 + pidfile：

```sh
start()  { ... }
stop()   { kill "$(cat "$PIDFILE")"; ... }
restart(){ stop; start; }
```

### 判断进程是否还活着

```sh
kill -0 "$(cat /var/run/linkr-bee.pid)" 2>/dev/null
```

`kill -0` 不发信号，只检查“这个 PID 存在且我有权限”。
退出码 0 表示活着。

> 注意 PID 复用：进程死了，PID 可能被别的程序占用。
> 严格的做法是再核对 `/proc/$pid/cmdline`。教学项目里
> pidfile + `kill -0` 够用。

### 前台调试

```sh
# 停掉服务
/etc/init.d/S53linkr stop

# 前台跑，日志直接打屏幕
python3 -u /usr/bin/linkr-bee-daemon --port 80 --web-root /usr/share/linkr-bee/web
```

前台跑能看到完整 traceback。**排障第一步：把后台服务拉到前台。**

---

## 4.9 日志：写什么、放哪、怎么看

### 板子上的日志位置

| 日志 | 内容 |
|---|---|
| `/var/log/linkr-bee.log` | daemon |
| `/var/log/linkr-ble.log` | BLE GATT |
| `/var/log/bluetooth-init.log` | S54 脚本 |
| `/var/log/hciattach.log` | 蓝牙控制器 attach |
| `dmesg` | 内核 |
| `trace0` | E907 |

### 日志要包含什么

一条有用的日志至少有：

- 时间（如果空间允许）。
- 谁（模块/线程）。
- 发生了什么（动作 + 关键参数）。
- 结果（成功/错误码）。

反例：

```text
error
```

正例：

```text
rpmsg endpoint /dev/rpmsg0 ready, handshake reply b'LINKR1 OK rx=0\n'
https handshake from 192.168.0.50:51234 failed: ...
serial queue full: dropped 8192 bytes so far (limit 262144)
```

### 不要在热路径里刷日志

串口每来一个字节就 `printf`，日志会：

- 把 CPU 吃满。
- 把存储写满。
- 把真正重要的错误淹没。

daemon 的丢弃日志做了“每 8192 字节最多一条”的节流：

```python
mark = self.dropped // 8192
if mark != self.drop_logged:
    self.drop_logged = mark
    log(...)
```

> 这条规则在嵌入式里尤其重要：**日志本身也是资源消耗者。**

### 实验 4.5：实时看三路日志

开三个终端：

```sh
ssh root@<board-ip> 'tail -f /var/log/linkr-bee.log'
ssh root@<board-ip> 'tail -f /var/log/linkr-ble.log'
ssh root@<board-ip> 'tail -f /sys/kernel/debug/remoteproc/remoteproc0/trace0'
```

然后在网页里输入 `@i?`，观察哪一路有输出。

---

## 4.10 挂载、overlayfs、`/var` 是内存盘

### `mount` 在做什么

把“一个文件系统”挂到“目录树的一个点”上：

```sh
mount -t debugfs none /sys/kernel/debug
mount /dev/ubi0_5 /mnt/UDISK
mount --bind /mnt/UDISK/persist/bluetooth /etc/lib/bluetooth
```

挂载后，访问 `/mnt/UDISK` 实际访问的是 UBI 卷。

### 这块板的根文件系统

```text
/            overlayfs
├─ lower     /rom        squashfs（只读，来自 rootfs 分区）
└─ upper     /mnt/UDISK/overlay/upper（可写）
```

效果：

- 你 `echo x > /etc/foo`，实际写到 upper，lower 不动。
- 重启后 upper 还在，修改保留。
- 重新刷 rootfs 只换 lower，upper 里的旧文件可能“盖住”新文件。

> 这是升级时最容易踩的坑：镜像里更新了 `/etc/xxx`，
> 但板子 upper 里还有旧版本，看起来“刷了没生效”。

### `/var` 为什么不能存重要数据

```sh
ls -ld /var
```

在 Tina 上常见：

```text
lrwxrwxrwx  /var -> /tmp
```

而 `/tmp` 是 tmpfs（内存盘），重启清空。

所以 BlueZ 的配对信息不能放默认的 `/var/lib/bluetooth`。
这个镜像用 `--localstatedir=/etc` 构建，配对实际在
`/etc/lib/bluetooth`，再由启动脚本 bind mount 到 UDISK：

```sh
mount --bind /mnt/UDISK/persist/bluetooth /etc/lib/bluetooth
```

### 实验 4.6：看挂载和 overlay

```sh
cat /proc/mounts | head -20
cat /proc/mounts | grep -E 'overlay|UDISK|bluetooth'
df -h
```

重点看：

- `/` 的类型是不是 `overlay`。
- `/mnt/UDISK` 来自哪个 ubi 卷。
- `/etc/lib/bluetooth` 有没有 bind mount。

### 断电安全

- 只读 lower 不会因为掉电损坏。
- upper 和 UDISK 是 UBIFS，掉电可能丢最后几次写。
- 重要配置写完要 `sync`，或者用“写临时文件 + rename”保证原子性。

daemon 保存令牌时用临时文件再 rename 的思路，就是为了
避免“写到一半掉电，留下半个令牌”。

---

## 4.11 init 与 `S??` 脚本

### 启动顺序

```text
内核
 └─ /init
     └─ busybox init
         └─ /etc/init.d/rcS
             └─ rc.final
                 └─ for i in /etc/init.d/S??*; do $i start; done
```

文件名决定顺序：`S30` 在 `S52` 前，`S99` 最后。

### 一个最小服务脚本

```sh
#!/bin/sh
# /etc/init.d/S99hello

PIDFILE=/var/run/hello.pid
DAEMON=/usr/bin/hello

start() {
    [ -x "$DAEMON" ] || return 1
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        echo "hello: already running"
        return 0
    fi
    "$DAEMON" >/var/log/hello.log 2>&1 &
    echo $! >"$PIDFILE"
    echo "hello: started"
}

stop() {
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        kill "$(cat "$PIDFILE")"
        rm -f "$PIDFILE"
        echo "hello: stopped"
    fi
}

case "$1" in
    start)   start ;;
    stop)    stop ;;
    restart) stop; sleep 1; start ;;
    *)       echo "usage: $0 {start|stop|restart}"; exit 1 ;;
esac
```

```sh
chmod 755 /etc/init.d/S99hello
/etc/init.d/S99hello start
/etc/init.d/S99hello restart
/etc/init.d/S99hello stop
```

### 顺序错误会怎样

| 错误顺序 | 后果 |
|---|---|
| `S53linkr` 在 `S52e907` 前 | daemon 启动时没有 `/dev/rpmsgN`，只能重试 |
| `S54bluetooth` 在 Wi-Fi 固件前 | XR829 蓝牙 attach 失败 |
| `S30persist` 在账号服务后 | 登录用的是旧密码 |

> 改启动脚本前先画依赖图：**谁需要谁先就绪。**
> 不确定就加日志，不要靠猜。

### 板子上没有 `systemctl`

```sh
systemctl status linkr-bee     # 不存在
/etc/init.d/S53linkr status    # 取决于脚本是否实现 status
ps w | grep linkr-bee-daemon   # 最可靠
```

---

## 4.12 常用排查命令

```sh
# 进程和线程
ps w
ps w | grep -E 'linkr|bluetoothd|wpa'
ls /proc/$(pidof linkr-bee-daemon)/task | wc -l

# 打开的文件和端口
ls -l /proc/$(pidof linkr-bee-daemon)/fd | head
netstat -ltnp 2>/dev/null || ss -ltnp

# 谁占用了串口
fuser -v /dev/ttyS2

# 内存
free
cat /proc/meminfo | head

# 磁盘/挂载
df -h
cat /proc/mounts

# 内核日志
dmesg | tail -50

# 系统调用（如果板上有 strace）
strace -f -p $(pidof linkr-bee-daemon) 2>&1 | head -50
```

> `strace` 是理解“程序到底在等什么”的利器。没有的话，
> 用日志 + `/proc/<pid>/wchan` 也能看出阻塞在哪个内核函数。

---

## 4.13 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| `Address already in use` | 没设 `SO_REUSEADDR` 或旧进程没退 | `ss -ltnp`、`kill` 旧进程 |
| 服务起来又立刻退出 | 前台跑看 traceback | 手动执行 daemon |
| 日志为空 | stdout 缓冲（缺 `-u`） | 加 `-u` 或 `fflush` |
| 写文件后重启丢失 | 写到了 `/var` 或 overlay upper 被清 | `df`、`mount` |
| 刷机后配置没更新 | overlay upper 盖住新文件 | 检查 `/mnt/UDISK/overlay/upper` |
| 令牌文件权限 644 | `umask` 或创建后没 `chmod` | `stat -c '%a'` |
| 一个客户端卡住所有人 | 阻塞写没有超时 | 设 `SO_SNDTIMEO`，或改事件循环 |
| `EMSGSIZE` | 一次写超过 rpmsg MTU | 切 256 字节 |
| 线程数暴涨 | 每连接一线程 + 客户端不关 | 加超时、限制并发 |
| Python 报 `BrokenPipeError` | 对端已关闭还继续写 | 捕获并关闭连接 |

---

## 4.14 练习

### 练习 4.1（必做）

跑通 4.4 的 `echo_server.c`，然后用 `nc` 连两次，观察：

1. 第一个连接不断开时，第二个连接能进来吗？为什么？
2. 把 `listen(s, 4)` 改成 `listen(s, 1)`，行为有变化吗？

### 练习 4.2（必做）

给 `echo_server.c` 加多线程：每个 `accept` 后
`pthread_create` 一个线程处理连接。要求：

- 线程 detach，不需要 join。
- 客户端断开后线程退出。
- 用 `-pthread` 编译。

### 练习 4.3（必做）

写一个 `S99hello` 脚本，让它在板子启动时把
`date` 和 `uptime` 追加到 `/mnt/UDISK/hello.log`。
要求支持 `start`/`stop`/`restart`，并且重复 `start`
不会启动两份。

### 练习 4.4（选做）

用 `strace` 或日志观察 daemon 在以下时刻分别阻塞在哪个系统调用：

1. 没有客户端连接时。
2. 有客户端但没发数据时。
3. 正在往 rpmsg 写大量数据时。

### 练习 4.5（选做）

把 `SerialWriter` 的 `limit` 从 262144 改成 1024，
用网页粘贴 100 KB，观察：

- 丢弃日志多久出现一次。
- 网页是否变慢。
- `@i?` 的 `drop` 计数是否增加。

---

## 4.15 本章验收

1. 能说清进程和线程的区别，以及 daemon 里有哪些线程。
2. 能解释为什么 `write()` 可能只写一部分，并写出重试循环。
3. 能独立写出一个多线程 TCP echo 服务器。
4. 能解释 `SO_REUSEADDR`、`SO_SNDTIMEO`、`EINTR` 各解决什么问题。
5. 能说出 600 权限保护了什么，以及为什么私钥不能进镜像。
6. 能解释 overlayfs 的 lower/upper，以及“刷机后配置没更新”的原因。
7. 能写一个带 pidfile 的 `S99` 启动脚本并让它开机运行。

### 自测题

1. 为什么 `read(fd, buf, 100)` 可能只返回 10？
2. `accept()` 返回的 fd 和监听 fd 是什么关系？
3. 为什么条件变量要用 `while` 包住 `wait()`？
4. 为什么 `bridge.send()` 要放在锁外面？
5. `/var` 是 tmpfs 意味着什么？蓝牙绑定应该放哪？
6. `nohup python3 -u` 里的 `-u` 不加会怎样？
7. overlayfs 的 upper 在哪个分区？重刷 rootfs 会清掉它吗？

> 到这里，第 0 部分结束。你已经具备了读这份代码的工具和语言基础。
> 下一章开始进入硬件：电平、UART、波特率、FIFO。
