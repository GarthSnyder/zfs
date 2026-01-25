/*
 * Simple, efficient linear allocator with memory or disk backing
 * Portable across Windows, macOS, and UNIX/Linux
 */

#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>

typedef struct allocator allocator_t;
typedef int64_t record_ix;

/*
 * Initialize a memory-backed allocator.
 *
 * @param record_size Size of each data record in bytes
 * @param max_memory Maximum memory consumption in bytes
 * @return Pointer to allocator, or NULL on failure
 */
int 
allocator_init_memory(allocator_t *alloc, size_t record_size, size_t max_memory);

/*
 * Initialize a disk-backed allocator.
 *
 * @param record_size Size of each data record in bytes
 * @param filepath Path to file (must not exist)
 * @return Pointer to allocator, or NULL on failure
 */
int
allocator_init_disk(allocator_t *alloc, size_t record_size, const char* filepath);

/*
 * Append a new data record.
 *
 * @param alloc Allocator instance
 * @param data Buffer containing record data
 * @return Opaque locator for the record, or 0 on error
 */
record_ix
allocator_append(allocator_t* alloc, const void* data);

/*
 * Retrieve a data record.
 *
 * @param alloc Allocator instance
 * @param loc Opaque locator returned by allocator_append
 * @param buffer Buffer to receive record data (must be >= record_size)
 * @return 0 on success, -1 on error
 */
int 
allocator_retrieve(allocator_t* alloc, record_ix record, void* buffer);

/*
 * Overwrite a data record.
 *
 * @param alloc Allocator instance
 * @param loc Opaque locator returned by allocator_append
 * @param data Buffer containing new record data
 * @return 0 on success, -1 on error
 */
int
allocator_overwrite(allocator_t* alloc, record_ix record, const void* data);

/*
 * Destroy allocator and free all resources.
 * For memory allocators: frees all allocated memory.
 * For disk allocators: closes and deletes the file.
 *
 * @param alloc Allocator instance (may be NULL)
 */
void allocator_destroy(allocator_t* alloc);

#endif /* ALLOCATOR_H */
