#!/usr/bin/env python3
"""Push LE advertising data straight to the XR829 controller.

Allwinner's 4.9 kernel comments out both HCI_OP_LE_SET_SCAN_RSP_DATA
(hci_request.c:1078) and HCI_OP_LE_SET_ADV_DATA (hci_request.c:1172), so the
data BlueZ hands to the kernel via MGMT Add Advertising never reaches the
controller -> empty LE advertisement -> a Web Bluetooth chooser filtering on
the Management Service UUID cannot find the board.

This script issues HCI 0x0008 / 0x0009 directly. Must run AFTER linkr-ble
registers its advertisement (so our values are not replaced), and again after
a name change or adapter power cycle.
"""
import subprocess
import sys

UUID_LE = bytes.fromhex("01c0a0126f3d8a8b4e4f7e9a01004b4c")  # 4c4b0001-...
NAME_FILE = "/mnt/UDISK/persist/ble-name"
DEFAULT_NAME = "Linkr Bee V851S"


def send(ocf, payload):
    data = bytes([len(payload)]) + payload
    data = data.ljust(32, b"\x00")           # 1 len byte + 31 data bytes
    cmd = ["/usr/bin/hcitool", "-i", "hci0", "cmd", "0x08", ocf]
    cmd += ["%02x" % b for b in data]
    r = subprocess.run(cmd, capture_output=True, timeout=5)
    return r.returncode == 0


def main():
    try:
        name = open(NAME_FILE, "rb").read(29).decode("utf-8")
        if not name:
            name = DEFAULT_NAME
    except (OSError, UnicodeDecodeError):
        name = DEFAULT_NAME

    ad = b"\x02\x01\x06" + b"\x11\x07" + UUID_LE
    srd = bytes([len(name.encode()) + 1, 0x09]) + name.encode()

    ok = send("0x0008", ad) and send("0x0009", srd)
    print("AD/SRD pushed: %s (name=%r, ad=%dB, srd=%dB)" %
          ("ok" if ok else "FAILED", name, len(ad), len(srd)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
