/*
 * Linear hashing implementation
 */

#include "linear_hash.h"
#include "allocator.h"
#include <stdlib.h>
#include <string.h>

#define ENTRIES_PER_BUCKET 8
#define LOAD_FACTOR_THRESHOLD 0.75
#define END_OF_CHAIN_BIT ((uint64_t)1 << 63)
#define HASH_MASK (~END_OF_CHAIN_BIT)

/* Entry in a bucket: hash value + locator to data */
typedef struct {
    uint64_t hash;           /* 63-bit hash + end-of-chain bit */
    locator_t data_locator;  /* locator to actual data */
} bucket_entry_t;

/* Bucket structure: fixed array of entries + overflow pointer */
typedef struct {
    bucket_entry_t entries[ENTRIES_PER_BUCKET];
    locator_t overflow_locator;  /* 0 if no overflow */
} bucket_t;

struct linear_hash {
    size_t data_size;
    size_t initial_buckets;
    uint64_t num_buckets;      /* current number of buckets */
    uint64_t level;            /* current level */
    uint64_t split_pointer;    /* next bucket to split */
    uint64_t total_entries;    /* total entries in table */
    uint64_t num_splits;       /* statistics */

    allocator_t* data_alloc;     /* data records */
    allocator_t* bucket_alloc;   /* main buckets */
    allocator_t* overflow_alloc; /* overflow buckets */

    /* In-memory bucket array (grows dynamically) */
    bucket_t* buckets;
    size_t buckets_capacity;
};

/* Calculate bucket index for a hash value */
static uint64_t hash_to_bucket(linear_hash_t* lh, uint64_t hash) {
    hash &= HASH_MASK;  /* clear high bit */

    uint64_t bucket_size = lh->initial_buckets << lh->level;
    uint64_t bucket = hash % bucket_size;

    /* If bucket hasn't been split yet, use previous level */
    if (bucket < lh->split_pointer) {
        bucket_size <<= 1;
        bucket = hash % bucket_size;
    }

    return bucket;
}

/* Expand bucket array if needed */
static int expand_buckets(linear_hash_t* lh) {
    if (lh->num_buckets >= lh->buckets_capacity) {
        size_t new_capacity = lh->buckets_capacity * 2;
        bucket_t* new_buckets = (bucket_t*)realloc(lh->buckets,
                                                    new_capacity * sizeof(bucket_t));
        if (!new_buckets) {
            return -1;
        }

        /* Initialize new buckets */
        memset(new_buckets + lh->buckets_capacity, 0,
               lh->buckets_capacity * sizeof(bucket_t));

        lh->buckets = new_buckets;
        lh->buckets_capacity = new_capacity;
    }
    return 0;
}

/* Split a bucket */
static int split_bucket(linear_hash_t* lh) {
    uint64_t old_bucket_idx = lh->split_pointer;
    uint64_t new_bucket_idx = lh->num_buckets;

    /* Expand bucket array for new bucket */
    if (expand_buckets(lh) != 0) {
        return -1;
    }

    lh->num_buckets++;

    /* Collect all entries from old bucket and overflow chain */
    bucket_entry_t all_entries[256];  /* Large enough for most cases */
    int entry_count = 0;

    bucket_t* old_bucket = &lh->buckets[old_bucket_idx];
    bucket_t* new_bucket = &lh->buckets[new_bucket_idx];

    /* Collect entries from main bucket */
    for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
        if (old_bucket->entries[i].data_locator != 0 && entry_count < 256) {
            all_entries[entry_count++] = old_bucket->entries[i];
        }
    }

    /* Collect entries from overflow chain */
    locator_t overflow_loc = old_bucket->overflow_locator;
    bucket_t overflow_bucket;

    while (overflow_loc != 0 && entry_count < 256) {
        if (allocator_retrieve(lh->overflow_alloc, overflow_loc,
                              &overflow_bucket) != 0) {
            break;
        }

        for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
            if (overflow_bucket.entries[i].data_locator != 0 && entry_count < 256) {
                all_entries[entry_count++] = overflow_bucket.entries[i];
            }
        }

        overflow_loc = overflow_bucket.overflow_locator;
    }

    /* Clear old bucket */
    memset(old_bucket->entries, 0, sizeof(old_bucket->entries));
    old_bucket->overflow_locator = 0;

    /* Redistribute all entries */
    for (int i = 0; i < entry_count; i++) {
        uint64_t hash = all_entries[i].hash & HASH_MASK;

        /* Calculate target bucket using new hash function level */
        uint64_t new_bucket_size = (lh->initial_buckets << lh->level) * 2;
        uint64_t target_bucket_idx = hash % new_bucket_size;

        bucket_t* target = (target_bucket_idx == new_bucket_idx) ? new_bucket : old_bucket;

        /* Find empty slot in target bucket */
        int placed = 0;
        for (int j = 0; j < ENTRIES_PER_BUCKET; j++) {
            if (target->entries[j].data_locator == 0) {
                target->entries[j] = all_entries[i];
                placed = 1;
                break;
            }
        }

        /* If main bucket full, create/use overflow bucket */
        if (!placed) {
            bucket_t overflow;

            if (target->overflow_locator == 0) {
                /* Create new overflow bucket */
                memset(&overflow, 0, sizeof(overflow));
                overflow.entries[0] = all_entries[i];
                locator_t new_overflow = allocator_append(lh->overflow_alloc, &overflow);
                if (new_overflow != 0) {
                    target->overflow_locator = new_overflow;
                }
            } else {
                /* Try to add to existing overflow chain */
                locator_t current = target->overflow_locator;
                locator_t prev = 0;

                while (current != 0) {
                    if (allocator_retrieve(lh->overflow_alloc, current, &overflow) != 0) {
                        break;
                    }

                    /* Try to find empty slot */
                    for (int j = 0; j < ENTRIES_PER_BUCKET; j++) {
                        if (overflow.entries[j].data_locator == 0) {
                            overflow.entries[j] = all_entries[i];
                            allocator_overwrite(lh->overflow_alloc, current, &overflow);
                            placed = 1;
                            break;
                        }
                    }

                    if (placed) break;

                    prev = current;
                    current = overflow.overflow_locator;
                }

                /* If still not placed, create new overflow at end of chain */
                if (!placed && prev != 0) {
                    memset(&overflow, 0, sizeof(overflow));
                    overflow.entries[0] = all_entries[i];
                    locator_t new_overflow = allocator_append(lh->overflow_alloc, &overflow);

                    if (new_overflow != 0 &&
                        allocator_retrieve(lh->overflow_alloc, prev, &overflow) == 0) {
                        overflow.overflow_locator = new_overflow;
                        allocator_overwrite(lh->overflow_alloc, prev, &overflow);
                    }
                }
            }
        }
    }

    /* Update split pointer */
    lh->split_pointer++;

    /* Check if we've completed this level */
    uint64_t buckets_at_level = lh->initial_buckets << lh->level;
    if (lh->split_pointer >= buckets_at_level) {
        lh->level++;
        lh->split_pointer = 0;
    }

    lh->num_splits++;
    return 0;
}

linear_hash_t* lh_init(size_t data_size, size_t initial_buckets, size_t max_memory) {
    if (data_size == 0 || initial_buckets == 0) {
        return NULL;
    }

    linear_hash_t* lh = (linear_hash_t*)malloc(sizeof(linear_hash_t));
    if (!lh) {
        return NULL;
    }

    memset(lh, 0, sizeof(linear_hash_t));
    lh->data_size = data_size;
    lh->initial_buckets = initial_buckets;
    lh->num_buckets = initial_buckets;
    lh->level = 0;
    lh->split_pointer = 0;

    /* Initialize allocators */
    lh->data_alloc = allocator_init_memory(data_size, max_memory);
    if (!lh->data_alloc) {
        free(lh);
        return NULL;
    }

    /* Bucket allocator: estimate based on expected table growth */
    size_t bucket_memory = sizeof(bucket_t) * initial_buckets * 8;
    lh->bucket_alloc = allocator_init_memory(sizeof(bucket_t), bucket_memory);
    if (!lh->bucket_alloc) {
        allocator_destroy(lh->data_alloc);
        free(lh);
        return NULL;
    }

    /* Overflow allocator: make it large to handle worst-case collisions */
    /* Allocate based on max_memory / data_size to estimate max entries */
    size_t max_entries = max_memory / data_size;
    size_t overflow_memory = sizeof(bucket_t) * (max_entries / ENTRIES_PER_BUCKET);
    if (overflow_memory < bucket_memory) {
        overflow_memory = bucket_memory * 4;  /* ensure reasonable minimum */
    }
    lh->overflow_alloc = allocator_init_memory(sizeof(bucket_t), overflow_memory);
    if (!lh->overflow_alloc) {
        allocator_destroy(lh->data_alloc);
        allocator_destroy(lh->bucket_alloc);
        free(lh);
        return NULL;
    }

    /* Allocate initial bucket array in regular memory */
    lh->buckets_capacity = initial_buckets;
    lh->buckets = (bucket_t*)calloc(lh->buckets_capacity, sizeof(bucket_t));
    if (!lh->buckets) {
        allocator_destroy(lh->data_alloc);
        allocator_destroy(lh->bucket_alloc);
        allocator_destroy(lh->overflow_alloc);
        free(lh);
        return NULL;
    }

    return lh;
}

int lh_insert(linear_hash_t* lh, uint64_t hash_value, const void* data) {
    if (!lh || !data) {
        return -1;
    }

    hash_value &= HASH_MASK;  /* ensure high bit is clear */

    /* Store data */
    locator_t data_loc = allocator_append(lh->data_alloc, data);
    if (data_loc == 0) {
        return -1;  /* out of memory */
    }

    /* Find bucket */
    uint64_t bucket_idx = hash_to_bucket(lh, hash_value);
    bucket_t* bucket = &lh->buckets[bucket_idx];

    /* Try to insert in main bucket */
    for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
        if (bucket->entries[i].data_locator == 0) {
            bucket->entries[i].hash = hash_value;
            bucket->entries[i].data_locator = data_loc;
            lh->total_entries++;

            /* Check if we should split */
            double load_factor = (double)lh->total_entries /
                                (lh->num_buckets * ENTRIES_PER_BUCKET);
            if (load_factor > LOAD_FACTOR_THRESHOLD) {
                split_bucket(lh);  /* ignore split failures */
            }

            return 0;
        }
    }

    /* Bucket full, use overflow */
    locator_t current_overflow = bucket->overflow_locator;
    bucket_t overflow_bucket;

    /* Search overflow chain for empty slot */
    while (current_overflow != 0) {
        if (allocator_retrieve(lh->overflow_alloc, current_overflow,
                              &overflow_bucket) != 0) {
            return -1;
        }

        for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
            if (overflow_bucket.entries[i].data_locator == 0) {
                overflow_bucket.entries[i].hash = hash_value;
                overflow_bucket.entries[i].data_locator = data_loc;
                allocator_overwrite(lh->overflow_alloc, current_overflow,
                                   &overflow_bucket);
                lh->total_entries++;
                return 0;
            }
        }

        current_overflow = overflow_bucket.overflow_locator;
    }

    /* Need new overflow bucket */
    memset(&overflow_bucket, 0, sizeof(overflow_bucket));
    overflow_bucket.entries[0].hash = hash_value;
    overflow_bucket.entries[0].data_locator = data_loc;

    locator_t new_overflow = allocator_append(lh->overflow_alloc, &overflow_bucket);
    if (new_overflow == 0) {
        return -1;
    }

    /* Link to chain */
    if (bucket->overflow_locator == 0) {
        bucket->overflow_locator = new_overflow;
    } else {
        /* Find end of chain and link */
        current_overflow = bucket->overflow_locator;
        while (current_overflow != 0) {
            if (allocator_retrieve(lh->overflow_alloc, current_overflow,
                                  &overflow_bucket) != 0) {
                return -1;
            }
            if (overflow_bucket.overflow_locator == 0) {
                overflow_bucket.overflow_locator = new_overflow;
                allocator_overwrite(lh->overflow_alloc, current_overflow,
                                   &overflow_bucket);
                break;
            }
            current_overflow = overflow_bucket.overflow_locator;
        }
    }

    lh->total_entries++;
    return 0;
}

int lh_retrieve_start(linear_hash_t* lh, uint64_t hash_value,
                      lh_iterator_t* iter, void* buffer) {
    if (!lh || !iter || !buffer) {
        return -1;
    }

    hash_value &= HASH_MASK;

    memset(iter, 0, sizeof(lh_iterator_t));
    iter->hash_value = hash_value;
    iter->bucket_index = hash_to_bucket(lh, hash_value);
    iter->entry_index = 0;
    iter->overflow_loc = 0;
    iter->mode = 0;  /* hash search mode */

    return lh_next(lh, iter, buffer);
}

int lh_retrieve_all_start(linear_hash_t* lh, lh_iterator_t* iter, void* buffer) {
    if (!lh || !iter || !buffer) {
        return -1;
    }

    memset(iter, 0, sizeof(lh_iterator_t));
    iter->bucket_index = 0;
    iter->entry_index = 0;
    iter->overflow_loc = 0;
    iter->mode = 1;  /* retrieve all mode */

    return lh_next(lh, iter, buffer);
}

int lh_next(linear_hash_t* lh, lh_iterator_t* iter, void* buffer) {
    if (!lh || !iter || !buffer) {
        return -1;
    }

    bucket_t bucket;
    bucket_t* bucket_ptr;

    while (1) {
        /* Determine which bucket we're scanning */
        if (iter->overflow_loc == 0) {
            /* Scanning main bucket */
            if (iter->mode == 0) {
                /* Hash search: only search one bucket */
                if (iter->bucket_index >= lh->num_buckets) {
                    return 0;  /* no more entries */
                }
                bucket_ptr = &lh->buckets[iter->bucket_index];
            } else {
                /* Retrieve all: scan all buckets */
                if (iter->bucket_index >= lh->num_buckets) {
                    return 0;  /* no more buckets */
                }
                bucket_ptr = &lh->buckets[iter->bucket_index];
            }
        } else {
            /* Scanning overflow bucket */
            if (allocator_retrieve(lh->overflow_alloc, iter->overflow_loc,
                                  &bucket) != 0) {
                return -1;
            }
            bucket_ptr = &bucket;
        }

        /* Scan entries in current bucket */
        while (iter->entry_index < ENTRIES_PER_BUCKET) {
            bucket_entry_t* entry = &bucket_ptr->entries[iter->entry_index];
            iter->entry_index++;

            if (entry->data_locator == 0) {
                continue;  /* empty slot */
            }

            /* Check if entry matches (for hash search) */
            if (iter->mode == 0) {
                uint64_t entry_hash = entry->hash & HASH_MASK;
                if (entry_hash != iter->hash_value) {
                    continue;  /* hash doesn't match */
                }
            }

            /* Retrieve data */
            if (allocator_retrieve(lh->data_alloc, entry->data_locator,
                                  buffer) != 0) {
                return -1;
            }

            return 1;  /* found entry */
        }

        /* Move to overflow bucket if exists */
        if (iter->overflow_loc == 0 && bucket_ptr->overflow_locator != 0) {
            iter->overflow_loc = bucket_ptr->overflow_locator;
            iter->entry_index = 0;
            continue;
        }

        /* Move to next overflow in chain */
        if (iter->overflow_loc != 0) {
            if (bucket_ptr->overflow_locator != 0) {
                iter->overflow_loc = bucket_ptr->overflow_locator;
                iter->entry_index = 0;
                continue;
            }
        }

        /* Done with this bucket */
        if (iter->mode == 0) {
            /* Hash search: only search one bucket */
            return 0;
        } else {
            /* Retrieve all: move to next bucket */
            iter->bucket_index++;
            iter->entry_index = 0;
            iter->overflow_loc = 0;

            if (iter->bucket_index >= lh->num_buckets) {
                return 0;  /* no more buckets */
            }
        }
    }
}

void lh_get_stats(linear_hash_t* lh, uint64_t* total_entries,
                  uint64_t* num_buckets, uint64_t* num_splits) {
    if (!lh) {
        return;
    }

    if (total_entries) *total_entries = lh->total_entries;
    if (num_buckets) *num_buckets = lh->num_buckets;
    if (num_splits) *num_splits = lh->num_splits;
}

void lh_destroy(linear_hash_t* lh) {
    if (!lh) {
        return;
    }

    allocator_destroy(lh->data_alloc);
    allocator_destroy(lh->bucket_alloc);
    allocator_destroy(lh->overflow_alloc);

    free(lh->buckets);
    free(lh);
}
