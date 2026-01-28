/*
 * Simple linear hashing implementation using the linear allocator
 *
 * Features:
 * - Dynamic growth using linear hashing algorithm
 * - No deletions (append-only)
 * - Handles collisions via chaining
 * - Iterator-based retrieval (no memory allocation during retrieval)
 * - Hash values provided by caller (64-bit)
 */

#ifndef LINEAR_HASH_H
#define LINEAR_HASH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "allocator.h"

#define ENTRIES_PER_BUCKET 2

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

/* Forward declaration for iterator */
typedef struct linear_hash linear_hash_t;

/* Internal iterator for bucket entries */
typedef struct {
	allocator_t *alloc;
	record_ix bucket_ix;
	int64_t entry_ix; /* -1 == bucket not yet retrieved */
	bucket_t bucket;
	bool dirty;
    bool iteration_complete;
} entry_iterator_t;

/* Hash table structure */
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

/* Iterator for retrieving records by hash */
typedef struct {
	linear_hash_t *lh;
	uint64_t hash;
	entry_iterator_t entry_iterator;
    bool iteration_complete;
} lh_iterator_t;

/*
 * Initialize a linear hash table.
 *
 * @param lh Linear hash table instance to initialize
 * @param record_size Size of each data record in bytes
 * @param max_memory Maximum memory for storage
 * @return 0 on success, negative on failure
 */
int
lh_init(linear_hash_t *lh, size_t record_size, size_t max_memory);

/*
 * Insert a data record with the given hash value.
 *
 * @param lh Hash table instance
 * @param hash 64-bit hash value
 * @param data Buffer containing data record
 * @return 0 on success, negative on error (e.g., out of memory)
 */
int
lh_insert(linear_hash_t* lh, uint64_t hash, const void* data);

/*
 * Setup retrieval for all data records with a given hash value.
 * Use lh_retrieve_next() to iterate through matching records.
 *
 * @param lh Hash table instance
 * @param hash Hash value to search for
 * @param iter Iterator to initialize
 * @return 0 on success, negative on error
 */
int
lh_retrieve_setup(linear_hash_t* lh, uint64_t hash, lh_iterator_t* iter);

/*
 * Get next data record matching the hash in the iterator.
 *
 * @param iter Iterator from lh_retrieve_setup
 * @param buffer Buffer to receive next record (>= record_size)
 * @return 0 if record found, 1 if no more records, negative on error
 */
int
lh_retrieve_next(lh_iterator_t *iter, void *buffer);

/*
 * Destroy hash table and free all resources.
 *
 * @param lh Hash table instance (may be NULL)
 */
void
lh_destroy(linear_hash_t* lh);

#endif /* LINEAR_HASH_H */
