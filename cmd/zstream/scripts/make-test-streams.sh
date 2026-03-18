#!/bin/sh

if [ $# -ne 1 ]; then
	echo "Usage: $0 <device>" >&2
	exit 1
fi

DEVICE="$1"
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"

zpool create -o ashift=12 test "$DEVICE"
zfs set compression=on xattr=sa test
zfs create test/source

"$SCRIPTDIR/gen-lorem-files.py" -d /test/source 3
"$SCRIPTDIR/add-xattrs.py" /test/source/*
echo "very small" > /test/source/small
echo "password" > /test/source/to-be-redacted
chmod 400 /test/source/to-be-redacted

zfs snapshot -r test/source@baseline
zfs clone test/source@baseline test/redacted
rm /test/redacted/to-be-redacted

echo "password" > /test/redacted/new-key
zfs create -o encryption=on -o keylocation=file:///test/redacted/new-key \
    -o keyformat=passphrase test/redacted/encrypted
"$SCRIPTDIR/gen-lorem-files.py" -d /test/redacted/encrypted 3
echo "very small" > /test/redacted/encrypted/small-encrypted
"$SCRIPTDIR/add-xattrs.py" /test/redacted/encrypted/*

zfs snapshot -r test/redacted@clean

zfs redact test/source@baseline redaction-bookmark test/redacted@clean
zfs send -e --redact redaction-bookmark test/source@baseline > /tmp/part1.zfs
zfs send -Rew -i test/source@baseline test/redacted@clean > /tmp/part2.zfs
