# 第 18 章 启动脚本与持久化：让配置活过重启和刷机

> 本章目标：
> 1. 看懂 BusyBox init 的启动顺序，以及 `S??` 脚本的编号含义。
> 2. 知道这块板每个启动脚本做什么、为什么在那个位置。
> 3. 理解 overlayfs 下 `/etc` 的可写性，以及它和 UDISK 的关系。
> 4. 会写一个“可重复、可回滚、内容校验”的持久化脚本。
> 5. 能排查“重启丢配置”“刷机后旧文件盖新文件”。

---

## 18.1 BusyBox init 怎么启动系统

这块板没有 systemd，用的是 BusyBox init：

```text
内核启动
  │
  ├─ 挂载 rootfs（squashfs + overlayfs）
  ├─ 执行 /sbin/init（BusyBox）
  │   ├─ 读 /etc/inittab
  │   ├─ 执行 rcS（系统初始化）
  │   └─ 按编号顺序执行 /etc/init.d/S??*
  │
  └─ 起登录 shell（串口/网络）
```

### `/etc/inittab` 里通常有

```text
::sysinit:/etc/init.d/rcS
::respawn:/sbin/getty -L ttyS0 115200 vt100
```

- `sysinit`：系统初始化，跑一次。
- `respawn`：进程退出就重启（登录终端）。

`rcS` 做的事：

```sh
for i in /etc/init.d/S??*; do
    [ -x "$i" ] || continue
    "$i" start
done
```

**按文件名排序**，这就是为什么脚本叫 `S30persist`、
`S53linkr`——数字决定顺序。

### 编号不是随便写的

```text
S30persist    账号、/etc 绑定        ← 后面所有服务都依赖
S31hostname   主机名
S45wifi-conf  Wi-Fi 配置             ← wpa_supplicant 之前
S50wifideamon 启动 Wi-Fi
S52e907       加载 uart3pin、启动 E907  ← daemon 需要 rpmsg
S53linkr      启动 Python daemon
S54bluetooth  蓝牙 + linkr-ble
S99postnet    DHCP / NTP              ← 最后，网络已可用
```

依赖链：

```text
S30persist
  └─ S45wifi-conf → S50wifideamon → (wlan0)
                                       └─ S54bluetooth（等 wlan0）
  └─ S52e907 → S53linkr（需要 /dev/rpmsgN）
  └─ S54bluetooth → linkr-ble（需要 /var/run/linkr-bee.sock）
  └─ S99postnet（需要网络）
```

> 启动顺序错，服务就会“有时能起、有时起不来”。
> 这类问题最难查，因为日志里看不出“顺序”。

---

## 18.2 `/etc` 到底是只读还是可写

### 事实

```text
/         → overlayfs
  lower   → /rom（squashfs，只读）
  upper   → /mnt/UDISK/overlay/upper（可写）
```

所以 `/etc` **看起来可写**：

```sh
echo test > /etc/foo
```

实际发生：

1. 文件从 lower 复制到 upper（copy-up）。
2. 你改的是 upper 里的副本。
3. `/rom/etc/foo` 永远不变。

### 三个后果

**后果一：重启后改动还在**

因为 upper 在 UDISK，掉电/重启不丢。

**后果二：刷 rootfs 后改动还在**

重刷 `ubi0_4` 只换 lower。upper 里的旧副本仍盖在新文件上。

**后果三：UDISK 满了就写不动**

upper 在 UDISK。UDISK 满时，改 `/etc` 会失败。

### 查看某个文件的来源

```sh
ls -l /mnt/UDISK/overlay/upper/etc/foo 2>/dev/null
ls -l /rom/etc/foo
```

- upper 有 → 你看到的是 upper 版本。
- upper 没有 → 你看到的是 lower 版本。

### 恢复 lower 版本

```sh
rm /mnt/UDISK/overlay/upper/etc/foo
sync
```

删除 upper 副本 = 让 lower 重新可见。**这不是“删除文件”。**

> 第 8 章讲过同样的机制。这里再强调一次，因为启动脚本
> 的很多“坑”都来自 overlay。

---

## 18.3 持久化原则：什么放哪

| 数据 | 位置 | 原因 |
|---|---|---|
| 系统程序 | rootfs（`/usr`、`/bin`） | 刷机即更新 |
| 系统默认配置 | rootfs（`/etc` lower） | 出厂值 |
| 用户改的配置 | `/mnt/UDISK/persist/` | 跨刷机保留 |
| 运行时状态 | `/var`（tmpfs） | 不需要持久 |
| Wi-Fi 凭据 | `/mnt/UDISK/wpa_supplicant.conf` | 不能进镜像 |
| HTTPS 私钥 | `/mnt/UDISK/persist/certs/` | 不能进镜像 |
| LAN 令牌 | `/mnt/UDISK/persist/ws-token`（600） | 每板不同 |
| BLE 绑定 | `/mnt/UDISK/persist/bluetooth/` | `/var` 是 tmpfs |
| 账号 | `/mnt/UDISK/persist/etc/passwd` 等 | rootfs 只读 |

### 一条铁律

> **任何“只在本板有意义”或“敏感”的东西，都不能进镜像。**

镜像会被复制、分发、解压。密码、私钥、令牌进镜像 = 泄露。

---

## 18.4 `S30persist`：账号与 `/etc` 的绑定

### 它解决什么问题

- rootfs 是只读的，`passwd` 不能改。
- `/var` 是 tmpfs，重启清空，不能放账号。
- 掉电可能留下“大小正常、内容全 NUL”的坏文件。

### 脚本骨架

```sh
#!/bin/sh
PERSIST=/mnt/UDISK/persist

seed() {
    src=$1
    dst=$2
    # 目标不存在、为空、或内容不健康 → 从 rootfs 重新播种
    if [ ! -s "$dst" ] || ! grep -q '^root:' "$dst" 2>/dev/null; then
        mkdir -p "$(dirname "$dst")"
        cp "$src" "$dst"
        sync
    fi
}

# 1. 确保 UDISK 已挂载
grep -q ' /mnt/UDISK ' /proc/mounts || exit 0

# 2. 播种账号文件
seed /rom/etc/passwd "$PERSIST/etc/passwd"
seed /rom/etc/shadow "$PERSIST/etc/shadow"
seed /rom/etc/group  "$PERSIST/etc/group"

# 3. 绑定到 /etc
mount --bind "$PERSIST/etc/passwd" /etc/passwd
mount --bind "$PERSIST/etc/shadow" /etc/shadow
mount --bind "$PERSIST/etc/group"  /etc/group
```

### 为什么先校验再绑定

如果 `passwd` 是坏的（全 NUL），直接绑定会让：

```text
dbus-daemon: Could not get UID and GID for username "root"
```

然后蓝牙、daemon 全起不来。

`grep -q '^root:'` 是最便宜的“内容健康检查”：只要有一行以
`root:` 开头，就认为文件可用。

> 这比“检查文件大小”可靠得多。文件大小对，内容可能全 0。

### 为什么用 `mount --bind` 而不是符号链接

有些程序（`dbus-daemon`、`login`）对 `/etc/passwd` 的路径
和权限有要求。符号链接在某些场景下会被拒绝或解析异常。
bind mount 让路径完全不变，只是底层换了存储。

---

## 18.5 `S45wifi-conf` / `S50wifideamon` / `S99postnet`

### 分工

| 脚本 | 做什么 |
|---|---|
| `S45wifi-conf` | 准备 `wpa_supplicant.conf`，建符号链接 |
| `S50wifideamon` | 启动 `wpa_supplicant` |
| `S99postnet` | DHCP、NTP 校时 |

### 为什么 Wi-Fi 配置要单独一个脚本

因为配置必须在 `wpa_supplicant` **之前**准备好：

```text
/etc/wifi/wpa_supplicant.conf → /mnt/UDISK/wpa_supplicant.conf
                                  （符号链接）
```

真正的凭据在 UDISK，`/etc` 下只是链接。这样：

- 镜像里没有密码。
- 改凭据只改 UDISK。
- 刷机不丢 Wi-Fi。

### 为什么 NTP 要放最后

板子没有电池 RTC，启动时时间是 1970 年。

TLS 证书有“生效时间”。时间不对，浏览器会报：

```text
NET::ERR_CERT_DATE_INVALID
```

所以 `S99postnet` 在拿到 IP 后：

```sh
ntpd -q -p pool.ntp.org
hwclock -w   # 如果硬件支持
```

> 顺序：网络 → 时间 → 证书验证 → 浏览器正常。

---

## 18.6 `S52e907`：加载引脚模块、启动从核

### 它做什么

```text
1. insmod uart3pin.ko   把 PE12/PE13 切成 UART3
2. 确认 /lib/firmware/ 里有固件
3. 启动 remoteproc（start_e907 或写 sysfs）
4. 等 /dev/rpmsgN 出现
```

### 为什么需要 `uart3pin.ko`

Linux 侧 `&uart3` 是 `disabled`，没有驱动认领，pinctrl 不会
自动生效。内核模块 `uart3pin.ko` 直接写 PIO 寄存器，把引脚
切成 UART3。

这和 E907 的 `board_early.c` 是**同一件事的两个副本**：

- E907 侧：Zephyr 启动时配。
- A7 侧：Linux 启动时配。

> 双保险。如果 E907 先启动、Linux 后配，可能会有短暂冲突，
> 但最终状态一致。

### 启动 remoteproc

```sh
echo zephyr.elf > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
```

或者用封装好的 `/usr/bin/start_e907`。

### 等待端点

```sh
i=0
while [ $i -lt 50 ]; do
    ls /dev/rpmsg[0-9]* >/dev/null 2>&1 && exit 0
    sleep 0.1
    i=$((i + 1))
done
```

注意：**这里等的是 E907 宣告端点**，不是 daemon 创建端点。
顺序是：

```text
E907 启动 → 宣告 rpmsg_chrdev → A7 内核登记
```

daemon 真正创建 `/dev/rpmsgN` 是后面的 `S53linkr` 做的。

---

## 18.7 `S53linkr`：启动 Python daemon

### 关键逻辑

```sh
CERT_DIR=/usr/share/linkr-bee/certs
if [ -f /mnt/UDISK/persist/certs/linkr-bee.crt ] &&
   [ -f /mnt/UDISK/persist/certs/linkr-bee.key ]; then
    CERT_DIR=/mnt/UDISK/persist/certs
fi

tls=0
if [ -f "$CERT_DIR/linkr-bee.crt" ] && [ -f "$CERT_DIR/linkr-bee.key" ]; then
    tls=$TLS_PORT      # 8443
fi

nohup python3 -u "$DAEMON" --port 80 --tls-port "$tls" \
    --cert "$CERT_DIR/linkr-bee.crt" \
    --key "$CERT_DIR/linkr-bee.key" \
    --web-root "$WEBROOT" >>"$LOGFILE" 2>&1 &
echo $! >"$PIDFILE"
```

### 逐段解释

- **证书优先 UDISK**：每板一份，跨刷机保留，私钥不进镜像。
- **没有证书就 `tls=0`**：只跑 HTTP，不阻断启动。
- **`nohup ... &`**：后台运行，脱离终端。
- **`python3 -u`**：不缓冲输出，日志实时。
- **PID 文件**：`start/stop/status` 用。

### 为什么不用 systemd

板子 64 MB 内存，BusyBox init 足够。systemd 会多占几十 MB，
还要维护 unit 文件。**在小系统上，shell 脚本 + PID 文件
是合理选择。**

### `stop` 为什么要等

```sh
kill "$(cat "$PIDFILE")"
for _ in 1 2 3; do
    sleep 1
    kill -0 "$(cat "$PIDFILE")" 2>/dev/null || break
done
kill -9 ...
```

daemon 有多个线程（rpmsg、serial-tx、HTTPS、BLE socket）。
直接 `kill -9` 可能留下：

- 未关闭的 socket。
- 未 flush 的日志。
- 半写的令牌文件。

先 `TERM`，等 3 秒，再 `KILL`。

---

## 18.8 `S54bluetooth`：顺序最讲究的脚本

这个脚本前面已经看过全文，这里提炼**为什么**。

### 六条顺序规则

1. **先起 D-Bus**：`bluetoothd` 需要系统总线。
2. **等 `wlan0` 出现再 attach BT**：XR829 是 Wi-Fi + BT 二合一，
   加载 Wi-Fi 固件会复位整颗芯片。BT 早 attach 白费。
3. **每次 attach 前 `killall hciattach`**：半开的 attach 会让
   内核保持 `HCI_RUNNING`，后续帧报 `-49`。
4. **不要碰 `/sys/class/rfkill`**：sunxi-bt 驱动的 `set_block()`
   逻辑反了，额外 unblock 会让固件握手超时。
5. **`bluetoothd` 必须在 `hci0` 之后**：否则不会创建 LE GATT
   数据库。
6. **最后 `hciconfig hci0 noscan`**：只做 LE，关掉经典蓝牙扫描。

### BLE MAC 为什么要固定

`hciattach` 默认从 `/etc/bluetooth/xr_bt.conf` 读地址，
没有就随机生成。全盘刷机后地址会变，手机里的绑定就失效。

脚本从 Wi-Fi MAC 派生：

```sh
wmac=$(cat /sys/class/net/wlan0/address)
btmac="22:22:$(echo "$wmac" | cut -d: -f3-6)"
```

`22:22:` 是本地管理地址前缀。这样每块板的 BLE 地址稳定，
手机重连不用重新配对。

### 为什么要 `EATTChannels = 1`

4.9 内核没有 EATT PSM 支持。BlueZ 默认开 3 个 ATT 通道，
`bluetoothd` 创建 socket 会失败。

```conf
[GATT]
EATTChannels = 1
```

> 注意：旧版 BlueZ 读的是 `Channels`，5.54 读 `EATTChannels`。
> 写错键名会被**静默忽略**，很难查。

### 为什么用 `script` 包 `hciattach`

`hciattach` 输出到管道/文件时是**块缓冲**，崩溃时最后几行丢失。
`script` 给它一个 pty，stdout 变行缓冲，失败时日志完整。

```sh
timeout 35 script -q -a -c "hciattach -n $TTY xradio" /var/log/hciattach.log
```

---

## 18.9 实验：观察启动顺序

### 实验 A：看脚本执行顺序

```sh
ls -l /etc/init.d/S*
```

按文件名排序即执行顺序。

### 实验 B：看每个脚本的状态

```sh
/etc/init.d/S53linkr status
/etc/init.d/S54bluetooth status
```

### 实验 C：加一个测试脚本

```sh
cat > /etc/init.d/S70hello <<'EOF'
#!/bin/sh
case "$1" in
    start) echo "$(date) hello from S70" >> /var/log/boot-order.log ;;
esac
EOF
chmod +x /etc/init.d/S70hello
```

重启后：

```sh
cat /var/log/boot-order.log
```

注意：`/var` 是 tmpfs，**重启后日志会被清空**。所以这个实验
要在一轮启动内看，或者把日志写到 UDISK。

### 实验 D：验证 overlay

```sh
ls -l /rom/etc/hostname
cat /etc/hostname
echo test > /etc/hostname
ls -l /mnt/UDISK/overlay/upper/etc/hostname
cat /etc/hostname
```

然后恢复：

```sh
rm /mnt/UDISK/overlay/upper/etc/hostname
sync
cat /etc/hostname
```

---

## 18.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 服务没起 | 脚本没执行权限 | `ls -l` |
| 服务顺序错 | 编号不对 | `S??` 排序 |
| 重启丢配置 | 写到 `/var`/tmpfs | `df`、挂载点 |
| 刷机后旧配置生效 | overlay upper 旧文件 | 查 upper |
| `passwd` 全 NUL | 掉电写一半 | `xxd`、重新播种 |
| Wi-Fi 密码没了 | 写了 `/etc` 没写 UDISK | 查符号链接 |
| 证书不生效 | 时间不对（1970） | `date`、NTP |
| 蓝牙起不来 | 顺序/rfkill/EATT | `/var/log/bluetooth-init.log` |
| `hciattach` 卡住 | 冷启动固件握手慢 | `timeout`、重试 |
| 掉电后服务坏 | 写文件没 `sync`/没 rename | 改原子写 |

---

## 18.11 练习

### 练习 18.1（必做）

在板子上：

```sh
ls -l /etc/init.d/S*
cat /etc/inittab
cat /proc/mounts | grep -E 'overlay|UDISK|passwd'
```

回答：

1. 脚本按什么排序？
2. `/etc/passwd` 实际来自哪里？
3. overlay 的 upper 在哪个文件系统？

### 练习 18.2（必做）

写一个 `seed()` 函数，要求：

- 目标不存在、为空、或内容不含 `^root:` 时，从源复制。
- 复制后 `sync`。
- 注释说明为什么检查内容而不只检查大小。

### 练习 18.3（必做）

回答：

1. 为什么 `S54bluetooth` 要等 `wlan0` 出现？
2. 为什么不能用 `/sys/class/rfkill`？
3. 为什么 BLE MAC 要从 Wi-Fi MAC 派生？
4. 为什么 NTP 放在 `S99postnet`？

### 练习 18.4（选做）

在虚拟机上模拟“掉电写坏 `passwd`”：

1. 把 `passwd` 内容清成全 0（`dd if=/dev/zero`）。
2. 重启，观察哪些服务起不来。
3. 用 `S30persist` 的校验逻辑恢复。

**不要在主力板上做。**

---

## 18.12 本章验收

1. 能解释 BusyBox init 如何按编号执行脚本。
2. 能说出这块板每个 S 脚本的职责和顺序原因。
3. 理解 overlayfs 下 `/etc` 的“可写但不持久于刷机”。
4. 会写“校验 + 播种 + bind mount”的持久化逻辑。
5. 能排查“重启丢配置”“刷机旧文件覆盖”“passwd 全 NUL”。
6. 知道 BLE MAC、EATT、rfkill 三个真实坑。

### 自测题

1. `S30persist` 和 `S53linkr` 谁先执行？为什么？
2. 删掉 upper 里的文件，等于删除文件吗？
3. 为什么 `/var` 不能放需要持久的数据？
4. 掉电为什么会产生“大小正常、内容全 0”的文件？
5. 为什么 `hciattach` 要用 `script` 包一层？
6. 时间不对会导致什么 TLS 问题？

> 下一章写 A7 上的串口 daemon：线程、队列、背压。
