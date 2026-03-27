#!/bin/sh

if [ $# -ne 1 ]; then
	echo "Usage: $0 <device>" >&2
	exit 1
fi

DEVICE="$1"
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"

zpool create -o ashift=12 test "$DEVICE"

zfs set compression=off recordsize=16MiB test
"$SCRIPTDIR/gen-lorem-files.py" -d /test -r --min-size 16000000 \
    --max-size 32000000 3

zfs snapshot test@long-payloads
zfs send -L test@long-payloads > /tmp/long-payloads.zsend
