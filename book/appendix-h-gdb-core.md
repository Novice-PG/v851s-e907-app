# 附录 H：GDB、core dump 与崩溃分析

> 这一篇补上“程序为什么会崩”的工具链。
> 书里第 17 章讲 E907 的 RAM console 和分层排查；这里讲更通用的
> GDB、core dump、反汇编和符号定位。

---

## H.1 先分清三种“崩”

```text
1. 编译/链接失败      → 看编译器报错
2. 运行中崩溃（段错误）→ core dump + GDB
3. 系统级崩溃（panic） → 串口日志 + 符号 + 反汇编
```

三种用的工具不同。**不要拿 GDB 去查编译错误。**

---

## H.2 准备工作：符号

### 不要 strip 掉调试信息

```sh
# 发布用（小）
arm-linux-gnueabi-strip myprog -o myprog.stripped

# 调试用（带符号）
arm-linux-gnueabi-gcc -g -O0 -o myprog myprog.c
```

保留一份**和板上二进制完全一致**的带符号文件：

```sh
# 板上
md5sum /usr/bin/myprog

# 主机
md5sum myprog
```

> 符号文件对不上，GDB 会给你“看起来像”的错误行号。
> **先核对 md5。**

### 优化等级的影响

```text
-O0  变量都在，单步最准
-O2  变量可能被优化掉，行号会跳
-g   保留符号（和优化等级无关）
```

调试时优先用 `-O0 -g`；性能问题再用 `-O2 -g`。

---

## H.3 交叉 GDB

### 找到对应的 gdb

```sh
# ARM 32 位
arm-linux-gnueabi-gdb
arm-none-linux-gnueabi-gdb

# RISC-V
riscv32-zephyr-elf-gdb
riscv64-unknown-elf-gdb

# 通用（能切架构）
gdb-multiarch
```

```sh
gdb-multiarch -q myprog
(gdb) set architecture arm
(gdb) set architecture riscv:rv32
```

### 最常见的错误

```text
用主机的 gdb 调试目标程序 → 架构不匹配/符号乱
```

> 用工具链自带的 gdb，或者 `gdb-multiarch` 并显式设架构。

---

## H.4 调试一个 Linux 用户态程序

### 方式一：本地跑（同架构或 QEMU）

```sh
gdb --args ./myprog --host 192.168.0.121
(gdb) run
```

### 方式二：attach 到板上进程

板上：

```sh
# 确认有 gdbserver
which gdbserver
gdbserver :2345 /usr/bin/myprog
# 或 attach
gdbserver :2345 --attach $(pidof myprog)
```

主机：

```sh
gdb-multiarch /path/to/myprog
(gdb) set sysroot /path/to/board-rootfs
(gdb) target remote 192.168.0.121:2345
(gdb) continue
```

`set sysroot` 让 GDB 在主机上找到板子的库符号。

### 方式三：core dump

板上：

```sh
ulimit -c unlimited
cat /proc/sys/kernel/core_pattern
# 常见：core 或 /tmp/core.%e.%p
```

触发崩溃后：

```sh
ls -l core*
```

把 core 和二进制一起拷回主机：

```sh
scp -O root@<board>:/tmp/core.1234 .
scp -O root@<board>:/usr/bin/myprog .
```

主机：

```sh
gdb-multiarch ./myprog ./core.1234
(gdb) bt full
(gdb) info registers
(gdb) thread apply all bt
```

### 常用命令

```text
bt                 调用栈
bt full            调用栈 + 局部变量
frame N            切到第 N 帧
info locals        当前帧局部变量
info args          当前帧参数
info registers     寄存器
x/16wx 0xADDR      看内存（16 个 32 位字）
disassemble /m     带源码的反汇编
list               看源码
thread apply all bt 所有线程栈
```

---

## H.5 调试一个崩溃的守护进程

### 先看日志

```sh
tail -100 /var/log/myprog.log
dmesg | tail -50
```

段错误时内核会打印：

```text
segfault at 0 ip 00000000... sp ... error 4 in myprog[...]
```

`ip` 是指令地址。用 `addr2line` 直接定位：

```sh
arm-linux-gnueabi-addr2line -e myprog -f -C 0x0000abcd
```

### 再看 core

如果没有 core，检查：

```sh
ulimit -c
cat /proc/sys/kernel/core_pattern
```

有些系统把 core 交给 `systemd-coredump` 或直接丢弃。

### 最后才上 GDB

GDB 适合“能复现、能停下来”的问题。**线上崩溃优先 core + 日志。**

---

## H.6 调试 E907 固件

### 没有调试器时

```text
RAM console（trace0）
LINKRSTAT 计数
panic 信息
addr2line 反查
```

```sh
riscv32-zephyr-elf-addr2line -e build/zephyr/zephyr_e907.elf -f -C 0x43c01234
```

### 有 JTAG/调试器时

不同探针命令不同，通用流程：

```text
1. 连接探针
2. halt
3. load ELF
4. 设断点
5. continue
```

GDB 侧（以 OpenOCD 为例）：

```sh
gdb-multiarch build/zephyr/zephyr_e907.elf
(gdb) target remote :3333
(gdb) monitor reset halt
(gdb) load
(gdb) break main
(gdb) continue
```

> 具体 `monitor` 命令取决于探针/OpenOCD 配置。**不要照抄别板子的。**
> 没有调试器也能工作——这也是第 17 章强调 RAM console 的原因。

### 固件崩溃的常见原因

```text
- 栈溢出
- 空指针/野指针
- 堆用完（k_malloc 返回 NULL 没检查）
- 中断里做了阻塞操作
- 共享内存越界
- 时钟/引脚配置错导致外设异常
```

---

## H.7 反汇编与符号

### 找函数地址

```sh
arm-linux-gnueabi-nm -n myprog | grep main
riscv32-zephyr-elf-nm -n build/zephyr/zephyr_e907.elf | grep linkr
```

### 看一段代码

```sh
arm-linux-gnueabi-objdump -d -S myprog | less
riscv32-zephyr-elf-objdump -d -S build/zephyr/zephyr_e907.elf | less
```

### 看段和大小

```sh
size myprog
readelf -S myprog
readelf -l myprog
```

### 地址 → 源码行

```sh
addr2line -e myprog -f -C 0x1234
```

> 优化后的代码行号可能“跳来跳去”。**看反汇编确认实际执行路径。**

---

## H.8 内存问题：先上工具

GDB 能看崩溃现场，但不擅长发现“越界写”。

### AddressSanitizer（主机/部分目标）

```sh
gcc -fsanitize=address -g -o myprog myprog.c
./myprog
```

能抓到：

```text
heap-buffer-overflow
use-after-free
stack-buffer-overflow
```

### Valgrind（主机）

```sh
valgrind --leak-check=full ./myprog
```

目标板通常跑不动 Valgrind（内存/CPU 不够），所以在主机上跑逻辑测试。

### 嵌入式上的土办法

```text
- 在缓冲区前后放哨兵值（0xDEADBEEF）
- 定期检查哨兵有没有被改
- 给队列加高水位/低水位计数
- 打开内核的 slub_debug/KASAN（如果内核支持）
```

> 工具能解决大部分“神秘崩溃”。**不要一上来就靠眼睛看代码。**

---

## H.9 一个排查顺序

```text
1. 现象：崩在什么时候？能复现吗？
2. 日志：dmesg / 应用日志 / trace0
3. 地址：ip / PC 是多少？
4. 符号：addr2line 定位到函数/行
5. 现场：core 或 GDB，看栈和寄存器
6. 假设：最可能的原因是什么？
7. 验证：改一处、加一个断言、再跑
```

### 不要做的事

```text
❌ 没有符号文件就开始 GDB
❌ 用错架构的 gdb
❌ 在 -O2 下怪“变量看不见”
❌ 看到崩溃就重写整个模块
❌ 不复现就“修好了”
```

---

## H.10 实验

### 实验 A：制造一个段错误

```c
int main(void) {
    int *p = 0;
    *p = 42;
    return 0;
}
```

```sh
gcc -g -O0 -o crash crash.c
ulimit -c unlimited
./crash
gdb ./crash core
(gdb) bt
```

### 实验 B：用 addr2line

从 `dmesg` 或 GDB 拿一个地址，反查源码行。

### 实验 C：attach 到守护进程

```sh
gdbserver :2345 --attach $(pidof myprog)
# 主机 gdb 连接，执行 bt、info threads
```

### 实验 D：E907 崩溃定位

在备用板上故意触发一次 panic（例如空指针），
用 `trace0` + `addr2line` 定位到函数。

### 实验 E：符号核对

比较板上二进制和主机符号文件的 md5，故意用错版本，
观察 GDB 给出的错误行号。

---

## H.11 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| GDB 连不上 | 端口/防火墙/gdbserver 没起 | `ss -ltn`、进程 |
| 栈全是 `??` | 没有符号/架构错 | `file`、`set architecture` |
| 行号不对 | 二进制和符号不匹配 | md5 |
| 变量被优化掉 | `-O2` | 换 `-O0 -g` |
| core 不生成 | `ulimit`/core_pattern | 两个都查 |
| `addr2line` 无输出 | 地址错/无符号 | `nm` 对照 |
| attach 后程序卡住 | 断点/停止状态 | `continue` |
| 反汇编和源码对不上 | 优化/内联 | 看实际指令 |

---

## H.12 本章验收

1. 能区分编译错误、运行崩溃、内核 panic。
2. 会为交叉目标选择正确的 GDB。
3. 能用 core dump + `bt full` 定位崩溃。
4. 会用 `addr2line` 把地址翻译成源码行。
5. 知道 E907 没有调试器时怎么定位。
6. 能说清“符号必须和板上二进制一致”。

> 工具是手的延伸。**先有假设，再用工具验证。**
