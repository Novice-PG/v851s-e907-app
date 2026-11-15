#!/usr/bin/env python3
"""Push a file to the V851S board over the debug UART, raw bytes, no base64.

The board rootfs has no base64/gzip/tar, but it does have python3, so we
bootstrap a tiny termios raw-mode receiver with a heredoc and then stream
the file straight down the wire.

Usage:
    uart_push.py --src busybox --dest /usr/bin/bb --mode 755
"""
import argparse
import hashlib
import os
import sys
import time

import serial

PROMPTS = [b'=> ', b'# ', b'$ ', b'login:']
READY = b'@@READY@@'
DONE = b'@@GOT'

RECV_PY = r'''
import os, sys, termios, tty
path = sys.argv[1]
want = int(sys.argv[2])
fd = 0
old = termios.tcgetattr(fd)
tty.setraw(fd)
f = open(path, 'wb')
os.write(1, b'@@READY@@\n')
got = 0
try:
    while got < want:
        b = os.read(fd, min(65536, want - got))
        if not b:
            break
        f.write(b)
        got += len(b)
finally:
    f.close()
    termios.tcsetattr(fd, termios.TCSADRAIN, old)
os.write(1, b'\n@@GOT %d@@\n' % got)
'''


class Link:
    def __init__(self, port, baud, verbose=False):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.ser.reset_input_buffer()
        self.verbose = verbose

    def close(self):
        self.ser.close()

    def _pump(self, sink):
        d = self.ser.read(65536)
        if d:
            sink.extend(d)
            if self.verbose:
                sys.stdout.write(d.decode('utf-8', 'replace'))
                sys.stdout.flush()
        return bool(d)

    def wait_for(self, needles, timeout):
        """Wait until any of `needles` appears in the incoming stream."""
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            self._pump(buf)
            for n in needles:
                if n in buf:
                    return n, bytes(buf)
            time.sleep(0.01)
        return None, bytes(buf)

    def sync(self, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            self.ser.write(b'\r')
            got, _ = self.wait_for(PROMPTS, 1.5)
            if got:
                return got
        return None

    def cmd(self, line, timeout=30.0, expect=PROMPTS):
        self.ser.write(line.encode() + b'\r')
        return self.wait_for(expect, timeout)


def md5_file(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def push(link, src, dest, mode=None, chunk=4096, pace=0.0, tries=3):
    size = os.path.getsize(src)
    want = md5_file(src)
    for attempt in range(1, tries + 1):
        print('[push] %s -> %s (%d bytes, md5 %s) try %d'
              % (src, dest, size, want, attempt), file=sys.stderr)

        # 1. install receiver (idempotent)
        link.ser.write(b"cat > /tmp/rx.py <<'PYEOF'\r")
        time.sleep(0.2)
        for line in RECV_PY.strip('\n').split('\n'):
            link.ser.write(line.encode() + b'\r')
            time.sleep(0.01)
        got, _ = link.cmd('PYEOF', timeout=10)
        if got is None:
            print('[push] receiver heredoc failed', file=sys.stderr)
            continue

        # 2. start receiver
        link.ser.reset_input_buffer()
        link.ser.write(('python3 -u /tmp/rx.py %s %d\r' % (dest, size)).encode())
        got, tail = link.wait_for([READY], 30)
        if got is None:
            print('[push] no READY, tail=%r' % tail[-300:], file=sys.stderr)
            continue

        # 3. stream the payload
        t0 = time.time()
        sent = 0
        with open(src, 'rb') as f:
            while sent < size:
                b = f.read(chunk)
                if not b:
                    break
                link.ser.write(b)
                link.ser.flush()
                sent += len(b)
                if pace:
                    time.sleep(pace)
        print('[push] sent %d bytes in %.1fs (%.1f KB/s)'
              % (sent, time.time() - t0, sent / 1024.0 / max(1e-6, time.time() - t0)),
              file=sys.stderr)

        # 4. wait for the receiver to acknowledge
        got, tail = link.wait_for([DONE], 60)
        if got is None:
            print('[push] no GOT ack, tail=%r' % tail[-300:], file=sys.stderr)
            continue
        line = tail[tail.rfind(DONE):].split(b'@@')[0]
        print('[push] receiver says: %s' % line.decode('utf-8', 'replace'), file=sys.stderr)

        # 5. verify on the board
        if mode:
            link.ser.write(('chmod %s %s\r' % (mode, dest)).encode())
            link.wait_for(PROMPTS, 10)
        link.ser.reset_input_buffer()
        link.ser.write(('md5sum %s\r' % dest).encode())
        got, tail = link.wait_for([want.encode(), b'No such', b'md5sum:'], 30)
        if got == want.encode():
            print('[push] md5 OK', file=sys.stderr)
            return True
        print('[push] md5 MISMATCH (board output %r)' % tail[-200:], file=sys.stderr)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM1')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--src', required=True)
    ap.add_argument('--dest', required=True)
    ap.add_argument('--mode', default=None)
    ap.add_argument('--chunk', type=int, default=4096)
    ap.add_argument('--pace', type=float, default=0.0)
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    link = Link(args.port, args.baud, args.verbose)
    try:
        if not link.sync():
            print('[push] no shell prompt', file=sys.stderr)
            return 2
        ok = push(link, args.src, args.dest, args.mode, args.chunk, args.pace)
    finally:
        link.close()
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
