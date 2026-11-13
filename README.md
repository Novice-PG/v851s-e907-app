# v851s-e907-app

**English** | [简体中文](README.zh-CN.md)

Zephyr firmware for the **E907 RISC-V co-processor** on the YuzukiPI-Lizard
(Allwinner V851S), plus the **Linkr Bee** changes made for this board on the
A7 side (Linux/Tina).

The E907 owns UART3, which is the Linkr Bee target console. The A7 talks to it
over OpenAMP/rpmsg and exposes the console over HTTP/WebSocket (LAN) and BLE.

```
browser ── HTTP(S)/WS ──┐
                        ├── linkr-bee-daemon ── /dev/rpmsgN ── E907 ── UART3
BLE central ── GATT ── linkr-ble ── /var/run/linkr-bee.sock ──┘
                              │
                              └── D-Bus GattManager1 ── bluetoothd ── hci0
```

## Layout

```
src/, prj.conf, CMakeLists.txt   E907 Zephyr application (rpmsg + UART3 bridge)
firmware/zephyr_e907.elf         built from src/ (what remoteproc loads)
tools/                           host-side rpmsg smoke test and benchmark
linkr-bee/
├── patches/                     five web patches on top of linkr-bee d9b2e00
├── snapshot/web/                the patched web files (md5-pinned)
├── web/                         full web terminal as shipped on the board
├── a7/                          A7 daemon, GATT server, init scripts, config
├── apply-web.sh                 apply the patches to any linkr-bee checkout
├── build-linkr-ble.sh           cross-build the BlueZ GATT server
└── verify.sh                    md5 self-check + optional board/image check
docs/
├── PROTOCOL.md                  rpmsg control lines, BLE socket frames, WS auth
├── BUILD.md                     build the firmware and the A7 binaries
├── DEPLOY.md                    push to the board (SSH or UART)
└── REVIEW.md                    review notes for the V851S adaptation
```

## Firmware features

- UART3 bridge with 64 KiB rings in each direction (runtime `LINKRCFG`
  reconfiguration, `LINKRSTAT` occupancy/drop counters)
- OpenAMP/rpmsg `rpmsg_chrdev` endpoint, 256-byte messages, backpressure into
  the A7 instead of silent drops
- A7 side: HTTP(80)+HTTPS(8443) terminal, WSS, BLE GATT (NUS, Management v1,
  Reliable UART v1), LAN session tickets, Wi-Fi control via `wctl`
- Diagnostics: `@i?` reports firmware version, uptime, live UART ring fill and
  drop counters, Wi-Fi and upload state; the LAN page queries it on connect

## Quick start

E907 firmware (needs a Zephyr checkout with the `v851s` BSP):

```sh
west build -b v851s_e907 apps/e907_bringup
```

A7 side, on the board (already installed by the board image):
`/usr/bin/linkr-bee-daemon`, `/usr/bin/linkr-ble`, `/etc/init.d/S53linkr`,
`/etc/init.d/S54bluetooth`. With a per-board certificate in
`/usr/share/linkr-bee/certs/` the page is served at
`https://<board>:8443/` (self-signed; see `docs/DEPLOY.md`), otherwise at
`http://<board>/`. See [docs/BUILD.md](docs/BUILD.md) to rebuild and
[docs/DEPLOY.md](docs/DEPLOY.md) to update a running board.

## Provenance

| Component | Upstream / origin | Revision | License | In this repo |
|---|---|---|---|---|
| Zephyr RTOS | `zephyrproject-rtos/zephyr` | `v4.5.0` | Apache-2.0 | no (dependency) |
| V851S BSP (board, DTS, SoC glue) | `Novice-PG/zephyr` branch `v851s-e907-bsp` (fork of upstream) | `6ebcb271c` | Apache-2.0 | no (dependency) |
| OpenAMP / libmetal | Zephyr module set | matched to v4.5.0 | BSD-3-Clause | no (dependency) |
| Linkr Bee (project) | `radxa/linkr-bee` | `d9b2e00` | Apache-2.0 | web tree, patches, snapshot; A7 side is an independent implementation of the same services |
| Linkr Bee ESP32/Zephyr firmware | `radxa/linkr-bee` | `d9b2e00` | Apache-2.0 | **no** (not vendored) |
| Vendored web libs | npm packages (xterm, marked, DOMPurify, JSZip, esptool-js, agent runtime) | see `linkr-bee/web/vendor/README.md` | MIT / Apache-2.0 / MPL-2.0 / GPL-3.0-or-MIT | yes |
| Fonts | BabelStone / Linkr legacy font | see `linkr-bee/web/fonts/` | SIL OFL 1.1 | yes |
| E907 app, daemon, GATT server, init scripts | this project | — | Apache-2.0 | yes |

Nothing from the Allwinner Tina SDK, vendor libraries/firmware, full flash
images or chip datasheets is included.

## Scope and licensing

This repository contains **only original code, board integration scripts and
redistributable third-party components**. It does not contain, and must not
contain, Allwinner SDK sources, vendor libraries or firmware blobs, full flash
images, or chip datasheets — those are covered by the Allwinner EULA and are
not redistributable.

- `src/`, `tools/`, `linkr-bee/a7/linkr_ble.c`, `linkr-bee/a7/linkr-bee-daemon`:
  original work, Apache-2.0 (see `LICENSE`).
- Linkr Bee: [radxa/linkr-bee](https://github.com/radxa/linkr-bee), baseline
  `d9b2e00`, Apache-2.0.  `linkr-bee/web/`, `snapshot/web/` and `patches/` are
  the patched front-end from that project.  The ESP32/Zephyr firmware sources
  are **not** copied here; `linkr-bee/a7/linkr_ble.c` and
  `linkr-bee/a7/linkr-bee-daemon` are an independent V851S implementation of
  the same GATT services, UUIDs, management commands and Reliable UART
  framing.  See `linkr-bee/LICENSE` and the per-package licenses under
  `linkr-bee/web/vendor/` and `linkr-bee/web/fonts/`.
- `linkr-ble` binary in `linkr-bee/a7/` is built from `linkr_ble.c` here.

See [NOTICE](NOTICE) for details.
