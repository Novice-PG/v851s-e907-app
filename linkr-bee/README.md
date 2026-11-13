# linkr-bee (V851S side)

Everything this board runs for the Linkr Bee bridge. This directory is the
**only** place where the V851S changes live; the `Novice-PG/linkr-bee` fork
and `radxa/linkr-bee` stay untouched.

| Path | Installed as | Description |
|---|---|---|
| `a7/linkr-bee-daemon` | `/usr/bin/linkr-bee-daemon` | HTTP/WebSocket ↔ rpmsg daemon (Python 3, stdlib) |
| `a7/linkr-ble` | `/usr/bin/linkr-ble` | BlueZ GATT server (NUS + Management v1 + Reliable UART v1) |
| `a7/linkr_ble.c` | — | source of `linkr-ble` |
| `a7/S53linkr` | `/etc/init.d/S53linkr` | starts the LAN/WebSocket daemon |
| `a7/S54bluetooth` | `/etc/init.d/S54bluetooth` | XR829 BT + bluetoothd + linkr-ble |
| `a7/main.conf` | `/etc/bluetooth/main.conf` | BlueZ config (`Channels = 1`) |
| `a7/ble-adv-data.py` | `/usr/bin/ble-adv-data.py` | pushes LE ADV/SCAN_RSP the 4.9 kernel drops |
| `a7/certs/san.cnf` | `/usr/share/linkr-bee/certs/` | certificate template (key is per board) |
| `a7/S99linkrbee.legacy` | — | previous boot script, reference only |
| `patches/` | — | two web patches on top of `d9b2e00` |
| `snapshot/web/` | — | patched `web/index.html` and `web/app.js` |
| `web/` | `/usr/share/linkr-bee/web/` | complete web terminal as shipped |
| `apply-web.sh` | — | apply `patches/` to a linkr-bee checkout |
| `build-linkr-ble.sh` | — | cross-build the GATT server |
| `verify.sh` | — | md5 check of the tree and optionally a rootfs |

See `../docs/PROTOCOL.md` for the wire protocols and `../docs/REVIEW.md` for
the review of this adaptation.

## Baseline

| Item | Value |
|---|---|
| Upstream | `radxa/linkr-bee` |
| Web baseline commit | `d9b2e00` |
| Patched `web/index.html` | `39f8ce3c04cde21152e0d3e09a7b4b74` |
| Patched `web/app.js` | `8fcb8095b583945b65ee55aa7d933851` |

The patches only touch those two files. `snapshot/web/` exists so the board
tree can be recovered even if the upstream checkout is lost.

## Licence

`a7/linkr_ble.c` and `a7/linkr-bee-daemon` are original work, Apache-2.0.
`web/`, `snapshot/web/` and `patches/` derive from radxa/linkr-bee
(Apache-2.0, see `LICENSE`); vendored assets keep their own licences.
