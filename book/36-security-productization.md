# 第 36 章 安全与产品化：从“能用”到“敢交付”

> 本章目标：
> 1. 会写威胁模型：谁、能从哪进来、想拿走什么、失败会怎样。
> 2. 理解这块板的三条信任边界：局域网、BLE、物理接触。
> 3. 知道 TLS、令牌、配对各自能防什么、不能防什么。
> 4. 会做凭据管理：不进镜像、每板独立、可轮换、可撤销。
> 5. 能列出一份可执行的加固清单和量产流程。

---

## 36.1 为什么“家用设备”也要谈安全

很多人的第一反应：

```text
“就一块开发板，接在我家路由器后面，谁会攻击它？”
```

现实是：

```text
- 家里的 IoT 设备是最常见的僵尸网络来源
- 局域网里可能有访客手机、被感染的笔记本
- BLE 的作用范围能到几十米，穿墙
- 二手的、退货的设备里可能还留着你的凭据
- 设备被攻破后，它连着你的内网
```

所以安全不是“做大系统才需要”，是**任何联网设备都要有的基本盘**。

> 这一章不追求“军用级”。目标是：**已知风险都有明确对策或明确接受。**
> 这比“感觉挺安全”强得多。

---

## 36.2 威胁模型

### 四个问题

```text
1. 要保护什么？（资产）
2. 谁想动它？（对手）
3. 他能从哪进来？（入口）
4. 成功了他能做什么？（影响）
```

### 资产清单

| 资产 | 泄露后果 | 被改后果 |
|---|---|---|
| Wi-Fi 凭据 | 别人能上你的网 | 设备掉线/被引到假 AP |
| LAN 令牌 | 能控制串口 | 能向目标板发任意命令 |
| HTTPS 私钥 | 能冒充设备 | 中间人攻击 |
| BLE 绑定密钥 | 能连设备 | 同上 |
| SSH 私钥/授权 | 能登录 root | 完全控制 |
| 串口数据 | 看到目标板输出 | 向目标板注入命令 |
| 用户文件（WebDAV） | 隐私泄露 | 数据损坏 |

### 对手

```text
A. 同一局域网的普通用户（访客、家人、室友）
B. 被感染的设备（自动扫描、爆破）
C. 物理接触者（拿到板子的人）
D. 无线电范围内的攻击者（BLE）
E. 供应链（固件/镜像被换）
```

### 入口与对策总表

| 入口 | 对手 | 现状 | 对策 |
|---|---|---|---|
| HTTP :80 | A/B | 301 跳 HTTPS | 不允许明文传输敏感数据 |
| HTTPS :8443 | A/B | 自签名证书 | 客户端固定证书 |
| WebSocket | A/B | 可选令牌 | 默认开令牌、限速 |
| BLE | D | Just Works 配对 | 接受风险或加应用层令牌 |
| SSH | A/B/C | 密钥登录 | 禁密码、禁 root 密码 |
| 物理串口 | C | 无条件 shell | 接受（调试口） |
| 固件更新 | E | 手工刷 | 校验和 + 签名（可选） |

### 明确“接受”的风险

写威胁模型最重要的是**把不修的写下来**：

```text
接受：
- 物理接触者能通过串口拿到 root（调试口必须保留）
- BLE Just Works 允许范围内配对（控制器固件限制）
- 局域网内知道令牌的人能控制串口（这是设计意图）

不接受：
- 镜像里带私钥/凭据
- 明文传输令牌
- 任意人无需令牌即可控制
```

> “接受”不是“忽略”，是**有意识地承担**。评审时要能说出理由。

---

## 36.3 三条信任边界

### 边界一：局域网

```text
浏览器 ──TCP/TLS──► daemon
```

防护：

```text
1. HTTPS（自签名）+ 证书固定
2. 令牌（第一帧文本）
3. 只监听需要的端口
4. 限速/限连接数
```

弱点：

```text
- 自签名证书需要用户首次“接受风险”
- 令牌如果通过 HTTP 传，等于公开
```

### 边界二：BLE

```text
手机 ──BLE──► linkr-ble ──Unix socket──► daemon
```

防护：

```text
1. 配对（bond）
2. 只暴露需要的 GATT 服务
3. 管理命令需要显式写 CMD 特性
```

弱点：

```text
- Just Works 没有中间人保护
- 控制器固件限制（双连接问题）
- 配对信息存在 UDISK，物理接触可读
```

### 边界三：物理

```text
串口 / USB / 拆机读 NAND
```

防护：

```text
- 几乎无法防（这是调试设备的现实）
- 可以把敏感数据加密存储（代价大）
```

> 结论：**物理安全靠“设备在谁手里”，不靠软件。**
> 所以不要把它当成可软件解决的问题。

---

## 36.4 TLS：能防什么

### 它防

```text
- 窃听（同网段抓包看不到内容）
- 篡改（中间人改不了数据）
- 冒充服务端（如果客户端校验证书）
```

### 它不防

```text
- 服务端被攻破（攻击者拿到私钥）
- 客户端不校验（自签名 + “继续访问” = 无保护）
- 重放（需要协议层 nonce）
- 暴力破解令牌（需要限速）
```

### 自签名的现实

浏览器第一次访问会警告。用户点“继续”之后：

```text
浏览器记住了例外 → 之后不再提示
但这也意味着：任何用同样证书的假设备都能冒充
```

所以**客户端固定证书**才有意义：

```python
ctx = ssl.create_default_context(cafile="~/.config/bee/ca.pem")
ctx.check_hostname = True
```

```js
// 浏览器里做不到 pinning，只能靠：
// 1. 首次人工核对指纹
// 2. 或改用原生 App
```

### 核对指纹

```sh
openssl s_client -connect 192.168.0.121:8443 </dev/null 2>/dev/null \
  | openssl x509 -noout -fingerprint -sha256
```

第一次连接时人工比对屏幕上的指纹和板子上的：

```sh
openssl x509 -in /mnt/UDISK/persist/certs/server.crt -noout -fingerprint -sha256
```

> 这是一次性的、必须做的事。**不核对指纹的 HTTPS 只是“看起来安全”。**

---

## 36.5 令牌：正确用法与常见错误

### 令牌是什么

```text
32 位十六进制（128 bit）
存在 /mnt/UDISK/persist/ws-token
服务端在 WebSocket 握手后要求第一帧文本
```

### 正确用法

```sh
# 生成
openssl rand -hex 16 > /mnt/UDISK/persist/ws-token
chmod 600 /mnt/UDISK/persist/ws-token

# 下发
@s token=<hex>
```

### 常见错误

| 错误 | 后果 |
|---|---|
| 令牌写在镜像里 | 所有设备同一个令牌 |
| 令牌通过 HTTP 传 | 同网段可抓 |
| 令牌进 shell history | 本机其他用户可读 |
| 令牌写进日志 | 日志被拷走就泄露 |
| 令牌永不过期 | 泄露后永久有效 |
| 令牌用 `Math.random` 生成 | 可预测 |

### 轮换

```text
1. 生成新令牌
2. 下发 @s token=<new>
3. 更新所有客户端配置
4. 确认旧令牌被拒
```

> 轮换要有“确认旧令牌失效”这一步，否则可能根本没生效。

### 限速

没有限速时，令牌可以被暴力猜：

```text
128 bit 空间 → 暴力不可行
但如果令牌是 4 位数字 → 几秒就破
```

所以：

```text
1. 令牌必须足够长（≥128 bit）
2. 认证失败要退避（如 1 秒、2 秒、4 秒）
3. 连续失败 N 次可临时封 IP
```

> 令牌强度的前提是**长度 + 随机源**，不是“看起来乱”。

---

## 36.6 BLE：Just Works 的真相

### 配对方式

| 方式 | 有中间人保护 | 需要输入 |
|---|---|---|
| Just Works | ❌ | 无 |
| Passkey | ✅ | 6 位数字 |
| Numeric Comparison | ✅ | 两边确认 |
| OOB | ✅ | 带外通道 |

这块板用的是 BlueZ 默认，通常是 **Just Works**：范围内任何人都能配对。

### 为什么

```text
- 板子没有屏幕/键盘
- 应用层没实现 passkey 流程
- 部分控制器固件能力有限
```

### 缓解

```text
1. 接受（家用场景，作用范围有限）
2. 应用层加令牌：连上后第一条管理命令必须是 @auth <token>
3. 配对后限制“同时只有一个 central”
```

第 2 种最实用：**BLE 传输不可信，但应用层仍要认证。**

### 现有的缓解

```text
- 管理命令需要显式写 CMD 特性（不是随便读就能用）
- 串口数据通道是只读/只写，不暴露 shell
- daemon 的 owner 机制限制并发写
```

> BLE 的安全模型应该是：**传输层不可信，业务层自证身份。**

---

## 36.7 凭据管理

### 铁律

```text
1. 凭据不进镜像
2. 凭据不进 git
3. 每台设备独立
4. 可轮换、可撤销
5. 备份加密
```

### 存在哪

```text
/mnt/UDISK/wpa_supplicant.conf      Wi-Fi（600）
/mnt/UDISK/persist/ws-token         LAN 令牌（600）
/mnt/UDISK/persist/certs/           TLS 私钥（600）
/mnt/UDISK/persist/bluetooth/       BLE 绑定
/root/.ssh/authorized_keys          SSH 公钥
```

**都不在 rootfs 里**，所以重刷 rootfs 不会丢，也不会被误打包。

### 检查镜像里有没有

```sh
unsquashfs -l rootfs.squashfs | \
  grep -E '\.key$|ws-token|wpa_supplicant\.conf|id_rsa|id_ed25519|\.pem$'
```

应该**没有任何输出**（证书模板 `server.crt` 可以留，私钥不行）。

### git 里有没有

```sh
git log --all --diff-filter=A --name-only | \
  grep -E '\.key$|token|secret|password' | sort -u
```

```sh
# 用 gitleaks / trufflehog 扫描历史
gitleaks detect --source . --verbose
```

> 提交过的密钥 = 已泄露。**删除文件不够，要换密钥。**

### 每板独立

```text
设备 A：令牌 A、证书 A、Wi-Fi A
设备 B：令牌 B、证书 B、Wi-Fi B
```

量产时写一个脚本：

```sh
#!/bin/sh
set -eu
SERIAL="$1"

openssl rand -hex 16 > "/tmp/prod/$SERIAL/ws-token"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout "/tmp/prod/$SERIAL/server.key" \
  -out "/tmp/prod/$SERIAL/server.crt" \
  -days 3650 -nodes -subj "/CN=$SERIAL"
```

然后通过串口/SSH 写进板子，**不留任何副本**。

---

## 36.8 加固清单

### 系统层

```sh
# 1. 禁用密码登录
sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config
sed -i 's/^#*PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config

# 2. 关掉不需要的服务
ls /etc/init.d/
# 把用不到的 S?? 改名（去掉执行位）而不是删除

# 3. 只监听需要的端口
ss -ltnp
```

### 应用层

```text
- HTTP 只做 301，不服务内容
- HTTPS 强制
- WebSocket 默认要求令牌
- 认证失败退避
- 限制并发连接数
- 日志脱敏
```

### 数据层

```text
- 凭据 600
- 目录 700
- 备份加密
- 日志不落敏感字段
```

### 更新层

```text
- 发布带 SHA256SUMS
- 可选用 minisign/gpg 签名
- 版本号可查
- 回滚路径明确
```

### 一个检查脚本

```sh
#!/bin/sh
set -u
fail=0

check() {
    if eval "$2"; then
        echo "OK   $1"
    else
        echo "FAIL $1"
        fail=1
    fi
}

check "sshd no password"      "grep -q '^PasswordAuthentication no' /etc/ssh/sshd_config"
check "token mode 600"        "[ \"\$(stat -c %a /mnt/UDISK/persist/ws-token 2>/dev/null)\" = 600 ]"
check "https listening"       "ss -ltn | grep -q ':8443'"
check "http redirect"         "curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/ | grep -q 301"
check "no key in rootfs"      "! unsquashfs -l /rom/rootfs.squashfs 2>/dev/null | grep -q '\\.key$'"

exit $fail
```

> 加固不是“做一次”，是**每次发布都跑一遍**。

---

## 36.9 日志与隐私

### 什么不能进日志

```text
- 完整令牌（只能打前 4 后 4）
- Wi-Fi 密码
- TLS 私钥
- 用户文件内容
- 完整 URL（可能带 token 参数）
```

### 脱敏函数

```python
def redact(value, keep=4):
    if not value:
        return ""
    if len(value) <= keep * 2:
        return "*" * len(value)
    return value[:keep] + "..." + value[-keep:]
```

```python
log("auth token=%s" % redact(token))
# auth token=0123...cdef
```

### 日志轮转

```text
/var/log 是 tmpfs → 重启丢，不占 NAND
但如果写到 UDISK，必须轮转
```

```sh
# 简单限大小
log_size=$(wc -c < /var/log/linkr-bee.log)
[ "$log_size" -gt 1048576 ] && mv /var/log/linkr-bee.log /var/log/linkr-bee.log.1
```

> 日志写满分区 = 设备变砖的一种。**日志也要有上限。**

---

## 36.10 失败安全

### 原则

```text
出错时应该“更安全”，而不是“更方便”。
```

### 例子

| 场景 | 不安全 | 安全 |
|---|---|---|
| 证书读不到 | 关掉 TLS 校验 | 拒绝启动 HTTPS |
| 令牌文件缺失 | 不要求令牌 | 要求令牌或只读 |
| 配置解析失败 | 用默认（可能开放） | 用最保守默认 |
| BLE 配对失败 | 允许明文 | 拒绝连接 |

### 具体到这块板

```text
- 私钥缺失 → HTTPS 不启动，HTTP 只返回 503，不降级成明文服务
- 令牌为空 → 允许连接但只读？还是要求设置？
  选择：允许（家用便利），但 @i? 里明确 security=none
- 认证失败 → 断开，不退化成“无认证”
```

> “降级到可用”是安全设计里最危险的一句话。

---

## 36.11 产品化流程

### 从开发板到产品

```text
1. 硬件定型（BOM、天线、外壳）
2. 固件冻结（版本号、校验和）
3. 每板个性化（MAC、证书、令牌、序列号）
4. 烧录 + 验收
5. 包装 + 记录
```

### 每板个性化脚本（概念）

```sh
#!/bin/sh
set -eu
SERIAL="$1"
BOARD_IP="$2"

# 1. 生成
TOKEN=$(openssl rand -hex 16)

# 2. 写入
ssh root@"$BOARD_IP" "printf '%s\n' '$TOKEN' > /mnt/UDISK/persist/ws-token && chmod 600 /mnt/UDISK/persist/ws-token"

# 3. 验证
REMOTE=$(ssh root@"$BOARD_IP" 'cat /mnt/UDISK/persist/ws-token')
[ "$REMOTE" = "$TOKEN" ] || { echo "mismatch"; exit 1; }

# 4. 记录（只记序列号和令牌指纹，不记明文）
echo "$SERIAL $(printf '%s' "$TOKEN" | sha256sum | cut -c1-16)" >> production.log
```

### 序列号放哪

```text
/etc/serial-number          只读，随镜像
/mnt/UDISK/persist/serial   可写，个性化
```

设备启动时读出来，写进 `@i?` 的输出。

### 出厂验收

```text
- 版本号正确
- 序列号唯一
- 令牌可认证
- 证书指纹已记录
- Wi-Fi 可连
- BLE 可发现
- 串口可通
```

> 量产的核心不是“刷得快”，是**每台都可追溯、可复现**。

---

## 36.12 供应链与更新

### 镜像完整性

```sh
sha256sum -c SHA256SUMS
```

### 更强的：签名

```sh
# 生成（离线）
minisign -G -p pub.key -s sec.key
minisign -S -s sec.key -m SHA256SUMS

# 验证
minisign -V -p pub.key -m SHA256SUMS
```

好处：**即使下载站被换，没有私钥也签不出有效文件。**

### 更新机制的现实

这块板目前是**手工刷机**。这本身是一种安全策略：

```text
优点：没有远程更新通道 = 没有远程更新攻击面
缺点：修漏洞要靠人上门/寄回
```

如果要加 OTA：

```text
1. 必须有签名验证
2. 必须有回滚（A/B 分区或保留旧卷）
3. 必须防降级（版本号单调）
4. 必须能断电恢复
```

> OTA 是“把刷机风险自动化”。**没有回滚的 OTA 不能上线。**

---

## 36.13 实验

### 实验 A：写威胁模型

给你的设备写一页纸：

```text
资产：……
对手：……
入口：……
影响：……
接受的风险：……
```

### 实验 B：检查镜像

```sh
unsquashfs -l rootfs.squashfs | grep -E '\.key$|token|wpa_supplicant'
```

确认无输出。

### 实验 C：验证令牌

```sh
# 正确令牌
bee-cli --token-file /tmp/good send "echo ok\n"

# 错误令牌
echo wrong > /tmp/bad
bee-cli --token-file /tmp/bad send "echo ok\n"; echo $?   # 应为 4
```

### 实验 D：核对证书指纹

分别从板子和客户端取指纹，人工比对。

### 实验 E：脱敏检查

```sh
grep -i 'token\|password\|key' /var/log/linkr-bee.log | head
```

应该只有脱敏后的形式。

### 实验 F：失败安全

```sh
# 故意移走私钥，重启 HTTPS
mv /mnt/UDISK/persist/certs/server.key /tmp/
/etc/init.d/S?? restart
ss -ltn | grep 8443      # 应该没有监听
mv /tmp/server.key /mnt/UDISK/persist/certs/
```

确认它是“拒绝服务”而不是“降级成明文”。

---

## 36.14 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 镜像里有私钥 | 打包目录没清 | `unsquashfs -l` |
| 所有设备同令牌 | 令牌写死在镜像 | 查 `/etc` |
| 令牌被抓包 | 走 HTTP | 抓包、看 301 |
| 证书警告被忽略 | 没做 pinning | 客户端配置 |
| 日志泄露令牌 | 没脱敏 | `grep` 日志 |
| 暴力破解成功 | 令牌短/无限速 | 长度、退避 |
| BLE 谁都能连 | Just Works | 应用层加认证 |
| 私钥丢失后降级 | 失败不安全 | 故意测一次 |
| 量产记录泄露 | 记了明文 | 只记指纹 |
| 更新被替换 | 无签名 | 加 minisign |

---

## 36.15 练习

### 练习 36.1（必做）

为这块板写一份威胁模型，至少包含：

```text
5 个资产
4 类对手
5 个入口
3 条“明确接受”的风险
```

### 练习 36.2（必做）

回答：

1. HTTPS 能防什么、不能防什么？
2. 为什么自签名证书需要人工核对指纹？
3. 令牌至少要多长？为什么？
4. Just Works 的风险是什么？怎么缓解？
5. 为什么凭据不能进镜像？
6. “失败安全”是什么意思？举一个反例。

### 练习 36.3（必做）

写一个 `harden-check.sh`，至少检查 8 项，输出 PASS/FAIL。

### 练习 36.4（必做）

实现令牌轮换流程，并验证旧令牌失效。

### 练习 36.5（选做）

给发布流程加 minisign 签名，写验证脚本。

### 练习 36.6（选做）

设计一个“应用层 BLE 认证”方案：

```text
连上后第一条 CMD 必须是 @auth <token>
超时未认证则断开
```

说明它和配对的关系，以及能否被重放。

---

## 36.16 本章验收

1. 能写出完整的威胁模型，包括“接受的风险”。
2. 理解三条信任边界和各自的防护/弱点。
3. 知道 TLS、令牌、配对各自的能力边界。
4. 会做凭据管理：不进镜像、每板独立、可轮换。
5. 有一份可执行的加固清单和量产流程。
6. 理解失败安全，并能验证它。

### 自测题

1. 威胁模型的四个问题是什么？
2. 自签名证书为什么不够？
3. 令牌泄露后第一步做什么？
4. Just Works 为什么没有中间人保护？
5. 加固检查应该多久跑一次？
6. 没有回滚的 OTA 为什么不能上线？

> 下一章讲怎么把这一切交给别人：开源、文档、许可证。
