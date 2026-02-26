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

typedef int64_t record_ix_t;
struct allocator;
typedef struct allocator allocator_t;

typedef struct {
	uint64_t    as_num_ops;        /* Number of stores and retrieves */
	uint64_t    as_mem_used;       /* Current memory use */
	uint64_t    as_num_records;
} allocator_stats_t;

/*
 * Initialize an allocator. Record size is required. If a file handle is
 * provided and max_memory is 0, the allocator will use disk backing.
 * If both a memory limit and a file handle are supplied, the allocator
 * is a convertible allocator that starts by using memory but converts
 * to disk storage if memory use is exceeded. A conversion can also be
 * triggered externally by allocator_convert_to_disk.
 *
 * If no file handle is supplied, the allocator will be memory-only. 
 * max_memory must be specified and nonzero.
 */
allocator_t *
allocator_init(size_t record_size, size_t max_memory, FILE *file);

/*
 * Convert a memory-backed allocator to disk-backed.
 * The file handle must have been provided during initialization.
 * A negative return value indicates an error.
 */
int
allocator_convert_to_disk(allocator_t *alloc);

/*
 * Append a new data record. A negative return value indicates
 * an error.
 */
record_ix_t
allocator_append(allocator_t *alloc, const void* data);

/*
 * Skip a record (allocate space but leave it zero-filled). A 
 * negative return value indicates an error.
 */
record_ix_t
allocator_skip(allocator_t *alloc);

/*
 * Store data at a specific record index (can skip records). A
 * negative return value indicates an error.
 */
record_ix_t
allocator_store(allocator_t *alloc, record_ix_t record, const void *data);

/*
 * Retrieve a data record. A negative value indicates an error.
 */
record_ix_t
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buffer);

void
allocator_get_stats(allocator_t *alloc, allocator_stats_t *stats);

/*
 * Destroy allocator and free all resources.
 * For memory allocators: frees all allocated memory.
 * For disk allocators: closes and deletes the file.
 *
 * @param alloc Allocator instance (may be NULL)
 */
void
allocator_destroy(allocator_t *alloc);

#endif /* _ZSTREAM_ALLOC_H */
