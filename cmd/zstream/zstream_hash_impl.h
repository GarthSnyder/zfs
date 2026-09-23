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
 * This is the internal header for the linear hash table. It holds the
 * #defines and struct definitions that back the implementation but that
 * shouldn't be part of the public API in zstream_hash.h.
 */

#ifndef _ZSTREAM_HASH_IMPL_H
#define	_ZSTREAM_HASH_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zstream_alloc.h"
#include "zstream_hash.h"

#define	ENTRIES_PER_BUCKET	6
#define	NUM_ALLOC		3

/*
 * Entry in a bucket: hash value + locator to data
 */
typedef struct {
	uint64_t  	be_hash;
	record_ix_t 	be_record;  /* index of actual data */
} bucket_entry_t;

/*
 * Bucket structure: fixed array of entries + overflow pointer. Overflow
 * buckets have a separate allocator.
 */
typedef struct {
	bucket_entry_t	b_entries[ENTRIES_PER_BUCKET];
	record_ix_t	b_overflow;			/* 0 == no overflow */
} bucket_t;

/*
 * These allocators are in memory clawback order, first to last
 */
typedef union {
	struct {
		allocator_t	*data;		/* Data records */
		allocator_t	*overflow;	/* Overflow buckets */
		allocator_t	*bucket;	/* Main buckets */
	};
	allocator_t		*all[NUM_ALLOC];
} lh_allocators_t;

/* Format is wonky here because checkstyle doesn't understand static asserts */
_Static_assert(sizeof (lh_allocators_t) == NUM_ALLOC * sizeof (allocator_t *),
	    "lh_allocators_t has padding");

struct linear_hash {
	size_t		lh_record_size;		/* Params */
	uint64_t	lh_max_memory;
	lh_allocators_t	lh_alloc;
	uint8_t		lh_hash_suffix_length;	  /* Granularity above split */
	record_ix_t	lh_split_pointer;	  /* Next bucket to split */
	int		lh_next_memory_check;	  /* # inserts before check */
	uint64_t	lh_num_top_level_buckets;
	uint64_t	lh_num_top_level_entries;
	uint64_t	lh_generation;
};

/*
 * Internal iterator for bucket entries
 */
typedef struct {
	linear_hash_t	*ei_lh;		/* The hash that owns this iterator */
	record_ix_t	ei_bucket_ix;	/* Index of bucket within allocator */
	int		ei_entry_ix;	/* Ix within bucket; -1 == not read */
	bucket_t	ei_bucket;	/* Working copy of bucket */
	boolean_t	ei_in_overflow;	/* Which allocator: main or overflow? */
	boolean_t	ei_dirty;	/* Needs writeback? */
} entry_iterator_t;

/*
 * Client-facing iterator for retrieving records by hash
 */
struct lh_iterator {
	uint64_t		lhi_hash;		/* Client's query */
	uint64_t		lhi_generation;		/* Validity check */
	entry_iterator_t	lhi_entry_iterator;
};

/*
 * Memory-management pacing knobs (defined in zstream_hash.c). They are
 * exposed so that selftests can exercise memory-pressure behavior at small
 * scales: lh_memory_margin is the extra memory reclaimed beyond the strict
 * overage whenever a clawback occurs, and lh_mem_check_interval is the
 * number of insertions between memory-budget checks. Both are process-wide,
 * so a caller that changes them must put them back.
 */
extern size_t	lh_memory_margin;
extern int	lh_mem_check_interval;

#ifdef __cplusplus
}
#endif

#endif /* _ZSTREAM_HASH_IMPL_H */
