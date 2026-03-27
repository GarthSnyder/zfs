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
# Copyright (c) 2026 by Garth Snyder. All rights reserved.
#

. $STF_SUITE/tests/functional/zstream/zstream.kshlib

#
# Description:
# Verify that zstream redup produces output identical to input for the
# decompress and decompress-crypt test streams.
#
# Strategy:
# 1. For each stream, decompress with bzcat
# 2. Pipe through zstream redup
# 3. Compare with cmp against the original decompressed stream
#

verify_runnable "both"

log_assert "Verify zstream redup is identity on decompress/crypt streams."

typeset -a streams=(
	decompress
	decompress-crypt
)

typeset failed=""

for stem in "${streams[@]}"; do
	typeset src="$ZSTREAM_DATADIR/${stem}.zsend.bz2"
	typeset orig="$BACKDIR/${stem}.orig"
	typeset redup_out="$BACKDIR/${stem}.redup"

	bzcat "$src" > "$orig"
	zstream redup < "$orig" > "$redup_out"

	if ! cmp -s "$orig" "$redup_out"; then
		log_note "MISMATCH: zstream redup output differs for $stem"
		failed="$failed $stem"
	fi

	rm -f "$orig" "$redup_out"
done

[[ -z $failed ]] || log_fail "Redup identity check failed for:$failed"

log_pass "zstream redup is identity on decompress/crypt streams."
