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
#include "stream_fletcher4.h"

/*
 * Copied from zfs_fletcher.c. See comments below regarding the
 * fletcher_4_incremental_combine function.
 */
#define	MAX_FLETCHER_BLOCK	(8ULL << 20)

void
chain_calc_fletcher4(drr_fletcher4_t *item);

boolean_t
chain_validate_fletcher4(drr_fletcher4_t *item);

boolean_t
chain_finalize_fletcher4(drr_fletcher4_t *item);

static zio_cksum_t	fletcher4_contexts[MAX_FLETCHER_4];
static int		next_context = 0;

typedef struct chain_step {
			step_type_t		cs_type;
			size_t 			cs_out_size;
	union {
		struct {
			zc_serial_process_f	*css_process;
			void			*css_context;
		} serial;
		struct {
			size_t 			csp_queue_length;
			size_t 			csp_batch_budget;
			zq_estimate_cost_f	*csp_cost;
			zq_process_item_f	*csp_process;
		} parallel;
	};
} chain_step_t;

chain_step_t
parallel_calc_fletcher4() {
	return (chain_step_t) {
		.step_type_t = CS_PARALLEL,
		.cs_out_size = sizeof(drr_fletcher4_t),
		.parallel = {
			.csp_queue_length = 64,
			.csp_batch_budget = 128 * 1024,
			.csp_process = (zq_process_item_f *)chain_calc_fletcher4
		}		
	}
}

chain_step_t
serial_validate_fletcher4() {
	fletcher4_context_t *context = &fletcher4_contexts[next_context];
	ZIO_SET_CHECKSUM(context, 0, 0, 0, 0);
	next_context++;
	return (chain_step_t) {
		.step_type_t = CS_SERIAL,
		.cs_out_size = sizeof(drr_packet_t),
		.serial = {
			.css_process =
				(zc_serial_process_f *)chain_validate_fletcher,
			.css_context = context
		}
	};
}

typedef struct {
	drr_packet_t	dp_base;
	zio_cksum_t	dp_fletcher4_payload;
	zio_cksum_t	*dp_fletcher4_overflow[MAX_FLETCHER_BLOCKS];
	int		dp_num_overflows;
} drr_fletcher4_t;

/*
 * fletcher_4_init() appears to run a benchmark, so make sure it's only once.
 */
void
fletcher4_init_once() {
	static boolean_t initialized = B_FALSE;
	if (!initialized) {
		fletcher_4_init();
		initialized = B_TRUE;
	}
}

void
chain_calc_fletcher4(drr_fletcher4_t *item)
{
	assert(item->dp_base.dp_payload_size > 0);

	ssize_t remaining = items->dp_base.dp_payload_size;
	uint8_t *data = items->dp_base.dp_payload;
	size_t write_size = MIN(remaining, MAX_FLETCHER_BLOCK);
	int num_overflow = DIV_ROUND_UP(remaining, MAX_FLETCHER_BLOCK) - 1;
	zio_cksum_t *fragment = &item->dp_fletcher4_payload;

	fletcher_4_init_once();
	ZIO_SET_CHECKSUM(fragment, 0, 0, 0, 0);
	fletcher_4_incremental_native(data, write_size, fragment);
	if (num_overflow) {
		fragment = safe_calloc(num_overflow * sizeof(zio_cksum_t));
		item->dp_fletcher4_overflow = fragment;
	}
	while(remaining -= write_size) {
		data += write_size;
		write_size = MIN(remaining, MAX_FLETCHER_BLOCK);
		fletcher_4_incremental_native(data, write_size, fragment);
		fragment++;
	}
}

boolean_t
chain_validate_fletcher4(drr_fletcher4_t *item, zio_cksum_t *stream_cksum,
	chain_attrs_t chain)
{
	if (!item || (chain->ca_flags & CA_IGNORE_CKSUMS)) { return B_TRUE; }

	dmu_replay_record_t *drr = &item->dp_base.dp_drr;
	auto record_type = item->dp_base.dp_drr.drr_u.drr_type;
	zio_cksum_t *record_cksum = &drr->drr_u.drr_checksum.drr_checksum;
	zio_cksum_t *end_cksum = &drr->drr_u.drr_end.drr_checksum;
	boolean_t skip_record_cksum = (record_type == DRR_BEGIN) ||
		(record_type == DRR_END && !drr->drr_u.drr_end.drr_toguid);

	fletcher_4_init_once();
	if (record_type == DRR_END) {
		if (!validate_checksum(stream_cksum, end_cksum,
			"in DRR_END record")) { exit(1); }
	}
	if (record_type == DRR_BEGIN) {
		ZIO_SET_CHECKSUM(stream_cksum, 0, 0, 0, 0);
	}
	fletcher_4_incremental_native(drr, offset, stream_cksum);
	if (!skip_record_cksum && !validate_checksum(stream_cksum,
		record_cksum, "at end of DRR record")) { exit(1); }
	assemble_payload_cksum(item, stream_cksum);
}

void
assemble_payload_cksum(drr_fletcher4_t *item, zio_cksum_t *stream_ck)
{
	ssize_t remaining = item->dp_base.dp_payload_size;
	size_t read_size = MIN(remaining, MAX_FLETCHER_BLOCK);
	zio_cksum_t *fragment = item->dp_fletcher4_overflow;

	if (!item->dp_base.dp_payload_size) { return; }
	fletcher_4_incremental_combine(stream_ck, read_size,
		&item->dp_fletcher4_payload);
	while (remaining -= read_size) {
		read_size = MIN(remaining, MAX_FLETCHER_BLOCK);
		fletcher_4_incremental_combine(stream_ck, read_size, fragment);
		fragment++;
	}
}

boolean_t
chain_add_fletcher4(drr_fletcher4_t *item, zio_cksum_t *stream_cksum,
	chain_attrs_t chain)


/*
 * The function below (and the MAX_FLETCHER_BLOCK define) are
 * copied from zfs_fletcher.c, where they're internal. These internals
 * should perhaps be made public to facilitate multithreaded checksum
 * calculations. However, the original function is inline and so this
 * probably needs some adult supervision.
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
fletcher_4_incremental_combine(zio_cksum_t *zcp, const uint64_t size,
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



/* Init only the filename, chain_read_stream will prepare the FILE *. */
typedef struct {
	const char	*ic_filename;
	FILE		*ic_fp;
	boolean_t	ic_for_reading;
	off_t		ic_offset;
} io_context_t;

static chain_step_t
setup_io(const char *filename, boolean_t for_reading);

static boolean_t
chain_read(drr_packet_t *item, io_context_t *ctxt, chain_attrs_t chain);

static boolean_t
chain_write(drr_packet_t *item, io_context_t *ctxt, chain_attrs_t chain);

static io_context_t io_contexts[MAX_IO_STREAMS];
static int next_io_context = 0;

chain_step_t
read_stream(const char *filename) {
	return (setup_io(filename, B_TRUE));
}

chain_step_t
write_stream(const char *filename) {
	return (setup_io(filename, B_FALSE));
}

static chain_step_t
setup_io(const char *filename, boolean_t for_reading) {
	int context = next_io_context % MAX_IO_STREAMS;
	next_io_context++;
	io_contexts[context] = (io_context_t) {
		.ic_filename = filename,
		.ic_for_reading
	};
	return (chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_out_size = sizeof(drr_packet_t),
		.serial = {
			.css_process = (zc_serial_process_f *)(for_reading ?
				chain_read : chain_write),
			.css_context = &io_contexts[context]
		},
	};
}

static void
open_file(io_context_t *context) {
	if (context->ic_filename) {
		context->ic_fp = fopen(context->ic_filename,
			context->ic_for_reading ? "r" : "w+");
		if (!context->ic_fp) {
			perror(context->rc_filename);
			exit(1);
		}
	} else if (context->ic_for_reading && isatty(STDIN_FILENO)) {
		(void) fprintf(stderr,
		    "Error: Stream cannot be read from a terminal.\n"
		    "Name a file or take input from a pipe.\n");
		exit(1);
	} else if (context->ic_for_reading) {
		context->ic_fp = stdin;
	} else if (isatty(STDOUT_FILENO)) {
		(void) fprintf(stderr,
		    "Error: Stream cannot be written to a terminal.\n"
		    "Name a file or pipe to another command.\n");
		exit(1);
	} else {
		context->ic_fp = stdout;
	}
}

static boolean_t
chain_read(drr_packet_t *item, io_context_t *ctxt, chain_attrs_t chain)
{
	struct dmu_replay_record_t *drr = &item->dp_drr;

	if (!ctxt->rc_fp) {
		open_file(ctxt);
	}
	if (fread(drr, sizeof(dmu_replay_record_t), 1,
		ctxt->ic_fp) == 0)
	{
		if (ferror(ctxt->rc_fp)) {
			fprintf(stderr, "Error reading stream: %s\n",
			    strerror(errno));
			exit(1);
		} else {
			return B_FALSE;
		}
	}
	if (ctxt->ic_offset == 0) {
		uint64_t magic = drr->drr_u.drr_begin.drr_magic;
		if (magic == BSWAP_64(DMU_BACKUP_MAGIC)) {
			chain->ca_flags |= CA_BYTESWAPPED;
		} else if (magic != DMU_BACKUP_MAGIC) {
			fprintf(stderr, "Invalid ZFS stream, bad magic "
				"number %lx\n", magic);
			exit(1);
		}
	}
	payload_size =(chain->ca_flags & CA_BYTESWAPPED) ?
		BSWAP_32(drr->drr_payloadlen) : drr->drr_payloadlen;
	if (payload_size) {
		item->dp_payload = safe_malloc(payload_size);
		size_t items_read = fread(item->dp_payload,
			payload_size, 1, context->rc_fp);
		if (items_read != 1) {
			fprintf(stderr, "Error reading record payload "
				" at offset %lu", ctxt->ic_offset);
			exit(1);
		}
	} else {
		item->dp_payload = NULL;
	}
	item->dp_payload_size = payload_size;
	item->dp_stream_offset = ctxt->ic_offset;
	context->ic_offset += sizeof(*drr) + payload_size;
	return B_TRUE;
}

static boolean_t
chain_write(drr_packet_t *item, io_context_t *ctxt, chain_attrs_t chain)
{
	struct dmu_replay_record_t *drr = &item->dp_drr;

	if (!ctxt->rc_fp) {
		open_file(ctxt);
	}
	if (!item) {
		fclose(ctxt->ic_fp);
		return B_TRUE;
	}
	if (fwrite(drr, sizeof(dmu_replay_record_t), 1,
		ctxt->ic_fp) != 1)
	{
		fprintf(stderr, "Error writing record: %s\n",
		    strerror(errno));
		exit(1);
	} else if (item->payload_size > 0) {
		if (fwrite(item->dp_payload, item->dp_payload_size,
			1, ctxt->ic_fp) != 1)
		{
			fprintf(stderr, "Error writing payload: %s\n",
			    strerror(errno));
			exit(1);
		} else {
			free(item->dp_payload);
			item->dp_payload = NULL;
			item->dp_payload_size = 0;
		}
	}
	return B_TRUE;
}

