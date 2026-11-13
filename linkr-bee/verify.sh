#!/bin/sh
#
# Verify the Linkr Bee tree and, optionally, a board root filesystem.
#
#   ./linkr-bee/verify.sh                    # md5 of this repository
#   ./linkr-bee/verify.sh /path/to/rootfs    # also check installed files
#
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
cd "$ROOT"

fail=0

echo "== repository =="
if md5sum -c --quiet MANIFEST.sha256; then
	echo "all files match MANIFEST.sha256"
else
	fail=1
fi

if [ $# -ge 1 ]; then
	target=$1
	[ -d "$target" ] || { echo "no such directory: $target" >&2; exit 2; }
	echo
	echo "== installed tree: $target =="

	check() {
		src=$1; dst=$2
		if [ ! -e "$target/$dst" ]; then
			echo "MISS $dst"
			fail=1
			return
		fi
		a=$(md5sum "$src" | cut -d' ' -f1)
		b=$(md5sum "$target/$dst" | cut -d' ' -f1)
		if [ "$a" = "$b" ]; then
			echo "ok   $dst"
		else
			echo "DIFF $dst ($a != $b)"
			fail=1
		fi
	}

	check linkr-bee/a7/linkr-bee-daemon usr/bin/linkr-bee-daemon
	check linkr-bee/a7/linkr-ble         usr/bin/linkr-ble
	check linkr-bee/a7/ble-adv-data.py   usr/bin/ble-adv-data.py
	check linkr-bee/a7/S53linkr          etc/init.d/S53linkr
	check linkr-bee/a7/S54bluetooth      etc/init.d/S54bluetooth
	check linkr-bee/a7/main.conf         etc/bluetooth/main.conf

	for f in $(cd linkr-bee/web && find . -type f); do
		check "linkr-bee/web/$f" "usr/share/linkr-bee/web/${f#./}"
	done
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "verify: all checks passed"
else
	echo "verify: differences found" >&2
fi
exit "$fail"
