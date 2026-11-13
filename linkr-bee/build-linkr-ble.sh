#!/bin/sh
#
# Cross-build the BlueZ GATT server (linkr-ble) for the V851S.
#
#   ./linkr-bee/build-linkr-ble.sh [path/to/tina-v853-open]
#
# The SDK is only needed for its cross toolchain and staging sysroot; no SDK
# source is copied into this repository. Output: linkr-bee/a7/linkr-ble
#
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SDK=${1:-${TINA_SDK:-}}
[ -n "$SDK" ] || { echo "usage: $0 /path/to/tina-v853-open (or set TINA_SDK)" >&2; exit 2; }

STAGE="$SDK/out/v851s/lizard/openwrt/staging_dir"
[ -d "$STAGE" ] || { echo "no staging dir at $STAGE (build the SDK once first)" >&2; exit 2; }

CC=$(ls "$STAGE"/toolchain/bin/*-gcc-* 2>/dev/null | head -1)
[ -n "$CC" ] || { echo "no cross gcc under $STAGE/toolchain/bin" >&2; exit 2; }

OUT=${OUT:-$HERE/a7/linkr-ble}

echo "compiler: $CC"
STAGING_DIR="$STAGE" "$CC" -Os -o "$OUT" "$HERE/a7/linkr_ble.c" \
	-I"$STAGE/target/usr/include/glib-2.0" \
	-I"$STAGE/target/usr/lib/glib-2.0/include" \
	-I"$STAGE/target/usr/include" \
	-L"$STAGE/target/usr/lib" \
	-Wl,-rpath-link,"$STAGE/target/usr/lib" \
	-lgio-2.0 -lgobject-2.0 -lgmodule-2.0 -lglib-2.0 -lffi -lz

STRIP=$(ls "$STAGE"/toolchain/bin/*-strip 2>/dev/null | head -1)
[ -n "$STRIP" ] && "$STRIP" "$OUT"

chmod 0755 "$OUT"
ls -l "$OUT"
md5sum "$OUT"
