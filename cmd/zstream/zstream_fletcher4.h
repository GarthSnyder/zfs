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

chain_step_t
parallel_calc_fletcher4(void);

chain_step_t
serial_validate_fletcher4(void);

chain_step_t
serial_add_fletcher4(void);

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_FLETCHER4_H */
