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
# http://www.illumos.org/license/CDDL.
#

#
# Copyright (c) 2026 by ConnectWise. All rights reserved.
#

. $STF_SUITE/tests/functional/zstream/zstream.kshlib

#
# Description:
# Verify that zstream decompress with "off" as the compression type
# changes record headers to mark them as uncompressed but leaves the
# actual data payload untouched.  This means the received files will
# contain raw compressed data (junk) for the affected records.
#
# Strategy:
# 1. Decompress selected records with type "off"
# 2. Verify via zstream dump that selected records now show
#    compression type = 0 and logical_size is unchanged
# 3. Receive both original and "off" streams into a test pool
# 4. Verify that file hashes differ (junk data in affected files)
#

verify_runnable "both"

log_assert "Verify decompress with 'off' changes headers but not data."
log_onexit cleanup_pool $POOL

typeset src="$ZSTREAM_DATADIR/decompress.zsend.bz2"
typeset orig="$BACKDIR/decompress.orig"
typeset off_out="$BACKDIR/decompress-off.out"
typeset orig_dump="$BACKDIR/decompress-orig.dump"
typeset off_dump="$BACKDIR/decompress-off.dump"

typeset -a records=(2,0 4,0 5,131072)

bzcat "$src" > "$orig"
log_must eval "zstream decompress 2,0,off 4,0,off 5,131072,off \
    < '$orig' > '$off_out'"

# Dump both streams
log_must eval "zstream dump -v < '$orig' > '$orig_dump' 2>&1"
log_must eval "zstream dump -v < '$off_out' > '$off_dump' 2>&1"

# Verify selected records show compression type = 0 with same logical_size
typeset failed=""
for rec in "${records[@]}"; do
	typeset obj=${rec%,*}
	typeset off=${rec#*,}

	typeset orig_line=$(awk \
	    "/^WRITE object = $obj .* offset = $off /" \
	    "$orig_dump")
	typeset orig_lsize=$(echo "$orig_line" | \
	    sed 's/.*logical_size = \([0-9]*\).*/\1/')

	typeset off_line=$(awk \
	    "/^WRITE object = $obj .* offset = $off /" \
	    "$off_dump")

	if ! echo "$off_line" | grep -q 'compression type = 0'; then
		log_note "Record $rec: compression type not 0"
		failed="$failed ${rec}(comp)"
	fi

	typeset off_lsize=$(echo "$off_line" | \
	    sed 's/.*logical_size = \([0-9]*\).*/\1/')
	if [[ "$off_lsize" != "$orig_lsize" ]]; then
		log_note "Record $rec: logical_size changed ($orig_lsize -> $off_lsize)"
		failed="$failed ${rec}(lsize)"
	fi
done

[[ -z $failed ]] || \
    log_fail "Header verification failed for:$failed"

# Receive original and hash
recv_and_hash "$BACKDIR/hash-orig.txt" "$orig" cleanup

# Receive "off" stream and hash
recv_and_hash "$BACKDIR/hash-off.txt" "$off_out" cleanup

# Hashes must differ (affected files now contain junk data)
if diff -q "$BACKDIR/hash-orig.txt" "$BACKDIR/hash-off.txt" \
    > /dev/null 2>&1; then
	log_fail "Expected file contents to differ, but hashes are identical"
fi

log_pass "Decompress with 'off' changes headers but not data."
