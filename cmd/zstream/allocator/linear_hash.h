/*
 * Simple linear hashing implementation using the linear allocator
 *
 * Features:
 * - Dynamic growth using linear hashing algorithm
 * - No deletions (append-only)
 * - Handles collisions via chaining
 * - Iterator-based retrieval (no memory allocation during retrieval)
 * - Hash values provided by caller (63-bit)
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
 * @param data_size Size of each data record in bytes
 * @param initial_buckets Initial number of buckets (power of 2 recommended)
 * @param max_memory Maximum memory for data storage
 * @return Pointer to hash table, or NULL on failure
 */
linear_hash_t* lh_init(size_t data_size, size_t initial_buckets, size_t max_memory);

/*
 * Insert a data record with the given hash value.
 *
 * @param lh Hash table instance
 * @param hash_value 63-bit hash value (high bit reserved for internal use)
 * @param data Buffer containing data record
 * @return 0 on success, -1 on error (e.g., out of memory)
 */
int lh_insert(linear_hash_t* lh, uint64_t hash_value, const void* data);

/*
 * Start retrieving all data records with a given hash value.
 * Use lh_next() to iterate through matching records.
 *
 * @param lh Hash table instance
 * @param hash_value Hash value to search for
 * @param iter Iterator to initialize
 * @param buffer Buffer to receive first matching record (>= data_size)
 * @return 1 if record found, 0 if no matches, -1 on error
 */
int lh_retrieve_start(linear_hash_t* lh, uint64_t hash_value,
                      lh_iterator_t* iter, void* buffer);

/*
 * Get next data record in the current retrieval operation.
 *
 * @param lh Hash table instance
 * @param iter Iterator from lh_retrieve_start or lh_retrieve_all_start
 * @param buffer Buffer to receive next record (>= data_size)
 * @return 1 if record found, 0 if no more records, -1 on error
 */
int lh_next(linear_hash_t* lh, lh_iterator_t* iter, void* buffer);

/*
 * Start retrieving all data records in the hash table.
 * Use lh_next() to iterate through all records.
 *
 * @param lh Hash table instance
 * @param iter Iterator to initialize
 * @param buffer Buffer to receive first record (>= data_size)
 * @return 1 if record found, 0 if table empty, -1 on error
 */
int lh_retrieve_all_start(linear_hash_t* lh, lh_iterator_t* iter, void* buffer);

/*
 * Destroy hash table and free all resources.
 *
 * @param lh Hash table instance (may be NULL)
 */
void lh_destroy(linear_hash_t* lh);

/*
 * Get statistics about the hash table.
 *
 * @param lh Hash table instance
 * @param total_entries Output: total number of entries
 * @param num_buckets Output: current number of buckets
 * @param num_splits Output: number of splits performed
 */
void lh_get_stats(linear_hash_t* lh, uint64_t* total_entries,
                  uint64_t* num_buckets, uint64_t* num_splits);

/* Opaque iterator structure - size known to users for stack allocation */
struct lh_iterator {
    uint64_t hash_value;      /* hash we're searching for (or 0 for all) */
    uint64_t bucket_index;    /* current bucket */
    uint64_t entry_index;     /* current entry in bucket */
    uint64_t overflow_loc;    /* current overflow bucket locator */
    int mode;                 /* 0 = hash search, 1 = retrieve all */
};

#endif /* LINEAR_HASH_H */
