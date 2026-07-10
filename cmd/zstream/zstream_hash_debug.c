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
#include <string.h>
#include "zstream_hash_impl.h"
#include "zstream_hash_stats.h"
#include "zstream_hash_debug.h"

boolean_t
lh_validate(linear_hash_t *lh) {
	uint64_t total_entries = 0;
	allocator_stats_t bucket_stats;
	allocator_get_stats(lh->lh_bucket_alloc, &bucket_stats);
	for (uint64_t i = 0; i < bucket_stats.as_num_records; i++) {
		entry_iterator_t iter = ITER_BUCKET(lh, i);
		uint64_t full_entries = 0;
		uint64_t empty_entries = 0;
		while (entry_iterator_next(&iter, B_FALSE)) {
			bucket_entry_t *entry = &iter.ei_bucket.b_entries[iter.ei_entry_ix];
			if (entry->be_record) {
				if (empty_entries) {
					fprintf(stderr, "validate: bucket %lu has "
						"uncompacted entries.\nEntry %ld is the "
						"first after an empty entry.\n", i, 
						iter.ei_entry_ix);
					return B_FALSE;
				}
				full_entries++;
			} else {
				empty_entries++;
			}
		}
		total_entries += full_entries;
	}
	if (total_entries != lh->lh_stats.lhs_num_entries) {
		fprintf(stderr, "validate: linear hash is supposed to have %lu "
			"entries, but actually has %lu.\n", lh->lh_stats.lhs_num_entries,
			total_entries);
		return B_FALSE;
	}
	return B_TRUE;
}

void
lh_get_stats(linear_hash_t *lh, lh_report_t *stats)
{
	assert(lh && stats);
	memset(stats, 0, sizeof(*stats));
	
	allocator_stats_t bucket_stats, overflow_stats, data_stats;
	allocator_get_stats(lh->lh_bucket_alloc, &bucket_stats);
	allocator_get_stats(lh->lh_overflow_alloc, &overflow_stats);
	allocator_get_stats(lh->lh_data_alloc, &data_stats);
	stats->lr_bytes_in_data = data_stats.as_num_records * lh->lh_record_size;
	stats->lr_bytes_in_buckets =
		(bucket_stats.as_num_records + overflow_stats.as_num_records) *
		sizeof(bucket_t);

	for (uint64_t i = 0; i < bucket_stats.as_num_records; i++) {
		entry_iterator_t iter = ITER_BUCKET(lh, i);
		uint64_t num_entries = 0;
		uint64_t num_filled = 0;
		while (entry_iterator_next(&iter, B_FALSE)) {
			bucket_entry_t *entry = &iter.ei_bucket.b_entries[iter.ei_entry_ix];
			num_entries++;
			if (entry->be_record) {
				num_filled++;
			}
		}
		uint64_t chain_length = num_entries / ENTRIES_PER_BUCKET;
		if (chain_length >= MAX_BUCKET_CHAIN) {
			continue;
		}
		bucket_stats_t *chain_stats = &stats->lr_chains_by_length[chain_length];
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
		bs->bs_pct_empty = (double) bs->bs_num_empty_chains / bs->bs_num_chains;
		bs->bs_occupancy = (double) bs->bs_num_slots_filled /
			(i * bs->bs_num_chains * ENTRIES_PER_BUCKET);
		bs->bs_nonempty_occupancy = (double) bs->bs_num_slots_filled /
			(i * (bs->bs_num_chains - bs->bs_num_empty_chains) * ENTRIES_PER_BUCKET);
	}

	stats->lr_splits = lh->lh_stats.lhs_splits;
	stats->lr_inserts = lh->lh_stats.lhs_inserts;
	stats->lr_retrieves = lh->lh_stats.lhs_retrieves;
	stats->lr_occupancy = (double)stats->lr_total_entries / (stats->lr_total_chains * ENTRIES_PER_BUCKET);
	stats->lr_overall_occupancy = (double)stats->lr_total_entries /
		(ENTRIES_PER_BUCKET * stats->lr_bytes_in_buckets / sizeof(bucket_t));
}

void
lh_print_stats(linear_hash_t *lh) {
	lh_report_t stats;
	lh_get_stats(lh, &stats);
	fprintf(stderr, "%lu entries in %lu bucket chains (occupancy %.0f%%):\n",
		stats.lr_total_entries, stats.lr_total_chains, 100 * stats.lr_occupancy);
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
						cs->bs_num_empty_chains, 100 * cs->bs_pct_empty);
				}
				fprintf(stderr, "nonempty occupancy avg %.0f%%\n", 
					cs->bs_nonempty_occupancy * 100);
			}
		}
	}
	fprintf(stderr, "%lu inserts with %lu total I/O ops, %.2f ops/insert\n",
		stats.lr_inserts.os_count, stats.lr_inserts.os_num_io_ops,
		(double)stats.lr_inserts.os_num_io_ops / stats.lr_inserts.os_count);
	fprintf(stderr, "%lu retrieve chains with %lu total I/O ops, %.2f "
		"ops/retrieve\n", stats.lr_retrieves.os_count, stats.lr_retrieves.os_num_io_ops,
		(double)stats.lr_retrieves.os_num_io_ops / stats.lr_retrieves.os_count);
	fprintf(stderr, "%lu splits with %lu total I/O ops, %.2f ops/split\n",
		stats.lr_splits.os_count, stats.lr_splits.os_num_io_ops,
		(double)stats.lr_splits.os_num_io_ops / stats.lr_splits.os_count);
}

uint64_t
lh_get_mem_highwater(linear_hash_t *lh) {
	return lh->lh_stats.lhs_mem_highwater;
}

uint64_t
total_io_ops(linear_hash_t *lh) {
	allocator_stats_t data, bucket, over;
	allocator_get_stats(lh->lh_data_alloc, &data);
	allocator_get_stats(lh->lh_bucket_alloc, &bucket);
	allocator_get_stats(lh->lh_overflow_alloc, &over);
	return data.as_num_ops + bucket.as_num_ops + over.as_num_ops;
}

void
begin_ops_tracking(linear_hash_t *lh, op_stats_t *bin) {
	if (lh->lh_ops_tracker.ot_stat_bin) {
		complete_ops_tracking(lh);
	}
	lh->lh_ops_tracker.ot_stat_bin = bin;
	lh->lh_ops_tracker.ot_start_ops = total_io_ops(lh);
}

void
update_ops_tracking(linear_hash_t *lh){
	if (lh->lh_ops_tracker.ot_stat_bin) {
		lh->lh_ops_tracker.ot_latest_ops = total_io_ops(lh);
	}
}

void
complete_ops_tracking(linear_hash_t *lh) {
	ops_tracker_t *tracker = &lh->lh_ops_tracker;
	if (tracker->ot_stat_bin) {
		update_ops_tracking(lh);
		tracker->ot_stat_bin->os_count++;
		tracker->ot_stat_bin->os_num_io_ops += tracker->ot_latest_ops - tracker->ot_start_ops;
	}
}
