// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

/*
 * This is the internal header for allocator_t. It exposes the layout
 * calculation so that selftests can check it directly. In particular, it
 * allows the selftests to check page sizes other than the host's. None of
 * this is part of the public API in zstream_alloc.h.
 */

#ifndef _ZSTREAM_ALLOC_IMPL_H
#define	_ZSTREAM_ALLOC_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "zstream_alloc.h"

/*
 * Inputs to the record-size rounding calculation in calc_granularities().
 * See the discussion there for details.
 */
#define	MAX_PAGES_PER_MEMORY_UNIT	33	    /* ~128K with 4K pages */

/*
 * (Rough) granularity at which memory pages are converted from PROT_NONE to
 * PROT_READ | PROT_WRITE, in bytes. This value will be rounded up to a
 * multiple of the system page size.
 */
#define	TARGET_FRONTIER_MOVEMENT	(4 << 20)   /* 4MB */

/*
 * See the discussion of granularities in zstream_alloc.c.
 */
typedef struct {
	size_t		memory;			/* Memory/disk boundary */
	size_t		frontier;		/* Writable frontier w/in mem */
	size_t		records_per_mem_unit;	/* # of records that fit */
} granularity_t;

granularity_t
calc_granularities(size_t record_size, size_t pagesize);

#ifdef __cplusplus
}
#endif

#endif /* _ZSTREAM_ALLOC_IMPL_H */
