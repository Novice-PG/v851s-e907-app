# 第 25 章 Wi-Fi 与网络配置

> 本章目标：
> 1. 知道这块板的 Wi-Fi 硬件和驱动栈。
> 2. 看懂 `wpa_supplicant` 的角色，会用 `wctl` 配网。
> 3. 理解为什么凭据只放 UDISK 不放镜像。
> 4. 能排查“扫不到”“连不上”“有 IP 但网页打不开”。
> 5. 知道 DHCP、DNS、NTP 各自解决什么问题。

---

## 25.1 硬件与驱动栈

### 硬件

```text
A7 ──SDIO──► XR829 ──2.4 GHz──► 路由器
       │
       └── 同一颗芯片还提供蓝牙（UART2）
```

- Wi-Fi 走 SDIO（SDC1）。
- 2.4 GHz only。
- 和蓝牙共用一个固件/电源域。

### 软件栈

```text
应用（daemon / wctl）
  │  wpa_cli / 配置文件
wpa_supplicant
  │  nl80211
内核（xradio 驱动 + cfg80211）
  │  SDIO
XR829 固件
```

| 层 | 负责 |
|---|---|
| 驱动 | SDIO 通信、固件加载、扫描/连接 |
| cfg80211 | 内核无线框架 |
| wpa_supplicant | 认证（WPA2/WPA3）、漫游 |
| DHCP 客户端 | 拿 IP |
| DNS | 解析域名 |
| NTP | 校时 |

---

## 25.2 为什么不用 NetworkManager

- 板子 64 MB 内存，BusyBox init。
- NetworkManager 依赖 D-Bus、大量库、几十 MB 内存。
- 家用场景只需要“连一个已知 AP”。

所以用：

```text
wpa_supplicant（认证）
  + udhcpc（DHCP）
  + wctl（封装脚本）
```

> 工具选择要看场景。服务器/桌面用 NetworkManager 合理，
> 小系统用 shell + wpa_supplicant 更合适。

---

## 25.3 配置放哪里

### 符号链接

```text
/etc/wifi/wpa_supplicant.conf
  → /mnt/UDISK/wpa_supplicant.conf
```

为什么：

- 镜像里没有密码。
- 改凭据只改 UDISK。
- 刷 rootfs 不丢 Wi-Fi。

### `S45wifi-conf` 做什么

```sh
# 概念版
CONF=/mnt/UDISK/wpa_supplicant.conf
LINK=/etc/wifi/wpa_supplicant.conf

# 1. 第一次启动：播种模板
if [ ! -s "$CONF" ]; then
    cat > "$CONF" <<'EOF'
ctrl_interface=/etc/wifi/wpa_supplicant/sockets
update_config=1
country=CN
EOF
    sync
fi

# 2. 建符号链接
mkdir -p /etc/wifi
ln -sf "$CONF" "$LINK"
```

### `wpa_supplicant.conf` 长什么样

```conf
ctrl_interface=/etc/wifi/wpa_supplicant/sockets
update_config=1
country=CN

network={
    ssid="MyHome"
    psk="my-password"
    key_mgmt=WPA-PSK
}
```

| 字段 | 作用 |
|---|---|
| `ctrl_interface` | `wpa_cli` 的 socket 目录 |
| `update_config=1` | 允许 `wpa_cli` 写回配置 |
| `country` | 管制域，影响信道/功率 |
| `ssid` | 网络名 |
| `psk` | 预共享密钥 |
| `key_mgmt` | 认证方式 |

> `country=CN` 不是可选项。没有它，某些信道不可用，
> 或者功率被限制到几乎连不上。

---

## 25.4 `wctl`：把 wpa_cli 包成人类可用的命令

### 命令

```text
wctl status
wctl list
wctl connect SSID password PASS
wctl disconnect
wctl saved
wctl forget SSID
```

### 实现思路

```sh
#!/bin/sh
SOCK=/etc/wifi/wpa_supplicant/sockets
IFACE=wlan0

case "$1" in
  status)
    wpa_cli -p "$SOCK" -i "$IFACE" status
    ip -4 addr show "$IFACE"
    ;;
  list)
    wpa_cli -p "$SOCK" -i "$IFACE" scan
    sleep 3
    wpa_cli -p "$SOCK" -i "$IFACE" scan_results
    ;;
  connect)
    ssid=$2
    pass=$3
    id=$(wpa_cli -p "$SOCK" -i "$IFACE" add_network | tail -1)
    wpa_cli -p "$SOCK" -i "$IFACE" set_network "$id" ssid "\"$ssid\""
    if [ -n "$pass" ]; then
        wpa_cli -p "$SOCK" -i "$IFACE" set_network "$id" psk "\"$pass\""
    else
        wpa_cli -p "$SOCK" -i "$IFACE" set_network "$id" key_mgmt NONE
    fi
    wpa_cli -p "$SOCK" -i "$IFACE" enable_network "$id"
    wpa_cli -p "$SOCK" -i "$IFACE" save_config
    ;;
  ...
esac
```

> 这是概念版。真实 `wctl` 还要处理：引号、隐藏 SSID、
> 保存/删除、DHCP 重启、错误码。

### 为什么不用 `wpa_passphrase` 每次重写

`wpa_cli add_network` + `set_network` + `save_config` 可以
保留多个网络，按优先级自动选。重写会丢掉其他配置。

---

## 25.5 扫描、连接、DHCP 的时序

### 完整流程

```text
1. 驱动加载，wlan0 出现
2. wpa_supplicant 读配置，扫描
3. 找到匹配 SSID → 认证 → 关联
4. 四次握手（WPA2-PSK）
5. 拿到链路层连接
6. udhcpc 发 DHCP Discover
7. 拿到 IP/网关/DNS
8. NTP 校时
9. 网页可访问
```

### 每步怎么查

| 步骤 | 命令 | 正常现象 |
|---|---|---|
| 驱动 | `ip link show wlan0` | `state DOWN/UP` |
| 扫描 | `wpa_cli scan_results` | 列出 SSID |
| 关联 | `wpa_cli status` | `wpa_state=COMPLETED` |
| IP | `ip -4 addr show wlan0` | `inet 192.168.x.x` |
| 网关 | `ip route` | `default via ...` |
| DNS | `cat /etc/resolv.conf` | `nameserver ...` |
| 外网 | `ping -c1 1.1.1.1` | 通 |
| DNS | `nslookup example.com` | 有结果 |
| 时间 | `date` | 不是 1970 |

> 排查要**按顺序**。跳过中间步骤，会误判。

---

## 25.6 为什么凭据不能进镜像

### 镜像会去哪

- 复制到多块板。
- 上传到发布页。
- 被任何人解压。

### 后果

如果镜像里有 `psk="my-password"`：

- 任何下载镜像的人都知道你家 Wi-Fi 密码。
- 所有板共用同一个密码。
- 改密码要重新刷所有板。

### 正确做法

```text
镜像：只有模板（无 network 块）
UDISK：真实凭据
```

`S45wifi-conf` 第一次启动时播种模板，用户通过网页/BLE
输入 SSID/密码，写入 UDISK。

> 这是“配置与代码分离”的具体体现。凡是“每台设备不同”
> 或“敏感”的东西，都不能固化在通用镜像里。

---

## 25.7 `S50wifideamon` 与 `S99postnet`

### `S50wifideamon`

```sh
# 概念版
wpa_supplicant -B -i wlan0 -c /etc/wifi/wpa_supplicant.conf \
    -P /var/run/wpa_supplicant.pid
```

- `-B`：后台。
- `-i wlan0`：接口。
- `-c`：配置。
- `-P`：PID 文件。

### `S99postnet`

```sh
# 等链路
i=0
while [ $i -lt 30 ]; do
    ip -4 addr show wlan0 | grep -q 'inet ' && break
    sleep 1
    i=$((i + 1))
done

# DHCP（如果还没 IP）
udhcpc -i wlan0 -q -n -t 5

# NTP
ntpd -q -p pool.ntp.org 2>/dev/null
```

### 为什么 NTP 放最后

- 没有电池 RTC，启动是 1970。
- TLS 证书有生效时间。
- 时间不对 → 浏览器报证书错误。
- 必须等网络通了才能校时。

> 顺序：**网络 → 时间 → 证书 → 浏览器**。

---

## 25.8 常见 Wi-Fi 问题

### 扫不到

| 原因 | 查法 |
|---|---|
| 驱动没加载 | `lsmod`、`dmesg` |
| 接口 down | `ip link set wlan0 up` |
| 管制域错 | `iw reg get` |
| 5 GHz only | XR829 只支持 2.4 GHz |
| 隐藏 SSID | 需要显式 `scan_ssid=1` |

### 连不上

| 原因 | 查法 |
|---|---|
| 密码错 | `wpa_cli status` 看 `wpa_state` |
| 认证方式错 | WPA3 还是 WPA2 |
| MAC 过滤 | 路由器白名单 |
| 信号弱 | `wpa_cli signal_poll` |
| 信道冲突 | 换信道 |

### 有 IP 但网页打不开

| 原因 | 查法 |
|---|---|
| daemon 没起 | `pgrep linkr-bee-daemon` |
| 端口没监听 | `ss -ltnp` |
| 防火墙 | `iptables -L` |
| 证书问题 | 浏览器错误码 |
| 时间错 | `date` |
| 路由错 | `ip route` |

### 时好时坏

| 原因 | 查法 |
|---|---|
| 信号波动 | `wpa_cli signal_poll` 多次 |
| 省电模式 | `iw dev wlan0 set power_save off` |
| 蓝牙共存干扰 | 2.4 GHz 共享 |
| DHCP 租期 | `udhcpc` 日志 |

> XR829 的 Wi-Fi 和蓝牙共用 2.4 GHz 射频。大流量 BLE +
> Wi-Fi 同时跑时，互相干扰是正常的。

---

## 25.9 实验：配网与排障

### 实验 A：看接口

```sh
ip link show wlan0
iw dev wlan0 info
```

### 实验 B：扫描

```sh
wctl list
```

记录：

- SSID。
- 信道。
- 信号强度。
- 加密方式。

### 实验 C：连接

```sh
wctl connect MyHome my-password
wctl status
```

### 实验 D：验证网络

```sh
ip -4 addr show wlan0
ip route
ping -c1 1.1.1.1
nslookup example.com
date
```

### 实验 E：断开

```sh
wctl disconnect
```

### 实验 F：持久化验证

```sh
cat /mnt/UDISK/wpa_supplicant.conf
ls -l /etc/wifi/wpa_supplicant.conf
```

确认是符号链接，真实内容在 UDISK。

---

## 25.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| `wlan0` 不存在 | 驱动/固件没加载 | `dmesg`、`lsmod` |
| 扫不到 AP | 接口 down/管制域 | `ip link`、`iw reg` |
| 连不上 | 密码/认证方式 | `wpa_cli status` |
| 拿到 169.254 | DHCP 失败 | `udhcpc` 日志 |
| 有 IP 不能上网 | 网关/DNS | `ip route`、`resolv.conf` |
| 网页打不开 | daemon 没起 | `ss -ltnp` |
| 证书报错 | 时间 1970 | `date`、NTP |
| 凭据丢失 | 写了 `/etc` 没写 UDISK | 符号链接 |
| 镜像里有密码 | 打包时复制了配置 | 检查 rootfs |
| 时断时续 | 信号/省电/干扰 | `signal_poll`、`power_save` |

---

## 25.11 练习

### 练习 25.1（必做）

在板子上执行 25.9 的 A～F，记录每步输出。

### 练习 25.2（必做）

回答：

1. XR829 支持 5 GHz 吗？
2. 为什么 `wpa_supplicant.conf` 要放 UDISK？
3. `S45wifi-conf`、`S50wifideamon`、`S99postnet` 各做什么？
4. 为什么 NTP 要等网络通了再跑？
5. `wpa_state=COMPLETED` 表示什么？
6. `169.254.x.x` 说明什么？

### 练习 25.3（必做）

写一个 `net-status.sh`，依次检查：

```text
wlan0 存在 → 有 IP → 有默认路由 → DNS 可用 → 时间正常 → 8443 监听
```

每步输出“正常/异常”。

### 练习 25.4（选做）

用 `iw dev wlan0 set power_save off` 关闭省电，测一次吞吐，
再打开对比。记录差异。

---

## 25.12 本章验收

1. 能画出 Wi-Fi 的硬件和软件栈。
2. 知道为什么用 `wpa_supplicant` 而不是 NetworkManager。
3. 能解释凭据为什么只放 UDISK。
4. 会用 `wctl` 扫描、连接、查看状态。
5. 能按“驱动→扫描→关联→IP→路由→DNS→时间→端口”排查。
6. 知道 2.4 GHz、共存干扰、省电模式的影响。

### 自测题

1. XR829 走什么总线和什么频段？
2. `wpa_supplicant.conf` 里 `country` 有什么用？
3. 为什么凭据不能进镜像？
4. DHCP 失败会拿到什么地址？
5. 时间不对会导致什么？
6. 蓝牙和 Wi-Fi 为什么可能互相干扰？

> 第 3 部分到此结束。你已经能写 A7 上的服务、Web、TLS、BLE 和网络。
> 下一部分做多端：Web 前端、TUI、CLI、Android。
