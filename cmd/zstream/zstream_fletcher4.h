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

#include "zstream_chain.h"
#include "zstream_chain_types.h"

#define MAX_FLETCHER_4 4	/* Max in one chain */

chain_step_t
parallel_calc_fletcher4();

chain_step_t
serial_validate_fletcher4();

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_FLETCHER4_H */
