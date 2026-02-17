#include "linear_hash_impl.h"
#include "linear_hash_stats.h"

#define START_VALIDATION(lh) 						\
	boolean_t started_ok = B_TRUE;					\
	if (lh->lh_validate) {						\
		started_ok = lh_validate(lh);				\
	}

#define END_VALIDATION(lh)						\
	if (lh->lh_validate && !lh_validate(lh) && started_ok) {	\
		fprintf(stderr, "%s broke the hash table\n", __func__);	\
	}

bool
entry_iterator_next(entry_iterator_t *iter, bool extend);

bool
lh_validate(linear_hash_t lh);

uint64_t
total_io_ops(linear_hash_t lh);

void
begin_ops_tracking(linear_hash_t lh, op_stats_t *bin);

void
update_ops_tracking(linear_hash_t lh);

void
complete_ops_tracking(linear_hash_t lh);
