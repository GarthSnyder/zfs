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

#ifndef _ZSTREAM_MODULES_H
#define _ZSTREAM_MODULES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zstream_blake3.h"
#include "zstream_byteswap.h"
#include "zstream_chain.h"
#include "zstream_fletcher4.h"
#include "zstream_io.h"
#include "zstream_shared.h"
#include "zstream_validate.h"
#include "zstream_compress.h"

#define STANDARD_INPUT_STACK(infile, f4_queue_length) 			\
	serial_read_stream(infile),					\
	PARALLEL_CALC_FLETCHER4(f4_queue_length),			\
	SERIAL_VALIDATE_FLETCHER4(),					\
	serial_byteswap(),						\
	serial_validate_records()

#define STANDARD_OUTPUT_STACK(outfile, f4_queue_length) 		\
	PARALLEL_CALC_FLETCHER4(f4_queue_length),			\
	SERIAL_ADD_FLETCHER4(),						\
	serial_write_stream(outfile),					\
	CHAIN_TERMINATOR

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_MODULES_H */

