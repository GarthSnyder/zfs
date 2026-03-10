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

#include <stdio.h>
#include <sys/types.h>
#include <sys/zfs_ioctl.h>

#include "zstream_chain.h"
#include "zstream_fletcher4.h"
#include "zstream_io.h"
#include "zstream_util.h"

/*
 * Copied from zfs_fletcher.c. See comments below regarding the
 * fletcher_4_incremental_combine function.
 */
#define	MAX_FLETCHER_BLOCK	(8ULL << 20)

typedef enum { F4_SET, F4_VALIDATE } fletcher4_op_t;

typedef struct {
	zio_cksum_t	fc_stream_cksum;
	fletcher4_op_t	fc_operation;
} fletcher4_context_t;

static fletcher4_context_t	fletcher4_contexts[MAX_FLETCHER_4];
static int			next_context = 0;

/*
 * fletcher_4_init() appears to run a benchmark, so make sure it's only once.
 */
static inline void
fletcher4_init_once(void) {
	static boolean_t initialized = B_FALSE;
	if (!initialized) {
		fletcher_4_init();
		initialized = B_TRUE;
	}
}

/*
 * The function below (and the MAX_FLETCHER_BLOCK define) are
 * copied from zfs_fletcher.c, where they're internal.
 *
 * Fletcher checksums CAN be computed in parallel, with the segments later
 * being reassembled. However, the combine function needs to know the
 * original length of each segment, and there's a hard limit as to how long
 * any given segment can be because 64-bit coefficients used in the combine
 * operation may overflow if the size is larger than 8MB.
 *
 * My understanding of this is that the checksum fields themselves can and
 * will overflow for long hash texts. However, they still function properly
 * as checksums when this happens. It's just that overflow has to be
 * handled correctly in a structured fashion, not by allowing intermediate
 * calculations to overflow.
 */
static inline void
fletcher4_incremental_combine(zio_cksum_t *zcp, const uint64_t size,
    const zio_cksum_t *nzcp)
{
	const uint64_t c1 = size / sizeof (uint32_t);
	const uint64_t c2 = c1 * (c1 + 1) / 2;
	const uint64_t c3 = c2 * (c1 + 2) / 3;

	/*
	 * Value of 'c3' overflows on buffer sizes close to 16MiB. For that
	 * reason we split incremental fletcher4 computation of large buffers
	 * to steps of (MAX_FLETCHER_BLOCK) size.
	 */
	ASSERT3U(size, <=, MAX_FLETCHER_BLOCK);

	zcp->zc_word[3] += nzcp->zc_word[3] + c1 * zcp->zc_word[2] +
	    c2 * zcp->zc_word[1] + c3 * zcp->zc_word[0];
	zcp->zc_word[2] += nzcp->zc_word[2] + c1 * zcp->zc_word[1] +
	    c2 * zcp->zc_word[0];
	zcp->zc_word[1] += nzcp->zc_word[1] + c1 * zcp->zc_word[0];
	zcp->zc_word[0] += nzcp->zc_word[0];
}

/*
 * This is the parallel calculation.
 */
static void
chain_calc_fletcher4(drr_fletcher4_t *item, void *context)
{
	(void) context;
	VERIFY3U(item->dp_base.dp_payload_size, >, 0);

	ssize_t remaining = item->dp_base.dp_payload_size;
	uint8_t *data = item->dp_base.dp_payload;
	size_t write_size = MIN(remaining, MAX_FLETCHER_BLOCK);
	int num_overflow = DIV_ROUND_UP(remaining, MAX_FLETCHER_BLOCK) - 1;
	zio_cksum_t *fragment = &item->dp_fletcher4_payload;

	fletcher4_init_once();
	fletcher_4_native(data, write_size, NULL, fragment);
	if (num_overflow) {
		fragment = safe_calloc(num_overflow * sizeof (zio_cksum_t));
		item->dp_fletcher4_overflow = fragment;
	}
	while (remaining -= write_size) {
		data += write_size;
		write_size = MIN(remaining, MAX_FLETCHER_BLOCK);
		fletcher_4_native(data, write_size, NULL, fragment);
		fragment++;
	}
}

/*
 * Used by the serial validation or inscription step. Combine the payload
 * checksum(s) produced by the parallel phase into the stream checksum.
 */
static void
assemble_payload_cksum(drr_fletcher4_t *item, zio_cksum_t *stream_ck)
{
	ssize_t remaining = item->dp_base.dp_payload_size;
	size_t read_size = MIN(remaining, MAX_FLETCHER_BLOCK);
	zio_cksum_t *fragment = item->dp_fletcher4_overflow;

	if (remaining == 0) { return; }
	fletcher4_incremental_combine(stream_ck, read_size,
		&item->dp_fletcher4_payload);
	while (remaining -= read_size) {
		read_size = MIN(remaining, MAX_FLETCHER_BLOCK);
		fletcher4_incremental_combine(stream_ck, read_size, fragment);
		fragment++;
	}
	if (item->dp_fletcher4_overflow != NULL) {
		free(item->dp_fletcher4_overflow);
		item->dp_fletcher4_overflow = NULL;
	}
}

/*
 * This is the serial step, which implements both validation and inscription.
 */
static boolean_t
chain_fletcher4(drr_fletcher4_t *item, fletcher4_context_t *context,
	chain_attrs_t *attrs)
{
	zio_cksum_t *stream_cksum = &context->fc_stream_cksum;
	dmu_replay_record_t *drr = &item->dp_base.dp_drr;
	zio_cksum_t *record_cksum = &drr->drr_u.drr_checksum.drr_checksum;
	zio_cksum_t *end_cksum = &drr->drr_u.drr_end.drr_checksum;
	off_t offset = offsetof(dmu_replay_record_t,
	    drr_u.drr_checksum.drr_checksum);
	boolean_t is_conclusion_record =
	    drr->drr_type == DRR_END &&
	    drr->drr_u.drr_end.drr_toguid == 0 &&
	    ZIO_CHECKSUM_IS_ZERO(&drr->drr_u.drr_checksum.drr_checksum);
	boolean_t end_cksum_zero = ZIO_CHECKSUM_IS_ZERO(&drre->drr_checksum);

	if (!item) {
		fletcher_4_fini();
		return (B_TRUE);
	}
	if (op == F4_VALIDATE && (attrs->ca_flags & CA_IGNORE_CKSUMS)) {
		return (B_TRUE);
	}
	if (item->dp_base.dp_stream_offset == 0) {
		fletcher4_init_once();
		VERIFY3U(offset, ==,
		    sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	}
	if (drr->drr_type == DRR_BEGIN) {
		ZIO_SET_CHECKSUM(stream_cksum, 0, 0, 0, 0);
	} else if (drr->drr_type == DRR_END) {
		if (op == F4_VALIDATE) {
			validate_or_exit(stream_cksum, end_cksum,
			    "in DRR_END record");
		} else {
			*end_cksum = *stream_cksum;
		}
	}
	fletcher_4_incremental_native(drr, offset, stream_cksum);
	if (drr->drr_type != DRR_BEGIN && !is_conclusion_record) {
		if (op == F4_VALIDATE) {
			validate_or_exit(stream_cksum, record_cksum,
				"at end of DRR record");
		} else {
			*record_cksum = *stream_cksum;
		}
	}
	if (drr->drr_type == DRR_END) {
		ZIO_SET_CHECKSUM(stream_cksum, 0, 0, 0, 0);
		VERIFY0(item->dp_base.dp_payload_size);
	} else {
		fletcher_4_incremental_native(record_cksum,
		    sizeof(drr->drr_u.drr_checksum.drr_checksum), stream_cksum);
		assemble_payload_cksum(item, stream_cksum);
	}
	return (B_TRUE);
}

/*
 * These queues double as I/O buffers, so the queue length is long.
 */
chain_step_t
parallel_calc_fletcher4(int queue_length) {
	return (chain_step_t) {
		.cs_type = CS_PARALLEL,
		.cs_in_size = sizeof(drr_packet_t),
		.cs_out_size = sizeof(drr_fletcher4_t),
		.cs_parallel = {
			.queue_length = queue_length,
			.batch_budget = 256 * 1024,
			.process = (zq_process_item_f *)chain_calc_fletcher4,
			.cost = (zq_estimate_cost_f *)payload_size_as_cost
		}
	};
}

static chain_step_t
fletcher4_serial_step(fletcher4_op_t operation) {
	fletcher4_context_t *context = &fletcher4_contexts[next_context];
	context->fc_operation = operation;
	ZIO_SET_CHECKSUM(&context->fc_stream_cksum, 0, 0, 0, 0);
	next_context++;
	return (chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof(drr_fletcher4_t),
		.cs_out_size = sizeof(drr_packet_t),
		.cs_context = context,
		.cs_serial = {
			.process =
				(zc_serial_process_f *)chain_fletcher4,
		}
	};
}

chain_step_t
serial_add_fletcher4(void) {
	return fletcher4_serial_step(F4_SET);
}

chain_step_t
serial_validate_fletcher4(void) {
	return fletcher4_serial_step(F4_VALIDATE);
}
