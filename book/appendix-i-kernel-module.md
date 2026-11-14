# 附录 I：最小内核模块（选读）

> 这本书的主线**不需要写内核模块**：UART3 由 E907 独占，A7 通过
> rpmsg 和用户态 daemon 工作。但读到这里你可能会问：
> “那驱动到底怎么写？”
>
> 这一篇给一个能跑的最小例子，以及“什么时候才真的需要它”。

---

## I.1 什么时候需要内核模块

| 需求 | 用户态能不能做 | 需要模块 |
|---|---|---|
| 读写 `/dev/ttyS*` | ✅ | ❌ |
| 通过 rpmsg 通信 | ✅ | ❌ |
| 操作 GPIO（sysfs/libgpiod） | ✅ | ❌ |
| 处理外设中断 | ❌ | ✅ |
| DMA 大块数据 | ❌ | ✅ |
| 写一个新的总线/子系统 | ❌ | ✅ |
| 改内核调度/内存行为 | ❌ | ✅ |
| 注册字符设备 | ❌（除非已有驱动） | ✅ |

> 判据：**“这件事需要在内核态、能碰中断/寄存器/内核对象吗？”**
> 不需要，就优先用户态。用户态崩溃不会带崩整机。

### 这块板为什么没有自定义模块

```text
UART3 的寄存器由 E907 固件直接操作
A7 通过 /dev/rpmsgN 拿到字节
daemon 是 Python 用户态程序
```

所以整条链路**不需要 A7 内核模块**。这是有意的设计，不是偷懒。

---

## I.2 最小可加载模块

### `linkr_demo.c`

```c
// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEV_NAME "linkr_demo"
#define BUF_SIZE 256

static char demo_buf[BUF_SIZE] = "hello from kernel\n";
static size_t demo_len = sizeof("hello from kernel\n") - 1;

static ssize_t demo_read(struct file *file, char __user *to,
                         size_t count, loff_t *ppos)
{
    return simple_read_from_buffer(to, count, ppos, demo_buf, demo_len);
}

static ssize_t demo_write(struct file *file, const char __user *from,
                          size_t count, loff_t *ppos)
{
    size_t n = min(count, (size_t)(BUF_SIZE - 1));

    if (copy_from_user(demo_buf, from, n))
        return -EFAULT;

    demo_buf[n] = '\0';
    demo_len = n;
    pr_info("linkr_demo: received %zu bytes\n", n);
    return n;
}

static const struct file_operations demo_fops = {
    .owner   = THIS_MODULE,
    .read    = demo_read,
    .write   = demo_write,
    .llseek  = no_llseek,
};

static struct miscdevice demo_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEV_NAME,
    .fops  = &demo_fops,
    .mode  = 0600,
};

static int __init demo_init(void)
{
    int ret = misc_register(&demo_dev);

    if (ret)
        pr_err("linkr_demo: misc_register failed: %d\n", ret);
    else
        pr_info("linkr_demo: loaded, /dev/%s\n", DEV_NAME);

    return ret;
}

static void __exit demo_exit(void)
{
    misc_deregister(&demo_dev);
    pr_info("linkr_demo: unloaded\n");
}

module_init(demo_init);
module_exit(demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Abduwaris Nurmemet <daabu@qq.com>");
MODULE_DESCRIPTION("Minimal misc device example for the book");
```

### 逐段解释

```text
misc_register  注册一个杂项设备，自动分配次设备号，自动创建 /dev 节点
file_operations   read/write/llseek 的实现
copy_from_user  从用户地址拷贝，必须检查返回值
copy_to_user    用 simple_read_from_buffer 包了一层，省事
pr_info         内核日志，dmesg 可见
MODULE_LICENSE("GPL")  内核模块必须声明，否则 taint
```

> 内核里**不能**直接解引用用户指针。所有用户地址都要
> `copy_from_user` / `copy_to_user`。

---

## I.3 Makefile

```make
obj-m += linkr_demo.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
```

### 主机上编译（只验证语法）

```sh
make
```

需要主机装了当前内核的头文件：

```sh
sudo apt install linux-headers-$(uname -r)
```

### 交叉编译到板子

```sh
make -C /path/to/board-kernel \
     M=$PWD \
     ARCH=arm \
     CROSS_COMPILE=arm-linux-gnueabi- \
     modules
```

产物：

```text
linkr_demo.ko
```

> 必须用**板子内核源码/配置**编译。版本不匹配会报
> `invalid module format`。

---

## I.4 加载与验证

```sh
# 拷到板子
scp -O linkr_demo.ko root@<board>:/tmp/

# 加载
insmod /tmp/linkr_demo.ko

# 看日志
dmesg | tail -5
# linkr_demo: loaded, /dev/linkr_demo

# 看设备节点
ls -l /dev/linkr_demo

# 读
cat /dev/linkr_demo
# hello from kernel

# 写
printf 'hello module\n' > /dev/linkr_demo
cat /dev/linkr_demo
# hello module

# 卸载
rmmod linkr_demo
dmesg | tail -3
```

### 看模块信息

```sh
lsmod | grep linkr
modinfo linkr_demo.ko
```

---

## I.5 带参数与 sysfs

### 模块参数

```c
static int verbose;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, "enable verbose logging");

static int __init demo_init(void)
{
    if (verbose)
        pr_info("linkr_demo: verbose enabled\n");
    ...
}
```

```sh
insmod linkr_demo.ko verbose=1
cat /sys/module/linkr_demo/parameters/verbose
echo 0 > /sys/module/linkr_demo/parameters/verbose
```

> 参数适合“加载时决定的行为”。**运行时频繁改的状态用 sysfs/ioctl。**

---

## I.6 设备树匹配（platform driver）

如果设备是“硬件上存在、地址/中断由设备树描述”，应该用
platform driver，而不是 misc device。

### 驱动侧

```c
#include <linux/of.h>
#include <linux/platform_device.h>

static int demo_probe(struct platform_device *pdev)
{
    struct resource *res;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res)
        return -ENODEV;

    dev_info(&pdev->dev, "reg=%pa len=%pa\n",
             &res->start, &resource_size(res));
    return 0;
}

static int demo_remove(struct platform_device *pdev)
{
    return 0;
}

static const struct of_device_id demo_of_match[] = {
    { .compatible = "linkr,demo" },
    { }
};
MODULE_DEVICE_TABLE(of, demo_of_match);

static struct platform_driver demo_driver = {
    .probe  = demo_probe,
    .remove = demo_remove,
    .driver = {
        .name = "linkr-demo",
        .of_match_table = demo_of_match,
    },
};
module_platform_driver(demo_driver);
```

### 设备树侧

```dts
demo@2501000 {
    compatible = "linkr,demo";
    reg = <0x02501000 0x1000>;
    status = "okay";
};
```

### 匹配流程

```text
设备树节点 compatible
  → 内核匹配 of_device_id
  → 调用 probe
  → 驱动拿 resource/irq/clk
  → 注册字符设备/子系统
```

> 这块板加新外设时，**先看设备树有没有节点，再看驱动有没有匹配**。
> 大多数“驱动没起来”是 compatible 写错或 status 是 disabled。

---

## I.7 中断与并发（只给骨架）

```c
static irqreturn_t demo_irq(int irq, void *data)
{
    /* 中断上下文：不能睡眠、不能 copy_to_user、不能拿会睡眠的锁 */
    return IRQ_HANDLED;
}

ret = request_irq(irq, demo_irq, IRQF_SHARED, "linkr-demo", dev);
...
free_irq(irq, dev);
```

### 必须记住

```text
- 中断里不能 sleep / mutex_lock / copy_to_user
- 用 spinlock 保护中断和进程上下文共享的数据
- 耗时工作丢到 workqueue/threaded IRQ
- 共享中断要检查是不是自己的设备
```

> 这块板的 UART3 中断在 E907 侧，不在 Linux 侧。**这正是双核的价值：
> 实时中断不经过 Linux。**

---

## I.8 常见错误

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| `invalid module format` | vermagic 不匹配 | `modinfo`、内核版本 |
| `Unknown symbol` | 依赖模块没加载/未导出 | `dmesg`、`modprobe` |
| `insmod: Operation not permitted` | 没 root/签名限制 | `id`、内核配置 |
| 加载后 oops | 空指针/越界 | `dmesg`、`addr2line` |
| 设备节点不存在 | misc 注册失败/udev | `dmesg`、`/sys/class/misc` |
| 读写返回 `-EFAULT` | 用户指针没检查 | `copy_*_user` |
| 卸载后崩溃 | 没释放资源/竞态 | `rmmod` 前停用 |
| 编译找不到头文件 | 没指定 `KDIR` | `make -C ...` |
| 模块被 taint | 许可证不是 GPL | `MODULE_LICENSE` |

### `dmesg` 是第一个看的地方

```sh
dmesg | tail -30
```

内核模块的错误几乎都会在这里留下痕迹。

---

## I.9 什么时候不该写模块

```text
❌ 只是想读一个文件/串口 → 用户态
❌ 只是想定时做一件事 → 用户态 timer/线程
❌ 只是想转发字节 → 用户态 daemon
❌ 只是想调一个 GPIO → libgpiod/sysfs
❌ 只想快速试一下 → 模块崩溃会带崩整机
```

> 模块是“进入内核的签证”，不是“更高级的写法”。
> **能用用户态解决，就不要进内核。**

---

## I.10 实验

### 实验 A：编译并加载最小模块

在**备用板**或虚拟机里：

```sh
make
insmod linkr_demo.ko
cat /dev/linkr_demo
printf 'test\n' > /dev/linkr_demo
cat /dev/linkr_demo
rmmod linkr_demo
dmesg | tail
```

### 实验 B：加一个模块参数

加 `verbose`，用 `insmod ... verbose=1` 验证日志。

### 实验 C：制造一次 oops（虚拟机）

在 `demo_init` 里故意解引用空指针，观察 `dmesg` 的
`BUG: unable to handle kernel NULL pointer dereference`，
然后用 `addr2line` 定位。

> **只在虚拟机/备用板做。** 主力板不要拿来试崩溃。

### 实验 D：platform driver 匹配

加一个 DTS 节点和 platform driver，确认 `probe` 被调用。

---

## I.11 本章验收

1. 能判断一个问题该用户态还是内核态解决。
2. 能编译、加载、卸载一个最小模块。
3. 理解 `file_operations`、`copy_*_user`、`misc_register`。
4. 知道 platform driver 如何通过 `compatible` 匹配。
5. 知道中断上下文不能做什么。
6. 能解释为什么这块板不需要自定义 A7 内核模块。

> 读完这一篇，你不一定立刻要写模块。
> 但下次看到 `/dev/xxx` 时，你知道它背后大概发生了什么。
