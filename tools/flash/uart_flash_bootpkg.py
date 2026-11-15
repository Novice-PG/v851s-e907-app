#!/usr/bin/env python3
"""Flash the new boot_package.fex into the V851S SPI NAND from the u-boot shell.

Why this exists: the board's storage is 128 MB SPI NAND and the local
sunxi-fel only implements SPI NOR access, so FEL cannot write it.  But u-boot
has CONFIG_SUNXI_SPRITE=y and `sunxi_flash write <addr> uboot`, which calls
spinand_mtd_download_uboot() - the very same function PhoenixSuit uses for
the boot chain.  From the 128 MB NAND dump we know the boot chain area is:

    0x100000 / 0x200000 / 0x300000   three identical copies of boot_package.fex

so writing the new 966656-byte package (472 x 2048, 8 x 128K blocks) there
replaces the patched u-boot, the new kernel DTB and OP-TEE in one go.

Flow:
  1. spam CR until the u-boot prompt appears (bootdelay=1)
  2. ymodem (loady) the package to RAM
  3. verify crc32 in u-boot against the host value
  4. sunxi_flash write <addr> uboot
  5. setenv boot_normal/boot_recovery + saveenv
  6. reset and capture the boot log

Usage:
  uart_flash_bootpkg.py --port /dev/ttyACM1 --file boot_package.fex \
      --log /tmp/opencode/flash.log
"""
import argparse
import os
import sys
import time
import zlib

import serial

PROMPT = b'=> '
CAN = 0x18
ACK = 0x06
NAK = 0x15
STX = 0x02
EOT = 0x04

# ---------------------------------------------------------------- crc16/xmodem
def _mk_table():
    t = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        t.append(crc)
    return t


CRC_TABLE = _mk_table()


def crc16(data):
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ CRC_TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc


class Console:
    def __init__(self, port, baud, logpath):
        self.port = port
        self.baud = baud
        self.raw = open(logpath + '.raw', 'wb', buffering=0)
        self.echo = open(logpath, 'w', buffering=1)
        self.buf = bytearray()
        self._open()

    def _open(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.02)
        self.ser.reset_input_buffer()

    def _reopen(self):
        try:
            self.ser.close()
        except Exception:
            pass
        while True:
            try:
                self._open()
                print('\n[uart re-enumerated, reopened %s]\n' % self.port, flush=True)
                return
            except Exception:
                time.sleep(0.5)

    def close(self):
        try:
            self.ser.close()
        finally:
            self.raw.close()
            self.echo.close()

    def pump(self):
        try:
            d = self.ser.read(65536)
        except (serial.SerialException, OSError):
            self._reopen()
            return b''
        if d:
            self.buf += d
            self.raw.write(d)
            txt = d.decode('utf-8', 'replace')
            self.echo.write(txt)
            sys.stdout.write(txt)
            sys.stdout.flush()
        return d

    def send(self, data):
        try:
            self.ser.write(data)
            self.ser.flush()
        except (serial.SerialException, OSError):
            self._reopen()
            try:
                self.ser.write(data)
                self.ser.flush()
            except Exception:
                pass

    def clear(self):
        self.buf.clear()

    def wait_for(self, needles, timeout=30.0, spam=None, spam_interval=0.04,
                 forbidden=None):
        """Return the matched needle, 'FORBIDDEN:<x>' or None on timeout."""
        deadline = time.time() + timeout
        last = 0.0
        while time.time() < deadline:
            if spam is not None and time.time() - last >= spam_interval:
                self.send(spam)
                last = time.time()
            self.pump()
            for n in needles:
                if n in self.buf:
                    return n
            if forbidden:
                for n in forbidden:
                    if n in self.buf:
                        return b'FORBIDDEN:' + n
        return None

    def wait_prompt(self, timeout=30.0):
        self.clear()
        self.send(b'\r')
        return self.wait_for([PROMPT], timeout) == PROMPT

    def cmd(self, line, timeout=60.0):
        self.clear()
        self.send(line.encode() + b'\r')
        return self.wait_for([PROMPT], timeout) == PROMPT


# ------------------------------------------------------------------- ymodem tx
class Ymodem:
    def __init__(self, con):
        self.con = con

    def _block(self, seq, data):
        assert len(data) == 1024
        c = crc16(data)
        return bytes([STX, seq & 0xFF, 0xFF - (seq & 0xFF)]) + data + bytes([c >> 8, c & 0xFF])

    def _wait_reply(self, timeout=8.0):
        """Return 'ACK', 'NAK', 'CAN', 'C' or None."""
        deadline = time.time() + timeout
        seen_any = False
        while time.time() < deadline:
            self.con.pump()
            b = self.con.buf
            if CAN in b:
                return 'CAN'
            if ACK in b:
                self.con.clear()
                return 'ACK'
            if NAK in b:
                self.con.clear()
                return 'NAK'
            if b'C' in b:
                seen_any = True
            time.sleep(0.005)
        self.con.clear()
        return 'C' if seen_any else None

    def send(self, filename, payload, verbose=True):
        size = len(payload)
        # 1) wait for the receiver's 'C'
        if verbose:
            print('waiting for receiver C ...', flush=True)
        t0 = time.time()
        while time.time() - t0 < 30:
            if self._wait_reply(3.0) == 'C':
                break
        else:
            raise SystemExit('receiver never sent C')

        # 2) header block: filename + size
        hdr = (filename.encode() + b'\0' + str(size).encode() + b' 0 0 0').ljust(1024, b'\0')
        if not self._send_block(0, hdr):
            raise SystemExit('header block rejected')

        # 3) data blocks
        seq = 1
        off = 0
        nblk = (size + 1023) // 1024
        t0 = time.time()
        while off < size:
            chunk = payload[off:off + 1024]
            if len(chunk) < 1024:
                chunk = chunk + b'\x1a' * (1024 - len(chunk))
            if not self._send_block(seq, chunk):
                raise SystemExit('block %d rejected' % seq)
            off += 1024
            seq = (seq + 1) & 0xFF
            if verbose and (seq % 64 == 0):
                el = time.time() - t0
                print('  %d/%d blocks  %.1f KB/s' % (seq - 1, nblk,
                      (off / 1024.0) / max(el, 0.001)), flush=True)

        # 4) EOT + final empty header
        for attempt in range(5):
            self.con.clear()
            self.con.send(bytes([EOT]))
            r = self._wait_reply(6.0)
            if r in ('ACK', 'C'):
                break
        else:
            raise SystemExit('EOT not acknowledged')

        empty = b'\0' * 1024
        for attempt in range(5):
            self.con.clear()
            self.con.send(self._block(0, empty))
            if self._wait_reply(6.0) == 'ACK':
                break
        else:
            raise SystemExit('final header not acknowledged')

    def _send_block(self, seq, data):
        pkt = self._block(seq, data)
        for attempt in range(12):
            self.con.clear()
            self.con.send(pkt)
            r = self._wait_reply(8.0)
            if r == 'ACK':
                return True
            if r == 'CAN':
                raise SystemExit('receiver cancelled')
        return False


# ------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM1')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--file', required=True, help='boot_package.fex to write')
    ap.add_argument('--addr', default='0x40600000')
    ap.add_argument('--log', default='/tmp/opencode/flash.log')
    ap.add_argument('--prompt-timeout', type=float, default=300)
    ap.add_argument('--no-write', action='store_true', help='stop after crc verify')
    ap.add_argument('--no-reset', action='store_true')
    ap.add_argument('--reboot', action='store_true',
                    help='send "reboot" first (board is at the Linux shell)')
    ap.add_argument('--verify-readback', action='store_true',
                    help='after the write, read the uboot partition back and crc32 it')
    args = ap.parse_args()

    payload = open(args.file, 'rb').read()
    host_crc = zlib.crc32(payload) & 0xFFFFFFFF
    print('file %s  %d bytes  crc32 %08x' % (args.file, len(payload), host_crc), flush=True)

    con = Console(args.port, args.baud, args.log)
    try:
        if args.reboot:
            print('=== [0] requesting reboot from the Linux shell ===', flush=True)
            con.send(b'\r')
            time.sleep(0.3)
            con.send(b'reboot\r')
            time.sleep(0.5)
            con.clear()
        print('=== [1] waiting for u-boot prompt (power the board now) ===', flush=True)
        r = con.wait_for([PROMPT, b'U-Boot 2018'], args.prompt_timeout,
                         spam=b'\r', spam_interval=0.04,
                         forbidden=[b'Kernel panic'])
        if r != PROMPT and r != b'U-Boot 2018':
            raise SystemExit('no u-boot prompt: %r' % (r,))
        # keep spamming briefly so autoboot really aborts
        t0 = time.time()
        while time.time() - t0 < 1.5:
            con.send(b'\r')
            con.pump()
            time.sleep(0.05)
        con.clear()
        con.send(b'\r')
        if con.wait_for([PROMPT], 10) != PROMPT:
            raise SystemExit('u-boot shell did not answer')
        print('=== u-boot shell is live ===', flush=True)
        con.cmd('setenv bootdelay 3')

        print('=== [2] loady %s ===' % args.addr, flush=True)
        con.clear()
        con.send(('loady %s\r' % args.addr).encode())
        if con.wait_for([b'C'], 20) is None:
            raise SystemExit('loady did not start')
        Ymodem(con).send(os.path.basename(args.file), payload)
        if not con.wait_prompt(30):
            raise SystemExit('no prompt after transfer')
        print('=== transfer done ===', flush=True)

        print('=== [3] verify crc32 in RAM ===', flush=True)
        con.clear()
        con.send(('crc32 %s %x\r' % (args.addr, len(payload))).encode())
        con.wait_for([PROMPT], 30)
        txt = bytes(con.buf).decode('utf-8', 'replace')
        ok = ('%08x' % host_crc) in txt.lower()
        print(txt.strip()[-300:], flush=True)
        if not ok:
            raise SystemExit('CRC MISMATCH - nothing written')
        print('=== crc32 matches %08x ===' % host_crc, flush=True)

        if args.no_write:
            print('--no-write: stopping before NAND write', flush=True)
            return 0

        print('=== [4] sunxi_flash write %s uboot ===' % args.addr, flush=True)
        con.clear()
        con.send(('sunxi_flash write %s uboot\r' % args.addr).encode())
        if con.wait_for([PROMPT], 240) != PROMPT:
            raise SystemExit('write command did not return')
        print(bytes(con.buf).decode('utf-8', 'replace')[-1500:], flush=True)

        if args.verify_readback:
            print('=== [4b] read the uboot partition back and crc32 copy 1 ===', flush=True)
            con.clear()
            con.send(('sunxi_flash read %s uboot\r' % args.addr).encode())
            if con.wait_for([PROMPT], 120) != PROMPT:
                print('read-back did not return', flush=True)
            con.clear()
            con.send(('crc32 %s %x\r' % (args.addr, len(payload))).encode())
            con.wait_for([PROMPT], 60)
            txt = bytes(con.buf).decode('utf-8', 'replace')
            print(txt.strip()[-300:], flush=True)
            if ('%08x' % host_crc) in txt.lower():
                print('=== read-back crc32 matches %08x ===' % host_crc, flush=True)
            else:
                print('=== WARNING: read-back crc32 does not match ===', flush=True)

        print('=== [5] env: boot load address 0x40800000 ===', flush=True)
        con.cmd("setenv boot_normal 'sunxi_flash read 40800000 ${boot_partition};bootm 40800000'")
        con.cmd("setenv boot_recovery 'sunxi_flash read 40800000 extend;bootm 40800000'")
        con.cmd('setenv bootdelay 1')
        con.cmd('saveenv')
        con.clear()
        con.send(b'printenv boot_normal\r')
        con.wait_for([PROMPT], 20)
        print(bytes(con.buf).decode('utf-8', 'replace')[-400:], flush=True)

        if not args.no_reset:
            print('=== [6] reset ===', flush=True)
            con.clear()
            con.send(b'reset\r')
            time.sleep(60)          # capture the boot log
            con.pump()
        return 0
    finally:
        con.close()


if __name__ == '__main__':
    sys.exit(main())
