# 第 2 章 工位：主机、串口、SSH、Git、交叉编译

> 本章目标：
> 1. 把主机、串口、网络三条通道都打通。
> 2. 分清“主机编译”和“目标板编译”。
> 3. 能把自己的程序放到板子上跑起来。

---

## 2.1 工位长什么样

```text
┌──────────────┐        USB          ┌──────────────┐
│ Linux 主机    │────────────────────│ USB-TTL / 板子│
│              │                    │              │
│ 终端 / 编辑器 │        Wi-Fi        │  V851S       │
│ Git / 编译器  │────────────────────│  Linux       │
└──────────────┘                    └──────────────┘
```

三条通道：

| 通道 | 用途 | 工具 |
|---|---|---|
| 串口 | 板子没网、系统挂了时的最后入口 | `picocom`、`minicom` |
| SSH | 日常开发、传文件、跑命令 | `ssh`、`scp` |
| Git | 拉代码、看历史、提交修改 | `git` |

> 先打通串口，再打通 SSH。不要反过来。

---

## 2.2 主机环境

### 检查系统

```sh
uname -a
cat /etc/os-release
```

本书假设你是 Linux 主机。Windows 用户请用 WSL2 或 Ubuntu 虚拟机。

### 安装基础工具

```sh
sudo apt update
sudo apt install -y \
  build-essential git cmake ninja-build python3 python3-pip \
  device-tree-compiler picocom minicom \
  curl openssl net-tools iproute2 usbutils
```

### 检查版本

```sh
gcc --version
cmake --version
ninja --version
python3 --version
git --version
```

> 版本不需要和书里完全一致。能编译、能跑就行。

---

## 2.3 串口：最后一道门

### 找到设备

插上板子的调试口：

```sh
dmesg | tail -20
ls -l /dev/ttyACM* /dev/ttyUSB*
```

常见结果：

- `/dev/ttyACM0`：板载 USB 转串口（A7 控制台）。
- `/dev/ttyUSB0`：CH340，接 E907 的 UART3（目标串口）。

> 不同板子名字可能不同。看 `dmesg` 里“new full-speed USB device”
> 后面的驱动名，比猜更可靠。

### 权限

```sh
sudo usermod -aG dialout "$USER"
```

然后**退出登录再重新登录**。验证：

```sh
groups | grep dialout
```

### 打开串口

```sh
picocom -b 115200 /dev/ttyACM0
```

退出：

```text
Ctrl-A，然后 Ctrl-X
```

> 如果看到乱码，先确认波特率是 115200、8N1。
> 如果什么都没看到，按一下回车；有些系统要你“唤醒”控制台。

### 串口能做什么

- 看 bootloader 和内核日志。
- 登录 Linux（如果 getty 在跑）。
- 在系统起不来时进 U-Boot 或 rescue。
- 直接看 E907 的目标串口（`/dev/ttyUSB0`）。

> 串口不依赖网络、不依赖 SSH 服务，所以它是“最后一道门”。

---

## 2.4 SSH：日常开发的主通道

### 登录

```sh
ssh root@<board-ip>
```

第一次会问指纹，输入 `yes`。

### 免密登录（推荐）

```sh
ssh-keygen -t ed25519
ssh-copy-id root@<board-ip>
```

> 私钥不要提交到 Git。不要把它拷到公共镜像里。

### 传文件

板子上的 `scp` 可能不支持 SFTP 协议，使用：

```sh
scp -O local_file root@<board-ip>:/tmp/
```

`-O` 强制走旧版 SCP 协议，兼容性更好。

### 常用远程命令

```sh
ssh root@<board-ip> 'cat /etc/rootfs_version'
ssh root@<board-ip> 'ps w | grep linkr'
ssh root@<board-ip> 'tail -20 /var/log/linkr-bee.log'
```

> 把“登录 + 敲命令 + 退出”合成一条，适合脚本化。

---

## 2.5 Git：不要用“最终版.zip”

### 为什么必须用 Git

- 你能看到自己改了什么。
- 改坏了可以回退。
- 能对比“书上的代码”和“板上的代码”。
- 能提交给别人 review。

### 基本操作

```sh
git clone <repo-url>
cd v851s-e907-app
git status
git log --oneline -5
git diff
```

### 本书的建议

- 每做一章实验，提交一次。
- 提交信息写“做了什么”，不要写“update”。
- 不要提交密码、私钥、镜像、SDK。

示例：

```sh
git add book/00-how-to-use.md
git commit -m "docs: add chapter 0"
```

---

## 2.6 编译：主机和目标板不是一回事

### 主机编译

```sh
gcc hello.c -o hello
./hello
```

这个 `hello` 只能在你的 x86 主机上跑。

### 目标板编译

板子是 ARM 32 位（V851S 的 A7）。你需要交叉编译器：

```sh
arm-linux-gnueabihf-gcc hello.c -o hello-arm
file hello-arm
```

预期：

```text
hello-arm: ELF 32-bit LSB executable, ARM, EABI5 ...
```

把这个文件拷到板子上：

```sh
scp -O hello-arm root@<board-ip>:/tmp/
ssh root@<board-ip> '/tmp/hello-arm'
```

### 为什么不能直接拷主机程序

因为指令集不同、动态库不同、系统调用接口也可能不同。
就像你不能把 Windows 的 `.exe` 直接放到 Linux 上跑。

### 静态编译和动态编译

```sh
# 动态链接：依赖板子上的 libc
arm-linux-gnueabihf-gcc hello.c -o hello-arm

# 静态链接：把库打进程序，体积大但依赖少
arm-linux-gnueabihf-gcc -static hello.c -o hello-arm-static
```

查看依赖：

```sh
arm-linux-gnueabihf-readelf -d hello-arm | grep NEEDED
```

> 如果板子上报 `not found` 或 `No such file or directory`，
> 先看解释器/动态库是否存在，不要先怀疑代码。

---

## 2.7 一次完整的“写—传—跑”

### 1. 写

```c
/* hello.c */
#include <stdio.h>

int main(void)
{
    printf("hello from target\n");
    return 0;
}
```

### 2. 交叉编译

```sh
arm-linux-gnueabihf-gcc -static hello.c -o hello
file hello
```

### 3. 传

```sh
scp -O hello root@<board-ip>:/tmp/
```

### 4. 跑

```sh
ssh root@<board-ip> 'chmod +x /tmp/hello; /tmp/hello'
```

预期：

```text
hello from target
```

> 这四步你会重复几百次。把它写成脚本，能省很多时间。

---

## 2.8 看日志：不要靠猜

### 内核日志

```sh
dmesg | tail -50
```

### 服务日志

```sh
tail -50 /var/log/linkr-bee.log
tail -50 /var/log/bluetooth-init.log
```

### E907 日志

```sh
tail -50 /sys/kernel/debug/remoteproc/remoteproc0/trace0
```

### 实时看

```sh
tail -f /var/log/linkr-bee.log
```

> 日志要看“时间、进程、错误码”，不要只看最后一行。
> 很多问题在更早的几十行里。

---

## 2.9 常见错误

### `Permission denied` 打开串口

- 你不在 `dialout` 组。
- 或者另一个程序占用了串口（`picocom` 没退干净）。

```sh
fuser /dev/ttyACM0
```

### `No such file or directory`

- 文件真的不存在。
- 或者是“解释器不存在”：`file` 看架构，`readelf -l` 看 interpreter。

### `Exec format error`

- 你把 x86 程序拷到了 ARM 板子上。
- 重新交叉编译。

### `Text file busy`

- 程序正在运行，不能覆盖。
- 先停进程，再拷贝。

### SSH 连不上但串口正常

- 板子没联网。
- `sshd` 没起。
- IP 变了。
- 防火墙/路由问题。

> 先串口，后网络。这是嵌入式的铁律。

---

## 2.10 本章验收

1. 能用 `picocom` 打开板子串口，并退出。
2. 能用 SSH 登录板子，并执行一条远程命令。
3. 能交叉编译一个 `hello`，传到板子并运行。
4. 能用 `file` 和 `readelf` 解释“为什么主机程序不能在板子上跑”。
5. 能说出串口、SSH、Git 各自不可替代的场景。

### 自测题

1. `scp -O` 里的 `-O` 是干什么的？
2. 动态链接的程序在板子上报“not found”，可能缺什么？
3. 为什么串口是“最后一道门”？
4. 日志应该从哪一行开始看？
5. 交叉编译器和主机编译器有什么区别？

> 工位准备好了，下一章补 C。不是学完整本 C 教材，
> 而是学“看懂这份代码所需要的 20%”。
