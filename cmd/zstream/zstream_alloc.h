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

#ifndef _ZSTREAM_ALLOC_H
#define	_ZSTREAM_ALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/*
 * An allocator_t defines a thin record storage API that can be backed by
 * memory, a disk file, or both; the API is identical.
 *
 * Dual-backed allocators keep the first N records in memory and later
 * records on disk. For data that grows linearly but is accessed randomly
 * (e.g., linear hash tables), this arrangement allows for gradual
 * performance degradation after memory becomes full.
 *
 * The goal is to use memory as long as it's available but not give up
 * arbitrarily when memory has been exhausted. With sufficient disk space,
 * it's possible to process streams of arbitrary size.
 *
 *   - Blocks are of uniform fixed size (sizes may vary per allocator).
 *   - Every block lives at a 64-bit record_ix_t address.
 *   - Record indexes are in ordinal units, not bytes.
 *   - Record indexes with the high bit set are reserved; don't use them.
 *   - Allocators are not thread-safe.
 *   - You may read a block at any valid index, even if never written.
 *   - Uninitialized records read as zeros.
 *
 * All allocators are members of an allocator group, which may be a group
 * of 1. Allocators in a group share a collective memory limit. Regardless
 * of the data stored in the individual allocators, the group will never use
 * more memory than the limit specified at the time the group was created
 * (modulo rounding; see below).
 */

#define	MAX_ALLOCATOR_GROUPS		6
#define	MAX_ALLOCATORS_PER_GROUP	6
#define	MAX_RECORD_SIZE			(8 << 10)	/* 8KB */

typedef uint64_t record_ix_t;

typedef struct allocator allocator_t;

/*
 * Create a set of num_allocators allocator_t's. One allocator_t is created
 * for each value in the record_sizes array.
 *
 * Over time, a group may internally reassign memory, forcing some records
 * to disk. (If there is no disk backup, this becomes a fatal error.) Memory
 * rebalancing is transparent to the client.
 *
 * Records are evicted from memory one allocator at a time.
 * allocator_group_init()[0] is the first allocator asked to yield memory.
 * When it has no more memory to surrender, memory will be drawn from
 * allocator_group_init()[1], and so on. The ultimate end state under high
 * load is that the last allocator will hold all the group's memory, with
 * other allocators having functionally become disk-only. If this is not the
 * behavior you want, create more than one group.
 *
 * - Memory is managed in page clusters of 1MB (or one page, on systems with
 *   larger pages), so 8MB is a good working minimum for mem_limit.
 *
 * - The requested mem_limit will be rounded up to the nearest multiple of
 *   the cluster size.
 *
 * - No record size can be larger than MAX_RECORD_SIZE (8KB).
 *
 * - The returned array of allocator_t's is stable and valid until
 *   the allocator group is finalized.
 */
allocator_t **
allocator_group_init(const size_t *record_sizes, unsigned num_allocators,
    size_t mem_limit, const char *dir_path);

void
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buff);

void
allocator_store(allocator_t *alloc, record_ix_t record, const void *buff);

/*
 * Append a new record and return its index.
 */
record_ix_t
allocator_append(allocator_t *alloc, const void *data);

/*
 * Skip ahead one record. Returns the index that was skipped.
 */
record_ix_t
allocator_skip(allocator_t *alloc);

/*
 * Destroy an allocator group. You can name any allocator in the group.
 */
void
allocator_group_fini(allocator_t *alloc);

#ifdef __cplusplus
}
#endif

#endif /* _ZSTREAM_ALLOC_H */
