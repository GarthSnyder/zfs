/*
 * Linear hashing implementation
 */

#include "linear_hash.h"
#include "allocator.h"
#include <stdlib.h>
#include <string.h>

#define ENTRIES_PER_BUCKET 4
#define INITIAL_NUMBER_OF_BUCKETS 2
#define LOAD_FACTOR_THRESHOLD 0.75

/* Hashes are 63 bits. The top bit is always set to 1. 0 == empty */
#define VALID_HASH_BIT ((uint64_t)1 << 63)
#define HASH_MASK (~END_OF_CHAIN_BIT)

/* Entry in a bucket: hash value + locator to data */
typedef struct {
    uint64_t  hash;    /* 63-bit hash, high bit set */
    record_ix record;  /* locator of actual data */
} bucket_entry_t;

/* Bucket structure: fixed array of entries + overflow pointer */
typedef struct {
    bucket_entry_t entries[ENTRIES_PER_BUCKET];
    record_ix overflow;  /* 0 if no overflow */
} bucket_t;

struct linear_hash {
    size_t record_size;
    size_t initial_buckets;
    uint64_t num_buckets;      /* current number of buckets */
    uint64_t level;            /* current hashing level (and this + 1) */
    record_ix split_pointer;    /* next bucket to split */
    uint64_t total_entries;    /* total entries in table */
    uint64_t num_splits;       /* statistics */

    allocator_t data_alloc;     /* data records */
    allocator_t bucket_alloc;   /* main buckets */
    allocator_t overflow_alloc; /* overflow buckets */

    /* In-memory bucket array (grows dynamically) */
    bucket_t* buckets;
    size_t buckets_capacity;
};

typedef struct traced_bucket {
    bucket_t bucket;
    record_ix record;
    allocator_t *alloc;
} traced_bucket_t;

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
        if (old_bucket->entries[i].record != 0 && entry_count < 256) {
            all_entries[entry_count++] = old_bucket->entries[i];
        }
    }

    /* Collect entries from overflow chain */
    locator_t overflow_loc = old_bucket->overflow;
    bucket_t overflow_bucket;

    while (overflow_loc != 0 && entry_count < 256) {
        if (allocator_retrieve(lh->overflow_alloc, overflow_loc,
                              &overflow_bucket) != 0) {
            break;
        }

        for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
            if (overflow_bucket.entries[i].record != 0 && entry_count < 256) {
                all_entries[entry_count++] = overflow_bucket.entries[i];
            }
        }

        overflow_loc = overflow_bucket.overflow;
    }

    /* Clear old bucket */
    memset(old_bucket->entries, 0, sizeof(old_bucket->entries));
    old_bucket->overflow = 0;

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
            if (target->entries[j].record == 0) {
                target->entries[j] = all_entries[i];
                placed = 1;
                break;
            }
        }

        /* If main bucket full, create/use overflow bucket */
        if (!placed) {
            bucket_t overflow;

            if (target->overflow == 0) {
                /* Create new overflow bucket */
                memset(&overflow, 0, sizeof(overflow));
                overflow.entries[0] = all_entries[i];
                locator_t new_overflow = allocator_append(lh->overflow_alloc, &overflow);
                if (new_overflow != 0) {
                    target->overflow = new_overflow;
                }
            } else {
                /* Try to add to existing overflow chain */
                locator_t current = target->overflow;
                locator_t prev = 0;

                while (current != 0) {
                    if (allocator_retrieve(lh->overflow_alloc, current, &overflow) != 0) {
                        break;
                    }

                    /* Try to find empty slot */
                    for (int j = 0; j < ENTRIES_PER_BUCKET; j++) {
                        if (overflow.entries[j].record == 0) {
                            overflow.entries[j] = all_entries[i];
                            allocator_overwrite(lh->overflow_alloc, current, &overflow);
                            placed = 1;
                            break;
                        }
                    }

                    if (placed) break;

                    prev = current;
                    current = overflow.overflow;
                }

                /* If still not placed, create new overflow at end of chain */
                if (!placed && prev != 0) {
                    memset(&overflow, 0, sizeof(overflow));
                    overflow.entries[0] = all_entries[i];
                    locator_t new_overflow = allocator_append(lh->overflow_alloc, &overflow);

                    if (new_overflow != 0 &&
                        allocator_retrieve(lh->overflow_alloc, prev, &overflow) == 0) {
                        overflow.overflow = new_overflow;
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

static int
check_split(linear_hash_t *lh) {
    double load_factor = (double)lh->total_entries /
        (lh->num_buckets * ENTRIES_PER_BUCKET);
    if (load_factor > LOAD_FACTOR_THRESHOLD) {
        return (split_bucket(lh));
    }
    return 0;
}

static boolean_t
attempt_add_to_bucket(bucket_t *bucket, lh_hash_t hash, record_ix record) {
    for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
        if (bucket->entries[i].hash == 0) {
            bucket->entries[i].hash = hash | VALID_HASH_BIT;
            bucket->entries[i].record = record;
            return (B_TRUE);
        }
    }
    return (B_FALSE);
}

static bucket_t
new_bucket() {
    bucket_t bucket;
    memset(&bucket, 0, sizeof (bucket));
    return bucket
}

static int
retrieve_traced_bucket(allocator_t *alloc, record_ix record,
    traced_bucket_t *tbucket)
{
    memset(tbucket, 0, sizeof(*tbucket));
    if (ret = allocator_retrieve(alloc, record, &tbucket->bucket)) {
        return ret;
    }
    tbucket->record = record;
    tbucket->alloc = alloc;
    return 0;
}

static int
store_traced_bucket(traced_bucket_t *tbucket)
{
    memset(tbucket, 0, sizeof(*tbucket));
    if (ret = allocator_retrieve(alloc, record, &tbucket->bucket)) {
        return ret;
    }
    tbucket->record = record;
    tbucket->alloc = alloc;
    return 0;
}
 
int
lh_init(linear_hash_t *lh, size_t record_size, size_t max_memory)
{
    if (record_size == 0) {
        return (-1);
    }

    memset(lh, 0, sizeof(linear_hash_t));
    lh->record_size = record_size;
    lh->initial_buckets = INITIAL_NUMBER_OF_BUCKETS;
    lh->num_buckets = INITIAL_NUMBER_OF_BUCKETS;
    lh->level = 0;
    lh->split_pointer = 0;

    /* Initialize allocators */
    if (allocator_init_memory(&lh->data_alloc, record_size, max_memory)) {
        return (-2);
    }
    /* Bucket allocator: estimate based on expected table growth */
    if (allocator_init_memory(&lh->bucket_alloc, sizeof (bucket_t), 
        max_memory))
    {
        allocator_destroy(&lh->data_alloc);
        return (-3);
    }
    if (allocator_init_memory(&lh->overflow_alloc, sizeof (bucket_t), 
        max_memory))
    {
        allocator_destroy(&lh->data_alloc);
        allocator_destroy(&lh->bucket_alloc);
        return (-4);
    }
    /* Skip first overflow bucket to allow 0 = end of chain */
    bucket_t dummy;
    memset(&dummy, 0, sizeof(bucket_t));
    if (allocator_append(&lh->overflow_alloc, &dummy)) {
        return (-5);
    }
    return 0;
}

int lh_insert(linear_hash_t* lh, uint64_t hash, const void* data) {
    if (!lh || !data) {
        return (-1);
    }

    hash |= VALID_HASH_BIT;  /* ensure high bit is set */

    /* Store data */
    record_ix record = allocator_append(&lh->data_alloc, data);
    if (record < 0) {
        return (-2);
    }

    record_ix bucket_ix = hash_to_bucket(lh, hash);
    traced_bucket_t traced_bucket;
    traced_bucket_t *this_bucket = &traced_bucket;
    traced_bucket_t *prev_bucket = NULL;

    if (retrieve_traced_bucket(&lh->bucket_alloc, bucket_ix, this_bucket)
        != bucket_ix)
    {
        return (-3);
    }

    int retcode = 0;

    while (B_TRUE) {
        if (attempt_add_to_bucket(&this_bucket->bucket, hash, record)) {
            retcode = store_traced_bucket(this_bucket);
            break;
        } else if (!this_bucket->bucket.overflow) {
            bucket_t new_overflow = new_bucket();
            /* Can't fail here */
            (void) attempt_add_to_bucket(&new_overflow, hash, record);
            record_ix new_rec = allocator_append(alloc->overflow_alloc,
                &new_overflow);
            if (new_rec < 0) { return new_rec; }
            this_bucket->bucket.overflow = new_rec;
            return store_traced_bucket(this_bucket);
        } else {
            retrieve_traced_bucket(&lh->overflow_alloc, this_bucket,
                this_bucket->bucket.overflow);
        }
    }


    /* Try to insert in main bucket */
    if (attempt_add_to_bucket(&bucket, hash, record) {
        if (allocator_store(&lh->bucket_alloc, &bucket, bucket_ix) < 0) {
            return (-4);
        }
    } else if (!bucket.overflow) {
        /* New overflow bucket, and it's the first */
        bucket_t overflow_bucket = new_bucket();
        (void) attempt_add_to_bucket(&overflow_bucket, hash, record);
        record_ix overflow_bucket_ix = allocator_append(&lh->overflow_alloc, 
            &overflow_bucket);
        if (overflow_bucket_ix < 0) {
            return (-5);
        }
        bucket.overflow = overflow_bucket_ix;
        if (allocator_store(&lh->bucket_alloc, &bucket, record) < 0) {
            return (-6);
        }
    } else {
        struct bucket_with_index {
            bucket_t bucket;
            record_ix index;
        }
        /* Store in overflow, chaining back to another overflow */
        bucket_t this_bucket = new_bucket();
        record_ix this_bucket_ix = bucket.overflow;
        record_ix previous_bucket_ix = -1;
        bucket_t prev_bucket = new_bucket();
        while (B_TRUE) {
            if (allocator_retrieve(&lh->overflow_alloc, &this_bucket,
                this_bucket_ix) < 0) {
                return (-7);
            }
            if (attempt_add_to_bucket(&this_bucket, hash, record)) {
                if (allocator_store(&lh->overflow_alloc, &this_bucket,
                    this_bucket_ix) < 0) 
                {
                    return (-8)
                } else if (this_bucket.overflow) {
                    prev_bucket = this_bucket;
                    prev_bucket_ix = this_bucket_ix;
                    this_bucket_ix = this_bucket.overflow;
                    continue;
                } else {
                    record_ix = allocator_append(&lh->overflow_alloc, 
                        &overflow_bucket);
                    if (record_ix < 0) {
                        return (-8);
                    }
                    prev_bucket.overflow = record_ix;
                    if (allocator_store(&lh->overflow_alloc, &prev_bucket,
                        prev_bucket_ix) < 0) 
                    {
                        return (-9);
                    }
                    prev_bucket = this_bucket;
                    prev_bucket_ix = this_bucket_ix;
                    this_bucket_ix = this_bucket.overflow;
                }
            }
        }
        new_overflow_bucket.entries[0].hash = hash;
        new_overflow_bucket.entries[0].record = record;
        record_ix new_record = allocator_append(&lh->overflow_alloc, 
            &overflow_bucket)
        if record_ix < 0 {
            return (-7);
        }
        bucket.overflow = new_record;

        /* Search overflow chain for empty slot */
        record_ix previous = bucket_ix;
        record_ix overflow;
        boolean_t already_has_overflow = B_FALSE;
        while (B_TRUE) {
            overflow = bucket.overflow;
            if (overflow) {
                already_has_overflow = B_TRUE;
                if (allocator_retrieve(&lh->overflow_alloc, &bucket, 
                    overflow))
                {
                    return (-5);
                }
                if (attempt_add_to_bucket(&bucket, hash)) {
                    if (allocator_store(&lh->overflow_alloc, &bucket, 
                        bucket_ix) < 0) 
                    {
                        return (-6);
                    }
                    break;         
                }
                previous = overflow;
        }
        if (!overflow) {
            /* Need a new overflow bucket */
            bucket_t new_overflow_bucket;
            memset(&new_overflow_bucket, 0, sizeof (bucket_t));
            (void) attempt_add_to_bucket(&new_overflow_bucket, hash, record);
            record_ix new_overflow_ix = 
            new_overflow_bucket.entries[0].hash = hash;
            new_overflow_bucket.entries[0].record = record;
            record_ix new_record = allocator_append(&lh->overflow_alloc, 
                &overflow_bucket)
            if record_ix < 0 {
                return (-7);
            }
            bucket.overflow = new_record;
            /* link chain */
            allocator_t *domain = already_has_overflow ? &lh->overflow_alloc : 
                &lh->bucket_alloc;
            if (allocator_store(domain, &bucket, previous) < 0) {
                return (-8);
            }
        }
    }

    if (check_split(lh) < 0) {
        return (-5);
    }
    lh->total_entries++;
    return 0;
}

int lh_retrieve_start(linear_hash_t* lh, uint64_t hash,
                      lh_iterator_t* iter, void* buffer) {
    if (!lh || !iter || !buffer) {
        return -1;
    }

    hash &= HASH_MASK;

    memset(iter, 0, sizeof(lh_iterator_t));
    iter->hash = hash;
    iter->bucket_index = hash_to_bucket(lh, hash);
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

            if (entry->record == 0) {
                continue;  /* empty slot */
            }

            /* Check if entry matches (for hash search) */
            if (iter->mode == 0) {
                uint64_t entry_hash = entry->hash & HASH_MASK;
                if (entry_hash != iter->hash) {
                    continue;  /* hash doesn't match */
                }
            }

            /* Retrieve data */
            if (allocator_retrieve(lh->data_alloc, entry->record,
                                  buffer) != 0) {
                return -1;
            }

            return 1;  /* found entry */
        }

        /* Move to overflow bucket if exists */
        if (iter->overflow_loc == 0 && bucket_ptr->overflow != 0) {
            iter->overflow_loc = bucket_ptr->overflow;
            iter->entry_index = 0;
            continue;
        }

        /* Move to next overflow in chain */
        if (iter->overflow_loc != 0) {
            if (bucket_ptr->overflow != 0) {
                iter->overflow_loc = bucket_ptr->overflow;
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
