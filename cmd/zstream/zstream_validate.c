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

#include <assert.h>		/* VERIFY3U, VERIFY0			*/
#include <stdio.h>		/* fprintf, stderr, NULL		*/
#include <stdlib.h>		/* exit					*/
#include <sys/stdtypes.h>	/* B_TRUE, boolean_t			*/
#include <sys/zfs_ioctl.h>	/* dmu_replay_record, drr_object...	*/
#include <sys/zio_compress.h>	/* zio_compress				*/

#include "zstream_io.h"		/* drr_packet_t				*/
#include "zstream_validate.h"	/* serial_validate_records		*/

/*
 * Validate consistency and well-formedness of the actual DRR records. I
 * have swept all the existing validation code into this module, but it's
 * still pretty spare.
 */

#define MAX_VALIDATIONS 4

typedef struct {
	int	nesting;
} validate_context_t;

static validate_context_t 	contexts[MAX_VALIDATIONS];
static int			next_context = 0;

static boolean_t
chain_validate_records(drr_packet_t *item, validate_context_t *context)
{
	struct dmu_replay_record *drr	 = &item->dp_drr;
	struct drr_write *drrw		 = &drr->drr_u.drr_write;
	struct drr_object *drro 	 = &drr->drr_u.drr_object;

	if (item == NULL || !OPTION_ENABLED(chain_attrs, CA_DO_NOT_VALIDATE)) {
		return (B_TRUE);
	}
	if (item->dp_stream_offset == 0 && drr->drr_type != DRR_BEGIN) {
		fprintf(stderr, "Warning: first record is not DRR_BEGIN\n");
	}

	if (drr->drr_type == DRR_BEGIN) {
		VERIFY0(context->nesting);
		context->nesting++;
	} else if (drr->drr_type == DRR_END) {
		VERIFY3U(context->nesting, >=, 0);
		context->nesting--;
	} else if (drr->drr_type > DRR_NUMTYPES) {
		fprintf(stderr, "Unknown record type: %d\n", drr->drr_type);
		exit(1);
	} else {
		VERIFY3U(context->nesting, ==, 1);
	}

	switch (drr->drr_type) {
	case DRR_BEGIN:
		VERIFY3U(item->dp_payload_size, <=, 1UL << 28);
		break;

	case DRR_OBJECT:
		if (chain_attrs->ca_feature_flags & DMU_BACKUP_FEATURE_RAW &&
		    drro->drr_bonuslen > drro->drr_raw_bonuslen)
		{
			fprintf(stderr,
			    "Warning: Object %zu has bonuslen = "
			    "%u > raw_bonuslen = %u\n\n",
			    drro->drr_object, drro->drr_bonuslen,
			    drro->drr_raw_bonuslen);
		}
		break;

	case DRR_WRITE:
		if (drrw->drr_compressiontype >= ZIO_COMPRESS_FUNCTIONS) {
		    fprintf(stderr, "Invalid compression type: %d\n",
		    	drrw->drr_compressiontype);
		    exit(3);
		}
		break;

	default:
		break;
	}
	return (B_TRUE);
}

chain_step_t
serial_validate_records(void)
{
	int context_ix = next_context++ % MAX_VALIDATIONS;
	validate_context_t *context = &contexts[context_ix];

	return (chain_step_t) {
	    .cs_type = CS_SERIAL,
	    .cs_in_size = sizeof(drr_packet_t),
	    .cs_out_size = sizeof(drr_packet_t),
	    .cs_context = context,
	    .cs_serial = {
		.process = (zc_serial_process_f *)chain_validate_records,
	    }
	};
}
