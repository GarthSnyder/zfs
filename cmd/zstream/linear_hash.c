/*
 * Linear hashing implementation
 */

#include <stdlib.h>
#include <string.h>
#include "linear_hash.h"
#include "linear_hash_stats.h"
#include "linear_hash_types.h"
#include "zstream_shared.h"

#define MAX_OCCUPANCY 0.75
#define INITIAL_HASH_SUFFIX_LENGTH 10
#define INSERTIONS_BETWEEN_MEM_CHECKS 1024

#define CHECKED(type, expr, doing_what) \
	type ret = expr; \
	if (ret < 0) { \
		(void) fprintf(stderr, "Error while %s.\n", doing_what); \
		exit(1); \
	} 

#define ITER_BUCKET(lh, bucket) \
	{lh, lh->bucket_alloc, bucket, -1, {{{0}}}, false}

#define EMPTY_BUCKET {{{0}}, 0}

static uint64_t
total_io_ops(linear_hash_t *lh) {
	allocator_stats_t data, bucket, over;
	allocator_get_stats(lh->data_alloc, &data);
	allocator_get_stats(lh->bucket_alloc, &bucket);
	allocator_get_stats(lh->overflow_alloc, &over);
	return data.num_ops + bucket.num_ops + over.num_ops;
}

/*
Calculate bucket for a given hash value.

Here, suffix length is defined as the hash suffix length 
in effect at or above the split point. Below the split,
it is one bit longer.
E.g., if hash_suffix_length = 3, items below the split point  
are hashed into 16 buckets. At the split point or above, they are 
hashed into 8 buckets. Ergo, when the split pointer reaches
index 8, 2^level, all mod 8 entries have been upgraded. The
split pointer is reset to zero and the suffix length increases. 
*/

static uint64_t
bucket_for_hash(linear_hash_t *lh, uint64_t hash) {
	uint64_t mask = (1ULL << (lh->hash_suffix_length)) - 1;
	uint64_t bucket = hash & mask;
	if ((record_ix)bucket < lh->split_pointer) {
		mask = (mask << 1) | 1;
		bucket = hash & mask;
	}
	return bucket;
}

/* Send an entry iterator struct back to its allocator */
static void
save_entry_iterator(entry_iterator_t *iter) {
	if (iter->dirty) {
		CHECKED(record_ix, allocator_store(iter->alloc, iter->bucket_ix,
			&iter->bucket), "saving from entry iterator");
		iter->dirty = false;
	}
}

/* Read the bucket corresponding to an entry iterator */
static void
read_entry_iterator(entry_iterator_t *iter) {
	CHECKED(record_ix, allocator_retrieve(iter->alloc, iter->bucket_ix,
		&iter->bucket), "reading from entry iterator"); 
}

/* Updates entry_iterator_t struct, returns false when there are no more
 * entries, or, alternately, extends the bucket chain indefinitely.
  */
static bool
entry_iterator_next(entry_iterator_t *iter, bool extend) {
	while (true) {
		if (iter->entry_ix < 0) {
			read_entry_iterator(iter);
			iter->entry_ix = 0;
			iter->dirty = false;
			return true;
		} else if (iter->entry_ix == ENTRIES_PER_BUCKET - 1) {
			save_entry_iterator(iter);
			if (iter->bucket.overflow) {
				iter->alloc =  lh->overflow_alloc;
				iter->entry_ix = -1;
				iter->bucket_ix = iter->bucket.overflow;
			} else if (!extend) {
				return false;
			} else {
				bucket_t new_overflow_bucket = EMPTY_BUCKET;
				iter->bucket.overflow = allocator_append(lh->overflow_alloc,
					&new_overflow_bucket);
				assert(iter->bucket.overflow > 0);
				iter->dirty = true;
				save_entry_iterator(iter);
				iter->bucket_ix = iter->bucket.overflow;
				iter->bucket = new_overflow_bucket;
				iter->alloc = lh->overflow_alloc;
				iter->entry_ix = 0;
				return true;
			}
		} else {
			iter->entry_ix += 1;
			return true;
		}
	}
}

/* Split a bucket, rehashing all entries according to the one-higher
suffix length. Since only one bit is added, existing entries either
stay where they are or go to one alternate bucket. We'll do this partition
in two passes for clarity and reliability: one to eject relocated entries
and one to consolidate entries now that some may have been removed. 

It is an invariant that at steady state, bucket entries must be filled
* linearly, even if there are unused entries. */
static void
split_bucket(linear_hash_t* lh)
{
	bool started_ok = true;
	if (lh->validate) {
		started_ok = lh_validate(lh);
	}

	uint64_t start_ops = total_io_ops(lh);
	lh->stats.splits.count++;
	record_ix bucket_being_split = (record_ix)lh->split_pointer;
	record_ix buddy_ix = bucket_being_split | 
		(1 << (lh->hash_suffix_length + 1));
	bucket_entry_t empty_entry = {0, 0};

	entry_iterator_t source = ITER_BUCKET(lh, bucket_being_split);
	entry_iterator_t stay = ITER_BUCKET(lh, bucket_being_split);;
	entry_iterator_t move = ITER_BUCKET(lh, buddy_ix);

	/* Increment split pointer now so bucket_for_hash uses extended length */
	lh->split_pointer++;

	/* Partition */
	while(entry_iterator_next(&source, false)) {
		bucket_entry_t *source_entry = &source.bucket.entries[source.entry_ix];
		if (source_entry->record != 0) {
			entry_iterator_t *dest = (bucket_for_hash(lh, source_entry->hash) == 
				bucket_being_split) ? stay : move;
			(void) entry_iterator_next(dest, false);
			dest->bucket.entries[dest->entry_ix] = *source_entry;
			dest->dirty = true;
		} else {
			/* Entries must be filled in order */
			break;
		}
	}
	/* Zero out the rest of the source bucket */
	while(entry_iterator_next(&stay, false)) {
		stay.bucket.entries[stay.entry_ix] = empty_entry;
		stay.dirty = true;
	}
	save_entry_iterator(&stay);
	save_entry_iterator(&move);

	/* Completed the full hashing cycle at this suffix length? */
	uint64_t buckets_this_cycle = (1ULL << (lh->hash_suffix_length));
	if (lh->split_pointer >= (record_ix)buckets_this_cycle) {
		lh->hash_suffix_length++;
		lh->split_pointer = 0;
	}

	uint64_t end_ops = total_io_ops(lh);
	lh->stats.splits.num_io_ops += (end_ops - start_ops);

	if (lh->validate) {
		if (!validate(lh, false) && started_ok) {
			fprintf(stderr, "split_bucket corrupted the hash table\n");
		}
	}
}

static void
check_split(linear_hash_t *lh) {
	double occupancy = (double)lh->stats.num_entries /
		((1ULL << lh->hash_suffix_length) * ENTRIES_PER_BUCKET);
	if (occupancy > MAX_OCCUPANCY) {
		split_bucket(lh);
	}
}

static void
check_memory_use(linear_hash_t *lh) {
	allocator_stats_t bucket, over, data;
	allocator_get_stats(lh->bucket_alloc, &bucket);
	allocator_get_stats(lh->overflow_alloc, &over);
	allocator_get_stats(lh->data_alloc, &data);
	size_t total_memory = bucket.mem_used + over.mem_used + data.mem_used;
	lh->stats.mem_highwater = (total_memory > lh->stats.mem_highwater) ?
		total_memory : lh->stats.mem_highwater;
	if (total_memory > lh->max_memory) {
		allocator allocator_to_convert;
		if (data.mem_used) {
			allocator_to_convert = lh->data_alloc;
		} else if (over.mem_used) {
			allocator_to_convert = lh->overflow_alloc;
		} else {
			allocator_to_convert = lh->bucket_alloc;
		}
		CHECKED(int, allocator_convert_to_disk(allocator_to_convert),
			"converting allocator to disk storage");
	}
}

/* Buffer length must be pre-checked */
static FILE *
create_temp_file(const char *dir, char *pathbuff) {
	sprintf(pathbuff, "%s/linear-hash-XXXXXX", dir);
	int fd = mkstemp(pathbuff);
	if (fd < 0) {
		(void) fprintf(stderr, "Unable to open temp file %s.\n", pathbuff);
		exit(1);
	}
	FILE *fp = fdopen(fd, "w+");
	if (!fp) {
		(void) fprintf(stderr, "fdopen failed for temp file %s.\n", pathbuff);
		exit(1);
	}
	if (unlink(pathbuff) < 0) {
		perror(pathbuff);
		exit(1);
	}
	return fp;
}

void *
lh_init(size_t record_size, size_t max_memory, const char *cache_dir)
{
	assert(record_size);
	linear_hash_t *lh = safe_malloc(sizeof(linear_hash_t));
	memset(lh, 0, sizeof(*lh));
	lh->record_size = record_size;
	lh->hash_suffix_length = INITIAL_HASH_SUFFIX_LENGTH;
	lh->split_pointer = 0;
	lh->max_memory = max_memory;
	lh->validate = true;

	/* Initialize allocators */
	if (cache_dir) {
		char path[1024];
		if (strlen(cache_dir) > sizeof(path) - 32) {
			(void) fprintf(stderr, "Linear hash directory path too long.\n");
			free(lh);
			return NULL;
		}
		FILE *fp = create_temp_file(cache_dir, path);
		lh->data_alloc = allocator_init(record_size, max_memory, fp);
		fp = create_temp_file(cache_dir, path);
		lh->bucket_alloc = allocator_init(sizeof(bucket_t), max_memory, fp);
		fp = create_temp_file(cache_dir, path);
		lh->overflow_alloc = allocator_init(sizeof(bucket_t), max_memory, fp);
	} else {
		lh->data_alloc = allocator_init(record_size, max_memory, NULL);
		lh->bucket_alloc = allocator_init(sizeof(bucket_t), max_memory, NULL);
		lh->overflow_alloc = allocator_init(sizeof(bucket_t), max_memory, NULL);
	}
	if (!lh->data_alloc || !lh->bucket_alloc || !lh->overflow_alloc) {
		(void) fprintf(stderr, "Unable to initialize allocators\n");
		lh_destroy(lh);
		return NULL;
	}
	/*
	 * Skip first overflow and data buckets to allow 0
	 * to indicate empty or end-of-chain
	 */
	if ((allocator_skip(lh->data_alloc) < 0) ||
		(allocator_skip(lh->overflow_alloc) < 0))
	{
		lh_destroy(lh);
		return NULL;
	}
	return lh;
}

void
lh_insert(void* lh_in, uint64_t hash, const void* data) {
	linear_hash_t *lh = lh_in;
	assert(lh && data);
	uint64_t start_ops = total_io_ops(lh);
	record_ix record = allocator_append(lh->data_alloc, data);
	if (record < 0) {
		(void) fprintf(stderr, "Error inserting new hash entry.\n");
		exit(1);
	}
	bool ok_to_start = true;
	if (lh->validate) {
		ok_to_start = validate(lh, false);
	}
	entry_iterator_t iter = ITER_BUCKET(lh, bucket_for_hash(lh, hash));
	bucket_entry_t new_entry = {hash, record};
	while (entry_iterator_next(lh, &iter, true)) {
		bucket_entry_t *entry = &iter.bucket.entries[iter.entry_ix];
		if (!entry->record) {
			*entry = new_entry;
			iter.dirty = true;
			save_entry_iterator(&iter);
			break;
		}
	}
	lh->stats.num_entries++;
	uint64_t end_ops = total_io_ops(lh);
	lh->stats.inserts.num_io_ops += (end_ops - start_ops);
	lh->stats.inserts.count++;
	if (lh->validate && ok_to_start && !validate(lh, false)) {
		fprintf(stderr, "Problem created in lh_insert\n");
	}
	check_split(lh);
	lh->next_memory_check--;
	if (lh->next_memory_check <= 0) {
		lh->next_memory_check = INSERTIONS_BETWEEN_MEM_CHECKS;
		check_memory_use(lh);
	}
}

static uint64_t retrieve_start_ops = 0;
static uint64_t retrieve_end_ops = 0;

void *
lh_initiate_retrieve(void *lh_in, uint64_t hash) {
	linear_hash_t *lh = lh_in;
	assert(lh);
	if (retrieve_start_ops && retrieve_end_ops) {
		lh->stats.retrieves.count++;
		lh->stats.retrieves.num_io_ops +=
			(retrieve_end_ops - retrieve_start_ops);
	}
	retrieve_start_ops = total_io_ops(lh);
	retrieve_end_ops = 0;
	lh_iterator_t *iter = &lh->iterators[lh->next_iterator];
	lh->next_iterator = (lh->next_iterator + 1) % MAX_ITERATORS_OUTSTANDING;
	memset(iter, 0, sizeof(*iter));
	iter->lh = lh;
	iter->hash = hash;
	iter->entry_iterator.alloc = lh->bucket_alloc;
	iter->entry_iterator.bucket_ix = bucket_for_hash(lh, hash);
	iter->entry_iterator.entry_ix = -1;
	return iter;
}

bool
lh_retrieve_next(void *iter_in, void *buffer) {
	lh_iterator_t *iter = iter_in;
	entry_iterator_t *entry_iterator = &iter->entry_iterator;
	while (entry_iterator_next(iter->lh, entry_iterator, false)) {
		bucket_entry_t *entry =
			&entry_iterator->bucket.entries[entry_iterator->entry_ix];
		if (entry->record == 0) {
			retrieve_end_ops = total_io_ops(iter->lh);
			return false;
		}
		if (entry->hash == iter->hash) {
			CHECKED(record_ix, allocator_retrieve(iter->lh->data_alloc,
				entry->record, buffer), "retrieving next hash entry");
			retrieve_end_ops = total_io_ops(iter->lh);
			return true;
		}
	}
	retrieve_end_ops = total_io_ops(iter->lh);
	return false;
}

void
lh_destroy(void *lh_in) {
	if (!lh_in) { return; }
	linear_hash_t *lh = lh_in;
#ifdef DEBUG
	if (lh->validate) {
		validate(lh, true);
	}
#endif
	if (lh->data_alloc) 	{ allocator_destroy(lh->data_alloc); }
	if (lh->bucket_alloc) 	{ allocator_destroy(lh->bucket_alloc); }
	if (lh->overflow_alloc) { allocator_destroy(lh->overflow_alloc); }
	free(lh);
}
