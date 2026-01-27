/*
 * Linear hashing implementation
 */

#include "linear_hash.h"
#include "allocator.h"
#include <stdlib.h>
#include <string.h>

#define ENTRIES_PER_BUCKET 2
#define MAX_OCCUPANCY 0.75
#define INITIAL_HASH_SUFFIX_LENGTH 7
#define ITER_COMPLETE 1

n = 2^(level + 1)
#define CHECKED(type, expr) \
	type code = expr;
	if (code < 0) { return code; }

/* Entry in a bucket: hash value + locator to data */
typedef struct {
	uint64_t  hash;
	record_ix record;  /* index of actual data */
} bucket_entry_t;

/* Bucket structure: fixed array of entries + overflow pointer */
typedef struct {
	bucket_entry_t entries[ENTRIES_PER_BUCKET];
	record_ix overflow;  /* 0 if no overflow */
} bucket_t;

struct linear_hash {
	size_t record_size;
	uint8_t hash_suffix_length;/* current hashing granularity below split */
	record_ix split_pointer;   /* next bucket to split */
	uint64_t num_entries;      /* total entries in table */
	uint64_t num_splits;       /* statistics */

	allocator_t data_alloc;     /* data records */
	allocator_t bucket_alloc;   /* main buckets */
	allocator_t overflow_alloc; /* overflow buckets */
};

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
	uint64_t mask = 1ULL << (lh->hash_suffix_length - 1) - 1;
	uint64_t bucket = hash & mask;
	if bucket < lh->split_pointer {
		mask = (mask << 1) & 1;
		bucket = hash & mask;
	}
	return bucket;
}

typedef static struct entry_iterator {
	allocator_t *alloc;
	record_ix bucket_ix;
	int64_t entry_ix; /* -1 == bucket not yet retrieved */
	bucket_t bucket;
	bool dirty = false;
} entry_iterator_t;

/* ITER_COMPLETE for end of chain, negative for error, 0 == valid */
static int
entry_iterator_get_next(linear_hash_t *lh, entry_iterator_t *iter) {
	if (iter->entry_ix < 0) {
		CHECKED(record_ix, allocator_retrieve(iter->alloc, iter->bucket_ix,
			&iter->bucket));
		iter->entry_ix = 0;
		iter->dirty = false;
	} else if (iter->entry_ix == ENTRIES_PER_BUCKET - 1) {
		if (iter->dirty) {
			CHECKED(record_ix, allocator_store(iter->alloc, &iter->bucket, 
				iter->bucket_ix));
			iter->dirty = false;
		}
		if (!iter->bucket.overflow) {
			return ITER_COMPLETE;
		}
		iter->alloc = lh->overflow_alloc;
		iter->entry_index = -1;
		iter->bucket_ix = iter->bucket.overflow;
		return entry_iterator_get_next(lh, iter);
	} else {
		iter->entry_ix += 1;
		return 0;
	}
}

/* Split a bucket, rehashing all entries according to the current
suffix length. Since only one bit is added, existing entries either
stay where they are or go to one alternate bucket. We'll do this partition
in two passes for clarity and reliability: one to eject relocated entries
and one to consolidate entries now that some may have been removed. */
static int 
split_bucket(linear_hash_t* lh) {

	record_ix bucket_being_split = lh->split_pointer++;
	entry_iterator_t iter = {lh->bucket_alloc, bucket_being_split, 0 };
	bucket_entry_t empty_entry = {0, 0};

	/* Partition */
	while(true) {
		CHECKED(int, entry_iterator_get_next(lh, &iter));
		if (code == ITER_COMPLETE) { break; }
		bucket_entry_t entry = iter.bucket.entries[iter.entry_ix];
		record_ix new_bucket_ix = bucket_for_hash(lh, entry->hash);
		if (new_bucket_ix != bucket_being_split) {
			iter.bucket.entries[iter.entry_ix] = empty_entry;
			iter.dirty = true;
			CHECKED(int, store_in_bucket_chain(lh, new_bucket_ix, entry));
		}	
	}

	/* Consolidate. It doesn't matter that there are two iterators
	running simultaneously because the head will never write and will
	ever be behind the tail. */

	entry_iterator_t head = {lh->bucket_alloc, bucket_being_split, 0};
	entry_iterator_t tail = head;

	CHECKED(int, entry_iterator_get_next(lh, &tail));
	while (true) {
		CHECKED(int, entry_iterator_get_next(lh, &head));
		if (code == ITER_COMPLETE) { break; }
		bucket_entry_t head_entry = head.bucket.entries[head.entry_ix];
		if (head_entry.record != 0) {
			bucket_entry_t tail_entry = tail.bucket.entries[tail.entry_ix];
			if (tail_entry.hash != head_entry.hash || 
				tail_entry.record != head_entry.record)
			{
				tail.bucket.entries[tail.entry_ix] = head_entry;
				tail.dirty = true;
			}
			/* Can't get ITER_COMPLETE before head */
			CHECKED(int, entry_iterator_get_next(lh, &tail));
		}
	}

	/* Zero out the remaining entries */
	while (true) {
		tail.bucket.entries[tail.entry_ix] = empty_entry;
		tail.dirty = true;
		CHECKED(int, entry_iterator_get_next(lh, &tail));
		if (code == ITER_COMPLETE) { break; }
	}

	/* Check if we've completed this level. Split pointer was 
	already incremented. */
	uint64_t buckets_this_cycle = (1ULL << lh->hash_suffix_length) - 1;
	if (lh->split_pointer >= buckets_this_cycle) {
		lh->hash_suffix_length++;
		lh->split_pointer = 0;
	}

	lh->num_splits++;
	return 0;
}

static int
check_split(linear_hash_t *lh) {
	double occupancy = (double)lh->num_entries /
		(lh->num_buckets * ENTRIES_PER_BUCKET);
	if (occupancy > OCCUPANCY_THRESHOLD) {
		return (split_bucket(lh));
	}
	return 0;
}

/* Considers only the specific bucket, not the chain.
 * Bucket must already have been retrieved before calling, and
 * the bucket must be resaved afterwards (if successful). */
static boolean_t
attempt_add_to_bucket(bucket_t *bucket, bucket_entry_t entry) {
	for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
		if (bucket->entries[i].record == 0) {
			bucket->entries[i] = entry;
			return (true);
		}
	}
	return (false);
}

/* Stores somewhere in bucket chain, extending if needed */
static int
store_in_bucket_chain(linear_hash_t *lh, record_ix chain_head,
	bucket_entry_t entry)
{
	bucket_t this_bucket;
	allocator_t *this_allocator = lh->bucket_alloc;
	record_ix bucket_ix = chain_head;

	CHECKED(record_ix, allocator_retrieve(alloc, record, &this_bucket));

	while (true) {
		if (attempt_add_to_bucket(&this_bucket, entry)) {
			CHECKED(record_ix, allocator_store(this_allocator, 
				&this_bucket, bucket_ix)); 
			return 0;
		} else if (this_bucket.overflow) {
			bucket_ix = this_bucket.overflow;
			this_allocator = lh->overflow_alloc;
			CHECKED(record_ix, allocator_retrieve(this_allocator, 
				bucket_ix, &this_bucket));
		} else {
			bucket_t new_overflow = new_bucket();
			(void) attempt_add_to_bucket(&new_overflow, entry);
			record_ix new_rec = allocator_append(lh->overflow_alloc,
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
	return bucket
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
	lh->num_buckets = 1 << (INITIAL_HASH_SUFFIX_LENGTH - 1);

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
	if record < 0 { return record; }
	bucket_entry_t entry = {hash, record};
	record_ix bucket_ix = bucket_for_hash(lh, hash);
	CHECKED(int, store_in_bucket_chain(lh, bucket_ix, entry));
	lh->num_entries++;
	return (check_split(lh) < 0) ? -1 : 0;
}

int lh_retrieve_start(linear_hash_t* lh, uint64_t hash,
					  lh_iterator_t* iter, void* buffer) {
	if (!lh || !iter || !buffer) {
		return -1;
	}

	hash &= HASH_MASK;

	memset(iter, 0, sizeof(lh_iterator_t));
	iter->hash = hash;
	iter->bucket_index = bucket_for_hash(lh, hash);
	iter->entry_index = 0;
	iter->overflow_loc = 0;
	iter->mode = 0;  /* hash search mode */

	return lh_next(lh, iter, buffer);
}

int lh_retrieve_all_start(linear_hash_t* lh, lh_iterator_t* iter, void* buffer) {
	if (!lh || !iter || !buffer) {
		return -1;
	}

	memset(iter, 0, sizeof(lh_iterator_t));
	iter->bucket_index = 0;
	iter->entry_index = 0;
	iter->overflow_loc = 0;
	iter->mode = 1;  /* retrieve all mode */

	return lh_next(lh, iter, buffer);
}

int lh_next(linear_hash_t* lh, lh_iterator_t* iter, void* buffer) {
	if (!lh || !iter || !buffer) {
		return -1;
	}

	bucket_t bucket;
	bucket_t* bucket_ptr;

	while (1) {
		/* Determine which bucket we're scanning */
		if (iter->overflow_loc == 0) {
			/* Scanning main bucket */
			if (iter->mode == 0) {
				/* Hash search: only search one bucket */
				if (iter->bucket_index >= lh->num_buckets) {
					return 0;  /* no more entries */
				}
				bucket_ptr = &lh->buckets[iter->bucket_index];
			} else {
				/* Retrieve all: scan all buckets */
				if (iter->bucket_index >= lh->num_buckets) {
					return 0;  /* no more buckets */
				}
				bucket_ptr = &lh->buckets[iter->bucket_index];
			}
		} else {
			/* Scanning overflow bucket */
			if (allocator_retrieve(lh->overflow_alloc, iter->overflow_loc,
								  &bucket) != 0) {
				return -1;
			}
			bucket_ptr = &bucket;
		}

		/* Scan entries in current bucket */
		while (iter->entry_index < ENTRIES_PER_BUCKET) {
			bucket_entry_t* entry = &bucket_ptr->entries[iter->entry_index];
			iter->entry_index++;

			if (entry->record == 0) {
				continue;  /* empty slot */
			}

			/* Check if entry matches (for hash search) */
			if (iter->mode == 0) {
				uint64_t entry_hash = entry->hash & HASH_MASK;
				if (entry_hash != iter->hash) {
					continue;  /* hash doesn't match */
				}
			}

			/* Retrieve data */
			if (allocator_retrieve(lh->data_alloc, entry->record,
								  buffer) != 0) {
				return -1;
			}

			return 1;  /* found entry */
		}

		/* Move to overflow bucket if exists */
		if (iter->overflow_loc == 0 && bucket_ptr->overflow != 0) {
			iter->overflow_loc = bucket_ptr->overflow;
			iter->entry_index = 0;
			continue;
		}

		/* Move to next overflow in chain */
		if (iter->overflow_loc != 0) {
			if (bucket_ptr->overflow != 0) {
				iter->overflow_loc = bucket_ptr->overflow;
				iter->entry_index = 0;
				continue;
			}
		}

		/* Done with this bucket */
		if (iter->mode == 0) {
			/* Hash search: only search one bucket */
			return 0;
		} else {
			/* Retrieve all: move to next bucket */
			iter->bucket_index++;
			iter->entry_index = 0;
			iter->overflow_loc = 0;

			if (iter->bucket_index >= lh->num_buckets) {
				return 0;  /* no more buckets */
			}
		}
	}
}

void lh_get_stats(linear_hash_t* lh, uint64_t* num_entries,
				  uint64_t* num_buckets, uint64_t* num_splits) {
	if (!lh) {
		return;
	}

	if (num_entries) *num_entries = lh->num_entries;
	if (num_buckets) *num_buckets = lh->num_buckets;
	if (num_splits) *num_splits = lh->num_splits;
}

void lh_destroy(linear_hash_t* lh) {
	if (!lh) {
		return;
	}

	allocator_destroy(lh->data_alloc);
	allocator_destroy(lh->bucket_alloc);
	allocator_destroy(lh->overflow_alloc);

	free(lh->buckets);
	free(lh);
}
