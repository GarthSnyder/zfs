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
 */

#include <stdio.h>
#include <stddef.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_ioctl.h>
#include <sys/dmu.h>

#ifndef	_ZSTREAM_STREAM_H
#define	_ZSTREAM_STREAM_H

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * read_stream: generalized ZFS stream processing
 *
 * read_stream copies input to output, passing individual records to
 * one or more filters. It handles checksumming, checksum validation,
 * marshalling, and byteswapping.
 *
 * Filters may alter the record blocks or payloads as they wish. Payloads
 * are malloc'ed. If a filter wants to increase a payload's size, it can
 * call realloc() or free the old payload and write a new payload address.
 *
 * If a client wants full control over the tail of the pipeline, it can
 * pass a negative value in the output field, which is functionally
 * identical to passing an fd to /dev/null. If the retain_payload
 * argument is B_TRUE, the client is responsible for deallocating
 * payloads. The record header buffer is always reused, so that
 * must be copied if the client wants to refer to earlier blocks.
 *
 * read_stream checks the stream's first block to determine endianness.
 * It validates checksums and converts records to native-endian.
 * Callbacks are always called with native-endian records.
 *
 * Record-type-specific callbacks are always called with native-endian
 * order. To emit a stream in nonnative-endian order, call drr_byteswap
 * from all_records_post and calculate your own running checksums.
 *
 * The context argument is a pointer to client-specific data. It is
 * not examined and is passed to every callback.
 */

typedef void stream_callback_t(dmu_replay_record_t *drr, uint8_t **payload,
	uint32_t *payload_size, void *context);

typedef struct {
	stream_callback_t	*all_records_pre;			/* checksums validated */
	stream_callback_t 	*by_type[DRR_NUMTYPES];		/* Record-type specific */
	stream_callback_t 	*all_records_post;			/* checksummed */
} stream_filter_t;

extern void read_stream(FILE *input, int output, stream_filter_t *filters,
	int num_filters, boolean_t retain_payload, void *context);

/*
 * Byteswapping only, invertible through a second call. Always swaps, not
 * "byteswap if needed."
 */
extern void
drr_byteswap(dmu_replay_record_t *drr, int record_type);

#ifdef	__cplusplus
}
#endif

#endif	/* _ZSTREAM_STREAM_H */
