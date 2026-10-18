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

#define	lh_find(lh, buff)	lh_record(lh, LH_FIND, (buff), (buff))
#define	lh_insert(lh, buff)	lh_record(lh, LH_FIND_OR_INSERT, (buff), NULL)
#define	lh_upsert(lh, buff)	lh_record(lh, LH_UPSERT, (buff), NULL)

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
 * A linear_hash_t must not be used by more than one thread at a time.
 */

typedef struct linear_hash linear_hash_t;

typedef enum {
	LH_FIND, LH_FIND_OR_INSERT, LH_UPSERT
} lh_op_t;

typedef enum {
	LH_NOT_FOUND, LH_FOUND, LH_INSERTED, LH_UPDATED
} lh_result_t;

/*
 * Create a hash table.
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
 * lh_init() may round up max_memory to a more convenient value. If
 * max_memory is zero, the hash table is disk-only.
 *
 * If not NULL, the spill_dir should be a place where memory can
 * meaningfully spill over to disk, which rules out /tmp on most systems
 * because it's often implemented as a ramdisk. If spill_dir is NULL, the
 * hash table does not use disk backup and may eventually run out of memory.
 */
linear_hash_t *
lh_init(size_t record_size, size_t key_size, size_t max_memory,
    const char *spill_dir);

/*
 * There are three possible operations on records (LH_FIND, LH_FIND_OR_INSERT,
 * and LH_UPSERT), and lh_record() implements all of them. It's a single
 * function because the implementations are essentially similar. You may
 * find the #defines above to be a more straightforward API.
 *
 * lh_record() accepts an opcode and pointers to two record buffers, inbuff
 * and outbuff. All operations require the key fields of inbuff to be filled
 * out. LH_FIND_OR_INSERT and LH_UPSERT expect a full record there. Any
 * operation that returns an existing record will store it in outbuff.
 *
 * You can pass the same pointer as both inbuff and outbuff. This is fully
 * supported. However, for operations other than LH_FIND, it may entail an
 * extra in-memory copy.
 *
 * If you don't care about the contents of the existing record (if there
 * is one), you can pass NULL as the outbuff. The old record still has to
 * be retrieved, so this isn't any more efficient than providing an output
 * buffer. It just saves you from having to declare the buffer.
 *
 * Whenever you supply an outbuff pointer, that buffer may be used as a scratch area
 *
 * Opcodes:
 *
 * LH_FIND
 *    Retrieves the matching record from the table, if one exists.
 *    Returns either LH_FOUND or LH_NOT_FOUND.
 *
 * LH_FIND_OR_INSERT
 *    If the key matches an existing record, returns that record in
 *    outbuff (if non-NULL). Otherwise, inserts inbuff. Returns either
 *    LH_FOUND or LH_INSERTED.
 *
 * LH_UPSERT
 *    Saves the inbuff record in the table, overwriting any existing
 *    record. Returns LH_INSERTED or LH_UPDATED. If the return value is
 *    LH_UPDATED, also saves a copy of the previous table record in outbuff.
 */
lh_result_t
lh_record(linear_hash_t *lh, lh_op_t op, void *inbuff, void *outbuff);

/*
 * Destroy the table and release all resources.
 */
void
lh_fini(linear_hash_t *lh);

#ifdef	__cplusplus
}
#endif

#endif /* _ZSTREAM_HASH_H */
