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

/*
 * zstream_chain module for calculating, validating, and inscribing
 * Fletcher4 checksums.
 *
 * serial_validate_fletcher4() validates record checksums against the
 * running stream checksum and fails loudly on any mismatch.
 *
 * serial_add_fletcher4() inscribes record checksums from the running
 * stream checksum, replacing whatever was there before.
 */

#define MAX_FLETCHER_4 8	/* Max checksum ops in one chain */

chain_step_t
serial_validate_fletcher4(void);

chain_step_t
serial_add_fletcher4(void);

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_FLETCHER4_H */
