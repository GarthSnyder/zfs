#include "allocator.h"
#include "linear_hash.h"
#include "linear_hash_stats.h"

#ifndef LINEAR_HASH_TYPES_H
#define LINEAR_HASH_TYPES_H

#define ENTRIES_PER_BUCKET 6

/* Entry in a bucket: hash value + locator to data */
typedef struct {
	uint64_t  	be_hash;
	record_ix_t 	be_record;  /* index of actual data */
} bucket_entry_t;

/* Bucket structure: fixed array of entries + overflow pointer */
typedef struct {
	bucket_entry_t	b_entries[ENTRIES_PER_BUCKET];
	record_ix_t	b_overflow;  /* 0 if no overflow */
} bucket_t;

/* Internal iterator for bucket entries */
typedef struct {
	linear_hash_t	ei_lh;
	allocator_t	ei_alloc;
	record_ix_t	ei_bucket_ix;	/* -1 == overflow not yet assigned */
	record_ix_t	ei_entry_ix;	/* -1 == bucket not yet retrieved */
	bucket_t	ei_bucket;	/* Working copy of allocator version */
	bool		ei_dirty;	/* Needs writeback */
} entry_iterator_t;

/* Iterator for retrieving records by hash */
struct lh_iterator {
	uint64_t		hash;
	entry_iterator_t	entry_iterator;
};

typedef struct {
	op_stats	*ot_stat_bin;
	uint64_t	ot_start_ops;
	uint64_t	ot_latest_ops;
} ops_tracker_t;

typedef struct {
	uint64_t	num_entries;	/* total entries in table */
	uint64_t	mem_highwater;	/* Most memory used by allocators */
	op_stats	inserts;	/* Inserts from external client */
	op_stats	retrieves;	/* Retrieves from external client */
	op_stats	splits;		/* Splits, generated internally */
} lh_stats_t;

/* Hash table structure */
struct linear_hash {
	size_t		record_size;
	uint8_t		hash_suffix_length;	/* Granularity above split */
	record_ix_t	split_pointer;    	/* Next bucket to split */
	bool		validate;		/* Validation per operation (slow) */
	uint64_t	max_memory;
	int     	next_memory_check;	/* Number of splits before check */
	int       	next_iterator;
	lh_iterator_t	iterators[MAX_ITERATORS_OUTSTANDING];
	lh_stats_t	stats;
	ops_tracker_t	ops_tracker;
	allocator_t	data_alloc;		/* Data records */
	allocator_t	bucket_alloc;		/* Main buckets */
	allocator_t	overflow_alloc;		/* Overflow buckets */
};

#endif /* LINEAR_HASH_TYPES_H */
