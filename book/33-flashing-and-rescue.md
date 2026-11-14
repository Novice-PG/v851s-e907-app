# 第 33 章 刷机与救砖：把风险控制在动手之前

> 本章目标：
> 1. 知道这块板有哪几条刷机路径，各自的前置条件和风险。
> 2. 理解“救援 initramfs”为什么是最稳的日常方案。
> 3. 会做刷机前备份、刷机后验收、失败时回滚。
> 4. 知道 FEL 模式能做什么、不能做什么。
> 5. 能按现象判断“是刷坏了、还是启动参数错了、还是硬件问题”。

---

## 33.1 三条路

```text
1. 整片刷（PhoenixSuit / FEL）
   写整个 NAND，包括 boot0/u-boot/env

2. 救援 initramfs
   在内存里跑一个小系统，往指定 UBI 卷写文件

3. 运行中替换文件
   只换 /usr/bin 下的程序或网页
```

| 路径 | 能救砖 | 速度 | 风险 | 日常用 |
|---|---|---|---|---|
| 整片 | 能 | 慢 | 高（动启动链） | 首次/救砖 |
| 救援 | 部分 | 中 | 低（只动目标卷） | ✅ |
| 换文件 | 不能 | 快 | 很低 | ✅ |

### 选择原则

```text
能用“换文件”解决的，不刷机
能“只刷 rootfs”的，不整片刷
必须整片刷的，先确认备份和供电
```

> 刷机不是“更彻底”，是“更危险”。**能少动就少动。**

---

## 33.2 刷机前必须做的事

### 一、确认供电

```text
- 用稳定电源，不要用快没电的充电宝
- 不要刷到一半拔线
- 串口线插稳
```

NAND 写一半掉电 = 该卷损坏，可能连启动都失败。

### 二、备份

至少要备份：

```text
/mnt/UDISK 下的：
  wpa_supplicant.conf      Wi-Fi 凭据
  persist/ws-token         LAN 令牌
  persist/certs/           HTTPS 证书私钥
  persist/bluetooth/       BLE 绑定
  persist/etc/passwd 等    账号
  .ssh/authorized_keys     SSH 公钥
```

```sh
mkdir -p /tmp/board-backup
scp -O -r root@<board>:/mnt/UDISK/persist /tmp/board-backup/
scp -O root@<board>:/mnt/UDISK/wpa_supplicant.conf /tmp/board-backup/
scp -O -r root@<board>:/root/.ssh /tmp/board-backup/
```

> 这些文件**不在 rootfs 里**，重刷 rootfs 不应该丢。
> 但“不应该”不等于“不会”。备份是保险。

### 三、备份启动卷

如果这次要动 boot 卷：

```sh
# 在板子上
dd if=/dev/ubi0_3 of=/mnt/UDISK/boot-prev-$(date +%Y%m%d).img bs=1M
sync
```

确保 UDISK 有足够空间（boot 卷约 4 MB）。

### 四、记录当前状态

```sh
cat /etc/rootfs_version
md5sum /usr/bin/linkr-bee-daemon /usr/bin/linkr-ble
cat /sys/class/remoteproc/remoteproc0/state
mount | grep -E 'UDISK|overlay|bluetooth'
df -h
```

> 刷完要对比。**没有“刷前记录”，就无法证明“刷对了”。**

---

## 33.3 救援 initramfs：日常首选

### 原理

```text
1. 把救援镜像写到 boot 卷（ubi0_3）
2. 重启，U-Boot 用临时 bootargs 启动救援镜像
3. 救援系统在内存里跑，带 ubi 工具
4. 从串口/网络把 rootfs 写进 ubi0_4
5. 把正常 kernel 写回 ubi0_3
6. 重启
```

关键：**救援系统在 RAM 里，不依赖 rootfs**。所以 rootfs 坏了也能刷。

### 完整步骤（这块板的实际流程）

#### 第一步：把救援镜像送进 boot 卷

在运行中的系统上：

```sh
python3 ubi_stream.py --src boot-rescue.img --vol /dev/ubi0_3
```

`ubi_stream.py` 做的是：

```text
1. 打开 /dev/ubi0_3
2. 分块写入
3. 读回校验
```

写入后可以 md5 读回对比。

#### 第二步：重启并打断 U-Boot

```sh
reboot
```

在串口终端（`/dev/ttyACM0`，115200）里，看到 U-Boot 倒计时时按任意键：

```text
Hit any key to stop autoboot: 3
```

#### 第三步：设置临时 bootargs

```text
setenv bootargs ubi.mtd=sys console=ttyS0,115200 loglevel=7
run boot_normal
```

逐项：

- `ubi.mtd=sys`：告诉内核从 `sys` 这个 MTD 分区挂 UBI。
- `console=ttyS0,115200`：控制台走串口。
- `loglevel=7`：打印详细日志（救援时要看）。
- `run boot_normal`：用 boot 卷里的镜像启动。

**不要 `saveenv`**。这些参数只在这次生效。

> 为什么不用默认 bootargs？因为默认可能指向 rootfs（已经坏了）。

#### 第四步：在救援 shell 里写 rootfs

救援系统起来后：

```sh
ubi_recv /dev/ubi0_4 < rootfs.squashfs
```

或者通过网络/串口把文件传进去再写。不同版本的救援镜像工具不同，
本质是：

```sh
# 概念
ubiupdatevol /dev/ubi0_4 /tmp/rootfs.squashfs
```

写入后：

```sh
# 读回校验
md5sum /dev/ubi0_4    # 或按块读取
```

#### 第五步：写回正常 kernel

```sh
ubi_recv /dev/ubi0_3 < boot-normal.img
```

#### 第六步：重启

```sh
sync
reboot -f
```

`-f` 强制重启，不再跑正常关机流程（救援系统不需要）。

### 为什么“先写救援，再写回正常 kernel”

因为 boot 卷只有一个：

```text
救援镜像占了 boot 卷
→ 刷完 rootfs 必须把正常 kernel 放回去
→ 否则下次启动还是救援系统
```

> 忘了第五步的现象：每次重启都进救援 shell。

---

## 33.4 整片刷：FEL 与 PhoenixSuit

### FEL 是什么

全志芯片的 BROM 里有一个 USB 恢复模式：

```text
上电时按住 FEL 键（或短接特定引脚）
→ BROM 不读 NAND，进入 USB FEL 模式
→ 主机用工具把 SPL/U-Boot/镜像推过去
```

优点：

```text
- 不依赖 NAND 里的任何东西
- 能救“启动链全坏”的板
```

缺点：

```text
- 需要 USB 连接和驱动
- 需要按键/短接
- 工具链复杂
```

### PhoenixSuit

Windows 上的官方刷机工具，用整片镜像：

```text
选择 v851s_linux_lizard_uart0.img
→ 按住 FEL / 按提示上电
→ 写入
```

Linux 上有 `sunxi-fel`、`xfel` 等开源工具，但**整片写入的
分区布局必须和厂商一致**，否则启动不了。

> 这一章不展开 FEL 命令细节。原则是：**整片刷是最后手段，
> 操作前先读发布 README 的刷机章节。**

### 什么时候必须整片刷

```text
- boot0/u-boot 损坏
- 分区表损坏
- 从别的产品线换回来
- NAND 更换后
```

### 整片刷的风险

```text
- 写错文件 → 变砖
- 中途断电 → 变砖
- 分区布局不匹配 → 起不来
- 把 UDISK 也覆盖 → 用户数据全丢
```

> 整片镜像通常**不包含**用户数据，但会重建分区。
> 刷前一定要确认 README 怎么说。

---

## 33.5 回滚

### 场景一：新 rootfs 有问题

如果 boot 卷没动，只需把旧 rootfs 写回：

```sh
# 救援系统里
ubi_recv /dev/ubi0_4 < rootfs-old.squashfs
```

### 场景二：新 kernel 有问题

把备份的 boot 卷写回：

```sh
ubi_recv /dev/ubi0_3 < /mnt/UDISK/boot-prev-20261114.img
```

### 场景三：启动参数被改坏

如果只是 `bootargs` 错了（比如 `saveenv` 存了错误参数）：

```text
U-Boot 里：
setenv bootargs <正确的>
saveenv
```

或者在救援系统里重置 env 分区（危险，先备份）。

### 场景四：配置被覆盖

overlay upper 里的旧文件盖住了新文件：

```sh
rm /mnt/UDISK/overlay/upper/etc/<file>
sync
```

> 第 18 章讲过：删 upper = 恢复 lower。**不要以为是删文件。**

### 回滚的前提

```text
你必须事先有旧镜像
```

所以发布流程里应该保留：

```text
上一版 rootfs.squashfs
上一版 boot 卷
```

这块板的 `/mnt/UDISK/boot-prev-20261114.img` 就是上一版 boot 卷。

> “能回滚”不是能力，是**准备**。没备份就没回滚。

---

## 33.6 刷机后验收

### 必查项

```sh
cat /etc/rootfs_version
md5sum /usr/bin/linkr-bee-daemon /usr/bin/linkr-ble /etc/init.d/S54bluetooth
cat /sys/class/remoteproc/remoteproc0/state
mount | grep -E '/etc/lib/bluetooth|/mnt/UDISK'
ss -ltnp | grep -E ':80|:8443'
```

### 功能项

```sh
# HTTP → HTTPS 重定向
curl -sI http://<board>/ | head -1        # 301

# HTTPS 页面
curl -sk https://<board>:8443/ | head -1  # 200

# WebSocket + UART3
# （用网页或脚本发一条命令，看目标板是否收到）

# BLE
hciconfig hci0
ps | grep linkr-ble
```

### 数据项

```sh
# Wi-Fi 还在
cat /mnt/UDISK/wpa_supplicant.conf

# 令牌还在
ls -l /mnt/UDISK/persist/ws-token

# 证书还在
ls -l /mnt/UDISK/persist/certs/

# BLE 绑定还在
mount | grep bluetooth
ls /mnt/UDISK/persist/bluetooth/
```

### 记录

把结果写进发布 README：

```markdown
| check | result |
|---|---|
| rootfs_version | v851s-lizard-e907-v12-20261114 |
| daemon md5 | 8b8db65e... |
| E907 | running (2) |
| HTTP/HTTPS | 301 / 200 |
| Wi-Fi | 192.168.0.121/24 |
| BLE | hci0 UP, advertising |
```

> 没有记录的验收 = 没验收。**下次出问题无法对比。**

---

## 33.7 现象 → 判断

### 完全没有任何输出

```text
可能：boot0/u-boot 坏了、供电问题、串口接错
下一步：
  1. 确认串口线、波特率、GND
  2. 换一个串口终端
  3. 尝试 FEL
```

### 有 U-Boot，但起不来内核

```text
可能：boot 卷坏了、bootargs 错、kernel 不匹配
下一步：
  1. 在 U-Boot 里 printenv 看 bootargs
  2. 用救援镜像启动
  3. 检查 boot 卷 md5
```

### 内核起来，但 panic

```text
可能：rootfs 挂载失败、驱动缺失、设备树不匹配
下一步：
  1. 看 panic 信息里的 mount/root= 相关行
  2. 确认 ubi 卷号
  3. 用救援系统检查 ubi0_4 是不是有效 squashfs
```

### 进救援 shell，但写不进去

```text
可能：卷被挂载、写保护、空间不足
下一步：
  1. umount
  2. ubinfo -a 看卷大小
  3. dmesg 看 NAND 错误
```

### 刷完能启动，但服务没起来

```text
可能：S?? 脚本失败、文件权限、配置错
下一步：
  1. /etc/init.d/S53linkr status
  2. tail /var/log/linkr-bee.log
  3. 手动跑一次 daemon 看报错
```

### 网络不通

```text
可能：Wi-Fi 凭据丢/overlay 旧文件/时间不对
下一步：
  1. ip link show wlan0
  2. wpa_cli status
  3. date
```

### 网页打不开，但 SSH 能连

```text
可能：daemon 没起、端口没监听、证书问题
下一步：
  1. pgrep linkr-bee-daemon
  2. ss -ltnp
  3. curl -v
```

> 记住顺序：**供电 → 串口 → boot → kernel → rootfs → 服务 → 网络**。
> 从下往上查，不要跳。

---

## 33.8 一个真实的刷机记录

```text
2026-11-14

1. 备份 UDISK（persist、wifi、ssh）
2. dd 备份 boot 卷到 /mnt/UDISK/boot-prev-20261114.img
3. ubi_stream.py 把 boot-rescue.img 写入 ubi0_3，md5 读回 9aca39e5…
4. 重启，打断 U-Boot
5. setenv bootargs ubi.mtd=sys console=ttyS0,115200 loglevel=7
6. run boot_normal
7. 救援 shell：ubi_recv /dev/ubi0_4 < rootfs.squashfs
   读回 dc24a473…
8. ubi_recv /dev/ubi0_3 < boot-normal.img
   读回 230e8c37…
9. sync; reboot -f
10. 验收：版本号、md5、E907 running、HTTP/HTTPS、WS↔UART3、
    Wi-Fi、BLE、bond mount、UDISK 数据
```

### 这次为什么没整片刷

```text
启动链没变，只有 rootfs 和 kernel 变了
→ 分卷更新足够
→ 风险最低
```

### 如果当时失败了

```text
- boot 卷写坏 → 用 boot-prev-20261114.img 写回
- rootfs 写坏 → 重进救援，再写一次
- 都写不进去 → FEL 整片刷
```

> 每一步都有“下一步怎么办”，这才叫可操作的流程。

---

## 33.9 实验

> 以下实验除 A、B 外，**必须在备用板上做，或确认能恢复**。

### 实验 A：只读地检查（安全）

```sh
cat /proc/mtd
ubinfo -a
cat /etc/rootfs_version
ls -l /mnt/UDISK/boot-prev-*.img 2>/dev/null
```

把分区表和卷大小画出来。

### 实验 B：模拟“版本没变”

```sh
# 不改任何东西，只看流程
md5sum /usr/bin/linkr-bee-daemon
```

思考：如果刷完 md5 没变，可能是哪几种原因？

### 实验 C：救援启动（备用板）

在备用板上：

1. 写入 `boot-rescue.img`。
2. 打断 U-Boot，设临时 bootargs。
3. 进入救援 shell。
4. **不写任何东西**，只执行 `ubinfo -a`、`cat /proc/mtd`、`ls`。

目标：熟悉界面，不冒险。

### 实验 D：故意改坏 bootargs（备用板）

```text
setenv bootargs console=ttyS0,115200 root=/dev/nonexistent
run boot_normal
```

观察 panic，然后**不 saveenv**，重启回正常。

### 实验 E：回滚演练（备用板）

1. 备份 boot 卷。
2. 写入救援镜像。
3. 用备份写回。
4. 确认正常启动。

---

## 33.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 刷完还是旧版本 | 写错卷/没重启 | `rootfs_version` |
| 每次重启进救援 | 忘了写回 boot-normal | ubi0_3 内容 |
| 启动卡在 U-Boot | bootargs 错/env 坏 | `printenv` |
| 内核 panic 挂 rootfs | 卷号/文件系统错 | panic 行 |
| 写卷失败 | 卷被挂载/空间不足 | `umount`、`ubinfo` |
| 刷完网络没了 | 凭据/overlay/时间 | `wpa_cli`、`date` |
| 服务起不来 | 权限/S 脚本/依赖 | `S53linkr status` |
| 数据全丢 | 整片刷覆盖 UDISK | 备份 |
| 板子完全无输出 | boot0/u-boot 坏 | FEL |
| 刷机中途断电 | 卷损坏 | 重刷/救援 |

---

## 33.11 练习

### 练习 33.1（必做）

画一张流程图，包含：

```text
运行中系统 → 写救援镜像 → 重启 → 打断 U-Boot →
设 bootargs → 救援 shell → 写 rootfs → 写 boot →
重启 → 验收
```

在每个节点标注“失败时怎么办”。

### 练习 33.2（必做）

回答：

1. 为什么救援系统要在 initramfs 里，而不是用现有 rootfs？
2. 为什么设了临时 bootargs 后不能 `saveenv`？
3. 为什么刷完 rootfs 还要写回 boot-normal？
4. 整片刷最大的三个风险是什么？
5. 回滚需要提前准备什么？
6. 刷机后验收至少要看哪五类信息？

### 练习 33.3（必做）

写一个 `preflight.sh`，在刷机前检查：

- 电池/供电（如果能读到）。
- UDISK 剩余空间。
- 备份文件是否存在。
- 当前版本号是否记录。
- 串口设备是否存在。

### 练习 33.4（选做）

在备用板上完整走一次救援刷机并回滚，记录每步耗时和 md5。

### 练习 33.5（选做）

写一份“救砖决策树”，从“板子完全没输出”开始，列出
每一步的判断和动作，直到“FEL 整片刷”。

---

## 33.12 本章验收

1. 能说出三条刷机路径及各自适用场景。
2. 理解救援 initramfs 的原理和完整步骤。
3. 会做刷机前备份和刷机后逐项验收。
4. 知道 FEL 能救什么、不能救什么。
5. 有明确的回滚方案。
6. 能按现象定位问题，而不是盲目重刷。

### 自测题

1. 日常更新 rootfs 用哪条路径？
2. 救援镜像写到哪个卷？
3. 为什么临时 bootargs 不能保存？
4. `ubi0_3` 和 `ubi0_4` 分别是什么？
5. 整片刷会不会覆盖用户数据？
6. 刷完第一件事查什么？

> 下一章讲怎么证明“它真的稳定”：测试、长稳、硬件在环。
