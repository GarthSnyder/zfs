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
# Verify that zstream decompress refuses to decompress encrypted records,
# printing a warning for each and leaving the output stream identical to
# the input.
#
# Strategy:
# 1. Select compressed encrypted records from decompress-crypt.zsend
# 2. Attempt to decompress them
# 3. Verify stderr contains warnings for each record
# 4. Verify output stream is byte-identical to input
#

verify_runnable "both"

log_assert "Verify encrypted records refuse to decompress."

typeset src="$ZSTREAM_DATADIR/decompress-crypt.zsend.bz2"
typeset orig="$BACKDIR/decompress-crypt.orig"
typeset output="$BACKDIR/decompress-crypt.out"
typeset errfile="$BACKDIR/decompress-crypt.err"

bzcat "$src" > "$orig"

# Attempt to decompress encrypted records
zstream decompress 2,0 5,0 3,131072 \
    < "$orig" > "$output" 2>"$errfile"

# Output stream must be identical to input
log_must cmp -s "$orig" "$output"

# Stderr should contain warnings about each record
typeset errcount=$(wc -l < "$errfile")
if [[ $errcount -eq 0 ]]; then
	log_fail "Expected warning messages on stderr, got none"
fi
log_note "Got $errcount lines of warning output (expected)"

log_pass "Encrypted records refuse to decompress."
