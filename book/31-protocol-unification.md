# 第 31 章 协议统一：一套 `@` 命令服务四种端

> 本章目标：
> 1. 知道为什么“每种端一套协议”是维护灾难。
> 2. 理解三层协议：字节流、命令流、事件流。
> 3. 看懂 daemon 的 `Session` / `Client` / `BleClient` 如何把
>    WebSocket、BLE、CLI、TUI 收敛到同一套语义。
> 4. 理解会话所有权（owner）、epoch、yield、release 的真实语义。
> 5. 能设计并实现“新增一种传输不用改业务逻辑”的结构。

---

## 31.1 四种端，一条串口

### 现状

```text
浏览器 ──ws(s)──┐
手机 App ─BLE───┤
CLI ──ws(s)─────┼──► linkr-bee-daemon ──rpmsg──► E907 ──UART3──► 目标板
TUI ──ws(s)─────┘
```

一条串口，多个使用者。如果每种端有自己的协议：

```text
浏览器：JSON {cmd: "uart", baud: 115200}
手机：二进制 0x11 0x01 ... 
CLI：--uart-baud 115200
TUI：直接改寄存器？
```

后果：

```text
- 后端要维护四套解析
- 文档要写四份
- 新功能要改四处
- 四种端行为不一致
```

### 统一后的结构

```text
            ┌──────────────┐
四种传输 ───►│  传输适配层  │  只负责“把字节搬进搬出”
            └──────┬───────┘
                   ▼
            ┌──────────────┐
            │  会话与命令  │  一套 @ 命令 + owner 管理
            └──────┬───────┘
                   ▼
            ┌──────────────┐
            │  rpmsg 传输  │  LINKRCFG/LINKRSTAT + 字节
            └──────┬───────┘
                   ▼
                  E907 ── UART3
```

> 核心思想：**传输是“怎么到”，协议是“到了说什么”。**
> 两者必须分离。

---

## 31.2 三层协议

### 第一层：字节流

```text
浏览器 ↔ daemon    WebSocket 二进制帧
手机 ↔ linkr-ble   ATT Write / Notification
daemon ↔ E907      rpmsg 消息（≤ chunk）
```

这一层只关心“这些字节”，不解释内容。

### 第二层：命令流

```text
以 @ 开头的一行文本
```

```text
@c?                 查询控制权
@c claim            申请
@c release          释放
@i?                 诊断
@u? / @u=...        UART 参数
@w? / @w scan / @w= / @w off
@s? / @s token ...  LAN 令牌
@n? / @n=...        BLE 名
@d? / @d=...        WebDAV
@h                  帮助
```

这一层是**纯文本、按行**。好处：

- 人能直接敲。
- 脚本能直接拼。
- 抓包能直接看。
- 任何传输只要能传字节，就能传命令。

### 第三层：事件流

```text
@event wifi operation=7 phase=queued ...
@scan result MyHome WPA2 ch=6 -45dBm
@scan done
@info fw version=... 
```

事件是**异步**的：命令发出后，结果可能分多帧回来，也可能很久
才回来（Wi-Fi 扫描要几秒）。

### 为什么事件不用请求-响应

因为：

```text
Wi-Fi 扫描可能 10 秒
串口数据随时到来
多个客户端可能同时在看
```

如果阻塞等，整个会话就卡住了。所以：

```text
命令 → 立刻返回（或返回“已受理”）
结果 → 通过事件帧推送
```

> 这是所有异步系统的通用模式：**请求与结果解耦。**

---

## 31.3 `Session`：唯一的真相源

### 它持有什么

```python
class Session:
    def __init__(...):
        self.clients = {}          # id → Client
        self.next_id = 1
        self.lock = threading.RLock()

        self.owner = 0             # 当前控制者
        self.owner_active = 0.0    # 最后一次写入时间
        self.epoch = 1             # 所有权变更计数

        self.managed = set()       # 主动管理过的 LAN 客户端
        self.ws_token = ...        # LAN 令牌
        self.webdav_url = ...      # 上传目标
        self.bond_names = ...      # BLE 绑定备注
```

关键设计：**所有共享状态都在 `Session` 里，`Client` 只是“一个连接”。**

```text
Session = 房间
Client  = 房间里的一个人
```

### `Client` 的接口

```python
class Client:
    transport = "ws"        # 或 "ble"

    def send_binary(self, data): ...   # 串口字节
    def send_text(self, text): ...     # 控制回复/事件
    def send_event(self, reqid, flags, text): ...
```

**所有传输只要实现这三个方法**，就能接入。

### `BleClient` 的差异

```python
class BleClient:
    transport = "ble"

    def send_event(self, reqid, flags, text):
        # 包成 Management 的 12 字节头
        ...
```

WebSocket 的 `send_event` 直接发文本帧；BLE 的要加头、要排队、
要分片。**差异被限制在一个方法里。**

### 注册与注销

```python
def register(self, client):
    with self.lock:
        client.id = self.next_id
        self.next_id += 1
        self.clients[client.id] = client
        return client.id

def unregister(self, client):
    with self.lock:
        self.clients.pop(client.id, None)
        self.managed.discard(client.id)
        if self.owner == client.id:
            self.owner = 0
            self.bump_epoch()
```

注意最后三行：

```text
owner 走了 → owner 清零 → epoch+1
```

否则“所有者”会是一个不存在的 id，其他客户端永远拿不到控制权。

> 清理逻辑不只是“从字典删掉”，还要修复所有引用它的状态。

---

## 31.4 会话所有权：为什么需要

### 问题

两个浏览器同时往串口敲字：

```text
用户 A 输入 "ls"
用户 B 输入 "cat /etc/passwd"
实际发送 "lcat /etc/passwds"   ← 互相插字
```

串口是**字符设备**，没有“消息边界”。两个人同时写，就是乱码。

### 方案：一个 owner

```text
同一时刻只有 owner 能写
其他人只读
```

规则：

```text
1. 第一个写的客户端自动成为 owner
2. 显式 @c claim 可以抢
3. owner 3 秒不写，别人可以接管（claim idle）
4. @c release 主动释放，且不会被自动重新 claim
5. @c yield <id> 把控制权交给指定 peer
```

### `claim idle`：为什么是 3 秒

```text
0 秒：A 正在输入
1 秒：A 停顿
3 秒：B 按了回车 → 接管
```

3 秒太短，会抢走正在思考的人；太长，会让别人等太久。

```python
OWNER_IDLE_SECONDS = 3.0
```

> 这是一个**产品参数**，不是技术参数。改它要问“用户觉得多久算离开”。

### `release` 的特殊语义

```python
elif action == "release":
    if self.owner != client.id:
        return "ERR control=not_owner\r\n"
    self.owner = 0
    self.bump_epoch()
```

同时：

```python
self.managed.add(client.id)     # 仅 LAN
```

`managed` 的作用：**标记“这个客户端主动管理过控制权”**，于是
不会被“自动 claim”重新抓回去。

```python
if not self.owner:
    if client.id in self.managed and not shared_ble:
        log("serial write from %d dropped: released, owner=0")
        return
    self.set_owner(client.id)
```

即：

```text
release 后 → 再写也不会自动变成 owner
要重新获得 → 必须显式 @c claim
```

> 这是一个细节但重要的语义：**“我放开了”和“我从没拿过”不一样。**

### BLE 为什么特殊

```python
shared_ble = getattr(client, "transport", "") == "ble"
```

因为 `linkr-ble` 把**所有 BLE 主机复用到一个 Unix socket**：

```text
手机 A ─┐
手机 B ─┼─► linkr-ble ── 一个 socket ──► daemon（一个 BleClient）
手机 C ─┘
```

daemon 看到的是**一个** `BleClient`，不是三个。所以：

- BLE 的 `release` 不能把共享桥永久锁死。
- 单个手机的“释放”语义无法准确映射。
- 代码选择：BLE 不加入 `managed`，`release` 后仍可自动 claim。

> 这是“传输层抹掉了身份”导致的折中。**要修就得让 socket 协议
> 带上主机身份**——那是更大的改动。

### `epoch`：为什么需要版本号

```python
def bump_epoch(self):
    self.epoch = (self.epoch + 1) & 0xFFFFFFFF
    if self.epoch == 0:
        self.epoch = 1
```

客户端缓存了 `owner` 状态，如果只比较 id：

```text
A 是 owner(id=3)
A 断开
B 拿到 id=3（因为 next_id 会回绕吗？不会，但重连会新建）
```

真正的问题不是 id 复用，而是**客户端不知道“所有权变过”**。

`epoch` 是一个单调计数：

```text
epoch 变了 → 所有权发生过变化 → 重新查询
```

```text
@c? → OK control self=3 owner=3 mode=control epoch=17 peers=2 ...
```

> 客户端不需要理解“谁抢了谁”，只需要知道“我的缓存过期了”。

### `status` 的返回

```python
return ("OK control self=%d owner=%d mode=%s epoch=%d peers=%d "
        "ble_max=2 linkr_reserved=0\r\n" % ...)
```

| 字段 | 含义 |
|---|---|
| `self` | 你的 id |
| `owner` | 当前 owner 的 id（0 = 无） |
| `mode` | `control` / `readonly` / `available` |
| `epoch` | 所有权变更计数 |
| `peers` | 当前连接数 |

`mode` 是给 UI 用的“结论”：

```text
control   → 你可以写
readonly  → 别人在写，你只能看
available → 没有 owner，谁写谁拿
```

> 把“结论”算在服务端，客户端就不用重复实现规则。
> **规则只有一份。**

---

## 31.5 命令分发

### 入口

```python
def handle_text(self, client, text):
    # Service: control (@c), name (@n), Wi-Fi (@w), LAN token (@s),
    # diagnostics (@i), UART (@u) and WebDAV (@d).
    ...

def handle_binary(self, client, data):
    # 串口字节
    ...
```

一个连接上：

```text
文本帧 → handle_text
二进制帧 → handle_binary
```

这就是“同一通道，两种语义”。

### 别名

```python
if line.startswith("@control "):
    line = "@c " + line[9:]
if line in ("@c?", "@control?"):
    ...
```

为什么要别名：

```text
早期版本用 @control，后来简化为 @c
老客户端还在用 @control
→ 两个都支持
```

代价是代码里到处 `in (...)`。收益是**不破坏已部署的客户端**。

> 兼容性不是免费的，但它比“让所有用户升级”便宜。

### 帮助就是协议文档

```python
if line in ("@h", "@help", "help", "h"):
    return ("OK cmds: @c?|@c claim|@c claim idle|@c release|"
            "@c yield <peer> @i? @u? @n?|@n=<name>|@n default "
            "@w?|@w scan|@w=<ssid>,<pass>|@w off "
            "@s?|@s on|@s off|@s token|@s token=<hex>|@s token off "
            "@d?|@d=<url>[,<user>,<pass>]|@d off "
            "@u?|@u=<baud>,<data>,<n|e|o>,<1|2>,<n|rtscts> "
            ...)
```

这段字符串**就是**协议清单。改协议必须改它。

### 参数校验

```python
for prefix in ("@u=", "@uart="):
    if line.startswith(prefix):
        parts = line[len(prefix):].split(",")
        if len(parts) != 5:
            return "ERR format: @u=115200,8,n,1,n\r\n"
```

错误格式统一：

```text
ERR <类别>: <说明>
ERR control=not_owner
ERR format: @u=115200,8,n,1,n
```

统一前缀让客户端能机械判断：

```text
OK ...   成功
ERR ...  失败
@event   异步事件
@info    诊断
@scan    扫描结果
```

---

## 31.6 事件路由：reqid 的两种命运

### LAN：直接文本帧

```python
def emit_event(self, client, reqid, text, final=False, error=False):
    # LAN clients use reqid=0: they receive async results as plain text
    # frames.  BLE needs a non-zero request id to route the event.
    if client is None:
        return
    if reqid == 0 and getattr(client, "transport", "") == "ble":
        return
    flags = (0x0001 if final else 0) | (0x0002 if error else 0)
    try:
        client.send_event(reqid, flags, text)
    except (OSError, AttributeError):
        pass
```

- WebSocket：`send_event` 直接 `send_text(text)`。浏览器看到的就是
  一行 `@event ...`。
- BLE：`send_event` 包成 12 字节头，`reqid` 用来让手机知道
  “这是哪个请求的结果”。

### 为什么 LAN 不需要 reqid

因为 LAN 是**长连接 + 文本流**：

```text
浏览器发 @w scan
→ @scan result ...
→ @scan result ...
→ @scan done
```

浏览器按顺序读就行。而且 WebSocket 只有它一个在读这条流。

BLE 不一样：手机可能同时发多个请求（虽然通常不会），而
Management 的响应特性是 **indicate**，需要明确“这条属于谁”。

### `final` 和 `error` 标志

```text
flags bit0 (0x0001)  FINAL
flags bit1 (0x0002)  ERROR
flags bit2 (0x0004)  ASYNC
```

用途：

```text
final=1  → 这个请求的结果到此结束
error=1  → 结果是失败
async=1  → 主动推送，不对应某个请求
```

手机端可以据此：

- 收到 `final` 才结束 loading。
- 收到 `error` 显示红色提示。
- 收到 `async` 不当作命令结果。

> 有了这三个位，客户端不需要解析文本内容就能知道“状态”。

### Wi-Fi 连接的完整事件序列

```python
self.emit_event(client, reqid,
                "@event wifi operation=%d phase=queued result=0 "
                "state=off ip=down\r\n" % reqid)
...
self.emit_event(client, reqid,
                "@event wifi operation=%d phase=ready result=0 "
                "state=connected ip=%s\r\n" % (reqid, info["ip"]),
                final=True)
```

序列：

```text
@event wifi ... phase=queued
（几秒后）
@event wifi ... phase=ready    ← final=True
```

失败：

```text
@event wifi ... phase=failed ...   ← final=True, error=True
```

> 长操作必须有“已受理”和“已完成”两个事件。否则用户不知道
> 是卡住了还是在进行中。

---

## 31.7 四种端如何复用同一套

### WebSocket 客户端

```js
ws.send("@w scan");            // 文本帧
ws.onmessage = (e) => {
  if (typeof e.data === "string") {
    if (e.data.startsWith("@scan ")) renderScan(e.data);
    if (e.data.startsWith("@event ")) renderEvent(e.data);
  } else {
    term.write(new Uint8Array(e.data));   // 串口字节
  }
};
```

### BLE 客户端

```text
写 CMD 特性：12 字节头 + "@w scan"
订阅 RSP：收到 12 字节头 + "@scan result ..."
```

### CLI

```html
bee-cli wifi scan
→ 内部发 @w scan，读文本帧直到 @scan done
```

### TUI

```text
F3 触发 @i?，把 @info 行渲染到侧栏
```

四者的业务代码不同，**协议完全一样**。

### 一个“能力声明”问题

LAN 和 BLE 能力不同：

```text
LAN：能改 Wi-Fi？可以（daemon 在跑 wctl）
BLE：能改 Wi-Fi？可以，但要 Management
```

第 27 章补丁 5 让 LAN 也能跑 `@` 命令后，其实**能力已经对齐**。
剩下的是 UI 表达问题。

> 长期方向：让 daemon 在连接建立时**声明能力**：

```text
@hello caps=serial,control,wifi,webdav,name,token transport=ws
```

客户端据此显示/隐藏功能，而不是硬编码“LAN 不能改 Wi-Fi”。

**这是“协议统一”的最后一步：不要用传输类型推断能力。**

---

## 31.8 新增一种传输要改哪里

假设要加一个“Unix socket 本地客户端”。

### 不该改的

```text
- Session 的业务逻辑
- @ 命令解析
- owner 规则
- 事件语义
```

### 要改的

```text
1. 写一个 LocalClient 类，实现 send_binary/send_text/send_event
2. 在 accept 循环里 register/unregister
3. 完成
```

```python
class LocalClient:
    transport = "local"

    def __init__(self, conn, session):
        self.conn = conn
        self.session = session
        self.id = 0

    def send_binary(self, data):
        self._frame(0x01, data)

    def send_text(self, text):
        self._frame(0x02, text.encode())

    def send_event(self, reqid, flags, text):
        self._frame(0x03, text.encode())

    def _frame(self, typ, payload):
        self.conn.sendall(bytes([typ]) + struct.pack("<H", len(payload)) + payload)
```

**没有一行业务逻辑。** 如果加传输要改业务代码，说明分层没做对。

### 反例：按传输分支的业务代码

```python
# 不好
if transport == "ws":
    send_json({"result": x})
elif transport == "ble":
    send_mgmt_frame(x)
elif transport == "cli":
    print(x)
```

这样的话每加一种传输，所有命令都要改一遍。

正确：

```python
# 好
client.send_event(reqid, flags, text)
```

> 判断标准：**`if transport ==` 只应该出现在适配层。**

这块板代码里确实有 `transport == "ble"` 的判断（owner 语义、事件路由）。
那是**已知的、有注释的例外**，不是设计目标。

---

## 31.9 协议版本与演进

### 为什么现在没有版本号

```text
@ 命令是文本，加命令不破坏旧客户端
未知命令返回 ERR，客户端忽略即可
```

所以靠“向后兼容”而不是“版本协商”。

### 什么时候必须加版本

```text
- 改变已有命令的语义
- 改变二进制帧格式
- 改变事件顺序保证
```

这时应该：

```text
1. 新命令用新名字（@u2=...），不覆盖旧的
2. 或握手时声明版本：@hello v=2
3. 保留旧行为至少一个大版本
```

### 二进制帧的版本

Management 头里有 `version=1` 字段：

```text
0  2  'L','K'
2  1  版本
```

Reliable 头里也有：

```text
0  2  'L','R'
2  1  版本
```

但**目前没有检查版本**——只检查魔数和类型。

```python
if mgmt_rx_len == 0:
    if len < 12: return
    if data[0] != 'L' or data[1] != 'K' or data[2] != 1 or data[3] != 1:
        return
```

`data[2] != 1` 就是版本检查：不是 1 就丢。

> 版本字段现在只是“拒绝未知版本”，不是“兼容多版本”。
> 这是合理的保守做法：**宁可拒绝，不要猜。**

---

## 31.10 安全边界

### 三种身份

```text
传输认证  → 你能不能连（TLS 证书、BLE 配对）
会话认证  → 你能不能控制（LAN 令牌、owner）
操作授权  → 你能不能改这个（Wi-Fi、令牌、名称）
```

### 现在的实现

| 层 | 机制 |
|---|---|
| 传输 | 自签名 TLS（可选）/ BLE Just Works |
| 会话 | LAN 令牌（可选，第一帧文本） |
| 操作 | owner（写串口）；管理命令无额外校验 |

### 缺口

```text
1. BLE Just Works → 范围内任何人可配对
2. LAN 令牌是明文（ws://）或第一帧（wss 下加密）
3. 管理命令（改 Wi-Fi、改令牌）没有独立授权
4. 没有速率限制
```

> 这是“家用/教学”定位下的合理取舍，但**必须写清楚**。
> 第 36 章会系统讲威胁模型。

### 一条重要规则

```text
控制命令永远不能被当作串口数据发给目标板
```

这就是 daemon 要剥离 `LINKRCFG`/`LINKRSTAT` 的原因：

```python
CTL_PREFIXES = (b"LINKRCFG ", b"LINKRSTAT ")
```

否则 E907 的控制回复会出现在目标板终端里，甚至被目标板的
shell 当成命令执行。

> 协议混合时，“哪部分属于谁”必须显式划分。

---

## 31.11 实验：用四种方式发同一条命令

### 实验 A：WebSocket

```js
// 浏览器控制台
const ws = new WebSocket("ws://192.168.0.121/ws");
ws.onmessage = e => console.log(e.data);
ws.onopen = () => ws.send("@i?");
```

### 实验 B：CLI

```sh
bee-cli diag
```

### 实验 C：curl + Python

```python
# 用第 20 章的 ws 实现
send_text("@i?")
```

### 实验 D：手机 App

```text
写 Management CMD：LK 头 + "@i?"
订阅 RSP，打印
```

### 对比

四次输出应该**语义一致**：

```text
@info fw version=...
@info sys uptime_ms=...
@info uart ...
@info wifi ...
@info upload ...
@info ws ...
@info done
```

BLE 会多一层 12 字节头，但载荷相同。

> 如果四种结果不一致，说明有传输分支漏了统一。

### 实验 E：并发 owner

1. 开两个浏览器标签，都连 `/ws`。
2. 在 A 里输入字符。
3. 在 B 里输入字符。
4. 两边都发 `@c?`，观察 `owner`/`mode`/`epoch`。

预期：

```text
A 先写 → A 是 owner
B 再写 → 3 秒内被丢弃（或 A 空闲则 B 接管）
```

### 实验 F：release 语义

1. A 执行 `@c release`。
2. A 再输入字符。
3. 观察是否被丢弃。
4. A 执行 `@c claim`，再输入。

预期：release 后不自动 reclaim。

---

## 31.12 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 两个终端互相插字 | 没有 owner 机制 | `@c?`、检查 daemon |
| release 后又自动拿到 | `managed` 逻辑 | 看 `handle_binary` |
| owner 显示不更新 | 没看 `epoch` | 每次变更后重查 |
| BLE 释放后桥锁死 | 把 BLE 也加进 `managed` | 看 `transport` 分支 |
| 控制回复出现在目标终端 | 没剥离 `LINKR*` | `CTL_PREFIXES` |
| 事件收不到 | reqid=0 被 BLE 丢弃 | `emit_event` |
| 同名事件乱序 | 没按 reqid 匹配 | 客户端用 map 存 |
| 新传输加了要改一堆 | 业务里有 `if transport` | 重构到适配层 |
| 旧客户端坏了 | 改了命令语义 | 加新命令，别改旧的 |
| 帮助和实际不符 | 忘了更新 `@h` | 改协议必改帮助 |

---

## 31.13 练习

### 练习 31.1（必做）

用四个客户端（两个浏览器、一个 CLI、一个手机）同时连上，
执行：

```text
@c?
@c claim
@i?
```

记录每个客户端的 `self`/`owner`/`mode`/`epoch`，画出所有权变化。

### 练习 31.2（必做）

回答：

1. 为什么一条串口需要 owner？
2. `claim idle` 的 3 秒是技术限制还是产品选择？
3. `release` 和“从没 claim 过”有什么不同？
4. `epoch` 解决什么问题？
5. BLE 为什么不能有独立的 owner？
6. 为什么管理命令不能当串口数据发？

### 练习 31.3（必做）

读 `emit_event` 和 `handle_binary`，写出：

```text
1. LAN 客户端的事件怎么到浏览器
2. BLE 客户端的事件怎么到手机
3. 两者在 reqid 上的差异
```

### 练习 31.4（必做）

给 daemon 加一个新命令 `@t?`（返回当前目标标识），要求：

- 在 LAN 和 BLE 上都能用。
- 更新 `@h`。
- 写一条测试命令验证两种传输。

### 练习 31.5（选做）

设计并实现一个 `LocalClient`（Unix socket），要求：

- 复用全部 `@` 命令。
- 不改 `Session` 业务代码。
- 写一个最小客户端验证 `@i?`。

### 练习 31.6（选做）

提出一个“能力声明”方案：

```text
连接建立时，daemon 发 @hello caps=...
```

要求说明：

- 哪些能力应该声明。
- 老客户端怎么忽略。
- 前端怎么用它替代 `state.mode` 判断。

---

## 31.14 本章验收

1. 能解释“传输”和“协议”的分离，以及为什么必须分离。
2. 能画出三层协议（字节流、命令流、事件流）。
3. 能说清 `Session`/`Client`/`BleClient` 的职责边界。
4. 理解 owner、claim idle、release、yield、epoch 的完整语义。
5. 能解释 BLE 共享桥带来的所有权折中。
6. 知道新增一种传输只需要实现三个方法。
7. 能识别“业务代码里出现 transport 分支”这个坏味道。

### 自测题

1. 文本帧和二进制帧分别承载什么？
2. `mode=readonly` 是什么意思？
3. `epoch` 变了一定代表 owner 变了吗？
4. 为什么 BLE 的事件需要 reqid，LAN 不需要？
5. `final`/`error`/`async` 三个标志各表示什么？
6. 为什么控制回复必须从串口流里剥离？

> 第 4 部分到此结束。你已经能让四种端用同一套协议工作。
> 下一部分进入工程化：构建、刷机、测试、性能、安全、开源。
