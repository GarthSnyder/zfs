#!/bin/sh

if [ $# -ne 1 ]; then
	echo "Usage: $0 <device>" >&2
	exit 1
fi

DEVICE="$1"
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"

zpool create -o ashift=12 test "$DEVICE"
echo "password" > /test/password

zfs create -o compression=zstd-5 test/unencrypted
"$SCRIPTDIR/gen-lorem-files.py" -d /test/unencrypted --min-size 100000 \
    --max-size 200000 5

zfs create -o compression=lz4 -o encryption=on -o keylocation=file:///test/password -o keyformat=passphrase test/encrypted
"$SCRIPTDIR/gen-lorem-files.py" -d /test/encrypted --min-size 100000 \
    --max-size 200000 5

zfs snapshot -r test@decompression
zfs send -cw test/unencrypted@decompression > /tmp/decompression.zsend
zfs send -cw test/encrypted@decompression > /tmp/decompression-crypt.zsend
