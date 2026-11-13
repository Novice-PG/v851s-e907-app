# Deploying to the board

The board is a V851S running Tina Linux with a 64 MB DDR / 128 MB NAND and
`/mnt/UDISK` mounted from the data partition. Everything persistent lives
there (`/mnt/UDISK/persist`), because `/var` is tmpfs.

## Files and their destinations

| Repository file | Board path | Mode |
|---|---|---|
| `linkr-bee/a7/linkr-bee-daemon` | `/usr/bin/linkr-bee-daemon` | 0755 |
| `linkr-bee/a7/linkr-ble` | `/usr/bin/linkr-ble` | 0755 |
| `linkr-bee/a7/ble-adv-data.py` | `/usr/bin/ble-adv-data.py` | 0755 |
| `linkr-bee/a7/S53linkr` | `/etc/init.d/S53linkr` | 0755 |
| `linkr-bee/a7/S54bluetooth` | `/etc/init.d/S54bluetooth` | 0755 |
| `linkr-bee/a7/main.conf` | `/etc/bluetooth/main.conf` | 0644 |
| `linkr-bee/a7/certs/san.cnf` | `/usr/share/linkr-bee/certs/san.cnf` | 0644 |
| `linkr-bee/web/*` | `/usr/share/linkr-bee/web/` | as-is |

`S99linkrbee.legacy` is kept for reference only; the current boot path is
`S53linkr` (LAN daemon) plus `S54bluetooth` (XR829 + bluetoothd + linkr-ble).

## Stopping services before replacing files

```sh
/etc/init.d/S53linkr stop
killall linkr-ble 2>/dev/null
```

To restart the E907 (for example after replacing the firmware):

```sh
echo stop  > /sys/class/remoteproc/remoteproc0/state
sleep 8
echo start > /sys/class/remoteproc/remoteproc0/state
```

## Over SSH

```sh
B=root@<board-ip>
scp linkr-bee/a7/linkr-bee-daemon  $B:/usr/bin/
scp linkr-bee/a7/linkr-ble        $B:/usr/bin/
scp -r linkr-bee/web/*            $B:/usr/share/linkr-bee/web/
```

## Over the UART console

When SSH is not reachable, use the serial console (`/dev/ttyACM0` on the host
is the board console, `/dev/ttyUSB0` is UART3). Paste a base64 blob:

```sh
base64 -w0 linkr-bee/a7/linkr-bee-daemon > /tmp/b64
# on the board console:
base64 -d > /usr/bin/linkr-bee-daemon <<'EOF'
<contents of /tmp/b64>
EOF
chmod 0755 /usr/bin/linkr-bee-daemon
```

`sz`/`rz` (ZMODEM) also works if `busybox` was built with it.

## E907 firmware

`firmware/zephyr_e907.elf` is built from `src/`. On the board the remoteproc
controller is under debugfs:

```sh
mount -t debugfs none /sys/kernel/debug
RP=/sys/kernel/debug/remoteproc/remoteproc0
/etc/init.d/S53linkr stop
killall linkr-ble
echo stop > $RP/state && sleep 8
cp zephyr_e907.elf /lib/firmware/e907_firmware.elf
cp zephyr_e907.elf /lib/firmware/zephyr.elf
echo e907_firmware.elf > $RP/firmware
echo start > $RP/state
/etc/init.d/S53linkr start
/etc/init.d/S54bluetooth restart
```

A failed start leaves the state at `offline (0)`; the old firmware is still in
`/mnt/UDISK/backup-*/` if you kept a copy. `cat $RP/trace0` shows the RAM
console.

## HTTPS (self-signed, port 8443)

The board serves the page on both listeners:

| Listener | Purpose |
|---|---|
| `http://<board>/` (80) | redirects page requests to HTTPS; `/ws` still works over plain `ws://` for scripts |
| `https://<board>:8443/` | the page, `wss://<board>:8443/ws`, secure context (Web Bluetooth) |

HTTPS is enabled automatically by `S53linkr` when both
`/usr/share/linkr-bee/certs/linkr-bee.crt` and `.key` exist; without them the
daemon stays HTTP-only. The certificate template
(`linkr-bee/a7/certs/san.cnf`) already contains the board's addresses.

Generate a certificate per board (the key is never committed):

```sh
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 825 \
  -keyout linkr-bee.key -out linkr-bee.crt \
  -config linkr-bee/a7/certs/san.cnf -extensions v3
install -d -m 700 /usr/share/linkr-bee/certs        # on the board
install -m 644 linkr-bee.crt /usr/share/linkr-bee/certs/
install -m 600 linkr-bee.key /usr/share/linkr-bee/certs/
/etc/init.d/S53linkr restart
```

Because the certificate is self-signed, the browser shows a warning the first
time; accept it once per device. Open **`https://<board-ip>:8443/`** and leave
the device field at the default (the board fills in its own IP). The page
derives `wss://<board-ip>:8443/ws` from its own origin, so a bare IP is enough;
a full `wss://...` URL is also accepted.

On Linux Chromium, Web Bluetooth additionally needs
`chrome://flags/#enable-experimental-web-platform-features` and a restart.

If you ever want HTTP-only again, remove the certificate (or set
`TLS_PORT=0` before `S53linkr start`); plain `http://<board>/` then works as
before.

## Verifying

```sh
./linkr-bee/verify.sh /path/to/mounted/rootfs   # whole tree
```

On the board:

```sh
md5sum /usr/bin/linkr-bee-daemon /usr/bin/linkr-ble \
       /etc/init.d/S53linkr /etc/init.d/S54bluetooth
hciconfig hci0; pidof bluetoothd linkr-ble
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1/
```

Expected md5 values are listed in `linkr-bee/verify.sh`.
