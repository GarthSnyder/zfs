/*
 * Example demonstrating linear hash table usage
 */

#include "linear_hash.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Example: User database with name-based lookups */

#define NAME_SIZE 32
#define EMAIL_SIZE 64
#define USER_RECORD_SIZE 128

typedef struct {
    uint64_t user_id;
    char name[NAME_SIZE];
    char email[EMAIL_SIZE];
    uint32_t age;
    char padding[24];  /* pad to 128 bytes */
} user_record_t;

/* FNV-1a hash function */
static uint64_t hash_string(const char* str) {
    uint64_t hash = 14695981039346656037ULL;
    while (*str) {
        hash ^= (unsigned char)(*str++);
        hash *= 1099511628211ULL;
    }
    return hash & ~((uint64_t)1 << 63);  /* clear high bit */
}

static void example_user_database(void) {
    printf("=== User Database Example ===\n\n");

    /* Initialize hash table */
    linear_hash_t* db = lh_init(USER_RECORD_SIZE, 8, 5 * 1024 * 1024);
    if (!db) {
        fprintf(stderr, "Failed to initialize hash table\n");
        return;
    }
    printf("User database initialized\n");
    printf("Record size: %d bytes\n\n", USER_RECORD_SIZE);

    /* Insert some users */
    user_record_t user;

    user.user_id = 1001;
    strncpy(user.name, "Alice Johnson", NAME_SIZE);
    strncpy(user.email, "alice@example.com", EMAIL_SIZE);
    user.age = 28;
    lh_insert(db, hash_string(user.name), &user);
    printf("Inserted: %s (%s), age %u\n", user.name, user.email, user.age);

    user.user_id = 1002;
    strncpy(user.name, "Bob Smith", NAME_SIZE);
    strncpy(user.email, "bob@example.com", EMAIL_SIZE);
    user.age = 35;
    lh_insert(db, hash_string(user.name), &user);
    printf("Inserted: %s (%s), age %u\n", user.name, user.email, user.age);

    user.user_id = 1003;
    strncpy(user.name, "Charlie Davis", NAME_SIZE);
    strncpy(user.email, "charlie@example.com", EMAIL_SIZE);
    user.age = 42;
    lh_insert(db, hash_string(user.name), &user);
    printf("Inserted: %s (%s), age %u\n", user.name, user.email, user.age);

    user.user_id = 1004;
    strncpy(user.name, "Diana Prince", NAME_SIZE);
    strncpy(user.email, "diana@example.com", EMAIL_SIZE);
    user.age = 31;
    lh_insert(db, hash_string(user.name), &user);
    printf("Inserted: %s (%s), age %u\n\n", user.name, user.email, user.age);

    /* Look up a user by name */
    printf("Looking up 'Bob Smith'...\n");
    lh_iterator_t iter;
    user_record_t found;

    int result = lh_retrieve_start(db, hash_string("Bob Smith"), &iter, &found);
    if (result == 1) {
        printf("Found: %s (ID: %lu, Email: %s, Age: %u)\n\n",
               found.name, found.user_id, found.email, found.age);
    } else {
        printf("User not found\n\n");
    }

    /* List all users */
    printf("All users in database:\n");
    int count = 0;
    result = lh_retrieve_all_start(db, &iter, &found);
    while (result == 1) {
        printf("  %lu. %s - %s (age %u)\n",
               found.user_id, found.name, found.email, found.age);
        count++;
        result = lh_next(db, &iter, &found);
    }
    printf("Total: %d users\n\n", count);

    /* Get statistics */
    uint64_t total_entries, num_buckets, num_splits;
    lh_get_stats(db, &total_entries, &num_buckets, &num_splits);
    printf("Database statistics:\n");
    printf("  Total entries: %lu\n", total_entries);
    printf("  Buckets: %lu\n", num_buckets);
    printf("  Splits performed: %lu\n\n", num_splits);

    lh_destroy(db);
    printf("Database destroyed\n\n");
}

static void example_duplicate_keys(void) {
    printf("=== Handling Duplicate Keys Example ===\n\n");

    linear_hash_t* lh = lh_init(USER_RECORD_SIZE, 4, 2 * 1024 * 1024);
    if (!lh) {
        fprintf(stderr, "Failed to initialize hash table\n");
        return;
    }

    /* Insert multiple users with the same name (hash collision) */
    user_record_t user;
    uint64_t hash = hash_string("John Doe");

    printf("Inserting 5 users with name 'John Doe':\n");
    for (int i = 0; i < 5; i++) {
        user.user_id = 2000 + i;
        strncpy(user.name, "John Doe", NAME_SIZE);
        snprintf(user.email, EMAIL_SIZE, "johndoe%d@example.com", i);
        user.age = 25 + i;
        lh_insert(lh, hash, &user);
        printf("  ID %lu: %s\n", user.user_id, user.email);
    }
    printf("\n");

    /* Retrieve all users with that name */
    printf("Retrieving all 'John Doe' users:\n");
    lh_iterator_t iter;
    user_record_t found;

    int count = 0;
    int result = lh_retrieve_start(lh, hash, &iter, &found);
    while (result == 1) {
        printf("  ID %lu: %s (age %u)\n",
               found.user_id, found.email, found.age);
        count++;
        result = lh_next(lh, &iter, &found);
    }
    printf("Found %d users with name 'John Doe'\n\n", count);

    lh_destroy(lh);
    printf("Hash table destroyed\n\n");
}

static void example_performance(void) {
    printf("=== Performance Example ===\n\n");

    linear_hash_t* lh = lh_init(USER_RECORD_SIZE, 16, 50 * 1024 * 1024);
    if (!lh) {
        fprintf(stderr, "Failed to initialize hash table\n");
        return;
    }

    const int num_users = 50000;
    user_record_t user;

    printf("Inserting %d users...\n", num_users);
    clock_t start = clock();

    for (int i = 0; i < num_users; i++) {
        user.user_id = i;
        snprintf(user.name, NAME_SIZE, "User_%d", i);
        snprintf(user.email, EMAIL_SIZE, "user%d@example.com", i);
        user.age = 20 + (i % 50);

        uint64_t hash = hash_string(user.name);
        if (lh_insert(lh, hash, &user) != 0) {
            printf("Insert failed at user %d (out of memory)\n", i);
            break;
        }

        if ((i + 1) % 10000 == 0) {
            printf("  Inserted %d users...\n", i + 1);
        }
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    uint64_t total_entries, num_buckets, num_splits;
    lh_get_stats(lh, &total_entries, &num_buckets, &num_splits);

    printf("\nInsertion complete:\n");
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Rate: %.0f inserts/sec\n", total_entries / elapsed);
    printf("  Total entries: %lu\n", total_entries);
    printf("  Buckets: %lu\n", num_buckets);
    printf("  Splits: %lu\n", num_splits);
    printf("  Load factor: %.2f\n\n",
           (double)total_entries / num_buckets / 8.0);

    /* Test retrieval performance */
    printf("Testing retrieval performance...\n");
    start = clock();

    int found = 0;
    for (int i = 0; i < 10000; i++) {
        char name[NAME_SIZE];
        snprintf(name, NAME_SIZE, "User_%d", i);
        uint64_t hash = hash_string(name);

        lh_iterator_t iter;
        user_record_t retrieved;
        if (lh_retrieve_start(lh, hash, &iter, &retrieved) == 1) {
            found++;
        }
    }

    end = clock();
    elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("  Retrieved 10000 entries in %.3f seconds\n", elapsed);
    printf("  Rate: %.0f lookups/sec\n", 10000.0 / elapsed);
    printf("  Success rate: %d/10000\n\n", found);

    /* Test iterate all performance */
    printf("Testing iterate-all performance...\n");
    start = clock();

    int count = 0;
    lh_iterator_t iter2;
    user_record_t retrieved2;
    int result = lh_retrieve_all_start(lh, &iter2, &retrieved2);
    while (result == 1) {
        count++;
        result = lh_next(lh, &iter2, &retrieved2);
    }

    end = clock();
    elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("  Iterated through %d entries in %.3f seconds\n", count, elapsed);
    printf("  Rate: %.0f entries/sec\n\n", count / elapsed);

    lh_destroy(lh);
    printf("Hash table destroyed\n\n");
}

int main(void) {
    printf("========================================\n");
    printf("   Linear Hash Table Examples\n");
    printf("========================================\n\n");

    example_user_database();
    example_duplicate_keys();
    example_performance();

    printf("Examples complete!\n");
    return 0;
}
