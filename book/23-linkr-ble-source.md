# 第 23 章 linkr-ble 源码精读：从 D-Bus 到字节流

> 本章目标：
> 1. 知道 `linkr-ble` 为什么用 C + GLib/GIO，而不是 Python。
> 2. 看懂 D-Bus 对象注册：Service、Characteristic、ObjectManager、Advertisement、Agent。
> 3. 理解 Unix socket 帧协议和重连逻辑。
> 4. 看懂 Management 服务的 12 字节头和可靠 UART 的序号。
> 5. 能定位“服务注册失败”“写数据丢了”“通知节流不对”。

---

## 23.1 为什么单独写一个 C 进程

### 两个进程的分工

```text
linkr-bee-daemon（Python）
  - HTTP/WS/HTTPS
  - 会话、令牌、Wi-Fi
  - 持有 rpmsg 端点

linkr-ble（C）
  - 向 BlueZ 注册 GATT
  - 处理 D-Bus 读写
  - 维护 ATT 分片与节流
  - 通过 Unix socket 和 daemon 通信
```

### 为什么不用 Python 直接做 GATT

Python 通过 D-Bus 注册 GATT 当然可以（`pydbus`/`dbus-python`），
但：

- 板子没装这些库，也不一定联网。
- 高频通知（每 5 ms）在 Python 里开销大。
- C + GLib 是 BlueZ 官方示例的语言。

> 分工原则：**控制面用 Python（好改），数据面用 C（够快）。**

### 为什么不能合并成一个进程

- Python 的 GLib 主循环和 `threading` 混用容易出问题。
- D-Bus 对象注册需要稳定的事件循环。
- 两个进程崩溃互不影响：BLE 挂了，HTTP 还能用。

代价：多一个 Unix socket 协议。

---

## 23.2 数据模型

### 三个服务

```c
#define NUS_SVC  APP_PATH "/s0"
#define MGMT_SVC APP_PATH "/s1"
#define REL_SVC  APP_PATH "/s2"

static const struct svc_def services[3] = {
    { NUS_SVC,  UUID_NUS  },
    { MGMT_SVC, UUID_MGMT },
    { REL_SVC,  UUID_REL  },
};
```

### 九个特性

```c
enum ckind {
    CK_NUS_RX, CK_NUS_TX,
    CK_MGMT_PROTO, CK_MGMT_ID, CK_MGMT_CMD, CK_MGMT_RSP,
    CK_REL_RX, CK_REL_TX, CK_REL_STATE,
    CK_COUNT
};

static const struct chr_def chars[CK_COUNT] = {
    [CK_NUS_RX] = { NUS_RX, UUID_NUS_RX, NUS_SVC,
                    { "write", "write-without-response", NULL }, CK_NUS_RX },
    [CK_NUS_TX] = { NUS_TX, UUID_NUS_TX, NUS_SVC,
                    { "notify", NULL }, CK_NUS_TX },
    ...
};
```

### D-Bus 路径

```text
/com/v851s/linkr/s0         NUS 服务
/com/v851s/linkr/s0/c0      NUS RX
/com/v851s/linkr/s0/c1      NUS TX
/com/v851s/linkr/s1         Management 服务
...
/com/v851s/linkr/advertisement
/com/v851s/linkr/agent
```

> 路径只是标识，语义靠 UUID 和 `Flags`。

---

## 23.3 注册 D-Bus 对象

### 需要实现哪些接口

| 接口 | 谁调用 | 作用 |
|---|---|---|
| `org.bluez.GattService1` | BlueZ | 描述服务 |
| `org.bluez.GattCharacteristic1` | BlueZ | 读写/订阅 |
| `org.freedesktop.DBus.ObjectManager` | BlueZ | 一次返回所有对象 |
| `org.bluez.LEAdvertisement1` | BlueZ | 广播内容 |
| `org.bluez.Agent1` | BlueZ | 配对 |

### XML 定义

```c
static const char chr_xml[] =
    "<node><interface name=\"org.bluez.GattCharacteristic1\">"
    "<method name=\"ReadValue\">"
    "<arg name=\"options\" type=\"a{sv}\" direction=\"in\"/>"
    "<arg name=\"value\" type=\"ay\" direction=\"out\"/></method>"
    "<method name=\"WriteValue\">"
    "<arg name=\"value\" type=\"ay\" direction=\"in\"/>"
    "<arg name=\"options\" type=\"a{sv}\" direction=\"in\"/></method>"
    "<method name=\"StartNotify\"/>"
    "<method name=\"StopNotify\"/>"
    "<property name=\"UUID\" type=\"s\" access=\"read\"/>"
    "<property name=\"Service\" type=\"o\" access=\"read\"/>"
    "<property name=\"Flags\" type=\"as\" access=\"read\"/>"
    "<property name=\"Notifying\" type=\"b\" access=\"read\"/>"
    "<property name=\"Value\" type=\"ay\" access=\"read\"/>"
    "</interface></node>";
```

为什么用 XML：`g_dbus_node_info_new_for_xml` 生成 introspection
数据，BlueZ 通过它知道有哪些方法和属性。

### 虚表

```c
static const GDBusInterfaceVTable chr_vtable = {
    .method_call  = char_method_call,
    .get_property = char_get_property,
    .set_property = NULL,
};
```

- `method_call`：处理 `ReadValue`/`WriteValue`/`StartNotify`。
- `get_property`：处理 `UUID`/`Flags`/`Notifying`/`Value`。
- `set_property`：特性属性都是只读，所以 `NULL`。

### 注册

```c
g_dbus_connection_register_object(bus, chars[i].path,
                                  chr_iface, &chr_vtable,
                                  &chars[i], NULL, &err);
```

最后一个 `user_data` 是 `&chars[i]`，在回调里通过
`user_data` 反查是哪个特性。这是 GLib 常用的“容器反查”模式。

### `RegisterApplication`

```c
g_dbus_connection_call(
    bus, "org.bluez", "/org/bluez/hci0",
    "org.bluez.GattManager1", "RegisterApplication",
    g_variant_new("(oa{sv})", APP_PATH, NULL), ...);
```

`APP_PATH` 是对象树的根。BlueZ 会调用
`ObjectManager.GetManagedObjects` 来发现所有服务和特性。

> 注意：BlueZ 可能异步处理注册。如果立刻去注册广播，可能
> 还没看到服务。代码里用回调/重试处理。

---

## 23.4 Unix socket 帧协议

### 帧格式

```text
type   u8
length u16 LE
payload[length]
```

### 类型

```c
#define FRAME_DATA    0x01
#define FRAME_CTL_REQ 0x02
#define FRAME_CTL_RSP 0x03
#define FRAME_HELLO   0x10
#define FRAME_NAME    0x11
#define FRAME_EVENT   0x12
```

### 发送

```c
static void send_frame(int fd, guint8 type, const void *data, gsize len)
{
    guint8 hdr[3];

    if (len > 0xffff) {
        logmsg("frame too large (%zu), dropped", len);
        return;
    }
    hdr[0] = type;
    hdr[1] = len & 0xff;
    hdr[2] = (len >> 8) & 0xff;
    write_all(fd, hdr, sizeof(hdr));
    write_all(fd, data, len);
}
```

`write_all` 必须循环：`write()` 可能只写一部分。

### 接收

```c
static gboolean sock_read_cb(gint fd, GIOCondition cond, gpointer data)
{
    guint8 buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n <= 0) goto drop;

    g_byte_array_append(sock_rx, buf, n);

    while (sock_rx->len >= 3) {
        guint8 type = sock_rx->data[0];
        gsize len = sock_rx->data[1] | (sock_rx->data[2] << 8);

        if (sock_rx->len < 3 + len) {
            break;                 /* 半包，等下次 */
        }
        /* 处理完整帧 */
        g_byte_array_remove_range(sock_rx, 0, 3 + len);
    }
    return G_SOURCE_CONTINUE;
}
```

**这是所有流式协议的标准写法：**

```text
1. 读到缓冲
2. 循环：够一个头吗？够一个完整帧吗？
3. 不够就 break，等下次读
4. 够了就处理，从缓冲移除
```

> 不这样写，就会遇到“粘包/拆包”问题：一次 `read` 可能拿到
> 半个帧，也可能拿到三个帧。

### 重连

```c
static void schedule_reconnect(void)
{
    g_timeout_add_seconds(1, sock_connect_cb, NULL);
}
```

断线后 1 秒重连。连接成功发：

```c
send_frame(fd, FRAME_HELLO, "linkr-ble", 9);
```

### 为什么不直接让 daemon 连 BLE

因为 BlueZ 的 GATT 注册必须由“拥有对象”的进程做。
daemon 注册了对象，但它的主循环是 HTTP accept，不适合跑
D-Bus 事件循环。

---

## 23.5 Management 服务：12 字节头

### 帧格式

```text
偏移  长度  字段
0     2     'L','K'
2     1     版本 = 1
3     1     类型（1=请求，2=响应，3=事件）
4     4     reqid（小端）
8     2     载荷长度（小端）
10    2     flags
12    N     payload
```

### 请求解析

```c
static void mgmt_write(const guint8 *data, gsize len)
{
    if (mgmt_rx_len == 0) {
        if (len < 12) return;
        if (data[0] != 'L' || data[1] != 'K' ||
            data[2] != 1 || data[3] != 1) return;

        mgmt_rx_reqid = data[4] | (data[5] << 8) |
                        (data[6] << 16) | (data[7] << 24);
        mgmt_rx_need = data[8] | (data[9] << 8);
        if (mgmt_rx_need == 0 || mgmt_rx_need > MGMT_MAX_PAYLOAD) return;

        mgmt_rx_got = 0;
        mgmt_rx_len = 12;
        memcpy(mgmt_rx_buf, data, 12);
        ...
    }
    /* 累加后续片段，直到 mgmt_rx_got >= mgmt_rx_need */
}
```

### 为什么要有自己的头

BLE 一次只能写 `att_chunk` 字节（20～514）。一条命令可能
超过这个长度，必须分片。

12 字节头提供：

- 魔数 `LK`：快速丢弃非本协议数据。
- 版本：以后扩展。
- 类型：请求/响应/事件。
- `reqid`：匹配请求和响应（异步）。
- 长度：知道还要收多少。
- flags：final/error/async。

> BLE 的 ATT 本身有“写长值”机制，但它依赖 MTU 和控制器，
> 不如自己在应用层分片可控。

### 响应排队与节流

```c
static void mgmt_send_tick(gint64 now)
{
    if (!notifying[CK_MGMT_RSP] || now < mgmt_next_us) return;

    if (!mgmt_cur) {
        /* 从队列取一个响应，加 12 字节头 */
    }
    gsize n = MIN(att_chunk, mgmt_cur->len - mgmt_off);
    emit_value(MGMT_RSP, mgmt_cur->data + mgmt_off, n);
    mgmt_off += n;

    if (mgmt_off >= mgmt_cur->len) {
        /* 发完一帧，等 40 ms */
        mgmt_next_us = now + 40000;
    } else {
        /* 还有分片，等 15 ms */
        mgmt_next_us = now + 15000;
    }
}
```

为什么节流：

- 手机处理通知需要时间。
- 控制器缓冲有限。
- 发太快会被丢弃或触发流控。

15 ms / 40 ms 是经验值，随 MTU 和手机不同可调。

---

## 23.6 可靠 UART 服务：序号与确认

### 为什么需要“可靠”

NUS 的 notify 是**无确认**的。链路差时可能丢包。对串口
控制台来说，丢几个字节可能只是显示乱，但对命令输入可能是
灾难。

Reliable UART 服务用 **indicate**（有确认）+ 序号：

```text
发送方：帧头带 seq
接收方：按 seq 顺序接受，重复丢弃，乱序报 gap
```

### 帧头

```text
0  2  'L','R'
2  1  版本 = 1
3  1  保留
4  4  seq
8  2  长度
10 2  保留
12 N  数据
```

### 接收

```c
if (rel_rx_seq == rel_rx_expected) {
    send_frame(sock_fd, FRAME_DATA, rel_rx_buf + 12, rel_rx_need);
    rel_rx_expected++;
    if (rel_rx_expected == 0) rel_rx_expected = 1;
} else if (rel_rx_seq != prev) {
    logmsg("reliable RX sequence gap: expected %u got %u",
           rel_rx_expected, rel_rx_seq);
}
```

- 等于期望值：接受，期望值 +1。
- 等于上一个值：重复，忽略。
- 其他：缺口，记日志。

> 这里的“可靠”是**顺序 + 去重**，不是“自动重传”。
> 真正的重传需要接收方发 NACK 或发送方超时重发。这块板
> 依赖 BLE 链路层自己的重传 + indicate 确认。

### 发送合并

```c
if (!rel_cur) {
    /* 等最多 10 ms，凑够 232 字节再发 */
    if (serial_in->len < 232) {
        if (!rel_fill_since) rel_fill_since = now;
        if (now - rel_fill_since < 10000) return;
    }
    ...
}
```

为什么合并：

- 每帧有固定开销（12 字节头 + indicate 确认）。
- 30 字节一帧太浪费。
- 凑到 232 字节，吞吐明显提升。

### 232 的来历

```text
MTU 247 - 3(ATT) - 12(头) = 232
```

MTU 247 是常见手机协商值。代码里硬编码 232，是因为可靠
服务假设对端支持较大 MTU；小 MTU 设备用 NUS。

### 状态特性

```c
state[0] = 1;                    /* 版本 */
state[1] = 1;                    /* 选中可靠服务 */
state[2..3] = 232;               /* 每帧最大数据 */
state[4..7] = rel_rx_expected;   /* 期望的下一个 seq */
state[8..11] = rel_tx_next;      /* 下一个要发的 seq */
state[12..15] = rel_tx_acked;    /* 已被确认的 seq */
```

客户端读这个特性，就知道当前窗口和进度。

---

## 23.7 三个发送 tick 的优先级

```text
mgmt_send_tick   控制响应优先
rel_send_tick    可靠串口
nus_send_tick    普通串口
```

关键代码：

```c
static void nus_send_tick(gint64 now)
{
    if (notifying[CK_REL_TX] || !notifying[CK_NUS_TX] ||
        now < nus_next_us) {
        return;
    }
    ...
}
```

- 有可靠订阅时，**NUS 不发**（避免同一份数据发两遍）。
- 没人订阅就跳过。
- 每 5 ms 最多发 4 个 chunk。

### 为什么 NUS 一次发 4 个

```c
for (i = 0; i < 4 && serial_in->len; i++) {
    n = MIN(att_chunk, serial_in->len);
    emit_value(NUS_TX, ...);
    ...
}
```

4 个 chunk 是“突发上限”。再多会塞满控制器缓冲。

> 吞吐 = att_chunk × 4 / 5 ms。MTU 247 时约 195 KB/s 理论值，
> 实际受连接间隔和手机影响。

---

## 23.8 广播与名字更新

### 注册广播

```c
g_dbus_connection_call(
    bus, "org.bluez", "/org/bluez/hci0",
    "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement",
    g_variant_new("(oa{sv})", ADV_PATH, NULL), ...);
```

### 广播内容

```c
if (!strcmp(prop, "Type")) {
    return g_variant_new_string("peripheral");
}
if (!strcmp(prop, "ServiceUUIDs")) {
    const char *uuids[] = { UUID_MGMT, NULL };
    return g_variant_new_strv(uuids, -1);
}
if (!strcmp(prop, "LocalName")) {
    return g_variant_new_string(adv_name);
}
```

- 广播里带 Management 服务 UUID，方便客户端过滤。
- 带设备名。

### `Includes` 为什么是空

```c
/* LocalName already adds MGMT_ADV_FLAG_LOCAL_NAME; asking for
 * "local-name" here makes BlueZ reject the advertisement. */
return g_variant_new_strv(NULL, 0);
```

BlueZ 对 `Includes` 的语义有坑：重复请求 `local-name` 会被
拒绝。空数组最安全。

### 改名

```c
static void apply_new_name(const char *name, gsize len)
{
    ...
    /* Unregister + Register，因为 BlueZ 缓存 LocalName */
}
```

daemon 通过 `FRAME_NAME` 发新名字，linkr-ble 重新注册广播。

---

## 23.9 实验：观察 D-Bus

### 实验 A：看对象树

```sh
# 板子上（如果有 busctl）
busctl --system tree org.bluez | head -40
```

或者：

```sh
dbus-send --system --print-reply --dest=org.bluez \
  /org/bluez/hci0 org.freedesktop.DBus.Introspectable.Introspect
```

### 实验 B：看我们的服务

```sh
busctl --system tree com.v851s.linkr 2>/dev/null || \
  dbus-send --system --print-reply --dest=org.freedesktop.DBus \
  / org.freedesktop.DBus.ListNames | grep v851s
```

### 实验 C：看日志

```sh
tail -f /var/log/linkr-ble.log
```

正常应看到：

```text
advertising as "Linkr Bee V851S"
connected to /var/run/linkr-bee.sock
StartNotify /com/v851s/linkr/s0/c1
```

### 实验 D：看 MTU 目录

```sh
ls -l /var/run/linkr-bt-mtu/
cat /var/run/linkr-bt-mtu/*
```

---

## 23.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 服务不出现 | `RegisterApplication` 失败 | linkr-ble 日志 |
| 广播空 | 内核 bug | `ble-adv-data.py` |
| 写数据丢 | 分片头错/长度超限 | 日志、抓包 |
| 通知不来 | 没订阅/`notifying` 没置 | `StartNotify` 日志 |
| 吞吐低 | `att_chunk=20`/节流 | MTU 目录 |
| 数据重复 | 可靠服务 seq 处理 | gap 日志 |
| socket 断 | daemon 重启 | 重连日志 |
| 改名无效 | 没重新注册广播 | `apply_new_name` |
| 配对失败 | Agent 没注册 | BlueZ 日志 |
| 内存增长 | 帧未释放/队列积压 | `VmRSS` |

---

## 23.11 练习

### 练习 23.1（必做）

阅读 `sock_read_cb`，回答：

1. 为什么用 `g_byte_array` 而不是定长缓冲？
2. 为什么 `while (sock_rx->len >= 3)` 里还要检查 `3 + len`？
3. 断线后怎么重连？

### 练习 23.2（必做）

阅读 `mgmt_write`，回答：

1. 12 字节头各字段是什么？
2. 为什么要检查 `mgmt_rx_need <= MGMT_MAX_PAYLOAD`？
3. 分片没接齐时，下一帧数据怎么处理？

### 练习 23.3（必做）

对比 NUS 和 Reliable：

| | NUS | Reliable |
|---|---|---|
| GATT 属性 | | |
| 有没有序号 | | |
| 有没有确认 | | |
| 每帧最大 | | |
| 适用场景 | | |

### 练习 23.4（选做）

用 `btmon` 或手机工具，观察一次 Reliable 服务的 indicate：

- 找到 `L`,`R` 头。
- 记录 seq 变化。
- 观察客户端确认。

---

## 23.12 本章验收

1. 能解释为什么 BLE 数据面用 C、控制面用 Python。
2. 能列出注册到 BlueZ 的五种接口。
3. 能画出 Unix socket 帧格式和处理循环。
4. 能解释 Management 的 12 字节头和可靠服务的序号。
5. 知道三个发送 tick 的优先级和节流值。
6. 能排查注册失败、广播空、写丢、通知不来。

### 自测题

1. `WriteValue` 的参数类型是什么？
2. Unix socket 帧头几字节？长度怎么表示？
3. `LK` 和 `LR` 分别是什么服务？
4. 为什么可靠服务用 indicate 而不是 notify？
5. 为什么有可靠订阅时 NUS 不发？
6. `232` 是怎么算出来的？

> 下一章讲吞吐、分片和可靠性：把 BLE 的性能调到能用。
