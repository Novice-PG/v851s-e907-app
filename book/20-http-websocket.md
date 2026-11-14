# 第 20 章 从零写 HTTP 与 WebSocket

> 本章目标：
> 1. 看懂一个 HTTP/1.1 请求和响应到底长什么样。
> 2. 理解 WebSocket 升级握手的每个头。
> 3. 能自己实现 WebSocket 帧的解析和构造。
> 4. 知道二进制帧和文本帧分别用来干什么。
> 5. 能排查“升级失败”“帧解析错”“大帧被拒”“路径穿越”。

---

## 20.1 HTTP 就是“一行请求 + 若干头 + 空行”

### 客户端发什么

```text
GET /index.html HTTP/1.1\r\n
Host: 192.168.0.121:8443\r\n
User-Agent: Mozilla/5.0\r\n
Accept: text/html\r\n
\r\n
```

拆开：

```text
方法 路径 版本
头: 值
...
空行（\r\n）
```

- 方法：`GET`、`POST`、`PUT`、`DELETE` 等。
- 路径：包含查询串（`?a=b`）。
- 版本：这块板只处理 HTTP/1.1。
- 头：大小写不敏感。
- 空行之后是 body（`GET` 通常没有）。

### 服务器回什么

```text
HTTP/1.1 200 OK\r\n
Content-Type: text/html; charset=utf-8\r\n
Content-Length: 1234\r\n
Connection: close\r\n
Cache-Control: no-cache\r\n
\r\n
<1234 字节的 HTML>
```

关键头：

| 头 | 作用 |
|---|---|
| `Content-Type` | 告诉浏览器怎么解析 |
| `Content-Length` | body 长度 |
| `Connection: close` | 这条连接只服务一个请求 |
| `Cache-Control` | 缓存策略 |

### 最小服务器骨架

```python
def handle(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return
        data += chunk
        if len(data) > 65536:
            return                      # 防大请求头攻击

    head = data.split(b"\r\n\r\n", 1)[0].decode("latin-1")
    lines = head.split("\r\n")
    method, path, _version = lines[0].split()

    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()

    body = b"hello\n"
    resp = ("HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n" % len(body)).encode() + body
    conn.sendall(resp)
```

要点：

1. **一直读到 `\r\n\r\n`**，不能假设一次 `recv` 拿到完整头。
2. **限制总大小**，否则恶意客户端可以发无限头。
3. **header key 转小写**，方便查找。
4. **`Content-Length` 必须准确**，否则浏览器会等或截断。

> 用 `latin-1` 解码请求头：HTTP 头理论上可以是任意字节，
> `latin-1` 不会因为非法 UTF-8 抛异常。

---

## 20.2 静态文件服务与路径穿越

### 天真实现

```python
path = "/" + url_path.lstrip("/")
full = os.path.join(web_root, path)
open(full, "rb")
```

问题：请求 `/../../etc/passwd` 会读到系统文件。

### 正确实现

```python
root = os.path.realpath(web_root)
full = os.path.realpath(os.path.join(root, path.lstrip("/")))

# full 必须在 root 之内
if full != root and not full.startswith(root + os.sep):
    return 403

if not os.path.isfile(full):
    return 404

with open(full, "rb") as fp:
    body = fp.read()
```

三重防护：

1. `realpath` 解析 `..` 和符号链接。
2. 前缀检查：结果必须在 root 下。
3. `isfile` 检查：不服务目录/设备。

> 别用字符串 `startswith(root)` 而不加 `os.sep`：
> `/var/www-evil` 也会匹配 `/var/www`。加分隔符才安全。

### MIME 类型

```python
MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".woff2": "font/woff2",
    ".wasm": "application/wasm",
}
ct = MIME.get(ext, "application/octet-stream")
```

> `application/octet-stream` 是“我不知道，下载吧”。
> 如果 `.js` 的 MIME 错，浏览器会拒绝执行（尤其是 module）。

### 缓存

这块板用：

```text
Cache-Control: no-cache
```

因为固件/网页会随刷机更新。加长缓存会让用户看到旧页面。

---

## 20.3 WebSocket 升级握手

### 客户端请求

```text
GET /ws HTTP/1.1\r\n
Host: 192.168.0.121\r\n
Upgrade: websocket\r\n
Connection: Upgrade\r\n
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n
Sec-WebSocket-Version: 13\r\n
\r\n
```

关键头：

| 头 | 值 |
|---|---|
| `Upgrade` | `websocket`（大小写不敏感） |
| `Connection` | 包含 `Upgrade` |
| `Sec-WebSocket-Key` | 客户端随机 16 字节的 base64 |
| `Sec-WebSocket-Version` | `13` |

### 服务器响应

```text
HTTP/1.1 101 Switching Protocols\r\n
Upgrade: websocket\r\n
Connection: Upgrade\r\n
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n
\r\n
```

### `Sec-WebSocket-Accept` 怎么算

```python
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
accept = base64.b64encode(
    hashlib.sha1((key + GUID).encode("ascii")).digest()
).decode("ascii")
```

即：

```text
accept = base64(sha1(client_key + GUID))
```

这个 GUID 是 RFC 6455 写死的，用来证明“服务器真的懂 WebSocket”，
而不是普通 HTTP 服务器误回 101。

### 最小握手实现

```python
if route == "/ws" and headers.get("upgrade", "").lower() == "websocket":
    key = headers.get("sec-websocket-key", "")
    accept = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    ).decode("ascii")

    conn.sendall((
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n" % accept
    ).encode("ascii"))
    # 从这里开始是 WebSocket 帧，不再是 HTTP
```

> 握手成功后，**同一条 TCP 连接**变成双向帧通道。
> 不要再发 HTTP 头，也不要用 `Content-Length`。

---

## 20.4 WebSocket 帧格式

### 结构

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+-------------------------------+
|     Extended payload length continued, if payload len == 127  |
+-------------------------------+-------------------------------+
|                               |Masking-key, if MASK set to 1  |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------- - - - - - - - - - - - - - - -+
```

### 逐字段

| 字段 | 位 | 含义 |
|---|---|---|
| FIN | 1 | 是否最后一帧 |
| RSV1-3 | 3 | 扩展用，通常 0 |
| opcode | 4 | 帧类型 |
| MASK | 1 | 客户端→服务器必须 1 |
| payload len | 7 | 0..125 直接；126/127 表示后续长度 |
| masking key | 32 | 掩码，客户端发时必须 |
| payload | N | 数据 |

### opcode

| 值 | 名称 | 含义 |
|---|---|---|
| `0x0` | continuation | 分片的后续帧 |
| `0x1` | text | UTF-8 文本 |
| `0x2` | binary | 二进制 |
| `0x8` | close | 关闭 |
| `0x9` | ping | 心跳 |
| `0xA` | pong | 心跳回复 |

### 掩码

**客户端发给服务器的帧必须加掩码**（MASK=1）。服务器发给客户端
的帧**不能加掩码**。

掩码算法：

```python
data = bytes(byte ^ mask[i % 4] for i, byte in enumerate(data))
```

解密和加密是同一个操作（异或）。

> 为什么客户端要掩码？防止中间代理缓存污染。服务器不加掩码
> 是因为浏览器能正确处理。

### 长度编码

```text
len <= 125          → 7 位直接放
126 <= len <= 65535 → 7 位写 126，后跟 2 字节大端
len > 65535         → 7 位写 127，后跟 8 字节大端
```

### 解析实现

```python
def ws_recv_message(sock):
    fragments = b""
    first_opcode = None
    while True:
        header = recv_exact(sock, 2)
        fin = header[0] & 0x80
        opcode = header[0] & 0x0F
        masked = header[1] & 0x80
        length = header[1] & 0x7F

        if length == 126:
            length = struct.unpack("!H", recv_exact(sock, 2))[0]
        elif length == 127:
            length = struct.unpack("!Q", recv_exact(sock, 8))[0]
        if length > MAX_FRAME:
            raise ValueError("frame too large")

        mask = recv_exact(sock, 4) if masked else b""
        data = recv_exact(sock, length) if length else b""
        if masked:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))

        if opcode == 0x0:                 # 分片后续
            fragments += data
            if fin:
                return first_opcode or 0x1, fragments
        elif opcode in (0x1, 0x2):
            if fin:
                return opcode, data
            first_opcode = opcode
            fragments = data
        elif opcode in (0x8, 0x9, 0xA):
            return opcode, data
        else:
            raise ValueError("unsupported opcode")
```

要点：

1. **`recv_exact` 必须循环**。`sock.recv(n)` 可能只返回一部分。
2. **限制 `MAX_FRAME`**（这块板 1 MiB），防止内存耗尽。
3. **处理分片**：`FIN=0` 的帧要拼起来，直到 `FIN=1`。
4. **ping/pong/close 单独返回**，由上层处理。

### 构造实现

```python
def send_frame(sock, opcode, payload):
    header = bytes([0x80 | opcode])       # FIN=1
    n = len(payload)
    if n < 126:
        header += bytes([n])
    elif n < 65536:
        header += bytes([126]) + struct.pack("!H", n)
    else:
        header += bytes([127]) + struct.pack("!Q", n)
    sock.sendall(header + payload)
```

服务器不加掩码（MASK=0），所以第二个字节最高位是 0。

---

## 20.5 这块板的 WebSocket 用法

### 二进制帧 = 串口字节

```text
浏览器 → /ws:  0x02 帧，payload 是用户输入/粘贴的字节
/ws → 浏览器:  0x02 帧，payload 是目标板串口输出
```

为什么用二进制而不是文本？

- 串口是任意字节，不保证 UTF-8。
- 文本帧要求 UTF-8，二进制字节会损坏。
- 二进制帧不做编码，原样传输。

### 文本帧 = 控制命令

```text
@c?
@c claim
@i?
@u?
@w scan
@s token
```

daemon 收到文本帧后交给 `session.handle_text`，返回文本回复。

### 心跳

```python
if opcode == 0x9:          # ping
    send_frame(0xA, data)  # pong
    continue
if opcode == 0xA:          # pong
    continue
```

浏览器/代理可能发 ping。不回 pong，连接会被判定死亡。

### 关闭

```python
if opcode == 0x8:          # close
    break
```

收到 close 就退出循环，关闭 socket。

---

## 20.6 认证：第一帧发令牌

### 为什么不用 HTTP Header

浏览器的 `WebSocket` API **不能自定义请求头**：

```js
new WebSocket("ws://host/ws");   // 无法加 Authorization
```

所以这块板用“连接后第一帧文本”传令牌。

### 服务端流程

```python
token = self.session.ws_token
if token:
    self.send_text("@ws auth=required\r\n")
    self.conn.settimeout(3.0)
    opcode, data = ws_recv_message(self.conn)
    supplied = data.decode("utf-8", "replace").strip().lower()
    if opcode != 0x1 or supplied != token:
        return                      # 直接断开
    self.conn.settimeout(None)
    self.send_text("@ws auth=ok\r\n")
else:
    self.send_text("@ws auth=none\r\n")
```

### 客户端流程（JS 示意）

```js
const ws = new WebSocket("wss://board:8443/ws");
ws.onmessage = (e) => {
  if (e.data.startsWith("@ws auth=required")) {
    ws.send(token);
  }
};
```

### 安全边界

- 明文 `ws://` 下，令牌在局域网里是明文。
- `wss://` 下才加密。
- 令牌是 32 位十六进制（128 位随机）。
- 存 UDISK，权限 600。

> 这不是“强认证”，是“防止邻居随手连上你的串口”。
> 真正的安全要靠 HTTPS + 网络隔离 + 强密码。

---

## 20.7 实验：用 `curl` 和 Python 验证

### 实验 A：看 HTTP 响应头

```sh
curl -sI http://<board-ip>/
```

预期：

```text
HTTP/1.1 301 Moved Permanently
Location: https://<board-ip>:8443/
```

### 实验 B：看静态文件

```sh
curl -s https://<board-ip>:8443/ -k | head -20
```

### 实验 C：用 Python 做 WebSocket 客户端

```python
import socket, base64, os, struct, hashlib

host, port = "192.168.0.121", 80
key = base64.b64encode(os.urandom(16)).decode()
s = socket.create_connection((host, port))
s.sendall((
    "GET /ws HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n" % (host, key)
).encode())

resp = s.recv(4096)
print(resp.decode(errors="replace"))
```

应该看到 `101 Switching Protocols`。

### 实验 D：读第一帧

接上面：

```python
hdr = s.recv(2)
fin = hdr[0] & 0x80
op = hdr[0] & 0x0F
masked = hdr[1] & 0x80
n = hdr[1] & 0x7F
if n == 126:
    n = struct.unpack("!H", s.recv(2))[0]
payload = s.recv(n)
print(op, payload)
```

应该收到 `@ws auth=...`。

---

## 20.8 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 握手 400/404 | 路径不是 `/ws`、没 `Upgrade` | 请求头 |
| 握手 101 但连不上 | `Sec-WebSocket-Accept` 算错 | 公式/密钥 |
| 帧解析错 | 没循环 `recv`、长度编码错 | 抓包/日志 |
| 大帧被拒 | `MAX_FRAME` 太小 | 调大或分片 |
| 浏览器报 1006 | 没回 pong/连接被关 | 心跳逻辑 |
| 中文乱码 | 用文本帧传二进制 | 改二进制帧 |
| 路径穿越 | 没 `realpath` 检查 | 加前缀校验 |
| `.js` 不执行 | MIME 错 | `Content-Type` |
| 令牌不生效 | 大小写/换行 | 去空白、比较小写 |
| 明文令牌泄露 | 用 `ws://` | 改 `wss://` |

---

## 20.9 练习

### 练习 20.1（必做）

用 Python 写一个最小 HTTP 服务器，返回一个 HTML 页面。要求：

- 处理 `GET /`。
- 其他路径 404。
- 限制请求头 64 KiB。

### 练习 20.2（必做）

在 20.1 基础上加 WebSocket 升级：

- 校验 `Upgrade`/`Connection`。
- 计算 `Sec-WebSocket-Accept`。
- 连上后发一条文本帧。

### 练习 20.3（必做）

实现 `ws_recv_message`，要求：

- 支持 125/126/127 三种长度。
- 支持客户端掩码。
- 支持分片。
- 拒绝超过 1 MiB 的帧。

### 练习 20.4（选做）

给静态服务器加路径穿越防护，并写测试：

```text
/../etc/passwd   → 403
//etc/passwd     → 404/403
/%2e%2e/etc/passwd → 403
```

---

## 20.10 本章验收

1. 能写出 HTTP/1.1 请求和响应的最小结构。
2. 能解释 WebSocket 握手的每个头，并手算 Accept。
3. 能解析和构造 WebSocket 帧（含掩码、长度、分片）。
4. 知道二进制帧传串口、文本帧传控制命令。
5. 能实现路径穿越防护和帧大小限制。
6. 能解释为什么令牌走第一帧文本。

### 自测题

1. `Sec-WebSocket-Accept` 怎么算？
2. 客户端→服务器的帧为什么必须加掩码？
3. 126 和 127 分别表示什么长度？
4. 为什么串口数据用二进制帧？
5. `recv(n)` 为什么不保证返回 n 字节？
6. `realpath` 在防路径穿越里起什么作用？

> 下一章讲 HTTPS、证书和令牌，把明文连接变成加密连接。
