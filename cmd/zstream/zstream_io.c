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

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/zfs_ioctl.h>

#include "zstream_io.h"
#include "zstream_chain.h"
#include "zstream_shared.h"

#define STDIO_BUFSIZE 1024 * 1024

/* Init only the filename, chain_read_stream will prepare the FILE *. */
typedef struct {
	const char	*ic_filename;
	FILE		*ic_fp;
	char		*ic_stdio_buffer;
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
serial_read_stream(const char *filename) {
	return (setup_io(filename, B_TRUE));
}

chain_step_t
serial_write_stream(const char *filename) {
	return (setup_io(filename, B_FALSE));
}

static chain_step_t
setup_io(const char *filename, boolean_t for_reading) {
	int context = next_io_context % MAX_IO_STREAMS;
	next_io_context++;
	io_contexts[context] = (io_context_t) {
		.ic_filename = filename,
		.ic_for_reading = for_reading
	};
	return (chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof(drr_packet_t),
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
			perror(context->ic_filename);
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
		    "Capture output to a file or pipe to another command.\n");
		exit(1);
	} else {
		context->ic_fp = stdout;
	}
	context->ic_stdio_buffer = safe_malloc(STDIO_BUFSIZE);
	setbuffer(context->ic_fp, context->ic_stdio_buffer, STDIO_BUFSIZE);
}

/*
 * Extract the payload size from a replay record that is potentially byteswapped. We
 * want to leave the bulk of byteswapping to another module, so just take a quick,
 * nondestructive peek.
 *
 * Record-specific macros such as DRR_WRITE_PAYLOAD_SIZE are not byteswap-aware.
 * However, with the exception of DRR_OBJECT_PAYLOAD_SIZE, they happen to work
 * with post-swapping since they are switching on either a uint8_t value or 0.
 */
static size_t
calc_payload_size(dmu_replay_record_t *drr, chain_attrs_t chain)
{
	struct drr_object *drro 	 = &drr->drr_u.drr_object;
	struct drr_write *drrw 		 = &drr->drr_u.drr_write;
	struct drr_spill *drrs 		 = &drr->drr_u.drr_spill;
	struct drr_write_embedded *drrwe = &drr->drr_u.drr_write_embedded;

	boolean_t swap = !!(chain->ca_flags & CA_BYTESWAPPED);
	uint32_t drr_type = swap ? BSWAP_32(drr->drr_type) : drr->drr_type;
	uint32_t size;

	switch (drr_type) {
	case DRR_OBJECT:
		if (swap && drro->drr_raw_bonuslen) {
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
chain_read(drr_packet_t *item, io_context_t *ctxt, chain_attrs_t chain)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_begin *drrb	 = &drr->drr_u.drr_begin;

	if (!ctxt->ic_fp) {
		open_file(ctxt);
	}
	if (fread(drr, sizeof(dmu_replay_record_t), 1, ctxt->ic_fp) == 0) {
		if (ferror(ctxt->ic_fp)) {
			fprintf(stderr, "Error reading stream: %s\n",
			    strerror(errno));
			exit(1);
		} else {
			fclose(ctxt->ic_fp);
			free(ctxt->ic_stdio_buffer);
			return B_FALSE;
		}
	}
	if (ctxt->ic_offset == 0) {
		uint64_t magic = drrb->drr_magic;
		uint64_t versioninfo = drrb->drr_versioninfo;
		if (magic == BSWAP_64(DMU_BACKUP_MAGIC)) {
			chain->ca_flags |= CA_BYTESWAPPED;
			versioninfo = BSWAP_64(drrb->drr_versioninfo);
		} else if (magic != DMU_BACKUP_MAGIC) {
			fprintf(stderr, "Invalid ZFS stream, bad magic "
				"number %lx\n", magic);
			exit(1);
		}
		uint64_t fflags = DMU_GET_FEATUREFLAGS(versioninfo);
		if (fflags & (DMU_BACKUP_FEATURE_DEDUP |
			DMU_BACKUP_FEATURE_DEDUPPROPS))
		{
			chain->ca_flags |= CA_DEDUPED;
		}
	}
	uint32_t payload_size = calc_payload_size(&item->dp_drr, chain);
	if (payload_size) {
		item->dp_payload = safe_malloc(payload_size);
		size_t items_read = fread(item->dp_payload,
			payload_size, 1, ctxt->ic_fp);
		if (items_read != 1) {
			fprintf(stderr, "Error reading record payload "
				" at offset %lu\n", ctxt->ic_offset);
			exit(1);
		}
	} else {
		item->dp_payload = NULL;
	}
	item->dp_payload_size = payload_size;
	item->dp_stream_offset = ctxt->ic_offset;
	ctxt->ic_offset += sizeof(*drr) + payload_size;
	return B_TRUE;
}

static boolean_t
chain_write(drr_packet_t *item, io_context_t *ctxt, chain_attrs_t attrs)
{
	(void) attrs;
	dmu_replay_record_t *drr = &item->dp_drr;

	if (!ctxt->ic_fp) {
		open_file(ctxt);
	}
	if (!item) {
		fclose(ctxt->ic_fp);
		free(ctxt->ic_stdio_buffer);
		return B_TRUE;
	}
	if (fwrite(drr, sizeof(dmu_replay_record_t), 1,
		ctxt->ic_fp) != 1)
	{
		fprintf(stderr, "Error writing record: %s\n",
		    strerror(errno));
		exit(1);
	} else if (item->dp_payload_size > 0) {
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

size_t
constant_cost_of_one(drr_packet_t *packet, void *context) {
	(void) context; (void) packet;
	return (1);
}

size_t
payload_size_as_cost(drr_packet_t *packet, void *context) {
	(void) context;
	return (packet->dp_payload_size);
}

