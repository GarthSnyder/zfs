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

#ifndef _ZSTREAM_BLAKE3_H
#define _ZSTREAM_BLAKE3_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/zfs_ioctl.h>
#include "zstream_io.h"

#define PARALLEL_CALC_BLAKE3()
	((chain_step_t) {						      \
		.cs_type = CS_PARALLEL,					      \
		.cs_in_size = sizeof(drr_packet_t),			      \
		.cs_out_size = sizeof(drr_blake3_t),			      \
		.cs_parallel = {					      \
		    .csp_queue_length = 512,				      \
		    .csp_batch_budget = 64 * 1024,			      \
		    .csp_process = (zq_process_item_f *)chain_calc_blake3,    \
		    .csp_cost = (zq_estimate_cost_f *)payload_size_as_cost    \
		}							      \
	})

typedef struct {
	drr_packet_t	dp_base;
	zio_cksum_t	dp_blake3_payload;
} drr_blake3_t;

chain_step_t
parallel_calc_blake3(void);

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_BLAKE3_H */
