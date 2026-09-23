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
 * zstream_alloc.[ch] define a thin storage API that can be backed by
 * memory, a disk file, or both; the API is the same.
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
 * An allocator's memory use can later be trimmed, but not expanded.
 *
 * - Blocks are of uniform fixed size.
 * - Every block lives at a 64-bit record_ix_t address.
 * - Record indexes are in ordinal units, not bytes.
 * - You may read a block at any index, even if you haven't written it.
 * - Uninitialized blocks read as zeros.
 * - Allocators are not thread-safe.
 */

typedef uint64_t record_ix_t;

typedef struct allocator allocator_t;

/*
 * Initialize an allocator.
 *
 * If dir_path is non-NULL, the allocator creates a temporary file there for
 * backup storage on disk.
 *
 * The mem_size parameter determines how much RAM the allocator is allowed
 * to consume, in bytes. If it's 0, the allocator will be disk-only. The
 * memory limit is recorded for future reference, but allocations occur only
 * as memory is actually needed.
 */
allocator_t *
allocator_init(size_t record_size, size_t mem_size, const char *dir_path);

/*
 * The basic API, which is essentially just read() and write() but
 * abstracted across memory and disk.
 */
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
 * Skip ahead one record. Useful if you want to use index 0 as a sentinel.
 * Like other unwritten records, the skipped record is guaranteed to contain
 * zeros if read. Returns the index that was skipped.
 */
record_ix_t
allocator_skip(allocator_t *alloc);

/*
 * Returns the amount of memory actually used. This includes all page
 * allocations, so it's not necessarily the same as the record size * the
 * number of records.
 */
size_t
allocator_memory_used(allocator_t *alloc);

/*
 * This function attempts to trim at least delta_bytes from the allocator's
 * memory use and returns the number of bytes actually trimmed, which may be
 * different because of internal rounding boundaries.
 *
 * A call to this function will always result in some trimming as long as
 * the current memory use and the specified delta_bytes are both nonzero.
 *
 * If the allocator also has disk backing, the allocator will transparently
 * move records trimmed from memory onto disk.
 *
 * The delta is relative to actual memory use (that is, the value returned
 * by allocator_memory_used()), not to the mem_size specified when the
 * allocator was created. A corollary is that you must not call this
 * function on a memory-only allocator. There is nowhere for trimmed data to
 * go, so it will cause the program to abort rather than silently losing
 * data.
 */
size_t
allocator_trim_memory(allocator_t *alloc, size_t delta_bytes);

/*
 * Destroy allocator and free all resources.
 */
void
allocator_destroy(allocator_t *alloc);

#ifdef __cplusplus
}
#endif

#endif /* _ZSTREAM_ALLOC_H */
