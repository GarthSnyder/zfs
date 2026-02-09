#ifndef LINEAR_HASH_TYPES_H
#define LINEAR_HASH_TYPES_H

#define ENTRIES_PER_BUCKET			6
#define MAX_ITERATORS_OUTSTANDING	4

/* Entry in a bucket: hash value + locator to data */
typedef struct {
	uint64_t  hash;
	record_ix record;  /* index of actual data */
} bucket_entry_t;

/* Bucket structure: fixed array of entries + overflow pointer */
typedef struct {
	bucket_entry_t	entries[ENTRIES_PER_BUCKET];
	record_ix		overflow;  /* 0 if no overflow */
} bucket_t;

/* Internal iterator for bucket entries */
typedef struct {
	struct linear_hash	*lh
	allocator			alloc;
	record_ix			bucket_ix;	/* -1 == overflow not yet assigned */
	record_ix			entry_ix;   /* -1 == bucket not yet retrieved */
	bucket_t			bucket;		/* Working copy of allocator version */
	bool				dirty;		/* needs writeback */
} entry_iterator_t;

/* Iterator for retrieving records by hash */
typedef struct {
	uint64_t			hash;
	entry_iterator_t	entry_iterator;
} lh_iterator_t;

typedef struct {
	uint64_t	count;
	uint64_t	num_io_ops;
} op_stats_t;

typedef struct {
	uint64_t	num_entries;	/* total entries in table */
	op_stats_t	inserts;		/* Inserts from external client */
	op_stats_t	retrieves;		/* Retrieves from external client */
	op_stats_t	splits;			/* Splits, generated internally */
} lh_stats_t;

/* Hash table structure */
typedef struct linear_hash 
{
	size_t			record_size;
	uint8_t			hash_suffix_length;	/* hashing granularity below split */
	record_ix		split_pointer;    	/* next bucket to split */
	bool			validate;			/* Validation per operation (slow) */
	uint64_t		max_memory;
	int     		next_memory_check;  /* Number of splits before check */
	int       		next_iterator;
	lh_iterator_t	iterators[MAX_ITERATORS_OUTSTANDING];
	lh_stats_t		stats;
	allocator		data_alloc;			/* data records */
	allocator		bucket_alloc;		/* main buckets */
	allocator		overflow_alloc;		/* overflow buckets */
} linear_hash_t;

#endif /* LINEAR_HASH_TYPES_H */