// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the Common
 * Development and Distribution License ("CDDL"), version 1.0. You may only use
 * this file in accordance with the terms of version 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this source. A
 * copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#include <assert.h>
#include <err.h>
#include <string.h>
#include "zstream_hash_impl.h"

#ifdef LH_STATS_AND_VALIDATION

/*
 * Validate the entire hash table. Returns B_TRUE if valid and B_FALSE if
 * not valid.
 */
boolean_t
lh_validate(linear_hash_t *lh)
{
	uint64_t total_entries = 0;
	uint64_t total_top_level_entries = 0;
	boolean_t warned = B_FALSE;
	allocator_stats_t bucket_stats =
	    allocator_get_stats(lh->lh_bucket_alloc);

	for (uint64_t i = 0; i < bucket_stats.as_num_records; i++) {

		entry_iterator_t iter = ITER_BUCKET(lh, i);
		uint64_t full_entries = 0;
		uint64_t empty_entries = 0;
		uint64_t top_level_entries = 0;
		bucket_entry_t *entry;

		while ((entry = entry_iterator_next(&iter, B_FALSE))) {
			if (entry->be_record) {
				if (empty_entries != 0) {
					warnx("bucket %llu has uncompacted "
					    "entries", (u_longlong_t)i);
					warned = B_TRUE;
				}
				record_ix_t bfh =
				    bucket_for_hash(lh, entry->be_hash);
				if (bfh != i) {
					warnx("bucket %llu contains hash %llx, "
					    "which should be in bucket %llu",
					    (u_longlong_t)i,
					    (u_longlong_t)entry->be_hash,
					    (u_longlong_t)bfh);
					warned = B_TRUE;
				}
				full_entries++;
				if (!iter.ei_in_overflow)
					top_level_entries++;
			} else {
				empty_entries++;
			}
		}

		total_entries += full_entries;
		total_top_level_entries += top_level_entries;
	}
	if (total_entries != lh->lh_num_entries) {
		warnx("linear hash is supposed to have %llu entries, but "
		    "actually has %llu", (u_longlong_t)lh->lh_num_entries,
		    (u_longlong_t)total_entries);
		warned = B_TRUE;
	}
	if (total_top_level_entries != lh->lh_num_top_level_entries) {
		warnx("linear hash is supposed to have %llu top-level entries, "
		    "but actually has %llu",
		    (u_longlong_t)lh->lh_num_top_level_entries,
		    (u_longlong_t)total_top_level_entries);
		warned = B_TRUE;
	}
	return (!warned);
}

void
lh_get_stats(linear_hash_t *lh, lh_report_t *stats)
{
	VERIFY(lh != NULL && stats != NULL);
	memset(stats, 0, sizeof (*stats));

	allocator_stats_t bucket = allocator_get_stats(lh->lh_bucket_alloc);
	allocator_stats_t overflow = allocator_get_stats(lh->lh_overflow_alloc);
	allocator_stats_t data = allocator_get_stats(lh->lh_data_alloc);
	stats->lr_bytes_in_data = data.as_num_records * lh->lh_record_size;
	stats->lr_bytes_in_buckets = sizeof (bucket_t) *
	    (bucket.as_num_records + overflow.as_num_records);

	for (uint64_t i = 0; i < bucket.as_num_records; i++) {

		entry_iterator_t iter = ITER_BUCKET(lh, i);
		uint64_t num_entries = 0;
		uint64_t num_filled = 0;

		bucket_entry_t *entry;
		while ((entry = entry_iterator_next(&iter, B_FALSE))) {
			num_entries++;
			if (entry->be_record) {
				num_filled++;
			}
		}
		uint64_t chain_length = num_entries / ENTRIES_PER_BUCKET;
		if (chain_length >= MAX_BUCKET_CHAIN) {
			continue;
		}
		bucket_stats_t *chain_stats =
		    &stats->lr_chains_by_length[chain_length];
		chain_stats->bs_num_chains++;
		if (!num_filled) {
			chain_stats->bs_num_empty_chains++;
		} else {
			chain_stats->bs_num_slots_filled += num_filled;
		}
	}

	/* Summarize per-chain-length stats into rollup */
	for (uint64_t i = 0; i < MAX_BUCKET_CHAIN; i++) {
		bucket_stats_t *bs = &stats->lr_chains_by_length[i];
		stats->lr_total_entries += bs->bs_num_slots_filled;
		stats->lr_total_chains += bs->bs_num_chains;
		bs->bs_pct_empty =
		    (double) bs->bs_num_empty_chains / bs->bs_num_chains;
		bs->bs_occupancy = (double) bs->bs_num_slots_filled /
			(i * bs->bs_num_chains * ENTRIES_PER_BUCKET);
		bs->bs_nonempty_occupancy = (double) bs->bs_num_slots_filled /
		    (i * (bs->bs_num_chains - bs->bs_num_empty_chains) *
		    ENTRIES_PER_BUCKET);
	}

	stats->lr_splits = lh->lh_stats.lhs_splits;
	stats->lr_inserts = lh->lh_stats.lhs_inserts;
	stats->lr_retrieves = lh->lh_stats.lhs_retrieves;

	stats->lr_occupancy = (double)lh->lh_num_top_level_entries /
	    (stats->lr_total_chains * ENTRIES_PER_BUCKET);
	stats->lr_overall_occupancy = (double)lh->lh_num_entries /
	    (ENTRIES_PER_BUCKET * stats->lr_bytes_in_buckets/sizeof (bucket_t));
}

void
lh_print_stats(linear_hash_t *lh)
{
	lh_report_t stats;
	lh_get_stats(lh, &stats);
	fprintf(stderr, "%lu entries in %lu chains (occupancy %.0f%%):\n",
	    stats.lr_total_entries, stats.lr_total_chains,
	    100 * stats.lr_occupancy);
	for (int i = 0; i < MAX_BUCKET_CHAIN; i++) {
		bucket_stats_t *cs = &stats.lr_chains_by_length[i];
		if (cs->bs_num_chains) {
			fprintf(stderr, "    %lu bucket chains of length %d, ",
			    cs->bs_num_chains, i);
			if (cs->bs_num_empty_chains == cs->bs_num_chains) {
				fprintf(stderr, "all empty\n");
			} else {
				if (!cs->bs_num_empty_chains) {
					fprintf(stderr, "none empty, ");
				} else {
					fprintf(stderr, "%lu (%.0f%%) empty, ",
					    cs->bs_num_empty_chains,
					    100 * cs->bs_pct_empty);
				}
				fprintf(stderr,
				    "nonempty occupancy avg %.0f%%\n",
				    cs->bs_nonempty_occupancy * 100);
			}
		}
	}
	uint64_t insert_ops = stats.lr_inserts.os_ops_mem +
	    stats.lr_inserts.os_ops_disk;
	uint64_t retrieve_ops = stats.lr_retrieves.os_ops_mem +
	    stats.lr_retrieves.os_ops_disk;
	uint64_t split_ops = stats.lr_splits.os_ops_mem +
	    stats.lr_splits.os_ops_disk;
	fprintf(stderr, "%lu inserts with %lu total I/O ops, %.2f ops/insert\n",
	    stats.lr_inserts.os_count, insert_ops,
	    (double)insert_ops / stats.lr_inserts.os_count);
	fprintf(stderr, "%lu retrieve chains with %lu total I/O ops, %.2f "
	    "ops/retrieve\n", stats.lr_retrieves.os_count,
	    retrieve_ops, (double)retrieve_ops / stats.lr_retrieves.os_count);
	fprintf(stderr, "%lu splits with %lu total I/O ops, %.2f ops/split\n",
	    stats.lr_splits.os_count, split_ops,
	    (double)split_ops / stats.lr_splits.os_count);
}

op_stats_t
total_io_ops(linear_hash_t *lh)
{
	allocator_stats_t data = allocator_get_stats(lh->lh_data_alloc);
	allocator_stats_t bucket = allocator_get_stats(lh->lh_bucket_alloc);
	allocator_stats_t over = allocator_get_stats(lh->lh_overflow_alloc);
	op_stats_t stats = {
		.os_ops_mem = data.as_io_ops_mem + bucket.as_io_ops_mem +
		    over.as_io_ops_mem,
		.os_ops_disk = data.as_io_ops_disk + bucket.as_io_ops_disk +
		    over.as_io_ops_disk
	};
	return (stats);
}

static op_stats_t
delta_ops(op_stats_t *start, op_stats_t *end)
{
	op_stats_t delta = {
		.os_ops_mem = end->os_ops_mem - start->os_ops_mem,
		.os_ops_disk = end->os_ops_disk - start->os_ops_disk
	};
	return (delta);
}
void
begin_ops_tracking(linear_hash_t *lh, op_stats_t *bin)
{
	if (lh->lh_ops_tracker.ot_stat_bin) {
		complete_ops_tracking(lh);
	}
	lh->lh_ops_tracker.ot_stat_bin = bin;
	lh->lh_ops_tracker.ot_start_ops = total_io_ops(lh);
}

void
update_ops_tracking(linear_hash_t *lh)
{
	if (lh->lh_ops_tracker.ot_stat_bin) {
		lh->lh_ops_tracker.ot_latest_ops = total_io_ops(lh);
	}
}

void
complete_ops_tracking(linear_hash_t *lh)
{
	ops_tracker_t *tracker = &lh->lh_ops_tracker;
	if (tracker->ot_stat_bin) {
		update_ops_tracking(lh);
		op_stats_t delta = delta_ops(&tracker->ot_start_ops,
		    &tracker->ot_latest_ops);
		tracker->ot_stat_bin->os_count++;
		tracker->ot_stat_bin->os_ops_mem += delta.os_ops_mem;
		tracker->ot_stat_bin->os_ops_disk += delta.os_ops_disk;
		tracker->ot_stat_bin = NULL;
	}
}

#endif  /* LH_STATS_AND_VALIDATION */
