# Linear Hash Table Implementation

A high-performance, dynamic hash table implementation using linear hashing algorithm. Built on top of the linear allocator, this hash table grows incrementally without requiring full table reorganization.

## Features

- **Linear Hashing**: Incremental growth by splitting one bucket at a time
- **No Deletions**: Append-only design optimized for growing datasets
- **Collision Handling**: Multiple entries with the same hash value supported
- **Iterator-Based Retrieval**: Zero-allocation traversal using stack-allocated iterators
- **Memory Efficient**: Uses the linear allocator for predictable memory usage
- **High Performance**: ~6M inserts/sec, ~20M lookups/sec on modern hardware

## Architecture

### Data Structures

The implementation uses three separate allocators:
- **Data Allocator**: Stores actual data records
- **Bucket Allocator**: Stores main bucket structures
- **Overflow Allocator**: Handles bucket overflow via chaining

### Linear Hashing Algorithm

Linear hashing is a dynamic hashing technique that:
1. Starts with an initial number of buckets
2. Splits buckets incrementally as load factor increases
3. Uses two hash functions: h_level and h_(level+1)
4. Maintains a split pointer to track which buckets have been split

When the load factor exceeds a threshold (default 0.75):
- The bucket at the split pointer is split into two buckets
- Entries are redistributed using the next-level hash function
- The split pointer advances, eventually wrapping to start a new level

This approach provides:
- Smooth growth without rehashing the entire table
- Predictable performance (no sudden pauses)
- Good space utilization

## API Reference

### Initialization

```c
linear_hash_t* lh_init(size_t data_size, size_t initial_buckets, size_t max_memory);
```

Creates a linear hash table.

**Parameters:**
- `data_size`: Size of each data record in bytes
- `initial_buckets`: Starting number of buckets (power of 2 recommended)
- `max_memory`: Maximum memory for data storage

**Returns:** Pointer to hash table or NULL on failure

### Operations

```c
int lh_insert(linear_hash_t* lh, uint64_t hash_value, const void* data);
```

Inserts a data record with the given hash value.

**Parameters:**
- `lh`: Hash table instance
- `hash_value`: 63-bit hash (high bit reserved)
- `data`: Buffer containing data record

**Returns:** 0 on success, -1 on error (e.g., out of memory)

**Note:** The caller is responsible for computing hash values. The hash table does not perform hashing.

---

```c
int lh_retrieve_start(linear_hash_t* lh, uint64_t hash_value,
                      lh_iterator_t* iter, void* buffer);
```

Starts retrieving all data records with a given hash value.

**Parameters:**
- `lh`: Hash table instance
- `hash_value`: Hash value to search for
- `iter`: Iterator to initialize (stack-allocated)
- `buffer`: Buffer to receive first record (≥ data_size)

**Returns:** 1 if record found, 0 if no matches, -1 on error

---

```c
int lh_next(linear_hash_t* lh, lh_iterator_t* iter, void* buffer);
```

Gets the next data record in the current retrieval operation.

**Parameters:**
- `lh`: Hash table instance
- `iter`: Iterator from lh_retrieve_start or lh_retrieve_all_start
- `buffer`: Buffer to receive next record (≥ data_size)

**Returns:** 1 if record found, 0 if no more records, -1 on error

---

```c
int lh_retrieve_all_start(linear_hash_t* lh, lh_iterator_t* iter, void* buffer);
```

Starts retrieving all data records in the hash table.

**Parameters:**
- `lh`: Hash table instance
- `iter`: Iterator to initialize (stack-allocated)
- `buffer`: Buffer to receive first record (≥ data_size)

**Returns:** 1 if record found, 0 if table empty, -1 on error

---

```c
void lh_get_stats(linear_hash_t* lh, uint64_t* total_entries,
                  uint64_t* num_buckets, uint64_t* num_splits);
```

Gets statistics about the hash table.

---

```c
void lh_destroy(linear_hash_t* lh);
```

Destroys hash table and frees all resources.

## Usage Example

```c
#include "linear_hash.h"

#define RECORD_SIZE 128

typedef struct {
    uint64_t id;
    char name[32];
    char data[88];
} record_t;

/* FNV-1a hash function */
uint64_t hash_string(const char* str) {
    uint64_t hash = 14695981039346656037ULL;
    while (*str) {
        hash ^= (unsigned char)(*str++);
        hash *= 1099511628211ULL;
    }
    return hash & ~((uint64_t)1 << 63);  /* clear high bit */
}

int main(void) {
    /* Create hash table */
    linear_hash_t* lh = lh_init(RECORD_SIZE, 8, 10 * 1024 * 1024);

    /* Insert records */
    record_t rec = {.id = 1, .name = "Alice"};
    strcpy(rec.data, "Some data");
    lh_insert(lh, hash_string(rec.name), &rec);

    /* Retrieve by name */
    lh_iterator_t iter;
    record_t found;
    if (lh_retrieve_start(lh, hash_string("Alice"), &iter, &found) == 1) {
        printf("Found: %s\n", found.name);
    }

    /* Retrieve all with same name (handles collisions) */
    while (lh_next(lh, &iter, &found) == 1) {
        printf("Also found: %s\n", found.name);
    }

    /* Iterate through all records */
    if (lh_retrieve_all_start(lh, &iter, &found) == 1) {
        do {
            printf("Record: %s\n", found.name);
        } while (lh_next(lh, &iter, &found) == 1);
    }

    /* Clean up */
    lh_destroy(lh);
    return 0;
}
```

## Building

```bash
make test-hash          # Run comprehensive test suite
make run-example-hash   # Run example program
```

## Performance Characteristics

### Time Complexity
- **Insert**: O(1) amortized (occasional O(n) during split)
- **Lookup**: O(1 + α) where α is the load factor
- **Iterate All**: O(n) where n is number of entries

### Space Complexity
- Main buckets: O(b) where b is number of buckets
- Overflow buckets: O(c) where c is number of collisions
- Data storage: O(n * data_size)

### Load Factor
The table maintains a target load factor of 0.75, balancing:
- **Space efficiency**: Higher load factor uses less memory
- **Time efficiency**: Lower load factor reduces collisions

## Design Considerations

### Hash Function Requirements
Users must provide hash values. The hash function should:
- Produce uniform distribution across the 63-bit space
- Use the high bit carefully (reserved for internal use)
- Be deterministic (same input → same hash)

### Collision Handling
Multiple entries with the same hash value are stored in:
1. Main bucket slots (8 per bucket)
2. Overflow buckets (chained when main bucket full)

### Iterator Safety
Iterators are **not** safe against concurrent modifications. Do not insert/modify while iterating.

### Memory Management
All memory is managed through the linear allocators:
- No individual malloc/free per entry
- Predictable memory usage
- Efficient for append-only workloads

## Limitations

- No deletion support (append-only)
- Fixed data record size (determined at initialization)
- Iterators invalidated by concurrent modifications
- Memory bounded by max_memory parameter
- High bit of hash values reserved for internal use

## Testing

The test suite (`test_linear_hash.c`) verifies:
- Basic insert/retrieve operations
- Collision handling (multiple entries with same hash)
- Retrieve-all functionality
- Dynamic growth and splitting
- Large datasets (50,000+ entries)
- Iterator patterns

All tests pass with no memory leaks (verified with valgrind).

## Performance Tips

1. **Choose appropriate initial_buckets**: Start with a power of 2 roughly matching expected entry count / 8
2. **Use a good hash function**: Poor distribution causes excessive collisions
3. **Size max_memory appropriately**: Out-of-memory errors occur when limit is reached
4. **Batch inserts**: Insert multiple entries before retrieval for better cache locality
5. **Reuse iterators**: Stack-allocated iterators can be reused for multiple queries

## Implementation Details

### Bucket Structure
Each bucket contains:
- 8 entry slots (hash + data locator pairs)
- Overflow locator (points to overflow bucket if full)

### Split Algorithm
When splitting bucket N:
1. Collect all entries from bucket N and its overflow chain
2. Create new bucket M
3. Redistribute entries based on next-level hash function
4. Entries go to either bucket N or M
5. Update split pointer

### Hash Calculation
```
level_size = initial_buckets * 2^level
bucket = hash % level_size

if (bucket < split_pointer) {
    level_size *= 2
    bucket = hash % level_size
}
```

This ensures smooth transition as buckets are split incrementally.

## License

This is a reference implementation for educational and development purposes.
