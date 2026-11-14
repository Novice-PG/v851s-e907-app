# 附录 D：常见错误 100 例

> 用法：按“现象”查，不要按“原因”查——因为你先看到的是现象。
> 每条给一个最可能的解释和第一个该做的检查。**不是全部可能性。**

---

## D.1 环境与工具链（1-10）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 1 | `command not found: arm-linux-gcc` | PATH 没配 / 没装 | `which`、`echo $PATH` |
| 2 | 编译出的程序在板子上 `Exec format error` | 架构/ABI 不匹配 | `file`、`readelf -h` |
| 3 | `cmake` 找不到编译器 | 工具链文件没指定 | `-DCMAKE_TOOLCHAIN_FILE` |
| 4 | `west` 报 “not in a workspace” | 不在 west workspace 根 | `.west/config` |
| 5 | 串口 `Permission denied` | 不在 `dialout` 组 | `id`、`ls -l /dev/ttyUSB0` |
| 6 | `ninja: command not found` | 没装 Ninja | `apt install ninja-build` |
| 7 | Python 脚本报语法错 | 主机版本比板子新 | `python3 --version` |
| 8 | `pip install` 后板子跑不了 | 装到了主机 / 架构不对 | 交叉编译或纯 Python |
| 9 | git 提交作者不对 | 没配 `user.name/email` | `git config --list` |
| 10 | `scp` 到板子失败 | BusyBox scp 需要 `-O` | 加 `-O` |

---

## D.2 串口与硬件（11-20）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 11 | 串口全是乱码 | 波特率不对 | 115200、8N1 |
| 12 | 串口完全没输出 | TX/RX 接反 / 没共地 | 交换 TX/RX、接 GND |
| 13 | 能收不能发 | 只接了 RX | 检查 TX |
| 14 | 偶尔丢字节 | FIFO 溢出 / 中断太慢 | LSR overrun 位 |
| 15 | 发出去对方收到乱码 | 时钟算错 | 重算 divisor |
| 16 | 电平不对 | TTL 直接接 RS-232 | 用电平转换 |
| 17 | 板子反复重启 | 供电不足 | 换电源、量电压 |
| 18 | USB 串口时有时无 | 线材/接触 | 换线、换口 |
| 19 | 引脚功能不对 | mux 没配 | 读 PIO 寄存器 |
| 20 | 按键抖动导致重复触发 | 没去抖 | 软件去抖/RC |

---

## D.3 启动链（21-30）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 21 | 上电完全无输出 | boot0/U-Boot 坏 / 供电 | FEL、量电压 |
| 22 | 卡在 `Hit any key` | 没接串口/没按键 | 发回车 |
| 23 | U-Boot 找不到内核 | 卷号/分区错 | `printenv`、`ubi list` |
| 24 | 内核 panic `Unable to mount root` | rootfs 卷坏/参数错 | `bootargs`、卷内容 |
| 25 | 启动后进救援 shell | 忘了写回 boot-normal | 写 ubi0_3 |
| 26 | `Bad magic number` | 镜像格式不对 | 重新生成 |
| 27 | 设备树不匹配 | DTB 和板子不对应 | 对比 compatible |
| 28 | 启动很慢 | 日志级别高/坏块重试 | `loglevel`、dmesg |
| 29 | 改了 bootargs 不生效 | 没 `saveenv` 或被覆盖 | `printenv bootargs` |
| 30 | `saveenv` 后起不来 | 存了错误参数 | 串口进 U-Boot 改回 |

---

## D.4 存储与文件系统（31-40）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 31 | 写 `/etc` 报只读 | squashfs 只读 | 改 overlay/UDISK |
| 32 | 重启后配置丢 | 写在 tmpfs/upper 被清 | 放 UDISK/persist |
| 33 | 空间不足 | 日志/镜像太大 | `df -h`、`ubinfo` |
| 34 | `ubiupdatevol` 失败 | 卷被挂载 | `umount` |
| 35 | UDISK 挂不上 | 卷损坏/未 attach | `dmesg`、`ubiattach` |
| 36 | 删了 upper 文件没恢复 | 理解错 overlay | 删的是 upper 副本 |
| 37 | NAND 报 ECC 错误 | 坏块/写入中断 | dmesg、重刷 |
| 38 | 刷完版本号没变 | 刷错卷/没重启 | `cat /etc/rootfs_version` |
| 39 | 镜像比预期大很多 | 调试符号/缓存 | `unsquashfs -ll` |
| 40 | 掉电后文件系统损坏 | 没 sync/无日志 | 用 ubifs、加 sync |

---

## D.5 设备树与驱动（41-50）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 41 | 驱动没 probe | compatible 不匹配 | `/sys/bus/*/devices` |
| 42 | 寄存器读写无效 | 地址/大小错 | DTS reg |
| 43 | 中断不触发 | 中断号/触发方式错 | `/proc/interrupts` |
| 44 | 时钟没开 | DTS clocks 缺失 | `clk_summary` |
| 45 | 引脚冲突 | 两个节点用同一 pin | pinctrl |
| 46 | Linux 内存变少 | reserved 太大 | `/proc/meminfo` |
| 47 | 覆盖了保留内存 | 缺 `no-map` | DTS reserved-memory |
| 48 | 加了 `iommus` 起不来 | 这块 SoC 没有 IOMMU | 删掉 |
| 49 | 设备树编译失败 | 语法/引用错 | `dtc` 报错行 |
| 50 | 改了 DTS 没生效 | 没重编 DTB/没换 | 对比 `/proc/device-tree` |

---

## D.6 E907、Zephyr、rpmsg（51-60）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 51 | remoteproc 一直 offline | 固件路径/格式 | `firmware`、`dmesg` |
| 52 | 加载后立刻崩 | 内存地址/DTS 不匹配 | 固件链接地址 |
| 53 | 没有 `.resource_table` | 配置/链接脚本 | `readelf -S` |
| 54 | 没有 `/dev/rpmsg*` | vring/端点没建 | `dmesg` |
| 55 | 发消息没回 | 回调没注册/名字不对 | E907 日志 |
| 56 | 大量丢消息 | 环太小/没背压 | 缓冲大小、计数 |
| 57 | 偶发数据错乱 | 内存屏障/缓存 | 共享区属性 |
| 58 | 重启 E907 后 A7 断 | 端点失效 | 重新打开 `/dev/rpmsg*` |
| 59 | `k_malloc` 返回 NULL | 堆太小 | map 文件、堆配置 |
| 60 | 固件跑飞 | 栈溢出 | 栈大小、canary |

---

## D.7 daemon、HTTP、WebSocket（61-70）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 61 | 端口没监听 | 服务没起/绑定失败 | `ss -ltnp` |
| 62 | 页面 404 | 根目录/路径错 | 服务端日志 |
| 63 | WS 握手 400 | `Sec-WebSocket-Accept` 错 | 对照 RFC 6455 |
| 64 | WS 连上就断 | 帧解析错/异常 | 抓帧、日志 |
| 65 | 输入没反应 | 前端没发/后端没转 | 两端日志 |
| 66 | 输出重复 | 多处转发/回显 | 调用链 |
| 67 | 两个终端互相插字 | 没有 owner | `@c?` |
| 68 | 命令回复出现在串口 | 没剥离 `LINKR*` | `CTL_PREFIXES` |
| 69 | daemon 内存一直涨 | 连接没清理/缓冲 | VmRSS、fd |
| 70 | 断线后不重连 | 没重连逻辑 | 客户端状态机 |

---

## D.8 HTTPS、证书、令牌（71-80）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 71 | 浏览器证书警告 | 自签名（正常） | 核对指纹 |
| 72 | `SSL_ERROR_RX_RECORD_TOO_LONG` | 用 HTTPS 连了 HTTP 端口 | 端口 |
| 73 | 证书/私钥不匹配 | 生成时配错 | `openssl x509 -modulus` |
| 74 | 证书过期 | 系统时间错/真过期 | `date`、`-dates` |
| 75 | SAN 不含 IP | 生成时漏了 | `-addext subjectAltName` |
| 76 | 80 端口没跳转 | 服务配置 | `curl -I` |
| 77 | 令牌不生效 | 第一帧顺序/格式 | 服务端日志 |
| 78 | 令牌进日志 | 没脱敏 | `grep token` |
| 79 | 私钥缺失后降级明文 | 失败不安全 | 故意测一次 |
| 80 | 所有设备同令牌 | 写死在镜像 | `unsquashfs -l` |

---

## D.9 BLE（81-90）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 81 | 手机扫不到 | 广播为空/权限 | 不过滤扫描 |
| 82 | 扫到但无服务 UUID | 广播数据没写 | `ble-adv-data.py` |
| 83 | 连不上 | 已有 central/距离 | 板子日志 |
| 84 | 连接返回 133 | GATT 没 close/太频繁 | 退避、close |
| 85 | 连上无服务 | 服务发现失败 | `onServicesDiscovered` |
| 86 | 能写收不到 | CCCD 值错 | notify=0x01/indicate=0x02 |
| 87 | 数据截断 | MTU 用错 | 打印协商值 |
| 88 | 写操作丢 | 并发 GATT | 加队列 |
| 89 | 后台断开 | 没前台服务 | Doze/电池优化 |
| 90 | 重连要重新配对 | MAC 变了/绑定没存 | 查 BT MAC |

---

## D.10 前端、构建、测试、交付（91-100）

| # | 现象 | 可能原因 | 先查这个 |
|---|---|---|---|
| 91 | 改了 JS 没生效 | 缓存 `?v=` 没改 | 强刷、Network |
| 92 | `crypto.randomUUID` 未定义 | HTTP 非安全上下文 | 加 polyfill 或 HTTPS |
| 93 | HTTPS 页面连 `ws://` 被拦 | 混合内容 | 用 `wss://` |
| 94 | 下载校验失败 | `crypto.subtle` 缺失 | 纯 JS SHA-256 |
| 95 | 构建不可复现 | 时间戳/路径 | `SOURCE_DATE_EPOCH` |
| 96 | `verify.sh` 失败 | 清单没重生成 | 重新生成清单 |
| 97 | 测试偶发失败 | 残留/时序 | 清缓冲、重试 |
| 98 | 长稳内存上涨 | 泄漏 | VmRSS 曲线 |
| 99 | 演示现场翻车 | 没备份路径 | 录屏、备用板 |
| 100 | 交付后一直有人问 | 文档缺口 | 补 FAQ/交接 |

---

## D.11 一条通用排查流程

```text
1. 现象是什么？（具体到一句话）
2. 最近改了什么？（git diff / 时间线）
3. 哪一层可能？（供电→串口→boot→kernel→rootfs→服务→网络→前端）
4. 怎么用一条命令证伪？（日志/状态/读寄存器）
5. 缩小到最小可复现步骤
6. 修，然后加一条回归用例
```

### 三条纪律

```text
不要一次改三个地方
不要在没看日志前猜原因
不要把“重启一下好了”当成修好了
```

> 100 条里真正重要的是最后这条流程。**记住流程，比记住条目有用。**
