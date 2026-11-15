#!/usr/bin/env python3
"""Reboot the board and catch the U-Boot prompt (spamming CR during bootdelay).

Assumes the Linux console is at 1.5 Mbaud (the Linkr Bee bootargs) and
U-Boot at 115200; pass --linux-baud / --baud for other setups.

Usage: uboot_enter.py [--port /dev/ttyACM1] [--baud 115200]
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM1')
    ap.add_argument('--linux-baud', type=int, default=1500000)
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--timeout', type=float, default=45.0)
    args = ap.parse_args()

    # 1) ask Linux to reboot at its current console baud
    s = serial.Serial(args.port, args.linux_baud, timeout=0.2)
    s.reset_input_buffer()
    for _ in range(3):
        s.write(b'\x03')
        time.sleep(0.1)
    s.write(b'\r')
    time.sleep(0.4)
    s.read(65536)
    s.write(b'sync; reboot\r')
    time.sleep(0.3)
    s.read(65536)
    s.close()

    # 2) catch u-boot
    s = serial.Serial(args.port, args.baud, timeout=0.02)
    s.reset_input_buffer()
    buf = bytearray()
    end = time.time() + args.timeout
    found = False
    while time.time() < end:
        s.write(b'\r')
        d = s.read(65536)
        if d:
            buf.extend(d)
            if b'=> ' in buf[-400:] or b'sunxi#' in buf[-400:]:
                found = True
                break
        time.sleep(0.02)
    s.close()
    txt = bytes(buf).decode('utf-8', 'replace')
    print(txt[-3000:])
    print('=== prompt found:', found)
    return 0 if found else 1


if __name__ == '__main__':
    sys.exit(main())
