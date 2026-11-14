# 第 34 章 测试与长稳：证明它真的能用

> 本章目标：
> 1. 知道测试金字塔在嵌入式里的变形：越往下越难自动化。
> 2. 会做“能重复”的硬件在环测试，而不是“我点了一下是好使的”。
> 3. 会设计长稳测试：跑多久、看什么指标、怎么判失败。
> 4. 会用清单和脚本把回归测试固化下来。
> 5. 能区分“功能正确”“边界正确”“长时间正确”。

---

## 34.1 为什么嵌入式测试更难

### 普通软件的测试

```text
函数有输入输出 → 单元测试
接口有请求响应 → 集成测试
有浏览器 → 端到端测试
```

### 嵌入式的现实

```text
- 一半逻辑在另一颗核上（E907）
- 一半逻辑在硬件里（UART、NAND、射频）
- 没有“重跑一次”这么便宜（要刷机）
- 时序、掉电、温度都会影响结果
```

所以测试要分层：

```text
        手工验收        （人看现象）
      硬件在环测试      （脚本 + 真板）
    集成测试           （多进程/多服务）
  单元测试             （纯函数）
静态检查               （编译、lint、manifest）
```

越往下，越快、越便宜、越该多写。

---

## 34.2 静态检查：零成本的守门员

### 编译警告

```sh
cmake -B build -GNinja -DBOARD=v851s_e907 .
ninja -C build
```

要求：

```text
- 零 warning（或至少新增零 warning）
- -Werror 用在 CI
```

```cmake
# 应用层
target_compile_options(app PRIVATE -Wall -Wextra -Werror)
```

### Python 语法检查

```sh
python3 -m py_compile linkr-bee/a7/linkr-bee-daemon
```

板子上是 Python 3.9，主机可能是 3.12。**用板子的版本检查**：

```sh
python3.9 -m py_compile linkr-bee/a7/linkr-bee-daemon
```

或者至少避免新语法（海象、`match`）。

### Shell 检查

```sh
sh -n linkr-bee/a7/S53linkr
command -v shellcheck && shellcheck linkr-bee/a7/S53linkr
```

### 清单校验

仓库里有个 `verify.sh`：

```sh
./linkr-bee/verify.sh
```

它做什么：

```text
1. 遍历 MANIFEST.sha256 里的每个文件
2. 重新算 md5
3. 不一致就报错
```

输出：

```text
== repository ==
all files match MANIFEST.sha256

verify: all checks passed
```

### `MANIFEST.sha256` 的两个坑

**坑一：自引用**

如果清单里包含 `MANIFEST.sha256` 自己，生成时算出的是旧内容，
永远对不上。

```sh
# 生成前先把自己从索引里拿掉
git rm --cached -q MANIFEST.sha256
```

**坑二：`__pycache__`**

跑过 `py_compile` 后目录里会有 `.pyc`，如果它们进了清单，
下次内容变了就失败。

```sh
# 生成时只列 git 跟踪的文件
git ls-files | LC_ALL=C sort -u | while IFS= read -r f; do
    md5sum "./$f"
done > MANIFEST.sha256
```

> `git ls-files` 天然排除未跟踪文件，也保证顺序稳定。

### 生成 + 提交 + 校验

```sh
git add <files>
git rm --cached -q MANIFEST.sha256
git ls-files | LC_ALL=C sort -u | while IFS= read -r f; do
    md5sum "./$f"
done > MANIFEST.sha256
git add MANIFEST.sha256
./linkr-bee/verify.sh
git commit -m "..."
```

> 清单是“仓库没被意外改动”的证据。**每次提交都重新生成。**

---

## 34.3 单元测试：纯逻辑

### 哪些能测

```text
- 协议帧的构造/解析
- 参数校验
- 状态机转移
- 编解码（UTF-8、base64、CRC）
- 分片/重组
```

### 哪些不好测

```text
- 直接操作寄存器的代码
- 依赖真实时序的代码
- 依赖中断的代码
```

做法：**把纯逻辑抽出来。**

### C 的例子：帧构造

```c
/* frame.h */
size_t frame_build(uint8_t *out, size_t cap, uint8_t type,
                   const void *payload, size_t len);
int frame_parse(const uint8_t *in, size_t len, struct frame *out);
```

测试：

```c
static void test_roundtrip(void)
{
    uint8_t buf[32];
    const char *msg = "hello";
    size_t n = frame_build(buf, sizeof(buf), 1, msg, 5);

    assert(n == 3 + 5);
    assert(buf[0] == 1);
    assert(buf[1] == 5);
    assert(buf[2] == 0);

    struct frame f;
    assert(frame_parse(buf, n, &f) == 0);
    assert(f.type == 1);
    assert(f.len == 5);
    assert(memcmp(f.data, msg, 5) == 0);
}
```

### 边界要覆盖

```text
长度 0
长度 1
长度 125/126/127（如果和长度编码有关）
长度正好等于缓冲
长度超过缓冲
非法的魔数
截断的帧
```

> 单元测试的价值不在“证明对”，在于**边界处不崩**。

### Python 的例子

```python
def test_control_status_owner():
    s = Session()
    a = FakeClient(); s.register(a)
    b = FakeClient(); s.register(b)

    s.set_owner(a.id)
    assert "owner=%d" % a.id in s.status(a)
    assert "mode=control" in s.status(a)
    assert "mode=readonly" in s.status(b)
```

`FakeClient` 只实现 `send_binary`/`send_text`/`send_event`。

> 这就是第 31 章“传输适配层”的额外好处：**业务逻辑能脱离网络测试。**

---

## 34.4 集成测试：多进程、真 socket

### 测什么

```text
daemon 起得来
监听 80/8443
WebSocket 握手成功
令牌校验
@ 命令有回复
控制回复不污染串口
```

### 用假 E907

没有板子时，可以写一个假的 rpmsg 端点：

```python
class FakeBridge:
    def send(self, data):
        # 模拟 E907：握手、回显、LINKRSTAT
        if data.startswith(b"LINKR1"):
            self.reply(b"LINKR1 OK rx=0\n")
        elif data.startswith(b"LINKRSTAT"):
            self.reply(b"LINKRSTAT rx=0/65536 tx=0/65536 drop=0/0\n")
        else:
            self.reply(data)
```

daemon 接这个假桥，就能在主机上跑完整流程。

### 一个集成测试骨架

```python
def test_ws_command():
    proc = start_daemon(port=18080, bridge=FakeBridge())
    try:
        ws = connect("ws://127.0.0.1:18080/ws")
        greeting = ws.recv()          # @ws auth=none
        ws.send("@i?")
        lines = recv_until(ws, "@info done")
        assert any("fw version=" in l for l in lines)
    finally:
        proc.terminate()
```

要点：

```text
1. 用高位端口，避免和真 daemon 冲突
2. 用临时目录做 web root
3. finally 里一定清理
4. 断言“有内容”，不要断言具体版本号
```

### 不要断言易变的东西

```python
# 容易坏
assert line == "@info sys uptime_ms=12345 owner=0 security=0"

# 更好
assert line.startswith("@info sys uptime_ms=")
assert "owner=" in line
```

> 测试应该验证**契约**，不是**当前实现细节**。

---

## 34.5 硬件在环（HIL）

### 什么算 HIL

```text
主机 ──串口/USB/网络──► 真板 ──UART3──► 目标板
```

脚本自动执行，人只看结果。

### 最小 HIL 能做什么

```text
1. 刷/替换文件
2. 重启服务或整机
3. 发命令
4. 查状态
5. 比对期望
```

### 这块板的关键 HIL 用例

| 编号 | 用例 | 判据 |
|---|---|---|
| H1 | 加载 E907 | `state=running`，`trace0` 有横幅 |
| H2 | rpmsg 端点 | `/dev/rpmsg*` 出现 |
| H3 | 握手 | `LINKR1 OK` |
| H4 | WS ↔ UART3 | 发 `WS2UART-OK`，目标板收到 |
| H5 | UART3 ↔ WS | 目标板发 `UART2WS-OK`，WS 收到 |
| H6 | 控制命令 | `@i?` 返回 `@info done` |
| H7 | 令牌 | 错误令牌被拒，正确令牌通过 |
| H8 | HTTPS | 8443 返回 200，80 返回 301 |
| H9 | BLE 广播 | `hciconfig hci0` UP，能扫到名字 |
| H10 | BLE 收发 | 订阅后能收到串口数据 |
| H11 | Wi-Fi | 有 IP、能 ping 网关 |
| H12 | 重启恢复 | 重启后 H1～H11 仍成立 |

### 用 Python 做 HIL

```python
import subprocess, socket, time

BOARD = "192.168.0.121"

def ssh(cmd, timeout=20):
    return subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "root@" + BOARD, cmd],
        capture_output=True, text=True, timeout=timeout)

def test_e907_running():
    out = ssh("cat /sys/class/remoteproc/remoteproc0/state").stdout
    assert "running" in out, out

def test_https():
    r = subprocess.run(
        ["curl", "-sk", "-o", "/dev/null", "-w", "%{http_code}",
         "https://%s:8443/" % BOARD],
        capture_output=True, text=True, timeout=10)
    assert r.stdout.strip() == "200"

def test_http_redirect():
    r = subprocess.run(
        ["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
         "http://%s/" % BOARD],
        capture_output=True, text=True, timeout=10)
    assert r.stdout.strip() == "301"
```

### 串口在环

```python
import serial

def test_uart3_roundtrip():
    # /dev/ttyUSB0 是 E907 的 UART3 侧
    ser = serial.Serial("/dev/ttyUSB0", 115200, timeout=2)
    send_ws("UART3-LOOP\r\n")
    line = ser.readline()
    assert b"UART3-LOOP" in line
```

> 串口测试要**先清缓冲**，否则会读到上一次的残留。

### 失败要留证据

```python
def run_case(name, fn):
    try:
        fn()
        print("PASS", name)
        return True
    except Exception as e:
        print("FAIL", name, e)
        dump_state()          # trace0、dmesg、daemon 日志
        return False
```

`dump_state()` 至少收集：

```text
remoteproc state
trace0 末尾 50 行
dmesg 末尾 50 行
/var/log/linkr-bee.log 末尾 50 行
当前版本号
```

> “失败了但没有日志”等于没测。

---

## 34.6 长稳测试

### 为什么要长稳

很多问题只在长时间后出现：

```text
- 内存泄漏（每小时涨一点）
- 句柄泄漏（fd 用满）
- 计数器溢出
- 日志把分区写满
- 定时器漂移
- BLE 连接慢慢变差
```

跑 5 分钟看不出来。

### 跑多久

| 目标 | 时长 |
|---|---|
| 冒烟 | 10 分钟 |
| 日常回归 | 2 小时 |
| 发布前 | 24 小时 |
| 极端 | 72 小时 + 断电重启 |

> 时间越长，越要自动化采集。**不要靠人盯着。**

### 看什么指标

```text
内存：VmRSS、MemAvailable
句柄：/proc/<pid>/fd 数量
线程：/proc/<pid>/status 的 Threads
串口：LINKRSTAT 的 drop
rpmsg：重连次数
网络：丢包、RTT
BLE：连接/断开次数
日志：文件大小
```

### 采集脚本

```sh
#!/bin/sh
# soak.sh —— 在板子上跑
OUT=/mnt/UDISK/soak-$(date +%Y%m%d-%H%M).log
PID=$(pidof linkr-bee-daemon)

while :; do
    {
        date +%s
        grep -E 'VmRSS|Threads' /proc/$PID/status
        ls /proc/$PID/fd | wc -l
        awk '/MemAvailable|MemFree/ {print}' /proc/meminfo
        cat /sys/class/remoteproc/remoteproc0/state
        wc -c /var/log/linkr-bee.log
    } >> "$OUT"
    sleep 60
done
```

注意：

- 写到 **UDISK**，不要写 `/var`（tmpfs，重启丢）。
- 日志要轮转，否则长稳测试本身会把磁盘写满。
- 记录时间戳，方便对齐事件。

### 判定标准

```text
通过：
- 进程没重启
- VmRSS 波动在 ±10% 内（无单调上升）
- fd 数量稳定
- drop 计数不持续增长
- 功能仍然可用

失败：
- 内存单调上升
- fd 单调上升
- drop 持续增长
- 服务重启过
- 功能失效
```

> “没崩”不等于“稳定”。**泄漏是慢性的崩溃。**

### 压力要真实

```text
- 周期性发串口数据（不要一直满速，真实场景是突发）
- 周期性开关 BLE 连接
- 周期性访问网页
- 周期性查 @i?
- 偶尔重启 E907
```

### 断电测试

```sh
# 在主机上
for i in $(seq 1 20); do
    # 通过智能插座断电 5 秒，再上电
    power_cycle
    sleep 60
    check_board_alive || fail
done
```

没有智能插座就手动，但至少做 5 次。

看什么：

```text
- 能否正常启动
- UDISK 是否损坏
- passwd/令牌/证书是否还在
- 服务是否自愈
```

> 掉电是嵌入式最常见的“真实故障”。**必须测。**

---

## 34.7 回归矩阵

### 为什么要矩阵

改了 A，可能弄坏 B。要有一张表告诉你“至少测哪些”。

| 改动 | 必测 |
|---|---|
| E907 固件 | H1、H2、H3、H4、H5、H12 |
| daemon | H4、H5、H6、H7、H8 |
| 网页 | H8、H6、浏览器手测 |
| linkr-ble | H9、H10、H12 |
| 启动脚本 | H1、H11、H12 |
| 内核/驱动 | 全部 |
| 证书/令牌 | H7、H8 |

### 记录方式

```text
日期 | 版本 | 改了啥 | H1..H12 | 备注
```

```text
2026-11-14 | v12 | S54 EATTChannels | ✓✓✓✓✓✓✓✓✓✓✓✓ | bond 保留
```

> 矩阵的作用不是“形式”，是**决定这次要不要全测**。

### 自动化程度

```text
能自动的：H1、H2、H6、H7、H8、H11
要半自动：H3、H4、H5（需要目标板配合）
要手动：H9、H10（手机）、浏览器手测
```

---

## 34.8 测试数据与清理

### 不要污染发布镜像

```text
- 测试脚本不要进 rootfs
- 测试产生的日志不要进镜像
- 测试用的假证书不要进镜像
```

### 测试后的清理

```sh
# 板子上
rm -rf /mnt/UDISK/soak-*.log
rm -rf /mnt/UDISK/test-*
```

### 用独立的目标板

```text
主力板：只跑发布版本
测试板：随便折腾
```

> 如果没有第二块板，至少做到“每次实验前备份、实验后恢复”。

---

## 34.9 实验

### 实验 A：跑一次静态检查

```sh
python3 -m py_compile linkr-bee/a7/linkr-bee-daemon
sh -n linkr-bee/a7/S53linkr
./linkr-bee/verify.sh
```

记录输出。故意改一个文件的一个字节，再跑 `verify.sh`，
看它是否报错，然后恢复。

### 实验 B：写一个单元测试

给“帧解析”写 5 个用例：

```text
正常帧
空载荷
截断帧
错误魔数
超大长度
```

### 实验 C：写一个 HIL 脚本

实现 H1、H6、H8 三个用例，输出 PASS/FAIL。

### 实验 D：跑 2 小时长稳

用 34.6 的脚本采集，结束后画一张 `VmRSS` 随时间变化的图
（用 `gnuplot` 或直接看数值趋势）。

### 实验 E：掉电 5 次

每次断电 5 秒，上电后检查：

```sh
cat /etc/rootfs_version
cat /mnt/UDISK/persist/ws-token >/dev/null && echo token-ok
ls /mnt/UDISK/persist/certs/
```

---

## 34.10 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| `verify.sh` 失败 | 文件被改/清单没重生成 | 重新生成清单 |
| 清单自引用失败 | 包含了 `MANIFEST.sha256` | `git rm --cached` |
| 测试偶发失败 | 时序/残留 | 清缓冲、加重试 |
| 长稳内存上涨 | 泄漏 | `VmRSS` 曲线 |
| fd 用满 | 连接没关 | `/proc/<pid>/fd` |
| 日志把盘写满 | 没轮转 | `logrotate`/限频 |
| HIL 刷不进文件 | 服务占用/权限 | 先 stop |
| 测试通过但用户报错 | 只测了契约没测体验 | 加手测 |
| 改了 A 坏了 B | 没跑回归矩阵 | 按矩阵补测 |
| 断电后起不来 | NAND/overlay 损坏 | 救援刷机 |

---

## 34.11 练习

### 练习 34.1（必做）

为你的项目列一张测试金字塔，写出每一层：

```text
测什么
用什么工具
多久跑一次
谁来跑
```

### 练习 34.2（必做）

写 5 个单元测试，覆盖一个纯函数的所有边界。

### 练习 34.3（必做）

实现一张回归矩阵，至少 8 个用例，标注“自动/半自动/手动”。

### 练习 34.4（必做）

回答：

1. 为什么 `MANIFEST.sha256` 不能包含自己？
2. 单元测试应该验证契约还是实现细节？
3. 长稳测试为什么要写 UDISK 而不是 `/var`？
4. “没崩”为什么不等于稳定？
5. 为什么掉电测试必须做？
6. 测试失败时最少要收集哪些信息？

### 练习 34.5（选做）

用假 E907 写一个集成测试，验证：

```text
WS 发 @i? → 收到 @info done
WS 发二进制 → 假桥收到 → 回显 → WS 收到
```

### 练习 34.6（选做）

跑 24 小时长稳，产出一份报告：

```text
起始/结束内存
fd 数量变化
drop 计数
重连次数
是否重启
结论
```

---

## 34.12 本章验收

1. 能画出适合嵌入式的测试金字塔。
2. 会做静态检查：编译、语法、清单。
3. 能把纯逻辑抽出来做单元测试，覆盖边界。
4. 能用假桥做集成测试，用真板做 HIL。
5. 会设计长稳测试：时长、指标、采集、判定。
6. 有回归矩阵，知道改什么测什么。
7. 能区分“功能对”“边界对”“长时间对”。

### 自测题

1. 测试金字塔从下到上是什么？
2. `MANIFEST.sha256` 生成时要注意什么？
3. 单元测试最该覆盖什么？
4. HIL 失败时最少收集哪三类日志？
5. 长稳测试看哪四个指标？
6. 回归矩阵解决什么问题？

> 下一章面对这块板最硬的约束：64 MB 内存和一颗 600 MHz 的核。
