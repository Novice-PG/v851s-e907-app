#!/bin/sh
# Apply the V851S linkr-bee web patches to a linkr-bee checkout.
#
#   ./apply-web.sh /path/to/linkr-bee
#
# Works on a real git checkout (uses `git apply`) or on a plain directory
# (falls back to `patch -p1`).  It never commits and never touches a remote.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
target=${1:-}
if [ -z "$target" ]; then
	echo "usage: $0 /path/to/linkr-bee" >&2
	exit 2
fi
if [ ! -f "$target/web/index.html" ] || [ ! -f "$target/web/app.js" ]; then
	echo "$target does not look like a linkr-bee checkout (web/ missing)" >&2
	exit 2
fi

cd "$target"

for p in "$here"/patches/*.patch; do
	name=$(basename "$p")
	if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		if git apply --check "$p" 2>/dev/null; then
			git apply "$p"
			echo "applied $name"
		elif git apply --reverse --check "$p" 2>/dev/null; then
			echo "skipped $name (already applied)"
		else
			echo "cannot apply $name (checkout is not at the expected baseline)" >&2
			exit 1
		fi
	else
		if patch -p1 --dry-run -s -i "$p" >/dev/null 2>&1; then
			patch -p1 -s -i "$p"
			echo "applied $name"
		elif patch -p1 -R --dry-run -s -i "$p" >/dev/null 2>&1; then
			echo "skipped $name (already applied)"
		else
			echo "cannot apply $name (not at the expected baseline)" >&2
			exit 1
		fi
	fi
done

md5sum web/index.html web/app.js web/local_download.js \
	web/agent_task_state.js web/sha256_fallback.js
echo
echo "expected:"
echo "39f8ce3c04cde21152e0d3e09a7b4b74  web/index.html"
echo "839bea52e730f36393657a2c4e994e1c  web/app.js"
echo "8fa8feb9f17e51009a9340fc15e35f92  web/local_download.js"
echo "1575e4a0d5d93babcd5d697298ce3f07  web/agent_task_state.js"
echo "fd08119b6db4467aab171fc12df28acf  web/sha256_fallback.js"
