// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#ifndef _ZSTREAM_HASH_EXTRAS_H
#define _ZSTREAM_HASH_EXTRAS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zstream_hash_impl.h"

#define	START_VALIDATION(lh) 						\
	boolean_t started_ok = B_TRUE;					\
	if (lh->lh_validate) {						\
		started_ok = lh_validate(lh);				\
	}

#define	END_VALIDATION(lh)						\
	if (lh->lh_validate && !lh_validate(lh) && started_ok) {	\
		warnx("%s broke the hash table", __func__);		\
	}

/*
 * MAX_BUCKET_CHAINS is a reporting/stats limitation, not an implementation
 * limitation. There is no limit on the actual length of bucket chains.
 */
#define	MAX_BUCKET_CHAINS 16

typedef struct {
	uint64_t	os_count;	/* Inserts, splits, etc. */
	uint64_t	os_num_io_ops;	/* Total # of reads/writes */
} op_stats_t;

typedef struct {
	op_stats_t	lhs_inserts;		/* Inserts from caller */
	op_stats_t	lhs_retrieves;		/* Retrieves from caller */
	op_stats_t	lhs_splits;		/* Splits, internal */
} lh_stats_t;

typedef struct {
	op_stats_t	*ot_stat_bin;
	uint64_t	ot_start_ops;
	uint64_t	ot_latest_ops;
} ops_tracker_t;

typedef struct {
	uint64_t	bs_num_chains;
	uint64_t	bs_num_empty_chains;
	uint64_t	bs_num_slots_filled;
	double		bs_pct_empty;
	double		bs_occupancy;          /* Full slots / All slots */
	double		bs_nonempty_occupancy;
} bucket_stats_t;

typedef struct lh_report {
	bucket_stats_t	lr_chains_by_length[MAX_BUCKET_CHAINS];
	uint64_t	lr_total_entries;
	uint64_t	lr_total_chains;
	uint64_t	lr_bytes_in_data;
	uint64_t	lr_bytes_in_buckets;
	op_stats_t	lr_splits;
	op_stats_t	lr_inserts;
	op_stats_t	lr_retrieves;
	double		lr_occupancy;        /* Entries / number of chains */
	double		lr_overall_occupancy;    /* Full slots / Total slots */
} lh_report_t;

boolean_t
entry_iterator_next(entry_iterator_t *iter, boolean_t extend);

boolean_t
lh_validate(linear_hash_t *lh);

uint64_t
total_io_ops(linear_hash_t *lh);

void
begin_ops_tracking(linear_hash_t *lh, op_stats_t *bin);

void
update_ops_tracking(linear_hash_t *lh);

void
complete_ops_tracking(linear_hash_t *lh);

void
lh_get_stats(linear_hash_t *lh, lh_report_t *stats);

void
lh_print_stats(linear_hash_t *lh);

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_HASH_EXTRAS_H */
