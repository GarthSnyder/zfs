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

#ifndef _ZSTREAM_HASH_H
#define	_ZSTREAM_HASH_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/*
 * This code implements linear hashing with 64-bit hash keys. It runs on top
 * of allocator_t, which allows a structured approach to memory management
 * and allows the hash table to expand indefinitely as long as disk storage
 * remains available.
 *
 * For more details on linear hashing, see the Wikipedia article or the
 * comments in zstream_hash.c. Briefly, the table grows (roughly) linearly
 * as items are inserted. When an occupancy threshold is crossed, one bucket
 * is split into two. This incremental growth is ideal for tables that we'd
 * really like to keep in memory but that might eventually get too big to
 * keep there. As more disk storage is used, the performance of the hash
 * table declines smoothly with the number of entries.
 *
 * API clients are not required to memory-manage iterators, nor are they
 * obligated to pursue iterations to completion. In return, callers must
 * limit themselves to MAX_LH_ITERATORS concurrent iterators.
 *
 * Hash keys are 64-bit values, and you must supply them yourself. If you
 * want to use longer hash keys, give the linear hash 64-bit digests and
 * check returned records against the full hash value.
 *
 * This header file contains the struct definitions and #defines needed to
 * compile the core linear hash implementation found in zstream_hash.c.
 *
 * To enable validation and statistical profiling, add the following
 * definition:
 *
 * #define LH_STATS_AND_VALIDATION
 */

#define LH_STATS_AND_VALIDATION
#define	MAX_LH_ITERATORS 8

struct linear_hash;
typedef struct linear_hash linear_hash_t;

struct lh_iterator;
typedef struct lh_iterator lh_iterator_t;

/*
 * The cache_dir should be a place where memory can meaningfully spill over
 * to disk, which rules out /tmp on most systems because it's often
 * implemented as a ramdisk. The default is /var/tmp.
 */
linear_hash_t *
lh_init(size_t record_size, size_t max_memory, const char *cache_dir);

void
lh_insert(linear_hash_t *lh, uint64_t hash, const void* data);

/*
 * Set up retrieval for all data records with a given hash value.
 * Use lh_retrieve_next() to iterate through matching records.
 */
lh_iterator_t *
lh_initiate_retrieve(linear_hash_t *lh, uint64_t hash);

/*
 * Get the next data record matching the hash. If the return value is
 * B_FALSE, there are no more records and the data buffer is invalid.
 */
boolean_t
lh_retrieve_next(lh_iterator_t *iter, void *buffer);

void
lh_destroy(linear_hash_t *lh);

#ifdef	__cplusplus
}
#endif

#endif /* _ZSTREAM_HASH_H */
