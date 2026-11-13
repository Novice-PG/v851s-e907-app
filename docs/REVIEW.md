# Review notes: Linkr Bee on the V851S

This is a review of the Linkr Bee adaptation kept in this repository. None of
it is proposed to `radxa/linkr-bee`, and none of it lives in the
`Novice-PG/linkr-bee` fork: that fork stays clean and only ever carries
upstreamable work.

## What the adaptation actually changes

### Web front-end (generic fixes, two patches)

1. **`crypto.randomUUID` fallback** — `randomUUID` is only exposed in secure
   contexts, but the LAN bridge is normally served over plain
   `http://<board>/`. Several modules (agent panel, device executor) call it
   and throw on plain HTTP. The patch builds a v4 UUID from
   `crypto.getRandomValues` when `randomUUID` is missing.
2. **LAN management hint** — the management panel assumes a BLE transport in a
   few places; the patch makes it state that the session is reachable over
   LAN instead of showing a BLE-only hint.

Both patches apply cleanly on upstream `d9b2e00` and reproduce the snapshot
byte for byte (`linkr-bee/verify.sh`).

### A7 integration (V851S only)

- `linkr-bee-daemon`: adds the rpmsg endpoint for `/dev/rpmsg_ctrl0`, the
  `LINKRCFG` control path, the BLE socket bridge, the LAN session-ticket model,
  persistence on `/mnt/UDISK/persist` (`ws-token`, `ble-name`, `webdav-url`,
  `target`, `bond-labels`), and `wctl`-backed Wi-Fi commands.
- `linkr_ble.c`: BlueZ 5.54 GATT server exposing NUS, Linkr Management
  Service v1 and Reliable UART Service v1, plus a manual
  `HCI_OP_LE_SET_ADV_DATA`/`SCAN_RSP` path because the Allwinner 4.9 kernel
  comments those commands out of `hci_event.c`.
- `S53linkr` / `S54bluetooth`: boot ordering (XR829 firmware and `wlan0`
  before `hciattach`, `hciattach` from a clean state, `bluetoothd` after
  `hci0`, then `linkr-ble`), bond persistence via bind mount onto UDISK.
- `main.conf`: `Channels = 1`, because the 4.9 kernel has no EATT PSM and the
  default of 3 makes `bluetoothd` fail to create its socket.

## Findings

### Fixed in this tree

1. `linkr-bee-daemon` imported `base64` twice (`import base64` appeared in
   two places in the stdlib import block). Harmless, removed.
2. `_local_ip()` probed a hard-coded `192.168.0.1` to discover the LAN
   address. A UDP `connect()` sends no packet, so it worked, but on any other
   subnet it fell back to `0.0.0.0` and the daemon advertised the wrong
   address. It now tries `1.1.1.1`, `8.8.8.8` and `192.168.0.1` in order.

Both changes are in `linkr-bee/a7/linkr-bee-daemon`; the board still runs the
pre-fix build (md5 `07eb2785…`) until the next deploy. The protocol is
unchanged.

### Known limitations (not bugs)

- **Session ownership is A7-side only.** The E907 bridge is stateless; every
  LAN/BLE client shares the same UART3. `@c` arbitration is enforced by the
  daemon, so two A7 processes would not coordinate.
- **`LINKRCFG` replies are filtered by prefix.** A target device that prints
  a line starting with `LINKRCFG ` is swallowed. This is an accepted trade-off
  for a console bridge.
- **HTTPS is off by default** (`--tls-port 0`). Web Bluetooth needs a secure
  context, so enabling it requires a certificate on the board; the private key
  is generated per board and never committed.
- **BLE advertising name** is stored in `/mnt/UDISK/persist/ble-name`; if the
  data partition is missing the name falls back to `Linkr Bee V851S`.
- **Bond data** lives in `/mnt/UDISK/persist/bluetooth`, bind-mounted over
  `/var/lib/bluetooth`. Re-flashing the data partition loses pairings.

## Not done / next steps

- Exercise a phone BLE pairing, reconnect and long-run (bond persistence) on
  hardware; only short smoke tests were done.
- `linkr-ble` currently starts only from `S54bluetooth`; if `bluetoothd` dies
  the GATT server is not restarted automatically.
- The two web patches are generic enough to go upstream, but they are
  deliberately not submitted from here.
