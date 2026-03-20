// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the Common
 * Development and Distribution License ("CDDL"), version 1.0. You may only use
 * this file in accordance with the terms of version 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this source. A
 * copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/byteorder.h>
#include <sys/spa_checksum.h>
#include <sys/stdtypes.h>
#include <sys/types.h>
#include <sys/zfs_ioctl.h>
#include <zfs_fletcher.h>

#include "zstream_modules.h"
#include "zstream_util.h"

typedef enum { F4_SET, F4_VALIDATE } fletcher4_op_t;

typedef struct {
	zio_cksum_t	fc_stream_cksum;
	fletcher4_op_t	fc_operation;
} fletcher4_context_t;

static fletcher4_context_t	fletcher4_contexts[MAX_FLETCHER_4];
static int			next_context = 0;

static inline int
fletcher_4_incremental(boolean_t swap, void *buff, size_t size, void *cksum)
{
	if (swap) {
		return fletcher_4_incremental_byteswap(buff, size, cksum);
	} else {
		return fletcher_4_incremental_native(buff, size, cksum);
	}
}

/*
 * Implements both validation and inscription, based on fc_operation.
 *
 * It emits or validates a replay record with proper checksums and with
 * proper maintenance of the stream checksum. That is:
 *
 *   1) Update stream checksum with the record header up to drr_checksum.
 *   2) Update drr_checksum field in the record header from stream checksum.
 *   3) Update stream checksum with the checksum field in the record header.
 *   4) Update stream checksum with the contents of the payload.
 *
 * DRR_BEGIN records do not have record checksums. They can't, because the
 * drr_begin struct overlaps with space that would otherwise be used for the
 * end-record checksum.
 *
 * DRR_END records normally do have end-record checksums. However, records
 * emitted by send_conclusion_record() in libzfs_sendrecv.c have the
 * checksum set to zero. zfs receive ignores those checksums. DRR_END records
 * also have an internal checksum that applies to the stream-to-date since the
 * most recent DRR_BEGIN.
 *
 * Null zstream transformations should be idempotent. E.g., a zstream redup
 * that does not redup anything should yield a stream that is bit-for-bit
 * identical to the original stream. So, it's helpful to emulate zfs send's
 * checksumming practices just to minimize spurious differences between
 * input and output streams.
 */
static disposition_t
chain_fletcher4(drr_packet_t *item, fletcher4_context_t *context)
{
	if (item == NULL || (context->fc_operation == F4_VALIDATE &&
	    OPTION_ENABLED(chain_attrs, CA_IGNORE_CKSUMS)))
	{
		return (D_OK);
	}

	zio_cksum_t *stream_cksum	= &context->fc_stream_cksum;
	dmu_replay_record_t *drr	= &item->dp_drr;
	struct drr_end *drre		= &item->dp_drr.drr_u.drr_end;
	zio_cksum_t *record_cksum	= &drr->drr_u.drr_checksum.drr_checksum;
	zio_cksum_t *end_cksum		= &drre->drr_checksum;

	boolean_t is_swapped = (context->fc_operation == F4_VALIDATE &&
	    ATTR_IS_SET(chain_attrs, CA_BYTESWAPPED)) ||
	    (context->fc_operation == F4_SET &&
	    OPTION_ENABLED(chain_attrs, CA_BYTESWAP_ON_OUTPUT));
	uint32_t drr_type = is_swapped ?
	    BSWAP_32(drr->drr_type) : drr->drr_type;
	off_t off = offsetof(dmu_replay_record_t,
	    drr_u.drr_checksum.drr_checksum);
	boolean_t is_conclusion_record =
	    drr_type == DRR_END &&
	    drre->drr_toguid == 0 &&
	    ZIO_CHECKSUM_IS_ZERO(&drr->drr_u.drr_checksum.drr_checksum);

	if (item->dp_stream_offset == 0) {
		VERIFY3U(off, ==, sizeof (dmu_replay_record_t) -
		    sizeof (zio_cksum_t));
	}
	if (drr_type == DRR_BEGIN) {
		ZIO_SET_CHECKSUM(stream_cksum, 0, 0, 0, 0);
	} else if (drr_type == DRR_END) {
		if (context->fc_operation == F4_VALIDATE) {
			off_t stream_offset = item->dp_stream_offset +
			    offsetof(dmu_replay_record_t,
			    drr_u.drr_end.drr_checksum);
			validate_or_exit(stream_cksum, end_cksum, is_swapped,
			    "in DRR_END record", stream_offset);
		} else {
			*end_cksum = *stream_cksum;
			if (is_swapped) {
				ZIO_CHECKSUM_BSWAP(end_cksum);
			}
		}
	}
	fletcher_4_incremental(is_swapped, drr, off, stream_cksum);
	if (drr_type != DRR_BEGIN && !is_conclusion_record) {
		if (context->fc_operation == F4_VALIDATE) {
			off_t stream_offset = item->dp_stream_offset +
			    offsetof(dmu_replay_record_t,
			    drr_u.drr_checksum.drr_checksum);
			validate_or_exit(stream_cksum, record_cksum,
			    is_swapped, "at DRR record end", stream_offset);
		} else {
			*record_cksum = *stream_cksum;
			if (is_swapped) {
				ZIO_CHECKSUM_BSWAP(record_cksum);
			}
		}
	}
	if (drr_type == DRR_END) {
		ZIO_SET_CHECKSUM(stream_cksum, 0, 0, 0, 0);
	} else {
		fletcher_4_incremental(is_swapped, record_cksum,
		    sizeof (drr->drr_u.drr_checksum.drr_checksum),
		    stream_cksum);
		if (item->dp_payload_size > 0) {
			fletcher_4_incremental(is_swapped, item->dp_payload,
			    item->dp_payload_size, stream_cksum);
		}
	}
	return (D_OK);
}

static chain_step_t
fletcher4_serial_step(fletcher4_op_t operation)
{
	int context_ix = next_context++ % MAX_FLETCHER_4;
	fletcher4_context_t *context = &fletcher4_contexts[context_ix];

	context->fc_operation = operation;
	ZIO_SET_CHECKSUM(&context->fc_stream_cksum, 0, 0, 0, 0);
	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = context,
		.cs_serial = {
			.process =
			    (zc_serial_process_f *)chain_fletcher4,
		}
	});
}

chain_step_t
serial_add_fletcher4(void)
{
	return (fletcher4_serial_step(F4_SET));
}

chain_step_t
serial_validate_fletcher4(void)
{
	return (fletcher4_serial_step(F4_VALIDATE));
}
