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
struct allocator;
typedef struct allocator *allocator;

typedef struct {
    uint64_t    num_ops;        /* Number of stores and retrieves */
    uint64_t    mem_used;       /* Current memory use */
    uint64_t    num_records;
} allocator_stats;

/*
 * Initialize an allocator. Record size is required. If a file handle is
 * provided and max_memory is 0, the allocator will use disk backing.
 * If both a memory limit and a file handle are supplied, the allocator
 * is a convertible allocator that starts by using memory but converts
 * to disk storage if memory use is exceeded. A conversion can also be
 * triggered externally by allocator_convert_to_disk.
 *
 * If no file handle is supplied, the allocator will be memory-only. 
 * max_memory must be specified and nonzero.
 */
allocator
allocator_init(uint64_t record_size, uint64_t max_memory, FILE *file);

/*
 * Convert a memory-backed allocator to disk-backed.
 * The file handle must have been provided during initialization.
 * A negative return value indicates an error.
 */
int
allocator_convert_to_disk(allocator alloc);

/*
 * Append a new data record. A negative return value indicates
 * an error.
 */
record_ix
allocator_append(allocator alloc, const void* data);

/*
 * Skip a record (allocate space but leave it zero-filled). A 
 * negative return value indicates an error.
 */
record_ix
allocator_skip(allocator alloc);

/*
 * Store data at a specific record index (can skip records). A
 * negative return value indicates an error.
 */
record_ix
allocator_store(allocator alloc, record_ix record, const void *data);

/*
 * Retrieve a data record. A negative value indicates an error.
 */
record_ix
allocator_retrieve(allocator alloc, record_ix record, void *buffer);

void
allocator_get_stats(allocator alloc, allocator_stats *stats);

/*
 * Destroy allocator and free all resources.
 * For memory allocators: frees all allocated memory.
 * For disk allocators: closes and deletes the file.
 *
 * @param alloc Allocator instance (may be NULL)
 */
void
allocator_destroy(allocator alloc);

#endif /* ALLOCATOR_H */
