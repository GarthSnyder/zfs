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
#include <sys/blake3.h>
#include "zstream_blake3.h"
#include "zstream_util.h"

static void
chain_calc_blake3(drr_blake3_t *item, void *context)
{
	(void) context;
	drr_packet_t *base = &item->dp_base;
	VERIFY3U(base->dp_payload_size, >, 0);

	BLAKE3_CTX ctx;
	Blake3_Init(&ctx);
	Blake3_Update(&ctx, base->dp_payload, base->dp_payload_size);
	Blake3_Final(&ctx, (uint8_t *)&item->dp_blake3_payload);
}

chain_step_t
parallel_calc_blake3(void) {
	return (chain_step_t) {
		.cs_type = CS_PARALLEL,
		.cs_in_size = sizeof(drr_packet_t),
		.cs_out_size = sizeof(drr_blake3_t),
		.cs_parallel = {
			.csp_queue_length = 512,
			.csp_batch_budget = 64 * 1024,
			.csp_process = (zq_process_item_f *)chain_calc_blake3,
			.csp_cost = (zq_estimate_cost_f *)payload_size_as_cost
		}
	};
}

