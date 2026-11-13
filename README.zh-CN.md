# v851s-e907-app

[English](README.md) | **简体中文**

YuzukiPI-Lizard（全志 V851S）上 **E907 RISC-V 协处理器** 的 Zephyr 固件，以及
本板 A7（Linux/Tina）侧的 **Linkr Bee** 适配。

E907 接管 UART3，也就是 Linkr Bee 的目标串口；A7 通过 OpenAMP/rpmsg 与它通信，
再把控制台通过 HTTP/WebSocket（局域网）和 BLE 暴露出来。

```
浏览器 ── HTTP(S)/WS ──┐
                       ├── linkr-bee-daemon ── /dev/rpmsgN ── E907 ── UART3
BLE 主机 ── GATT ── linkr-ble ── /var/run/linkr-bee.sock ──┘
                             │
                             └── D-Bus GattManager1 ── bluetoothd ── hci0
```

## 目录结构

```
src/, prj.conf, CMakeLists.txt   E907 Zephyr 应用（rpmsg + UART3 桥）
firmware/zephyr_e907.elf         由 src/ 编译，remoteproc 加载它
tools/                           主机侧 rpmsg 冒烟测试与基准
linkr-bee/
├── patches/                     基于 linkr-bee d9b2e00 的 5 个补丁
├── snapshot/web/                打完补丁的 web 文件（md5 固定）
├── web/                         板内实际运行的完整 web 终端
├── a7/                          A7 daemon、GATT server、启动脚本、配置
├── apply-web.sh                 把补丁打到任意 linkr-bee 检出目录
├── build-linkr-ble.sh           交叉编译 BlueZ GATT server
└── verify.sh                    md5 自检，可选校验板内/镜像
docs/
├── PROTOCOL.md                  rpmsg 控制行、BLE socket 帧、WS 鉴权
├── BUILD.md                     编译固件和 A7 二进制
├── DEPLOY.md                    部署到板子（SSH 或 UART）
└── REVIEW.md                    V851S 适配的 review 记录
```

## 固件特性

- UART3 桥，双向各 **64 KiB** 环形缓冲（运行时 `LINKRCFG` 改串口，
  `LINKRSTAT` 查询占用与丢包计数）
- OpenAMP/rpmsg `rpmsg_chrdev` 端点，256 字节消息，队列满时向上游施加背压，
  不静默丢数据
- A7 侧：HTTP(80) + HTTPS(8443) 终端、WSS、BLE GATT（NUS、Management v1、
  Reliable UART v1）、LAN 会话控制、通过 `wctl` 管理 Wi-Fi
- 诊断：`@i?` 返回固件版本、运行时间、真实 UART 环形缓冲占用与丢包、Wi-Fi
  和上传状态；局域网页面连接后会自动查询一次
- BLE 一次只能连一个中心设备：XR829 控制器在已有连接时不会真正发送广播
  （HCI 开启广播返回成功，但空中收不到）。第二个终端可以走 LAN/WSS，局域网
  接口支持多客户端。

## 快速开始

E907 固件（需要一个带 `v851s` BSP 的 Zephyr 检出）：

```sh
west build -b v851s_e907 apps/e907_bringup
```

A7 侧（板内镜像已自带）：`/usr/bin/linkr-bee-daemon`、`/usr/bin/linkr-ble`、
`/etc/init.d/S53linkr`、`/etc/init.d/S54bluetooth`。在
`/mnt/UDISK/persist/certs/`（或 `/usr/share/linkr-bee/certs/`）放好每板证书后，
页面地址是 `https://<板子 IP>:8443/`（自签名，见 `docs/DEPLOY.md`），否则是
`http://<板子 IP>/`。重新编译见 [docs/BUILD.md](docs/BUILD.md)，更新运行中的
板子见 [docs/DEPLOY.md](docs/DEPLOY.md)。

## 来源与版本

| 组件 | 上游 / 来源 | 版本 | 许可证 | 是否入库 |
|---|---|---|---|---|
| Zephyr RTOS | `zephyrproject-rtos/zephyr` | `v4.5.0` | Apache-2.0 | 否（依赖） |
| V851S BSP（板级、DTS、SoC glue） | `Novice-PG/zephyr` 分支 `v851s-e907-bsp`（上游 fork） | `6ebcb271c` | Apache-2.0 | 否（依赖） |
| OpenAMP / libmetal | Zephyr module 集 | 随 v4.5.0 | BSD-3-Clause | 否（依赖） |
| Linkr Bee（项目） | `radxa/linkr-bee` | `d9b2e00` | Apache-2.0 | web 树、补丁、快照；A7 侧是同一套服务的独立实现 |
| Linkr Bee ESP32/Zephyr 固件 | `radxa/linkr-bee` | `d9b2e00` | Apache-2.0 | **否**（未复制、未 vendored） |
| 前端 vendored 库 | npm 包（xterm、marked、DOMPurify、JSZip、esptool-js、agent runtime） | 见 `linkr-bee/web/vendor/README.md` | MIT / Apache-2.0 / MPL-2.0 / GPL-3.0-or-MIT | 是 |
| 字体 | BabelStone / Linkr legacy font | 见 `linkr-bee/web/fonts/` | SIL OFL 1.1 | 是 |
| E907 应用、daemon、GATT server、启动脚本 | 本项目 | — | Apache-2.0 | 是 |

本仓库**不包含**全志 Tina SDK、厂商库/固件、整片刷机镜像或芯片 datasheet。

## 范围与许可

本仓库只包含 **自己编写的代码、板级集成脚本和可再分发的第三方组件**。不得包含
全志 SDK 源码、厂商库或固件 blob、完整刷机镜像、芯片 datasheet——这些受全志
EULA 约束，不能分发。

- `src/`、`tools/`、`linkr-bee/a7/linkr_ble.c`、`linkr-bee/a7/linkr-bee-daemon`：
  原创，Apache-2.0（见 `LICENSE`）。
- Linkr Bee：[radxa/linkr-bee](https://github.com/radxa/linkr-bee)，基线
  `d9b2e00`，Apache-2.0。`linkr-bee/web/`、`snapshot/web/`、`patches/` 是该
  项目打补丁后的前端；ESP32/Zephyr 固件源码**没有**复制进来，
  `linkr-bee/a7/linkr_ble.c` 和 `linkr-bee/a7/linkr-bee-daemon` 是为 V851S
  独立实现的同套 GATT 服务、UUID、管理命令和 Reliable UART 帧格式。详见
  `linkr-bee/LICENSE` 以及 `linkr-bee/web/vendor/`、`linkr-bee/web/fonts/`
  下各组件自己的许可证。
- `linkr-bee/a7/linkr-ble` 二进制由本仓库的 `linkr_ble.c` 编译而来。

详见 [NOTICE](NOTICE)。
