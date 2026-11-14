# 第 22 章 BLE、BlueZ 与 GATT

> 本章目标：
> 1. 理解 BLE 的角色、广播、连接、GATT 模型。
> 2. 知道 BlueZ 在 Linux 里扮演什么角色，以及它和内核的分工。
> 3. 能设计一个 GATT 服务：Service、Characteristic、Property、Flag。
> 4. 理解 MTU、ATT、通知、指示的差别。
> 5. 能排查“搜不到”“连不上”“能连但不通知”“MTU 不对”。

---

## 22.1 BLE 的基本模型

### 四个角色

| 角色 | 这块板 |
|---|---|
| Peripheral（外设） | 板子，提供服务 |
| Central（中心） | 手机/浏览器，发起连接 |
| Broadcaster | 只广播不连接（beacon） |
| Observer | 只扫描不连接 |

这块板是 **Peripheral**：

```text
板子 ──广播──► 手机发现
手机 ──连接──► 板子
手机 ──读/写/订阅──► GATT 服务
```

### 广播 vs 连接

```text
广播（Advertising）
  - 单向，最多 31 字节（传统）
  - 包含：设备名、服务 UUID、厂商数据
  - 不需要连接

连接（Connection）
  - 双向，有连接间隔
  - 可以读写 GATT 特性
  - 可以订阅通知
```

### 为什么 BLE 省电

- 广播间隔可调（100 ms 到几秒）。
- 连接间隔可调（7.5 ms 到 4 s）。
- 每次只传少量数据。
- 大部分时间芯片在睡眠。

代价：

- 吞吐低（几十 KB/s，取决于 MTU 和连接间隔）。
- 协议复杂。

---

## 22.2 GATT 模型：服务、特性、描述符

### 三层结构

```text
Profile（应用层约定，比如心率、串口）
  └─ Service（服务，一组相关特性）
       └─ Characteristic（特性，一个值 + 属性）
            └─ Descriptor（描述符，比如用户描述、CCCD）
```

### 这块板的三个服务

| 服务 | UUID | 用途 |
|---|---|---|
| NUS（Nordic UART Service） | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | 兼容现成串口工具 |
| Management Service v1 | `4c4b0001-9a7e-4f4e-8b8a-3d6f12a0c001` | 控制命令 |
| Reliable UART Service v1 | `4c4b0010-9a7e-4f4e-8b8a-3d6f12a0c001` | 带序号/窗口的可靠传输 |

### NUS 的两个特性

| 特性 | UUID | 属性 | 方向 |
|---|---|---|---|
| RX | `6e400002-...` | write / write-without-response | 手机 → 板子 |
| TX | `6e400003-...` | notify | 板子 → 手机 |

命名容易反：**RX 是“板子接收”**，**TX 是“板子发送”**。

> 这是 Nordic 的命名约定。写代码时以“板子视角”为准，
> 不要以“手机视角”。

### 属性（Properties）

| 属性 | 含义 |
|---|---|
| `read` | 可以读 |
| `write` | 可以写，有响应 |
| `write-without-response` | 可以写，无响应 |
| `notify` | 可以订阅，无确认 |
| `indicate` | 可以订阅，有确认 |
| `read`/`write` 组合 | 读写都行 |

### Flags 在代码里怎么表达

```c
[CK_NUS_RX] = { NUS_RX, UUID_NUS_RX, NUS_SVC,
                { "write", "write-without-response", NULL }, CK_NUS_RX },

[CK_NUS_TX] = { NUS_TX, UUID_NUS_TX, NUS_SVC,
                { "notify", NULL }, CK_NUS_TX },
```

### 为什么 notify 和 indicate 不同

| | notify | indicate |
|---|---|---|
| 确认 | 无 | 有（ATT confirmation） |
| 吞吐 | 高 | 低 |
| 可靠性 | 可能丢 | 不丢 |
| 适用 | 实时串口 | 关键状态 |

这块板的串口数据用 **notify**（要吞吐）；可靠服务用
**indicate**（要确认）。

---

## 22.3 BlueZ 与内核的分工

### 分层

```text
应用（linkr-ble）
  │  D-Bus
BlueZ（bluetoothd）
  │  HCI socket / mgmt socket
内核（net/bluetooth）
  │  hciattach / UART
XR829 控制器
  │  无线电
手机
```

| 层 | 负责 |
|---|---|
| 内核 | HCI 传输、连接管理、L2CAP、ATT 底层 |
| BlueZ | GATT 数据库、D-Bus API、配对、广播管理 |
| linkr-ble | 注册服务、处理读写、推送通知 |
| XR829 | 射频、链路层 |

### 为什么不直接写 HCI

直接写 HCI 要自己实现：

- 广播数据格式。
- ATT 协议。
- 配对/加密。
- 连接参数协商。

BlueZ 已经做好，通过 D-Bus 暴露。**用 D-Bus 是正确选择。**

### D-Bus 是什么

一种进程间通信机制。BlueZ 在系统总线上注册对象：

```text
/org/bluez/hci0                             适配器
/org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX       设备
/com/v851s/linkr/s0                         我们注册的服务
/com/v851s/linkr/s0/c0                      特性
```

linkr-ble 通过 `GDBusConnection` 注册这些对象，BlueZ 调用它们。

---

## 22.4 `hciattach`：把 UART 变成 HCI

### 为什么需要

XR829 通过 UART 连到 A7（`/dev/ttyS2`），不是 USB/SDIO。
内核需要 `hciattach` 把这条 UART 变成标准 HCI 设备。

```sh
hciattach -n /dev/ttyS2 xradio
```

- `-n`：不 daemon 化，前台运行（由脚本管理）。
- `/dev/ttyS2`：BT UART。
- `xradio`：厂商协议类型。

### 成功标志

```sh
ls /sys/class/bluetooth/hci0
hciconfig hci0
```

### 为什么要它一直活着

`hciattach` 进程退出，`hci0` 就消失。所以脚本用后台进程
管理它，并用 `kill -0` 检查。

### 常见失败

| 现象 | 原因 |
|---|---|
| `hci0` 不出现 | 固件没加载、波特率不对、UART 没通 |
| 出现又消失 | hciattach 退出 |
| `-49` 错误 | 半开的 attach，内核状态没清 |
| 固件握手超时 | 冷启动、电源不稳 |

详见第 18 章 `S54bluetooth`。

---

## 22.5 广播：让手机能搜到

### 广播数据里放什么

传统 BLE 广播最多 31 字节：

```text
Flags           3 字节
Service UUIDs   2 + 2/16 字节每个
Local Name      2 + N 字节
Manufacturer    2 + N 字节
```

### 这块板的问题

Allwinner 4.9 内核把 `HCI_OP_LE_SET_ADV_DATA` 注释掉了，导致：

- BlueZ 注册的广播数据**到不了控制器**。
- 控制器发的是**空广播**。
- 手机按服务 UUID 过滤时搜不到。

### 绕过方案

注册广播后，直接调脚本写控制器：

```c
static void push_adv_data_to_controller(void)
{
    const char *cmd = "/usr/bin/python3 /usr/bin/ble-adv-data.py";
    g_spawn_command_line_async(cmd, NULL);
}
```

`ble-adv-data.py` 用 HCI socket 直接发 `LE_SET_ADV_DATA`。

> 这是典型的“内核有 bug，用户态绕过”。代价是多一个脚本、
> 一次额外 HCI 调用；收益是手机能搜到。

### 广播名

```text
Linkr Bee V851S
```

可改：

```text
@n=My Board
```

存在 `/mnt/UDISK/persist/ble-name`，最长 29 字节 UTF-8。

改名要**重新注册广播**：

```c
/* BlueZ 缓存 LocalName，必须 Unregister + Register */
```

### LE only

XR829 是双模（经典 + LE）。如果经典扫描开着：

- 手机会把板子缓存成经典设备。
- 连接时可能选 BR/EDR。
- 报 `br-connection-profile-unavailable`。

所以脚本最后：

```sh
hciconfig hci0 noscan
```

---

## 22.6 MTU 与分片

### 什么是 MTU

ATT MTU = 一次 ATT 数据包能带的最大字节数。

```text
默认 MTU = 23
ATT 头 = 3 字节
可用 payload = 20 字节
```

### 协商更大的 MTU

连接后双方可以交换 MTU：

```text
手机：Exchange MTU Request, 517
板子：Exchange MTU Response, 247
最终：取较小值
```

常见值：

| 设备 | MTU |
|---|---|
| 老设备 | 23 |
| 一般手机 | 185 / 247 |
| BlueZ 默认 | 517 |

### 这块板怎么知道 MTU

BlueZ（打过补丁）把每个 peer 的 MTU 写到：

```text
/var/run/linkr-bt-mtu/<device>
```

linkr-ble 每秒扫一次目录，取**最小值**：

```c
static void refresh_att_chunk(gint64 now)
{
    ...
    if (min_mtu > 3) {
        gsize chunk = min_mtu - 3;
        if (chunk > 514) chunk = 514;
        if (chunk < 20) chunk = 20;
        att_chunk = chunk;
    } else {
        att_chunk = 20;
    }
}
```

- `MTU - 3`：减去 ATT 头。
- 上限 514：BLE 最大 ATT payload。
- 下限 20：默认安全值。

### 为什么要取最小

多个手机同时连（这块板限制一个 central，但保险起见）：

- 一个 MTU 247，一个 MTU 23。
- 按 244 发，小 MTU 的会丢字节。
- 取 20，都能收。

> 这是“最小公倍数”思路：宁可慢，不可错。

### 分片

```c
gsize n = MIN(att_chunk, serial_in->len - serial_off);
emit_value(NUS_TX, data + serial_off, n);
serial_off += n;
```

一次通知最多 `att_chunk` 字节。超出部分下一轮发。

通知之间有节流：

```c
nus_next_us = now + 5000;   /* 5 ms */
```

为什么？发太快会：

- 塞满控制器缓冲。
- 被 BlueZ 丢弃。
- 手机来不及处理。

> 5 ms 是经验值。吞吐和稳定性之间取平衡。

---

## 22.7 读写与通知的处理

### 手机写 RX

```c
if (!strcmp(method, "WriteValue")) {
    GVariant *v = g_variant_get_child_value(params, 0);
    gsize len = 0;
    const guint8 *data = g_variant_get_fixed_array(v, &len, 1);

    if (data && len) {
        switch (chr->kind) {
        case CK_NUS_RX:
            if (sock_fd >= 0) {
                send_frame(sock_fd, FRAME_DATA, data, len);
            }
            break;
        ...
        }
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}
```

- 从 D-Bus 参数取 `ay`（字节数组）。
- 通过 Unix socket 发给 daemon。
- **立刻返回**，不阻塞 BlueZ。

### 板子发 TX

```c
emit_value(chr->path, data, len);
```

发 `PropertiesChanged` 信号，BlueZ 转成 ATT 通知。

前提：手机调过 `StartNotify`。

### 订阅时清旧数据

```c
if (!strcmp(method, "StartNotify")) {
    notifying[chr->kind] = TRUE;
    if (chr->kind == CK_NUS_TX && serial_in->len) {
        logmsg("dropping %zu stale serial bytes on NUS subscribe",
               serial_in->len);
        g_byte_array_set_size(serial_in, 0);
    }
}
```

为什么？手机连上之前，串口可能已经积累了几十 KB。不清掉的话，
一订阅就收到一大堆**旧数据**，用户会以为设备在乱喷。

> 这是一个很容易被忽略的体验问题：**实时流在订阅时应该
> 从“现在”开始，而不是从“缓存”开始。**

### 没人订阅时不发

```c
if (!notifying[chr->kind]) {
    return;
}
```

没人订阅还发通知，浪费带宽和电量，BlueZ 也可能报错。

---

## 22.8 配对与安全

### Just Works

这块板用 `NoInputNoOutput` agent：

```text
手机：请求配对
板子：自动接受
```

适合没有屏幕/键盘的设备。

### 为什么这样设计

- 板子没有显示屏，不能显示 6 位码。
- 用户期望“连上就能用”。
- 串口数据本身不是高敏感。

代价：**任何人在范围内都能配对**。所以：

- 建议开 LAN 令牌。
- 不要把这个桥暴露在不可信环境。
- 真要安全，加物理按键确认（上游固件有，这块板暂未做）。

### 绑定持久化

BlueZ 把 LTK 存在：

```text
/etc/lib/bluetooth/<adapter>/<device>/info
```

但 `/var` 是 tmpfs，所以 `S54bluetooth` 把它 bind mount 到
`/mnt/UDISK/persist/bluetooth/`。

重刷 rootfs 后绑定还在，手机不用重新配对。

---

## 22.9 实验：用手机/工具看 GATT

### 实验 A：扫描

手机装 nRF Connect 或 LightBlue：

1. 扫描。
2. 找 `Linkr Bee V851S`。
3. 看广播里的服务 UUID。

### 实验 B：连接并浏览服务

应该看到三个主服务，以及：

```text
NUS:
  6e400002...  write, write-without-response
  6e400003...  notify

Management:
  4c4b0002...  read
  4c4b0003...  read
  4c4b0004...  write
  4c4b0005...  indicate

Reliable:
  4c4b0011...  write
  4c4b0012...  indicate
  4c4b0013...  read
```

### 实验 C：订阅 NUS TX

1. 点 TX 特性的“订阅”（三个箭头图标）。
2. 目标板发数据。
3. 手机应收到通知。

### 实验 D：写 NUS RX

1. 在 RX 特性里输入文本。
2. 选 “Write” 或 “Write Without Response”。
3. 目标板串口应收到。

### 实验 E：看 MTU

nRF Connect 会显示协商后的 MTU。对比：

```sh
ls /var/run/linkr-bt-mtu/
cat /var/run/linkr-bt-mtu/*
```

---

## 22.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 搜不到 | 广播数据空/没注册 | `ble-adv-data.py`、日志 |
| 搜到但连不上 | 已有一个 central | XR829 限制 |
| 连上但看不到服务 | GATT 没注册 | `RegisterApplication` 日志 |
| 能写不能收 | 没订阅/没 `StartNotify` | 手机端订阅 |
| 收到旧数据 | 订阅时没清缓存 | `serial_in` |
| 数据截断 | MTU 不匹配 | `att_chunk`、MTU 目录 |
| 通知丢 | 发太快/缓冲满 | 节流、`nus_next_us` |
| 经典蓝牙干扰 | 没 `noscan` | `hciconfig` |
| 重连要重新配对 | 绑定没持久化 | bind mount |
| `-49` 错误 | 半开 attach | `killall hciattach` |

---

## 22.11 练习

### 练习 22.1（必做）

用手机工具连接板子，列出：

1. 三个服务的 UUID。
2. NUS RX/TX 的属性和方向。
3. 协商后的 MTU。

### 练习 22.2（必做）

回答：

1. Peripheral 和 Central 有什么区别？
2. Service、Characteristic、Descriptor 是什么关系？
3. notify 和 indicate 的差别？
4. 为什么 RX 是“板子接收”？
5. 为什么订阅时要清空旧数据？
6. `att_chunk = MTU - 3` 里的 3 是什么？

### 练习 22.3（必做）

阅读 `refresh_att_chunk`，回答：

1. 为什么要取所有 peer 的最小 MTU？
2. 为什么上限是 514、下限是 20？
3. 多久刷新一次？为什么？

### 练习 22.4（选做）

用 `btmon` 抓一次连接和 MTU 交换，找到：

```text
Exchange MTU Request
Exchange MTU Response
```

记录双方的值。

---

## 22.12 本章验收

1. 能解释 BLE 的四个角色和这块板的定位。
2. 能画出 Service → Characteristic → Descriptor 的层次。
3. 知道 BlueZ、内核、linkr-ble 的分工。
4. 能解释广播数据为空的真实原因和绕过方案。
5. 理解 MTU 协商、分片、节流。
6. 能排查搜不到、连不上、不通知、截断四类问题。

### 自测题

1. 这块板是 Peripheral 还是 Central？
2. NUS 的 RX 是谁发给谁？
3. notify 需要手机确认吗？
4. 为什么经典蓝牙扫描要关？
5. 默认 ATT payload 是多少？
6. 订阅时为什么要清 `serial_in`？

> 下一章精读 `linkr-ble` 源码：D-Bus 对象、Unix socket、可靠传输。
