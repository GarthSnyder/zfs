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
# Verify that the only differences between -old and -new dump reference
# files are the expected additions: nvlist encoding lines and two
# additional record types in the summary (DRR_OBJECT_RANGE, DRR_REDACT).
#
# Strategy:
# 1. Determine system endianness
# 2. For native-endian streams and all XDR streams, filter the -new dump
#    to remove the known additions
# 3. Compare the filtered output to the -old dump — must be identical
#

verify_runnable "both"

log_assert "Verify old-vs-new dump diff contains only expected additions."

typeset sys_endian=$(get_system_endian)

# All streams whose -old and -new dumps should differ only by the
# known additions: native-endian streams (any encoding) plus all XDR.
# Non-native NATIVE streams have additional differences (error messages)
# and are excluded.
typeset -a streams=(
	big-endian-all-drr-types-base-XDR
	big-endian-all-drr-types-incr-XDR
	little-endian-all-drr-types-base-XDR
	little-endian-all-drr-types-incr-XDR
)

if [[ $sys_endian == "little" ]]; then
	streams+=(
		little-endian-all-drr-types-base-NATIVE
		little-endian-all-drr-types-incr-NATIVE
	)
else
	streams+=(
		big-endian-all-drr-types-base-NATIVE
		big-endian-all-drr-types-incr-NATIVE
	)
fi

typeset failed=""

for stem in "${streams[@]}"; do
	typeset abbrev=$(get_stream_abbrev "$stem")
	typeset newdump="$ZSTREAM_DATADIR/${abbrev}-new.dump"
	typeset olddump="$ZSTREAM_DATADIR/${abbrev}-old.dump"
	typeset filtered="$BACKDIR/${abbrev}-filtered.dump"

	# Remove the lines that are new additions:
	# 1. "nvlist encoding = ..." lines
	# 2. Summary lines for DRR_OBJECT_RANGE and DRR_REDACT
	grep -v '^nvlist encoding = ' "$newdump" | \
	    grep -v 'Total DRR_OBJECT_RANGE records' | \
	    grep -v 'Total DRR_REDACT records' > "$filtered"

	if ! diff -q "$olddump" "$filtered" > /dev/null 2>&1; then
		log_note "MISMATCH after filtering: $stem (abbrev $abbrev)"
		log_note "$(diff "$olddump" "$filtered")"
		failed="$failed $stem"
	fi
done

[[ -z $failed ]] || \
    log_fail "Filtered new dump did not match old dump for:$failed"

log_pass "Old-vs-new dump diff contains only expected additions."
