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

#ifndef _ZSTREAM_IO_H
#define _ZSTREAM_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include "zstream_chain.h"
#include "zstream_queue.h"

#define MAX_IO_STREAMS 4

typedef struct {
	dmu_replay_record_t	dp_drr;
	uint8_t			*dp_payload;
	uint32_t		dp_payload_size;
	off_t			dp_stream_offset;
} drr_packet_t;

chain_step_t
serial_read_stream(const char *filename);

chain_step_t
serial_write_stream(const char *filename);

size_t
constant_cost_of_one(drr_packet_t *packet);

size_t
payload_size_as_cost(drr_packet_t *packet);


#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_IO_H */
