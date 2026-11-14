# 第 15 章 Zephyr 移植：board、DTS、SoC、时钟与引脚

> 本章目标：
> 1. 知道“移植 Zephyr 到一块新板”到底要写哪些文件。
> 2. 看懂 board 目录的五个文件：`.dts`、`_defconfig`、`Kconfig.*`、`.yaml`、`board.yml`。
> 3. 理解 SoC `.dtsi` 和 board `.dts` 的分工。
> 4. 知道这块板的 UART3 时钟和引脚为什么要在 `board_early.c` 里手工配。
> 5. 会用 `app.overlay` 覆盖板级配置，用 `DEVICE_DT_GET` 拿设备。

---

## 15.1 移植 = 描述硬件 + 选编译开关

“把 Zephyr 移植到一个新 SoC/新板”听起来很吓人，其实只做两件事：

```text
1. 描述硬件：地址、中断、时钟、引脚、内存  → 设备树
2. 选编译开关：内核要哪些功能、驱动要哪些  → Kconfig
```

如果 SoC 的 CPU 架构已经有 Zephyr 支持（这里是 RISC-V），
你不需要写调度器、写中断入口、写上下文切换。你要写的是
**板级胶水**。

### 这块板需要的胶水

| 胶水 | 文件 | 为什么需要 |
|---|---|---|
| CPU 描述 | `dts/riscv/allwinner/v851s.dtsi` | CLINT/CLIC、频率、ISA 扩展 |
| UART3 节点 | 同上 | 地址、中断、时钟频率 |
| MSGBOX 节点 | 同上 | 双向门铃、CCU 门控 |
| DRAM 窗口 | 同上 | 固件加载到 `0x43c00000` |
| 板级选择 | `boards/yuzuki/v851s_e907/*` | chosen 节点、IPM、共享内存 |
| 引脚 + 时钟 | `src/board_early.c` | Linux 不管 UART3，必须自己开 |
| 构建开关 | `v851s_e907_defconfig` + `prj.conf` | RAM console、XIP 关、驱动选择 |

> 注意最后一行：**引脚和时钟没写进设备树**。这是这块板的
> 特殊情况，后面 15.6 会解释。

---

## 15.2 Zephyr 的 board 目录

一块板的定义在：

```text
zephyr/boards/<vendor>/<board>/
```

这块板是：

```text
zephyr/boards/yuzuki/v851s_e907/
  v851s_e907.dts
  v851s_e907_defconfig
  v851s_e907.yaml
  Kconfig.v851s_e907
  board.yml
  bringup_uart_console.overlay
  doc/index.rst
```

### 五个文件各管什么

| 文件 | 作用 | 类比 |
|---|---|---|
| `board.yml` | 板名、厂商、SoC 名 | 身份证 |
| `v851s_e907.yaml` | 架构、RAM、支持的外设、测试标签 | 能力清单 |
| `Kconfig.v851s_e907` | `select SOC_V851S_E907` | 选 SoC |
| `v851s_e907_defconfig` | 默认 Kconfig 开关 | 出厂设置 |
| `v851s_e907.dts` | 板级设备树 | 接线图 |

### `board.yml`

```yaml
board:
  name: v851s_e907
  full_name: YuzukiPI-Lizard V851S E907 co-processor
  vendor: yuzuki
  socs:
    - name: v851s_e907
```

Zephyr 用它在 `-DBOARD=v851s_e907` 时找到板目录和 SoC。

### `v851s_e907.yaml`

```yaml
identifier: v851s_e907
name: YuzukiPI-Lizard V851S E907
vendor: yuzuki
type: mcu
arch: riscv
ram: 4096
toolchain:
  - zephyr
testing:
  ignore_tags:
    - net
    - bluetooth
supported:
  - uart
  - mbox
```

- `arch: riscv`：决定用哪套工具链和启动代码。
- `ram: 4096`：4 MiB，给链接脚本用。
- `supported`：声明这块板支持哪些外设，测试系统会用。

### `Kconfig.v851s_e907`

```conf
config BOARD_V851S_E907
	select SOC_V851S_E907
```

板上选 `BOARD_V851S_E907`，就自动选中 SoC 的配置。
SoC 配置再去选 CPU 架构、CLIC、驱动依赖。

### `v851s_e907_defconfig`

```conf
CONFIG_CONSOLE=y
CONFIG_SERIAL=y
CONFIG_UART_NS16550=y

CONFIG_RAM_CONSOLE=y
CONFIG_UART_CONSOLE=n

CONFIG_XIP=n

CONFIG_GEN_ISR_TABLES=y
CONFIG_GEN_SW_ISR_TABLE=y
```

逐条：

- `CONFIG_SERIAL` + `CONFIG_UART_NS16550`：编串口驱动。
- `CONFIG_RAM_CONSOLE=y`：日志进内存。
- `CONFIG_UART_CONSOLE=n`：日志**不**进 UART3。
- `CONFIG_XIP=n`：代码直接从 RAM 执行，不从 flash 取指。
- `CONFIG_GEN_ISR_TABLES` / `GEN_SW_ISR_TABLE`：生成中断向量表。

> defconfig 是**板级默认**。应用可以在 `prj.conf` 里覆盖。
> 改板级默认影响所有用这块板的应用；改 `prj.conf` 只影响
> 当前应用。**能不改 defconfig 就不改。**

---

## 15.3 设备树：SoC 级 vs 板级

### SoC `.dtsi`：芯片里有什么

`dts/riscv/allwinner/v851s.dtsi` 描述 E907 看到的硬件：

```dts
/ {
    cpus {
        timebase-frequency = <DT_FREQ_M(24)>;

        cpu0: cpu@0 {
            compatible = "xuantie,e907", "riscv";
            clock-frequency = <DT_FREQ_M(600)>;
            riscv,isa-base = "rv32i";
            riscv,isa-extensions = "i", "m", "a", "c", "zicsr",
                                   "zifencei", "xtheadba", "xtheadbb",
                                   "xtheadbs", "xtheadcmo";

            clic: clic@30800000 {
                compatible = "nuclei,eclic";
                reg = <0x30800000 0x5000>;
                interrupt-controller;
            };
        };
    };

    soc {
        uart3: serial@2500c00 {
            compatible = "ns16550";
            reg = <0x02500c00 0x400>;
            reg-shift = <2>;
            clock-frequency = <DT_FREQ_M(24)>;
            interrupt-parent = <&clic>;
            interrupts = <21 1>;
            status = "disabled";
        };

        msgbox: msgbox@6020000 {
            compatible = "allwinner,sunxi-msgbox-riscv";
            reg = <0x06020000 0x1000>, <0x03003000 0x1000>;
            reg-names = "rx", "tx";
            interrupts-extended = <&clic 144 1>;
            #mbox-cells = <1>;
            status = "okay";
        };

        dram0: memory@43c00000 {
            compatible = "zephyr,memory-region", "mmio-sram";
            reg = <0x43c00000 DT_SIZE_M(4)>;
            zephyr,memory-region = "DRAM0";
        };
    };
};
```

关键点：

1. **`uart3` 默认 `status = "disabled"`**，由板级 `.dts` 决定是否开。
2. **`interrupts = <21 1>`**：CLIC ID。注释里写了
   “GIC 37 → CLIC ID = GIC − 16 = 21”。
3. **`msgbox` 有两个 `reg`**：`rx` 是 E907 本地块，`tx` 是 CPUX 块。
4. **`dram0` 是 4 MiB**，起始 `0x43c00000`，和 Linux 侧
   `reserved-memory` 完全一致。
5. **没有 PLIC**：E907 用的是 CLINT + CLIC，不是 RISC-V 常见的 PLIC。
   这一点写错，中断永远不来。

### 板级 `.dts`：这块板怎么用

`boards/yuzuki/v851s_e907/v851s_e907.dts`：

```dts
#include <allwinner/v851s.dtsi>

/ {
    model = "YuzukiPI-Lizard V851S E907";

    chosen {
        zephyr,sram = &dram0;
        zephyr,ipc = &ipm0;
        zephyr,ipc_shm = &dram_shm;
    };

    ipm0: ipm-mbox {
        compatible = "zephyr,mbox-ipm";
        mboxes = <&msgbox 0>, <&msgbox 0>;
        mbox-names = "tx", "rx";
    };

    dram_shm: memory@43000000 {
        compatible = "mmio-sram";
        reg = <0x43000000 DT_SIZE_K(512)>;
    };
};

&uart3 {
    current-speed = <115200>;
    status = "okay";
};
```

逐段：

- `chosen`：
  - `zephyr,sram = &dram0`：Zephyr 的 RAM 从 `dram0` 来。
  - `zephyr,ipc = &ipm0`：IPM 设备是 `ipm0`。
  - `zephyr,ipc_shm = &dram_shm`：OpenAMP 的共享内存窗口。
- `ipm0`：在 MSGBOX 上包一层 IPM，收发都用通道 0。
- `dram_shm`：512 KiB，覆盖 buffer(256K) + vring0(128K) + vring1(128K)。
- `&uart3`：开 UART3，设 115200。

### `chosen` 是干什么的

`chosen` 不是硬件，是**“给系统的指定”**：

| chosen 属性 | 含义 |
|---|---|
| `zephyr,sram` | 主 RAM |
| `zephyr,console` | 控制台输出到哪 |
| `zephyr,shell-uart` | shell 用哪个串口 |
| `zephyr,ipc` | IPM 设备 |
| `zephyr,ipc_shm` | 共享内存区域 |

代码里这样取：

```c
#define SHM_NODE DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE DT_REG_SIZE(SHM_NODE)

static const struct device *const ipm_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));
```

> `chosen` 把“具体是哪个节点”和“代码里怎么用”解耦。
> 换板时只改 chosen，驱动代码不用动。

---

## 15.4 `app.overlay`：应用覆盖板级配置

板级 `.dts` 是“所有应用共用”。某个应用想改，就在应用目录放
`app.overlay`：

```dts
/* app.overlay */
&uart3 {
    current-speed = <9600>;
};

/ {
    chosen {
        zephyr,console = &uart3;
    };
};
```

构建时 Zephyr 自动合并：SoC `.dtsi` → board `.dts` → `app.overlay`。

### 合并顺序与覆盖规则

```text
1. SoC .dtsi
2. board .dts（include dtsi）
3. app.overlay
4. 命令行 -DEXTRA_DTC_OVERLAY_FILE=...
```

后面的覆盖前面的同名属性。节点用 `&label` 引用，新增节点
直接写在 `/ { }` 或 `&parent { }` 里。

### 构建后看最终结果

```sh
ninja -C build
less build/zephyr/zephyr.dts
```

`zephyr.dts` 是合并后的**最终**设备树。**调试设备树先看它，
不要猜 include 顺序。**

### `bringup_uart_console.overlay`

仓库里还有一个只用于 bring-up 的覆盖文件：

```dts
/ {
    chosen {
        zephyr,console = &uart3;
        zephyr,shell-uart = &uart3;
    };
};
```

用法：

```sh
west build -b v851s_e907 -s samples/hello_world -d build-bringup \
  -- -DEXTRA_DTC_OVERLAY_FILE=bringup_uart_console.overlay \
     -DCONFIG_RAM_CONSOLE=n -DCONFIG_UART_CONSOLE=y
```

它把控制台临时切到 UART3，方便在 OpenAMP 还没通时看启动日志。

> **正式桥接固件绝对不能用这个 overlay。** UART3 是目标板数据口，
> 控制台和桥接抢同一个串口，数据会被日志污染。

---

## 15.5 时钟与引脚：这块板为什么特殊

### 正常流程

一般来说：

1. 设备树写 `pinctrl-0 = <&uart3_pins>`。
2. 设备树写 `clocks = <&ccu CLK_UART3>`。
3. 驱动 probe 时，pinctrl 子系统配引脚，clock 子系统开时钟。

### 这块板的现实

- Linux 侧 `&uart3` 是 `disabled`，没有驱动认领，**pinctrl 不会生效**。
- A7 内核没开 `CONFIG_DEVMEM`，用户态不能直接写寄存器。
- E907 的 Zephyr 还没有完整的 CCU/pinctrl 驱动。

所以只能由 E907 固件在启动早期**手工写寄存器**：

```c
/* src/board_early.c */

#define V851S_PIO_CFG_PE12_PE13   0x020000C4u
#define V851S_CCU_UART_GATE       0x0200190Cu

#define PE12_PE13_MUX7            0x00770000u
#define UART3_GATE_EN             (1u << 19)
#define UART3_RESET_DEASSERT      (1u << 3)

static int v851s_e907_uart3_early_init(void)
{
    uint32_t v;

    /* 1. PE12/PE13 → mux 7（UART3） */
    v = sys_read32(V851S_PIO_CFG_PE12_PE13);
    v = (v & ~PE12_PE13_MUX7) | PE12_PE13_MUX7;
    sys_write32(v, V851S_PIO_CFG_PE12_PE13);

    /* 2. 开 UART3 时钟门、解复位 */
    v = sys_read32(V851S_CCU_UART_GATE);
    v |= (UART3_GATE_EN | UART3_RESET_DEASSERT);
    sys_write32(v, V851S_CCU_UART_GATE);

    return 0;
}

SYS_INIT(v851s_e907_uart3_early_init, PRE_KERNEL_1, 0);
```

### 寄存器怎么算出来的

**引脚**：

- PIO 基址 `0x02000000`。
- 每个 bank 间隔 `0x30`。
- PE 是第 4 个 bank（引脚 128 起）→ 偏移 `4 × 0x30 = 0xC0`。
- 引脚 8..15 在 `CFG1`，即 `0xC0 + 0x04 = 0xC4`。
- PE12 → bits 16..19；PE13 → bits 20..23。
- mux 7 → 每 4 位写 `0111`。
- 合起来：`0x00770000`。

**时钟**：

- CCU 基址 `0x02001000`。
- `UART_GATE` 偏移 `0x90C` → `0x0200190C`。
- bit 19 = UART3 时钟使能。
- bit 3 = UART3 复位，**低有效**，写 1 表示“解复位”。

> 这些数字不是猜的。注释里写着来源：厂商 Linux 4.9 内核的
> `pinctrl-sun8iw21p1.c`、`clk-sun8iw21.c`。**移植时永远
> 先找厂商内核/手册，别凭感觉写。**

### 为什么用 `PRE_KERNEL_1, 0`

- 必须在 ns16550 驱动初始化前完成。
- 串口驱动在 `POST_KERNEL`（`CONFIG_SERIAL_INIT_PRIORITY` 50+）。
- `PRE_KERNEL_1` 比它早，`0` 是该级别里最早的一批。

顺序错 = 驱动读 UART 寄存器时时钟没开 = 读到 0 或总线错误。

### 这种写法的问题

它是**板级胶水**，不是通用驱动：

- 只对 V851S 这块 SoC 有效。
- 引脚写死 PE12/PE13。
- 时钟门写死 bit 19。

正确的长期方案是写 CCU 和 pinctrl 驱动，然后设备树里声明。
但在 bring-up 阶段，**先让它跑起来**比“架构漂亮”更重要。
代码注释里也明确写了：“board-specific bring-up glue, not a
general driver.”

> 工程判断：什么时候写“脏但有效”的胶水，什么时候写通用驱动？
> 我的标准是——**如果只有一个用户、一个板子、一个引脚组合，
> 胶水可以接受，但必须注释清楚来源和局限。**

---

## 15.6 中断：CLIC 不是 PLIC

### RISC-V 常见的两种中断控制器

| 控制器 | 特点 | Zephyr 支持 |
|---|---|---|
| PLIC | 平台级中断控制器，标准 | `riscv,plic0` |
| CLIC | 核心级中断控制器，支持向量/嵌套 | `nuclei,eclic` |

E907 用的是 **CLINT + CLIC**，不是 PLIC。

### 设备树里怎么体现

```dts
clic: clic@30800000 {
    compatible = "nuclei,eclic";
    reg = <0x30800000 0x5000>;
    #interrupt-cells = <2>;
    interrupt-controller;
};
```

设备引用：

```dts
uart3 {
    interrupt-parent = <&clic>;
    interrupts = <21 1>;       /* CLIC ID 21，触发类型 1 */
};

msgbox {
    interrupts-extended = <&clic 144 1>;
};
```

### ID 怎么换算

这块板的约定：

```text
CLIC ID = GIC ID − 16
```

- UART3 是 GIC SPI 5 → GIC 37 → CLIC 21。
- MSGBOX 是 GIC 160 → CLIC 144。

写错 ID 的现象：

```text
- 设备 probe 成功
- 但永远收不到中断
- 轮询能工作，中断不工作
```

> 这也是为什么 `linkr_bridge.c` 的 RX 线程**同时**等信号量和
> 20 ms 轮询。CLIC 路由没验证时，轮询是保底手段，只损失延迟，
> 不会整个桥接失效。

### `interrupts` vs `interrupts-extended`

- `interrupts`：配合 `interrupt-parent` 使用。
- `interrupts-extended`：每个中断自己带控制器引用，
  适合多个控制器。

ns16550 的 Zephyr Kconfig 只在节点有 `interrupts` 属性时才开
`SERIAL_SUPPORT_INTERRUPT`。所以 UART3 用 `interrupts`，
msgbox 用 `interrupts-extended`。这不是风格问题，是驱动要求。

---

## 15.7 `DEVICE_DT_GET`：编译期拿设备

### 三种获取设备的方式

```c
/* 1. 编译期，节点必须存在且 status=okay */
static const struct device *const uart_dev =
    DEVICE_DT_GET(DT_NODELABEL(uart3));

/* 2. 编译期，可能为空 */
static const struct device *const dev =
    DEVICE_DT_GET_ANY(DT_NODELABEL(uart3));

/* 3. 运行时按名字查（慢，不推荐） */
const struct device *d = device_get_binding("uart3");
```

`DEVICE_DT_GET` 会生成对 `__device_dts_ord_N` 的引用。如果节点
没启用或驱动没编进来，**链接就失败**，而不是运行时才发现。

```text
undefined reference to `__device_dts_ord_42'
```

这个错误的意思就是：“设备树里有节点，但没有对应的设备对象。”

### 使用前检查

```c
if (!device_is_ready(uart_dev)) {
    LOG_ERR("UART3 not ready");
    return -ENODEV;
}
```

`device_is_ready` 检查驱动是否初始化成功。**即使
`DEVICE_DT_GET` 编译通过，运行时也可能没就绪**（比如时钟没开、
依赖没满足）。

### 这块板的实际代码

```c
/* linkr_bridge.c */
#define UART_NODE DT_NODELABEL(uart3)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);

/* openamp_slave.c */
static const struct device *const ipm_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));
```

- UART3 用标签 `uart3`。
- IPM 用 `chosen` 里的 `zephyr,ipc`，不写死节点名。

> 用 `chosen` 是更好的做法：换板/换 IPM 实现时，只改设备树，
> 代码不变。

---

## 15.8 移植一块新板的清单

假设你要把 Zephyr 跑到另一颗 RISC-V 核上：

### 第一步：确认架构支持

- Zephyr 是否已支持这个 CPU 架构/ISA？
- 中断控制器是 PLIC、CLIC 还是自定义？
- 有没有现成的 SoC 目录可以抄？

### 第二步：写 SoC `.dtsi`

```text
- cpu 节点：compatible、ISA、频率
- 中断控制器：地址、cell 数
- 定时器：mtime/mtimecmp
- 串口、mailbox 等外设：地址、中断
- 内存窗口
```

### 第三步：写 board 目录

```text
- board.yml：板名、SoC
- <board>.yaml：arch、ram、supported
- Kconfig.<board>：select SOC
- <board>_defconfig：console、XIP、驱动
- <board>.dts：chosen、板级外设、enable 节点
```

### 第四步：处理时钟和引脚

- 有 pinctrl/clock 驱动 → 设备树声明。
- 没有 → 写 `SYS_INIT` 胶水，注释来源。

### 第五步：启动测试

```sh
west build -b <board> -s samples/hello_world
```

- 先让 `printk` 出来。
- 再验证中断（定时器/串口）。
- 再验证 mailbox/IPM。
- 最后接 OpenAMP/rpmsg。

### 第六步：记录踩过的坑

```text
- 哪个中断 ID 是错的
- 哪个时钟门没开
- 哪个寄存器低有效
- 哪个地址和 Linux 不一致
```

> 移植文档的价值，80% 在“坑列表”，不在“步骤列表”。

---

## 15.9 实验：改设备树并验证

### 实验 A：看最终设备树

```sh
cd /path/to/app
export ZEPHYR_BASE=/path/to/zephyr
cmake -B build -GNinja -DBOARD=v851s_e907 .
ninja -C build

grep -n -A10 'uart3' build/zephyr/zephyr.dts
grep -n 'zephyr,ipc' build/zephyr/zephyr.dts
grep -n 'dram_shm' build/zephyr/zephyr.dts
```

确认：

1. `uart3` 是 `status = "okay"`。
2. `current-speed = <115200>`。
3. `chosen` 里有 `zephyr,ipc` / `zephyr,ipc_shm`。

### 实验 B：加一个 `app.overlay`

```dts
/* app.overlay */
&uart3 {
    current-speed = <9600>;
};
```

重新构建，`zephyr.dts` 里应该变成 9600。

**不要烧到主力板**，除非你确认改回来很容易。这个实验只看
生成的设备树即可。

### 实验 C：看编译期宏

```sh
grep -rn 'DT_NODELABEL_uart3' build/zephyr/include/generated/ \
    | head
grep -rn 'DT_REG_ADDR' build/zephyr/include/generated/ \
    | grep -i uart | head
```

### 实验 D：制造一个链接错误

把某处的 `DT_NODELABEL(uart3)` 改成不存在的标签：

```c
static const struct device *const uart_dev =
    DEVICE_DT_GET(DT_NODELABEL(uart9));
```

构建，观察错误：

```text
undefined reference to `__device_dts_ord_...'
```

然后改回来。**记住这个错误长什么样**——以后看到它，
先想“设备树节点/驱动没编进来”。

---

## 15.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 找不到板 | `board.yml` 名字/路径错 | `west boards` |
| 编译报 `__device_dts_ord` | 节点没 okay/驱动没编 | `zephyr.dts`、`.config` |
| 串口无输出 | 时钟没开/引脚没 mux | `board_early.c`、示波器 |
| 中断不来 | CLIC ID 错、没 `interrupts` | 对照 GIC−16、`/proc/interrupts`（Linux 侧） |
| 设备 `not ready` | 驱动 probe 失败 | 启动日志、`CONFIG_LOG` |
| 固件加载后跑飞 | DRAM 窗口/入口地址错 | 对照 Linux `reserved-memory` |
| mailbox 无反应 | `reg`/中断/CCU 门控错 | SoC `.dtsi`、厂商手册 |
| `chosen` 取到空 | 属性名拼错 | `zephyr.dts` |
| overlay 没生效 | 文件名/路径错 | 构建日志、`zephyr.dts` |
| 应用和板级配置冲突 | 改错层 | 分清 defconfig/prj.conf/overlay |

---

## 15.11 练习

### 练习 15.1（必做）

打开 `build/zephyr/zephyr.dts`，找出：

1. `uart3` 的 `reg` 和 `interrupts`。
2. `msgbox` 的两个 `reg` 和 `reg-names`。
3. `dram0` 和 `dram_shm` 的地址和大小。
4. `chosen` 里的三个属性。

### 练习 15.2（必做）

回答：

1. 为什么 `uart3` 在 SoC `.dtsi` 里是 `disabled`，在板级 `.dts` 里才 `okay`？
2. 为什么这块板的引脚和时钟不写在设备树里？
3. `CLIC ID = GIC ID − 16` 这条规则如果写错，会有什么现象？
4. `DEVICE_DT_GET` 和 `device_get_binding` 的本质区别是什么？
5. `app.overlay` 和 `prj.conf` 分别改什么？

### 练习 15.3（必做）

阅读 `src/board_early.c`，手算：

- PE12 在哪个 bank、哪个 CFG 寄存器、哪几位？
- `0x00770000` 是怎么来的？
- 为什么复位位是“写 1 解复位”？

### 练习 15.4（选做）

给一个假想的 UART4（地址 `0x02500c00 + 0x400`）写设备树节点，
中断假设是 CLIC 22。写出 `.dtsi` 片段和板级 enable 片段。
不需要真的能用，只要语法正确、能用 `dtc` 编译。

---

## 15.12 本章验收

1. 能说出 board 目录五个文件各自的作用。
2. 能区分 SoC `.dtsi` 和板级 `.dts` 的职责。
3. 能解释 `chosen` 的作用，并用 `DT_CHOSEN` 取设备。
4. 知道这块板为什么用 `SYS_INIT` 手工配时钟和引脚。
5. 能解释 CLIC 和 PLIC 的区别，以及中断 ID 怎么换算。
6. 能用 `app.overlay` 覆盖配置，并用 `zephyr.dts` 验证。
7. 看到 `__device_dts_ord_N` 链接错误知道查哪里。

### 自测题

1. `board.yml`、`_defconfig`、`.dts` 分别是什么？
2. 为什么 `CONFIG_XIP=n`？
3. UART3 的 CLIC 中断号是多少？怎么算的？
4. `zephyr,ipc_shm` 指向哪个节点？多大？
5. 为什么正式固件不能把 console 设成 UART3？
6. `DEVICE_DT_GET` 失败是编译期还是运行时？

> 下一章把前面所有零件拼起来：从一个 hello world 到完整的 E907 固件。
