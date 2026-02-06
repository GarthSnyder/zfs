/*
 * Linear hashing implementation
 */

#include "linear_hash.h"
#include "zstream_shared.h"
#include <stdlib.h>
#include <string.h>

#define MAX_OCCUPANCY 0.75
#define INITIAL_HASH_SUFFIX_LENGTH 6
#define ENTRIES_PER_BUCKET 2
#define INSERTIONS_BETWEEN_MEM_CHECKS 1024
#define MAX_ITERATORS_OUTSTANDING 4

/* Entry in a bucket: hash value + locator to data */
typedef struct {
	uint64_t  hash;
	record_ix record;  /* index of actual data */
} bucket_entry_t;

/* Bucket structure: fixed array of entries + overflow pointer */
typedef struct {
	bucket_entry_t	entries[ENTRIES_PER_BUCKET];
	record_ix		overflow;  /* 0 if no overflow */
} bucket_t;

/* Internal iterator for bucket entries */
typedef struct {
	allocator_t	*alloc;
	record_ix 	bucket_ix;
	int64_t 	entry_ix; 			/* -1 == bucket not yet retrieved */
	bucket_t 	bucket;
	bool 		dirty;				/* needs writeback */
} entry_iterator_t;

struct linear_hash;

/* Iterator for retrieving records by hash */
typedef struct lh_iterator {
	struct linear_hash	*lh;
	uint64_t 			hash;
	entry_iterator_t	entry_iterator;
} lh_iterator_t;

/* Hash table structure */
typedef struct linear_hash 
{
	size_t 		record_size;
	uint8_t 	hash_suffix_length;	/* current hashing granularity below split */
	record_ix 	split_pointer;   	/* next bucket to split */
	uint64_t	max_memory;			/* Can decrease if allocators convert */

	uint64_t 	num_entries;      	/* total entries in table */
	uint64_t 	num_splits;       	/* statistics */
	uint64_t	mem_highwater;		/* Most memory ever used */
	int			next_memory_check;  /* Number of splits before check */

	lh_iterator_t	iterators[MAX_ITERATORS_OUTSTANDING];
	int				next_iterator;
#ifdef DEBUG
	bool		validate;			/* Validate after every transaction (slow) */
#endif
	allocator_t data_alloc;    		/* data records */
	allocator_t bucket_alloc;  		/* main buckets */
	allocator_t overflow_alloc;		/* overflow buckets */
} linear_hash_t;

#define CHECKED(type, expr, doing_what) \
	type ret = expr; \
	if (ret < 0) { \
		(void) fprintf(stderr, "Error while %s.\n", doing_what); \
		exit(1); \
	} 

#define ITER_BUCKET(lh, bucket) \
	{&lh->bucket_alloc, bucket, -1, {{{0}}}, false}

#define EMPTY_BUCKET {{{0}}, 0}

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

/* Send an entry iterator struct back to its allocator */
static void
save_entry_iterator(entry_iterator_t *iter) {
	CHECKED(record_ix, allocator_store(iter->alloc, iter->bucket_ix,
		&iter->bucket), "saving from entry iterator"); 

}

/* Read the bucket corresponding to an entry iterator */
static void
read_entry_iterator(entry_iterator_t *iter) {
	CHECKED(record_ix, allocator_retrieve(iter->alloc, iter->bucket_ix,
		&iter->bucket), "reading from entry iterator"); 
}

/* Updates entry_iterator_t struct, false when there are no more entries. 
   This type of iteration does not care whether bucket entries are full
   or not. */
static bool
entry_iterator_get_next(linear_hash_t *lh, entry_iterator_t *iter) {
	while (true) {
		if (iter->entry_ix < 0) {
			read_entry_iterator(iter);
			iter->entry_ix = 0;
			iter->dirty = false;
			return true;
		} else if (iter->entry_ix == ENTRIES_PER_BUCKET - 1) {
			if (iter->dirty) {
				save_entry_iterator(iter);
				iter->dirty = false;
			}
			if (!iter->bucket.overflow) {
				return false;
			}
			iter->alloc = &lh->overflow_alloc;
			iter->entry_ix = -1;
			iter->bucket_ix = iter->bucket.overflow;
		} else {
			iter->entry_ix += 1;
			return true;
		}
	}
}

/* Stores in first available slot in bucket chain, extending if needed */
static void
store_in_bucket_chain(linear_hash_t *lh, record_ix chain_head,
	bucket_entry_t new_entry)
{
	bucket_entry_t *entry;
	entry_iterator_t iter = ITER_BUCKET(lh, chain_head);
	while (entry_iterator_get_next(lh, &iter)) {
		entry = &iter.bucket.entries[iter.entry_ix];
		if (entry->record == 0) {
			*entry = new_entry;
			save_entry_iterator(&iter);
			return;
		}
	}
	/*
	 * We're off the end of the iterator, but the iterator still has
	 * the allocator, contents, and index of the last bucket.
	 */
	bucket_t new_overflow_bucket = EMPTY_BUCKET;
	new_overflow_bucket.entries[0] = new_entry;
	iter.bucket.overflow = allocator_append(&lh->overflow_alloc,
		&new_overflow_bucket);
	assert(iter.bucket.overflow >= 0);
	save_entry_iterator(&iter);
}

#ifdef DEBUG
#define MAX_CHAIN 1024
static void
validate(linear_hash_t *lh, bool print_stats) {
	uint64_t total_entries = 0;
	uint64_t chain_lengths[MAX_CHAIN] = {0};
	uint64_t chain_occupancies[MAX_CHAIN] = {0};
	uint64_t max_occupancy[MAX_CHAIN] = {0};
	uint64_t empties[MAX_CHAIN] = {0};
	bool print_hash = false;
	if (print_hash) {
		fprintf(stderr, "Linear hash dump, split pointer at %lu:\n",
			(uint64_t)lh->split_pointer);
	}
	for (uint64_t i = 0; i < lh->bucket_alloc.count; i++) {
		entry_iterator_t iter = ITER_BUCKET(lh, i);
		bool saw_empty_entry = false;
		uint64_t full_entries = 0;
		uint64_t empty_entries = 0;
		while (entry_iterator_get_next(lh, &iter)) {
			bucket_entry_t *entry = &iter.bucket.entries[iter.entry_ix];
			if (entry->record) {
				if (saw_empty_entry) {
					fprintf(stderr, "validate: bucket %lu has "
						"uncompacted entries.\nEntry %lu is the "
						"first after an empty entry.\n", i, 
						iter.entry_ix);
				}
				full_entries++;
			} else {
				empty_entries++;
				saw_empty_entry = true;
			}
		}
		uint64_t chain_length = (full_entries + empty_entries) / 
			ENTRIES_PER_BUCKET;
		chain_lengths[chain_length]++;
		chain_occupancies[chain_length] += full_entries;
		empties[chain_length] += full_entries ? 0 : 1;
		max_occupancy[chain_length] = 
			(full_entries > max_occupancy[chain_length]) ?
			full_entries : max_occupancy[chain_length];
		total_entries += full_entries;
		if (print_hash) {
			fprintf(stderr, "    Bucket %lu: %lu blocks, %lu full, %lu empty\n",
				i, (full_entries + empty_entries) / ENTRIES_PER_BUCKET,
				full_entries, empty_entries);
		}
	}
	if (total_entries != lh->num_entries) {
		fprintf(stderr, "validate: linear hash is supposed to have %lu "
			"entries, but actually has %lu.\n", lh->num_entries,
			total_entries);
	}
	if (print_stats) {
		fprintf(stderr, "%lu entries in %lu bucket chains:\n", total_entries,
			lh->bucket_alloc.count);
		uint64_t total_bucket_entries = 0;
		for (uint64_t i = 0; i < MAX_CHAIN; i++) {
			if (chain_lengths[i]) {
				uint64_t entries_per_chain = i * ENTRIES_PER_BUCKET;
				total_bucket_entries += entries_per_chain * chain_lengths[i];
				double avg_full = (double)chain_occupancies[i] / 
					(chain_lengths[i] - empties[i]);
				double avg_occupancies = (double)avg_full / entries_per_chain;
				double max_occ = (double)max_occupancy[i] / entries_per_chain;
				fprintf(stderr, "    %lu bucket chains of length %lu, ",
					chain_lengths[i], i);
				if (empties[i] == chain_lengths[i]) {
					fprintf(stderr, "all empty\n");
				} else {
					if (!empties[i]) {
						fprintf(stderr, "none empty, ");
					} else {
						fprintf(stderr, "%lu empty, ", empties[i]);
					}
					fprintf(stderr, "nonempty occupancy avg %.0f%%, max %.0f%%\n", 
						avg_occupancies * 100, max_occ * 100);
				}
			}
		}
		fprintf(stderr, "Overall %lu of %lu bucket slots occupied, %.2f%%\n",
			lh->num_entries, total_bucket_entries, 
			(double)100 * lh->num_entries / total_bucket_entries);
	}
}
#endif

/* Split a bucket, rehashing all entries according to the current
suffix length. Since only one bit is added, existing entries either
stay where they are or go to one alternate bucket. We'll do this partition
in two passes for clarity and reliability: one to eject relocated entries
and one to consolidate entries now that some may have been removed. 

It is an invariant that bucket entries must be filled linearly, even
if there are unused entries. */
static void
split_bucket(linear_hash_t* lh) {

#ifdef DEBUG
	if (lh->validate) {
		validate(lh, false);
	}
#endif

	record_ix bucket_being_split = (record_ix)lh->split_pointer;
	entry_iterator_t iter = ITER_BUCKET(lh, bucket_being_split);
	bucket_entry_t empty_entry = {0, 0};

	/* Increment split pointer now so bucket_for_hash uses new value */
	lh->split_pointer++;

	/* Partition */
	while(entry_iterator_get_next(lh, &iter)) {
		bucket_entry_t entry = iter.bucket.entries[iter.entry_ix];
		/* 
		 * Entries in a bucket are required to be contiguous at the
		 * start of the bucket chain, so any empty spot signals
		 * the end. But we have to read to the end of the bucket
		 * if it needs saving.
		 */
		if (entry.record != 0) {
			record_ix new_bucket_ix = bucket_for_hash(lh, entry.hash);
			if (new_bucket_ix != bucket_being_split) {
				iter.bucket.entries[iter.entry_ix] = empty_entry;
				iter.dirty = true;
				store_in_bucket_chain(lh, new_bucket_ix, entry);
			}
		} else if (!iter.dirty) {
			break;
		}
	}

	/* Consolidate the bucket that was split. It doesn't matter that there
	are two iterators
	running simultaneously because the head will never write and will
	never be behind the tail. Also, initial reads on a valid bucket are 
	guaranteed to succeed. */

	entry_iterator_t head = ITER_BUCKET(lh, bucket_being_split);
	entry_iterator_t tail = head;
	while (entry_iterator_get_next(lh, &head)) {
		bucket_entry_t *head_entry = &head.bucket.entries[head.entry_ix];
		if (head_entry->record != 0) {
			assert(entry_iterator_get_next(lh, &tail) == true);
			bucket_entry_t *tail_entry = &tail.bucket.entries[tail.entry_ix];
			if (tail_entry->hash != head_entry->hash ||
				tail_entry->record != head_entry->record)
			{
				*tail_entry = *head_entry;
				tail.dirty = true;
			}
		}
	}

	/* Zero out the remaining entries */
	while (entry_iterator_get_next(lh, &tail)) {
		tail.bucket.entries[tail.entry_ix] = empty_entry;
		tail.dirty = true;
	}

	/* Check if we've completed this level. Split pointer was
	already incremented. */
	uint64_t buckets_this_cycle = (1ULL << (lh->hash_suffix_length - 1));
	if (lh->split_pointer >= (record_ix)buckets_this_cycle) {
		lh->hash_suffix_length++;
		lh->split_pointer = 0;
	}

	lh->num_splits++;

#ifdef DEBUG
	if (lh->validate) {
		validate(lh, false);
	}
#endif
}

static void
check_split(linear_hash_t *lh) {
	uint64_t num_buckets = lh->bucket_alloc.count + 
		lh->overflow_alloc.count;
	double occupancy = (double)lh->num_entries /
		(num_buckets * ENTRIES_PER_BUCKET);
	if (occupancy > MAX_OCCUPANCY) {
		split_bucket(lh);
	}
}

static void
check_memory(linear_hash_t *lh) {
	size_t bucket_memory = allocator_memory_use(&lh->bucket_alloc);
	size_t overflow_memory = allocator_memory_use(&lh->overflow_alloc);
	size_t data_memory = allocator_memory_use(&lh->data_alloc);
	size_t total_memory = bucket_memory + overflow_memory + data_memory;
	lh->mem_highwater = (total_memory > lh->mem_highwater) ? total_memory :
		lh->mem_highwater;
	if (total_memory > lh->max_memory) {
		allocator_t *allocator_to_convert;
		if (data_memory) {
			allocator_to_convert = &lh->data_alloc;
		} else if (overflow_memory) {
			allocator_to_convert = &lh->overflow_alloc;
		} else {
			allocator_to_convert = &lh->bucket_alloc;
		}
		CHECKED(int, allocator_convert_to_disk(allocator_to_convert),
			"converting allocator to disk storage");
	}
}

size_t
lh_memory_high_water(void *lh_in) {
	if (!lh_in) { return 0; }
	linear_hash_t *lh = lh_in;
	return lh->mem_highwater;
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
	return fp;
}

void *
lh_init(size_t record_size, size_t max_memory, const char *cache_dir)
{
	if (record_size == 0) {
		return (NULL);
	}

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
			return NULL;
		}
		FILE *fp = create_temp_file(cache_dir, path);
		int ret1 = allocator_init_convertible(&lh->data_alloc, record_size,
			max_memory, fp);
		fp = create_temp_file(cache_dir, path);
		int ret2 = allocator_init_convertible(&lh->bucket_alloc,
			sizeof(bucket_t), max_memory, fp);
		fp = create_temp_file(cache_dir, path);
		int ret3 = allocator_init_convertible(&lh->overflow_alloc,
			sizeof(bucket_t), max_memory, fp);
		if (ret1 < 0 || ret2 < 0 || ret3 < 0) {
			(void) fprintf(stderr, "Unable to initialize convertible "
				"allocators");
			return NULL;
		}
	} else {
		if (allocator_init_memory(&lh->data_alloc, record_size, max_memory)) {
			return NULL;
		}
		if (allocator_init_memory(&lh->bucket_alloc, sizeof (bucket_t), 
			max_memory))
		{
			allocator_destroy(&lh->data_alloc);
			return NULL;
		}
		if (allocator_init_memory(&lh->overflow_alloc, sizeof (bucket_t),
			max_memory))
		{
			allocator_destroy(&lh->data_alloc);
			allocator_destroy(&lh->bucket_alloc);
			return NULL;
		}
	}
	/*
	 * Skip first overflow and data buckets to allow 0
	 * to indicate empty or end-of-chain
	 */
	if ((allocator_skip(&lh->data_alloc) < 0) ||
		(allocator_skip(&lh->overflow_alloc) < 0))
	{
		return NULL;
	}
	return lh;
}

void
lh_insert(void* lh_in, uint64_t hash, const void* data) {
	linear_hash_t *lh = lh_in;
	if (!lh || !data) {
		return;
	}
	record_ix record = allocator_append(&lh->data_alloc, data);
	if (record < 0) {
		(void) fprintf(stderr, "Error inserting new hash entry.\n");
		exit(1);
	}
	bucket_entry_t entry = {hash, record};
	record_ix bucket_ix = bucket_for_hash(lh, hash);
	store_in_bucket_chain(lh, bucket_ix, entry);
	lh->num_entries++;
	check_split(lh);
	lh->next_memory_check--;
	if (lh->next_memory_check <= 0) {
		lh->next_memory_check = INSERTIONS_BETWEEN_MEM_CHECKS;
		check_memory(lh);
	}
}

void *
lh_initiate_retrieve(void *lh_in, uint64_t hash) {
	linear_hash_t *lh = lh_in;
	if (!lh) {
		return NULL;
	}
	lh_iterator_t *iter = &lh->iterators[lh->next_iterator];
	lh->next_iterator = (lh->next_iterator + 1) % MAX_ITERATORS_OUTSTANDING;
	memset(iter, 0, sizeof(*iter));
	iter->lh = lh;
	iter->hash = hash;
	iter->entry_iterator.alloc = &lh->bucket_alloc;
	iter->entry_iterator.bucket_ix = bucket_for_hash(lh, hash);
	iter->entry_iterator.entry_ix = -1;
	return iter;
}

bool
lh_retrieve_next(void *iter_in, void *buffer) {
	lh_iterator_t *iter = iter_in;
	entry_iterator_t *entry_iterator = &iter->entry_iterator;
	while (entry_iterator_get_next(iter->lh, entry_iterator)) {
		bucket_entry_t *entry =
			&entry_iterator->bucket.entries[entry_iterator->entry_ix];
		if (entry->record == 0) {
			return false;
		}
		if (entry->hash == iter->hash) {
			CHECKED(record_ix, allocator_retrieve(&iter->lh->data_alloc,
				entry->record, buffer), "retrieving next hash entry");
			return true;
		}
	}
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
	allocator_destroy(&lh->data_alloc);
	allocator_destroy(&lh->bucket_alloc);
	allocator_destroy(&lh->overflow_alloc);
	free(lh);
}
