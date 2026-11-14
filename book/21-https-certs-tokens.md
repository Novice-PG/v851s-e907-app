# 第 21 章 HTTPS、证书与令牌

> 本章目标：
> 1. 理解 TLS 在做什么，以及为什么浏览器强制要求它。
> 2. 会生成带 SAN 的自签名证书，并把私钥放在正确的位置。
> 3. 知道证书、私钥、令牌三者的区别和存储要求。
> 4. 能看懂 Python `ssl` 的服务器端用法。
> 5. 能排查“证书不受信”“私钥没权限”“时间不对”“Web Bluetooth 不可用”。

---

## 21.1 为什么必须上 HTTPS

### 原因一：Web Bluetooth 只在安全上下文可用

浏览器把“安全上下文（secure context）”定义为：

- `https://`
- `http://localhost`
- `file://`（部分）

在 `http://192.168.0.121/` 里：

```js
navigator.bluetooth   // undefined
```

网页会报：

```text
Bluetooth LE transport is not available
```

所以主入口是：

```text
https://<board>:8443/
```

### 原因二：明文会被同网段看到

`ws://` 下：

- 串口数据明文。
- LAN 令牌明文。
- 会话 cookie/密码明文。

在家庭/教室网络里，这不是理论风险。

### 原因三：浏览器会对“混合内容”报错

HTTPS 页面里不能加载 `ws://`。必须用 `wss://`。

---

## 21.2 TLS 在做什么（够用版）

### 握手目标

```text
1. 客户端确认“服务器确实是它声称的那台”
2. 双方协商出一个只有彼此知道的会话密钥
3. 之后所有数据用这个密钥加密
```

### 关键概念

| 概念 | 作用 |
|---|---|
| 证书（certificate） | 服务器身份 + 公钥，由 CA 签名 |
| 私钥（private key） | 只有服务器有，用来证明身份 |
| CA | 证书签发机构 |
| 自签名 | 自己给自己签，浏览器默认不信任 |
| SAN | Subject Alternative Name，证书覆盖哪些域名/IP |
| 握手 | 协商密钥、验证证书 |

### 自签名意味着什么

- 浏览器第一次会警告：“连接不安全”。
- 用户手动接受后，之后正常。
- 它提供**加密**，但不提供**身份可信**。

> 对家用/教学场景，“加密 + 手动接受一次”是可接受的。
> 对生产环境，应该用受信 CA 或企业内网 CA。

---

## 21.3 生成证书

### 用 openssl 一条命令

```sh
mkdir -p /mnt/UDISK/persist/certs
cd /mnt/UDISK/persist/certs

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout linkr-bee.key \
  -out linkr-bee.crt \
  -days 3650 \
  -subj "/CN=Linkr Bee V851S/O=Linkr Bee" \
  -addext "subjectAltName=DNS:linkr-bee.local,DNS:localhost,IP:127.0.0.1,IP:192.168.0.121"

chmod 600 linkr-bee.key
chmod 644 linkr-bee.crt
```

参数：

| 参数 | 含义 |
|---|---|
| `-x509` | 直接生成自签名证书，不经过 CSR/CA |
| `-newkey rsa:2048` | 生成 2048 位 RSA 密钥 |
| `-nodes` | 私钥不加密码（服务启动要自动读） |
| `-days 3650` | 10 年有效期 |
| `-subj` | 主题（CN/O） |
| `-addext subjectAltName` | SAN 列表 |

### 为什么必须要 SAN

现代浏览器**不看 CN**，只看 SAN。没有 SAN 的证书会报：

```text
NET::ERR_CERT_COMMON_NAME_INVALID
```

SAN 里要包含：

- 板子的 IP（你现在用的那个）。
- `localhost`（本机测试）。
- `linkr-bee.local`（mDNS，如果支持）。

### 用配置文件（可复用）

`linkr-bee/a7/certs/san.cnf`：

```ini
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no

[dn]
CN = Linkr Bee V851S
O = Linkr Bee

[v3]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt

[alt]
DNS.1 = linkr-bee.local
DNS.2 = localhost
IP.1 = 127.0.0.1
IP.2 = 192.168.0.121
```

生成：

```sh
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout linkr-bee.key -out linkr-bee.crt \
  -days 3650 -config san.cnf
```

> 换 IP 时要重新生成，或在 SAN 里多写几个常用地址。

### 检查证书

```sh
openssl x509 -in linkr-bee.crt -noout -text | grep -A2 'Subject Alternative Name'
openssl x509 -in linkr-bee.crt -noout -dates
```

### 检查私钥和证书是否配对

```sh
openssl x509 -in linkr-bee.crt -noout -pubkey | openssl sha256
openssl pkey -in linkr-bee.key -pubout | openssl sha256
```

两个哈希必须一样。不一样就是配错文件了，握手会失败。

---

## 21.4 私钥放哪里

### 三条铁律

1. **不进镜像。**
2. **不进 git。**
3. **权限 600。**

### 位置优先级

```text
1. /mnt/UDISK/persist/certs/    ← 每板一份，刷机保留
2. /usr/share/linkr-bee/certs/  ← 只读默认（通常为空）
```

`S53linkr`：

```sh
CERT_DIR=/usr/share/linkr-bee/certs
if [ -f /mnt/UDISK/persist/certs/linkr-bee.crt ] &&
   [ -f /mnt/UDISK/persist/certs/linkr-bee.key ]; then
    CERT_DIR=/mnt/UDISK/persist/certs
fi
```

### 为什么不能进镜像

镜像会被：

- 复制到多块板。
- 上传到发布页。
- 被任何人解压。

私钥进镜像 = 所有板共用同一把私钥 = 一台泄露，全部沦陷。

> 每板生成一次，是最低要求。

### 权限检查

```sh
ls -l /mnt/UDISK/persist/certs/
```

期望：

```text
-rw------- 1 root root ... linkr-bee.key
-rw-r--r-- 1 root root ... linkr-bee.crt
```

如果 key 是 `644`，同板其他用户（如果有）能读走私钥。

---

## 21.5 Python 服务器端 TLS

### 最小实现

```python
import ssl, socket

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain("linkr-bee.crt", "linkr-bee.key")

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("0.0.0.0", 8443))
server.listen(16)

while True:
    conn, addr = server.accept()
    try:
        conn = ctx.wrap_socket(conn, server_side=True)
        # 从这里开始，conn 是加密的
        handle(conn)
    except ssl.SSLError as exc:
        print("handshake failed:", exc)
        conn.close()
```

### 为什么握手要放线程里

```python
threading.Thread(target=_tls_connection,
                 args=(ctx, conn, addr, ...),
                 daemon=True).start()
```

如果客户端连上但不发数据，`wrap_socket` 会卡住。放在主
accept 循环里，会阻塞**所有**后续 HTTPS 连接。

这块板的注释写得很清楚：

> “The handshake runs in its own thread: a client that connects and
> sends nothing must not stall the accept loop.”

### 超时

```python
conn.settimeout(10.0)
conn = ctx.wrap_socket(conn, server_side=True)
conn.settimeout(None)
```

握手最多等 10 秒，成功后取消超时（长连接需要）。

### 发送超时

```python
conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO,
                struct.pack("ll", 10, 0))
```

为什么？`broadcast()` 在 rpmsg RX 线程上跑。如果某个客户端
不读数据，`sendall` 会无限阻塞，**整个串口转发都停住**。

10 秒发送超时是“慢客户端保护”。

---

## 21.6 HTTP → HTTPS 重定向

### 逻辑

```python
if (redirect_https and route != "/ws" and
        headers.get("x-forwarded-proto", "").lower() != "https"):
    host = headers.get("host", "").split(":")[0]
    location = "https://%s:%d%s" % (host, tls_port, path)
    # 301
```

### 三条规则

1. **`/ws` 不重定向**：已有 LAN 工具用明文 `ws://` 工作。
2. **HTTPS 连接不再重定向**：否则无限循环。
3. **`X-Forwarded-Proto: https` 不重定向**：反向代理已终止 TLS。

### 为什么 `/ws` 例外

如果强制 `/ws` 跳 HTTPS：

- 老的脚本/工具会坏。
- 有些环境没装证书，只有 HTTP。

代价：明文 `ws://` 下串口数据和令牌不加密。用户应主动用
`wss://`。

> 工程取舍：**兼容性 vs 安全性**。这里选了兼容，但在文档里
> 明确提示风险。

---

## 21.7 令牌：LAN 的“门禁”

### 它是什么

- 32 位十六进制字符串（128 位随机）。
- 存在 `/mnt/UDISK/persist/ws-token`，权限 600。
- WebSocket 连接后第一帧文本发送。
- 不在令牌列表里就断开。

### 生成

通过网页/WS 发：

```text
@s token
```

daemon 生成随机值并保存。或者指定：

```text
@s token=0123456789abcdef0123456789abcdef
```

关闭：

```text
@s token off
```

查询：

```text
@s?
```

### 服务端校验

```python
token = self.session.ws_token
if token:
    self.send_text("@ws auth=required\r\n")
    self.conn.settimeout(3.0)
    opcode, data = ws_recv_message(self.conn)
    supplied = data.decode("utf-8", "replace").strip().lower()
    if opcode != 0x1 or supplied != token:
        return
    self.send_text("@ws auth=ok\r\n")
```

注意：

- 3 秒超时。
- 只接受文本帧。
- 比较前 `strip().lower()`。

### 令牌 vs 证书

| | 证书 | 令牌 |
|---|---|---|
| 保护什么 | 传输加密 + 服务器身份 | 谁可以连 |
| 存哪 | `certs/` | `ws-token` |
| 泄露后果 | 可解密流量 | 可控制串口 |
| 是否需要 | Web Bluetooth 必需 | 建议开启 |

> 两者解决不同问题：证书防“路上被看”，令牌防“邻居乱连”。

---

## 21.8 实验：验证 TLS

### 实验 A：生成并部署证书

```sh
# 主机上生成（或板子上）
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout linkr-bee.key -out linkr-bee.crt -days 3650 \
  -subj "/CN=Linkr Bee V851S" \
  -addext "subjectAltName=DNS:localhost,IP:192.168.0.121"

scp -O linkr-bee.crt linkr-bee.key root@<board>:/mnt/UDISK/persist/certs/
ssh root@<board> 'chmod 600 /mnt/UDISK/persist/certs/linkr-bee.key'
ssh root@<board> '/etc/init.d/S53linkr restart'
```

### 实验 B：看是否监听 8443

```sh
ss -ltnp | grep 8443
```

### 实验 C：curl 验证

```sh
curl -k -v https://<board-ip>:8443/ 2>&1 | head -30
```

看：

- `SSL certificate verify result`（`-k` 会跳过）。
- `HTTP/1.1 200 OK`。

### 实验 D：看证书内容

```sh
echo | openssl s_client -connect <board-ip>:8443 -servername <board-ip> 2>/dev/null \
  | openssl x509 -noout -subject -dates -ext subjectAltName
```

### 实验 E：令牌

```sh
# 网页终端
@s token
@s?

# 断开重连，观察第一帧
```

---

## 21.9 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 8443 连不上 | 没证书/没监听 | `ss -ltnp`、日志 |
| 证书警告 | 自签名 | 正常，手动接受 |
| `ERR_CERT_COMMON_NAME_INVALID` | SAN 缺当前 IP | 重新生成 |
| `ERR_CERT_DATE_INVALID` | 时间是 1970 | NTP、`date` |
| 握手失败 | key 和 crt 不配对 | 公钥哈希对比 |
| 权限错误 | key 不是 600 | `ls -l` |
| 页面能开但蓝牙不可用 | 用的 HTTP | 换 `https://` |
| 重定向循环 | 代理头没识别 | `X-Forwarded-Proto` |
| 慢客户端拖死转发 | 没发送超时 | `SO_SNDTIMEO` |
| 令牌泄露 | 明文 `ws://` | 用 `wss://` |
| 私钥进了镜像 | 打包时复制了 certs | 检查 rootfs |

---

## 21.10 练习

### 练习 21.1（必做）

生成一份自签名证书，要求：

- SAN 包含 `localhost`、`127.0.0.1`、你的板子 IP。
- 有效期 3650 天。
- 私钥 600。

用 `openssl x509 -text` 验证 SAN。

### 练习 21.2（必做）

回答：

1. 为什么浏览器不信任自签名证书？
2. 为什么现代浏览器只看 SAN 不看 CN？
3. 私钥为什么不能进镜像？
4. 为什么 TLS 握手要放单独线程？
5. 为什么 `broadcast` 需要发送超时？
6. 证书和令牌分别防什么？

### 练习 21.3（必做）

用 `openssl s_client` 连接你的板子，导出证书，检查：

- 主题。
- 有效期。
- SAN。
- 公钥算法。

### 练习 21.4（选做）

把 SAN 里故意漏掉板子当前 IP，观察浏览器报什么错，然后
补上重新生成。记录错误码。

---

## 21.11 本章验收

1. 能解释 TLS 握手的目标，以及自签名的含义。
2. 能生成带 SAN 的证书并验证。
3. 知道证书、私钥、令牌的存储位置和权限。
4. 能写出最小 Python TLS 服务器。
5. 能解释重定向规则和 `/ws` 例外。
6. 能排查证书、时间、权限、蓝牙不可用四类问题。

### 自测题

1. 安全上下文包括哪些？
2. SAN 缺失会报什么错？
3. 私钥权限应该是多少？
4. `wrap_socket` 为什么要设超时？
5. 为什么 `/ws` 不强制跳 HTTPS？
6. 令牌存在哪里、多长？

> 下一章进入 BLE：BlueZ、GATT、广播与 MTU。
