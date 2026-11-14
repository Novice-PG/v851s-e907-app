# 第 30 章 Android BLE 客户端：权限、扫描、GATT、重连

> 本章目标：
> 1. 知道 Android 蓝牙权限从 6.0 到 13+ 的演变，以及为什么总在变。
> 2. 会扫描、过滤、连接、发现服务、订阅通知、请求 MTU。
> 3. 理解 GATT 操作的串行性与回调模型，知道为什么“写太快会丢”。
> 4. 能实现重连、后台限制规避、绑定持久化。
> 5. 能解析这块板的 NUS / Management / Reliable 三个服务，排查“搜不到、连不上、收不到”。

---

## 30.1 客户端 vs 浏览器

第 26～27 章用 Web Bluetooth 在网页里连板子。它简单，但有限制：

| | Web Bluetooth | 原生 Android |
|---|---|---|
| 权限 | 浏览器弹窗 | 系统权限 + 运行时授权 |
| 后台 | 不能后台 | 可以有前台服务 |
| 重连 | 页面关了就没了 | 可以持久化、自动重连 |
| 服务过滤 | 只能按 UUID/名字 | 任意过滤 |
| 调试 | 控制台 | `adb logcat`、`btmon` |
| 分发 | 打开网页 | 安装 APK |

需要“手机 App 常驻、自动重连、后台收数据”时，就必须原生。

> 本章不讲 Android 界面，只讲“把 BLE 数据通道做对”。

---

## 30.2 三个服务，九个特性

先把协议摆出来，后面代码都围着它转。

### NUS（兼容现成工具）

```text
Service  6e400001-b5a3-f393-e0a9-e50e24dcca9e
  RX     6e400002-...   write / write-without-response  手机→板
  TX     6e400003-...   notify                          板→手机
```

### Management（控制命令）

```text
Service  4c4b0001-9a7e-4f4e-8b8a-3d6f12a0c001
  PROTO  4c4b0002-...   read
  ID     4c4b0003-...   read
  CMD    4c4b0004-...   write
  RSP    4c4b0005-...   indicate
```

### Reliable UART（带序号）

```text
Service  4c4b0010-9a7e-4f4e-8b8a-3d6f12a0c001
  RX     4c4b0011-...   write
  TX     4c4b0012-...   indicate
  STATE  4c4b0013-...   read
```

### 怎么选

```text
只收发热门串口数据、要兼容   → NUS
要发控制命令、要可靠响应     → Management
要高吞吐 + 顺序保证          → Reliable
```

> 一个 App 可以同时订阅多个。但**同一个数据不要两条通道都发**，
> 否则会重复。板子侧的 `nus_send_tick` 就是在“有可靠订阅时不发 NUS”。

---

## 30.3 权限：一直在变的历史

### Android 6～11

```xml
<uses-permission android:name="android.permission.BLUETOOTH"/>
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN"/>
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION"/>
```

为什么需要定位权限？

> 因为 BLE 扫描能推断位置。系统把它归到“位置”这一类。

### Android 12（API 31）起

新增“附近设备”权限，扫描/连接不再需要定位：

```xml
<uses-permission android:name="android.permission.BLUETOOTH_SCAN"
    android:usesPermissionFlags="neverForLocation"/>
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT"/>
```

`neverForLocation` 是一个声明：**我保证不用扫描结果推断位置**。
声明了它，就可以不申请定位权限。

### Android 13（API 33）起

如果要连**经典蓝牙**（不是 BLE），需要：

```xml
<uses-permission android:name="android.permission.BLUETOOTH_ADVERTISE"/>
```

BLE 仍然主要是 `BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT`。

### 一份能用的清单

```xml
<uses-feature android:name="android.hardware.bluetooth_le" android:required="true"/>

<!-- Android 12+ -->
<uses-permission android:name="android.permission.BLUETOOTH_SCAN"
    android:usesPermissionFlags="neverForLocation"/>
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT"/>

<!-- Android 11 及以下 -->
<uses-permission android:name="android.permission.BLUETOOTH"
    android:maxSdkVersion="30"/>
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN"
    android:maxSdkVersion="30"/>
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION"
    android:maxSdkVersion="30"/>

<!-- 前台服务（后台保持连接） -->
<uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>
<uses-permission android:name="android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE"/>
<uses-permission android:name="android.permission.POST_NOTIFICATIONS"/>
```

### 运行时申请

```kotlin
private val blePermissions = if (Build.VERSION.SDK_INT >= 31) {
    arrayOf(
        Manifest.permission.BLUETOOTH_SCAN,
        Manifest.permission.BLUETOOTH_CONNECT,
    )
} else {
    arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
}

private val launcher = registerForActivityResult(
    ActivityResultContracts.RequestMultiplePermissions()
) { result ->
    val denied = result.filterValues { !it }.keys
    if (denied.isNotEmpty()) {
        showPermissionRationale(denied)
    } else {
        startScan()
    }
}
```

### 常见的权限错误

| 现象 | 原因 |
|---|---|
| 扫描返回空 | 没授 `BLUETOOTH_SCAN` 或定位 |
| 连接抛 `SecurityException` | 没授 `BLUETOOTH_CONNECT` |
| Android 12 上突然要定位 | 没写 `neverForLocation` |
| 后台被断 | 没前台服务 |
| 通知不显示 | Android 13 没 `POST_NOTIFICATIONS` |

> 权限问题是 Android BLE 的“第一道墙”。**先确认权限，再怀疑代码。**

---

## 30.4 扫描

### 过滤器

不要扫所有设备，用服务 UUID 过滤：

```kotlin
val filter = ScanFilter.Builder()
    .setServiceUuid(ParcelUuid(UUID.fromString("4c4b0001-9a7e-4f4e-8b8a-3d6f12a0c001")))
    .build()

val settings = ScanSettings.Builder()
    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
    .build()

val scanner = bluetoothAdapter.bluetoothLeScanner
scanner.startScan(listOf(filter), settings, scanCallback)
```

### 为什么用服务 UUID 而不是名字

```text
名字  → 可改、可重复、可能被截断
UUID  → 固定、唯一、写在广播里
```

这块板的广播里带 Management 服务 UUID，所以用它过滤最可靠。

### 扫描回调

```kotlin
private val scanCallback = object : ScanCallback() {
    override fun onScanResult(callbackType: Int, result: ScanResult) {
        val name = result.scanRecord?.deviceName ?: "(no name)"
        val rssi = result.rssi
        val addr = result.device.address
        Log.d(TAG, "found $name $addr rssi=$rssi")
        // 更新列表，去重
    }

    override fun onScanFailed(errorCode: Int) {
        Log.e(TAG, "scan failed: $errorCode")
    }
}
```

`onScanFailed` 的常见错误码：

| 码 | 含义 |
|---|---|
| 1 | 已经开始扫描（重复调用） |
| 2 | 应用注册太多扫描 |
| 3 | 内部错误 |
| 4 | 权限/位置服务未开 |

### 扫描要停

```kotlin
handler.postDelayed({ scanner.stopScan(scanCallback) }, 10_000)
```

一直扫会耗电，系统也会限制。

### 这块板的坑

第 22 章讲过：Allwinner 4.9 内核把 `LE_SET_ADV_DATA` 注释掉了，
BlueZ 的广播数据到不了控制器。板子用 `ble-adv-data.py` 绕过。

对客户端的影响：

```text
如果绕过脚本没跑 → 广播是空的 → 按 UUID 过滤扫不到
```

排查：

```sh
# 板子上
ps | grep ble-adv-data
tail /var/log/linkr-ble.log
```

或者**不用过滤器**先扫一遍，看能不能看到设备名/地址。能看到但
没有服务 UUID，就说明广播数据没写进去。

> 这是“底层有 bug，客户端要能分辨”。

---

## 30.5 连接

### 两种连接方式

```kotlin
// 1. 自动选择（推荐）
device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)

// 2. 指定 LE
device.connectGatt(context, false, gattCallback,
    BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_1M_MASK)
```

第二个参数 `autoConnect`：

```text
false  直接连接，超时较短，适合“用户点了就等”
true   系统排队，设备出现就自动连，适合“后台常驻”
```

### 回调是有状态的

```kotlin
private val gattCallback = object : BluetoothGattCallback() {
    override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
        if (status != BluetoothGatt.GATT_SUCCESS) {
            Log.w(TAG, "connect status=$status")
            gatt.close()
            scheduleReconnect()
            return
        }

        when (newState) {
            BluetoothProfile.STATE_CONNECTED -> {
                gatt.requestMtu(517)     // 下一步在 onMtuChanged
            }
            BluetoothProfile.STATE_DISCONNECTED -> {
                gatt.close()
                scheduleReconnect()
            }
        }
    }
}
```

关键点：

1. **`status != GATT_SUCCESS` 时 `newState` 可能仍是 CONNECTED**。
   必须先看 `status`。
2. **每次都要 `gatt.close()`**，否则会泄漏底层连接，几次之后
   Android 会拒绝新连接。
3. **状态机**：连接成功不等于可用，还要发现服务、订阅、协商 MTU。

### 133 这个错误码

```text
status = 133 (GATT_ERROR)
```

这是 Android BLE 最著名的“万能错误”。常见原因：

```text
- 上一个 GATT 没 close
- 连接太频繁
- 设备刚断又连
- 服务发现失败
- 蓝牙栈状态混乱
```

处理原则：

```text
1. 立刻 close
2. 等 1～2 秒再重连
3. 连续失败多次后提示用户重开蓝牙/重配对
```

**不要**在 133 之后立刻 `connectGatt`，会连锁失败。

---

## 30.6 MTU

### 为什么必须请求

默认 MTU 23 → 每次只能发 20 字节。对串口数据来说太慢。

```kotlin
override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
    if (status == BluetoothGatt.GATT_SUCCESS) {
        Log.d(TAG, "mtu=$mtu")
        maxPayload = mtu - 3
    } else {
        Log.w(TAG, "mtu request failed: $status")
        maxPayload = 20
    }
    gatt.discoverServices()          // 下一步
}
```

### 上限

```text
Android 客户端请求 517
板子（BlueZ）通常回 247 或按自己的配置
最终值 = min(双方)
```

实际观察：

```text
很多 Android 机：247
部分设备：512/517
老设备：23
```

### 用哪个值

```kotlin
maxPayload = (mtu - 3).coerceIn(20, 514)
```

- `-3`：ATT 头。
- 下限 20：默认安全值。
- 上限 514：BLE 单包最大 ATT payload。

### 和板子的配合

板子侧每秒读 `/var/run/linkr-bt-mtu/<peer>` 并取最小值：

```c
att_chunk = mtu - 3;   // 20..514
```

所以：

```text
客户端请求 517、协商 247 → 板子按 244 发 → 客户端能收
客户端发 244 → 板子按自己的 att_chunk 收
```

> 双方各算各的，但都用 `MTU-3`，所以自然一致。

### 常见错误

| 现象 | 原因 |
|---|---|
| 数据被截断 | 客户端按 244 发，实际 MTU 23 |
| `onMtuChanged` 不来 | 连接没真正建立/服务没发现 |
| MTU 一直是 23 | 设备不支持/系统限制 |

> **发之前一定用协商后的值，不要硬编码。**

---

## 30.7 发现服务、订阅

### 发现服务

```kotlin
override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
    if (status != BluetoothGatt.GATT_SUCCESS) {
        Log.e(TAG, "discover failed: $status")
        gatt.disconnect()
        return
    }

    val nus = gatt.getService(UUID.fromString(UUID_NUS))
    val mgmt = gatt.getService(UUID.fromString(UUID_MGMT))
    val rel = gatt.getService(UUID.fromString(UUID_REL))

    nusRx = nus?.getCharacteristic(UUID.fromString(UUID_NUS_RX))
    nusTx = nus?.getCharacteristic(UUID.fromString(UUID_NUS_TX))
    mgmtCmd = mgmt?.getCharacteristic(UUID.fromString(UUID_MGMT_CMD))
    mgmtRsp = mgmt?.getCharacteristic(UUID.fromString(UUID_MGMT_RSP))

    enableNotifications(gatt, nusTx)
}
```

### 订阅通知（写 CCCD）

```kotlin
private fun enableNotifications(gatt: BluetoothGatt, ch: BluetoothGattCharacteristic?) {
    if (ch == null) return
    gatt.setCharacteristicNotification(ch, true)

    val cccd = ch.getDescriptor(CCCD_UUID)
    cccd?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
    gatt.writeDescriptor(cccd)
}
```

三个值：

```text
ENABLE_NOTIFICATION_VALUE       0x01,0x00   notify
ENABLE_INDICATION_VALUE         0x02,0x00   indicate
DISABLE_NOTIFICATION_VALUE      0x00,0x00
```

**notify 用 0x01，indicate 用 0x02。** 写错就“订阅成功但收不到”。

### 接收数据

```kotlin
override fun onCharacteristicChanged(
    gatt: BluetoothGatt,
    ch: BluetoothGattCharacteristic,
    value: ByteArray,
) {
    when (ch.uuid) {
        UUID.fromString(UUID_NUS_TX) -> handleSerial(value)
        UUID.fromString(UUID_MGMT_RSP) -> handleMgmtFrame(value)
    }
}
```

注意：API 33 起推荐带 `value` 参数的重载。旧写法
`ch.value` 仍然可用但已废弃。**别混用，容易拿到旧数据。**

### 一次只能有一个 GATT 操作

这是 Android BLE 最重要的约束：

```text
写特性、读特性、写描述符、请求 MTU、发现服务
——全都必须串行
```

如果同时发两个：

```kotlin
gatt.writeCharacteristic(a)
gatt.writeCharacteristic(b)   // 返回 false，被丢弃
```

所以需要一个**操作队列**：

```kotlin
class GattQueue(private val gatt: BluetoothGatt) {
    private val queue = ArrayDeque<GattOp>()
    private var busy = false

    @Synchronized
    fun enqueue(op: GattOp) {
        queue.addLast(op)
        pump()
    }

    @Synchronized
    fun onComplete() {
        busy = false
        pump()
    }

    @Synchronized
    private fun pump() {
        if (busy) return
        val op = queue.removeFirstOrNull() ?: return
        busy = true
        op.execute(gatt)
    }
}
```

每次 `onCharacteristicWrite` / `onDescriptorWrite` / `onMtuChanged`
里调用 `queue.onComplete()`。

> 没有队列，就会出现“有时候能写、有时候丢”。这是新手最常见的
> BLE bug。

---

## 30.8 分片发送

### 写长度

```kotlin
fun writeChunk(gatt: BluetoothGatt, ch: BluetoothGattCharacteristic, data: ByteArray) {
    val n = min(data.size, maxPayload)
    val chunk = data.copyOfRange(0, n)

    if (Build.VERSION.SDK_INT >= 33) {
        gatt.writeCharacteristic(ch, chunk, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
    } else {
        ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        ch.value = chunk
        gatt.writeCharacteristic(ch)
    }
}
```

### 两种写类型

| 类型 | 有响应 | 吞吐 | 可靠 |
|---|---|---|---|
| `WRITE_TYPE_DEFAULT` | 有 | 低 | 高 |
| `WRITE_TYPE_NO_RESPONSE` | 无 | 高 | 可能丢 |

串口数据用哪个？

```text
NUS RX        → NO_RESPONSE 提高吞吐（丢了就丢了，像串口）
Management CMD → DEFAULT（必须确认）
Reliable RX    → DEFAULT（要顺序）
```

### Management 的 12 字节头

客户端要自己构造：

```text
0  2  'L','K'
2  1  版本 1
3  1  类型 1（请求）
4  4  reqid（小端）
8  2  载荷长度（小端）
10 2  flags
12 N  载荷（UTF-8 命令）
```

```kotlin
fun buildMgmtFrame(reqid: Int, payload: ByteArray): ByteArray {
    val out = ByteArray(12 + payload.size)
    out[0] = 'L'.code.toByte()
    out[1] = 'K'.code.toByte()
    out[2] = 1
    out[3] = 1
    out[4] = (reqid and 0xff).toByte()
    out[5] = ((reqid shr 8) and 0xff).toByte()
    out[6] = ((reqid shr 16) and 0xff).toByte()
    out[7] = ((reqid shr 24) and 0xff).toByte()
    out[8] = (payload.size and 0xff).toByte()
    out[9] = ((payload.size shr 8) and 0xff).toByte()
    out[10] = 0
    out[11] = 0
    payload.copyInto(out, 12)
    return out
}
```

然后按 `maxPayload` 分片发送。

### 接收分片

服务端响应也是分片的。客户端必须累积：

```kotlin
private val mgmtBuf = ByteArrayOutputStream()
private var need = -1
private var reqid = 0

fun handleMgmtFrame(chunk: ByteArray) {
    mgmtBuf.write(chunk)
    val buf = mgmtBuf.toByteArray()

    if (need < 0) {
        if (buf.size < 12) return
        if (buf[0] != 'L'.code.toByte() || buf[1] != 'K'.code.toByte()) {
            mgmtBuf.reset(); return
        }
        need = 12 + ((buf[8].toInt() and 0xff) or ((buf[9].toInt() and 0xff) shl 8))
        reqid = (buf[4].toInt() and 0xff) or
                ((buf[5].toInt() and 0xff) shl 8) or
                ((buf[6].toInt() and 0xff) shl 16) or
                ((buf[7].toInt() and 0xff) shl 24)
    }

    if (buf.size >= need) {
        val payload = buf.copyOfRange(12, need)
        onResponse(reqid, String(payload, Charsets.UTF_8))
        mgmtBuf.reset()
        need = -1
    }
}
```

**这是流式协议的标准写法**，和第 23 章 Unix socket 的循环一样。

### Reliable 的序号

```text
0  2  'L','R'
2  1  版本
3  1  保留
4  4  seq
8  2  长度
10 2  保留
12 N  数据
```

客户端检查 `seq`：

```kotlin
if (seq == expected) { deliver(data); expected++ }
else if (seq == lastSeq) { /* 重复，丢弃 */ }
else { /* 缺口，记录/请求重传 */ }
```

> “可靠”在这里是**顺序 + 去重**，不是自动重传。

---

## 30.9 后台与重连

### Android 的后台限制

```text
- 息屏后 CPU 可能休眠
- Doze 模式限制网络/扫描
- 长时间无前台服务会被杀
```

### 前台服务

```kotlin
class BridgeService : Service() {
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = buildNotification()
        startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        connect()
        return START_STICKY
    }
}
```

`START_STICKY`：被系统杀掉后尝试重建。

### 重连策略

```kotlin
private var backoff = 1000L

fun scheduleReconnect() {
    handler.postDelayed({
        if (shouldReconnect) {
            backoff = (backoff * 2).coerceAtMost(30_000L)
            connect()
        }
    }, backoff)
}

fun onConnected() {
    backoff = 1000L        // 成功就重置
}
```

指数退避：

```text
1s → 2s → 4s → 8s → 16s → 30s（封顶）
```

> 固定 1 秒重连会在板子重启时疯狂重试，反而更容易 133。

### 自动重连 vs 手动

```kotlin
connectGatt(context, true, callback, TRANSPORT_LE)
```

`autoConnect=true` 让系统在设备出现时自动连，但：

- 可能很慢（几十秒）。
- 不同厂商行为不一致。
- 不适合“用户点了立即要结果”。

推荐：

```text
前台/用户操作 → autoConnect=false
后台常驻     → 前台服务 + autoConnect=true 或自己的退避重连
```

### 绑定（bond）持久化

如果要加密连接，需要配对：

```kotlin
device.createBond()
```

绑定信息存在系统里，重连不需要重新配对——**前提是板子的
BLE MAC 稳定**。

第 18 章讲过：`S54bluetooth` 从 Wi-Fi MAC 派生 BT MAC：

```sh
btmac="22:22:$(echo "$wmac" | cut -d: -f3-6)"
```

所以重刷固件后地址不变，手机绑定继续有效。

> 如果不做这件事，每次刷机手机都要“忘记设备再重配”，体验极差。

### 一个连接状态机

```text
IDLE
  │ connect()
  ▼
CONNECTING ──失败──► BACKOFF ──延时──► CONNECTING
  │ 成功
  ▼
DISCOVERING ──失败──► DISCONNECT ──► BACKOFF
  │ 成功
  ▼
SUBSCRIBING ──失败──► DISCONNECT
  │ 成功
  ▼
READY ──断开──► BACKOFF
```

每一步的回调里只推进一个状态。**不要在一个回调里做三步。**

---

## 30.10 排查方法

### `adb logcat`

```sh
adb logcat -s BeeBle:D BluetoothGatt:E
```

### `btmon`（需要 root / 开发机）

在手机上看不到 HCI，但在 Linux 上可以看板子侧：

```sh
btmon -w /tmp/bt.snoop
```

然后分析：

```text
Exchange MTU Request/Response
Write Request / Write Response
Handle Value Notification
```

### 常见现象对照

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 扫不到 | 广播空/权限/定位关 | 不过滤扫一遍、看板子日志 |
| 连不上 | 已被占用/距离/133 | 板子是否已有 central |
| 连上但无服务 | 服务发现失败 | `onServicesDiscovered` |
| 能写收不到 | 没订阅/CCCD 错 | 用 0x01 还是 0x02 |
| 收到旧数据 | 订阅时板子清了缓存但 App 没清 | App 侧也清缓冲 |
| 数据截断 | MTU 用错 | 打印 `maxPayload` |
| 写丢 | 并发 GATT 操作 | 加队列 |
| 频繁断 | 重连太急/信号弱 | 退避、看 RSSI |
| 后台断 | 没前台服务 | Doze、电池优化 |
| 重连要重配 | MAC 变了/绑定没存 | 查板子 MAC |

### 一个最小自检流程

```text
1. 权限都授了吗？
2. 蓝牙开着吗？定位开着吗（旧系统）？
3. 不过滤扫描能看到设备吗？
4. 能看到服务 UUID 吗？
5. 连上后 status 是 0 吗？
6. onServicesDiscovered 成功吗？
7. MTU 请求成功吗？值是多少？
8. CCCD 写的是 0x01 还是 0x02？
9. 写操作有排队吗？
10. 板子侧 linkr-ble 日志怎么说？
```

> 按这十步走，90% 的问题能定位。**别一上来就改代码。**

---

## 30.11 实验

### 实验 A：不过滤扫描

写一个只调用 `startScan(null, settings, callback)` 的页面/App，
打印所有设备的地址、名字、RSSI、服务 UUID。

对比“按 UUID 过滤”的结果，确认广播数据是否真的存在。

### 实验 B：打印 MTU

在 `onMtuChanged` 里 `Log.d`，记录：

```text
requested 517 → granted ?
```

用三台不同手机对比。

### 实验 C：制造 133

1. 连上后立刻 `gatt.close()` 再 `connectGatt`。
2. 观察 `onConnectionStateChange` 的 `status`。
3. 加上 1 秒退避后重试，记录差异。

### 实验 D：验证操作队列

不加队列，连续调用两次 `writeCharacteristic`，看第二次返回值。
再加队列，确认两次都成功。

### 实验 E：订阅 NUS 与 Reliable

分别订阅，用板子的串口发数据，统计：

```text
- 每秒收到多少字节
- 有没有重复/缺口
```

---

## 30.12 练习

### 练习 30.1（必做）

写一个 Android 最小工程：

- 申请权限。
- 扫描 Management 服务 UUID。
- 连接并打印 MTU。
- 发现服务并订阅 NUS TX。
- 把收到的字节打印到日志。

### 练习 30.2（必做）

回答：

1. Android 12 之后为什么不再需要定位权限？
2. `neverForLocation` 是什么？
3. `connectGatt` 的 `autoConnect` 参数影响什么？
4. 为什么 `status=133` 后不能立刻重连？
5. notify 和 indicate 的 CCCD 值分别是什么？
6. 为什么所有 GATT 操作必须串行？

### 练习 30.3（必做）

实现一个 `GattQueue`，要求：

- 任意时刻只有一个操作在执行。
- 每个操作完成后自动取下一个。
- 超时后放弃并清空。

用日志验证：连续入队 5 个写操作，全部成功。

### 练习 30.4（必做）

手写 `buildMgmtFrame` 和接收分片逻辑，发一条 `@i?`，
解析响应。要求处理：

- 头部跨两片。
- 载荷跨多片。
- 非 `LK` 数据丢弃。

### 练习 30.5（选做）

实现前台服务 + 指数退避重连，让 App 息屏 10 分钟后
仍能自动连上。记录每次重连的时间间隔。

---

## 30.13 本章验收

1. 能说清 Android 蓝牙权限的版本差异。
2. 会按服务 UUID 扫描，并解释“扫不到”的两种原因。
3. 能画出连接状态机，知道每个回调推进哪一步。
4. 理解 MTU 协商，并用 `MTU-3` 分片。
5. 会写操作队列，避免并发 GATT 操作。
6. 能实现重连、前台服务、绑定持久化。
7. 能按十步自检流程定位问题。

### 自测题

1. `BLUETOOTH_SCAN` 和 `BLUETOOTH_CONNECT` 分别管什么？
2. 默认 ATT MTU 和 payload 各是多少？
3. `status=133` 通常怎么处理？
4. 为什么 `writeCharacteristic` 会返回 false？
5. Management 帧头多少字节、魔数是什么？
6. 板子的 BLE MAC 为什么要固定？

> 下一章收口：同一套 `@` 命令如何服务 Web、BLE、CLI、TUI。
