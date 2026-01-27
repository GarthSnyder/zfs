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

typedef struct linear_hash linear_hash_t;
typedef struct lh_iterator lh_iterator_t;

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
