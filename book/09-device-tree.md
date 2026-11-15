# 第 9 章 设备树：内核怎么知道板子上有什么

> 本章目标：
> 1. 看懂 `.dts` / `.dtsi` / `.dtb` 的关系。
> 2. 能读懂节点、属性、`compatible`、`reg`、`status`。
> 3. 理解内核怎么用 `compatible` 找到驱动、怎么拿到地址和中断。
> 4. 知道这块板的 `reserved-memory`、`e907_rproc`、`uart3` 怎么写。
> 5. 能在 Zephyr 里用 `DT_NODELABEL` 拿到设备。

---

## 9.1 为什么需要设备树

同一颗 SoC 可以做成很多块板子：

- 板 A 的 UART3 接 RS-485，板 B 接蓝牙。
- 板 A 的 I2C1 接温湿度传感器，板 B 接屏幕。
- 板 A 有 64 MB DDR，板 B 有 128 MB。

如果把这些都写进内核源码，每换一块板就要改内核、重编译。
设备树把“板子上有什么、接在哪、怎么配”抽成一份**数据文件**，
内核只负责解释它。

```text
内核（通用逻辑） + 设备树（板级数据） = 能跑的板级系统
```

> 一句话：**设备树是硬件的配置文件，不是驱动。**
> 它告诉内核“这里有块硬件”，驱动告诉内核“怎么操作它”。

---

## 9.2 三种文件

| 后缀 | 是什么 | 谁读 |
|---|---|---|
| `.dts` | 设备树源文件（板级） | 人写 |
| `.dtsi` | 被 include 的源文件（SoC 级） | 人写 |
| `.dtb` | 编译后的二进制 | 内核/U-Boot |

关系：

```text
soc.dtsi  ──include──┐
                     ├──► board.dts ──dtc──► board.dtb
board.dts  ──────────┘
```

- `.dtsi` 描述 SoC 内部有哪些控制器、地址是多少。
- `.dts` 描述这块板子怎么用它们（哪个口开、接什么）。
- `dtc`（Device Tree Compiler）把源文件编译成 `.dtb`。

### 看一个最小例子

```dts
/ {
    model = "YuzukiPI-Lizard";
    compatible = "yuzuki,lizard", "allwinner,sun8iw21p1";

    chosen {
        bootargs = "console=ttyS0,115200";
    };

    memory@40000000 {
        device_type = "memory";
        reg = <0x40000000 0x04000000>;   /* 64 MiB */
    };

    uart3: serial@2500c00 {
        compatible = "snps,dw-apb-uart";
        reg = <0x02500c00 0x400>;
        status = "disabled";
    };
};
```

逐行：

- `/` 是根节点。
- `model` 是人看的名字。
- `compatible` 是“匹配字符串”，驱动靠它认领设备。
- `chosen` 是启动参数。
- `memory@40000000` 描述内存起始和大小。
- `uart3:` 是标签（label），后面可以用 `&uart3` 引用。
- `serial@2500c00` 是节点名，`@` 后面是单元地址。
- `reg = <地址 长度>`。
- `status = "disabled"` 表示“硬件存在，但先别启用”。

---

## 9.3 节点与属性：语法就这几条

### 节点

```dts
节点名@单元地址 {
    属性 = 值;
    子节点 { ... };
};
```

- 节点名用 `-` 和 `_`，不要用大写。
- `@` 后面的地址要和 `reg` 第一项一致（约定）。
- 标签 `label:` 可选，用于引用。

### 属性类型

| 写法 | 类型 | 例子 |
|---|---|---|
| `= "text"` | 字符串 | `status = "okay"` |
| `= <0x1234>` | 32 位整数 | `reg = <0x02500c00 0x400>` |
| `= <&label>` | phandle | `clocks = <&ccu 5>` |
| `= [00 11 22]` | 字节数组 | `mac-address = [22 22 7a f2 e8 95]` |
| 只有名字 | 布尔 | `no-map;` |

### `status`

| 值 | 含义 |
|---|---|
| `"okay"` | 启用，驱动会 probe |
| `"disabled"` | 存在但不启用 |
| `"reserved"` | 保留，不给普通驱动 |

这块板的 UART3 在 Linux 侧是 `disabled`，因为它归 E907 用。
Linux 不碰它，E907 通过自己的设备树/寄存器配置使用。

### `compatible`

最重要的属性。格式通常是：

```text
"厂商,型号"
```

可以写多个，从具体到通用：

```dts
compatible = "yuzuki,lizard", "allwinner,sun8iw21p1";
```

内核按顺序找匹配的驱动。

### `reg`

```dts
reg = <0x02500c00 0x400>;
```

意思是“从 `0x02500c00` 开始，长 `0x400` 字节”。
驱动用 `platform_get_resource()` 拿到它，再 `ioremap`。

### 中断

```dts
interrupts = <GIC_SPI 32 IRQ_TYPE_LEVEL_HIGH>;
```

具体格式取决于中断控制器。ARM 常用 GIC，RISC-V 用 PLIC/CLIC。

### 时钟与复位

```dts
clocks = <&ccu CLK_UART3>;
resets = <&ccu RST_UART3>;
```

驱动 probe 时会让时钟子系统开时钟、释放复位。

### pinctrl

```dts
pinctrl-names = "default";
pinctrl-0 = <&uart3_pins>;
```

`uart3_pins` 是另一段节点，描述“把哪两个脚切成什么功能”。
Linux 的 pinctrl 子系统会在 probe 前应用它。

> 这块板的 UART3 引脚却是由 A7 的 `uart3pin.ko` 手工写的。
> 为什么不用 pinctrl？因为 Linux 侧把 `&uart3` 关了，**没有驱动
> 认领的设备，pinctrl 不会自动生效**。E907 的 `board_early.c` 里
> 也写了一遍同样的 mux，但实测写不进去（见 6.4），真正生效的
> 只有 A7 侧。

---

## 9.4 这块板的关键节点

### 保留内存

```dts
reserved-memory {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges;

    e907_dram: e907_dram@43c00000 {
        reg = <0x43c00000 0x00400000>;   /* 4 MiB */
        no-map;
    };

    vdev0buffer: vdev0buffer@43000000 {
        reg = <0x43000000 0x00040000>;   /* 256 KiB */
        no-map;
    };

    vdev0vring0: vdev0vring0@43040000 {
        reg = <0x43040000 0x00008000>;
        no-map;
    };

    vdev0vring1: vdev0vring1@43060000 {
        reg = <0x43060000 0x00008000>;
        no-map;
    };
};
```

- `#address-cells` / `#size-cells` 决定子节点 `reg` 用几个 32 位表示地址和长度。
- `no-map`：不要建立线性映射，不要当普通内存。
- 这些区域对应第 6 章的内存地图。

### remoteproc 节点

```dts
e907_rproc: e907_rproc@0 {
    compatible = "allwinner,sunxi-rproc";
    firmware-name = "zephyr.elf";
    memory-mappings = <0x43c00000 0x00400000 0x43c00000>;
    mboxes = <&msgbox 0>, <&msgbox 1>;
    status = "okay";
};
```

- `firmware-name`：remoteproc 去 `/lib/firmware/` 找哪个文件。
- `memory-mappings`：设备地址、大小、CPU 地址。
- `mboxes`：用哪两个 mailbox 通道（双向门铃）。
- **没有 `iommus`**：这块 SoC 没有 IOMMU，写了会启动失败。

### mailbox

```dts
msgbox: msgbox@3003000 {
    compatible = "allwinner,sunxi-msgbox";
    reg = <0x03003000 0x1000>;
    interrupts = <...>;
    #mbox-cells = <1>;
    clocks = <&ccu CLK_MSGBOX>;
    resets = <&ccu RST_MSGBOX>;
};
```

`#mbox-cells = <1>` 表示引用这个控制器时要传 1 个参数
（通道号），所以 `mboxes = <&msgbox 0>`。

### E907 侧的设备树

Zephyr 也有设备树，描述 E907 看到的硬件：

```dts
/ {
    soc {
        uart3: serial@2500c00 {
            compatible = "snps,dw-apb-uart";
            reg = <0x02500c00 0x400>;
            current-speed = <115200>;
            status = "okay";
        };
    };
};
```

固件里：

```c
#define UART_NODE DT_NODELABEL(uart3)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);
```

- `DT_NODELABEL(uart3)`：按标签找节点。
- `DEVICE_DT_GET()`：拿到设备对象指针。
- 编译时生成，不是运行时字符串查找。

> Zephyr 的设备树和 Linux 的设备树语法同源，但**用途不同**：
> Linux 用它在运行时匹配驱动；Zephyr 在编译时生成头文件，
> 把节点信息变成宏。

---

## 9.5 内核怎么用设备树找到驱动

### 三步

```text
1. 内核解析 DTB，建立设备树
2. 对每个 status="okay" 的节点，拿 compatible 去匹配驱动
3. 匹配成功 → 调用驱动的 probe()
```

### 驱动侧

```c
static const struct of_device_id my_of_match[] = {
    { .compatible = "yuzuki,my-device" },
    { }
};
MODULE_DEVICE_TABLE(of, my_of_match);

static int my_probe(struct platform_device *pdev)
{
    struct resource *res;
    void __iomem *base;
    int irq;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(base))
        return PTR_ERR(base);

    irq = platform_get_irq(pdev, 0);
    if (irq < 0)
        return irq;

    /* 用 base 读写寄存器，注册中断 */
    return 0;
}

static struct platform_driver my_driver = {
    .probe = my_probe,
    .driver = {
        .name = "my-device",
        .of_match_table = my_of_match,
    },
};
module_platform_driver(my_driver);
```

### `probe` 的常见错误

| 错误 | 含义 | 怎么办 |
|---|---|---|
| `-ENODEV` | 没找到硬件/时钟/ regulator | 检查 DTS、供电 |
| `-EPROBE_DEFER` | 依赖的驱动还没准备好 | 内核会自动重试，正常 |
| `-EINVAL` | 参数/地址不合法 | 检查 `reg`、`interrupts` |
| `-EBUSY` | 资源被占用 | 检查是否重复映射/申请 |
| `-ENOMEM` | 内存不足 | 检查大小、泄漏 |

> `-EPROBE_DEFER` 不是错误，是“稍后再试”。看到它不要慌，
> 看最后有没有 probe 成功。

### 查看设备树

运行时：

```sh
ls /proc/device-tree
cat /proc/device-tree/model
cat /proc/device-tree/compatible | tr '\0' '\n'
```

把运行时树转回源码：

```sh
dtc -I fs -O dts /proc/device-tree | less
```

找某个节点：

```sh
find /proc/device-tree -name '*e907*' -o -name '*uart3*'
```

---

## 9.6 改设备树的完整流程

### 步骤

```text
1. 找到板级 .dts（不是 SoC .dtsi）
2. 加/改节点
3. 编译 dtb
4. 和内核一起打包进 boot 分区
5. 启动，看 /proc/device-tree 和 dmesg
6. 失败就回滚
```

### 加一个 I2C 传感器（示例）

```dts
&i2c1 {
    status = "okay";
    clock-frequency = <100000>;

    sensor@48 {
        compatible = "ti,tmp102";
        reg = <0x48>;
    };
};
```

- `&i2c1` 引用 SoC 里已有的 I2C1 控制器。
- `status = "okay"` 打开控制器。
- `sensor@48` 是挂在总线上的设备，`reg = <0x48>` 是 I2C 地址。

### 编译

```sh
# 在内核源码树里
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- dtbs
```

或者用 SDK 的打包流程。产物是 `.dtb`，要和内核一起放进 boot 镜像。

### 验证

```sh
dmesg | grep -i tmp102
ls /sys/bus/i2c/devices/
```

### 常见失败

| 现象 | 原因 |
|---|---|
| 驱动没 probe | `compatible` 拼错、`status` 不是 okay |
| I2C 读失败 | 地址错、总线没打开、上拉电阻 |
| 内核起不来 | DTB 语法错、内存/保留区冲突 |
| 设备树没更新 | 刷了内核没刷 dtb，或反之 |

---

## 9.7 Zephyr 设备树：编译期生成

### 目录结构（概念）

```text
zephyr/
  boards/<vendor>/<board>/
    <board>.dts
    <board>_defconfig
    Kconfig.board
  dts/
    <soc>.dtsi
```

应用可以放 `app.overlay` 覆盖板级配置：

```dts
&uart3 {
    current-speed = <115200>;
    status = "okay";
};
```

### 生成的宏

编译后，Zephyr 会把设备树变成头文件：

```c
#define DT_NODELABEL_uart3 ...
#define DT_REG_ADDR(DT_NODELABEL(uart3)) 0x02500c00
#define DT_PROP(DT_NODELABEL(uart3), current_speed) 115200
```

常用 API：

| 宏 | 作用 |
|---|---|
| `DT_NODELABEL(label)` | 按标签取节点 |
| `DT_ALIAS(alias)` | 按 alias 取节点 |
| `DT_PROP(node, prop)` | 取属性 |
| `DT_REG_ADDR(node)` | 取 reg 起始地址 |
| `DEVICE_DT_GET(node)` | 取设备对象 |
| `DEVICE_DT_GET_ANY(node)` | 取设备，可能为空 |

### 构建时看最终设备树

```sh
ninja -C build
ls build/zephyr/zephyr.dts
```

`zephyr.dts` 是所有 include/overlay 合并后的最终结果。
**调试设备树问题时，先看这个文件，不要猜。**

### 常见错误

| 现象 | 原因 |
|---|---|
| `undefined reference to __device_dts_ord_N` | 节点没启用/驱动没编进来 |
| `DT_NODELABEL` 编译失败 | 标签拼错、overlay 没生效 |
| 设备 `not ready` | `status` 不是 okay、时钟/引脚没配 |
| 地址不对 | overlay 覆盖顺序错、单位地址不匹配 |

---

## 9.8 实验：读设备树

### 实验 A：Linux 侧

```sh
cat /proc/device-tree/model
echo
cat /proc/device-tree/compatible | tr '\0' '\n'

find /proc/device-tree -maxdepth 2 -name '*memory*' -o -name '*rproc*'
```

### 实验 B：找保留内存

```sh
find /proc/device-tree/reserved-memory -maxdepth 1 -type d
for d in /proc/device-tree/reserved-memory/*; do
    echo "$d"
    hexdump -C "$d/reg" 2>/dev/null
done
```

你会看到 4 段保留区，对应第 6 章的地址表。

### 实验 C：Zephyr 侧

```sh
# 在固件构建目录
grep -n 'uart3' build/zephyr/zephyr.dts
grep -n 'DT_NODELABEL_uart3' build/zephyr/include/generated/devicetree_generated.h | head
```

### 实验 D：改一个属性

把 `prj.conf` 里的某个配置改掉，重新构建，观察
`build/zephyr/.config` 和 `zephyr.dts` 的变化。

> 设备树和 Kconfig 是两套东西：设备树描述“硬件长什么样”，
> Kconfig 描述“编哪些功能进来”。下一章会讲 Kconfig。

---

## 9.9 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 驱动不 probe | `compatible` 不匹配 | `dmesg`、`/sys/bus/*/drivers` |
| probe 一直 defer | 依赖驱动没就绪 | 看依赖的时钟/ regulator |
| 地址访问 fault | `reg` 错、没 `ioremap` | 对照手册、看 `/proc/iomem` |
| 中断不来 | `interrupts` 错、没使能 | `/proc/interrupts` |
| 保留内存被覆盖 | 没写 `reserved-memory` | `dmesg`、内存地图 |
| IOMMU 报错 | 多了 `iommus` | 删除属性 |
| Zephyr 编译报节点未定义 | overlay 没生效 | 看 `zephyr.dts` |
| 设备 not ready | `status`/时钟/引脚 | `device_is_ready()` 前后日志 |

---

## 9.10 练习

### 练习 9.1（必做）

在板子上：

```sh
dtc -I fs -O dts /proc/device-tree > /tmp/live.dts
grep -n -A8 'reserved-memory' /tmp/live.dts
grep -n -A10 'e907' /tmp/live.dts
```

回答：

1. E907 的 `reg` 是多少？
2. 有几个 vring？
3. `firmware-name` 是什么？

### 练习 9.2（必做）

给下面这个节点挑错：

```dts
uart3: serial@2500c00 {
    compatible = "snps,dw-apb-uart"
    reg = <0x02500c00 0x400>
    status = "okay";
};
```

提示：至少三处。

### 练习 9.3（必做）

解释为什么这块板的 UART3 在 Linux 侧是 `disabled`，
而 E907 仍然能用它。涉及哪几个组件？

### 练习 9.4（选做）

在自己的 Zephyr 应用里加一个 `app.overlay`，把
`uart3` 的 `current-speed` 改成 9600，重新构建，
用 `zephyr.dts` 验证。**不要烧到主力板**，除非你确认
改回来很容易。

---

## 9.11 本章验收

1. 能解释 `.dts`、`.dtsi`、`.dtb` 的关系。
2. 能读懂节点、`compatible`、`reg`、`status`、`no-map`。
3. 能说出内核从 `compatible` 到 `probe` 的匹配过程。
4. 能在这块板上找到 `reserved-memory` 和 `e907_rproc`。
5. 知道 Zephyr 设备树是编译期生成宏，会用 `DT_NODELABEL`。
6. 能独立完成“加一个 I2C 节点 → 编译 → 验证”的流程（可在虚拟环境）。

### 自测题

1. 设备树是驱动还是数据？为什么？
2. `status = "disabled"` 和“节点不存在”有什么区别？
3. `reg = <0x02500c00 0x400>` 的两个数分别是什么？
4. 为什么 `e907_rproc` 不能写 `iommus`？
5. `-EPROBE_DEFER` 是错误吗？
6. Zephyr 的 `DT_NODELABEL` 是运行时查找还是编译期生成？

> 下一章讲构建：交叉编译、CMake、Kconfig。
> 你会亲手把一份源码变成能在板子上跑的程序。
