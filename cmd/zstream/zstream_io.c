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

#include <errno.h>		/* errno				*/
#include <libzutil.h>		/* zfs_nicenum				*/
#include <stdio.h>		/* fprintf, stderr, fclose, fread	*/
#include <stdlib.h>		/* exit, free				*/
#include <string.h>		/* strerror				*/
#include <sys/byteorder.h>	/* BSWAP_32, BSWAP_64			*/
#include <sys/stdtypes.h>	/* B_TRUE, boolean_t, B_FALSE		*/
#include <sys/sysmacros.h>	/* P2ROUNDUP				*/
#include <sys/types.h>		/* off_t				*/
#include <sys/zfs_ioctl.h>	/* drr_begin, dmu_replay_record_t	*/
#include <time.h>		/* timespec, clock_gettime, CLOC...	*/
#include <unistd.h>		/* isatty, STDIN_FILENO, STDOUT_...	*/

#include "zstream_io.h"		/* drr_packet_t, zc_serial_process_f	*/
#include "zstream_chain.h"
#include "zstream_util.h"	/* safe_malloc				*/

/* Init only the filename, chain_read_stream will prepare the FILE *. */
typedef struct {
	const char	*ic_filename;
	FILE		*ic_fp;
	boolean_t	ic_for_reading;
	off_t		ic_offset;
} io_context_t;

typedef struct {
	const char	*cc_name;
	double		cc_last_sec;
	double		cc_period_sec;
	uint64_t	cc_last_bytes;
} checkpoint_context_t;

static io_context_t io_contexts[MAX_IO_STREAMS];
static int next_io_context = 0;

static checkpoint_context_t checkpoint_contexts[MAX_IO_STREAMS];
static int next_checkpoint_context = 0;

/*
 * Run from within chain execution to initialize I/O. A NULL filename
 * indicates stdin or stdout.
 */
static void
open_file(io_context_t *context)
{
	if (context->ic_filename) {
		context->ic_fp = fopen(context->ic_filename,
		    context->ic_for_reading ? "r" : "w+");
		if (!context->ic_fp) {
			perror(context->ic_filename);
			exit(1);
		}
	} else if (context->ic_for_reading && isatty(STDIN_FILENO)) {
		fprintf(stderr,
		    "Error: Stream cannot be read from a terminal.\n"
		    "Name a file or take input from a pipe.\n");
		exit(1);
	} else if (context->ic_for_reading) {
		context->ic_fp = stdin;
	} else if (isatty(STDOUT_FILENO)) {
		fprintf(stderr,
		    "Error: Stream cannot be written to a terminal.\n"
		    "Capture output to a file or pipe to another command.\n");
		exit(1);
	} else {
		context->ic_fp = stdout;
	}
}

/*
 * Extract the payload size from a replay record that is potentially
 * byteswapped. We want to leave the bulk of byteswapping to another module,
 * so just take a quick, nondestructive peek.
 *
 * Record-specific macros such as DRR_WRITE_PAYLOAD_SIZE do not seem to be
 * byteswap-aware. However, with the exception of DRR_OBJECT_PAYLOAD_SIZE,
 * they happen to work with post-swapping since they are switching on either
 * a uint8_t value or 0.
 */
static size_t
calc_payload_size(dmu_replay_record_t *drr, chain_attrs_t *attrs)
{
	struct drr_object *drro		 = &drr->drr_u.drr_object;
	struct drr_write *drrw		 = &drr->drr_u.drr_write;
	struct drr_spill *drrs		 = &drr->drr_u.drr_spill;
	struct drr_write_embedded *drrwe = &drr->drr_u.drr_write_embedded;

	boolean_t swap = ATTR_IS_SET(attrs, CA_BYTESWAPPED);
	uint32_t drr_type = swap ? BSWAP_32(drr->drr_type) : drr->drr_type;
	uint32_t size;

	switch (drr_type) {
	case DRR_OBJECT:
		if (swap && drro->drr_raw_bonuslen != 0) {
			return (BSWAP_32(drro->drr_raw_bonuslen));
		} else if (swap) {
			return (P2ROUNDUP(BSWAP_32(drro->drr_bonuslen), 8));
		} else {
			return (DRR_OBJECT_PAYLOAD_SIZE(drro));
		}
	case DRR_WRITE:
		size = DRR_WRITE_PAYLOAD_SIZE(drrw);
		break;
	case DRR_SPILL:
		size = DRR_SPILL_PAYLOAD_SIZE(drrs);
		break;
	case DRR_WRITE_EMBEDDED:
		uint32_t drr_psize = drrwe->drr_psize;
		return (P2ROUNDUP((swap ? BSWAP_32(drr_psize) : drr_psize), 8));
	default:
		size = drr->drr_payloadlen;
	}
	return (swap ? BSWAP_32(size) : size);
}

static boolean_t
chain_read(drr_packet_t *item, io_context_t *context, chain_attrs_t *attrs)
{
	if (item == NULL)
		return (B_TRUE);

	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_begin *drrb = &drr->drr_u.drr_begin;

	if (!context->ic_fp) {
		open_file(context);
	}
	if (fread(drr, sizeof (dmu_replay_record_t), 1, context->ic_fp) != 1) {
		if (ferror(context->ic_fp)) {
			fprintf(stderr, "Error reading record header at "
			    "offset %lu: %s\n", context->ic_offset,
			    strerror(errno));
			exit(1);
		} else {
			fclose(context->ic_fp);
			return (B_FALSE);
		}
	}
	if (context->ic_offset == 0) {
		uint64_t magic = drrb->drr_magic;
		uint64_t versioninfo = drrb->drr_versioninfo;
		if (magic == BSWAP_64(DMU_BACKUP_MAGIC)) {
			SET_ATTR(attrs, CA_BYTESWAPPED);
			versioninfo = BSWAP_64(drrb->drr_versioninfo);
		} else if (magic != DMU_BACKUP_MAGIC) {
			fprintf(stderr, "Invalid ZFS stream, bad magic number "
			    "%lx\n", magic);
			exit(1);
		}
		attrs->ca_feature_flags = DMU_GET_FEATUREFLAGS(versioninfo);
		if (OPTION_ENABLED(attrs, CA_FORBID_DEDUP) &&
		    (STREAM_HAS_FEATURE(attrs, DMU_BACKUP_FEATURE_DEDUP) ||
		    STREAM_HAS_FEATURE(attrs, DMU_BACKUP_FEATURE_DEDUPPROPS)))
		{
			fprintf(stderr, "The input stream is deduplicated, "
			    "but this subcommand does not support deduplicated "
			    "streams. Use zstream redup to reduplicate.\n");
			exit(1);
		}
		if (OPTION_ENABLED(attrs, CA_REQUIRE_DEDUP) &&
		    !STREAM_HAS_FEATURE(attrs, DMU_BACKUP_FEATURE_DEDUP))
		{
			fprintf(stderr, "This subcommand requires a "
			    "deduplicated input stream, but the provided "
			    "stream is not deduplicated.\n");
			exit(1);
		}
	}
	item->dp_payload_size = calc_payload_size(&item->dp_drr, attrs);
	if (item->dp_payload_size > 0) {
		item->dp_payload = safe_malloc(item->dp_payload_size);
		size_t items_read = fread(item->dp_payload,
		    item->dp_payload_size, 1, context->ic_fp);
		if (items_read != 1) {
			fprintf(stderr, "Error reading record payload "
			    " at offset %lu\n", context->ic_offset);
			exit(1);
		}
	} else {
		item->dp_payload = NULL;
	}
	item->dp_stream_offset = context->ic_offset;

	context->ic_offset += sizeof (*drr) + item->dp_payload_size;

	record_stats_t *stats = &attrs->ca_stats_in[drr->drr_type];
	stats->rs_num_records++;
	stats->rs_total_header_bytes += sizeof(dmu_replay_record_t);
	stats->rs_total_payload_bytes += item->dp_payload_size;

	stats = &attrs->ca_totals_in;
	stats->rs_num_records++;
	stats->rs_total_header_bytes += sizeof(dmu_replay_record_t);
	stats->rs_total_payload_bytes += item->dp_payload_size;

	return (B_TRUE);
}

static boolean_t
chain_write(drr_packet_t *item, io_context_t *context, chain_attrs_t *attrs)
{
	(void) attrs;
	if (item == NULL) {
		if (context->ic_fp)
			fclose(context->ic_fp);
		return (B_TRUE);
	}

	if (!context->ic_fp) {
		open_file(context);
	}

	dmu_replay_record_t *drr = &item->dp_drr;

	if (fwrite(drr, sizeof (dmu_replay_record_t), 1, context->ic_fp) != 1) {
		fprintf(stderr, "Error writing record header: %s\n",
		    strerror(errno));
		exit(1);
	} else if (item->dp_payload_size > 0) {
		if (fwrite(item->dp_payload, item->dp_payload_size,
		    1, context->ic_fp) != 1)
		{
			fprintf(stderr, "Error writing payload: %s\n",
			    strerror(errno));
			exit(1);
		} else {
			free(item->dp_payload);
			item->dp_payload = NULL;
		}
	}

	record_stats_t *stats = &attrs->ca_stats_out[drr->drr_type];
	stats->rs_num_records++;
	stats->rs_total_header_bytes += sizeof(dmu_replay_record_t);
	stats->rs_total_payload_bytes += item->dp_payload_size;

	stats = &attrs->ca_totals_out;
	stats->rs_num_records++;
	stats->rs_total_header_bytes += sizeof(dmu_replay_record_t);
	stats->rs_total_payload_bytes += item->dp_payload_size;

	return (B_TRUE);
}

static boolean_t
chain_null_output(drr_packet_t *item, void *ctxt, chain_attrs_t *attrs)
{
	(void) ctxt; (void) attrs;
	if (item && item->dp_payload != NULL && item->dp_payload_size > 0) {
		free(item->dp_payload);
		item->dp_payload = NULL;
		item->dp_payload_size = 0;
	}
	return (B_TRUE);
}

/*
 * Storage for the filename must remain valid during chain execution
 */
static chain_step_t
setup_io(const char *filename, boolean_t for_reading)
{
	int context_num = next_io_context++ % MAX_IO_STREAMS;

	io_contexts[context_num] = (io_context_t) {
		.ic_filename = filename,
		.ic_for_reading = for_reading
	};

	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = 0,
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = &io_contexts[context_num],
		.cs_serial = {
			.process = (zc_serial_process_f *)
			    (for_reading ? chain_read : chain_write),
		}
	});
}

chain_step_t
serial_read_stream(const char *filename)
{
	return (setup_io(filename, B_TRUE));
}

chain_step_t
serial_write_stream(const char *filename)
{
	return (setup_io(filename, B_FALSE));
}

chain_step_t
serial_null_output(void)
{
	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = 0,
		.cs_context = NULL,
		.cs_serial = {
			.process = (zc_serial_process_f *)chain_null_output
		}
	});
}

static boolean_t
chain_checkpoint(drr_packet_t *item, checkpoint_context_t *ctxt, void *dummy)
{
	(void) dummy;

	struct timespec now;
	char buff[32];
	uint64_t delta_b, dbdt;
	double now_sec, delta_t;

	if (item == NULL)
		return (B_TRUE);

	clock_gettime(CLOCK_MONOTONIC, &now);
	now_sec = now.tv_sec + (double)now.tv_nsec / 1E9;
	if (ctxt->cc_last_sec > 1E-9) {
		delta_t = now_sec - ctxt->cc_last_sec;
		if (delta_t < ctxt->cc_period_sec)
			return (B_TRUE);
		delta_b = item->dp_stream_offset - ctxt->cc_last_bytes;
		dbdt = delta_b / delta_t;
		zfs_nicenum(dbdt, buff, sizeof (buff));
		fprintf(stderr, "Checkpoint %s: %s/s\n", ctxt->cc_name, buff);
	}
	ctxt->cc_last_sec = now_sec;
	ctxt->cc_last_bytes = item->dp_stream_offset;
	return (B_TRUE);
}

/*
 * Storage for name must remain valid throughout chain execution
 */
chain_step_t
serial_checkpoint(const char *name)
{
	int ctxt = next_checkpoint_context++ % MAX_IO_STREAMS;

	checkpoint_contexts[ctxt] = (checkpoint_context_t) {
		.cc_name = name,
		.cc_period_sec = 1.0
	};

	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = &checkpoint_contexts[ctxt],
		.cs_serial = {
			.process = (zc_serial_process_f *)chain_checkpoint
		},
	});
}
