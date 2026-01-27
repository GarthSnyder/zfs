/*
 * Simple, efficient linear allocator with memory or disk backing
 * Portable across Windows, macOS, and UNIX/Linux
 */

#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

typedef int64_t record_ix;

typedef struct allocator {

    bool using_disk;
    size_t record_size;
    uint64_t count;  /* number of records allocated */

    /* Memory allocator fields */
    char* base_addr;
    size_t max_memory;
    size_t page_size;
    size_t committed_bytes;  /* tracks committed memory on Windows */

    /* Disk allocator fields */
    FILE* file;

} allocator_t;

/*
 * Initialize a memory-backed allocator.
 *
 * @param alloc Allocator instance to initialize
 * @param record_size Size of each data record in bytes
 * @param max_memory Maximum memory consumption in bytes
 * @return 0 on success, negative on failure
 */
int
allocator_init_memory(allocator_t *alloc, size_t record_size, size_t max_memory);

/*
 * Initialize a disk-backed allocator.
 *
 * @param alloc Allocator instance to initialize
 * @param record_size Size of each data record in bytes
 * @param file Open file handle for disk storage
 * @return 0 on success, negative on failure
 */
int
allocator_init_disk(allocator_t *alloc, size_t record_size, FILE *file);

/*
 * Initialize a convertible allocator (starts as memory, can convert to disk).
 *
 * @param alloc Allocator instance to initialize
 * @param record_size Size of each data record in bytes
 * @param max_memory Maximum memory consumption in bytes before conversion
 * @param file Open file handle for disk storage (used when converted)
 * @return 0 on success, negative on failure
 */
int
allocator_init_convertible(allocator_t *alloc, size_t record_size,
    size_t max_memory, FILE *file);

/*
 * Convert a memory-backed allocator to disk-backed.
 * The file handle must have been provided during initialization.
 *
 * @param alloc Allocator instance
 * @return 0 on success, negative on failure
 */
int
allocator_convert_to_disk(allocator_t *alloc);

/*
 * Append a new data record.
 *
 * @param alloc Allocator instance
 * @param data Buffer containing record data
 * @return Record index on success, negative on error
 */
record_ix
allocator_append(allocator_t* alloc, const void* data);

/*
 * Skip a record (allocate space but leave it zero-filled).
 *
 * @param alloc Allocator instance
 * @return Record index on success, negative on error
 */
record_ix
allocator_skip(allocator_t* alloc);

/*
 * Store data at a specific record index (can skip records).
 *
 * @param alloc Allocator instance
 * @param data Buffer containing record data
 * @param record Record index to store at
 * @return Record index on success, negative on error
 */
record_ix
allocator_store(allocator_t *alloc, const void *data, record_ix record);

/*
 * Retrieve a data record.
 *
 * @param alloc Allocator instance
 * @param record Record index to retrieve
 * @param buffer Buffer to receive record data (must be >= record_size)
 * @return Record index on success, negative on error
 */
record_ix
allocator_retrieve(allocator_t* alloc, record_ix record, void* buffer);

/*
 * Destroy allocator and free all resources.
 * For memory allocators: frees all allocated memory.
 * For disk allocators: closes and deletes the file.
 *
 * @param alloc Allocator instance (may be NULL)
 */
void
allocator_destroy(allocator_t* alloc);

#endif /* ALLOCATOR_H */
