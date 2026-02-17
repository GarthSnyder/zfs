/*
 * Test program for the linear allocator
 */

#include "allocator.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define TEST_RECORD_SIZE 768
#define TEST_MAX_MEMORY (1024 * 1024 * 10)  /* 10 MB */
#define TEST_NUM_RECORDS 10000

typedef struct {
    uint32_t id;
    char data[TEST_RECORD_SIZE];
} test_record_t;

static void test_memory_allocator(void) {
    printf("Testing memory allocator...\n");

    allocator_t alloc;
    int result = allocator_init_memory(&alloc, sizeof(test_record_t), TEST_MAX_MEMORY);
    assert(result == 0);
    printf("  Memory allocator initialized\n");

    /* Test appending records */
    record_ix locators[TEST_NUM_RECORDS];
    for (int i = 0; i < TEST_NUM_RECORDS; i++) {
        test_record_t record;
        record.id = i;
        snprintf(record.data, sizeof(record.data), "Test record %d", i);

        locators[i] = allocator_append(&alloc, &record);
        assert(locators[i] >= 0);
    }
    printf("  Appended %d records\n", TEST_NUM_RECORDS);

    /* Test retrieving records */
    for (int i = 0; i < TEST_NUM_RECORDS; i++) {
        test_record_t record;
        record_ix result = allocator_retrieve(&alloc, locators[i], &record);
        assert(result >= 0);
        assert(record.id == (uint32_t)i);

        char expected[60];
        snprintf(expected, sizeof(expected), "Test record %d", i);
        assert(strcmp(record.data, expected) == 0);
    }
    printf("  Retrieved and verified %d records\n", TEST_NUM_RECORDS);

    /* Test overwriting records */
    for (int i = 0; i < 10; i++) {
        test_record_t record;
        record.id = i + 1000;
        snprintf(record.data, sizeof(record.data), "Modified record %d", i);

        record_ix result = allocator_store(&alloc, &record, locators[i]);
        assert(result >= 0);
    }
    printf("  Overwrote 10 records\n");

    /* Verify overwrites */
    for (int i = 0; i < 10; i++) {
        test_record_t record;
        record_ix result = allocator_retrieve(&alloc, locators[i], &record);
        assert(result >= 0);
        assert(record.id == (uint32_t)(i + 1000));

        char expected[60];
        snprintf(expected, sizeof(expected), "Modified record %d", i);
        assert(strcmp(record.data, expected) == 0);
    }
    printf("  Verified overwrites\n");

    /* Test out-of-range record (should zero-fill) */
    test_record_t record;
    record_ix ret = allocator_retrieve(&alloc, 99999999, &record);
    assert(ret >= 0);
    assert(record.id == 0);
    printf("  Out-of-range record correctly zero-filled\n");

    allocator_destroy(&alloc);
    printf("  Memory allocator destroyed\n");
    printf("Memory allocator tests PASSED\n\n");
}

static void test_disk_allocator(void) {
    printf("Testing disk allocator...\n");

    const char* filepath = "test_allocator_file.dat";
    FILE *file = fopen(filepath, "w+x");
    remove(filepath);

    allocator_t alloc;
    int result = allocator_init_disk(&alloc, sizeof(test_record_t), file);
    assert(result == 0);
    printf("  Disk allocator initialized\n");

    /* Test appending records */
    record_ix locators[TEST_NUM_RECORDS];
    for (int i = 0; i < TEST_NUM_RECORDS; i++) {
        test_record_t record;
        record.id = i;
        snprintf(record.data, sizeof(record.data), "Disk record %d", i);

        locators[i] = allocator_append(&alloc, &record);
        assert(locators[i] >= 0);
    }
    printf("  Appended %d records\n", TEST_NUM_RECORDS);

    /* Test retrieving records */
    for (int i = 0; i < TEST_NUM_RECORDS; i++) {
        test_record_t record;
        record_ix result = allocator_retrieve(&alloc, locators[i], &record);
        assert(result >= 0);
        assert(record.id == (uint32_t)i);

        char expected[60];
        snprintf(expected, sizeof(expected), "Disk record %d", i);
        assert(strcmp(record.data, expected) == 0);
    }
    printf("  Retrieved and verified %d records\n", TEST_NUM_RECORDS);

    /* Test overwriting records */
    for (int i = 0; i < 10; i++) {
        test_record_t record;
        record.id = i + 2000;
        snprintf(record.data, sizeof(record.data), "Disk modified %d", i);

        record_ix result = allocator_store(&alloc, &record, locators[i]);
        assert(result >= 0);
    }
    printf("  Overwrote 10 records\n");

    /* Verify overwrites */
    for (int i = 0; i < 10; i++) {
        test_record_t record;
        record_ix result = allocator_retrieve(&alloc, locators[i], &record);
        assert(result >= 0);
        assert(record.id == (uint32_t)(i + 2000));

        char expected[60];
        snprintf(expected, sizeof(expected), "Disk modified %d", i);
        assert(strcmp(record.data, expected) == 0);
    }
    printf("  Verified overwrites\n");

    allocator_destroy(&alloc);
    printf("  Disk allocator destroyed and file deleted\n");

    /* Verify file was deleted */
    FILE* test = fopen(filepath, "rb");
    assert(test == NULL);
    printf("  Verified file deletion\n");

    printf("Disk allocator tests PASSED\n\n");
}

static void test_capacity_limits(void) {
    printf("Testing capacity limits...\n");

    /* Create small allocator that can only hold 5 records */
    size_t small_max = sizeof(test_record_t) * 5;
    allocator_t alloc;
    int result = allocator_init_memory(&alloc, sizeof(test_record_t), small_max);
    assert(result == 0);

    /* Fill it up */
    test_record_t record;
    memset(&record, 0, sizeof(record));

    for (int i = 0; i < 5; i++) {
        record_ix loc = allocator_append(&alloc, &record);
        assert(loc >= 0);
    }
    printf("  Filled allocator to capacity (5 records)\n");

    /* Next append should fail */
    record_ix loc = allocator_append(&alloc, &record);
    assert(loc < 0);
    printf("  Correctly rejected append when full\n");

    allocator_destroy(&alloc);
    printf("Capacity limit tests PASSED\n\n");
}

static void test_large_allocation(void) {
    printf("Testing large memory allocation...\n");

    /* Test with 100 MB reservation to verify address space reservation works */
    size_t large_max = 100 * 1024 * 1024;  /* 100 MB */
    allocator_t alloc;
    int result = allocator_init_memory(&alloc, sizeof(test_record_t), large_max);
    assert(result == 0);
    printf("  Reserved 100 MB address space\n");

    /* Add just a few records - should only commit a few pages */
    test_record_t record;
    memset(&record, 0, sizeof(record));

    record_ix locs[10];
    for (int i = 0; i < 10; i++) {
        record.id = i;
        locs[i] = allocator_append(&alloc, &record);
        assert(locs[i] >= 0);
    }
    printf("  Added 10 records (should only commit ~1 page)\n");

    /* Verify we can read them back */
    for (int i = 0; i < 10; i++) {
        test_record_t read_record;
        record_ix result = allocator_retrieve(&alloc, locs[i], &read_record);
        assert(result >= 0);
        assert(read_record.id == (uint32_t)i);
    }
    printf("  Verified record retrieval\n");

    allocator_destroy(&alloc);
    printf("Large allocation test PASSED\n\n");
}

static void test_convert_to_disk(void) {
    printf("Testing allocator_convert_to_disk...\n");

    const char* filepath = "test_file.dat";

    /* Test 1: Basic conversion with data */
    printf("  Test 1: Basic conversion with data\n");
    FILE *file = fopen(filepath, "w+x");
    remove(filepath);
    allocator_t alloc;
    int result = allocator_init_convertible(&alloc, sizeof(test_record_t), TEST_MAX_MEMORY, file);
    assert(result == 0);

    /* Add some records to memory allocator */
    record_ix locs[100];
    for (int i = 0; i < 100; i++) {
        test_record_t record;
        record.id = i + 5000;
        snprintf(record.data, sizeof(record.data), "Convert test record %d", i);
        locs[i] = allocator_append(&alloc, &record);
        assert(locs[i] >= 0);
    }
    printf("    Added 100 records to convertible allocator (memory mode)\n");

    /* Convert to disk */
    result = allocator_convert_to_disk(&alloc);
    assert(result == 0);
    assert(alloc.using_disk == true);
    printf("    Successfully converted to disk\n");

    /* Verify all data is preserved */
    for (int i = 0; i < 100; i++) {
        test_record_t record;
        record_ix ret = allocator_retrieve(&alloc, locs[i], &record);
        assert(ret >= 0);
        assert(record.id == (uint32_t)(i + 5000));

        char expected[60];
        snprintf(expected, sizeof(expected), "Convert test record %d", i);
        assert(strcmp(record.data, expected) == 0);
    }
    printf("    Verified all 100 records after conversion\n");

    /* Test that we can still append after conversion */
    test_record_t new_record;
    new_record.id = 9999;
    snprintf(new_record.data, sizeof(new_record.data), "Post-conversion record");
    record_ix new_loc = allocator_append(&alloc, &new_record);
    assert(new_loc >= 0);
    printf("    Successfully appended record after conversion\n");

    /* Verify the new record */
    test_record_t read_record;
    result = allocator_retrieve(&alloc, new_loc, &read_record);
    assert(result >= 0);
    assert(read_record.id == 9999);
    assert(strcmp(read_record.data, "Post-conversion record") == 0);
    printf("    Verified post-conversion append\n");

    allocator_destroy(&alloc);
    printf("    Cleaned up\n");

    /* Test 2: Conversion with empty allocator */
    printf("  Test 2: Conversion with empty allocator\n");
    file = fopen(filepath, "w+x");
    remove(filepath);
    result = allocator_init_convertible(&alloc, sizeof(test_record_t), TEST_MAX_MEMORY, file);
    assert(result == 0);

    result = allocator_convert_to_disk(&alloc);
    assert(result == 0);
    assert(alloc.using_disk == true);
    assert(alloc.count == 0);
    printf("    Successfully converted empty allocator\n");

    /* Add data after conversion */
    test_record_t record;
    record.id = 123;
    snprintf(record.data, sizeof(record.data), "First record");
    record_ix loc = allocator_append(&alloc, &record);
    assert(loc >= 0);

    result = allocator_retrieve(&alloc, loc, &record);
    assert(result >= 0);
    assert(record.id == 123);
    printf("    Can use allocator normally after converting empty allocator\n");

    allocator_destroy(&alloc);
    printf("    Cleaned up\n");

    printf("allocator_convert_to_disk tests PASSED\n\n");
}

int main(void) {
    printf("=== Linear Allocator Test Suite ===\n\n");

    test_memory_allocator();
    test_disk_allocator();
    test_capacity_limits();
    test_large_allocation();
    test_convert_to_disk();

    printf("=== All Tests PASSED ===\n");
    return 0;
}
