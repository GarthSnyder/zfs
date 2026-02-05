// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 * Copyright 2022 Axcient.  All rights reserved.
 * Copyright (c) 2022 by Delphix. All rights reserved.
 * Copyright (c) 2024, Klara, Inc.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include "zstream.h"
#include "zfs_fletcher.h"
#include "zstream_shared.h"

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

void *
safe_malloc(size_t size)
{
	void *rv = malloc(size);
	if (rv == NULL) {
		(void) fprintf(stderr, "ERROR; failed to allocate %zu bytes\n",
		    size);
		abort();
	}
	return (rv);
}

void *
safe_calloc(size_t n)
{
	void *rv = calloc(1, n);
	if (rv == NULL) {
		fprintf(stderr,
		    "Error: could not allocate %u bytes of memory\n",
		    (int)n);
		exit(1);
	}
	return (rv);
}

/*
 * DRR_BEGIN records do not have record checksums, but DRR_END
 * records generally do. However, there are two cases in which the
 * record-level checksum is not filled out:
 *
 *   1) The END record that immediately follows a BEGIN record
 *      with a header type of DMU_COMPOUNDSTREAM
 *
 *   2) The final END record of a stream, which immediately 
 *      follows another END record.
 *
 * DRR_END records have two checksums that are distinct:
 *
 *   drr->drr_u.drr_end.drr_checksum
 *   drr->drr_u.drr_checksum.drr_checksum
 *
 * The former is the in-record checksum of the stream, which is
 * always filled out. The latter is the record checksum that is
 * common to every record type.
 *
 * zfs receive does not validate the record checksum of an END
 * record that fits into the two categories above, so it normally 
 * doesn't matter whether the checksum is there or not. However,
 * null zstream transformations should be idempotent. A zstream
 * redup that does not redup anything or a zstream recompress
 * that does not change actual compression should yield a stream
 * that is bit-for-bit identical to the original stream.
 *
 * DRR_END records that don't need a checksum can be identified
 * by their toguid of 0.
 */

int
dump_record(dmu_replay_record_t *drr, void *payload, int payload_len,
    zio_cksum_t *zc, int outfd)
{
	assert(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum)
	    == sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	fletcher_4_incremental_native(drr,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum), zc);
	boolean_t skip_checksum = (drr->drr_type == DRR_BEGIN) ||
		((drr->drr_type == DRR_END && drr->drr_u.drr_end.drr_toguid == 0));
	if (!skip_checksum) {
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

static char cksum_str[128];

char *
checksum_str(zio_cksum_t *cksum) {
	snprintf(cksum_str, sizeof(cksum_str), "%.16llx / %.16llx / %.16llx / %.16llx",
		(long long unsigned int) cksum->zc_word[0],
		(long long unsigned int) cksum->zc_word[1],
		(long long unsigned int) cksum->zc_word[2],
		(long long unsigned int) cksum->zc_word[3]);
	return cksum_str;
}

boolean_t
validate_checksum(zio_cksum_t *expected, zio_cksum_t *actual,
	const char *where) 
{
	if (ZIO_CHECKSUM_EQUAL(*expected, *actual)) {
		return B_TRUE;
	}
	fprintf(stderr, "Incorrect checksum %s.\n", where);
	fprintf(stderr, "Expected = %s\n", checksum_str(expected));
	fprintf(stderr, "  Actual = %s\n", checksum_str(actual));
	return B_FALSE;
}

