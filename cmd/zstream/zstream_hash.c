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
#include "zstream_hash_impl.h"
#include "zstream_util.h"

#define	MAX_OCCUPANCY 0.75
#define	INITIAL_HASH_SUFFIX_LENGTH 10
#define	INSERTIONS_BETWEEN_MEM_CHECKS 4096
#define	MEMORY_MARGIN (1ULL << 28) /* 256MB */

/*
 * A slightly more detailed description of linear hashing:
 *
 * Hash keys are masked to reduce their length. At any given time, two mask
 * lengths are in use, the longer being one bit longer than the shorter.
 * Buckets hashed with the long mask appear at the beginning of the table,
 * and the remaining buckets follow. A cursor, the split pointer, points to
 * the first bucket hashed with a shorter mask.
 *
 * When occupancy exceeds a given threshold, the contents of the bucket
 * pointed to by the split pointer are rehashed using the longer key length.
 * Since there's only one bit's difference between the two masks, there are
 * only two possible destinations for rehashed entries: the same bucket
 * they're already in, or one that's 2^n slots later, at the end of the
 * table. On average, half the entries are relocated. The split pointer is
 * then incremented.
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
 * Eventually, all short-key buckets have been rehashed with the longer
 * mask. At that point, what was previously the long key becomes the short
 * key and a new one-bit-longer mask is introduced. The split pointer is
 * reset to the beginning of the hash table.
 *
 * This scheme grows the table linearly and incrementally. It's useful for
 * stream processing because it's space-efficient and because we often
 * don't know how long the input stream will be. When the table becomes too
 * large to keep in memory, it can spill over to disk storage without
 * abandoning or reshuffling the existing in-memory entries. If 90% of
 * buckets are in memory, then 90% of lookups will happen at memory speed.
 *
 * This implementation uses open hashing (overflow buckets) and is
 * insert-only. (Linear hashing allows deletions, but they're probably
 * not useful here and are unimplemented.)
 *
 * Each hash table has three allocator_t's underneath it: one for the data
 * being stored, one for hash buckets, and one for overflow buckets.
 *
 * When total memory use reaches a designated threshold, the allocators are
 * asked to yield some of their memory. The first priority for eviction is
 * the data allocator, followed by the overflow allocator and then the
 * regular bucket allocator. Under extreme pressure, you can expect to see
 * the data and overflow buckets fully converted to disk storage while the
 * main bucket array is partially in memory and partially on disk.
 */

static int		next_iterator = 0;
static lh_iterator_t	lh_iterators[MAX_LH_ITERATORS];

/*
 * Calculate the destination bucket for a given hash value.
 *
 * lh_hash_suffix_length is defined as the hash suffix length in effect at
 * or above the split point. Below the split, it is one bit longer. E.g., if
 * hash_suffix_length = 3, items below the split point are hashed into 16
 * buckets. At the split point or above, they are hashed into 8 buckets.
 * Ergo, when the split pointer reaches index 8, 2^level, all mod 8 entries
 * have been upgraded. The split pointer is reset to zero and the suffix
 * length increases.
 */
inline uint64_t
bucket_for_hash(linear_hash_t *lh, uint64_t hash)
{
	uint64_t mask = (1ULL << (lh->lh_hash_suffix_length)) - 1;
	if ((hash & mask) < lh->lh_split_pointer)
		mask = (mask << 1) | 1;
	return hash & mask;
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
save_bucket(entry_iterator_t *iter) {
	if (iter->ei_dirty == B_FALSE)
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
inline bucket_entry_t *
entry_iterator_next(entry_iterator_t *iter, boolean_t extend)
{
	if (iter->ei_entry_ix < 0) {
		read_bucket(iter);
		return (BUCKET_ENTRY(iter));
	} else if (iter->ei_entry_ix == ENTRIES_PER_BUCKET - 1) {
		save_bucket(iter);
		if (iter->ei_bucket.b_overflow != 0) {
			iter->ei_in_overflow = B_TRUE;
			iter->ei_bucket_ix = iter->ei_bucket.b_overflow;
			read_bucket(iter);
			return (BUCKET_ENTRY(iter));
		} else if (!extend) {
			return (NULL);
		} else {
			/* Add overflow bucket */
			record_ix_t record =
			    allocator_skip(iter->ei_lh->lh_overflow_alloc);
			iter->ei_bucket.b_overflow = record;
			save_bucket(iter);
			*iter = (entry_iterator_t){
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
 * Split the bucket pointed to by the split pointer, rehashing all entries
 * according to the one-higher suffix length. Since only one bit is added,
 * existing entries either stay where they are or go to one alternate
 * bucket.
 *
 * During the partition pass, the source bucket has both a read iterator and
 * a write iterator. This is fine because each iterator has its own copy of
 * the bucket and the read iterator will always precede the write iterator.
 *
 * At steady state, bucket entries are packed at the front of buckets and
 * all inactive entries are zeroed out. The first entry with a record number
 * of 0 marks the end of entries. After partitioning, we have to zero out
 * the tail of the source bucket.
 */
static void
split_bucket(linear_hash_t *lh)
{
#ifdef LH_STATS_AND_VALIDATION
	START_VALIDATION(lh);
	begin_ops_tracking(lh, &lh->lh_stats.lhs_splits);
#endif
	record_ix_t bucket_ix = lh->lh_split_pointer;
	record_ix_t buddy_ix = bucket_ix | (1ULL << lh->lh_hash_suffix_length);

	entry_iterator_t source = ITER_BUCKET(lh, bucket_ix);
	entry_iterator_t stay   = ITER_BUCKET(lh, bucket_ix);
	entry_iterator_t move   = ITER_BUCKET(lh, buddy_ix);

	/* Increment split pointer so bucket_for_hash uses extended length */
	lh->lh_split_pointer++;

	/* Partition */
	uint64_t pre_num_top_level = 0;
	uint64_t post_num_top_level = 0;
	bucket_entry_t *sbe;
	while ((sbe = entry_iterator_next(&source, B_FALSE))) {
		if (sbe->be_record == 0)
			break;
		else if (!source.ei_in_overflow)
			pre_num_top_level++;
		boolean_t stays =
		    bucket_for_hash(lh, sbe->be_hash) == bucket_ix;
		entry_iterator_t *dest = stays ? &stay : &move;
		bucket_entry_t *dbe = entry_iterator_next(dest, B_TRUE);
		if (!dest->ei_in_overflow)
			post_num_top_level++;
		/* Don't mark dirty unless modified */
		if (memcmp(sbe, dbe, sizeof (bucket_entry_t)) != 0) {
			*dbe = *sbe;
			dest->ei_dirty = B_TRUE;
		}
	}

	/* Maintain top-level entry account used to determine occupancy */
	lh->lh_num_top_level_entries -= pre_num_top_level;
	lh->lh_num_top_level_entries += post_num_top_level;

	/* Zero out the rest of the source bucket */
	bucket_entry_t *entry;
	while ((entry = entry_iterator_next(&stay, B_FALSE))) {
		if (entry->be_record) {
			*entry = (bucket_entry_t){0};
			stay.ei_dirty = B_TRUE;
		}
	}
	save_bucket(&stay);
	save_bucket(&move);

	/* Have we completed the full hashing cycle at this suffix length? */
	record_ix_t buckets_this_cycle = 1ULL << (lh->lh_hash_suffix_length);
	if (lh->lh_split_pointer >= buckets_this_cycle) {
		lh->lh_hash_suffix_length++;
		lh->lh_split_pointer = 0;
	}

#ifdef LH_STATS_AND_VALIDATION
	complete_ops_tracking(lh);
	END_VALIDATION(lh);
#endif
}

static inline void
check_split(linear_hash_t *lh)
{
	double occupancy = (double)lh->lh_num_top_level_entries /
		((1ULL << lh->lh_hash_suffix_length) * ENTRIES_PER_BUCKET);
	if (occupancy > MAX_OCCUPANCY) {
		split_bucket(lh);
	}
}

/*
 * Free up memory if we're over budget. If we free, we reclaim MEMORY_MARGIN
 * more bytes than is strictly necessary to give ourselves some operating
 * room until the next memory check. We want to free in relatively large
 * chunks, not in small increments.
 *
 * Memory clawbacks are prioritized by allocator. The data allocator is the
 * first hit, followed by the overflow allocator and the bucket allocator.
 *
 * Depending on the state of things, we may need to reclaim memory from more
 * than one allocator, hence the loop.
 */
static void
check_memory_use(linear_hash_t *lh)
{
	allocator_stats_t bucket = allocator_get_stats(lh->lh_bucket_alloc);
	allocator_stats_t overflow = allocator_get_stats(lh->lh_overflow_alloc);
	allocator_stats_t data = allocator_get_stats(lh->lh_data_alloc);

check:	size_t total_used = bucket.as_mem_used + overflow.as_mem_used +
	    data.as_mem_used;
	ssize_t overage = (ssize_t)total_used - (ssize_t)lh->lh_max_memory;
	if (overage > 0) {
		allocator_stats_t *squeezee;
		if (data.as_max_memory != 0) {
			squeezee = &data;
		} else if (overflow.as_max_memory != 0) {
			squeezee = &overflow;
		} else {
			squeezee = &bucket;
		}
		size_t new_limit = MAX(0, (ssize_t)squeezee->as_mem_used -
		    (overage + MEMORY_MARGIN));
		allocator_set_max_memory(squeezee->as_allocator, new_limit);
		*squeezee = allocator_get_stats(squeezee->as_allocator);
		goto check;
	}
}

/*
 * Buffer length must be pre-checked
 */
static int
create_temp_file(const char *dir, char *pathbuff) {
	sprintf(pathbuff, "%s/zstream-linear-hash-XXXXXX", dir);
	int fd = mkstemp(pathbuff);
	if (fd >= 0) {
		if (unlink(pathbuff) < 0)
			err(1, "unlink of %s failed", pathbuff);
		return (fd);
	}
	err(1, "unable to create file %s", pathbuff);
}

linear_hash_t *
lh_init(size_t record_size, size_t max_mem, const char *cache_dir)
{
	VERIFY(record_size > 0);
	char path[1024];
	int fd;

	if (cache_dir == NULL)
		cache_dir = "/var/tmp";
	linear_hash_t *lh = safe_malloc(sizeof(struct linear_hash));
	*lh = (linear_hash_t){
		.lh_record_size = record_size,
		.lh_hash_suffix_length = INITIAL_HASH_SUFFIX_LENGTH,
		.lh_max_memory = max_mem
	};

	if (strlen(cache_dir) > sizeof(path) - 32)
		errx(1, "cache dir path too long");
	fd = create_temp_file(cache_dir, path);
	lh->lh_data_alloc = allocator_init(record_size, max_mem, fd);
	fd = create_temp_file(cache_dir, path);
	lh->lh_bucket_alloc = allocator_init(sizeof (bucket_t), max_mem, fd);
	fd = create_temp_file(cache_dir, path);
	lh->lh_overflow_alloc = allocator_init(sizeof (bucket_t), max_mem, fd);

	if (!lh->lh_data_alloc|| !lh->lh_bucket_alloc || !lh->lh_overflow_alloc)
		errx(1, "unable to initialize linear_hash_t allocators");
	/*
	 * Skip first overflow and data buckets to allow 0 to indicate
	 * empty or end-of-chain.
	 */
	allocator_skip(lh->lh_data_alloc);
	allocator_skip(lh->lh_overflow_alloc);
	return lh;
}

void
lh_insert(linear_hash_t *lh, uint64_t hash, const void* data)
{
	VERIFY(lh != NULL && data != NULL);

#ifdef LH_STATS_AND_VALIDATION
	START_VALIDATION(lh);
	begin_ops_tracking(lh, &lh->lh_stats.lhs_inserts);
#endif

	record_ix_t record = allocator_append(lh->lh_data_alloc, data);
	entry_iterator_t iter = ITER_BUCKET(lh, bucket_for_hash(lh, hash));

	bucket_entry_t *entry;
	while ((entry = entry_iterator_next(&iter, B_TRUE))) {
		if (entry->be_record == 0) {
			*entry = (bucket_entry_t){ hash, record };
			iter.ei_dirty = B_TRUE;
			save_bucket(&iter);
			break;
		}
	}
	lh->lh_num_entries++;
	if (!iter.ei_in_overflow) {
		lh->lh_num_top_level_entries++;
	}

#ifdef LH_STATS_AND_VALIDATION
	complete_ops_tracking(lh);
	END_VALIDATION(lh);
#endif

	check_split(lh);

	lh->lh_next_memory_check--;
	if (lh->lh_next_memory_check <= 0) {
		lh->lh_next_memory_check = INSERTIONS_BETWEEN_MEM_CHECKS;
		check_memory_use(lh);
	}
}

lh_iterator_t *
lh_initiate_retrieve(linear_hash_t *lh, uint64_t hash)
{
	ASSERT(lh != NULL);

#ifdef LH_STATS_AND_VALIDATION
	begin_ops_tracking(lh, &lh->lh_stats.lhs_retrieves);
#endif

	int which_iterator = next_iterator++ % MAX_LH_ITERATORS;
	lh_iterator_t *iter = &lh_iterators[which_iterator];
	record_ix_t bucket = bucket_for_hash(lh, hash);
	*iter = (lh_iterator_t){
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
			allocator_retrieve(ei->ei_lh->lh_data_alloc,
			    entry->be_record, buffer);
#ifdef LH_STATS_AND_VALIDATION
			update_ops_tracking(ei->ei_lh);
#endif
			return (B_TRUE);
		}
	}
#ifdef LH_STATS_AND_VALIDATION
	complete_ops_tracking(ei->ei_lh);
#endif
	return (B_FALSE);
}

void
lh_destroy(linear_hash_t *lh) {
	VERIFY(lh != NULL);
#ifdef LH_STATS_AND_VALIDATION
	if (lh->lh_validate) {
		lh_validate(lh);
	}
#endif
	if (lh->lh_data_alloc) 	{ allocator_destroy(lh->lh_data_alloc); }
	if (lh->lh_bucket_alloc) { allocator_destroy(lh->lh_bucket_alloc); }
	if (lh->lh_overflow_alloc) { allocator_destroy(lh->lh_overflow_alloc); }
	free(lh);
}
