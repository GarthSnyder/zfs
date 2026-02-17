# Simple Linear Allocator

A portable C implementation of a simple linear allocator that supports both memory and disk backing. The allocator is designed for append-only workloads with fixed-size data records.

## Features

- **Portable**: Works on Windows, macOS, Linux, and other UNIX variants
- **Dual backend**: Supports both memory and disk-backed storage
- **Efficient memory usage**: Uses OS features to reserve address space without committing physical memory until needed
- **Fixed record size**: Optimized for fixed-size data records
- **Append-only**: Linear allocation with no deletion support
- **Unified API**: Same interface regardless of backing store

## Platform-Specific Optimizations

### Windows
- Uses `VirtualAlloc` with `MEM_RESERVE` to reserve address space
- Commits pages on-demand with `MEM_COMMIT` as records are added
- Process appears to use only committed memory, not reserved address space

### Linux/macOS/UNIX
- Uses `mmap` with `MAP_ANONYMOUS` and `MAP_PRIVATE`
- On Linux, uses `MAP_NORESERVE` to avoid swap space reservation
- Leverages demand paging - physical pages allocated on first write
- Process RSS grows only as memory is actually used

## API Reference

### Initialization

```c
allocator_t* allocator_init_memory(size_t record_size, size_t max_memory);
```
Creates a memory-backed allocator. Returns `NULL` on failure.

```c
allocator_t* allocator_init_disk(size_t record_size, const char* filepath);
```
Creates a disk-backed allocator. Returns `NULL` if file already exists or on failure.

### Operations

```c
locator_t allocator_append(allocator_t* alloc, const void* data);
```
Appends a record. Returns opaque locator (non-zero) on success, 0 on error.

```c
int allocator_retrieve(allocator_t* alloc, locator_t loc, void* buffer);
```
Retrieves a record. Returns 0 on success, -1 on error.

```c
int allocator_overwrite(allocator_t* alloc, locator_t loc, const void* data);
```
Overwrites a record. Returns 0 on success, -1 on error.

```c
void allocator_destroy(allocator_t* alloc);
```
Destroys allocator, freeing memory or deleting disk file.

## Usage Example

```c
#include "allocator.h"

typedef struct {
    uint32_t id;
    char data[60];
} my_record_t;

int main(void) {
    // Create memory-backed allocator
    allocator_t* alloc = allocator_init_memory(
        sizeof(my_record_t),
        1024 * 1024  // 1 MB max
    );

    // Append records
    my_record_t record = {.id = 1, .data = "Hello"};
    locator_t loc = allocator_append(alloc, &record);

    // Retrieve record
    my_record_t retrieved;
    allocator_retrieve(alloc, loc, &retrieved);

    // Overwrite record
    record.id = 2;
    allocator_overwrite(alloc, loc, &record);

    // Clean up
    allocator_destroy(alloc);

    return 0;
}
```

## Building

### Linux/macOS
```bash
make
make test
```

### Windows (MinGW)
```bash
mingw32-make
mingw32-make test
```

### Manual compilation
```bash
gcc -Wall -O2 -c allocator.c
gcc -Wall -O2 -c test_allocator.c
gcc -o test_allocator allocator.o test_allocator.o
```

## Design Notes

### Locator Format
Locators are 64-bit values representing the byte offset from the start of the allocation space plus 1. This allows 0 to be used as an error indicator while maintaining a direct correspondence between locators and memory addresses/file offsets.

### Memory Efficiency
The memory allocator reserves a large contiguous address space but only commits physical memory as needed:
- On Windows, pages are explicitly committed using `VirtualAlloc` with `MEM_COMMIT`
- On POSIX systems, the OS automatically commits pages on first write (demand paging)

This approach allows the allocator to support large maximum sizes without consuming physical memory or affecting other processes until the memory is actually used.

### Page Alignment
Memory commits are aligned to system page boundaries. Since record size is assumed to be small relative to page size, adding a single record never requires more than one page to be committed.

### Error Handling
- `allocator_append` returns 0 when memory/disk space is exhausted
- `allocator_retrieve` and `allocator_overwrite` return -1 for invalid locators
- Invalid locators include: zero, out of range, or not aligned to record boundaries

## Limitations

- Fixed record size (determined at initialization)
- No deletion support (append-only)
- File size limited by filesystem constraints
- Memory size limited by address space (2^64 bytes theoretical maximum)
- Locator validation is basic (checks alignment and range only)

## Testing

Run the test suite:
```bash
./test_allocator
```

The test suite verifies:
- Memory allocator operations
- Disk allocator operations
- Capacity limits and error handling
- Large address space reservations
- Cross-platform compatibility

## License

This is a reference implementation for educational and development purposes.
