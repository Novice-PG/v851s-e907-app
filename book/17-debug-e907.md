# 第 17 章 调试 E907：日志、状态、崩溃与丢包

> 本章目标：
> 1. 会用 `trace0`、remoteproc 状态、`dmesg` 定位问题。
> 2. 知道固件崩溃/跑飞/卡住分别怎么查。
> 3. 能判断丢包发生在哪一层：UART、环、rpmsg、还是通知。
> 4. 会读 `ipm_overflow`、`LINKRSTAT`、drop 计数。
> 5. 建立“从现象到层”的排查顺序，而不是乱试。

---

## 17.1 调试的第一原则：先分层

E907 系统有很多层：

```text
网页/浏览器
  │
A7 daemon（WebSocket/HTTP）
  │
Linux rpmsg_char（/dev/rpmsgN）
  │
Linux remoteproc / virtio
  │
共享内存 + mailbox
  │
E907 OpenAMP / rpmsg
  │
E907 linkr_bridge（环 + 线程）
  │
E907 UART3 驱动
  │
目标板串口
```

“没数据”可能发生在任意一层。**不要一上来就改代码。**

排查顺序：**从两端往中间，或从中间往两端，但每次只验证一层。**

---

## 17.2 看 remoteproc 状态

### 基本命令

```sh
cat /sys/class/remoteproc/remoteproc0/state
```

可能的值：

| 值 | 含义 |
|---|---|
| `offline` | 没加载 |
| `running` | 正在跑 |
| `suspended` | 挂起 |
| `crashed` | 崩了 |
| `invalid` | 状态异常 |

### 启动/停止

```sh
echo stop > /sys/class/remoteproc/remoteproc0/state
echo start > /sys/class/remoteproc/remoteproc0/state
```

### 看固件名

```sh
cat /sys/class/remoteproc/remoteproc0/firmware
```

应该是 `zephyr.elf` 或 `e907_firmware.elf`，取决于
设备树 `firmware-name` 和启动脚本。**两个名字都放一份最省事。**

### 看内核日志

```sh
dmesg | grep -i -E 'remoteproc|rpmsg|mbox|msgbox'
```

正常会看到：

```text
remoteproc remoteproc0: powering up ...
remoteproc remoteproc0: Booting fw image zephyr.elf, size ...
remoteproc remoteproc0: remote processor ... is now up
```

异常关键字：

```text
timeout
failed to load
invalid resource table
no resource table
```

> `state=running` 只说明“核在跑”，不说明“固件逻辑正常”。
> 跑飞/死循环也可能是 `running`。

---

## 17.3 `trace0`：E907 的“黑匣子”

### 读日志

```sh
cat /sys/kernel/debug/remoteproc/remoteproc0/trace0
tail -f /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

`trace0` 映射的是 E907 的 RAM console。`printk`/`LOG_*`
都进这里。

### 正常启动应该看到

```text
*** YuzukiPI-Lizard V851S E907 co-processor ***
Zephyr 3.x.x | RV32IMAC + xthead @ 600 MHz
CLINT 0x30000000 | CLIC 0x30800000 | UART3 0x02500c00
firmware DRAM 0x43c00000 (4 MiB)
rpmsg buffers 0x43000000, vrings 0x43040000 / 0x43060000
resource table: 1 vdev + 2 vrings + RAM console trace
mode: Linkr Bee UART3 bridge over rpmsg
features: 64 KiB UART rings, LINKRCFG/LINKRSTAT, rpmsg chrdev
Linkr Bee bridge ready on uart@2500c00
OpenAMP rpmsg_chrdev ready (addr 0x...)
```

### `trace0` 是环形缓冲

- 有固定大小。
- 写满后覆盖最旧的。
- 开机日志可能被后来的日志冲掉。

**重要信息出现时先复制出来：**

```sh
cp /sys/kernel/debug/remoteproc/remoteproc0/trace0 /tmp/trace.txt
```

### 没有 `trace0`

说明内核没把 RAM console 暴露出来，或者固件没配：

```conf
CONFIG_RAM_CONSOLE=y
```

检查：

```sh
ls /sys/kernel/debug/remoteproc/remoteproc0/
```

---

## 17.4 固件崩了：panic 与跑飞

### 现象

| 现象 | 可能 |
|---|---|
| `state=crashed` | 触发了异常，remoteproc 捕获 |
| `state=running` 但没日志 | 跑飞/死循环/入口错 |
| 日志停在某一行 | 那一行之后崩了 |
| 反复重启 | watchdog 或 remoteproc 重载 |

### 读 panic

Zephyr 在异常时会打印类似：

```text
FATAL ERROR: ...
Current thread: ...
...
```

在 `trace0` 里找 `FATAL`、`Oops`、`Exception`、`EUNHANDLED`。

### 常见崩溃原因

| 原因 | 表现 | 怎么查 |
|---|---|---|
| 栈溢出 | 随机崩溃、数据被改 | 量栈用量、开 `INIT_STACKS` |
| 空指针 | 固定位置崩 | 看 PC/回溯 |
| 越界写 | 随机、难复现 | 查数组、环大小 |
| vring 未初始化就用 | 启动即崩 | 初始化顺序 |
| 中断里睡眠 | 断言失败 | 查 ISR |
| 时钟没开读寄存器 | 启动早期崩 | `board_early.c` |
| 入口地址错 | 加载后立刻崩 | remoteproc 日志 |

### 拿回溯

```sh
# 主机上
riscv32-zephyr-elf-addr2line -e build/zephyr/zephyr_e907.elf <PC>
```

或者用 `gdb`：

```sh
riscv32-zephyr-elf-gdb build/zephyr/zephyr_e907.elf
```

> 要能定位，必须保留**和烧录版本完全一致**的 ELF（带符号）。
> 不要 `strip` 后才发现要调试。

### 跑飞怎么判断

- `state=running`。
- `trace0` 没有任何新输出。
- 心跳也不打印。
- UART/rpmsg 全无反应。

可能：

- 固件入口地址写错（remoteproc 配置）。
- 固件链接到了错误的内存。
- 栈指针/全局指针初始化错。
- 死循环且关了中断。

先确认 `readelf -h` 的入口地址和链接地址是否在
`0x43c00000` 窗口内。

---

## 17.5 端点没出现：rpmsg 层

### 检查清单

```text
1. E907 起来了吗？          state=running + trace0 有横幅
2. OpenAMP 初始化成功了吗？  trace0 有 "rpmsg_chrdev ready"
3. NS bind 发生了吗？        trace0 有 "NS bind"
4. /dev/rpmsg_ctrl0 有吗？   ls /dev/rpmsg*
5. /dev/rpmsgN 有吗？        创建端点后出现
```

### 典型日志

E907 侧：

```text
OpenAMP rpmsg_chrdev ready (addr 0x...)
rpmsg NS bind: 'rpmsg_chrdev' dest=...
```

A7 侧：

```sh
dmesg | grep rpmsg
```

```text
rpmsg virtio0: assigned address 0x...
```

### 没有 `/dev/rpmsg0`

| 原因 | 查法 |
|---|---|
| 没打开 `/dev/rpmsg_ctrl0` 创建端点 | daemon 是否运行 |
| 名字不匹配 | E907 宣告名 vs 请求名 |
| NS 公告没发出去 | vring/mailbox 问题 |
| 端点池满 | `no free endpoint slot` 日志 |
| 权限 | `ls -l /dev/rpmsg*` |

### 重新握手

```sh
# 停 daemon
/etc/init.d/S99linkr-bee stop

# 重启 remoteproc
echo stop > /sys/class/remoteproc/remoteproc0/state
echo start > /sys/class/remoteproc/remoteproc0/state

# 起 daemon
/etc/init.d/S99linkr-bee start
```

---

## 17.6 丢包定位：四个位置

数据可能丢在：

```text
1. UART3 RX FIFO 溢出（ISR 来太慢/FIFO 太小）
2. uart_rx_ring 满（A7 发得太快/没消费）
3. rpmsg 发送失败（buffer 不够/端点错）
4. A7 侧环/WS 丢（不在本章范围）
```

反方向：

```text
1. rpmsg 消息太大（EMSGSIZE）
2. uart_tx_ring 满（背压超时）
3. UART3 TX 太慢（波特率低）
```

### 用 `LINKRSTAT` 看计数

```text
LINKRSTAT rx=0/65536 tx=0/65536 drop=0/0
```

字段：

| 字段 | 含义 |
|---|---|
| `rx=<used>/<size>` | RX 环当前占用 / 总大小 |
| `tx=<used>/<size>` | TX 环当前占用 / 总大小 |
| `drop=<rx>/<tx>` | RX 丢字节数 / TX 丢字节数 |

判断：

- `drop_rx > 0`：E907 没来得及把 UART 数据搬进环，或环满。
- `drop_tx > 0`：A7 发太快，TX 环满且 100 ms 内没腾出空间。
- `rx` 长期接近 `size`：A7 侧消费太慢。
- `tx` 长期接近 `size`：UART 波特率太低或对端不读。

### UART overrun

如果 UART FIFO 溢出，ns16550 会置 overrun 位。表现：

- 数据莫名少一段。
- 但 `drop_rx` 可能不增加（因为字节根本没进 ring）。

查法：

- 降低波特率测试。
- 看 ISR 是否真的在跑（加计数）。
- 确认 CLIC 中断路由。

> `drop_rx` 统计的是“进不了环”，不是“FIFO 溢出”。
> 两者要分开看。

### rpmsg 发送失败

日志：

```text
rpmsg tx failed, dropped N bytes
```

原因：

- 端点无效（`bridge_ept` 空）。
- A7 没在收（buffer 耗尽）。
- `bridge_dst` 错。

先确认握手成功，再确认 A7 侧在读 `/dev/rpmsgN`。

---

## 17.7 `ipm_overflow`：通知丢了但数据没丢

### 现象

- 高负载下偶发延迟。
- `trace0` 有溢出相关日志。
- 数据最终还能到（因为线程重扫 vring）。

### 原理

```c
if (k_msgq_put(&ipm_msgq, &notifyid, K_NO_WAIT) != 0) {
    atomic_set(&ipm_overflow, 1);
}
```

队列满 → 丢的是“通知”，不是“数据”。数据还在 vring 里。
线程处理完当前批次后：

```c
if (atomic_cas(&ipm_overflow, 1, 0)) {
    rproc_virtio_notified(rvdev.vdev, VRING0_ID);
    rproc_virtio_notified(rvdev.vdev, VRING1_ID);
}
```

重扫两个 vring，把漏掉的取出来。

### 什么时候要处理

- 偶发、能自愈：可以接受，记录即可。
- 频繁：说明队列太小或线程优先级太低。

改进：

```c
K_MSGQ_DEFINE(ipm_msgq, sizeof(uint32_t), 8, 4);  /* 加到 32/64 */
```

或者提高 `openamp_rx` 线程优先级。

> 这块板的设计原则是“宁可多重扫，不可丢数据”。
> 重扫的成本很低，丢数据的代价很高。

---

## 17.8 UART 层调试

### 确认端口参数

- 波特率：115200（默认）。
- 数据位：8。
- 校验：无。
- 停止位：1。
- 流控：无（默认）。

### 用 `stty`（如果目标板是 Linux）

```sh
stty -F /dev/ttyS0 115200 cs8 -cstopb -parenb -crtscts
```

### 回环测试

把 PE12 和 PE13 短接（**确认电压和引脚定义后再做**），
发送应该能收到自己。这验证 UART 硬件和引脚 mux。

### 逻辑分析仪/示波器

看 TX 线上有没有波形、波特率对不对。

```text
一位时间 = 1 / 波特率
115200 → 8.68 µs
```

如果实测位宽差很多，说明时钟源频率配错。

### 常见 UART 问题

| 现象 | 原因 |
|---|---|
| 全是乱码 | 波特率/时钟不对 |
| 偶尔错字节 | 干扰、地线、电平不匹配 |
| 收不到 | TX/RX 接反、mux 错、时钟没开 |
| 只能单向 | 对端 TX 没接/没配 |
| 大数据丢 | FIFO 溢出、ISR 太慢 |
| 特定字符错 | 校验/停止位不匹配 |

---

## 17.9 建立排查脚本

把常用检查写成脚本，省得每次手敲：

```sh
#!/bin/sh
# e907-status.sh

echo "== state =="
cat /sys/class/remoteproc/remoteproc0/state

echo "== firmware =="
cat /sys/class/remoteproc/remoteproc0/firmware

echo "== trace (tail) =="
tail -30 /sys/kernel/debug/remoteproc/remoteproc0/trace0

echo "== rpmsg nodes =="
ls -l /dev/rpmsg* 2>/dev/null || echo "none"

echo "== dmesg =="
dmesg | grep -i -E 'remoteproc|rpmsg|mbox' | tail -20

echo "== daemon =="
pgrep -a linkr-bee-daemon || echo "not running"
```

> 排查脚本的价值：**把“我现在该看什么”固化下来**，
> 避免慌乱中漏掉一层。

---

## 17.10 一个真实的排查案例

### 现象

> 网页终端能连上，但目标板没有任何输出；`@i?` 显示
> `uart rx=0 tx=0`。

### 排查过程

```text
1. state=running，trace0 有横幅        → E907 起来了
2. trace0 没有 "rpmsg_chrdev ready"    → OpenAMP 没初始化完
3. 往前看：卡在等 DRIVER_OK             → Linux 没填 vring
4. dmesg: "resource table" 相关错误     → 固件 resource table 有问题
5. readelf -S 固件，没有 .resource_table → 构建配置漏了
6. prj.conf 缺 CONFIG_OPENAMP_RSC_TABLE=y
```

### 修复

```conf
CONFIG_OPENAMP_RSC_TABLE=y
CONFIG_OPENAMP_RSC_TABLE_NUM_RPMSG_BUFF=8
CONFIG_OPENAMP_VRING_ALIGNMENT=16
```

重新构建、部署、重启 remoteproc，端点出现。

### 教训

- **“起来了”不等于“初始化完了”。**
- 看日志要看到最后一条，不是看到横幅就放心。
- resource table 是 E907 固件的硬性要求。

---

## 17.11 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| `state=crashed` | 异常/栈溢出/空指针 | `trace0` 的 FATAL、addr2line |
| `state=running` 无日志 | 跑飞/入口错 | 入口地址、链接地址 |
| 卡在等 DRIVER_OK | Linux 没填 vring | `dmesg`、resource table |
| 没有 `trace0` | RAM console 没配 | `.config` |
| 没有 `/dev/rpmsgN` | NS/名字/端点池 | `trace0`、`ls /dev` |
| 握手无回复 | 端点/回调没接上 | daemon 日志 |
| 单向数据 | `bridge_dst`/端点 | 握手是否成功 |
| `drop_rx` 增长 | 环满/消费慢 | `LINKRSTAT`、A7 负载 |
| `drop_tx` 增长 | 波特率低/对端不读 | 降速测试、接线 |
| UART 乱码 | 时钟/波特率 | 示波器、`clock-frequency` |
| 偶发延迟 | IPM 队列满 | `ipm_overflow`、加队列 |
| 重刷后行为不变 | overlay 旧文件/没重启 | 查 upper、重启 remoteproc |

---

## 17.12 练习

### 练习 17.1（必做）

在你的板上依次执行：

```sh
cat /sys/class/remoteproc/remoteproc0/state
cat /sys/class/remoteproc/remoteproc0/firmware
tail -30 /sys/kernel/debug/remoteproc/remoteproc0/trace0
ls -l /dev/rpmsg*
dmesg | grep -i -E 'remoteproc|rpmsg'
```

记录输出，并标注每一行对应哪一层。

### 练习 17.2（必做）

回答：

1. `state=running` 为什么不等于“固件正常”？
2. `trace0` 是文件还是环形缓冲？写满会怎样？
3. `drop_rx` 和 UART FIFO overrun 有什么区别？
4. `ipm_overflow` 丢了什么、没丢什么？
5. 为什么调试要保留未 strip 的 ELF？

### 练习 17.3（必做）

写一个 `e907-status.sh`，把 17.9 的内容实现出来，并在一台
正常板子上跑通。要求每个检查项有明确的“正常/异常”提示。

### 练习 17.4（选做）

在备用板上故意制造一次“卡在等 DRIVER_OK”：把
`CONFIG_OPENAMP_RSC_TABLE` 关掉，重新构建部署，记录
`state`、`trace0`、`dmesg` 的现象，然后恢复。

### 练习 17.5（选做）

用 `riscv32-zephyr-elf-addr2line` 把一次（人为的）崩溃
PC 翻译成源码行号。不需要真的改坏主力板，可以在主机上
对 ELF 做符号查询练习。

---

## 17.13 本章验收

1. 能读懂 remoteproc 的 `state`，并知道每种值的含义。
2. 会用 `trace0` 判断固件启动到哪一步。
3. 能区分 `crashed` 和“跑飞”。
4. 能按层排查“端点没出现”和“没数据”。
5. 会用 `LINKRSTAT` 和 drop 计数判断丢包位置。
6. 能解释 `ipm_overflow` 的补偿机制。
7. 有一个自己的排查脚本。

### 自测题

1. `state` 有哪几种值？
2. `trace0` 里哪一行表示 rpmsg 端点已经宣告？
3. 固件崩了，第一步看什么？
4. `drop_rx` 增长说明哪一层慢？
5. `ipm_overflow` 后线程做了什么？
6. UART 全是乱码，先查什么？

> 第 2 部分到此结束。你已经能设计、移植、编写、调试 E907 固件。
> 下一部分转到 A7：Linux 上的 daemon、Web、HTTPS、BLE。
