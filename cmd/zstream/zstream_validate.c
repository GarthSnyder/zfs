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
#include "zstream_shared.h"

/*
 * This is currently just a skeleton into which validation scraps from elsewhere can
 * be integrated. Overall, it looks like there may not be enough validation going on
 * in general to make this module necessary.
 */
static boolean_t
chain_validate_records(drr_packet_t *item, void *context, chain_attrs_t chain)
{
	(void) context; (void) chain;
	struct dmu_replay_record *drr	 = &item->dp_drr;
	int nesting = 0;

	if (!item) {
		return B_TRUE;
	}
	if (!item->dp_payload_size && drr->drr_type != DRR_BEGIN) {
		fprintf(stderr, "Warning: first record is not DRR_BEGIN\n");
	}

	switch (drr->drr_type) {
	case DRR_BEGIN:
	{
		VERIFY3U(item->dp_payload_size, <=, 1UL << 28);
		VERIFY0(nesting++);
		break;
	}

	case DRR_END:
	{
		nesting--;
		VERIFY3U(nesting, >=, 0);
		break;
	}

	case DRR_OBJECT:
	{
		VERIFY3U(nesting, ==, 1);
		break;
	}

	case DRR_SPILL:
	{
		VERIFY3U(nesting, ==, 1);
		break;
	}

	case DRR_WRITE_BYREF:
	{
		VERIFY3U(nesting, ==, 1);
		break;
	}

	case DRR_WRITE:
	{
		VERIFY3U(nesting, ==, 1);
		break;
	}

	case DRR_WRITE_EMBEDDED:
	{
		VERIFY3U(nesting, ==, 1);
		break;
	}

	case DRR_REDACT:
	{
		VERIFY3U(nesting, ==, 1);
		break;
	}

	case DRR_FREEOBJECTS:
	case DRR_FREE:
	case DRR_OBJECT_RANGE:
	{
		VERIFY3U(nesting, ==, 1);
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
	return (chain_step_t) {
	.cs_type = CS_SERIAL,
	.cs_in_size = sizeof(drr_packet_t),
	.cs_out_size = sizeof(drr_packet_t),
	.serial = {
		.css_process = (zc_serial_process_f *)chain_validate_records,
	}
	};
}
