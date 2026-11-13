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
