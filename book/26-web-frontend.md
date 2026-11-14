# 第 26 章 Web 前端基础：一个终端网页需要什么

> 本章目标：
> 1. 知道一个“网页串口终端”由哪些部分组成。
> 2. 理解 DOM、事件循环、`fetch`、`WebSocket` 的配合。
> 3. 会用 `xterm.js` 的思路把字节流变成屏幕字符。
> 4. 知道 ES module、缓存、安全上下文对页面的影响。
> 5. 能排查“页面空白”“连不上”“终端乱码”“HTTPS 下功能缺失”。

---

## 26.1 一个终端网页的组成

```text
index.html
  ├─ CSS（布局、主题、移动端适配）
  ├─ xterm.js（终端渲染）
  ├─ 传输层（WebSocket / Web Bluetooth）
  ├─ 会话/控制（@ 命令）
  └─ 设置面板（Wi-Fi、令牌、上传）
```

### 最小可用版本

只需要三样：

```text
1. 一个 <div> 放终端
2. 一个 WebSocket 连 /ws
3. 收到二进制帧 → 写进终端
```

```html
<div id="term"></div>
<script type="module">
  import { Terminal } from "./vendor/xterm/xterm.js";

  const term = new Terminal();
  term.open(document.getElementById("term"));

  const ws = new WebSocket("ws://192.168.0.121/ws");
  ws.binaryType = "arraybuffer";

  ws.onmessage = (e) => {
    if (typeof e.data === "string") {
      // 控制回复
      return;
    }
    term.write(new Uint8Array(e.data));
  };

  term.onData((data) => {
    ws.send(new TextEncoder().encode(data));
  });
</script>
```

这就是核心。剩下的都是体验和安全。

---

## 26.2 事件循环：JS 为什么“不卡”

### 单线程

浏览器 JS 是单线程：

```text
调用栈
  │
  ├─ 同步代码
  │
  └─ 事件队列（点击、网络、定时器）
```

长时间同步代码会卡住页面。所以：

- 网络用回调/Promise。
- 大计算用 `Worker`（这块板页面基本不需要）。
- 不要在主线程里做大循环。

### Promise 与 async/await

```js
async function connect() {
  const ws = new WebSocket(url);
  await new Promise((resolve, reject) => {
    ws.onopen = resolve;
    ws.onerror = reject;
  });
  return ws;
}
```

`await` 不阻塞线程，只是把后续代码放进微任务队列。

### 和 daemon 的对应

| 浏览器 | daemon |
|---|---|
| `ws.onmessage` | `Client.send_binary` |
| `ws.send` | `Client.run` 里 `ws_recv_message` |
| `setTimeout` | `time.sleep`/定时器 |
| `Promise` | 线程/队列 |

> 两边都是“事件驱动 + 队列”，只是语言不同。

---

## 26.3 字节、文本、编码

### 三种数据

| 数据 | 类型 | 处理 |
|---|---|---|
| 串口字节 | `Uint8Array` | 原样写终端 |
| 控制命令 | 字符串 | `@...` |
| 文件/JSON | 字符串/Blob | 解析 |

### `TextEncoder` / `TextDecoder`

```js
const enc = new TextEncoder();       // 字符串 → UTF-8 字节
const dec = new TextDecoder();       // UTF-8 字节 → 字符串
```

**不要**用 `dec` 解码串口字节：

- 串口可能是 GBK、二进制、ANSI 转义。
- 非 UTF-8 会变成 `�`。
- 终端需要原始字节。

### xterm.js 为什么吃 `Uint8Array`

它内部按字节解析 ANSI 转义序列、UTF-8、宽字符。传字符串会
丢掉“原始字节”的语义。

> 规则：**串口数据永远是字节，直到终端渲染才决定怎么解释。**

---

## 26.4 WebSocket 在页面里的用法

### 二进制类型

```js
ws.binaryType = "arraybuffer";
```

默认是 `Blob`。`Blob` 需要异步读，增加延迟。`arraybuffer` 直接可用。

### 发送

```js
ws.send(new TextEncoder().encode("ls\n"));   // 二进制帧
ws.send("@c?");                               // 文本帧
```

浏览器根据参数类型自动选 opcode：

- `string` → 文本帧（0x1）。
- `ArrayBuffer`/`TypedArray` → 二进制帧（0x2）。

### 重连

```js
function connect() {
  const ws = new WebSocket(url);
  ws.onclose = () => {
    setTimeout(connect, 1000);
  };
}
```

要加退避，避免断网时疯狂重连：

```js
let delay = 500;
ws.onclose = () => {
  setTimeout(connect, delay);
  delay = Math.min(delay * 2, 10000);
};
```

### 心跳

浏览器 WebSocket API 不暴露 ping。可以：

- 依赖 TCP keepalive（不一定生效）。
- 应用层发文本 `@ping`，服务端回 `@pong`。

这块板主要靠“有数据就说明活着”，没有专门心跳。

---

## 26.5 ES module 与缓存

### `type="module"`

```html
<script type="module" src="./app.js"></script>
```

好处：

- `import`/`export`。
- 默认严格模式。
- 自动 deferred（DOM 解析完才执行）。

代价：

- 需要正确的 MIME（`text/javascript`）。
- 跨域有 CORS 限制。
- 老浏览器不支持。

### 缓存破坏

```html
<script type="module" src="./app.js?v=20261113-lan-hint"></script>
```

`?v=` 让浏览器认为是新 URL，绕过缓存。

> 改完 JS 一定要改版本号，否则用户看到旧代码。
> 这是最常见的“我改了怎么没生效”。

### 相对路径

```html
<script src="./vendor/xterm/xterm.js"></script>
```

用相对路径，页面放在子路径下也能工作。

---

## 26.6 安全上下文与功能降级

### 哪些 API 只在 HTTPS 可用

| API | HTTP | HTTPS |
|---|---|---|
| `navigator.bluetooth` | ❌ | ✅ |
| `crypto.subtle` | ❌（部分浏览器） | ✅ |
| `crypto.randomUUID` | ❌ | ✅ |
| Service Worker | ❌ | ✅ |
| Clipboard（部分） | ❌ | ✅ |

### 后果

页面在 `http://` 下会：

- `navigator.bluetooth` 是 `undefined`。
- `crypto.subtle.digest` 报错。
- `crypto.randomUUID` 是 `undefined`。

### 处理方式

1. **主入口用 HTTPS**（8443）。
2. 对必须在 HTTP 下工作的功能做兜底：
   - UUID：用 `getRandomValues` 自己拼。
   - SHA-256：纯 JS 实现。
3. 明确提示用户：“管理功能需要 HTTPS/BLE”。

> 这就是第 27 章五个补丁的来源。

---

## 26.7 终端渲染：ANSI 转义序列

### 终端不是“打印字符串”

目标板发来的可能是：

```text
\x1b[2J\x1b[H       清屏 + 光标归位
\x1b[31m红色\x1b[0m  颜色
\r                   回车
\n                   换行
\x1b[?25l           隐藏光标
```

`xterm.js` 解析这些序列，维护：

- 字符网格。
- 光标位置。
- 颜色/属性。
- 滚动缓冲。

### 为什么不能自己 `innerHTML`

- 转义序列不是 HTML。
- 直接插入有 XSS 风险。
- 宽字符/组合字符处理复杂。

> 用成熟终端库（xterm.js）。自己写会掉进无底洞。

### 尺寸

```js
const fit = new FitAddon.FitAddon();
term.loadAddon(fit);
fit.fit();
```

终端行列数要和容器大小匹配。窗口 resize 时要重新 `fit` 并
通知后端（如果需要）。

---

## 26.8 设置面板：不只是终端

这个页面还做：

| 功能 | 传输 |
|---|---|
| 串口终端 | WebSocket 二进制 |
| 会话控制 `@c?` | WebSocket 文本 |
| Wi-Fi 扫描/连接 | 管理命令 |
| BLE 广播名 | 管理命令 |
| 令牌 | 管理命令 |
| WebDAV 上传 | HTTP/管理命令 |
| 诊断 `@i?` | 管理命令 |

### 为什么分 LAN 和 BLE 两种模式

- LAN：WebSocket，带宽高，支持多客户端。
- BLE：Web Bluetooth，手机直连，不需要同一 Wi-Fi。

两种模式的命令语言相同，但**能力不同**：

- LAN 可以改 Wi-Fi（因为已经联网）。
- BLE 才能首次配网（还没有 Wi-Fi）。

所以页面要根据模式显示/隐藏功能。

---

## 26.9 实验：最小终端页面

### 目标

写一个页面，能连 `/ws`，把输入发出去，把收到的字节显示出来。

### 代码

```html
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Mini Terminal</title>
  <style>
    body { margin: 0; background: #111; }
    #term { width: 100vw; height: 100vh; }
  </style>
  <link rel="stylesheet" href="/vendor/xterm/xterm.css">
</head>
<body>
  <div id="term"></div>
  <script src="/vendor/xterm/xterm.js"></script>
  <script src="/vendor/addon-fit/addon-fit.js"></script>
  <script>
    const term = new Terminal({ convertEol: false });
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);

    const ws = new WebSocket(`ws://${location.host}/ws`);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
      fit.fit();
      term.focus();
    };

    ws.onmessage = (e) => {
      if (typeof e.data === "string") {
        console.log("ctl:", e.data);
        return;
      }
      term.write(new Uint8Array(e.data));
    };

    term.onData((d) => {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(new TextEncoder().encode(d));
      }
    });

    window.addEventListener("resize", () => fit.fit());
    term.open(document.getElementById("term"));
  </script>
</body>
</html>
```

放到 `web/` 目录，访问测试。

### 观察

1. 打开页面，终端出现。
2. 输入字符，目标板收到。
3. 目标板输出，页面显示。
4. 用 `@c?` 文本命令测试控制通道。

---

## 26.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 页面空白 | JS 语法/MIME 错 | 浏览器控制台 |
| 改代码没生效 | 缓存 | 改 `?v=`、强制刷新 |
| 连不上 `/ws` | 路径/端口/令牌 | Network 面板 |
| 终端乱码 | 用文本解码字节 | 改 `Uint8Array` |
| 输入无反应 | 没 `term.focus`/没发 | 控制台、WS 状态 |
| HTTPS 下蓝牙不可用 | 用的 HTTP | 换 8443 |
| `crypto.subtle` 报错 | 非安全上下文 | 兜底实现 |
| 中文显示半宽 | 字体/字符集 | `xterm` 字体设置 |
| resize 后错位 | 没重新 fit | resize 监听 |
| 多标签互相抢 | 会话所有权 | `@c` 命令 |

---

## 26.11 练习

### 练习 26.1（必做）

完成 26.9 的最小终端页面，并验证：

- 能显示目标板输出。
- 能发送输入。
- 能收 `@ws auth=...` 文本帧。

### 练习 26.2（必做）

给页面加：

- 断线自动重连（指数退避）。
- 一个“发送 Ctrl-C”按钮。

### 练习 26.3（必做）

回答：

1. 为什么串口数据用 `Uint8Array` 而不是字符串？
2. `binaryType="arraybuffer"` 和默认 `Blob` 有什么区别？
3. 为什么改 JS 要改 `?v=`？
4. 哪些 API 只在安全上下文可用？
5. `xterm.js` 帮你做了什么？

### 练习 26.4（选做）

用浏览器 Network 面板观察：

- WebSocket 握手请求头。
- 101 响应。
- 二进制帧大小。

---

## 26.12 本章验收

1. 能画出终端网页的组成。
2. 理解事件循环和异步连接。
3. 知道字节流和文本流的区别。
4. 会写最小 WebSocket 终端页面。
5. 知道 ES module、缓存、安全上下文的影响。
6. 能排查空白页、连不上、乱码、缓存四类问题。

### 自测题

1. WebSocket 二进制帧对应什么 JS 类型？
2. `TextDecoder` 能安全解码任意串口字节吗？
3. `type="module"` 有什么特点？
4. `crypto.randomUUID` 在 HTTP 页面存在吗？
5. `xterm.js` 为什么需要原始字节？

> 下一章看 linkr-bee 的 5 个补丁：为 V851S 做的前端适配。
