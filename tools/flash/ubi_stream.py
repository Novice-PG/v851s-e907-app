#!/usr/bin/env python3
"""Robust host side of the ubi_recv protocol (see tools/ubi_recv.c).

Starts ``ubi_recv`` on the board at 115200, waits until it reports that it is
ready for the magic, switches to 1.5 Mbaud, streams the file with XON/XOFF
flow control and verifies the byte count.

Unlike the older uart_ubi_send.py this keeps the board's startup output, so a
failure (EBUSY, missing device, ...) is reported instead of silently turning
into garbage on the shell prompt.

The board-side helper (``ubi_recv``, built from ``ubi_recv.c`` next to this
file) must already be on the board; ``--binary`` defaults to
``/mnt/UDISK/tools/ubi_recv``.

Usage:
    ubi_stream.py --port /dev/ttyACM1 --src boot-rescue.img --vol /dev/ubi0_3
    ubi_stream.py --port /dev/ttyACM1 --src rootfs.new --vol /dev/ubi0_4 \
        --size 36241408
"""
import argparse
import hashlib
import os
import sys
import time

import serial

MAGIC = b'OPENUBI1'


def md5_file(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def read_until(ser, needles, timeout):
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        d = ser.read(65536)
        if d:
            buf.extend(d)
            for n in needles:
                if n in buf:
                    return bytes(buf), n
    return bytes(buf), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM1')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--fast', type=int, default=1500000)
    ap.add_argument('--src', required=True)
    ap.add_argument('--vol', required=True)
    ap.add_argument('--size', type=int)
    ap.add_argument('--chunk', type=int, default=8192)
    ap.add_argument('--binary', default='/mnt/UDISK/tools/ubi_recv')
    args = ap.parse_args()

    fsize = os.path.getsize(args.src)
    size = args.size if args.size is not None else fsize
    if size > fsize:
        sys.exit('--size larger than file')
    print('[ubi] %s -> %s (%d bytes, md5 %s)'
          % (args.src, args.vol, size, md5_file(args.src)), file=sys.stderr)

    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    ser.reset_input_buffer()
    for _ in range(3):
        ser.write(b'\x03')
        time.sleep(0.08)
    ser.write(b'\r')
    time.sleep(0.4)
    ser.read(65536)

    ser.write(('%s %s /dev/ttyS0 %d\r' % (args.binary, args.vol, size)).encode())
    ser.flush()

    # ubi_recv switches the tty to 1.5 Mbaud before it prints anything, so at
    # 115200 we only see the shell echo plus any error it reports *before* the
    # switch (open volume / UBI_IOCVOLUP / not found).
    out, _ = read_until(ser, [b'open volume', b'UBI_IOCVOLUP', b'not found'], 1.5)
    text = out.decode('utf-8', 'replace').replace('\r', '')
    print('[ubi] board: %s' % text.strip().splitlines()[-3:], file=sys.stderr)
    for bad in ('open volume', 'UBI_IOCVOLUP', 'not found'):
        if bad.encode() in out:
            sys.exit('[ubi] ubi_recv failed to start:\n' + text)

    ser.baudrate = args.fast
    time.sleep(0.1)

    ready = False
    buf = bytearray()
    end = time.time() + 15
    while time.time() < end:
        ser.write(MAGIC)
        ser.flush()
        d, mark = read_until(ser, [b'@@READY@@'], 0.25)
        buf.extend(d)
        if mark:
            ready = True
            break
    if not ready:
        sys.exit('[ubi] no @@READY@@, saw %r' % bytes(buf[-200:]))

    # Bytes sent at the old speed may still be queued on the board side; the
    # board flushes them itself right after @@READY@@.
    time.sleep(0.05)
    ser.reset_input_buffer()

    sent = 0
    t0 = time.time()
    with open(args.src, 'rb') as f:
        while sent < size:
            chunk = f.read(min(args.chunk, size - sent))
            if not chunk:
                break
            ser.write(chunk)
            sent += len(chunk)
            if sent % (4 << 20) < args.chunk:
                print('[ubi] %d/%d (%.1f KB/s)'
                      % (sent, size, sent / 1024.0 / max(time.time() - t0, 1e-9)),
                      file=sys.stderr)
    ser.flush()
    dt = time.time() - t0
    print('[ubi] sent %d bytes in %.1fs (%.1f KB/s)'
          % (sent, dt, sent / 1024.0 / max(dt, 1e-9)), file=sys.stderr)

    tail, mark = read_until(ser, [b'@@GOT'], 300)
    print('[ubi] ack: %r' % tail[-160:], file=sys.stderr)
    ok = mark is not None and ('@@GOT %d@@' % size).encode() in tail

    # Board keeps 1.5 Mbaud after exit; talk to it at that speed to restore.
    ser.write(b'\r')
    time.sleep(0.3)
    ser.write(b'stty -F /dev/ttyS0 %d\r' % args.baud)
    ser.flush()
    time.sleep(0.5)
    ser.write(b'\r')
    time.sleep(0.3)
    ser.read(65536)
    ser.close()

    if not ok:
        sys.exit('[ubi] transfer failed')
    print('[ubi] OK', file=sys.stderr)


if __name__ == '__main__':
    main()
