// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2020 by Delphix. All rights reserved.
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
 * Copyright (c) 2012 Pawel Jakub Dawidek <pawel@dawidek.net>.
 * All rights reserved
 * Copyright (c) 2013 Steven Hartland. All rights reserved.
 * Copyright 2015, OmniTI Computer Consulting, Inc. All rights reserved.
 * Copyright 2016 Igor Kozhukhov <ikozhukhov@gmail.com>
 * Copyright (c) 2018, loli10K <ezomori.nozomu@gmail.com>. All rights reserved.
 * Copyright (c) 2019 Datto Inc.
 * Copyright (c) 2024, Klara, Inc.
 */

#include <sys/debug.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zfs_fletcher.h>
#include "zstream_util.h"

/*
 * Originally from libzfs_sendrecv.c, but not identical. Emit a replay
 * record with proper checksums and with proper maintenance of the stream
 * checksum passed in as zc.
 *
 * DRR_BEGIN records do not have record checksums. They can't, because the
 * drr_begin struct overlaps with space that would otherwise be used for the
 * end-record checksum.
 *
 * DRR_END records normally do have end-record checksums. However, records
 * emitted by send_conclusion_record() in libzfs_sendrecv.c have the
 * checksum set to zero. zfs receive ignores those.
 *
 * Null zstream transformations should be idempotent. E.g., a zstream redup
 * that does not redup anything should yield a stream that is bit-for-bit
 * identical to the original stream. So, it's helpful to emulate zfs send's
 * checksumming practices just to minimize spurious differences between
 * input and output streams.
 *
 * Note that there are two separate checksums in a DRR_END record. One is
 * the drr->drr_u.drr_end.drr_checksum field, which is a stream-to-date
 * checksum that is always filled out and always validated. The other
 * checksum is the end-record drr->drr_u.drr_checksum.drr_checksum field,
 * which may or may not be present.
 */
int
dump_record(dmu_replay_record_t *drr, void *payload, size_t payload_len,
    zio_cksum_t *zc, int outfd)
{
	ASSERT3U(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
	    ==, sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	fletcher_4_incremental_native(drr,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum), zc);
	boolean_t is_conclusion_record =
	    drr->drr_type == DRR_END &&
	    drr->drr_u.drr_end.drr_toguid == 0 &&
	    ZIO_CHECKSUM_IS_ZERO(&drr->drr_u.drr_checksum.drr_checksum);
	if (!is_conclusion_record && drr->drr_type != DRR_BEGIN) {
		drr->drr_u.drr_checksum.drr_checksum = *zc;
	}
	fletcher_4_incremental_native(&drr->drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t), zc);
	if (write(outfd, drr, sizeof (*drr)) == -1)
		return (errno);
	if (payload_len != 0) {
		fletcher_4_incremental_native(payload, payload_len, zc);
		if (write(outfd, payload, payload_len) == -1)
			return (errno);
	}
	return (0);
}

void *
safe_malloc(size_t size)
{
	void *rv = malloc(size);
	if (rv == NULL) {
		(void) fprintf(stderr, "Error: failed to allocate %zu bytes\n",
		    size);
		exit(1);
	}
	return (rv);
}

void *
safe_calloc(size_t size)
{
	void *rv = calloc(1, size);
	if (rv == NULL) {
		(void) fprintf(stderr,
		    "Error: failed to allocate %zu bytes\n", size);
		exit(1);
	}
	return (rv);
}

/*
 * Safe version of fread(), exits on error.
 */
int
sfread(void *buf, size_t size, FILE *fp)
{
	int rv = fread(buf, size, 1, fp);
	if (rv == 0 && ferror(fp)) {
		(void) fprintf(stderr, "Error while reading file: %s\n",
		    strerror(errno));
		exit(1);
	}
	return (rv);
}
