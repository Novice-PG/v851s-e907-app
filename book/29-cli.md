# 第 29 章 CLI 工具：能放进脚本的最小接口

> 本章目标：
> 1. 知道一个好 CLI 的“契约”：参数、输入、输出、退出码。
> 2. 会设计子命令、选项、默认值、`--help`、`--json`。
> 3. 理解 stdout/stderr 分离、管道、`set -e` 的关系。
> 4. 能写一个连板子的 CLI：读令牌、发命令、收串口、带超时。
> 5. 能排查“脚本里能跑、终端里不行”“退出码永远是 0”“输出被日志污染”。

---

## 29.1 CLI 是给别人用的接口

### 它和 TUI/网页的差别

```text
网页：人看着用
TUI：人在终端里交互
CLI：程序/脚本调用，也可以人用
```

CLI 的用户**可能是另一个程序**。所以：

- 输出必须是可解析的。
- 失败必须用退出码表达。
- 不能交互式提问（除非显式要求）。

### 一个 CLI 的四个契约

```text
1. 参数     argv 怎么解析、有哪些子命令
2. 输入     stdin、文件、网络
3. 输出     stdout（数据）、stderr（诊断）
4. 退出码   0 成功，非 0 失败（且不同值有意义）
```

> 只要这四条稳定，内部怎么实现都可以换。

---

## 29.2 命令长什么样

### 这块板需要的操作

```text
bee-cli status                    看连接/固件/波特率
bee-cli send "ls\n"               发一段字节
bee-cli recv --timeout 3          收一段字节
bee-cli term                      进入交互（类似 TUI 简化版）
bee-cli diag [--json]             拉 @i? 并解析
bee-cli uart get|set 115200,8,n,1,n
bee-cli token get|set|off
bee-cli wifi status|scan|connect
```

### 为什么用子命令而不是一堆独立程序

| 方案 | 好处 | 坏处 |
|---|---|---|
| `bee-cli status` | 共享连接/配置/依赖，`--help` 统一 | 一个二进制稍大 |
| `bee-status`、`bee-send`… | 每个很小 | 重复代码、安装麻烦 |

> 经验：**相关的操作放一个程序，用子命令区分。**
> 这条规则在 `git`、`docker`、`kubectl` 上都成立。

### 命名规则

```text
bee-cli <verb> [object] [options]
```

- `verb` 用原形：`get` 而不是 `gets`。
- 布尔开关用 `--flag` / `--no-flag`。
- 有值的用 `--name value` 或 `--name=value`。
- 位置参数只用于“主要对象”。

---

## 29.3 参数解析

### 不要自己写 `argv` 循环

除非只有两三个参数，否则用现成库：

| 语言 | 常用 |
|---|---|
| C | `getopt_long` |
| Python | `argparse` |
| Go | `flag` / `cobra` |
| Rust | `clap` |

### Python `argparse` 骨架

```python
import argparse
import sys

def build_parser():
    p = argparse.ArgumentParser(
        prog="bee-cli",
        description="Linkr Bee command line client",
    )
    p.add_argument("--host", default="192.168.0.121")
    p.add_argument("--port", type=int, default=8443)
    p.add_argument("--token-file", default="~/.config/bee/token")
    p.add_argument("--timeout", type=float, default=5.0)
    p.add_argument("--json", action="store_true", help="machine readable output")
    p.add_argument("-v", "--verbose", action="count", default=0)

    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")

    s = sub.add_parser("send")
    s.add_argument("text")
    s.add_argument("--no-newline", action="store_true")

    r = sub.add_parser("recv")
    r.add_argument("--timeout", type=float, default=3.0)

    d = sub.add_parser("diag")

    u = sub.add_parser("uart")
    u.add_argument("action", choices=["get", "set"])
    u.add_argument("value", nargs="?")

    return p

def main(argv):
    args = build_parser().parse_args(argv)
    ...
```

要点：

1. **`required=True`** 让漏写子命令直接报错。
2. **`type=int`/`float`** 让解析失败在入口就报错。
3. **`choices`** 让非法值报错并列出可选项。
4. **`default`** 让常见情况零参数可用。

### `--help` 是文档

```text
$ bee-cli --help
usage: bee-cli [-h] [--host HOST] [--port PORT] ... {status,send,recv,diag,uart} ...

Linkr Bee command line client

positional arguments:
  {status,send,recv,diag,uart}

options:
  -h, --help            show this help message and exit
  --host HOST
  --port PORT
  --json                machine readable output
```

> 用户第一个看的不是 README，是 `--help`。
> 帮助写清楚，能省掉一半支持成本。

### C 的 `getopt_long`

```c
static const struct option long_opts[] = {
    { "host",   required_argument, 0, 'H' },
    { "port",   required_argument, 0, 'P' },
    { "json",   no_argument,       0, 'j' },
    { "help",   no_argument,       0, 'h' },
    { 0, 0, 0, 0 },
};

while ((c = getopt_long(argc, argv, "H:P:jh", long_opts, NULL)) != -1) {
    switch (c) {
    case 'H': host = optarg; break;
    case 'P': port = atoi(optarg); break;
    case 'j': json = 1; break;
    case 'h': usage(stdout); return 0;
    default:  usage(stderr); return 2;
    }
}
```

注意：**`--help` 退出码是 0，用法错误退出码是 2。**

---

## 29.4 退出码

### 只用两个不够

```text
0    成功
1    一般错误
2    用法错误（参数不对）
3    连接失败
4    认证失败（令牌错）
5    超时
6    远端返回错误
```

为什么区分：

```sh
if bee-cli send "reboot\n"; then
    echo ok
else
    case $? in
      3) echo "板子不在线" ;;
      4) echo "令牌不对，检查 ~/.config/bee/token" ;;
      *) echo "其他错误" ;;
    esac
fi
```

### 约定

```text
0       成功
1       失败（通用）
2       命令行用法错误
126     不能执行（权限）
127     命令不存在
130     被 SIGINT 中止
```

这是 shell 的通用约定，尽量遵守。

### 不要用 `sys.exit(1)` 到处糊

```python
# 不好
try:
    do()
except Exception:
    sys.exit(1)

# 好
class CliError(Exception):
    def __init__(self, code, message):
        self.code = code
        self.message = message

try:
    do()
except ConnectionError as e:
    raise CliError(3, f"connect failed: {e}")
except AuthError as e:
    raise CliError(4, f"auth failed: {e}")
```

然后在 `main` 里统一：

```python
def main(argv):
    try:
        return run(argv)
    except CliError as e:
        print(f"bee-cli: {e.message}", file=sys.stderr)
        return e.code
    except KeyboardInterrupt:
        return 130

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

> 退出码是**程序语言**。别人靠它判断发生了什么，别乱用。

---

## 29.5 stdout / stderr 分离

### 铁律

```text
stdout  → 数据（可以被管道/重定向消费）
stderr  → 诊断（进度、警告、错误）
```

### 反例

```python
print("connecting...")      # 进了 stdout
print("connected")          # 进了 stdout
print(data)                 # 数据也进了 stdout
```

结果：

```sh
bee-cli recv > out.bin
# out.bin 里混进了 "connecting..." 和 "connected"
```

正确：

```python
print("connecting...", file=sys.stderr)
print("connected", file=sys.stderr)
sys.stdout.buffer.write(data)
```

### 二进制数据不要用 `print`

```python
# 错：会做编码转换
print(data)

# 对：原样写字节
sys.stdout.buffer.write(data)
sys.stdout.buffer.flush()
```

### `--json` 模式

给人看：

```text
target: 192.168.0.121
firmware: zephyr_e907 1.2.3
uart: 115200,8,n,1,n
rx: 0/65536  tx: 0/65536  drop: 0/0
```

给程序看：

```json
{"target":"192.168.0.121","firmware":"zephyr_e907 1.2.3",
 "uart":{"baud":115200,"data":8,"parity":"n","stop":1,"flow":"n"},
 "rx":{"used":0,"size":65536},"tx":{"used":0,"size":65536},
 "drop":{"rx":0,"tx":0}}
```

```sh
bee-cli diag --json | jq '.drop.rx'
```

> 同一个程序，两种输出。**不要为了 JSON 另写一个程序。**

### 日志级别

```sh
bee-cli -v diag        # INFO 到 stderr
bee-cli -vv diag       # DEBUG 到 stderr
```

`-v` 越多越啰嗦，默认安静：

```text
默认：只输出结果
-v  ：连接、握手、重连
-vv ：收发的每个帧（注意脱敏）
```

**`-vv` 打印令牌时必须脱敏**：

```text
token: 0123...cdef
```

---

## 29.6 连接与认证

### 读令牌

优先级：

```text
1. --token（命令行，不推荐，会进 shell history）
2. $BEE_TOKEN
3. --token-file（默认 ~/.config/bee/token）
```

```python
def load_token(args):
    if args.token:
        return args.token.strip()
    env = os.environ.get("BEE_TOKEN")
    if env:
        return env.strip()
    path = os.path.expanduser(args.token_file)
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read().strip()
    except FileNotFoundError:
        return ""
```

### 不做令牌就直接连

如果板子没设令牌，服务端发 `@ws auth=none`，客户端直接继续。

```python
def authenticate(ws, token, timeout=3.0):
    first = ws.recv_text(timeout=timeout)
    if first.startswith("@ws auth=none"):
        return
    if first.startswith("@ws auth=required"):
        if not token:
            raise CliError(4, "server requires a token")
        ws.send_text(token)
        reply = ws.recv_text(timeout=timeout)
        if not reply.startswith("@ws auth=ok"):
            raise CliError(4, "token rejected")
        return
    raise CliError(1, f"unexpected greeting: {first!r}")
```

### 为什么要等 `auth=required`

因为服务端**先说话**。客户端如果抢着发数据，可能被当成令牌，
或者被直接断开。

> 协议里“谁先说第一句”是明确规定的。**不要凭感觉抢话。**

### TLS

自签名证书：

```python
import ssl

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE      # 或加载板子的证书做 pinning
```

更好的做法——**证书固定**：

```python
ctx = ssl.create_default_context(cafile=os.path.expanduser("~/.config/bee/ca.pem"))
ctx.check_hostname = True
```

即：把板子的证书下载一份，作为这个板子专属的 CA。
这样既不用关闭校验，也不依赖公共 CA。

```sh
# 首次信任
openssl s_client -connect 192.168.0.121:8443 </dev/null 2>/dev/null \
  | openssl x509 > ~/.config/bee/ca.pem
```

> `CERT_NONE` 只适合“局域网内临时调试”。
> 长期用的工具应该做 pinning。

### 超时

**每一个网络操作都要有超时。**

```python
ws = websocket.create_connection(url, timeout=args.timeout,
                                 sslopt={"context": ctx})
ws.settimeout(args.timeout)
```

没有超时的脚本，在板子掉线时会永远挂着——在 CI 里就是“卡死”。

---

## 29.7 收发串口

### `send`

```python
def cmd_send(args, conn):
    payload = args.text.encode("utf-8")
    if not args.no_newline:
        payload += b"\n"
    conn.send_binary(payload)
    return 0
```

注意：

- 用二进制帧，不用文本帧（文本帧是控制命令）。
- 换行默认加，因为大多数 shell 需要。
- `--no-newline` 给需要精确控制的场景。

### `recv`

```python
def cmd_recv(args, conn):
    deadline = time.time() + args.timeout
    total = 0
    while time.time() < deadline:
        conn.settimeout(max(0.05, deadline - time.time()))
        try:
            data = conn.recv_binary()
        except TimeoutError:
            break
        if not data:
            break
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        total += len(data)
    return 0 if total else 5
```

两个设计点：

1. **超时到了返回 5**，而不是 0——这样脚本能知道“什么都没收到”。
2. **边收边写**，而不是攒到最后。这样 `| head -c 100` 能提前结束。

### 一个组合用法

```sh
bee-cli send "dmesg | tail -5\n" && bee-cli recv --timeout 2
```

问题：`send` 和 `recv` 是两次连接，中间可能丢数据。

更好的是**一个连接里做两件事**：

```sh
bee-cli exec "dmesg | tail -5" --timeout 3
```

或者用 stdin：

```sh
echo "dmesg | tail -5" | bee-cli term --batch --timeout 3
```

> 这是 CLI 设计里常见的坑：**把一次会话拆成多次调用，会丢状态。**

### `term`：交互模式

```python
def cmd_term(args, conn):
    import select
    stdin_fd = sys.stdin.fileno()

    while True:
        r, _, _ = select.select([stdin_fd, conn.sock], [], [], 0.5)

        if stdin_fd in r:
            data = os.read(stdin_fd, 4096)
            if not data:
                break
            conn.send_binary(data)

        if conn.sock in r:
            data = conn.recv_binary_nowait()
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
```

这个版本不做 raw mode，所以是“行模式交互”——回车才发出。
够用于简单调试；要真正的逐键交互，用第 28 章的 raw mode。

---

## 29.8 配置文件

### 为什么要配置文件

```sh
bee-cli --host 192.168.0.121 --port 8443 --token-file ~/.config/bee/token send "ls\n"
```

每次都写这么长不现实。

```ini
# ~/.config/bee/config
host = 192.168.0.121
port = 8443
token_file = ~/.config/bee/token
timeout = 5
```

优先级：

```text
命令行 > 环境变量 > 配置文件 > 内置默认
```

### Python 实现

```python
import configparser

def load_config(path):
    cfg = configparser.ConfigParser()
    cfg.read(os.path.expanduser(path))
    return cfg

def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("--host")
    p.add_argument("--port", type=int)
    ...
    args = p.parse_args()
    cfg = load_config("~/.config/bee/config")
    if args.host is None:
        args.host = cfg.get("default", "host", fallback="192.168.0.121")
    if args.port is None:
        args.port = cfg.getint("default", "port", fallback=8443)
    return args
```

关键：**不要把默认值直接写进 `add_argument`，否则无法区分
“用户显式传了默认值”和“用户没传”。** 用 `None` 做哨兵。

### 权限

配置文件里有主机名，令牌文件里有密钥：

```sh
chmod 700 ~/.config/bee
chmod 600 ~/.config/bee/token
```

> 令牌文件权限不对时，给出警告而不是静默接受。

---

## 29.9 实验：写一个能用的 `bee-cli`

### 第一步：最小可用

```python
#!/usr/bin/env python3
import argparse, ssl, sys, time
from websocket import create_connection   # 或自己实现

def main():
    p = argparse.ArgumentParser(prog="bee-cli")
    p.add_argument("--host", default="192.168.0.121")
    p.add_argument("--port", type=int, default=8443)
    p.add_argument("--insecure", action="store_true")
    p.add_argument("cmd", choices=["send", "recv"])
    p.add_argument("text", nargs="?")
    args = p.parse_args()

    ctx = ssl.create_default_context()
    if args.insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    url = f"wss://{args.host}:{args.port}/ws"
    ws = create_connection(url, timeout=5, sslopt={"context": ctx})

    if args.cmd == "send":
        if not args.text:
            print("bee-cli: send needs text", file=sys.stderr)
            return 2
        ws.send_binary(args.text.encode() + b"\n")
        return 0

    if args.cmd == "recv":
        deadline = time.time() + 3
        while time.time() < deadline:
            try:
                data = ws.recv()
            except Exception:
                break
            if isinstance(data, bytes):
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
        return 0

if __name__ == "__main__":
    sys.exit(main())
```

### 第二步：加上令牌

```python
ws.settimeout(3)
greeting = ws.recv()
if isinstance(greeting, str) and greeting.startswith("@ws auth=required"):
    token = open(os.path.expanduser("~/.config/bee/token")).read().strip()
    ws.send(token)
    ws.recv()       # @ws auth=ok
ws.settimeout(None)
```

### 第三步：加上退出码

对照 29.4 的表，把“连接失败”“认证失败”“超时”分开。

### 第四步：验证

```sh
bee-cli send "uname -a"
bee-cli recv > /tmp/out.txt
cat /tmp/out.txt

bee-cli recv > /dev/null 2>/tmp/err.txt
echo $?      # 没数据应该是 5

# 错误主机
bee-cli --host 192.168.0.250 send "x"; echo $?   # 3
```

### 第五步：加进脚本

```sh
#!/bin/sh
set -eu

if ! bee-cli send "cat /proc/uptime\n"; then
    echo "board unreachable" >&2
    exit 1
fi

bee-cli recv --timeout 2 | sed 's/ .*//' > /tmp/uptime
echo "uptime: $(cat /tmp/uptime)s"
```

---

## 29.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 脚本里输出混了日志 | 日志走了 stdout | 改 stderr |
| 退出码永远是 0 | 没检查返回值 | `set -e` 测试 |
| 参数少写就崩 | 没做参数校验 | `argparse`/`choices` |
| `--help` 退出码非 0 | 当成错误处理 | 单独返回 0 |
| 板子掉线时卡死 | 没超时 | 所有 I/O 都设超时 |
| 证书校验失败 | 自签名 | pinning 或 `--insecure` |
| 令牌进 shell history | `--token` 明文 | 用文件/环境变量 |
| `recv` 丢开头 | send/recv 两次连接 | 用一次会话 |
| 二进制被转码 | 用了 `print` | `sys.stdout.buffer` |
| `-vv` 泄露令牌 | 没脱敏 | 打印前过滤 |
| 管道提前关闭报错 | `SIGPIPE` 未处理 | 忽略或正常退出 |
| 配置改了不生效 | 优先级理解错 | 打印最终配置 |

### 关于 `SIGPIPE`

```sh
bee-cli recv | head -c 10
```

`head` 读够就关管道，`bee-cli` 再写会收到 `SIGPIPE`，默认行为是
直接死掉（退出码 141）。

想让“对方关了管道”变成正常结束：

```python
import signal
signal.signal(signal.SIGPIPE, signal.SIG_DFL)   # C/Python 默认
```

或者捕获 `BrokenPipeError` 并返回 0。**对 `| head` 这类用法，
静默退出更符合预期。**

---

## 29.11 练习

### 练习 29.1（必做）

写一个 `bee-cli`，至少支持：

```text
status
send <text>
recv --timeout N
--json
```

要求：

- 日志走 stderr。
- 连接失败返回 3，超时无数据返回 5。
- `--help` 返回 0。

### 练习 29.2（必做）

给 `bee-cli` 加令牌支持：

- 读 `~/.config/bee/token`。
- 支持 `$BEE_TOKEN`。
- 权限不是 600 时警告。

### 练习 29.3（必做）

回答：

1. 为什么数据走 stdout、诊断走 stderr？
2. 退出码 2 和 3 分别应该表示什么？
3. 为什么 `--help` 要返回 0？
4. 为什么 `send` 和 `recv` 分两次调用可能丢数据？
5. 自签名证书有哪两种处理方式？各自代价是什么？
6. `-vv` 打印令牌前要做什么？

### 练习 29.4（选做）

实现 `bee-cli diag --json`：发 `@i?`，解析回复，输出 JSON。
要求字段稳定，缺字段时给 `null` 而不是崩。

### 练习 29.5（选做）

把 `bee-cli` 接进 CI：

```sh
#!/bin/sh
set -eu
bee-cli diag --json | jq -e '.firmware != null'
```

思考：板子不在线时，CI 应该失败还是跳过？为什么？

---

## 29.12 本章验收

1. 能说出 CLI 的四个契约。
2. 会用子命令 + 选项组织功能，`--help` 写得清楚。
3. 能用有意义的退出码表达不同失败。
4. 严格分离 stdout 和 stderr，二进制原样输出。
5. 会做令牌读取、TLS 校验/固定、全链路超时。
6. 能排查输出污染、退出码错误、卡死、丢数据四类问题。

### 自测题

1. stdout 和 stderr 各放什么？
2. 用法错误用哪个退出码？
3. 令牌的三种来源，优先级是什么？
4. 为什么每个网络操作都要超时？
5. `send` 为什么用二进制帧？
6. `| head` 关闭管道后，CLI 应该怎样退出？

> 下一章把客户端搬到手机上：Android + BLE 的权限、扫描、GATT 与重连。
