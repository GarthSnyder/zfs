// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#ifndef _ZSTREAM_HASH_DEBUG_H
#define _ZSTREAM_HASH_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zstream_hash_impl.h"
#include "zstream_hash_stats.h"

#define START_VALIDATION(lh) 						\
	boolean_t started_ok = B_TRUE;					\
	if (lh->lh_validate) {						\
		started_ok = lh_validate(lh);				\
	}

#define END_VALIDATION(lh)						\
	if (lh->lh_validate && !lh_validate(lh) && started_ok) {	\
		fprintf(stderr, "%s broke the hash table\n", __func__);	\
	}

boolean_t
entry_iterator_next(entry_iterator_t *iter, boolean_t extend);

boolean_t
lh_validate(linear_hash_t *lh);

uint64_t
total_io_ops(linear_hash_t *lh);

void
begin_ops_tracking(linear_hash_t *lh, op_stats_t *bin);

void
update_ops_tracking(linear_hash_t *lh);

void
complete_ops_tracking(linear_hash_t *lh);

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_HASH_DEBUG_H */
