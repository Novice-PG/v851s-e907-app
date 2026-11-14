# 附录 E：代码阅读路线

> 目标：给你一条**从入口到细节**的路线，而不是“从头读到尾”。
> 每步给：读哪个文件、先看什么、回答什么问题。

---

## E.1 阅读原则

```text
1. 先看“数据怎么流动”，再看“逻辑怎么分支”
2. 先看入口和接口，再看实现
3. 一次只追一条链路
4. 看不懂就加日志/打点，不要硬猜
5. 每读一段，用自己的话写一句“它负责什么”
```

### 不要这样做

```text
- 从第一个文件第一行开始顺序读
- 一开始就钻最复杂的函数
- 同时追三条链路
- 只看代码不看文档/提交历史
```

> 读代码的目标不是“读完”，是**能回答“改这里会影响什么”**。

---

## E.2 全局：先建立地图

### 仓库分工

```text
src/                    E907 固件（Zephyr）
linkr-bee/a7/           A7 侧 daemon、BLE 服务器、启动脚本
linkr-bee/web/          网页前端
linkr-bee/patches/      前端补丁
tools/                  主机侧测试工具
docs/                   构建、部署、协议、说明
```

### 三条链路

```text
链路 A：浏览器 ↔ daemon ↔ rpmsg ↔ E907 ↔ UART3
链路 B：手机 ↔ BLE ↔ linkr-ble ↔ Unix socket ↔ daemon
链路 C：主机 CLI/TUI ↔ WebSocket ↔ daemon
```

先选一条。推荐从 A 开始，因为它是主链路。

---

## E.3 链路 A：浏览器到串口

### 第一步：网页入口

```text
linkr-bee/web/index.html
```

看什么：

```text
1. 引入了哪些 JS（app.js、xterm.js）
2. 终端的 DOM 容器 id
3. WebSocket URL 从哪来
```

回答：

```text
- 页面怎么知道板子地址？
- 连接按钮触发哪个函数？
```

### 第二步：前端连接逻辑

```text
linkr-bee/web/app.js
```

搜索：

```sh
grep -n 'WebSocket\|onmessage\|send(' linkr-bee/web/app.js | head -40
grep -n 'websocketUrl\|connect(' linkr-bee/web/app.js | head -20
```

回答：

```text
- 文本帧和二进制帧怎么区分？
- 收到二进制后交给谁渲染？
- 断线后怎么重连？
```

### 第三步：daemon 入口

```text
linkr-bee/a7/linkr-bee-daemon
```

先看文件头注释和 `main()`：

```sh
grep -n '^def main\|^class \|^def handle_' linkr-bee/a7/linkr-bee-daemon
```

建议顺序：

```text
1. main()               启动哪些线程、监听哪些端口
2. HTTPServer/WS 处理    握手、帧解析
3. Session              客户端表、owner
4. handle_text/handle_binary
5. rpmsg 读取线程
6. SerialWriter         发送队列/背压
```

回答：

```text
- 一个 WebSocket 连接对应哪个对象？
- 二进制帧进来后调用链是什么？
- 控制回复怎么和串口数据分开？
```

### 第四步：rpmsg 读写

```sh
grep -n 'rpmsg\|/dev/rpmsg' linkr-bee/a7/linkr-bee-daemon | head -40
```

回答：

```text
- 端点名是什么？
- 读线程怎么处理阻塞和重连？
- 写失败怎么办？
```

### 第五步：E907 固件

```text
src/main.c              初始化顺序
src/openamp_slave.c     rpmsg 端点、回调
src/linkr_bridge.c      UART3 与 rpmsg 的桥接
src/board_early.c       早期初始化（时钟/引脚）
```

顺序：

```text
1. main.c              看线程/队列创建
2. openamp_slave.c     看端点注册和回调
3. linkr_bridge.c      看环形缓冲和 UART 中断
4. board_early.c       看时钟/引脚（最早执行）
```

回答：

```text
- 一条 rpmsg 消息怎么变成 UART 字节？
- UART 收到的字节怎么变成 rpmsg 消息？
- 缓冲满了怎么办？
```

### 第六步：串口硬件

```sh
grep -n '0x02500\|UART3\|uart_' src/linkr_bridge.c src/board_early.c | head -40
```

对照附录 C 的寄存器表。

回答：

```text
- 波特率怎么算的？
- 中断里做了什么、不做什么？
```

---

## E.4 链路 B：手机到串口

### 第一步：BLE 服务器入口

```text
linkr-bee/a7/linkr_ble.c
```

搜索：

```sh
grep -n 'GATT\|register_app\|service\|characteristic' linkr-bee/a7/linkr_ble.c | head -40
```

回答：

```text
- 注册了哪几个服务？
- 每个特性的 UUID 和权限？
- 回调怎么分发？
```

### 第二步：D-Bus 与 BlueZ

```sh
grep -n 'g_dbus\|GattManager\|Advertisement\|RegisterApplication' \
  linkr-bee/a7/linkr_ble.c | head -40
```

回答：

```text
- 应用怎么注册到 bluetoothd？
- 广播数据从哪来？
```

### 第三步：Unix socket 到 daemon

```sh
grep -n 'socket\|connect\|/var/run/linkr' linkr-bee/a7/linkr_ble.c | head -40
```

回答：

```text
- 多个 BLE 主机怎么复用一个 socket？
- 帧格式是什么（12 字节头？）？
```

### 第四步：管理帧

```sh
grep -n "'L'\|'K'\|mgmt\|reqid\|flags" linkr-bee/a7/linkr-bee-daemon | head -40
```

对照附录/第 31 章的帧格式。

---

## E.5 链路 C：CLI/TUI

如果你已经写了 CLI/TUI：

```text
1. 参数解析
2. 连接（TLS/令牌）
3. 发送文本/二进制
4. 接收并分发
```

对照第 29 章的四个契约检查。

---

## E.6 按问题读代码

### “为什么我的输入没到目标板？”

```text
前端 send → WS 帧 → daemon handle_binary → owner 检查
→ rpmsg write → E907 回调 → UART TX
```

逐跳加日志，找出第一处“没有发生”的地方。

```sh
grep -n 'handle_binary\|def write\|send_binary' linkr-bee/a7/linkr-bee-daemon
```

### “为什么目标板的输出没到网页？”

```text
UART RX → E907 环形缓冲 → rpmsg 消息 → daemon 读线程
→ 广播给客户端 → WS 二进制帧 → xterm 渲染
```

### “为什么两个浏览器互相插字？”

```sh
grep -n 'owner\|claim\|epoch' linkr-bee/a7/linkr-bee-daemon | head -40
```

### “为什么 BLE 收不到？”

```text
GATT 写 → linkr_ble 回调 → Unix socket → daemon → rpmsg → UART
```

从 `linkr_ble.c` 的写回调开始，逐跳确认。

### “为什么内存一直涨？”

```sh
grep -n 'Thread\|append\|\[\]\|dict\|cache' linkr-bee/a7/linkr-bee-daemon | head
```

重点看：连接关闭时有没有清理、缓冲有没有上限。

---

## E.7 读提交历史

```sh
# 某文件的演变
git log --oneline --follow -- src/linkr_bridge.c

# 某次提交改了什么、为什么
git show <commit>

# 找引入某行的提交
git blame -L 100,140 src/linkr_bridge.c

# 搜索提交信息
git log --grep='rpmsg' --oneline
```

> 提交信息里的“为什么”，往往比代码本身更有价值。

---

## E.8 读文档的顺序

```text
1. README.md             这是什么
2. docs/BUILD.md         怎么构建
3. docs/DEPLOY.md        怎么部署
4. docs/PROTOCOL.md      协议
5. docs/EXPLAIN.zh-CN.md 设计与坑
6. 本书对应章节          原理
```

读代码前先读 1-3，能省很多时间。

---

## E.9 一个 3 小时的阅读计划

```text
0:00-0:20  README + 目录结构 + 三条链路图
0:20-0:50  网页入口 + app.js 连接逻辑
0:50-1:30  daemon：main + Session + handle_binary
1:30-2:10  daemon：rpmsg 读写 + SerialWriter
2:10-2:40  E907：main + openamp + bridge
2:40-3:00  画一张自己的调用链图，标出不懂的点
```

### 产出

```text
一张图：一条字节从浏览器到 UART 经过哪些函数
三个问题：最不懂的三处，下次带着问题读
```

> 读一遍不可能全懂。**能画出链路，就已经超过大多数人。**

---

## E.10 代码地图（速查）

| 想找 | 去哪 |
|---|---|
| 网页入口 | `linkr-bee/web/index.html` |
| 前端连接/渲染 | `linkr-bee/web/app.js` |
| 前端补丁 | `linkr-bee/patches/*.patch` |
| daemon 主逻辑 | `linkr-bee/a7/linkr-bee-daemon` |
| BLE GATT | `linkr-bee/a7/linkr_ble.c` |
| 启动脚本 | `linkr-bee/a7/S53linkr`、`S54bluetooth` |
| 广播绕过 | `linkr-bee/a7/ble-adv-data.py` |
| E907 入口 | `src/main.c` |
| rpmsg 端点 | `src/openamp_slave.c` |
| UART 桥接 | `src/linkr_bridge.c` |
| 早期初始化 | `src/board_early.c` |
| 主机测试工具 | `tools/` |
| 构建说明 | `docs/BUILD.md` |
| 协议 | `docs/PROTOCOL.md` |

---

## E.11 练习

1. 画出链路 A 的完整调用链，标出每一跳的“数据形态”（字节/帧/消息）。
2. 找出 `handle_binary` 里所有可能“丢弃数据”的分支，说明每个的理由。
3. 找出 E907 侧所有缓冲区，写出各自大小和满了之后的策略。
4. 用 `git log --grep` 找一条和 BLE 有关的提交，读它的“为什么”。
5. 给 `linkr-bridge` 加一行日志，验证一条消息的完整路径。

> 读完不算会。**能改一处、验证一处，才算。**
