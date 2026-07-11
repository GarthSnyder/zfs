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

#ifndef _ZSTREAM_ALLOC_H
#define _ZSTREAM_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * zstream_alloc.[ch] define a thin block storage API that can be backed by
 * either memory, a disk file, or both; the API is the same. An allocator's
 * backing strategy can change on the fly without clients being aware that a
 * transition has occurred.
 *
 * The goal is to use memory as long as it's available but not give up
 * arbitrarily when memory has been exhausted. With sufficient disk space,
 * it is possible to process petabyte-scale streams.
 *
 * Dual-backed allocators keep the first N records in memory and later
 * records on disk. For data that grow linearly but are accessed randomly
 * (e.g., hash tables), this arrangement allows for gradual performance
 * degradation after memory is full.
 *
 * - Blocks are of uniform fixed size.
 * - Every block lives at a 64-bit record_ix_t address.
 * - Record indexes are in block units, not bytes.
 * - You may read a block at any index, even if you haven't written it.
 * - Uninitialized blocks are zero-filled.
 * - Allocators are not thread-safe.
 */

typedef uint64_t record_ix_t;

struct allocator;
typedef struct allocator allocator_t;

typedef struct {
	uint64_t    as_num_ops;        /* Number of stores and retrieves */
	uint64_t    as_mem_used;       /* Current memory use */
	uint64_t    as_num_records;
} allocator_stats_t;

/*
 * Initialize an allocator. The backing_fd can be omitted (set to -1), but
 * since it cannot later be changed, the allocator will be memory-only.
 *
 * The max_memory parameter determines how much RAM the allocator is allowed
 * to consume, in bytes. If it is 0, the allocator will initially be
 * disk-only.
 */
allocator_t *
allocator_init(size_t record_size, size_t max_memory, int backing_fd);

/*
 * An allocator's memory budget can be changed at any time. On a dual-backed
 * allocator, this operation incurs a disk-write cost proportionate to the
 * difference between old and new budgets. However, all data remain intact.
 *
 * If the allocator is memory-only, the new memory budget must be sufficient
 * to accomodate all existing records. If it is not, the program will abort.
 * You can check the current memory consumption with allocator_get_stats().
 *
 * You can also use this function to convert a disk-only allocator to a
 * dual-backed allocator.
 */
int
allocator_set_max_memory(allocator_t *alloc, size_t max_memory);

/*
 * Append a new record and return its index.
 */
record_ix_t
allocator_append(allocator_t *alloc, const void *data);

/*
 * Append a zero-filled record. This operation is potentially more efficient
 * than allocator_append() because zero-filling is implicit.
 */
void
allocator_skip(allocator_t *alloc);

void
allocator_store(allocator_t *alloc, record_ix_t record, const void *buff);

void
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buff);

/*
 * Overwrites stats with information about allocator utilization.
 */
void
allocator_get_stats(allocator_t *alloc, allocator_stats_t *stats);

/*
 * Destroy allocator and free all resources.
 */
void
allocator_destroy(allocator_t *alloc);

#endif /* _ZSTREAM_ALLOC_H */
