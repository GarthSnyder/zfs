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

#define	MAX_KEY_SIZE 32

#define	lh_find(lh, buff)	lh_findsert(lh, buff, buff, FIND)
#define	lh_insert(lh, buff)	lh_findsert(lh, buff, NULL, INSERT)
#define	lh_upsert(lh, buff)	lh_findsert(lh, buff, NULL, UPSERT)

/*
 * This module implements a linear hash table [Litwin 1980]. It runs on top
 * of allocator_t, which allows portions of the hash table to spill from
 * memory to disk when the memory budget is exhausted.
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

typealias enum {
	FOUND, NOT_FOUND, INSERTED, UPDATED
} lh_result_t;

/*
 * Creates a hash table.
 *
 * All stored records are of the same record_size. Storage and retrieval
 * keys are prefixes of the records themselves (key_size is the prefix
 * length). Multiple records cannot be stored under the same key, but
 * existing records can be updated.
 *
 * The record_size must be no more than 8K, and the key prefix can be no
 * longer than MAX_KEY_SIZE (32) bytes.
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
 * lh_find() searches the table for the key passed in buff. buff should
 * point to a region large enough to accommodate a full record. However,
 * only the key fields need be filled out. If the key is found, lh_find()
 * places the full record in buff and returns FOUND. Otherwise, it returns
 * NOT_FOUND.
 *
 * buff may have been modified even if NOT_FOUND is returned, in which case
 * its contents are undefined.
 */
lh_result_t
lh_find(linear_hash_t *lh, void *buff);

/*
 * lh_find_or_insert() searches the table for the key found in inbuff. If
 * found, it copies the existing record to outbuff and returns FOUND. If the
 * key is not found, it inserts the contents of inbuff and returns INSERTED.
 *
 * You may pass NULL for the outbuff if you don't care about a previous
 * value stored under this key.
 *
 * inbuff is not modified. If non-NULL, outbuff may have been modified even
 * if INSERTED is returned, in which case its contents are undefined.
 */
lh_result_t
lh_find_or_insert(linear_hash_t *lh, const void *inbuff, void *outbuff);

/*
 * lh_upsert() searches the table for the key found in inbuff. If found, it
 * copies the existing record to outbuff, replaces the existing table entry
 * with the full record from inbuff, and returns UPDATED. If the key is not
 * found, inserts the full contents of inbuff and returns INSERTED.
 *
 * You may pass NULL for the outbuff if you don't care about a previous
 * value under this key.
 *
 * inbuff is not modified. If non-NULL, outbuff may have been modified even
 * if INSERTED is returned, in which case its contents are undefined.
 */
lh_result_t
lh_upsert(linear_hash_t *lh, const void *inbuff, void *outbuff);

void
lh_fini(linear_hash_t *lh);

#ifdef	__cplusplus
}
#endif

#endif /* _ZSTREAM_HASH_H */
