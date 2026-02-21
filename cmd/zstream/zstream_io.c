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

#include "zstream_io.h"
#include "zstream_chain.h"
#include "zstream_shared.h"

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
		    "Name a file or pipe to another command.\n");
		exit(1);
	} else {
		context->ic_fp = stdout;
	}
}

static boolean_t
chain_read(drr_packet_t *item, io_context_t *ctxt, chain_attrs_t chain)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	size_t payload_size;

	if (!ctxt->ic_fp) {
		open_file(ctxt);
	}
	if (fread(drr, sizeof(dmu_replay_record_t), 1,
		ctxt->ic_fp) == 0)
	{
		if (ferror(ctxt->ic_fp)) {
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
	/* TODO: This code probably needs byteswapping */
	switch (drr->drr_type) {
	case DRR_OBJECT:
		payload_size = DRR_OBJECT_PAYLOAD_SIZE(&drr->drr_u.drr_object);
		break;
	case DRR_WRITE:
		payload_size = DRR_WRITE_PAYLOAD_SIZE(&drr->drr_u.drr_write);
		break;
	case DRR_SPILL:
		payload_size = DRR_SPILL_PAYLOAD_SIZE(&drr->drr_u.drr_spill);
		break;
	case DRR_WRITE_EMBEDDED:
		payload_size =
			P2ROUNDUP(drr->drr_u.drr_write_embedded.drr_psize, 8);
		break;
	default:
		payload_size =(chain->ca_flags & CA_BYTESWAPPED) ?
			BSWAP_32(drr->drr_payloadlen) : drr->drr_payloadlen;
	}
	if (payload_size) {
		item->dp_payload = safe_malloc(payload_size);
		size_t items_read = fread(item->dp_payload,
			payload_size, 1, ctxt->ic_fp);
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

