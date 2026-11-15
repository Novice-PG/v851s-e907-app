# Serial flashing and rescue tools

Host-side helpers for writing images to the V851S over its debug UART.  They
complement PhoenixSuit/FEL: this board has SPI NAND, which the local
`sunxi-fel` cannot write, so U-Boot + a small receiver are the reliable
recovery path.  The full procedure is in `book/33-flashing-and-rescue.md`.

| File | Runs on | Purpose |
|---|---|---|
| `ubi_stream.py` | host | stream a file into a UBI volume (`ubi0_3`, `ubi0_4`, …) through `ubi_recv`; switches to 1.5 Mbaud after the handshake and verifies the byte count |
| `ubi_recv.c` | board | the receiver used by `ubi_stream.py`; opens the volume with `UBI_IOCVOLUP`, reads the payload from the tty with XON/XOFF flow control |
| `uart_push.py` | host | push any file to the board's filesystem over the console (bootstraps a tiny Python receiver, then md5-checks the result) |
| `uboot_enter.py` | host | reboot and catch the U-Boot prompt (spams CR during `bootdelay`) |
| `uboot_cmd.py` | host | run one or more commands at an already-open U-Boot prompt |
| `uart_flash_bootpkg.py` | host | YMODEM-upload a `boot_package.fex` to U-Boot, `sunxi_flash write` it, `saveenv`, reset (last-resort boot-chain repair) |

All host tools need Python 3 and `pyserial` (`pip install pyserial`).  The
default serial port is `/dev/ttyACM1` (the CH343P console on this machine);
pass `--port` for anything else.  The board side must be a root shell.

## Building `ubi_recv` for the board

`ubi_recv` is a static musl binary.  With the Tina SDK cross toolchain:

```sh
STAGE=/path/to/tina-v853-open/out/v851s/lizard/openwrt/staging_dir
CC=$STAGE/toolchain/bin/arm-openwrt-linux-muslgnueabi-gcc-8.3.0
STAGING_DIR=$STAGE $CC -O2 -static -o ubi_recv ubi_recv.c
scp -O ubi_recv root@<board>:/mnt/UDISK/tools/ubi_recv
```

The SDK gcc refuses to run without `STAGING_DIR`.

`ubi_stream.py --binary` defaults to `/mnt/UDISK/tools/ubi_recv`; the file
must be executable.

## Typical rescue use

```sh
# 1. put the rescue image into the boot volume (board is running Linux)
python3 ubi_stream.py --src boot-rescue.img --vol /dev/ubi0_3

# 2. reboot, catch U-Boot, boot the rescue system
python3 uboot_enter.py
python3 uboot_cmd.py 'setenv bootargs ubi.mtd=sys console=ttyS0,115200 loglevel=7' 'run boot_normal'
#    (do NOT saveenv: the temporary bootargs must not persist)

# 3. in the rescue shell, write the rootfs and the normal kernel
python3 ubi_stream.py --src rootfs.squashfs --vol /dev/ubi0_4
python3 ubi_stream.py --src boot-normal.img --vol /dev/ubi0_3
```

`ubi_stream.py` uses the console tty passed to `ubi_recv` as `/dev/ttyS0` and
assumes the Linux console is at 1.5 Mbaud when the receiver takes over
(`--baud` / `--fast` change both).  If your bootargs use another console
baud, keep the two in sync.

## Safety

- `--vol` is written blindly: `/dev/ubi0_3` is the boot volume and
  `/dev/ubi0_4` the rootfs.  Writing the wrong file to `ubi0_3` leaves the
  board unbootable until it is recovered over FEL/U-Boot.
- Always compare the md5 printed by `ubi_stream.py` with the one in the
  release `SHA256SUMS`/manifest before rebooting.
- These tools write images, not partitions tables; they cannot fix a broken
  boot0/U-Boot.  For that, use `uart_flash_bootpkg.py` (boot chain area at
  0x100000/0x200000/0x300000) or PhoenixSuit.
