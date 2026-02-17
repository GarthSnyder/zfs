/*
 * Example usage of the linear allocator
 * Demonstrates both memory and disk-backed allocators
 */

#include "allocator.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Example: Simple key-value store with fixed-size records */
#define KEY_SIZE 32
#define VALUE_SIZE 224
#define RECORD_SIZE 256  /* KEY_SIZE + VALUE_SIZE */

typedef struct {
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
} kv_record_t;

static void example_memory_allocator(void) {
    printf("=== Memory-Backed Allocator Example ===\n\n");

    /* Create allocator with 10 MB max memory */
    allocator_t* alloc = allocator_init_memory(RECORD_SIZE, 10 * 1024 * 1024);
    if (!alloc) {
        fprintf(stderr, "Failed to initialize allocator\n");
        return;
    }

    printf("Allocator initialized with 10 MB max memory\n");
    printf("Record size: %d bytes\n", RECORD_SIZE);
    printf("Max records: %lu\n\n", (10UL * 1024 * 1024) / RECORD_SIZE);

    /* Store some key-value pairs */
    locator_t user1_loc, user2_loc, user3_loc;

    kv_record_t record;

    strncpy(record.key, "user:1001", KEY_SIZE);
    strncpy(record.value, "Alice Johnson", VALUE_SIZE);
    user1_loc = allocator_append(alloc, &record);
    printf("Stored: %s -> %s (locator: %lu)\n", record.key, record.value, user1_loc);

    strncpy(record.key, "user:1002", KEY_SIZE);
    strncpy(record.value, "Bob Smith", VALUE_SIZE);
    user2_loc = allocator_append(alloc, &record);
    printf("Stored: %s -> %s (locator: %lu)\n", record.key, record.value, user2_loc);

    strncpy(record.key, "user:1003", KEY_SIZE);
    strncpy(record.value, "Charlie Davis", VALUE_SIZE);
    user3_loc = allocator_append(alloc, &record);
    printf("Stored: %s -> %s (locator: %lu)\n\n", record.key, record.value, user3_loc);

    /* Retrieve records */
    kv_record_t retrieved;

    printf("Retrieving records:\n");
    if (allocator_retrieve(alloc, user2_loc, &retrieved) == 0) {
        printf("  %s -> %s\n", retrieved.key, retrieved.value);
    }

    if (allocator_retrieve(alloc, user1_loc, &retrieved) == 0) {
        printf("  %s -> %s\n", retrieved.key, retrieved.value);
    }

    /* Update a record */
    printf("\nUpdating user:1002...\n");
    strncpy(record.key, "user:1002", KEY_SIZE);
    strncpy(record.value, "Robert Smith", VALUE_SIZE);
    if (allocator_overwrite(alloc, user2_loc, &record) == 0) {
        printf("Updated successfully\n");
    }

    /* Verify update */
    if (allocator_retrieve(alloc, user2_loc, &retrieved) == 0) {
        printf("  %s -> %s\n\n", retrieved.key, retrieved.value);
    }

    allocator_destroy(alloc);
    printf("Allocator destroyed\n\n");
}

static void example_disk_allocator(void) {
    printf("=== Disk-Backed Allocator Example ===\n\n");

    const char* db_file = "example_database.dat";

    /* Create disk allocator */
    allocator_t* alloc = allocator_init_disk(RECORD_SIZE, db_file);
    if (!alloc) {
        fprintf(stderr, "Failed to initialize disk allocator\n");
        return;
    }

    printf("Disk allocator initialized: %s\n", db_file);
    printf("Record size: %d bytes\n\n", RECORD_SIZE);

    /* Generate and store multiple records */
    printf("Storing 1000 records to disk...\n");
    clock_t start = clock();

    locator_t locators[1000];
    kv_record_t record;

    for (int i = 0; i < 1000; i++) {
        snprintf(record.key, KEY_SIZE, "record:%04d", i);
        snprintf(record.value, VALUE_SIZE,
                 "This is record number %d with some data", i);
        locators[i] = allocator_append(alloc, &record);
        if (locators[i] == 0) {
            fprintf(stderr, "Failed to append record %d\n", i);
            break;
        }
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    printf("Stored 1000 records in %.3f seconds\n\n", elapsed);

    /* Retrieve some random records */
    printf("Retrieving random records:\n");
    int test_indices[] = {0, 100, 500, 999};

    for (int i = 0; i < 4; i++) {
        int idx = test_indices[i];
        kv_record_t retrieved;
        if (allocator_retrieve(alloc, locators[idx], &retrieved) == 0) {
            printf("  [%d] %s -> %s\n", idx, retrieved.key, retrieved.value);
        }
    }

    printf("\n");

    /* Clean up - this deletes the file */
    allocator_destroy(alloc);
    printf("Allocator destroyed and file deleted\n\n");
}

static void example_benchmark(void) {
    printf("=== Performance Comparison ===\n\n");

    const int num_records = 10000;
    kv_record_t record;
    clock_t start, end;
    double elapsed;

    /* Benchmark memory allocator */
    allocator_t* mem_alloc = allocator_init_memory(RECORD_SIZE, 100 * 1024 * 1024);
    if (mem_alloc) {
        start = clock();
        for (int i = 0; i < num_records; i++) {
            snprintf(record.key, KEY_SIZE, "key:%d", i);
            snprintf(record.value, VALUE_SIZE, "value:%d", i);
            allocator_append(mem_alloc, &record);
        }
        end = clock();
        elapsed = (double)(end - start) / CLOCKS_PER_SEC;
        printf("Memory allocator: %d records in %.3f seconds (%.0f records/sec)\n",
               num_records, elapsed, num_records / elapsed);
        allocator_destroy(mem_alloc);
    }

    /* Benchmark disk allocator */
    allocator_t* disk_alloc = allocator_init_disk(RECORD_SIZE, "benchmark.dat");
    if (disk_alloc) {
        start = clock();
        for (int i = 0; i < num_records; i++) {
            snprintf(record.key, KEY_SIZE, "key:%d", i);
            snprintf(record.value, VALUE_SIZE, "value:%d", i);
            allocator_append(disk_alloc, &record);
        }
        end = clock();
        elapsed = (double)(end - start) / CLOCKS_PER_SEC;
        printf("Disk allocator:   %d records in %.3f seconds (%.0f records/sec)\n",
               num_records, elapsed, num_records / elapsed);
        allocator_destroy(disk_alloc);
    }

    printf("\n");
}

int main(void) {
    example_memory_allocator();
    example_disk_allocator();
    example_benchmark();

    printf("Examples complete!\n");
    return 0;
}
