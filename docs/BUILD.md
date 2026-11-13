# Building

## E907 firmware

The application is a normal Zephyr out-of-tree app. The board, DTS and SoC
support come from the Zephyr `v851s` BSP branch (see the main project's
`west.yml`); nothing vendor-specific is vendored here.

```sh
export ZEPHYR_BASE=/path/to/zephyr          # branch with the V851S BSP
cd /path/to/apps                            # this repository's parent layout
west build -b yuzuki_lizard_v851s/e907 e907_bringup
```

Expected artefacts:

- `build/zephyr/zephyr.elf` — loaded by the A7 remoteproc driver from
  `/lib/firmware/e907_firmware.elf` (and `zephyr.elf`, a copy);
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
python3 -u /usr/bin/linkr-bee-daemon --port 80 --tls-port 0 \
        --web-root /usr/share/linkr-bee/web
```

`--tls-port 0` disables HTTPS. Web Bluetooth needs a secure context, so pass a
real port only after installing a certificate (see below).

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

The three patches are against [radxa/linkr-bee](https://github.com/radxa/linkr-bee)
commit `d9b2e00`. `linkr-bee/web/` is the complete patched tree as installed on
the board, so the board does not need a checkout at runtime.

### Certificates

The HTTPS key is never committed. Generate one per board:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 825 \
  -keyout linkr-bee.key -out linkr-bee.crt \
  -config linkr-bee/a7/certs/san.cnf -extensions v3
```

Copy both into `/usr/share/linkr-bee/certs/` on the board and start the daemon
with `--tls-port 443` (instead of 0).
