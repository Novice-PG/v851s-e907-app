# 第 27 章 linkr-bee Web 适配：五个补丁为什么存在

> 本章目标：
> 1. 知道“把别人的前端拿来用”时，改动应该怎么做才可维护。
> 2. 逐个看懂 5 个补丁要解决的问题，以及每个方案的副作用。
> 3. 理解“安全上下文”缺失时，怎么用降级实现补齐功能。
> 4. 理解 LAN 与 BLE 两种传输在能力上的差异，以及 UI 应该怎么表达。
> 5. 能写出补丁、能应用补丁、能判断什么时候该改上游、什么时候该本地打补丁。

---

## 27.1 为什么是“补丁”而不是“改代码”

### 现状

```text
linkr-bee/                 完整上游代码（含 web/）
linkr-bee/patches/         5 个 .patch
linkr-bee/apply-web.sh     把 patch 应用到 web/
```

构建时：

```sh
./linkr-bee/apply-web.sh
```

脚本做的是：

```text
1. 复制一份干净的 web/ 到临时目录
2. 依次 patch -p1 < patches/0001...
3. 失败就停
4. 把结果放进 rootfs
```

### 为什么不直接改 `web/`

| 方式 | 好处 | 坏处 | 什么时候用 |
|---|---|---|---|
| 直接改源码 | 简单、直观 | 上游一更新就冲突，且看不出改了哪 | 你自己的项目 |
| 补丁 | 改动可审计、可叠加、可丢弃 | 需要维护 patch 上下文行 | 第三方代码 |
| fork 后长期维护 | 完全自由 | 同步上游成本高 | 深度定制 |

> 原则：**能用补丁就用补丁，能用上游配置就不用补丁，必须改上游时
> 先提 PR。**

这 5 个补丁都是“V851S 这块板的特殊环境”导致的，不是上游的 bug。
所以用本地补丁是对的。

### 补丁的脆弱点

```text
@@ -5851,6 +5851,28 @@
```

`-5851` 是原文件行号。上游只要在前面加/删几行，行号就漂了。
`patch` 有模糊匹配能力，但漂得太多就失败。

所以：

```text
1. 补丁要小（一个补丁只做一件事）
2. 上下文行要够（默认 3 行，必要时手动加）
3. 构建时必须失败退出，不能“跳过”
```

### 应用脚本必须“失败就停”

```sh
#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

for p in patches/*.patch; do
    echo "applying $p"
    patch -p1 --forward --batch <"$p"
done
```

- `set -e`：任何一步失败立刻退出。
- `--forward`：已经打过就跳过，不无限叠加。
- `--batch`：不交互提问。

> 最危险的是“补丁失败但构建继续”，最后烧进去一个半新半旧的网页。

---

## 27.2 补丁一：`crypto.randomUUID` 兜底

### 现象

用 `http://<board>/` 打开页面，控制台报：

```text
TypeError: crypto.randomUUID is not a function
```

然后 agent 面板、设备执行器这类模块初始化失败。

### 原因

`crypto.randomUUID` 属于 **secure context only** API。

```text
https://...          → 有
http://localhost     → 有（localhost 被特殊对待）
http://192.168.0.121 → 没有
```

而这块板 LAN 的默认入口是明文 `http://<board>/`，于是：

```js
crypto.randomUUID()   // TypeError
```

### 为什么不用 `https` 就完事

因为：

- 证书是自签名的，第一次要手动接受。
- 有些脚本/工具用明文 `ws://` 工作。
- 不能强制所有用户都走 8443。

所以要么“全部改 HTTPS”，要么“给 HTTP 补上这个 API”。

### 补丁做了什么

在 `<head>` 里、所有模块之前插入一段脚本：

```js
(function () {
  var c = window.crypto;
  if (!c || typeof c.randomUUID === "function") return;   // 已有就不动
  function uuid() {
    var b = new Uint8Array(16);
    if (typeof c.getRandomValues === "function") c.getRandomValues(b);
    else for (var i = 0; i < 16; i++) b[i] = Math.floor(Math.random() * 256);
    b[6] = (b[6] & 0x0f) | 0x40;    // version 4
    b[8] = (b[8] & 0x3f) | 0x80;    // variant 10xx
    var h = Array.prototype.map.call(b, function (x) {
      return (x + 256).toString(16).slice(1);
    }).join("");
    return h.slice(0, 8) + "-" + h.slice(8, 12) + "-" + h.slice(12, 16) +
      "-" + h.slice(16, 20) + "-" + h.slice(20);
  }
  try {
    Object.defineProperty(Object.getPrototypeOf(c), "randomUUID",
      { value: uuid, configurable: true });
  } catch (e) {
    c.randomUUID = uuid;
  }
})();
```

逐段：

1. **先检查已有实现**。HTTPS 下原生函数存在，直接返回，零影响。
2. **`getRandomValues` 在 HTTP 页面是可用的**（它不属于 secure-only）。
   所以随机性来源仍然安全。
3. **`b[6]`/`b[8]`** 是 UUID v4 的版本位和变体位。
4. **`Object.defineProperty` 到原型上**，比直接赋值更接近原生行为。
5. **`catch` 兜底**：某些浏览器不允许改原型。

### 为什么不是 `Math.random` 直接拼

有 `getRandomValues` 就用它。`Math.random` 不是密码学安全的，
作为最后兜底可以，但不该是首选。

### 为什么要定义在原型上

有的代码会这样写：

```js
window.crypto.randomUUID()
```

也有代码会：

```js
Crypto.prototype.randomUUID.call(crypto)
```

定义在原型上两种情况都能工作。

### 副作用

- 全局修改了浏览器 API。页面内所有代码都会看到“假的”`randomUUID`。
- 但语义一致（都是 v4 UUID），风险很低。
- 只在“原生不存在”时生效。

> 这类“polyfill”是标准做法。关键是：**只在缺失时生效，不改已有行为。**

---

## 27.3 补丁二：LAN 模式下的管理功能提示

### 现象

用户用 LAN（`ws://`）连上，打开“WiFi & WebDAV”设置卡：

- 输入框在。
- 按钮全部灰掉。
- 没有任何解释。

用户以为“坏了”。

### 原因

Wi-Fi 和 WebDAV 的设置命令走 **BLE Management 服务**：

```text
手机 ──BLE──► Management Service ──Unix socket──► daemon ──► wctl / WebDAV
```

LAN 的 WebSocket 通道虽然也能执行 `@` 命令，但：

- 部分管理能力（如 `set_wifi`）依赖 daemon 的 BLE 侧状态/权限模型。
- 页面原本就是为 BLE 设计的管理面板。

所以 LAN 下这些控件被 `disabled`，但 UI 没告诉用户为什么。

### 补丁做了什么

**HTML**：加一行提示，默认隐藏：

```html
<p id="managementBleOnly" class="field-hint" role="note"
   data-i18n="managementBleOnly" hidden>
  WiFi and WebDAV settings use the BLE management channel.
  Connect over BLE to change them.
</p>
```

**JS（元素表）**：

```js
managementBleOnly: $("managementBleOnly"),
```

**JS（文案表，中英各一份）**：

```js
managementBleOnly: "WiFi and WebDAV settings use the BLE management channel. ...",
```

```js
managementBleOnly: "WiFi 与 WebDAV 设置通过 BLE 管理通道下发，请改用 BLE 连接后修改。",
```

**JS（显示逻辑）**：

```js
/* On the LAN transport the management service is not carried, so these
 * forms stay disabled; say why instead of leaving dead controls. */
if (elements.managementBleOnly) elements.managementBleOnly.hidden =
    state.mode !== "ws";
```

即：

```text
mode === "ws"  → 显示提示
mode === "ble" → 隐藏
```

### 这个补丁的真正价值

它修的不是功能，是**可理解性**。

```text
坏 UI：按钮灰着，用户不知道为什么
好 UI：按钮灰着，旁边写清楚“为什么”和“怎么办”
```

> “死控件”是体验缺陷里最常见、最容易修、最容易被忽略的一种。

### 顺带改了缓存版本号

```diff
-    <link rel="stylesheet" href="./style.css?v=20261030-full-labels" />
+    <link rel="stylesheet" href="./style.css?v=20261102-cli" />
...
-    <script type="module" src="./app.js?v=20261030-full-labels"></script>
+    <script type="module" src="./app.js?v=20261113-lan-hint"></script>
```

改了 `app.js` 就必须改版本号，否则浏览器继续用缓存里的旧 JS，
用户看不到提示。

> 记住：**改 JS 不改 `?v=`，等于没改。**

---

## 27.4 补丁三：SHA-256 纯 JS 兜底

### 现象

明文 HTTP 下：

```text
下载校验失败
TypeError: Cannot read properties of undefined (reading 'digest')
```

涉及两处：

- `local_download.js`：下载文件后算 SHA-256 校验。
- `agent_task_state.js`：给任务输入算指纹。

### 原因

`crypto.subtle` 同样是 secure context only：

```js
crypto.subtle.digest("SHA-256", bytes)   // HTTP 下 subtle 是 undefined
```

### 方案

新增一个模块 `web/sha256_fallback.js`：

```js
export async function sha256Hex(value) {
  const bytes = toBytes(value);
  const subtle = globalThis.crypto && globalThis.crypto.subtle;
  if (subtle && typeof subtle.digest === "function") {
    return toHex(new Uint8Array(await subtle.digest("SHA-256", bytes)));
  }
  return toHex(sha256(bytes));
}
```

两条路径：

```text
有 crypto.subtle  → 用原生（快，通常有硬件加速）
没有              → 用纯 JS（慢，但正确）
```

### 纯 JS 实现的关键结构

```js
const K = new Uint32Array([ ...64 个常量... ]);
const rotr = (x, n) => ((x >>> n) | (x << (32 - n))) >>> 0;
```

SHA-256 三步：

```text
1. 填充：末尾加 0x80，补 0 到 56 mod 64，最后 8 字节写原始比特长度（大端）
2. 分块：每 64 字节一块，扩展成 64 个 32 位字
3. 压缩：64 轮，更新 8 个状态字
```

填充：

```js
const blocks = ((len + 9 + 63) >> 6) * 64;
const padded = new Uint8Array(blocks);
padded.set(bytes);
padded[len] = 0x80;
const view = new DataView(padded.buffer);
view.setUint32(blocks - 8, Math.floor(len / 0x20000000), false);  // 高 32 位
view.setUint32(blocks - 4, (len * 8) >>> 0, false);              // 低 32 位
```

> `len * 8` 可能超过 32 位。高 32 位用 `len / 0x20000000`（即
> `len >> 29`）取。写错会导致大于 512 MB 的输入算错——虽然
> 实际用不到，但正确性不能靠“用不到”。

扩展：

```js
for (let i = 16; i < 64; i++) {
  const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
  const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
  w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
}
```

压缩：

```js
for (let i = 0; i < 64; i++) {
  const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
  const ch = (e & f) ^ (~e & g);
  const t1 = (k + S1 + ch + K[i] + w[i]) >>> 0;
  const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
  const maj = (a & b) ^ (a & c) ^ (b & c);
  const t2 = (S0 + maj) >>> 0;

  k = g; g = f; f = e; e = (d + t1) >>> 0;
  d = c; c = b; b = a; a = (t1 + t2) >>> 0;
}
```

### JavaScript 位运算的两个坑

**坑一：`>>> 0` 不能省**

JS 的位运算结果是**有符号 32 位**。`0x80000000 | 0` 是负数。
所有需要保持“无符号 32 位”的地方都要 `>>> 0`。

**坑二：加法会超过 32 位**

```js
a + b + c   // 可能是 2^53 内的普通数字
```

所以在每轮加法后 `>>> 0` 截断。代码里 `t1`/`t2`/`e`/`a`/`h[i]`
都做了截断。

### 调用方怎么改

`local_download.js`：

```diff
-  const actual = [...new Uint8Array(await crypto.subtle.digest("SHA-256", content))]
-    .map(b => b.toString(16).padStart(2, "0")).join("");
+  const actual = await sha256Hex(content);
```

`agent_task_state.js`：

```diff
-  const digest = await crypto.subtle.digest('SHA-256', bytes);
-  return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('');
+  return sha256Hex(bytes);
```

注意第二个原来**没有 `await`**，改成 `return sha256Hex(bytes)` 是因为
函数本身是 `async`，直接返回 Promise 即可。

### 怎么验证实现对

三种方法：

```text
1. 和 openssl 对比：
   echo -n "abc" | openssl dgst -sha256
   → ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad

2. 和原生 crypto.subtle 对比（HTTPS 页面或 Node）：
   同一份字节，两个结果必须相同

3. 边界长度：0、55、56、63、64、65 字节
   这些是填充边界，最容易写错
```

> 写密码学实现，“看起来对”没有意义。**必须有已知答案测试。**

### 性能取舍

纯 JS SHA-256 大约几 MB/s 到几十 MB/s，取决于引擎。

```text
128 MB 下载校验：纯 JS 可能要几秒到几十秒
```

所以策略是“**有原生就用原生**”，纯 JS 只是兜底。

> 这也是为什么主入口还是推荐 HTTPS：不只是蓝牙，性能和功能都更好。

---

## 27.5 补丁四：HTTPS 页面推导 `wss://`

### 现象

用 `https://192.168.0.121:8443/` 打开页面，在“设备地址”里填
`192.168.0.121`，点连接：

- 状态一直是“连接中/灰的”。
- 浏览器控制台报：

```text
Mixed Content: The page at 'https://...' was loaded over HTTPS,
but attempted to connect to the insecure WebSocket endpoint 'ws://...'.
This request has been blocked.
```

### 原因

原来的逻辑：

```js
const url = /^wss?:\/\//.test(host) ? host : `ws://${host}/ws`;
```

在 HTTPS 页面里连 `ws://` = **混合内容**，浏览器直接拒绝。

### 补丁做了什么

抽出一个函数：

```js
function websocketUrl(host) {
  if (/^wss?:\/\//.test(host)) return host;      // 用户写了完整 URL，尊重
  const securePage = location.protocol === "https:";
  let target = host;
  if (securePage) {
    if (host === location.hostname || host === location.host) {
      target = location.host;                    // 同一台：直接复用 origin
    } else if (!host.includes(":") && location.port) {
      target = `${host}:${location.port}`;       // 别的机器：带上同样的端口
    }
    return `wss://${target}/ws`;
  }
  return `ws://${target}/ws`;
}
```

### 四种情况

| 页面 | 输入 | 结果 |
|---|---|---|
| http | `192.168.0.121` | `ws://192.168.0.121/ws` |
| https | `192.168.0.121`（同机） | `wss://192.168.0.121:8443/ws` |
| https | `192.168.0.190`（异机） | `wss://192.168.0.190:8443/ws` |
| 任意 | `wss://x/ws` | 原样使用 |

### 为什么用 `location.host` 而不是自己拼

```js
location.host  // "192.168.0.121:8443"
```

它已经包含主机和端口。自己拼字符串容易漏端口、漏 IPv6 方括号。

### 为什么异机要“带上同样的端口”

因为这块板每个实例都用 8443 做 TLS。用户输入 `192.168.0.190`
时，意图是“另一块同样的板”。带上 8443 是合理默认值。

> 这是一种“约定优于配置”：同型号设备用同样的端口布局。

### 副作用

- 如果用户真的想要“HTTPS 页面连明文 `ws://` 别的主机”，现在做不到了。
- 但那种情况本来就会被浏览器拦截，所以不算损失。
- 用户仍可以显式写 `ws://...` 吗？会被 `^wss?://` 匹配后原样返回，
  然后被浏览器拦。这是浏览器的规则，不是页面的规则。

> 混合内容是**浏览器强制**的，不是应用能绕过的。只能顺着它走。

---

## 27.6 补丁五：LAN 诊断自动拉取

### 现象

通过 LAN 连上后：

- 诊断面板空的。
- Wi-Fi 状态空的。
- 令牌输入框空的。
- 必须手动点“刷新诊断”才有数据。

而 BLE 模式下是自动的。

### 原因

原来的判断：

```js
function requestDeviceState() {
  if (!state.connected || state.mode !== "ble" || !state.mgmtReady) {
    return Promise.resolve();
  }
  ...
}
```

`state.mode !== "ble"` 直接返回 → **LAN 永远不拉取**。

因为历史上这些数据只能通过 BLE Management 服务拿到。

但后来 daemon 的 WebSocket 也支持同一套 `@` 命令了，所以这个
限制过时了。

### 补丁做了什么

**一、放开 LAN**

```js
function requestDeviceState() {
  if (!state.connected) {
    return Promise.resolve();
  }
  if (state.mode === "ble" && !state.mgmtReady) {
    return Promise.resolve();
  }
  const requests = [requestDiagnostics()];
  const lan = state.mode === "ws";
  if (lan || hasManagementCapability(MGMT_CAP_WIFI)) {
    requests.push(sendControl("@w?"));
  }
  if (lan || hasManagementCapability(MGMT_CAP_WEBSOCKET)) {
    if (lan || canSerialInput()) requests.push(sendControl("@s?"));
  }
  return Promise.all(requests);
}
```

关键点：

```text
BLE  → 仍需 mgmtReady + 能力位
LAN  → 直接允许（因为 WS 就是文本通道）
```

**二、`requestDiagnostics` 同样放开**

```js
if (state.mode === "ble" && !state.mgmtReady) {
  return Promise.resolve();
}
```

**三、`sendControl` 支持 WS**

这是最关键的一段：

```js
function sendControl(command, opts = {}) {
  ...
  if (state.mode === "ws") {
    // The LAN bridge accepts the same command language as the BLE
    // Management Service; over WebSocket the commands are plain text frames.
    const ws = state.ws;
    if (!ws || ws.readyState !== 1) {
      throw new Error("LAN bridge is not connected");
    }
    ws.send(command);
    return Promise.resolve();
  }
  if (state.mode !== "ble" || !state.mgmtReady) {
    throw new Error("Management characteristic is not ready");
  }
  ...
}
```

`ws.readyState !== 1` 就是 `WebSocket.OPEN`。数字比常量更省事，
但可读性差；注释补上了。

**四、连接成功后自动拉一次**

```js
setConnected(true);
appendLine("[ready]");
// Query firmware/diagnostics/Wi-Fi once, exactly like the BLE path, so
// the panels are populated without pressing "Refresh diagnostics".
requestDeviceState().catch((error) =>
  appendLine(`[info] ${error.message}`),
);
```

注意 `.catch`：自动拉取失败不能中断连接。

### 为什么 `@s?` 在 LAN 下也要拉

注释写得很清楚：

```text
"@s?" is the only place the bridge reports its LAN access token,
and the web UI has no other management-command entry point.
Capturing it during the BLE session means the token field is
already filled when the user switches to LAN mode.
```

即：令牌只能通过 `@s?` 拿到。LAN 模式下顺手拉一次，用户切到
别的设备时输入框已经填好了。

### 副作用

- 连上后多几条控制帧（`@i?`/`@w?`/`@s?`）。
- 这些回复是文本帧，被控制通道消费，不会污染串口。
- 如果 daemon 版本旧、不支持 `@w?`，会返回错误——被 `.catch` 捕获，
  只在终端里打印一行 `[info] ...`。

> 兼容性处理：**新前端 + 旧后端时，功能降级但不崩。**

### 这个补丁背后的架构变化

```text
以前：LAN = 只传串口字节
现在：LAN = 串口字节 + 完整 @ 命令
```

这就是第 31 章要讲的“协议统一”的第一次落地。

> 一旦 LAN 能跑 `@` 命令，前端就没必要维护两套逻辑。
> **能力差异应该由后端声明，而不是由前端硬编码传输类型。**

---

## 27.7 五个补丁的关系

```text
0001 randomUUID     ┐
0003 sha256         ┘ 都是“HTTP 缺 secure-context API”

0002 LAN 提示         ┐
0004 wss 推导         │ 都是“HTTPS/LAN 的体验适配”
0005 LAN 诊断         ┘  把 LAN 提升为一等公民
```

按依赖排序：

```text
0001 必须最先（模块初始化就需要 UUID）
0003 独立，但也是模块级
0004 独立（连接前）
0005 依赖 WS 能发文本帧（daemon 侧已支持）
0002 纯 UI
```

实际顺序可以乱，因为它们改的文件基本不重叠（除了 `index.html`
被 0001 和 0002 都改，所以顺序固定）。

### 冲突是怎么产生的

```text
0001 改 index.html 第 5 行附近
0002 改 index.html 第 35 行附近
```

行号差得远，`patch` 不会冲突。但如果两个补丁改**同一行**，
第二个就会失败——这时候要么重排，要么合并。

> 补丁数量不是越少越好，但**同一文件同一区域的补丁要合并**。

---

## 27.8 实验：亲手应用和验证补丁

### 实验 A：看补丁内容

```sh
cd linkr-bee
for p in patches/*.patch; do
  echo "=== $p ==="
  head -5 "$p"
done
```

### 实验 B：干跑

```sh
cd linkr-bee
patch -p1 --dry-run <patches/0001-web-uuid-fallback-for-plain-http.patch
```

`--dry-run` 只检查能不能打，不改文件。**先干跑，再真打。**

如果要在一份副本上试：

```sh
cp -a web /tmp/web-clean
cd /tmp
patch -p1 -d web-clean --dry-run \
  < /path/to/linkr-bee/patches/0001-web-uuid-fallback-for-plain-http.patch
```

### 实验 C：真正应用

```sh
./apply-web.sh
grep -n 'randomUUID' web/index.html
grep -n 'managementBleOnly' web/index.html web/app.js
ls web/sha256_fallback.js
```

### 实验 D：验证 SHA-256

在浏览器控制台（明文 HTTP 页面）：

```js
const { sha256Hex } = await import("./sha256_fallback.js");
await sha256Hex(new TextEncoder().encode("abc"));
// "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
```

对比：

```sh
echo -n abc | sha256sum
```

必须一致。

### 实验 E：验证 wss 推导

在 HTTPS 页面控制台：

```js
// 把 websocketUrl 复制进去测（或从模块导出）
websocketUrl("192.168.0.121")
```

应该得到 `wss://192.168.0.121:8443/ws`（端口取当前页面）。

### 实验 F：验证 LAN 诊断

1. 用 LAN 连上。
2. 观察终端里出现 `[ready]`。
3. 打开诊断面板，应该有数据，不需要手点刷新。
4. 看 Network 面板，能看到文本帧 `@i?`、`@w?`、`@s?`。

---

## 27.9 什么时候该改上游

这五个补丁都是本地适配。但有些情况下应该提 PR：

| 情况 | 处理 |
|---|---|
| 上游也能受益的 bug 修复 | 提 PR |
| 上游的通用能力缺失（如 `sendControl` 只支持 BLE） | 提 PR |
| 只在你这块板成立的环境差异 | 本地补丁 |
| 依赖你的硬件/固件的特殊行为 | 本地补丁 |
| 临时绕过（内核 bug） | 本地补丁 + 注释写清原因 |

这五个里：

```text
0001 randomUUID 兜底      → 上游也可能接受（通用性）
0003 sha256 兜底          → 上游也可能接受
0004 wss 推导             → 上游可能已有更好的实现
0005 LAN 诊断             → 上游可能接受（协议已统一）
0002 LAN 提示             → 上游可能接受
```

> 判断标准：**“没有 V851S 的人会不会也遇到？”** 会 → 提 PR；
> 不会 → 本地补丁。

---

## 27.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 补丁打不上 | 上游行号漂了 | `--dry-run`、看 `.rej` |
| 补丁打了一半 | 脚本没 `set -e` | 检查 `apply-web.sh` |
| 改了没生效 | 缓存 `?v=` 没改 | 强刷、看 Network |
| 重复应用出错 | 没 `--forward` | 用干净副本 |
| LAN 提示不显示 | `state.mode` 不是 `"ws"` | 控制台打印 state |
| 提示一直显示 | `hidden` 逻辑写反 | 检查 `!==`/`===` |
| SHA-256 结果不对 | 填充/`>>> 0` 写错 | 已知答案测试 |
| 大文件校验慢 | 走了纯 JS | 换 HTTPS |
| 连接被浏览器拦 | 混合内容 | 看 Console |
| 诊断面板空 | 没触发 `requestDeviceState` | 看 Network 文本帧 |
| 令牌拿不到 | daemon 太旧不支持 `@s?` | 看 `[info]` 行 |
| 补丁顺序错 | 两个补丁改同一区域 | 合并或重排 |

---

## 27.11 练习

### 练习 27.1（必做）

阅读五个补丁，回答：

1. 哪几个是“secure context 缺失”导致的？
2. 哪几个是“LAN vs BLE 能力差异”导致的？
3. 哪个补丁只是 UI 提示？它为什么重要？

### 练习 27.2（必做）

在明文 HTTP 页面验证：

```js
typeof crypto.randomUUID     // ?
typeof crypto.subtle         // ?
typeof crypto.getRandomValues // ?
```

然后在 HTTPS 页面再测一次，记录差异。

### 练习 27.3（必做）

用 `sha256Hex` 计算下列输入，和 `sha256sum` 对比：

```text
""（空）
"abc"
"a" × 55
"a" × 56
"a" × 64
```

这五个覆盖了填充边界。

### 练习 27.4（必做）

回答：

1. 为什么 `websocketUrl` 要用 `location.host`？
2. 为什么 `requestDeviceState` 在 LAN 下不能再检查 `mgmtReady`？
3. 为什么自动拉取要 `.catch`？
4. 为什么补丁脚本必须 `set -e`？

### 练习 27.5（选做）

写一个第六个补丁：在 LAN 模式下，把“BLE 连接”按钮的文案改成
“BLE（需 HTTPS）”，并在点击时提示。要求：

- 只改前端。
- 有中英文。
- 改 `?v=`。
- 能用 `patch -p1` 应用。

---

## 27.12 本章验收

1. 能解释为什么用补丁而不是直接改第三方源码。
2. 能说清 `crypto.randomUUID`/`crypto.subtle` 为什么在 HTTP 下缺失。
3. 能读懂并验证纯 JS SHA-256 的实现要点。
4. 能解释混合内容限制，以及 `wss://` 推导的四种情况。
5. 能解释 LAN 为什么能跑 `@` 命令，以及这带来的架构变化。
6. 能判断一个问题该提 PR 还是本地打补丁。

### 自测题

1. `crypto.randomUUID` 在 `http://192.168.0.121/` 下存在吗？
2. SHA-256 填充时，最后 8 字节写什么？
3. 为什么 JS 里到处要 `>>> 0`？
4. HTTPS 页面连 `ws://` 会怎样？
5. 补丁 0005 让 `sendControl` 多支持了什么？
6. 补丁脚本里 `--forward` 和 `set -e` 各起什么作用？

> 下一章离开浏览器，写一个跑在终端里的 TUI 客户端。
