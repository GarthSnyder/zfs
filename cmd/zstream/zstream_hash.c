/*
 * Linear hashing implementation
 */

#include <stdlib.h>
#include <string.h>

#include "zstream_alloc.h"
#include "zstream_hash.h"
#include "zstream_hash_debug.h"
#include "zstream_hash_impl.h"
#include "zstream_hash_stats.h"
#include "zstream_util.h"

#define MAX_OCCUPANCY 0.75
#define INITIAL_HASH_SUFFIX_LENGTH 10
#define INSERTIONS_BETWEEN_MEM_CHECKS 4096

#define CHECKED(doing_what, expr) 					\
	if (expr < 0) {							\
		fprintf(stderr, "Error while %s.\n", doing_what);	\
		exit(1);						\
	}

/*
 * Calculate bucket for a given hash value.
 *
 * Here, suffix length is defined as the hash suffix length in effect at or
 * above the split point. Below the split, it is one bit longer. E.g., if
 * hash_suffix_length = 3, items below the split point are hashed into 16
 * buckets. At the split point or above, they are hashed into 8 buckets.
 * Ergo, when the split pointer reaches index 8, 2^level, all mod 8 entries
 * have been upgraded. The split pointer is reset to zero and the suffix
 * length increases.
 */
static uint64_t
bucket_for_hash(linear_hash_t *lh, uint64_t hash)
{
	uint64_t mask = (1ULL << (lh->lh_hash_suffix_length)) - 1;
	if ((hash & mask) < lh->lh_split_pointer) { mask = (mask << 1) | 1; }
	return hash & mask;
}

/* Send an entry iterator struct back to its allocator */
static void
save_entry_iterator(entry_iterator_t *iter) {
	if (!iter->ei_dirty) { return; }
	CHECKED("saving entry iterator", allocator_store(iter->ei_alloc,
		iter->ei_bucket_ix, &iter->ei_bucket));
	iter->ei_dirty = B_FALSE;
}

/* Read in the bucket struct corresponding to an entry iterator */
static void
read_entry_iterator(entry_iterator_t *iter)
{
	CHECKED("reading entry iterator", allocator_retrieve(iter->ei_alloc,
		iter->ei_bucket_ix, &iter->ei_bucket));
}

/*
 * Updates entry_iterator struct, returns false when there are no more
 * entries, or, alternately, extends the bucket chain indefinitely.
 */
boolean_t
entry_iterator_next(entry_iterator_t *iter, boolean_t extend)
{
	linear_hash_t *lh = iter->ei_lh;

start:	if (iter->ei_entry_ix < 0) {
		read_entry_iterator(iter);
		iter->ei_entry_ix = 0;
		iter->ei_dirty = B_FALSE;
		return B_TRUE;
	} else if (iter->ei_entry_ix == ENTRIES_PER_BUCKET - 1) {
		save_entry_iterator(iter);
		if (iter->ei_bucket.b_overflow) {
			iter->ei_alloc = lh->lh_overflow_alloc;
			iter->ei_entry_ix = -1;
			iter->ei_bucket_ix = iter->ei_bucket.b_overflow;
			goto start;
		} else if (!extend) {
			return B_FALSE;
		} else {
			bucket_t new_overflow_bucket = {};
			iter->ei_bucket.b_overflow =
				allocator_append(lh->lh_overflow_alloc,
					&new_overflow_bucket);
			assert(iter->ei_bucket.b_overflow > 0);
			iter->ei_dirty = B_TRUE;
			save_entry_iterator(iter);
			iter->ei_bucket_ix = iter->ei_bucket.b_overflow;
			iter->ei_bucket = new_overflow_bucket;
			iter->ei_alloc = lh->lh_overflow_alloc;
			iter->ei_entry_ix = 0;
			return B_TRUE;
		}
	} else {
		iter->ei_entry_ix += 1;
		return B_TRUE;
	}
}

/*
 *
  * Split a bucket, rehashing all entries according to the one-higher suffix
  * length. Since only one bit is added, existing entries either stay where
  * they are or go to one alternate bucket. We'll do this partition in two
  * passes for clarity and reliability: one to eject relocated entries and
  * one to consolidate entries now that some may have been removed.
  *
  * It is an invariant that at steady state, bucket entries must be filled
  * linearly, even if there are unused entries.
 */
static void
split_bucket(linear_hash_t *lh)
{
	START_VALIDATION(lh);
	begin_ops_tracking(lh, &lh->lh_stats.lhs_splits);

	record_ix_t bucket_ix = (record_ix_t)lh->lh_split_pointer;
	record_ix_t buddy_ix = bucket_ix | (1ULL << lh->lh_hash_suffix_length);

	entry_iterator_t source = ITER_BUCKET(lh, bucket_ix);
	entry_iterator_t stay   = ITER_BUCKET(lh, bucket_ix);
	entry_iterator_t move   = ITER_BUCKET(lh, buddy_ix);

	/* Increment split pointer so bucket_for_hash uses extended length */
	lh->lh_split_pointer++;

	/* Partition */
	while (entry_iterator_next(&source, B_FALSE)) {
		bucket_entry_t *sbe =
			&source.ei_bucket.b_entries[source.ei_entry_ix];
		if (sbe->be_record == 0) {
			break;	/* Entries must be filled in order; done */
		}
		boolean_t stays = bucket_for_hash(lh, sbe->be_hash) == bucket_ix;
		entry_iterator_t *dest = stays ? &stay : &move;
		(void) entry_iterator_next(dest, B_TRUE);
		bucket_entry_t *dbe =
			&dest->ei_bucket.b_entries[dest->ei_entry_ix];
		if (dbe->be_hash != sbe->be_hash ||
			dbe->be_record != sbe->be_record)
		{
			*dbe = *sbe;
			dest->ei_dirty = B_TRUE;
		}
	}

	/* Zero out the rest of the source bucket */
	while (entry_iterator_next(&stay, B_FALSE)) {
		record_ix_t stay_ix = stay.ei_entry_ix;
		bucket_entry_t *entry = &stay.ei_bucket.b_entries[stay_ix];
		if (entry->be_record) {
			*entry = (bucket_entry_t){};
			stay.ei_dirty = B_TRUE;
		}
	}
	save_entry_iterator(&stay);
	save_entry_iterator(&move);

	/* Have we completed the full hashing cycle at this suffix length? */
	record_ix_t buckets_this_cycle = (1ULL << (lh->lh_hash_suffix_length));
	if (lh->lh_split_pointer >= buckets_this_cycle) {
		lh->lh_hash_suffix_length++;
		lh->lh_split_pointer = 0;
	}

	complete_ops_tracking(lh);
	END_VALIDATION(lh);
}

static void
check_split(linear_hash_t *lh) {
	double occupancy = (double)lh->lh_stats.lhs_num_entries /
		((1ULL << lh->lh_hash_suffix_length) * ENTRIES_PER_BUCKET);
	if (occupancy > MAX_OCCUPANCY) {
		split_bucket(lh);
	}
}

static void
check_memory_use(linear_hash_t *lh) {
	allocator_stats_t bucket, over, data;
	allocator_get_stats(lh->lh_bucket_alloc, &bucket);
	allocator_get_stats(lh->lh_overflow_alloc, &over);
	allocator_get_stats(lh->lh_data_alloc, &data);
	size_t total_mem = bucket.as_mem_used + over.as_mem_used +
		data.as_mem_used;
	if (total_mem > lh->lh_stats.lhs_mem_highwater) {
		lh->lh_stats.lhs_mem_highwater = total_mem;
	}
	if (total_mem > lh->lh_max_memory) {
		allocator_t *allocator_to_convert;
		if (data.as_mem_used) {
			allocator_to_convert = lh->lh_data_alloc;
		} else if (over.as_mem_used) {
			allocator_to_convert = lh->lh_overflow_alloc;
		} else {
			allocator_to_convert = lh->lh_bucket_alloc;
		}
		CHECKED("converting allocator to disk storage",
			allocator_convert_to_disk(allocator_to_convert));
	}
}

/* Buffer length must be pre-checked */
static FILE *
create_temp_file(const char *dir, char *pathbuff) {
	sprintf(pathbuff, "%s/zstream-linear-hash-XXXXXX", dir);
	int fd = mkstemp(pathbuff);
	if (fd >= 0) {
		FILE *fp = fdopen(fd, "w+");
		assert(fp);
		if (unlink(pathbuff) < 0) {
			perror(pathbuff);
		}
		return fp;
	} else {
		perror(pathbuff);
		exit(1);
	}
}

linear_hash_t *
lh_init(size_t record_size, size_t max_mem, const char *cache_dir)
{
	assert(record_size);
	size_t bkt = sizeof(bucket_t);

	linear_hash_t *lh = safe_malloc(sizeof(struct linear_hash));
	*lh = (struct linear_hash){
		.lh_record_size = record_size,
		.lh_hash_suffix_length = INITIAL_HASH_SUFFIX_LENGTH,
		.lh_max_memory = max_mem
	};

	/* Initialize allocators */
	if (cache_dir) {
		char path[1024];
		if (strlen(cache_dir) > sizeof(path) - 32) {
			(void) fprintf(stderr, "Cache dir path too long.\n");
			free(lh);
			return NULL;
		}
		FILE *fp = create_temp_file(cache_dir, path);
		lh->lh_data_alloc = allocator_init(record_size, max_mem, fp);
		fp = create_temp_file(cache_dir, path);
		lh->lh_bucket_alloc = allocator_init(bkt, max_mem, fp);
		fp = create_temp_file(cache_dir, path);
		lh->lh_overflow_alloc = allocator_init(bkt, max_mem, fp);
	} else {
		lh->lh_data_alloc = allocator_init(record_size, max_mem, NULL);
		lh->lh_bucket_alloc = allocator_init(bkt, max_mem, NULL);
		lh->lh_overflow_alloc = allocator_init(bkt, max_mem, NULL);
	}
	if (!lh->lh_data_alloc || !lh->lh_bucket_alloc ||
		!lh->lh_overflow_alloc)
	{
		(void) fprintf(stderr, "Unable to initialize allocators\n");
		lh_destroy(lh);
		return NULL;
	}
	/*
	 * Skip first overflow and data buckets to allow 0 to indicate
	 * empty or end-of-chain.
	 */
	if ((allocator_skip(lh->lh_data_alloc) < 0) ||
		(allocator_skip(lh->lh_overflow_alloc) < 0))
	{
		lh_destroy(lh);
		return NULL;
	}
	return lh;
}

void
lh_insert(linear_hash_t *lh, uint64_t hash, const void* data)
{
	assert(lh && data);
	START_VALIDATION(lh);
	begin_ops_tracking(lh, &lh->lh_stats.lhs_inserts);

	record_ix_t record = allocator_append(lh->lh_data_alloc, data);
	if (record < 0) {
		(void) fprintf(stderr, "Error inserting new hash entry.\n");
		exit(1);
	}
	entry_iterator_t iter = ITER_BUCKET(lh, bucket_for_hash(lh, hash));
	bucket_entry_t new_entry = {hash, record};
	while (entry_iterator_next(&iter, B_TRUE)) {
		bucket_entry_t *entry =
			&iter.ei_bucket.b_entries[iter.ei_entry_ix];
		if (!entry->be_record) {
			*entry = new_entry;
			iter.ei_dirty = B_TRUE;
			save_entry_iterator(&iter);
			break;
		}
	}
	lh->lh_stats.lhs_num_entries++;

	complete_ops_tracking(lh);
	END_VALIDATION(lh);
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
	assert(lh);
	begin_ops_tracking(lh, &lh->lh_stats.lhs_retrieves);

	lh_iterator_t *iter = &lh->lh_iterators[lh->lh_next_iterator];
	lh->lh_next_iterator = (lh->lh_next_iterator + 1) % MAX_LH_ITERATORS;
	*iter = (struct lh_iterator){
		.lhi_hash = hash,
		.lhi_entry_iterator = {
			.ei_lh = lh,
			.ei_alloc = lh->lh_bucket_alloc,
			.ei_bucket_ix = bucket_for_hash(lh, hash),
			.ei_entry_ix = -1
		}
	};
	return (iter);
}

boolean_t
lh_retrieve_next(lh_iterator_t *iter, void *buffer) {
	entry_iterator_t *ei = &iter->lhi_entry_iterator;
	while (entry_iterator_next(ei, B_FALSE)) {
		bucket_entry_t *entry =
			&ei->ei_bucket.b_entries[ei->ei_entry_ix];
		if (entry->be_record == 0) {
			complete_ops_tracking(ei->ei_lh);
			return B_FALSE;
		}
		if (entry->be_hash == iter->lhi_hash) {
			CHECKED("retrieving next hash entry",
				allocator_retrieve(ei->ei_lh->lh_data_alloc,
					entry->be_record, buffer));
			update_ops_tracking(ei->ei_lh);
			return B_TRUE;
		}
	}
	complete_ops_tracking(ei->ei_lh);
	return B_FALSE;
}

void
lh_destroy(linear_hash_t *lh) {
	assert(lh);
	if (lh->lh_validate) {
		lh_validate(lh);
	}
	if (lh->lh_data_alloc) 	{ allocator_destroy(lh->lh_data_alloc); }
	if (lh->lh_bucket_alloc) { allocator_destroy(lh->lh_bucket_alloc); }
	if (lh->lh_overflow_alloc) { allocator_destroy(lh->lh_overflow_alloc); }
	free(lh);
}
