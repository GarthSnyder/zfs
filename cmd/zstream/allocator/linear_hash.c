/*
 * Linear hashing implementation
 */

#include "linear_hash.h"
#include <stdlib.h>
#include <string.h>

#define MAX_OCCUPANCY 0.75
#define INITIAL_HASH_SUFFIX_LENGTH 8
#define DEBUG_SPLITS true

#define CHECKED(type, expr) \
	type ret = expr; \
	if (ret < 0) { return ret; } 

#define ITER_BUCKET(lh, bucket) \
	{&lh->bucket_alloc, bucket, -1, {0}, false, false}

/* Forward declarations */
static int split_bucket(linear_hash_t* lh);
static int store_in_bucket_chain(linear_hash_t *lh, record_ix chain_head, bucket_entry_t entry);
static bucket_t new_bucket(void);
static int entry_iterator_get_next(linear_hash_t *lh, entry_iterator_t *iter);

/*
Calculate bucket for a given hash value.

Here, hashing level is defined as the hash suffix length 
in effect below the split point. At or above, it is one bit less.
E.g., if level = 3, items below the split point  
are hashed into 8 buckets and at the split point or above, they were
previously hashed into 4 buckets. Ergo, when the split pointer reaches
index 4, 2^(level-1), all mod 4 entries have been upgraded. The
split pointer is reset to zero and the suffix length increases. 
*/

static uint64_t
bucket_for_hash(linear_hash_t* lh, uint64_t hash) {
	uint64_t mask = (1ULL << (lh->hash_suffix_length - 1)) - 1;
	uint64_t bucket = hash & mask;
	if ((record_ix)bucket < lh->split_pointer) {
		mask = (mask << 1) | 1;
		bucket = hash & mask;
	}
	return bucket;
}

/* Sets iteration_complete after trying to read off end of chain. */
static int
entry_iterator_get_next(linear_hash_t *lh, entry_iterator_t *iter) {
	if (iter->entry_ix < 0) {
		CHECKED(record_ix, allocator_retrieve(iter->alloc, iter->bucket_ix,
			&iter->bucket));
		iter->entry_ix = 0;
		iter->dirty = false;
		return 0;
	} else if (iter->entry_ix == ENTRIES_PER_BUCKET - 1) {
		if (iter->dirty) {
			CHECKED(record_ix, allocator_store(iter->alloc, &iter->bucket,
				iter->bucket_ix));
			iter->dirty = false;
		}
		if (!iter->bucket.overflow) {
			iter->iteration_complete = true;
			return 0;
		}
		iter->alloc = &lh->overflow_alloc;
		iter->entry_ix = -1;
		iter->bucket_ix = iter->bucket.overflow;
		return entry_iterator_get_next(lh, iter);
	} else {
		iter->entry_ix += 1;
		return 0;
	}
}

#ifdef DEBUG_SPLITS
int
validate(linear_hash_t *lh) {
	uint64_t total_entries = 0;
	bool print_hash = false; // lh->num_entries >= 153;
	if (print_hash) {
		fprintf(stderr, "Linear hash dump, split pointer at %d:\n",
			(int)lh->split_pointer);
	}
	for (uint64_t i = 0; i < lh->bucket_alloc.count; i++) {
		entry_iterator_t iter = ITER_BUCKET(lh, i);
		bool saw_empty_entry = false;
		uint64_t full_entries = 0;
		uint64_t empty_entries = 0;
		CHECKED(int, entry_iterator_get_next(lh, &iter));
		while (true) {
			bucket_entry_t entry = iter.bucket.entries[iter.entry_ix];
			if (entry.record) {
				if (saw_empty_entry) {
					fprintf(stderr, "validate: bucket %d has "
						"uncompacted entries.\nEntry %d is the "
						"first after an empty entry.\n", (int)i, 
						(int)iter.entry_ix);
				}
				full_entries++;
			} else {
				empty_entries++;
				saw_empty_entry = true;
			}
			CHECKED(int, entry_iterator_get_next(lh, &iter));
			if (iter.iteration_complete) { break; }
		}
		total_entries += full_entries;
		if (print_hash) {
			fprintf(stderr, "    Bucket %d: %d blocks, %d full, %d empty\n",
				(int)i, (int)(full_entries + empty_entries) / ENTRIES_PER_BUCKET,
				(int)full_entries, (int)empty_entries);
		}
	}
	if (total_entries != lh->num_entries) {
		fprintf(stderr, "validate: linear hash is supposed to have %d "
			"entries, but actually has %d.\n", (int)lh->num_entries,
			(int)total_entries);
	}
	return 0;
}
#endif

/* Split a bucket, rehashing all entries according to the current
suffix length. Since only one bit is added, existing entries either
stay where they are or go to one alternate bucket. We'll do this partition
in two passes for clarity and reliability: one to eject relocated entries
and one to consolidate entries now that some may have been removed. 

It is an invariant that bucket entries must be filled linearly, even
if there are unused entries. */
static int
split_bucket(linear_hash_t* lh) {

#ifdef DEBUG_SPLITS
	if (validate(lh) != 0) {
		fprintf(stderr, "validation did not complete successfully\n");
	}
#endif

	record_ix bucket_being_split = (record_ix)lh->split_pointer;
	entry_iterator_t iter = ITER_BUCKET(lh, bucket_being_split);
	bucket_entry_t empty_entry = {0, 0};

	/* Increment split pointer now so bucket_for_hash uses new value */
	lh->split_pointer++;

	/* Partition */
	while(true) {
		CHECKED(int, entry_iterator_get_next(lh, &iter));
		if (iter.iteration_complete) { break; }
		bucket_entry_t entry = iter.bucket.entries[iter.entry_ix];
		/* Skip empty entries */
		if (entry.record == 0) continue;
		record_ix new_bucket_ix = bucket_for_hash(lh, entry.hash);
		if (new_bucket_ix != bucket_being_split) {
			iter.bucket.entries[iter.entry_ix] = empty_entry;
			iter.dirty = true;
			CHECKED(int, store_in_bucket_chain(lh, new_bucket_ix, entry));
		}
	}

	/* Consolidate. It doesn't matter that there are two iterators
	running simultaneously because the head will never write and will
	never be behind the tail. */

	entry_iterator_t head = ITER_BUCKET(lh, bucket_being_split);
	entry_iterator_t tail = head;

	CHECKED(int, entry_iterator_get_next(lh, &tail));
	while (true) {
		CHECKED(int, entry_iterator_get_next(lh, &head));
		if (head.iteration_complete) { break; }
		bucket_entry_t head_entry = head.bucket.entries[head.entry_ix];
		if (head_entry.record != 0) {
			bucket_entry_t tail_entry = tail.bucket.entries[tail.entry_ix];
			if (tail_entry.hash != head_entry.hash ||
				tail_entry.record != head_entry.record)
			{
				tail.bucket.entries[tail.entry_ix] = head_entry;
				tail.dirty = true;
			}
			CHECKED(int, entry_iterator_get_next(lh, &tail));
		}
	}

	/* Zero out the remaining entries */
	while (!tail.iteration_complete) {
		tail.bucket.entries[tail.entry_ix] = empty_entry;
		tail.dirty = true;
		CHECKED(int, entry_iterator_get_next(lh, &tail));
	}

	/* Check if we've completed this level. Split pointer was
	already incremented. */
	uint64_t buckets_this_cycle = (1ULL << (lh->hash_suffix_length - 1));
	if (lh->split_pointer >= (record_ix)buckets_this_cycle) {
		lh->hash_suffix_length++;
		lh->split_pointer = 0;
	}

	lh->num_splits++;

#ifdef DEBUG_SPLITS
	if (validate(lh) != 0) {
		fprintf(stderr, "validation did not complete successfully\n");
	}
#endif

	return 0;
}

static int
check_split(linear_hash_t *lh) {
	uint64_t num_buckets = (1ULL << (lh->hash_suffix_length - 1)) + lh->split_pointer;
	double occupancy = (double)lh->num_entries /
		(num_buckets * ENTRIES_PER_BUCKET);
	if (occupancy > MAX_OCCUPANCY) {
		return (split_bucket(lh));
	}
	return 0;
}

/* Considers only the specific bucket, not the chain.
 * Bucket must already have been retrieved before calling, and
 * the bucket must be resaved afterwards (if successful). */
static bool
attempt_add_to_bucket(bucket_t *bucket, bucket_entry_t entry) {
	for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
		if (bucket->entries[i].record == 0) {
			bucket->entries[i] = entry;
			return true;
		}
	}
	return false;
}

/* Stores somewhere in bucket chain, extending if needed */
static int
store_in_bucket_chain(linear_hash_t *lh, record_ix chain_head,
	bucket_entry_t entry)
{
	bucket_t this_bucket;
	allocator_t *this_allocator = &lh->bucket_alloc;
	record_ix bucket_ix = chain_head;

	CHECKED(record_ix, allocator_retrieve(this_allocator, bucket_ix, &this_bucket));

	while (true) {
		if (attempt_add_to_bucket(&this_bucket, entry)) {
			CHECKED(record_ix, allocator_store(this_allocator, 
				&this_bucket, bucket_ix)); 
			return 0;
		} else if (this_bucket.overflow) {
			bucket_ix = this_bucket.overflow;
			this_allocator = &lh->overflow_alloc;
			CHECKED(record_ix, allocator_retrieve(this_allocator,
				bucket_ix, &this_bucket));
		} else {
			bucket_t new_overflow = new_bucket();
			(void) attempt_add_to_bucket(&new_overflow, entry);
			record_ix new_rec = allocator_append(&lh->overflow_alloc,
				&new_overflow);
			if (new_rec < 0) { return new_rec; }
			this_bucket.overflow = new_rec;
			CHECKED(record_ix, allocator_store(this_allocator,
				&this_bucket, bucket_ix));
			return 0;
		}
	}
}

static bucket_t
new_bucket() {
	bucket_t bucket;
	memset(&bucket, 0, sizeof (bucket));
	return bucket;
}

int
lh_init(linear_hash_t *lh, size_t record_size, size_t max_memory)
{
	if (record_size == 0) {
		return (-1);
	}

	memset(lh, 0, sizeof(linear_hash_t));
	lh->record_size = record_size;
	lh->hash_suffix_length = INITIAL_HASH_SUFFIX_LENGTH;
	lh->split_pointer = 0;

	/* Initialize allocators */
	if (allocator_init_memory(&lh->data_alloc, record_size, max_memory)) {
		return (-2);
	}
	/* Bucket allocator */
	if (allocator_init_memory(&lh->bucket_alloc, sizeof (bucket_t), 
		max_memory))
	{
		allocator_destroy(&lh->data_alloc);
		return (-3);
	}
	/* Bucket overflow storage */
	if (allocator_init_memory(&lh->overflow_alloc, sizeof (bucket_t), 
		max_memory))
	{
		allocator_destroy(&lh->data_alloc);
		allocator_destroy(&lh->bucket_alloc);
		return (-4);
	}
	/* 
	 * Skip first overflow and data buckets to allow 0 
	 * to indicate empty or end-of-chain
	 */
	if ((allocator_skip(&lh->data_alloc) < 0) ||
		(allocator_skip(&lh->overflow_alloc) < 0))
	{
		return (-1);
	}
	return 0;
}

int lh_insert(linear_hash_t* lh, uint64_t hash, const void* data) {
	if (!lh || !data) {
		return (-1);
	}

	/* Store actual data record */
	record_ix record = allocator_append(&lh->data_alloc, data);
	if (record < 0) { return record; }
	bucket_entry_t entry = {hash, record};
	record_ix bucket_ix = bucket_for_hash(lh, hash);
	CHECKED(int, store_in_bucket_chain(lh, bucket_ix, entry));
	lh->num_entries++;
	return (check_split(lh) < 0) ? -1 : 0;
}

int
lh_retrieve_setup(linear_hash_t* lh, uint64_t hash, lh_iterator_t* iter) {
	if (!lh || !iter) {
		return -1;
	}
	memset(iter, 0, sizeof(lh_iterator_t));
	iter->lh = lh;
	iter->hash = hash;
	iter->entry_iterator.alloc = &lh->bucket_alloc;
	iter->entry_iterator.bucket_ix = bucket_for_hash(lh, hash);
	iter->entry_iterator.entry_ix = -1;
	return 0;
}

int
lh_retrieve_next(lh_iterator_t *iter, void *buffer) {
	entry_iterator_t *entry_iterator = &iter->entry_iterator;
	while (true) {
		CHECKED(int, entry_iterator_get_next(iter->lh, entry_iterator));
		if (entry_iterator->iteration_complete) {
			iter->iteration_complete = true;
			return 0;
		}
		bucket_entry_t entry = entry_iterator->bucket.entries[entry_iterator->entry_ix];
		if (entry.record == 0) {
			iter->iteration_complete = true;
			return 0;
		} 
		if (entry.hash == iter->hash) {
			CHECKED(record_ix, allocator_retrieve(&iter->lh->data_alloc, entry.record,
				buffer));
			return 0;
		}
	}
}

void lh_destroy(linear_hash_t* lh) {
	allocator_destroy(&lh->data_alloc);
	allocator_destroy(&lh->bucket_alloc);
	allocator_destroy(&lh->overflow_alloc);
}
