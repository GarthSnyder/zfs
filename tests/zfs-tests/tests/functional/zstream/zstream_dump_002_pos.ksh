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
# Verify that zstream dump -v output matches reference dump files for all
# pre-generated test streams (both endiannesses, base/incremental,
# NATIVE/XDR encoding).
#
# Strategy:
# 1. For each of the 8 test streams, run zstream dump -v
# 2. Compare stdout+stderr against the corresponding -new reference dump
#

verify_runnable "both"

log_assert "Verify zstream dump -v output matches reference dump files."

typeset -a streams=(
	big-endian-all-drr-types-base-NATIVE
	big-endian-all-drr-types-base-XDR
	big-endian-all-drr-types-incr-NATIVE
	big-endian-all-drr-types-incr-XDR
	little-endian-all-drr-types-base-NATIVE
	little-endian-all-drr-types-base-XDR
	little-endian-all-drr-types-incr-NATIVE
	little-endian-all-drr-types-incr-XDR
)

typeset failed=""

for stem in "${streams[@]}"; do
	typeset abbrev=$(get_stream_abbrev "$stem")
	typeset ref="$ZSTREAM_DATADIR/${abbrev}-new.dump"
	typeset out="$BACKDIR/${abbrev}-dump.out"

	bzcat "$ZSTREAM_DATADIR/${stem}.zsend.bz2" | \
	    zstream dump -v > "$out" 2>&1

	if ! diff -q "$ref" "$out" > /dev/null 2>&1; then
		log_note "MISMATCH: $stem (abbrev $abbrev)"
		log_note "$(diff "$ref" "$out")"
		failed="$failed $stem"
	fi
done

[[ -z $failed ]] || log_fail "Dump output mismatch for:$failed"

log_pass "zstream dump -v output matches reference dump files."
