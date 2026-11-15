#!/usr/bin/env python3
"""Run commands on the u-boot prompt and print the output.

Usage: uboot_cmd.py [--port /dev/ttyACM1] [--baud N] "cmd1" "cmd2" ...
"""
import argparse
import sys
import time

import serial

PROMPT = b'=> '


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM1')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--timeout', type=float, default=30.0)
    ap.add_argument('cmds', nargs='+')
    args = ap.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.05)
    s.reset_input_buffer()
    s.write(b'\r')
    time.sleep(0.3)
    s.read(65536)

    for c in args.cmds:
        s.reset_input_buffer()
        s.write(c.encode() + b'\r')
        buf = bytearray()
        end = time.time() + args.timeout
        while time.time() < end:
            d = s.read(65536)
            if d:
                buf.extend(d)
                if buf.rstrip().endswith(PROMPT.strip()):
                    break
        print('### %s' % c)
        print(bytes(buf).decode('utf-8', 'replace'))
    s.close()


if __name__ == '__main__':
    main()
