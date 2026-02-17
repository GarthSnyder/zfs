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
typedef struct lh_iterator {
	uint64_t		lhi_hash;
	entry_iterator_t	lhi_entry_iterator;
} lh_iterator_s;

typedef struct {
	op_stats_t	*ot_stat_bin;
	uint64_t	ot_start_ops;
	uint64_t	ot_latest_ops;
} ops_tracker_t;

typedef struct {
	uint64_t	lhs_num_entries;	/* Total entries in table */
	uint64_t	lhs_mem_highwater;	/* Most memory used by allocs */
	op_stats_t	lhs_inserts;		/* Inserts from caller */
	op_stats_t	lhs_retrieves;		/* Retrieves from caller */
	op_stats_t	lhs_splits;		/* Splits, internal */
} lh_stats_t;

/* Hash table structure */
struct linear_hash {
	size_t		lh_record_size;
	uint8_t		lh_hash_suffix_length;	/* Granularity above split */
	record_ix_t	lh_split_pointer;    	/* Next bucket to split */
	bool		lh_validate;		/* Validation per operation (slow) */
	uint64_t	lh_max_memory;
	int     	lh_next_memory_check;	/* Number of splits before check */
	int       	lh_next_iterator;
	lh_iterator_s	lh_iterators[MAX_ITERATORS_OUTSTANDING];
	lh_stats_t	lh_stats;
	ops_tracker_t	lh_ops_tracker;
	allocator_t	lh_data_alloc;		/* Data records */
	allocator_t	lh_bucket_alloc;	/* Main buckets */
	allocator_t	lh_overflow_alloc;	/* Overflow buckets */
};

#endif /* LINEAR_HASH_TYPES_H */
