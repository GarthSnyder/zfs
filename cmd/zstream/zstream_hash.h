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

#ifndef _ZSTREAM_HASH_H
#define	_ZSTREAM_HASH_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/stdtypes.h>

#define	MAX_KEY_SIZE			32

/*
 * This module implements a hash table using linear hashing [Litwin 1980].
 * It runs on top of allocator_t, which allows portions of the hash table to
 * spill from memory to disk when the memory budget is exhausted.
 *
 * See the comments in zstream_hash.c for more details about linear hashing.
 * Briefly, the table grows linearly as items are inserted. When an
 * occupancy threshold is crossed, one bucket is split into two, extending
 * the table by one bucket. This incremental growth is ideal for tables that
 * we'd really like to keep in memory but that might eventually get too big
 * to keep there. As more disk storage is used, the performance of the hash
 * table declines smoothly with the number of entries.
 *
 * Only one thread should make simultaneous calls into the same hash table.
 */

typedef struct linear_hash linear_hash_t;

/*
 * Create a hash table.
 *
 * All stored records are of the same record_size. Storage and retrieval
 * keys are prefixes of the records themselves (of length key_size).
 * Multiple records cannot be stored under the same key, although attempting
 * to do so is harmless. The table does not implement deletions, so the
 * first-inserted record always wins.
 *
 * The record_size must be no more than 8K, and the key prefix can be no
 * longer than MAX_KEY_SIZE bytes.
 *
 * At least one of max_memory and spill_dir should have nonzero/non-NULL
 * values.
 *
 * If max_memory is zero, the hash table is disk-only. lh_init() may round
 * up the requested size to a more convenient value.
 *
 * The spill_dir should be a place where memory can meaningfully spill over
 * to disk, which rules out /tmp on most systems because it's often
 * implemented as a ramdisk. If spill_dir is NULL, the hash table does not
 * use disk backup and may eventually run out of memory.
 */
linear_hash_t *
lh_init(size_t record_size, size_t key_size, size_t max_memory,
    const char *spill_dir);

/*
 * If a second attempt is made to insert the same key, lh_insert() returns
 * B_FALSE and makes no changes to the table.
 */
boolean_t
lh_insert(linear_hash_t *lh, const void *buffer);

/*
 * Set the key bytes of the buffer before calling lh_find(). If the return
 * value is B_TRUE, the record was found and the buffer contains a full copy
 * of the original record.
 */
boolean_t
lh_find(linear_hash_t *lh, void *buffer);

void
lh_fini(linear_hash_t *lh);

#ifdef	__cplusplus
}
#endif

#endif /* _ZSTREAM_HASH_H */
