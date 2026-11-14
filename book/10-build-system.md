# 第 10 章 交叉编译、CMake、Kconfig

> 本章目标：
> 1. 明白为什么主机上编译的程序不能在板子上跑。
> 2. 会用交叉工具链编译、检查、传输、运行一个程序。
> 3. 看懂 CMake 的最小工程和交叉编译配置。
> 4. 知道 Zephyr 的构建流程，以及 `prj.conf`/Kconfig 在做什么。
> 5. 能独立排查“架构不对、库缺失、符号未定义”三类错误。

---

## 10.1 为什么不能直接 `gcc hello.c`

你的主机大概率是 x86_64：

```sh
uname -m
# x86_64
```

板子的 A7 是 ARM 32 位：

```sh
# 在板子上
uname -m
# armv7l
```

`gcc hello.c` 生成的是 x86_64 指令。拿到 ARM 上运行：

```text
-sh: ./hello: Exec format error
```

因为：

- **指令集不同**：x86 的机器码 ARM 不认识。
- **ABI 不同**：函数调用约定、寄存器用途、结构体对齐。
- **动态库不同**：x86 的 `libc.so.6` 不能给 ARM 用。
- **系统调用接口**：虽然都是 Linux，但架构相关部分不同。

所以需要**交叉编译器**：在 x86 主机上运行，生成 ARM 代码。

### 交叉编译的三个词

| 词 | 含义 | 例子 |
|---|---|---|
| host | 编译器运行在哪 | x86_64 Linux |
| target | 生成什么架构的代码 | ARM 32 位 |
| triple | 工具链命名三元组 | `arm-linux-gnueabihf` |

`arm-linux-gnueabihf` 拆开：

```text
arm      架构
linux    操作系统
gnueabihf  ABI：GNU + EABI + 硬浮点（hard float）
```

### 软浮点与硬浮点

- `gnueabi`：软浮点，浮点参数通过整数寄存器传。
- `gnueabihf`：硬浮点，用 FPU 寄存器传。

这块板的 A7 有硬件浮点，Tina 用的是 `gnueabihf`。
**ABI 不匹配时，链接能过，运行会崩或结果错。**

---

## 10.2 工具链里有什么

```sh
ls /opt/toolchain/bin | head
```

典型内容：

```text
arm-linux-gnueabihf-gcc
arm-linux-gnueabihf-g++
arm-linux-gnueabihf-ld
arm-linux-gnueabihf-as
arm-linux-gnueabihf-objdump
arm-linux-gnueabihf-readelf
arm-linux-gnueabihf-strip
```

还有：

- **sysroot**：目标板的头文件和库的集合。
- **动态链接器**：`ld-linux-armhf.so.3`。
- **libc**：`libc.so.6`、`libm.so.6` 等。

```sh
arm-linux-gnueabihf-gcc -print-sysroot
arm-linux-gnueabihf-gcc -print-file-name=libc.so.6
```

### 工具链从哪来

| 来源 | 适用 |
|---|---|
| Tina SDK 自带 | A7 用户态程序、内核模块 |
| Zephyr SDK | E907 固件（RISC-V） |
| 发行版包 `gcc-arm-linux-gnueabihf` | 练手、简单程序 |
| 厂商提供 | 特定 libc/ABI |

> 给板子编译程序时，**优先用构建该 rootfs 的同一套工具链**。
> 用发行版工具链编出来的程序，可能依赖更新版本的 glibc，
> 在板子上报 `GLIBC_2.xx not found`。

---

## 10.3 实验：编译一个能在板子上跑的程序

### 1. 写代码

```c
/* hello.c */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("hello from ");
    fflush(stdout);
    execl("/bin/uname", "uname", "-m", (char *)NULL);
    perror("execl");
    return 0;
}
```

### 2. 交叉编译

```sh
arm-linux-gnueabihf-gcc -Wall -O2 hello.c -o hello
file hello
```

预期：

```text
hello: ELF 32-bit LSB executable, ARM, EABI5 ...
```

### 3. 看依赖

```sh
arm-linux-gnueabihf-readelf -l hello | grep interpreter
arm-linux-gnueabihf-readelf -d hello | grep NEEDED
```

预期类似：

```text
[Requesting program interpreter: /lib/ld-linux-armhf.so.3]
0x00000001 (NEEDED)  Shared library: [libc.so.6]
```

### 4. 传到板子并运行

```sh
scp -O hello root@<board-ip>:/tmp/
ssh root@<board-ip> 'chmod +x /tmp/hello; /tmp/hello'
```

预期：

```text
hello from armv7l
```

### 5. 如果失败

| 错误 | 原因 | 解决 |
|---|---|---|
| `Exec format error` | 架构不对 | 用交叉编译器重编 |
| `No such file or directory` | 动态链接器不存在 | 静态编译，或换匹配的 libc |
| `not found`（运行库） | 缺 `.so` | 把库带上去，或静态编译 |
| `GLIBC_2.xx not found` | 板子 libc 太旧 | 用板子同源工具链 |
| `Permission denied` | 没有执行权限 | `chmod +x` |

### 静态编译

```sh
arm-linux-gnueabihf-gcc -static -O2 hello.c -o hello-static
file hello-static
```

静态版本把 libc 打进可执行文件：

- 优点：不依赖板子上的库，拷过去就能跑。
- 缺点：体积大（几 MB），多个程序重复打包 libc。

> 调试小工具、救砖工具适合静态；正常系统服务适合动态，
> 省空间、好升级。

---

## 10.4 看二进制：`file`、`readelf`、`objdump`

### `file`

```sh
file hello
```

一眼看架构、位数、动态/静态。

### `readelf`

```sh
readelf -h hello     # ELF 头：架构、入口、类型
readelf -l hello     # 段表：INTERP、LOAD
readelf -d hello     # 动态段：NEEDED、RPATH
readelf -s hello     # 符号表
```

### `objdump`

```sh
arm-linux-gnueabihf-objdump -d hello | head -40
```

看反汇编。调试“为什么这段 C 编译成这样”时有用，
但大多数时候不需要。

### `ldd`

```sh
arm-linux-gnueabihf-ldd hello
```

`ldd` 在主机上跑的是主机动态链接器，结果可能不准。
更可靠的是在板子上：

```sh
ldd /tmp/hello
```

或者用 `readelf -d` 看 `NEEDED`。

---

## 10.5 从 Make 到 CMake

### Make 的问题

```make
hello: hello.c
    $(CC) $(CFLAGS) -o $@ $<
```

简单，但项目一大：

- 依赖关系要手写。
- 跨平台/跨工具链要写一堆条件。
- 找不到库、找不到头文件时错误难懂。

### CMake 的思路

```text
CMakeLists.txt（描述“要构建什么”）
        │
        ▼
   CMake 生成器
        │
        ├─ Ninja
        ├─ Make
        └─ ...
        ▼
   实际构建命令
```

CMake 不直接编译，它**生成构建文件**，再由 Ninja/Make 执行。

### 最小 CMake 工程

```cmake
cmake_minimum_required(VERSION 3.20)
project(hello C)

add_executable(hello hello.c)
target_compile_options(hello PRIVATE -Wall -Wextra)
```

```sh
cmake -B build -GNinja
ninja -C build
./build/hello
```

### 交叉编译工具链文件

```cmake
# toolchain-arm.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# 如果工具链有独立 sysroot
# set(CMAKE_SYSROOT /path/to/sysroot)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

```sh
cmake -B build-arm -GNinja \
      -DCMAKE_TOOLCHAIN_FILE=toolchain-arm.cmake
ninja -C build-arm
file build-arm/hello
```

> 工具链文件必须**在第一次配置时**指定。已经配置过的
> build 目录改工具链，最好删掉重建。

---

## 10.6 Zephyr 的构建：CMake + Kconfig + 设备树

### 仓库里的 `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(v851s_e907_bringup)

target_sources(app PRIVATE
  src/main.c
  src/openamp_slave.c
  src/linkr_bridge.c
  src/board_early.c
)
```

逐行：

- `find_package(Zephyr ...)`：引入 Zephyr 的构建系统。
- `project(...)`：工程名。
- `target_sources(app ...)`：把源文件加进 `app` 目标。

Zephyr 的 `app` 是一个预定义目标，最终链接成
`zephyr_e907.elf`（名字由 Kconfig 决定）。

### 构建命令

```sh
export ZEPHYR_BASE=/path/to/zephyr   # 带 v851s BSP 的分支
cmake -B build -GNinja -DBOARD=v851s_e907 .
ninja -C build
```

产物：

```text
build/zephyr/zephyr_e907.elf
```

也可以：

```sh
west build -b v851s_e907 .
```

`west` 是 Zephyr 的元工具，底层还是调用 CMake + Ninja。

### 构建过程做了什么

```text
1. 读 board defconfig + prj.conf → 生成 .config
2. 读设备树（board.dts + overlay）→ 生成 devicetree 头文件
3. 编译 C 文件
4. 链接 ELF（包含 .resource_table）
5. 生成 .bin/.hex（可选）
```

### 看构建结果

```sh
ninja -C build
ls build/zephyr/
cat build/zephyr/.config | head
grep -n 'uart3' build/zephyr/zephyr.dts
```

`build/zephyr/.config` 是最终 Kconfig 配置。
`zephyr.dts` 是最终设备树。
**出问题时先看这两个文件，不要猜。**

---

## 10.7 Kconfig：编哪些功能进来

### `prj.conf` 是什么

```conf
CONFIG_KERNEL_BIN_NAME="zephyr_e907"
CONFIG_RING_BUFFER=y
CONFIG_RING_BUFFER_LARGE=y
CONFIG_OPENAMP_RSC_TABLE_NUM_RPMSG_BUFF=8
```

它是应用的“功能开关清单”。Zephyr 的构建系统把它和
板级 defconfig 合并，生成 `.config`。

### 常见符号

| 符号 | 作用 |
|---|---|
| `CONFIG_LOG=y` | 打开日志 |
| `CONFIG_PRINTK=y` | 打开 `printk` |
| `CONFIG_SERIAL=y` | 串口驱动 |
| `CONFIG_UART_NS16550=y` | ns16550 驱动 |
| `CONFIG_MAIN_STACK_SIZE=...` | 主线程栈大小 |
| `CONFIG_HEAP_MEM_POOL_SIZE=...` | 堆大小 |
| `CONFIG_RING_BUFFER=y` | 环形缓冲 API |
| `CONFIG_OPENAMP=y` | OpenAMP 支持 |

### 依赖关系

Kconfig 符号有依赖。例如：

```conf
CONFIG_OPENAMP=y
```

可能自动选中：

```conf
CONFIG_IPC_SERVICE=y
CONFIG_MBOX=y
```

如果你写的符号没生效，先看 `.config` 里它的值，再看：

```sh
grep -n 'OPENAMP' build/zephyr/.config
```

如果显示 `# CONFIG_OPENAMP is not set`，说明依赖没满足，
或者被别的符号覆盖。

### `menuconfig`

```sh
ninja -C build menuconfig
```

或者：

```sh
west build -t menuconfig
```

打开一个文本界面，可以搜索符号、看依赖、改配置。
改完保存，会写到 `build/zephyr/.config`，**不会自动写回
`prj.conf`**。要固化，手动把需要的行加回 `prj.conf`。

### 为什么不用 `#define`

```c
#define CONFIG_LOG 1   /* 错误：这不是 Zephyr 的开关 */
```

Kconfig 的值由构建系统生成，写在
`build/zephyr/include/generated/autoconf.h`。你自己 `#define`
会覆盖或冲突。

正确用法：

```c
#if IS_ENABLED(CONFIG_LOG)
    ...
#endif
```

---

## 10.8 实验：改一个 Kconfig，观察结果

### 步骤

1. 在 `prj.conf` 里加：

   ```conf
   CONFIG_THREAD_NAME=y
   ```

2. 重新构建：

   ```sh
   ninja -C build
   ```

3. 看 `.config`：

   ```sh
   grep THREAD_NAME build/zephyr/.config
   ```

4. 如果构建报“符号不存在”，说明当前 Zephyr 版本没有这个
   符号，或者依赖没满足。用 `menuconfig` 搜索确认。

### 观察固件大小变化

```sh
ls -l build/zephyr/zephyr_e907.elf
```

改配置后重新构建，再对比。**Kconfig 直接决定固件里
有哪些代码。**

> 不要一次加十几个开关。一次改一个，构建、验证、
> 记录变化。否则出问题不知道是谁导致的。

---

## 10.9 构建产物与部署

### Zephyr 侧

```text
build/zephyr/zephyr_e907.elf
  ├─ 代码/数据
  ├─ .resource_table（remoteproc 必需）
  └─ 调试符号
```

部署：

```sh
# 仓库里保存一份
cp build/zephyr/zephyr_e907.elf firmware/zephyr_e907.elf

# 板内两个名字
cp zephyr_e907.elf /lib/firmware/e907_firmware.elf
cp zephyr_e907.elf /lib/firmware/zephyr.elf
```

为什么两个名字：

- `/usr/bin/start_e907` 写的是 `e907_firmware.elf`。
- 设备树 `firmware-name` 写的是 `zephyr.elf`。

两个都放，哪条路径都能启动。

### A7 用户态

```text
交叉编译 → scp 到 /tmp → chmod +x → 运行
确认稳定后 → 放进 rootfs 或 /usr/bin
```

不要直接覆盖正在运行的程序：

```text
Text file busy
```

先停服务，再替换，再启动。

### 内核模块

```sh
# 在 SDK 内核树里
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
```

产物 `.ko` 要和**运行中的内核版本**完全匹配，否则
`insmod` 报 `Invalid module format`。

```sh
uname -r
modinfo uart3pin.ko | grep vermagic
```

两边必须一致。

---

## 10.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| `Exec format error` | 架构不对 | `file`、重新交叉编译 |
| `No such file or directory`（明明文件在） | 动态链接器缺失 | `readelf -l` 看 INTERP |
| `GLIBC_2.xx not found` | 工具链/rootfs 不匹配 | 用 SDK 工具链 |
| `cannot find -lxxx` | 链接期缺库 | 加 `-L`、装 dev 包 |
| `undefined reference` | 缺源文件/库/顺序错 | 看链接命令 |
| `Invalid module format` | 内核版本/配置不匹配 | `vermagic` |
| Kconfig 改了没生效 | 依赖不满足/没重建 | 看 `.config` |
| 设备树改了没生效 | 没重编 dtb/没刷 | 看 `zephyr.dts` |
| CMake 找不到工具链 | 路径/权限/没指定 | `CMAKE_TOOLCHAIN_FILE` |
| build 目录越改越乱 | 旧缓存 | 删掉重建 |

---

## 10.11 练习

### 练习 10.1（必做）

完成 10.3 的 hello 实验，并回答：

1. `file` 输出里哪一项说明它是 ARM？
2. `readelf -l` 里的 interpreter 是什么？
3. 静态版本和动态版本大小差多少？为什么？

### 练习 10.2（必做）

写一个 `toolchain-arm.cmake`，用 CMake 交叉编译 hello，
并用 `file` 验证产物架构。

### 练习 10.3（必做）

在 Zephyr 构建目录里：

```sh
grep -n 'CONFIG_RING_BUFFER' build/zephyr/.config
grep -n 'CONFIG_OPENAMP_RSC_TABLE_NUM_RPMSG_BUFF' build/zephyr/.config
```

解释这两个值分别控制什么。如果把它们改小/关掉，
固件行为会怎样？

### 练习 10.4（选做）

故意把 `CONFIG_KERNEL_BIN_NAME` 改成别的名字，重新构建，
观察产物文件名变化。然后改回来。

### 练习 10.5（选做）

用 `arm-linux-gnueabihf-objdump -d` 看 `main` 的反汇编，
找到调用 `printk` 的地方。不需要看懂全部，只要能定位。

---

## 10.12 本章验收

1. 能解释 host/target/triple，以及 `gnueabihf` 的含义。
2. 能独立交叉编译、传输、运行一个 ARM 程序。
3. 能用 `file`/`readelf` 判断架构、动态链接器、依赖库。
4. 能写最小 CMake 工程和交叉编译工具链文件。
5. 能说清 Zephyr 构建的三条输入：defconfig + prj.conf + 设备树。
6. 能排查“架构不对、库缺失、符号未定义、模块版本不匹配”。

### 自测题

1. 为什么 `gcc hello.c` 的程序在 ARM 上跑不了？
2. `gnueabi` 和 `gnueabihf` 有什么区别？
3. `readelf -d` 里的 `NEEDED` 表示什么？
4. 静态链接的优缺点各是什么？
5. `find_package(Zephyr)` 做了什么？
6. `prj.conf` 和 `.config` 是什么关系？
7. 为什么内核模块必须和运行中的内核版本匹配？

> 第 1 部分到此结束。你已经能看懂硬件、启动、存储、设备树和构建。
> 下一部分进入这套系统真正的核心：两个 CPU 怎么通信。
