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

/*
 * This file contains the #defines and struct definitions for the basic
 * linear hash table. They're here (instead of in zstream_hash.c) so that
 * linear_hash_extras.[ch] can access them.
 */

#ifndef _ZSTREAM_HASH_IMPL_H
#define _ZSTREAM_HASH_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zstream_alloc.h"
#include "zstream_hash.h"

#define ENTRIES_PER_BUCKET 6

#define ITER_BUCKET(lh, bucket) (entry_iterator_t){			\
		.ei_lh = lh,						\
		.ei_bucket_ix = bucket,					\
		.ei_entry_ix = -1					\
	}

/*
 * Determine the allocator for the given entry_iterator. The current bucket
 * struct might be a primary bucket or an overflow bucket, and the allocator
 * corresponds to that.
 */
#define	ALLOC_FOR(iter) ((iter)->ei_in_overflow ? \
	    (iter)->ei_lh->lh_overflow_alloc : (iter)->ei_lh->lh_bucket_alloc)

#define	BUCKET_ENTRY(ei) (&(ei)->ei_bucket.b_entries[(ei)->ei_entry_ix])

/*
 * Entry in a bucket: hash value + locator to data
 */
typedef struct {
	uint64_t  	be_hash;
	record_ix_t 	be_record;  /* index of actual data */
} bucket_entry_t;

/*
 * Bucket structure: fixed array of entries + overflow pointer
 */
typedef struct {
	bucket_entry_t	b_entries[ENTRIES_PER_BUCKET];
	record_ix_t	b_overflow;  /* 0 if no overflow */
} bucket_t;

/*
 * Internal iterator for bucket entries
 */
typedef struct {
	linear_hash_t	*ei_lh;
	record_ix_t	ei_bucket_ix;	/* Index of bucket within allocator */
	int		ei_entry_ix;	/* Ix within bucket; -1 == not read */
	bucket_t	ei_bucket;	/* Working copy of bucket */
	boolean_t	ei_in_overflow;	/* Which allocator: main or overflow? */
	boolean_t	ei_dirty;	/* Needs writeback? */
} entry_iterator_t;

/*
 * User-facing iterator for retrieving records by hash
 */
typedef struct lh_iterator {
	uint64_t		lhi_hash;
	entry_iterator_t	lhi_entry_iterator;
} lh_iterator_t;

/*
 * The actual hash table struct
 */
struct linear_hash {

	size_t		lh_record_size;		/* Params */
	uint64_t	lh_max_memory;

	allocator_t	*lh_data_alloc;		/* Data records */
	allocator_t	*lh_bucket_alloc;	/* Main buckets */
	allocator_t	*lh_overflow_alloc;	/* Overflow buckets */

	uint8_t		lh_hash_suffix_length;	/* Granularity above split */
	record_ix_t	lh_split_pointer;	/* Next bucket to split */
	int     	lh_next_memory_check;	/* # of splits before check */
	uint64_t	lh_num_entries;
	uint64_t	lh_num_top_level_entries;

#ifdef LH_STATS_AND_VALIDATION
	boolean_t	lh_validate;		/* Debug: check per op (slow) */
	lh_stats_t	lh_stats;
	ops_tracker_t	lh_ops_tracker;
#endif
};

uint64_t
bucket_for_hash(linear_hash_t *lh, uint64_t hash);

bucket_entry_t *
entry_iterator_next(entry_iterator_t *iter, boolean_t extend);

#ifdef __cplusplus
}
#endif

#endif /* _ZSTREAM_HASH_IMPL_H */
