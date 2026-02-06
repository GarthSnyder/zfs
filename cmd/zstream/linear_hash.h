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


/*
 * Initialize a memory-based or convertible linear hash table.
 *
 * @param record_size Size of each data record in bytes
 * @param max_memory Maximum memory for storage
 * @param cache_dir Directory in which to store convertible files
 * @return malloc'ed linear allocator or aborts
 */
void *
lh_init(size_t record_size, size_t max_memory, const char *cache_dir);

/*
 * Insert a data record with the given hash value.
 *
 * @param lh Hash table instance
 * @param hash 64-bit hash value
 * @param data Buffer containing data record
 */
void
lh_insert(void *lh, uint64_t hash, const void* data);

/*
 * Set up retrieval for all data records with a given hash value.
 * Use lh_retrieve_next() to iterate through matching records.
 *
 * @param lh Hash table instance
 * @param hash Hash value to search for
 * @param iter Iterator to initialize
 * @return iterator pointer, aborts on error
 */
void *
lh_initiate_retrieve(void *lh, uint64_t hash);

/*
 * Get next data record matching the hash in the iterator.
 *
 * @param iter Iterator from lh_retrieve_setup
 * @param buffer Buffer to receive next record (>= record_size)
 * @return true if buffer is valid, false if no more records
 */
bool
lh_retrieve_next(void *iter, void *buffer);

size_t
lh_memory_high_water(void *lh);

/*
 * Destroy hash table and free all resources.
 *
 * @param lh Hash table instance (may be NULL)
 */
void
lh_destroy(void* lh);

#endif /* LINEAR_HASH_H */
