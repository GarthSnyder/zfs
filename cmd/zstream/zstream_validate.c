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

#include "zstream.h"
#include "zstream_validate.h"
#include "zstream_io.h"
#include "zstream_shared.h"

#define MAX_VALIDATIONS 4

typedef struct {
	int	nesting;
} validate_context_t;

static validate_context_t 	contexts[MAX_VALIDATIONS];
static int			next_context = 0;

/*
 * This is currently just a skeleton into which validation scraps from elsewhere can
 * be integrated. Overall, it looks like there may not be enough validation going on
 * in general to make this module necessary.
 */
static boolean_t
chain_validate_records(drr_packet_t *item, validate_context_t *context,
	chain_attrs_t attrs)
{
	(void) attrs;
	struct dmu_replay_record *drr	 = &item->dp_drr;
	struct drr_write *drrw		 = &drr->drr_u.drr_write;

	if (!item) {
		return B_TRUE;
	}
	if (!item->dp_stream_offset && drr->drr_type != DRR_BEGIN) {
		fprintf(stderr, "Warning: first record is not DRR_BEGIN\n");
	}

	switch (drr->drr_type) {
	case DRR_BEGIN:
	{
		VERIFY3U(item->dp_payload_size, <=, 1UL << 28);
		VERIFY0(context->nesting++);
		break;
	}

	case DRR_END:
	{
		context->nesting--;
		VERIFY3U(context->nesting, >=, 0);
		break;
	}

	case DRR_OBJECT:
	{
		VERIFY3U(context->nesting, ==, 1);
		break;
	}

	case DRR_SPILL:
	{
		VERIFY3U(context->nesting, ==, 1);
		break;
	}

	case DRR_WRITE_BYREF:
	{
		VERIFY3U(context->nesting, ==, 1);
		break;
	}

	case DRR_WRITE:
	{
		VERIFY3U(context->nesting, ==, 1);
		if (drrw->drr_compressiontype >= ZIO_COMPRESS_FUNCTIONS) {
		    fprintf(stderr, "Invalid compression type: %d\n",
		    	drrw->drr_compressiontype);
		    exit(3);
		}
		break;
	}

	case DRR_WRITE_EMBEDDED:
	{
		VERIFY3U(context->nesting, ==, 1);
		break;
	}

	case DRR_REDACT:
	{
		VERIFY3U(context->nesting, ==, 1);
		break;
	}

	case DRR_FREEOBJECTS:
	case DRR_FREE:
	case DRR_OBJECT_RANGE:
	{
		VERIFY3U(context->nesting, ==, 1);
		break;
	}

	default:
	{
		fprintf(stderr, "Unknown record type: %d\n", drr->drr_type);
		exit(1);
	}}
	return B_TRUE;
}

chain_step_t
serial_validate_records(void)
{
	int which = next_context % MAX_VALIDATIONS;
	validate_context_t *context = &contexts[which];

	return (chain_step_t) {
	    .cs_type = CS_SERIAL,
	    .cs_in_size = sizeof(drr_packet_t),
	    .cs_out_size = sizeof(drr_packet_t),
	    .serial = {
		.css_process = (zc_serial_process_f *)chain_validate_records,
		.css_context = context
	    }
	};
}
