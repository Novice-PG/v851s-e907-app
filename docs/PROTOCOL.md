# Protocol

Three links are involved: A7 ↔ E907 over rpmsg, A7 ↔ BLE host over a Unix
socket, and browser ↔ A7 over WebSocket.

## 1. rpmsg: A7 ↔ E907

The E907 opens an rpmsg endpoint. Text lines in both directions:

| Direction | Line | Meaning |
|---|---|---|
| A7 → E907 | `LINKR1\n` | handshake; tells the E907 the A7 endpoint address |
| A7 → E907 | `LINKRCFG <baud>,<data>,<parity>,<stop>,<flow>\n` | reprogram UART3 |
| A7 → E907 | `LINKRCFG ?\n` | report the active UART3 configuration |
| A7 → E907 | `LINKRSTAT\n` | report ring occupancy and drop counters |
| E907 → A7 | `LINKRSTAT rx=<used>/<size> tx=<used>/<size> drop=<rx>/<tx>\n` | current counters |
| E907 → A7 | `LINKRCFG OK uart=<baud>,<data>,<parity>,<stop>,<flow>\n` | applied |
| E907 → A7 | `LINKRCFG ERR=<errno>\n` | rejected (e.g. `-22` invalid) |

`data` is 5–8, `parity` is `n`/`e`/`o`, `stop` is 1/2, `flow` is `n` or
`rtscts`. Baud range is 300–4,000,000. Control lines are consumed on the E907
and never written to UART3. The A7 daemon strips `LINKRCFG ...` replies from
the serial stream, so the browser only sees the target console.

The E907 keeps 64 KiB rings in each direction (`.bss`, no NAND cost), so a
1.5 Mbaud burst can stall the A7 for ~437 ms before bytes are dropped.  The
`LINKRSTAT` counters are what `@i?` reports as `buffer=` and `dropped=`; the
A7 caches them for two seconds so a diagnostics panel refresh does not block
on the bridge.

Everything else on the endpoint is payload: bytes from the target board's
UART3 are forwarded verbatim to the A7, and bytes written by the A7 go to
UART3.

## 2. A7 daemon ↔ linkr-ble: Unix socket

`/var/run/linkr-bee.sock`. Each frame:

```
type   u8
length u16 (little endian)   payload length
payload[length]
```

| Type | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | DATA | both | serial bytes |
| `0x02` | CTL_REQ | BLE → daemon | `reqid u32 LE, flags u16 LE, text` |
| `0x03` | CTL_RSP | daemon → BLE | `reqid u32 LE, flags u16 LE, text` |
| `0x10` | HELLO | BLE → daemon | adapter/name info |
| `0x11` | NAME | daemon → BLE | new advertised name (UTF-8, ≤29 bytes) |
| `0x12` | EVENT | daemon → BLE | `reqid u32 LE, flags u16 LE, text` (async) |

`flags` bit 0 means "final", bit 1 means "error". Control text is the same
`@...` command language the WebSocket uses (`@c?`, `@c claim`, `@u?`, `@n=`,
`@s?`, …).

## 3. Browser ↔ daemon: WebSocket

`ws://<board>/ws` (or `wss://` when a certificate is installed). Binary frames
carry serial bytes; text frames carry `@...` session commands.

On connect the daemon sends:

```
@ws auth=required      (a token is configured)
@ws auth=none          (no token)
```

With no token file the bridge is open (`@ws auth=none`), which is the factory
default.  Set one with `@s token` (random) or `@s token=<32 hex>`; new
connections then get `@ws auth=required` and are closed unless they send the
token within 3 s. `@s token off` removes it again. The token is stored (mode
600) on the board at
`/mnt/UDISK/persist/ws-token` and managed with `@s token=<32 hex>`,
`@s token off` and `@s?`.

Session control is shared by up to four clients:

- a client is **claimed** on its first serial write, or explicitly with
  `@c claim`;
- the owner is preempted after 3 s of inactivity (`OWNER_IDLE_SECONDS`);
- `@c release` drops ownership and marks the client as released, so it is not
  auto-claimed again (this mirrors `linkr_session_ticket` upstream);
- BLE is a shared bridge: it never marks itself as `managed`, so a BLE write
  can claim an idle console but a LAN client's explicit release stays
  released.
