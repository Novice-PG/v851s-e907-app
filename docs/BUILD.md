# Building

## E907 firmware

The application is a normal Zephyr out-of-tree app. The board, DTS and SoC
support come from the Zephyr `v851s` BSP branch (see the main project's
`west.yml`); nothing vendor-specific is vendored here.

```sh
export ZEPHYR_BASE=/path/to/zephyr      # branch with the V851S BSP
cmake -B build -GNinja -DBOARD=v851s_e907 /path/to/v851s-e907-app
ninja -C build
```

From a west workspace the same build is
`west build -b v851s_e907 /path/to/v851s-e907-app`.

Expected artefacts:

- `build/zephyr/zephyr_e907.elf` — `prj.conf` sets
  `CONFIG_KERNEL_BIN_NAME="zephyr_e907"`. Copy it to
  `firmware/zephyr_e907.elf`; the A7 side installs the same file as
  `/lib/firmware/e907_firmware.elf` (the name `start_e907` writes to the
  remoteproc `firmware` attribute);
- the `.resource_table` section must be present, otherwise Linux refuses to
  start the remote processor.

Key configuration (`prj.conf`): OpenAMP in slave mode, MSGBOX/IPM transport,
RAM console (UART3 belongs to the bridge), XIP off, `CONFIG_KERNEL_BIN_NAME`
set so the ELF name matches the firmware path.

The E907 runs from the 4 MiB DRAM window at `0x43c00000`; vrings live at
`0x43040000`/`0x43060000`. UART3 is at `0x02500c00`, default 115200 8N1.

## A7 binaries

### linkr-bee-daemon

Plain Python 3, standard library only:

```sh
python3 -m py_compile linkr-bee/a7/linkr-bee-daemon
```

Install as `/usr/bin/linkr-bee-daemon` (mode 0755). It is run by
`/etc/init.d/S53linkr`:

```sh
python3 -u /usr/bin/linkr-bee-daemon --port 80 --tls-port 8443 \
        --cert /mnt/UDISK/persist/certs/linkr-bee.crt \
        --key /mnt/UDISK/persist/certs/linkr-bee.key \
        --web-root /usr/share/linkr-bee/web
```

`S53linkr` selects the certificate automatically and passes 8443; with no
certificate it starts the daemon without TLS (HTTP only). Web Bluetooth needs
a secure context, so install a certificate (see below) to use it.

### linkr-ble

BlueZ 5.54 GATT server, needs the SDK's cross toolchain and staging sysroot:

```sh
export TINA_SDK=/path/to/tina-v853-open
./linkr-bee/build-linkr-ble.sh
# -> linkr-bee/a7/linkr-ble
```

The script locates the toolchain, sets `STAGING_DIR`, and links against the
staging `glib-2.0`/`gio-2.0` plus their transitive dependencies (`gmodule`,
`ffi`, `z`). `linkr-bee/a7/linkr-ble` in the repository is the binary that
runs on the board; rebuild it after changing `linkr_ble.c`.

A rebuild is functionally equivalent but not byte-identical to the committed
binary (the SDK staging tree is not hermetic: link order, libc version and
the toolchain build path end up in the ELF). Verify behaviour on hardware
rather than by md5 after a rebuild.

### Web assets

```sh
./linkr-bee/apply-web.sh /path/to/linkr-bee-checkout
# installs into /usr/share/linkr-bee/web
```

The five patches are against [radxa/linkr-bee](https://github.com/radxa/linkr-bee)
commit `d9b2e00`. `linkr-bee/web/` is the complete patched tree as installed on
the board, so the board does not need a checkout at runtime.

### Certificates

The HTTPS key is never committed. Generate one per board:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 825 \
  -keyout linkr-bee.key -out linkr-bee.crt \
  -config linkr-bee/a7/certs/san.cnf -extensions v3
```

Copy both into `/mnt/UDISK/persist/certs/` on the board (preferred: survives a
rootfs reflash, and keeps the key out of any image) or into
`/usr/share/linkr-bee/certs/`. `S53linkr` then starts the daemon with
`--tls-port 8443` automatically; without the pair it serves HTTP only. Set
`TLS_PORT` to change the port.
