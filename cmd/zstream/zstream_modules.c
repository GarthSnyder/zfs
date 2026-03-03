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

#include "zstream_modules.h"

#define F4_DEFAULT_QUEUE_VERIFY	512
#define F4_DEFAULT_QUEUE_WRITE	1024

zstream_chain_t
standard_input_stack(char *infile, size_t f4_queue_length) {
	if (!f4_queue_length) {
		f4_queue_length = F4_DEFAULT_QUEUE_VERIFY;
	}
	return (zstream_chain_t) {
		serial_read_stream(infile),
		parallel_calc_fletcher4(f4_queue_length),
		serial_validate_fletcher4(),
		serial_byteswap(),
		serial_validate_records(),
		CHAIN_TERMINATOR
	}
}

zstream_chain_t
standard_output_stack(char *outfile, size_t f4_queue_length) {
	if (!f4_queue_length) {
		f4_queue_length = F4_DEFAULT_QUEUE_WRITE;
	}
	return (zstream_chain_t) {
		parallel_calc_fletcher4(1024),
		serial_add_fletcher4(),
		serial_write_stream(NULL),
		CHAIN_TERMINATOR
	}
}

zstream_chain_t
concatenate_stack(zstream_chain_t first, zstream_chain_t last)
{
	int i = 0, j = 0;

	while (first[i].cs_type != CS_TERMINATE) i++;
	while (last[j].cs_type != CS_TERMINATE) j++;

	chain_step_t steps[i+j+1];
	memcpy(steps, first, i * sizeof(chain_step_t));
	memcpy(&steps[i], last, j * sizeof(chain_step_t));
	steps[i+j] = (chain_step_t) { .cs_type = CS_TERMINATE };
	return steps;
}
