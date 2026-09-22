// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the Common
 * Development and Distribution License ("CDDL"), version 1.0. You may only use
 * this file in accordance with the terms of version 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this source. A
 * copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#include <err.h>
#include <stdlib.h>
#include <string.h>

#include "zstream_alloc.h"
#include "zstream_util.h"

/*
 * More details about linear hashing:
 *
 * Hash keys are masked to reduce their effective length. At any given time,
 * two mask lengths are in use, the longer being one bit longer than the
 * shorter. Buckets hashed with the long mask appear at the beginning of the
 * table, and the remaining buckets follow. A cursor, the split pointer,
 * points to the first bucket hashed with a shorter mask.
 *
 *      +----------------------------+ <- index 0
 *      |                            |
 *      |   long-mask (h+1) buckets  |
 *      |       already split        |
 *      |                            |
 *      +----------------------------+
 *      |                            | <- split pointer
 *      |   short-mask (h) buckets   |    (buckets to be split)
 *      |       not yet split        |
 *      |                            |
 *      +----------------------------+ <- power-of-2 boundary
 *      |                            |   (cycle restarts when split
 *      |   long-mask (h+1) buddies  |    pointer gets here)
 *      |    of buckets below the    |
 *      |       split pointer        |
 *      |                            |
 *      +----------------------------+
 *      |                            |
 *      |      not yet allocated     |
 *      |    (will hold buddies of   |
 *      |     buckets at/above the   |
 *      |        split pointer)      |
 *      |                            |
 *      +----------------------------+ <- power-of-2 boundary
 *
 * When occupancy exceeds a given limit, the contents of the bucket pointed
 * to by the split pointer are rehashed using the longer key length. Since
 * there's only one bit's difference between the two masks, there are only
 * two possible destinations for rehashed entries: the same bucket they're
 * already in, or one that's 2^n slots later, at the end of the table. On
 * average, half the entries are relocated. The split pointer is then
 * incremented.
 *
 * Eventually, all short-key buckets have been rehashed with the longer
 * mask. At that point, what was previously the long key becomes the short
 * key and a new one-bit-longer mask is introduced. The split pointer is
 * reset to the beginning of the hash table.
 *
 * This scheme grows the table linearly and incrementally. It's useful for
 * stream processing because it's space-efficient and because we often don't
 * know how long the input stream will be. When the table becomes too large
 * to keep in memory, it can spill over to disk storage without abandoning
 * or reshuffling the existing in-memory entries. If 90% of buckets are in
 * memory, then 90% of lookups will happen at memory speed.
 *
 * This implementation uses open hashing (overflow buckets) and is
 * insert-only. Linear hashing allows deletions, but they're not useful here
 * and are not implemented.
 *
 * Each hash table has three allocator_t's underneath it: one for the data
 * being stored, one for hash buckets, and one for overflow buckets.
 *
 * When total memory use reaches a designated threshold, one or more
 * allocators are asked to yield some of their memory. The first priority
 * for eviction is the data allocator, followed by the overflow allocator
 * and then the regular bucket allocator. Under extreme pressure, you can
 * expect to see the data and overflow buckets fully converted to disk
 * storage while the main bucket array is partially in memory and partially
 * on disk.
 */

#define	MAX_OCCUPANCY			0.75
#define	INITIAL_HASH_SUFFIX_LENGTH	10
#define	ENTRIES_PER_BUCKET		6
#define NUM_ALLOC			3

#define	ITER_BUCKET(lh, bucket) {					\
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
	    (iter)->ei_lh->lh_alloc.overflow : (iter)->ei_lh->lh_alloc.bucket)

#define	BUCKET_ENTRY(ei) (&(ei)->ei_bucket.b_entries[(ei)->ei_entry_ix])

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
typedef struct lh_iterator {
	uint64_t		lhi_hash;		/* Client's query */
	entry_iterator_t	lhi_entry_iterator;
} lh_iterator_t;

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

_Static_assert(sizeof (lh_allocators_t) == NUM_ALLOC * sizeof (allocator_t *),
    "lh_allocators_t has padding");

struct linear_hash {
	size_t		lh_record_size;		/* Params */
	uint64_t	lh_max_memory;
	lh_allocators_t	lh_alloc;
	uint8_t		lh_hash_suffix_length;	  /* Granularity above split */
	record_ix_t	lh_split_pointer;	  /* Next bucket to split */
	int		lh_next_memory_check;	  /* # of splits before check */
	uint64_t	lh_num_top_level_buckets;
	uint64_t	lh_num_top_level_entries;
};

/*
 * Memory management controls. These are variables rather than #defines so
 * that tests can tweak them. The margin should be sized so that a table
 * under sustained memory pressure performs a reasonable number of memory
 * clawbacks (a few dozen) over its lifetime rather than suffering many
 * small bites.
 */
size_t	lh_memory_margin	= 64ULL << 20;	/* 64MB */
int	lh_mem_check_interval	= 4096;		/* Insertions per check */

static int		next_iterator = 0;
static lh_iterator_t	lh_iterators[MAX_LH_ITERATORS];

/*
 * Calculate the destination bucket for a given hash value.
 *
 * lh_hash_suffix_length = hash suffix length in effect at or above the
 * split point. Below the split, it is one bit longer. E.g., if
 * hash_suffix_length = 3, items below the split point are hashed into 16
 * buckets. At the split point or above, they are hashed into 8 buckets.
 * Ergo, when the split pointer reaches index 8, 2^level, all mod 8 entries
 * have been upgraded. The split pointer is reset to zero and the suffix
 * length increases.
 */
static inline uint64_t
bucket_for_hash(linear_hash_t *lh, uint64_t hash)
{
	uint64_t mask = (1ULL << (lh->lh_hash_suffix_length)) - 1;
	if ((hash & mask) < lh->lh_split_pointer)
		mask = (mask << 1) | 1;
	return (hash & mask);
}

static inline void
read_bucket(entry_iterator_t *iter)
{
	allocator_retrieve(ALLOC_FOR(iter), iter->ei_bucket_ix,
	    &iter->ei_bucket);
	iter->ei_entry_ix = 0;
	iter->ei_dirty = B_FALSE;
}

static inline void
save_bucket(entry_iterator_t *iter, boolean_t force)
{
	if (!force && iter->ei_dirty == B_FALSE)
		return;
	allocator_store(ALLOC_FOR(iter), iter->ei_bucket_ix, &iter->ei_bucket);
	iter->ei_dirty = B_FALSE;
}

/*
 * Prepares an entry iterator to examine the next bucket entry. Updates
 * entry_iterator struct and returns a pointer to the current bucket entry.
 * Returns NULL when there are no more entries, or, alternately, extends the
 * bucket chain indefinitely.
 */
static inline bucket_entry_t *
entry_iterator_next(entry_iterator_t *iter, boolean_t extend)
{
	if (iter->ei_entry_ix < 0) {
		read_bucket(iter);
		return (BUCKET_ENTRY(iter));
	} else if (iter->ei_entry_ix == ENTRIES_PER_BUCKET - 1) {
		save_bucket(iter, B_FALSE);
		if (iter->ei_bucket.b_overflow != 0) {
			iter->ei_in_overflow = B_TRUE;
			iter->ei_bucket_ix = iter->ei_bucket.b_overflow;
			read_bucket(iter);
			return (BUCKET_ENTRY(iter));
		} else if (!extend) {
			return (NULL);
		} else {
			/* Extend by adding overflow bucket */
			record_ix_t record =
			    allocator_skip(iter->ei_lh->lh_alloc.overflow);
			iter->ei_bucket.b_overflow = record;
			save_bucket(iter, B_TRUE);
			*iter = (entry_iterator_t) {
				.ei_lh = iter->ei_lh,
				.ei_bucket_ix = record,
				.ei_in_overflow = B_TRUE,
				.ei_dirty = B_TRUE,
			};
			return (BUCKET_ENTRY(iter));
		}
	} else {
		iter->ei_entry_ix++;
		return (BUCKET_ENTRY(iter));
	}
}

/*
 * Split the bucket pointed to by the split pointer, rehashing entries
 * according to the one-higher suffix length. Since only one bit is added,
 * existing entries either stay where they are or go to one alternate buddy
 * bucket.
 *
 * During the partition pass, the source bucket has both a read iterator and
 * a write iterator. This is fine because each iterator has its own copy of
 * the bucket and the read iterator will always precede the write iterator.
 * The bucket will not be modified through the read iterator.
 *
 * At steady state, bucket entries are packed at the front of buckets and
 * all inactive entries are zeroed out. The first entry with a record number
 * of 0 marks the end of entries. After partitioning a bucket, we have to
 * zero out its now-unoccupied tail.
 */
static void
split_bucket(linear_hash_t *lh)
{
	record_ix_t bucket_ix = lh->lh_split_pointer;
	record_ix_t buddy_ix = bucket_ix | (1ULL << lh->lh_hash_suffix_length);

	entry_iterator_t source = ITER_BUCKET(lh, bucket_ix);
	entry_iterator_t stay   = ITER_BUCKET(lh, bucket_ix);
	entry_iterator_t move   = ITER_BUCKET(lh, buddy_ix);

	/*
	 * Increment split pointer early so that bucket_for_hash() now uses
	 * the extended mask length
	 */
	lh->lh_split_pointer++;

	bucket_entry_t *source_be;
	while ((source_be = entry_iterator_next(&source, B_FALSE)) &&
	    source_be->be_record != 0) {
		if (!source.ei_in_overflow)
			lh->lh_num_top_level_entries--;
		boolean_t this_entry_stays =
		    bucket_for_hash(lh, source_be->be_hash) == bucket_ix;
		entry_iterator_t *dest = this_entry_stays ? &stay : &move;
		bucket_entry_t *dest_be = entry_iterator_next(dest, B_TRUE);
		if (!dest->ei_in_overflow)
			lh->lh_num_top_level_entries++;
		/* Don't mark dirty unless actually modified */
		if (source_be->be_record != dest_be->be_record ||
		    source_be->be_hash != dest_be->be_hash) {
			*dest_be = *source_be;
			dest->ei_dirty = B_TRUE;
		}
	}

	lh->lh_num_top_level_buckets++;

	/*
	 * Continue iterating the "stay" bucket to zero out the tail. This
	 * is the writable copy of the original bucket.
	 */
	bucket_entry_t *ent;
	while ((ent = entry_iterator_next(&stay, B_FALSE)) && ent->be_record) {
		*ent = (bucket_entry_t) {0};
		stay.ei_dirty = B_TRUE;
	}
	save_bucket(&stay, B_FALSE);
	save_bucket(&move, B_FALSE);

	/*
	 * Have we completed the full hashing cycle at this suffix length?
	 */
	record_ix_t buckets_this_cycle = 1ULL << (lh->lh_hash_suffix_length);
	if (lh->lh_split_pointer >= buckets_this_cycle) {
		lh->lh_hash_suffix_length++;
		lh->lh_split_pointer = 0;
	}
}

static inline void
check_split(linear_hash_t *lh)
{
	double occupancy = (double)lh->lh_num_top_level_entries /
	    (lh->lh_num_top_level_buckets * ENTRIES_PER_BUCKET);
	if (occupancy > MAX_OCCUPANCY) {
		split_bucket(lh);
	}
}

/*
 * Free up memory if we're over budget. If we free, we reclaim
 * lh_memory_margin more bytes than is strictly necessary to give ourselves
 * some operating room until the next memory check. We want to free in
 * relatively large chunks, not just because this memory check is nontrivial
 * but also because we want to limit the frequency of reshuffling.
 *
 * Memory clawbacks are prioritized by allocator. The data allocator is the
 * first target, followed by the overflow allocator and the bucket
 * allocator. Depending on state, we may need to perform multiple reclaims,
 * hence the loop.
 */
static void
check_memory_use(linear_hash_t *lh)
{
	size_t current_use[NUM_ALLOC];
	size_t total_used = 0;
	for (int i = 0; i < NUM_ALLOC; i++) {
		current_use[i] = allocator_memory_used(lh->lh_alloc.all[i]);
		total_used += current_use[i];
	}
	ssize_t overage = (ssize_t)total_used - lh->lh_max_memory;
	while (overage > 0) {
		size_t to_trim = overage + lh_memory_margin;
		for (int i = 0; i < NUM_ALLOC; i++) {
			if (current_use[i] > 0) {
				size_t trimmed = allocator_trim_memory(
				    lh->lh_alloc.all[i], to_trim)
				overage -= trimmed;
				current_use[i] -= trimmed;
				break;
			}
		}
	}
}

/*
 * Every allocator_t is initialized to the same maximum memory size. We do
 * memory management from the hash (rather than the allocators) so that we
 * can control the order in which data gets spilled to disk.
 */
linear_hash_t *
lh_init(size_t record_size, size_t max_mem, const char *dir)
{
	linear_hash_t *lh = safe_malloc(sizeof (linear_hash_t));
	*lh = (linear_hash_t) {
		.lh_record_size = record_size,
		.lh_hash_suffix_length = INITIAL_HASH_SUFFIX_LENGTH,
		.lh_max_memory = max_mem,
		.lh_num_top_level_buckets = 1ULL << INITIAL_HASH_SUFFIX_LENGTH
	};
	size_t sizes[] = {record_size, sizeof (bucket_t), sizeof (bucket_t)};
	for (int i = 0; i < NUM_ALLOC; i++) {
		lh->lh_alloc.all[i] = allocator_init(sizes[i], max_mem,
		    (dir != NULL) ? dir : "/var/tmp");
		if (lh->lh_alloc.all[i] == NULL)
			errx(1, "failed to initialize linear hash allocators");
	}
	/* The index 0 is a sentinel value for these allocators */
	allocator_skip(lh->lh_alloc.data);
	allocator_skip(lh->lh_alloc.overflow);
	return (lh);
}

void
lh_insert(linear_hash_t *lh, uint64_t hash, const void* data)
{
	VERIFY(lh != NULL && data != NULL);
	record_ix_t record = allocator_append(lh->lh_alloc.data, data);
	entry_iterator_t iter = ITER_BUCKET(lh, bucket_for_hash(lh, hash));

	bucket_entry_t *entry;
	while ((entry = entry_iterator_next(&iter, B_TRUE))) {
		if (entry->be_record == 0) {
			*entry = (bucket_entry_t) { hash, record };
			save_bucket(&iter, B_TRUE);
			break;
		}
	}
	if (!iter.ei_in_overflow) {
		lh->lh_num_top_level_entries++;
	}
	check_split(lh);
	lh->lh_next_memory_check--;
	if (lh->lh_next_memory_check <= 0) {
		lh->lh_next_memory_check = lh_mem_check_interval;
		check_memory_use(lh);
	}
}

lh_iterator_t *
lh_initiate_retrieve(linear_hash_t *lh, uint64_t hash)
{
	ASSERT(lh != NULL);
	int which_iterator = next_iterator++ % MAX_LH_ITERATORS;
	lh_iterator_t *iter = &lh_iterators[which_iterator];
	record_ix_t bucket = bucket_for_hash(lh, hash);
	*iter = (lh_iterator_t) {
		.lhi_hash = hash,
		.lhi_entry_iterator = ITER_BUCKET(lh, bucket)
	};
	return (iter);
}

boolean_t
lh_retrieve_next(lh_iterator_t *lh_iter, void *buffer)
{
	entry_iterator_t *ei = &lh_iter->lhi_entry_iterator;
	bucket_entry_t *entry;
	while ((entry = entry_iterator_next(ei, B_FALSE))) {
		if (entry->be_record == 0)
			break;
		if (entry->be_hash == lh_iter->lhi_hash) {
			allocator_retrieve(ei->ei_lh->data,
			    entry->be_record, buffer);
			return (B_TRUE);
		}
	}
	return (B_FALSE);
}

void
lh_destroy(linear_hash_t *lh) {
	VERIFY(lh != NULL);
	for (int i = 0; i < NUM_ALLOC; i++) {
		if (lh->lh_alloc.all[i] != NULL)
			allocator_destroy(lh->lh_alloc.all[i]);
	}
	free(lh);
}
