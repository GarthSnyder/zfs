#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0

#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

#
# Copyright (c) 2026 by Garth Snyder. All rights reserved.
#

. $STF_SUITE/tests/functional/zstream/zstream.kshlib

#
# Description:
#
# Verify that "zstream redup -m" limits the memory used for reduplication
# information, and that information beyond the limit spills correctly to
# temporary files in the "zstream redup -d" directory.
#
# Strategy:
#
# 1. Reduplicate a dedup stream with the default memory limit, then with
#    limits small enough to force spilling, including -m 0 (disk only).
#    Spill files go to a dataset in the test pool. All outputs must match.
#
# 2. Receive the disk-only output and compare its contents with the
#    original files.
#
# 3. Send a filesystem with tens of thousands of small blocks. redup
#    tracks every WRITE record, so its table grows through several cycles
#    of bucket splits and outgrows a 1MB limit.
#
# 4. Verify that redup is an identity transform on that stream at each
#    memory limit.
#
# 5. Verify that the spill files really are in the -d directory and that
#    -m controls whether they are used: on a dataset with a small quota,
#    a disk-only run must fail for lack of space, and a run with a generous
#    limit must succeed.
#
# 6. Verify that no temporary files are left behind.
#

verify_runnable "both"

function cleanup
{
	destroy_dataset $POOL/recv "-r"
	destroy_dataset $POOL/small "-r"
	destroy_dataset $POOL/spill "-r"
	destroy_dataset $POOL/tiny "-r"
	rm -rf $BACKDIR/*
}
log_onexit cleanup

log_assert "zstream redup -m and -d spill reduplication data correctly."

typeset rsend=$STF_SUITE/tests/functional/rsend
typeset -a limits=(0 256K 1M 1.5M 64M)

log_must zfs create -o compression=off $POOL/spill
typeset spill=$(get_prop mountpoint $POOL/spill)

# Steps 1 and 2: a real dedup stream
typeset dedup=$BACKDIR/dedup.zsend
log_must eval "bzcat < $rsend/dedup.zsend.bz2 > $dedup"
log_must eval "zstream redup $dedup > $BACKDIR/dedup.ref"
for m in "${limits[@]}"; do
	log_must eval "zstream redup -m $m -d $spill $dedup > $BACKDIR/dedup.out"
	log_must cmp $BACKDIR/dedup.ref $BACKDIR/dedup.out
done

log_must eval "zstream redup -m 0 -d $spill $dedup > $BACKDIR/dedup.out"
log_must zfs create $POOL/recv
log_must eval "zfs recv -d $POOL/recv < $BACKDIR/dedup.out"
log_must mkdir $BACKDIR/tar
log_must tar --directory $BACKDIR/tar -xzf $rsend/fs.tar.gz
log_must directory_diff $BACKDIR/tar/fs $(get_prop mountpoint $POOL/recv/fs)

# Steps 3 and 4: a stream with many WRITE records
typeset small=$BACKDIR/small.zsend
log_must zfs create -o recordsize=512 -o compression=off $POOL/small
typeset smalldir=$(get_prop mountpoint $POOL/small)
log_must dd if=/dev/urandom of=$smalldir/file bs=1M count=40
log_must zfs snapshot $POOL/small@snap
log_must eval "zfs send $POOL/small@snap > $small"
typeset -i nwrites=$(zstream dump $small | \
    awk '/Total DRR_WRITE records/ { print $5 }')
(( nwrites > 60000 )) || log_fail "stream has only $nwrites WRITE records"

for m in "${limits[@]}"; do
	log_must eval "zstream redup -m $m -d $spill $small > $BACKDIR/small.out"
	log_must cmp $small $BACKDIR/small.out
done

#
# Step 5: About 58 bytes of spill data per WRITE record when disk-only,
# so several times the quota
#
log_must zfs create -o compression=off -o quota=1M $POOL/tiny
typeset tiny=$(get_prop mountpoint $POOL/tiny)
log_mustnot eval "zstream redup -m 0 -d $tiny $small > $BACKDIR/small.out \
    2> $BACKDIR/small.err"
log_must grep -q "out of space in $tiny" $BACKDIR/small.err
log_must eval "zstream redup -m 64M -d $tiny $small > $BACKDIR/small.out"
log_must cmp $small $BACKDIR/small.out

# Step 6
for dir in $spill $tiny; do
	[[ -z $(ls -A $dir) ]] || log_fail "temporary files left in $dir"
done

log_pass "zstream redup -m and -d spill reduplication data correctly."
