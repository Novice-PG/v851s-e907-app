# 附录 A：术语表

> 用法：读到不认识的词就回来查。每条给“一句话解释 + 它在本书哪一章出现”。
> 不追求教科书式严谨，追求“能继续往下读”。

---

## A.1 硬件与电路

| 术语 | 解释 | 章 |
|---|---|---|
| **TTL** | 用 0V/3.3V 表示 0/1 的逻辑电平 | 5 |
| **RS-232** | 用正负电压、范围更大的串口标准 | 5 |
| **UART** | 通用异步收发器，串口的硬件实现 | 5 |
| **波特率** | 每秒符号数；常见 115200 | 5 |
| **8N1** | 8 数据位、无校验、1 停止位 | 5 |
| **FIFO** | 先进先出缓冲，UART 里通常 16/32/64 字节 | 5 |
| **流控** | RTS/CTS 或 XON/XOFF，防止对方发太快 | 5 |
| **GND** | 地，所有信号的电平基准 | 5 |
| **上拉/下拉** | 用电阻把引脚默认拉到高/低 | 5 |
| **开漏** | 只能拉低、靠上拉拉高的输出结构 | 5 |
| **去抖** | 消除按键机械抖动的处理 | 3 |
| **原理图** | 元件与连接的图 | 6 |
| **引脚复用（mux）** | 一个引脚可选多种功能 | 6 |
| **SoC** | 片上系统，CPU + 外设 + 内存控制器 | 6 |
| **CCU** | 全志的时钟控制单元 | 6 |
| **PIO** | 全志的并行 IO 控制器 | 6 |
| **DRAM** | 动态内存，掉电丢失 | 6 |
| **SRAM** | 静态内存，快、小、贵 | 6 |
| **NAND** | 闪存的一种，有坏块、需磨损均衡 | 8 |
| **坏块** | 出厂或使用中损坏的 NAND 块 | 8 |
| **磨损均衡** | 把写分散到不同块，延长寿命 | 8 |
| **BROM** | 芯片出厂固化的启动 ROM | 7 |
| **FEL** | 全志 BROM 的 USB 恢复模式 | 7, 33 |

---

## A.2 启动与存储

| 术语 | 解释 | 章 |
|---|---|---|
| **boot0 / SPL** | 第二阶段引导，初始化 DRAM | 7 |
| **U-Boot** | 第三阶段引导，加载内核 | 7 |
| **bootargs** | 传给内核的启动参数 | 7 |
| **设备树（DTB）** | 描述硬件的二进制数据 | 9 |
| **DTS/DTB** | 设备树的源文件/编译后文件 | 9 |
| **MTD** | 内存技术设备，Linux 对裸闪存的抽象 | 8 |
| **UBI** | 建立在 MTD 上的逻辑卷层 | 8 |
| **UBIFS** | 建立在 UBI 上的可写文件系统 | 8 |
| **squashfs** | 只读压缩文件系统 | 8 |
| **overlayfs** | 把只读下层和可写上层合并 | 8 |
| **lower/upper** | overlay 的只读层/可写层 | 8 |
| **tmpfs** | 内存文件系统，掉电丢 | 8 |
| **initramfs** | 启动时加载到内存的小根文件系统 | 7, 33 |
| **救援镜像** | 带工具的最小系统，用来修 rootfs | 33 |
| **整片镜像** | 含完整启动链的 NAND 镜像 | 32 |
| **分卷更新** | 只写某个 UBI 卷 | 32 |

---

## A.3 异构与实时

| 术语 | 解释 | 章 |
|---|---|---|
| **AMP** | 非对称多处理，各核跑不同系统 | 11 |
| **SMP** | 对称多处理，各核跑同一个系统 | 11 |
| **异构** | 不同架构/能力的核共存 | 11 |
| **mailbox** | 核间发中断的硬件 | 11 |
| **共享内存** | 两核都能访问的内存区域 | 11 |
| **内存屏障** | 保证读写顺序的指令/语义 | 11 |
| **cache 一致性** | 多核看到同一份数据 | 11 |
| **IOMMU** | 把设备地址翻译成物理地址 | 11 |
| **DA/PA** | 设备地址/物理地址 | 11 |
| **remoteproc** | Linux 加载/启停远程核的框架 | 12, 17 |
| **rpmsg** | 基于 virtio 的核间消息总线 | 13 |
| **virtio** | 半虚拟化 I/O 标准 | 12 |
| **vring** | virtio 的环形缓冲 | 12 |
| **resource table** | 固件告诉 Linux“我需要什么”的结构 | 12 |
| **endpoint** | rpmsg 的通信端点（类似端口） | 13 |
| **name service** | rpmsg 端点名字广播机制 | 13 |
| **Zephyr** | 面向嵌入式的实时操作系统 | 14 |
| **RTOS** | 实时操作系统 | 14 |
| **线程/信号量/互斥量** | Zephyr 的并发原语 | 14 |
| **ring buffer** | 单生产者单消费者常用的环形队列 | 14 |
| **XIP** | 直接在闪存里执行代码 | 15 |
| **RAM console** | 把日志写进内存，供另一侧读取 | 17 |

---

## A.4 Linux 与网络

| 术语 | 解释 | 章 |
|---|---|---|
| **进程/线程** | 资源单位/调度单位 | 4 |
| **文件描述符（fd）** | 进程打开文件的编号 | 4 |
| **socket** | 网络通信端点 | 4 |
| **select/poll** | 等待多个 fd 就绪 | 19 |
| **阻塞/非阻塞** | 调用是否等待 | 19 |
| **背压** | 消费慢时让生产方减速 | 19 |
| **守护进程（daemon）** | 后台长期运行的服务 | 19 |
| **信号** | 异步通知机制 | 19 |
| **systemd** | 大型 init 系统（这块板不用） | 18 |
| **init 脚本（S??）** | BusyBox 风格的启动脚本 | 18 |
| **HTTP** | 请求-响应文本协议 | 20 |
| **WebSocket** | 全双工长连接，握手后按帧传 | 20 |
| **帧（frame）** | 协议的数据单元 | 20 |
| **掩码（mask）** | 客户端到服务端帧必须异或的密钥 | 20 |
| **TLS** | 加密传输层 | 21 |
| **证书/私钥** | 公钥身份/对应私钥 | 21 |
| **自签名** | 自己签自己，不在公共 CA 链上 | 21 |
| **SAN** | 证书里的可用主机名列表 | 21 |
| **证书固定（pinning）** | 客户端只信任指定证书 | 36 |
| **令牌（token）** | 共享密钥形式的访问凭证 | 21 |
| **中间人（MITM）** | 夹在两端之间转发/篡改 | 36 |
| **Just Works** | 无中间人保护的 BLE 配对 | 36 |
| **bond** | BLE 配对后保存的密钥 | 22 |
| **GATT** | BLE 的属性/服务模型 | 22 |
| **服务/特性/描述符** | GATT 的三层结构 | 22 |
| **CCCD** | 控制 notify/indicate 的描述符 | 22 |
| **notify/indicate** | 无确认/有确认的服务端推送 | 22 |
| **MTU** | 单次 ATT 载荷上限 | 24 |
| **NUS** | Nordic UART Service，常见透传服务 | 22 |
| **BlueZ** | Linux 的蓝牙协议栈 | 22 |
| **D-Bus** | Linux 进程间消息总线 | 23 |
| **hci0** | 第一个蓝牙控制器 | 22 |
| **central/peripheral** | BLE 主/从角色 | 22 |
| **Wi-Fi station/AP** | 客户端/热点模式 | 25 |
| **wpa_supplicant** | 管理 Wi-Fi 连接的守护进程 | 25 |
| **wctl** | 全志的 Wi-Fi 控制工具 | 25 |
| **DHCP** | 自动获取 IP | 25 |
| **regulatory domain** | 各国频谱规则 | 25 |

---

## A.5 前端与客户端

| 术语 | 解释 | 章 |
|---|---|---|
| **DOM** | 浏览器里的文档对象模型 | 26 |
| **ES module** | `import/export` 的 JS 模块 | 26 |
| **secure context** | HTTPS/localhost 才有的浏览器能力 | 27 |
| **polyfill** | 给旧环境补缺失 API | 27 |
| **混合内容** | HTTPS 页面加载 HTTP 资源，被浏览器拦 | 27 |
| **SHA-256** | 密码学哈希 | 27 |
| **ANSI 转义序列** | 终端控制字符 | 28 |
| **raw mode** | 关闭行缓冲/回显的终端模式 | 28 |
| **termios** | 终端属性结构 | 28 |
| **备用屏幕** | 全屏程序独占的终端缓冲区 | 28 |
| **ncurses** | 终端界面库 | 28 |
| **TUI** | 文本用户界面 | 28 |
| **CLI** | 命令行界面 | 29 |
| **退出码** | 程序返回给 shell 的状态 | 29 |
| **stdout/stderr** | 数据流/诊断流 | 29 |
| **SIGPIPE** | 写入已关闭管道时的信号 | 29 |
| **adb/logcat** | Android 调试桥/日志 | 30 |
| **前台服务** | Android 后台常驻的正式方式 | 30 |
| **GATT 队列** | 保证 BLE 操作串行的队列 | 30 |

---

## A.6 工程化

| 术语 | 解释 | 章 |
|---|---|---|
| **交叉编译** | 在 A 架构上编译 B 架构的程序 | 10 |
| **工具链** | 编译器 + 链接器 + 库 | 10 |
| **CMake** | 构建系统生成器 | 10 |
| **Kconfig** | 内核/固件的配置系统 | 10 |
| **out-of-tree** | 源码不在主仓库内的构建方式 | 10 |
| **squashfs 压缩** | gzip/xz/zstd/lz4 | 32 |
| **校验和** | 检测内容是否改变的摘要 | 32 |
| **可复现构建** | 同样输入得到等价产物 | 32 |
| **HIL** | 硬件在环测试 | 34 |
| **长稳（soak）** | 长时间运行测试 | 34 |
| **回归矩阵** | 改什么测什么的表 | 34 |
| **VmRSS** | 进程常驻物理内存 | 35 |
| **page cache** | 内核缓存的文件页 | 35 |
| **slab** | 内核对象缓存 | 35 |
| **OOM killer** | 内存不足时杀进程的机制 | 35 |
| **backpressure** | 背压 | 19 |
| **威胁模型** | 资产/对手/入口/影响 | 36 |
| **攻击面** | 所有可被攻击的入口总和 | 36 |
| **加固** | 减少攻击面的配置与代码改动 | 36 |
| **失败安全** | 出错时更保守而不是更开放 | 36 |
| **许可证兼容** | 不同许可证能否一起分发 | 37 |
| **SPDX** | 机器可读的许可证标识 | 37 |
| **DCO** | 开发者原创声明（`Signed-off-by`） | 37 |
| **ADR** | 架构决策记录 | 38 |
| **垂直切片** | 打通一条端到端最小路径 | 38 |
| **验收矩阵** | 需求 → 步骤 → 期望 → 结果 | 40 |
| **支持边界** | 明确管什么、不管什么 | 40 |

---

## A.7 常用缩写

```text
AMP     Asymmetric Multi-Processing
API     Application Programming Interface
BLE     Bluetooth Low Energy
BSP     Board Support Package
CCCD    Client Characteristic Configuration Descriptor
CLI     Command Line Interface
CMA     Contiguous Memory Allocator
DCO     Developer Certificate of Origin
DTB     Device Tree Blob
DTS     Device Tree Source
FEL     (Allwinner) USB recovery mode
FIFO    First In First Out
GATT    Generic Attribute Profile
HIL     Hardware In The Loop
HTTP    HyperText Transfer Protocol
I2C     Inter-Integrated Circuit
IOMMU   I/O Memory Management Unit
IPI     Inter-Processor Interrupt
IRQ     Interrupt Request
MAC     Media Access Control
MITM    Man In The Middle
MMU     Memory Management Unit
MTD     Memory Technology Device
MTU     Maximum Transmission Unit
NUS     Nordic UART Service
OOM     Out Of Memory
OTA     Over The Air
PIO     Programmable I/O
RSS     Resident Set Size
RTOS    Real-Time Operating System
SPI     Serial Peripheral Interface
SPDX    Software Package Data Exchange
TLS     Transport Layer Security
TUI     Text User Interface
UBI     Unsorted Block Images
UART    Universal Asynchronous Receiver/Transmitter
WS      WebSocket
WSS     WebSocket over TLS
XIP     eXecute In Place
```
