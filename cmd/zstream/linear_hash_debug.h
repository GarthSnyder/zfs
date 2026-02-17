#include "linear_hash_impl.h"
#include "linear_hash_stats.h"

#define ITER_BUCKET(lh, bucket) {					\
		.lh = lh,						\
		.alloc = lh->bucket_alloc,				\
		.bucket_ix = bucket,					\
		.entry_ix = -1						\
	}

#define CHECKED(type, expr, doing_what) 				\
	type ret = expr;						\
	if (ret < 0) {							\
		fprintf(stderr, "Error while %s.\n", doing_what);	\
		exit(1);						\
	}

#define START_VALIDATION(lh) 						\
	boolean_t started_ok;						\
	if (lh->validate) {						\
		started_ok = lh_validate(lh);				\
	}

#define END_VALIDATION(lh)						\
	if (lh->validate && !lh_validate(lh) && started_ok) {		\
		fprintf(stderr, "%s broke the hash table\n", __func__);	\
	}

bool
entry_iterator_next(entry_iterator *iter, bool extend);

bool
lh_validate(linear_hash_t lh);

uint64_t
total_io_ops(linear_hash_t lh);

void
begin_ops_tracking(linear_hash_t lh, op_stats *bin);

void
update_ops_tracking(linear_hash_t lh);

void
complete_ops_tracking(linear_hash_t lh);
