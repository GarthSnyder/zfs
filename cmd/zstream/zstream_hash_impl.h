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
 * This is the internal header for the linear hash table. It holds the
 * #defines and struct definitions that back the implementation but that
 * shouldn't be part of the public API in zstream_hash.h.
 */

#ifndef _ZSTREAM_HASH_IMPL_H
#define _ZSTREAM_HASH_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zstream_alloc.h"
#include "zstream_hash.h"

#define ENTRIES_PER_BUCKET 6

/*
 * MAX_BUCKET_CHAIN is a reporting/stats limitation, not an implementation
 * limitation. There is no limit on the actual length of bucket chains.
 */
#define	MAX_BUCKET_CHAIN 16

#define ITER_BUCKET(lh, bucket) (entry_iterator_t){			\
		.ei_lh = lh,						\
		.ei_bucket_ix = bucket,					\
		.ei_entry_ix = -1					\
	}

/*
 * Determine the allocator for the given entry_iterator. The current bucket
 * struct might be a primary bucket or an overflow bucket, and the allocator
 * switches based on that.
 */
#define	ALLOC_FOR(iter) ((iter)->ei_in_overflow ? \
	    (iter)->ei_lh->lh_overflow_alloc : (iter)->ei_lh->lh_bucket_alloc)

#define	BUCKET_ENTRY(ei) (&(ei)->ei_bucket.b_entries[(ei)->ei_entry_ix])

/*
 * Validation macros used by the core around mutating operations. They are
 * essentially no-ops unless lh_validate is set at runtime and are only
 * referenced inside #ifdef LH_STATS_AND_VALIDATION blocks.
 */
#define	START_VALIDATION(lh) 						\
	boolean_t started_ok = B_TRUE;					\
	if (lh->lh_validate) {						\
		started_ok = lh_validate(lh);				\
	}

#define	END_VALIDATION(lh)						\
	if (lh->lh_validate && !lh_validate(lh) && started_ok) {	\
		warnx("%s broke the hash table", __func__);		\
	}

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
 * Stats and validation types. These are defined unconditionally so this
 * header stays valid whether or not LH_STATS_AND_VALIDATION is set.
 * However, they are only embedded in struct linear_hash (below) and
 * populated by zstream_hash_extras.c when the define is active.
 */
typedef struct {
	uint64_t	os_count;		/* Inserts, splits, etc. */
	uint64_t	os_ops_mem;		/* Memory ops portion */
	uint64_t	os_ops_disk;		/* Disk portion */
} op_stats_t;

typedef struct {
	op_stats_t	lhs_inserts;		/* Inserts from caller */
	op_stats_t	lhs_retrieves;		/* Retrieves from caller */
	op_stats_t	lhs_splits;		/* Splits, internal */
} lh_stats_t;

typedef struct {
	op_stats_t	*ot_stat_bin;
	op_stats_t	ot_start_ops;
	op_stats_t	ot_latest_ops;
} ops_tracker_t;

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
	uint64_t	lh_num_buckets;		/* Top level only */
	uint64_t	lh_num_entries;
	uint64_t	lh_num_top_level_entries;

#ifdef LH_STATS_AND_VALIDATION
	boolean_t	lh_validate;		/* Debug: check per op (slow) */
	lh_stats_t	lh_stats;
	ops_tracker_t	lh_ops_tracker;
#endif
};

#ifdef LH_STATS_AND_VALIDATION

typedef struct {
	uint64_t	bs_num_chains;
	uint64_t	bs_num_empty_chains;
	uint64_t	bs_num_slots_filled;
	double		bs_pct_empty;
	double		bs_occupancy;          /* Full slots / All slots */
	double		bs_nonempty_occupancy;
} bucket_stats_t;

typedef struct lh_report {
	bucket_stats_t	lr_chains_by_length[MAX_BUCKET_CHAIN];
	uint64_t	lr_total_entries;
	uint64_t	lr_top_level_entries;
	uint64_t	lr_total_chains;
	uint64_t	lr_bytes_in_data;
	uint64_t	lr_bytes_in_buckets;
	op_stats_t	lr_splits;
	op_stats_t	lr_inserts;
	op_stats_t	lr_retrieves;
	double		lr_occupancy;		/* Top levels / # of chains */
	double		lr_overall_occupancy;	/* Full slots / Total slots */
} lh_report_t;

#endif  /* LH_STATS_AND_VALIDATION */

/*
 * Core routines from zstream_hash.c.
 */
uint64_t
bucket_for_hash(linear_hash_t *lh, uint64_t hash);

bucket_entry_t *
entry_iterator_next(entry_iterator_t *iter, boolean_t extend);

/*
 * Stats and validation routines from zstream_hash_extras.c.
 */
boolean_t
lh_validate(linear_hash_t *lh);

op_stats_t
total_io_ops(linear_hash_t *lh);

/*
 * Functions from zstream_hash_extras.c
 */
void
begin_ops_tracking(linear_hash_t *lh, op_stats_t *bin);

void
update_ops_tracking(linear_hash_t *lh);

void
complete_ops_tracking(linear_hash_t *lh);

void
lh_get_stats(linear_hash_t *lh, lh_report_t *stats);

void
lh_print_stats(linear_hash_t *lh);

#ifdef __cplusplus
}
#endif

#endif /* _ZSTREAM_HASH_IMPL_H */
