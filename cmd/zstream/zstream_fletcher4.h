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

#ifndef _ZSTREAM_FLETCHER4_H
#define _ZSTREAM_FLETCHER4_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zstream_io.h"

#define MAX_FLETCHER_4 8	/* Max cksum ops in one chain */

#define PARALLEL_CALC_FLETCHER4(queue_length)				      \
	((chain_step_t) {						      \
		.cs_type = CS_PARALLEL,					      \
		.cs_in_size = sizeof(drr_packet_t),			      \
		.cs_out_size = sizeof(drr_fletcher4_t),			      \
		.cs_parallel = {					      \
		    .csp_queue_length = queue_length,			      \
		    .csp_batch_budget = 256 * 1024,			      \
		    .csp_process = (zq_process_item_f *)chain_calc_fletcher4, \
		    .csp_cost = (zq_estimate_cost_f *)payload_size_as_cost    \
		}							      \
	})

#define SERIAL_VALIDATE_FLETCHER4()					      \
	((chain_step_t) {						      \
		.cs_type = CS_SERIAL,					      \
		.cs_in_size = sizeof(drr_fletcher4_t),			      \
		.cs_out_size = sizeof(drr_packet_t),			      \
		.cs_context = new_fletcher4_context(F4_VALIDATE),	      \
		.cs_serial = {						      \
		    .css_process = (zc_serial_process_f *)chain_fletcher4,    \
		}							      \
	})

#define SERIAL_ADD_FLETCHER4()						      \
	((chain_step_t) {						      \
		.cs_type = CS_SERIAL,					      \
		.cs_in_size = sizeof(drr_fletcher4_t),			      \
		.cs_out_size = sizeof(drr_packet_t),			      \
		.cs_context = new_fletcher4_context(F4_SET),		      \
		.cs_serial = {						      \
		    .css_process = (zc_serial_process_f *)chain_fletcher4,    \
		}							      \
	})

/*
 * Fletcher 4 incremental blocks are limited to 8MB in size, and some ZFS
 * payloads can be significantly larger than this, notably DRR_WRITE blocks
 * and DRR_BEGIN blocks with long nvlists. The single preallocated checksum
 * block will capture the majority of cases. If more checksum blocks are
 * needed, they must be allocated dynamically.
 *
 * It's likely pointless to precalculate the header checksum since the
 * amount of data involved is small. This allows payloadless records to
 * circumvent multithreaded dispatching altogether.
 */
typedef struct {
	drr_packet_t	dp_base;
	zio_cksum_t	dp_fletcher4_payload;
	zio_cksum_t	*dp_fletcher4_overflow;
} drr_fletcher4_t;

typedef enum { F4_SET, F4_VALIDATE } fletcher4_op_t;

struct fletcher4_context;
typedef struct fletcher4_context fletcher4_context_t;

fletcher4_context_t *
new_fletcher4_context(fletcher4_op_t operation);

void
chain_calc_fletcher4(drr_fletcher4_t *item, void *context);

boolean_t
chain_fletcher4(drr_fletcher4_t *item, fletcher4_context_t *context,
	chain_attrs_t chain);

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_FLETCHER4_H */
