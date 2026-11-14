# 附录 F：参考资料

> 原则：**优先看一手资料**（规范、官方文档、源码），二手资料只用来入门。
> 链接会变；找不到时按标题搜索。

---

## F.1 这块板相关

```text
芯片：Allwinner V851S（XR829 Wi-Fi/BT 配套）
板子：YuzukiPI-Lizard
```

| 资料 | 说明 |
|---|---|
| 芯片数据手册 / 用户手册 | 由厂商提供，受 EULA 限制，**不在本仓库** |
| 原理图 | 板厂提供，确认引脚/电源/串口 |
| Tina SDK 文档 | 厂商 SDK 内，受 EULA 限制 |
| 本仓库 `docs/EXPLAIN.zh-CN.md` | 这块板的完整技术参考 |
| 本仓库 `docs/PROTOCOL.md` | 线协议速查 |
| 本仓库 `docs/BUILD.md` / `DEPLOY.md` | 构建与部署 |

> 数据手册是寄存器问题的**最终依据**。找不到就查同类 SoC 的手册，
> 但一定要用实测确认。

---

## F.2 Zephyr

| 资料 | 地址 |
|---|---|
| 官方文档 | https://docs.zephyrproject.org/ |
| 内核 API | https://docs.zephyrproject.org/latest/kernel/ |
| 设备树指南 | https://docs.zephyrproject.org/latest/build/dts/ |
| 构建系统 | https://docs.zephyrproject.org/latest/build/cmake/ |
| 线程/同步 | https://docs.zephyrproject.org/latest/kernel/services/threads/ |
| 环形缓冲 | https://docs.zephyrproject.org/latest/kernel/data_structures/ring_buffers.html |
| 日志系统 | https://docs.zephyrproject.org/latest/services/logging/ |
| 测试框架 | https://docs.zephyrproject.org/latest/develop/test/ |

### 源码里值得读的

```text
kernel/                  调度、同步、内存
lib/os/ring_buffer.c     环形缓冲的经典实现
drivers/serial/          串口驱动
subsys/logging/          日志后端
soc/                     各 SoC 的时钟/引脚
boards/                  板级配置
```

---

## F.3 Linux 内核

| 资料 | 地址 |
|---|---|
| 内核文档 | https://docs.kernel.org/ |
| 设备树绑定 | https://docs.kernel.org/devicetree/bindings/ |
| remoteproc | https://docs.kernel.org/staging/remoteproc.html |
| rpmsg | https://docs.kernel.org/staging/rpmsg.html |
| virtio | https://docs.kernel.org/driver-api/virtio/ |
| MTD | https://docs.kernel.org/driver-api/mtd/ |
| UBI | https://docs.kernel.org/driver-api/ubi/ |
| overlayfs | https://docs.kernel.org/filesystems/overlayfs.html |
| squashfs | https://docs.kernel.org/filesystems/squashfs.html |
| IIO | https://docs.kernel.org/driver-api/iio/ |
| 串口 | https://docs.kernel.org/driver-api/serial/ |

### 源码

```text
drivers/rpmsg/            rpmsg 总线与端点
drivers/remoteproc/       remoteproc 框架
drivers/virtio/           virtio 实现
drivers/mtd/ubi/          UBI
fs/squashfs/              squashfs
fs/overlayfs/             overlayfs
```

---

## F.4 规范与协议

| 资料 | 说明 |
|---|---|
| virtio spec | https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html |
| OpenAMP | https://github.com/OpenAMP/open-amp |
| libmetal | https://github.com/OpenAMP/libmetal |
| RFC 6455（WebSocket） | https://www.rfc-editor.org/rfc/rfc6455 |
| RFC 8446（TLS 1.3） | https://www.rfc-editor.org/rfc/rfc8446 |
| RFC 5246（TLS 1.2） | https://www.rfc-editor.org/rfc/rfc5246 |
| RFC 9110（HTTP 语义） | https://www.rfc-editor.org/rfc/rfc9110 |
| Bluetooth Core Spec | https://www.bluetooth.com/specifications/specs/ |
| GATT / GAP | Bluetooth SIG 的 Core Spec 卷 3 |
| NUS（Nordic UART Service） | Nordic 官方示例与文档 |
| FHS | https://refspecs.linuxfoundation.org/FHS_3.0/fhs-3.0.html |

> 协议实现有疑问时，**回规范**。博客常有简化或过时。

---

## F.5 BlueZ 与蓝牙

| 资料 | 地址 |
|---|---|
| BlueZ 官网 | https://www.bluez.org/ |
| BlueZ 源码 | https://github.com/bluez/bluez |
| D-Bus API 文档 | https://github.com/bluez/bluez/tree/master/doc |
| GATT 示例（battery） | BlueZ `src/gatt-database.c`、`plugins/` |
| btmon | BlueZ 自带 |
| Android BLE 指南 | https://developer.android.com/develop/connectivity/bluetooth/ble |
| Android 权限 | https://developer.android.com/develop/connectivity/bluetooth/bt-permissions |

### 常读的源码

```text
src/gatt-database.c       GATT 数据库与 D-Bus 对象
src/advertising.c         广播
src/device.c              设备状态
monitor/                  btmon
```

---

## F.6 Web 与前端

| 资料 | 地址 |
|---|---|
| MDN Web Docs | https://developer.mozilla.org/ |
| WebSocket API | https://developer.mozilla.org/docs/Web/API/WebSocket |
| Web Bluetooth | https://developer.mozilla.org/docs/Web/API/Web_Bluetooth_API |
| Secure contexts | https://developer.mozilla.org/docs/Web/Security/Secure_Contexts |
| xterm.js | https://xtermjs.org/ |
| Fetch / Streams | https://developer.mozilla.org/docs/Web/API/Fetch_API |
| SHA-256（FIPS 180-4） | https://csrc.nist.gov/pubs/fips/180-4/upd1/final |

> 浏览器行为以 MDN + 实测为准。**规范管“应该”，浏览器管“实际”。**

---

## F.7 书

### 入门与系统

```text
《Computer Systems: A Programmer's Perspective》(CSAPP)
    理解内存、链接、并发、I/O 的最佳起点

《The Linux Programming Interface》(TLPI)
    进程、文件、socket、信号、终端——查手册级别

《Linux Device Drivers》(LDD3)
    略旧，但驱动模型和概念仍准确

《Mastering Embedded Linux Programming》
    构建、启动、存储、调试的完整流程

《Understanding the Linux Kernel》
    深入内核，选读
```

### 网络与协议

```text
《TCP/IP Illustrated, Volume 1》
    TCP 行为、拥塞、超时的经典

《UNIX Network Programming》
    socket API 的权威参考

《HTTP: The Definitive Guide》
    偏旧，但概念仍适用
```

### 嵌入式与实时

```text
《Making Embedded Systems》
    嵌入式工程思维入门

《Real-Time Concepts for Embedded Systems》
    RTOS 概念

Zephyr 官方文档（比大多数书新）
```

### 工程与协作

```text
《The Pragmatic Programmer》
《Clean Code》（取其意，不必照搬）
《Site Reliability Engineering》(Google)
    长稳、监控、故障处理
```

---

## F.8 工具文档

| 工具 | 用途 | 地址 |
|---|---|---|
| CMake | 构建 | https://cmake.org/documentation/ |
| Ninja | 构建执行 | https://ninja-build.org/manual.html |
| Git | 版本控制 | https://git-scm.com/doc |
| OpenSSL | 证书/加密 | https://www.openssl.org/docs/ |
| Wireshark | 抓包 | https://www.wireshark.org/docs/ |
| tcpdump | 抓包 | https://www.tcpdump.org/manpages/ |
| GDB | 调试 | https://sourceware.org/gdb/documentation/ |
| strace | 系统调用 | https://strace.io/ |
| perf | 性能 | https://perf.wiki.kernel.org/ |
| minisign | 签名 | https://jedisct1.github.io/minisign/ |
| reuse | SPDX 合规 | https://reuse.software/ |

---

## F.9 标准与法规（产品化时）

```text
CE / FCC          电磁兼容与射频
RoHS              有害物质限制
WEEE              电子废弃物回收
GDPR / 个人信息保护  如果涉及用户数据
```

> 这些不是软件问题，但**决定能不能卖**。做产品前先问清楚目标市场。

---

## F.10 怎么读资料

### 三层法

```text
第一层：官方教程/Getting Started —— 跑通
第二层：API 文档/规范 —— 用对
第三层：源码/手册 —— 搞懂边界行为
```

### 遇到不一致时

```text
规范 > 官方文档 > 源码 > 博客/论坛
```

但实际行为以**你板子上的实测**为准，并记录下来。

### 做笔记

```text
每条笔记包含：
- 问题（一句话）
- 结论（可执行）
- 证据（命令/日志/链接）
- 日期与版本
```

> 半年后你会感谢现在的自己。

---

## F.11 本书引用的仓库内文件

| 文件 | 用途 |
|---|---|
| `README.md` | 项目总览 |
| `docs/BUILD.md` | 构建 |
| `docs/DEPLOY.md` | 部署 |
| `docs/PROTOCOL.md` | 协议 |
| `docs/EXPLAIN.zh-CN.md` | 技术参考 |
| `docs/TALK.zh-CN.md` | 讲解稿 |
| `NOTICE` | 第三方与许可证 |
| `MANIFEST.sha256` | 文件校验 |

---

## F.12 最后一句

```text
资料会过时，方法不会：
分层、测量、记录、回滚、写下来。
```

> 遇到查不到的问题，先把它变成**一个能复现的最小实验**。
> 这比继续搜索更有用。
