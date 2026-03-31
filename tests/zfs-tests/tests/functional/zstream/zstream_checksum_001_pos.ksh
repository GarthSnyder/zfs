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
# Verify that very long payload records are checksummed correctly by
# running them through zstream redup (which recalculates checksums but is
# otherwise a no-op). This exercises the 8MiB Fletcher4 chunk boundary
# handling.
#
# Strategy:
# 1. Decompress the long-payloads test stream
# 2. Pipe through zstream redup
# 3. Verify the output is byte-identical to the input
#

verify_runnable "both"

log_assert "Verify long payload records are checksummed correctly."

typeset src="$ZSTREAM_DATADIR/long-payloads.zsend.bz2"
typeset orig="$BACKDIR/long-payloads.zsend.orig"
typeset redup_out="$BACKDIR/long-payloads.zsend.redup"

bzcat "$src" > "$orig"
log_must eval "zstream redup '$orig' > '$redup_out'"
log_must cmp -s "$orig" "$redup_out"

log_pass "Long-payload records are checksummed correctly."
