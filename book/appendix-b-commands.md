# 附录 B：命令速查

> 用法：忘了某条命令就查这里。`$` 表示主机，`#` 表示板子，
> `=>` 表示 U-Boot 或特殊环境。`<...>` 是占位符，别照抄。

---

## B.1 串口与连接

```sh
# 列出串口设备
$ ls -l /dev/ttyUSB* /dev/ttyACM*
$ dmesg | grep -i 'ttyUSB\|ttyACM'

# 加入 dialout 组（一次即可，重新登录生效）
$ sudo usermod -aG dialout "$USER"

# picocom（退出：Ctrl-A 然后 Ctrl-X）
$ picocom -b 115200 /dev/ttyUSB0

# minicom
$ minicom -D /dev/ttyUSB0 -b 115200

# 直接用 stty + cat（最轻量）
$ stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo
$ cat /dev/ttyUSB0
$ printf 'help\r\n' > /dev/ttyUSB0

# 看串口参数
$ stty -F /dev/ttyUSB0 -a

# 抓一段带时间戳的日志
$ stty -F /dev/ttyUSB0 115200 raw -echo
$ timeout 30 cat /dev/ttyUSB0 | ts '%H:%M:%.S' | tee uart.log
```

---

## B.2 SSH 与文件传输

```sh
# 登录
$ ssh root@192.168.0.121

# 指定密钥
$ ssh -i ~/.ssh/id_ed25519 root@192.168.0.121

# 执行单条命令
$ ssh root@192.168.0.121 'cat /etc/rootfs_version'

# 板内 scp 必须加 -O（BusyBox scp）
$ scp -O file root@192.168.0.121:/tmp/
$ scp -O -r dir root@192.168.0.121:/tmp/
$ scp -O root@192.168.0.121:/mnt/UDISK/persist/ws-token ./

# 免密（把公钥写进板子）
$ ssh-copy-id -i ~/.ssh/id_ed25519.pub root@192.168.0.121

# 保活
$ ssh -o ServerAliveInterval=15 root@192.168.0.121

# 端口转发（本地 8080 → 板子 8443）
$ ssh -L 8080:127.0.0.1:8443 root@192.168.0.121
```

---

## B.3 交叉编译与 Zephyr

```sh
# 看 ELF 架构
$ file build/zephyr/zephyr_e907.elf
$ readelf -h build/zephyr/zephyr_e907.elf | grep Machine

# Zephyr 构建
$ export ZEPHYR_BASE=/path/to/zephyr
$ cmake -B build -GNinja -DBOARD=v851s_e907 .
$ ninja -C build

# west
$ west build -b v851s_e907 .
$ west flash            # 如果有 runner

# 重新配置（改了 prj.conf 后）
$ ninja -C build menuconfig
$ ninja -C build

# 清理
$ rm -rf build && cmake -B build -GNinja -DBOARD=v851s_e907 .

# 查看配置是否生效
$ grep CONFIG_KERNEL_BIN_NAME build/zephyr/.config
$ grep RESOURCE_TABLE build/zephyr/.config

# 确认 resource table 段存在
$ readelf -S build/zephyr/zephyr_e907.elf | grep resource_table
```

---

## B.4 remoteproc 与 rpmsg

```sh
# 状态
# cat /sys/class/remoteproc/remoteproc0/state

# 重启 E907
# echo stop  > /sys/class/remoteproc/remoteproc0/state
# sleep 8
# echo start > /sys/class/remoteproc/remoteproc0/state

# 看固件名
# cat /sys/class/remoteproc/remoteproc0/firmware

# 换固件
$ scp -O build/zephyr/zephyr_e907.elf \
    root@192.168.0.121:/lib/firmware/e907_firmware.elf

# rpmsg 端点
# ls -l /dev/rpmsg*
# cat /sys/class/rpmsg/*/name

# RAM console（名字因平台而异）
# ls /sys/class/remoteproc/remoteproc0/
# tail -50 /sys/kernel/debug/remoteproc/remoteproc0/trace0

# dmesg 里的 rpmsg 信息
# dmesg | grep -i 'rpmsg\|remoteproc\|virtio'
```

---

## B.5 存储、UBI、MTD

```sh
# 分区表
# cat /proc/mtd
# cat /proc/partitions

# UBI 信息
# ubinfo -a
# ubinfo /dev/ubi0
# ubinfo -d 0 -N rootfs

# 卷信息
# ls -l /dev/ubi0_*
# cat /sys/class/ubi/ubi0_4/name
# cat /sys/class/ubi/ubi0_4/data_bytes

# 挂载
# mount -t ubifs ubi0_5 /mnt/UDISK
# mount | grep -E 'ubi|overlay|squashfs'

# 写卷（危险：先确认卷号）
# umount /mnt/UDISK
# ubiupdatevol /dev/ubi0_4 rootfs.squashfs

# 读回校验
# md5sum /dev/ubi0_4
# dd if=/dev/ubi0_4 bs=1M count=37 2>/dev/null | md5sum

# 查看 squashfs
$ unsquashfs -l rootfs.squashfs | head
$ unsquashfs -cat rootfs.squashfs /etc/rootfs_version
$ unsquashfs -d /tmp/rootfs rootfs.squashfs

# 制作 squashfs
$ mksquashfs rootfs/ rootfs.squashfs -comp gzip -noappend

# 备份 boot 卷
# dd if=/dev/ubi0_3 of=/mnt/UDISK/boot-prev.img bs=1M
# sync
```

---

## B.6 U-Boot

```text
=> help
=> printenv
=> printenv bootargs
=> setenv bootargs ubi.mtd=sys console=ttyS0,115200 loglevel=7
=> run boot_normal
=> saveenv          # 危险：会持久化
=> reset
=> mmc list
=> nand info
=> ubi part sys
=> ubi info
=> ubi list
```

**打断自动启动：** 看到 `Hit any key to stop autoboot` 时按任意键。

**不要**在临时调试时 `saveenv`，除非确认参数正确。

---

## B.7 进程、内存、性能

```sh
# 进程
# ps | grep linkr
# pidof linkr-bee-daemon
# top -H -p $(pidof linkr-bee-daemon)

# 内存
# free -m
# cat /proc/meminfo | grep -E 'MemTotal|MemFree|MemAvailable|Cached|Slab|Cma'
# grep -E 'VmRSS|RssAnon|RssFile|Threads' /proc/$(pidof linkr-bee-daemon)/status
# ls /proc/$(pidof linkr-bee-daemon)/fd | wc -l

# slab
# slabtop -o | head -20

# 内存泄漏曲线
# while :; do grep VmRSS /proc/$(pidof linkr-bee-daemon)/status; sleep 60; done

# OOM
# dmesg | grep -i -E 'oom|killed process'
# cat /proc/$(pidof linkr-bee-daemon)/oom_score

# 系统调用统计
# strace -c -p $(pidof linkr-bee-daemon)

# CPU 热点（如果支持）
# perf top -p $(pidof linkr-bee-daemon)

# 打开的文件
# lsof -p $(pidof linkr-bee-daemon) 2>/dev/null | head
```

---

## B.8 网络与端口

```sh
# 接口与地址
# ip addr show
# ip route
# ip -s link show wlan0

# 监听端口
# ss -ltnp
# ss -ltn | grep -E ':80|:8443'

# 连接测试
# ping -c3 192.168.0.1
# nc -zv 192.168.0.121 8443

# HTTP
$ curl -sI http://192.168.0.121/ | head -1
$ curl -sk https://192.168.0.121:8443/ | head -1
$ curl -skv https://192.168.0.121:8443/ 2>&1 | grep -E 'subject|issuer|SSL'

# 证书
$ openssl s_client -connect 192.168.0.121:8443 </dev/null 2>/dev/null \
    | openssl x509 -noout -subject -issuer -dates -fingerprint -sha256

# 本地证书
# openssl x509 -in /mnt/UDISK/persist/certs/server.crt -noout -fingerprint -sha256

# WebSocket 手测（Python）
$ python3 - <<'PY'
import socket, base64, os
s = socket.create_connection(("192.168.0.121", 80))
k = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {k}\r\n"
           f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
print(s.recv(4096).decode(errors="replace"))
PY
```

---

## B.9 Wi-Fi

```sh
# 状态
# ip link show wlan0
# wpa_cli -i wlan0 status
# iw dev wlan0 link
# iw dev wlan0 scan | grep -E 'SSID|signal'

# 连接（wpa_cli 交互）
# wpa_cli -i wlan0
> scan
> scan_results
> add_network
> set_network 0 ssid "MyHome"
> set_network 0 psk "password"
> enable_network 0
> save_config
> quit

# 重新加载
# wpa_cli -i wlan0 reconfigure

# DHCP
# udhcpc -i wlan0

# 配置位置（这块板）
# cat /mnt/UDISK/wpa_supplicant.conf
# chmod 600 /mnt/UDISK/wpa_supplicant.conf

# 国家码
# iw reg get
# iw reg set CN
```

---

## B.10 蓝牙

```sh
# 控制器
# hciconfig -a
# hciconfig hci0 up

# 进程
# ps | grep -E 'bluetoothd|linkr-ble'
# /etc/init.d/S54bluetooth status

# 服务与特性（本机）
# bluetoothctl
[bluetooth]# list
[bluetooth]# show
[bluetooth]# menus
[bluetooth]# list-attributes

# 扫描
# bluetoothctl --timeout 10 scan on

# 日志
# tail -50 /var/log/linkr-ble.log
# dmesg | grep -i blue

# 经典工具
# hcitool lescan
# gatttool -b <MAC> -I

# 抓包（需要 btmon）
# btmon -w /tmp/bt.snoop
```

---

## B.11 服务与日志

```sh
# 启停
# /etc/init.d/S53linkr status
# /etc/init.d/S53linkr restart
# /etc/init.d/S54bluetooth restart

# 手动跑一次看报错
# /usr/bin/linkr-bee-daemon --help
# python3 /usr/bin/linkr-bee-daemon

# 日志
# tail -f /var/log/linkr-bee.log
# dmesg | tail -50
# logread 2>/dev/null | tail

# 确认二进制版本
# md5sum /usr/bin/linkr-bee-daemon /usr/bin/linkr-ble
# strings /usr/bin/linkr-bee-daemon | grep -i version
```

---

## B.12 构建镜像与发布

```sh
# 校验发布
$ sha256sum -c SHA256SUMS

# 生成校验和
$ sha256sum *.img *.squashfs > SHA256SUMS

# 版本
$ cat /etc/rootfs_version

# 镜像里找敏感文件（应无输出）
$ unsquashfs -l rootfs.squashfs | \
    grep -E '\.key$|ws-token|wpa_supplicant\.conf|id_rsa'

# 大文件
$ unsquashfs -ll rootfs.squashfs | sort -k3 -n -r | head -20

# 生成证书
$ openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout server.key -out server.crt -days 3650 -nodes \
    -subj "/CN=v851s" -addext "subjectAltName=IP:192.168.0.121"

# 生成令牌
$ openssl rand -hex 16

# 哈希
$ sha256sum file
$ md5sum file
```

---

## B.13 Git

```sh
# 状态
$ git status -sb
$ git log --oneline -10
$ git diff --stat

# 提交（带签名）
$ git add -A
$ git commit -s -m "fix(ble): ..."

# 只看某文件历史
$ git log --follow -p -- path/to/file

# 找出引入某行的提交
$ git blame -L 100,120 file

# 暂时收起改动
$ git stash push -m wip
$ git stash pop

# 补提交（未推送）
$ git commit --amend --no-edit

# 检查要提交什么
$ git diff --cached

# 重新生成清单（避免自引用）
$ git rm --cached -q MANIFEST.sha256
$ git ls-files | LC_ALL=C sort -u | \
    while IFS= read -r f; do md5sum "./$f"; done > MANIFEST.sha256
$ git add MANIFEST.sha256
```

---

## B.14 文本处理（读日志用）

```sh
# 过滤 + 计数
$ grep -c 'ERROR' app.log
$ grep -oE 'phase=[a-z]+' app.log | sort | uniq -c

# 前后文
$ grep -n -B2 -A5 'panic' uart.log

# 时间范围
$ awk '$1 >= "10:00:00" && $1 <= "10:05:00"' uart.log

# 提取字段
$ sed -n 's/.*ip=\([0-9.]*\).*/\1/p' diag.txt

# 去重保序
$ awk '!seen[$0]++' file

# JSON
$ jq '.drop.rx' diag.json
$ jq -e '.firmware != null' diag.json

# 十六进制
$ xxd -g1 file | head
$ hexdump -C file | head
$ echo -n abc | xxd
```

---

## B.15 危险操作清单

```text
以下命令会改硬件状态，执行前确认：
1. ubiupdatevol /dev/ubi0_*        写错卷 = 起不来
2. dd of=/dev/ubi0_*               同上
3. saveenv（U-Boot）                持久化错误参数
4. rm -rf /mnt/UDISK/overlay/upper  等于恢复出厂
5. 整片刷机                         会重建分区
6. flash_erase /dev/mtd*           擦除启动链
7. mv /etc/init.d/S??               可能让服务不再自启
```

> 规则：**先备份、再确认卷号/路径、最后执行。**
