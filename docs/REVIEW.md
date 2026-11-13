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

3. `linkr_ble.c` did not ignore `SIGPIPE`. The daemon socket is a Unix
   stream; if the daemon restarted between two writes the kernel delivered
   `SIGPIPE` and killed the GATT server (default action). The handler is now
   ignored and short writes are retried.
4. `mgmt_write()` and `rel_rx_write()` dropped any bytes that followed a
   completed frame inside the same BLE write. A host that packs two frames
   into one write (`Write Without Response` batching, or a 512-byte Reliable
   UART window) lost the second frame. Both reassemblers now loop over the
   remainder, and `send_frame()` refuses frames larger than the 16-bit length
   field instead of truncating the header.

The daemon fixes are in `linkr-bee/a7/linkr-bee-daemon`; the GATT fixes are
in `linkr-bee/a7/linkr_ble.c`, and `linkr-bee/a7/linkr-ble` was rebuilt from
that source with `build-linkr-ble.sh`. The board still runs the previous
builds until the next deploy, so the three fixes need one hardware smoke test
(daemon + BLE console + a `@u=115200,8,n,1,n` round trip) before they are
considered verified. The protocol is unchanged.

### Open findings (not fixed)

1. **E907 mailbox notifications can be dropped.** `ipm_rx_cb()` queues the
   notify id with `k_msgq_put(..., K_NO_WAIT)` into an 8-entry queue and
   ignores the return value. If the queue is full, the vring notification is
   lost and rpmsg stalls until the next kick. Either increase the depth,
   handle `-ENOMSG` by processing in the thread that drains later, or use a
   counting semaphore plus a single "work pending" flag.
2. **`mailbox_notify()` passes a stack address to `ipm_send()`.** It is safe
   with the sunxi mailbox driver because the payload is copied into the data
   register synchronously, but a driver that defers the transfer would read
   freed stack. Pass a static `uint32_t` or let the IPM API copy.
3. **UART RX ring overflow is silent.** `rx_push()` ignores
   `ring_buf_put()`'s return value; at 4 KiB with a stalled A7 this loses
   bytes without a counter. `uart_poll_rx()` (thread) also reads the FIFO
   concurrently with `uart_isr()`, which the ns16550 driver does not
   guarantee to be safe. The polling path exists because CLIC routing was
   unproven; it should be removed once the IRQ path is trusted.
4. **LINKRCFG filtering is prefix- and chunk-based.** A control reply is only
   recognised when it starts an rpmsg chunk; a reply coalesced behind serial
   bytes reaches the browser and makes `@u=` time out. A target that prints
   `LINKRCFG ` also gets swallowed.
5. **Daemon detail.** `Client.run()` passes a module-global `bridge` into
   `handle_binary()`, which does not use it — fragile but harmless.
   `_local_ip()` fallback and the duplicate import were fixed above.

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
