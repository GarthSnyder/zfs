#include <assert.h>
#include <string.h>
#include "linear_hash_types.h"
#include "linear_hash_stats.h"
#include "linear_hash_extras.h"

bool
lh_validate(linear_hash_t lh) {
	uint64_t total_entries = 0;
	allocator_stats bucket_stats;
	allocator_get_stats(lh->bucket_alloc, &bucket_stats);
	for (uint64_t i = 0; i < bucket_stats.num_records; i++) {
		entry_iterator iter = ITER_BUCKET(lh, i);
		uint64_t full_entries = 0;
		uint64_t empty_entries = 0;
		while (entry_iterator_next(&iter, false)) {
			bucket_entry *entry = &iter.bucket.entries[iter.entry_ix];
			if (entry->record) {
				if (empty_entries) {
					fprintf(stderr, "validate: bucket %lu has "
						"uncompacted entries.\nEntry %lu is the "
						"first after an empty entry.\n", i, 
						iter.entry_ix);
					return false;
				}
				full_entries++;
			} else {
				empty_entries++;
			}
		}
		total_entries += full_entries;
	}
	if (total_entries != lh->stats.num_entries) {
		fprintf(stderr, "validate: linear hash is supposed to have %lu "
			"entries, but actually has %lu.\n", lh->stats.num_entries,
			total_entries);
		return false;
	}
	return true;
}

void
lh_get_stats(linear_hash_t lh, lh_report_t *stats)
{
	assert(lh && stats);
	memset(stats, 0, sizeof(*stats));
	
	allocator_stats bucket_stats, overflow_stats, data_stats;
	allocator_get_stats(lh->bucket_alloc, &bucket_stats);
	allocator_get_stats(lh->overflow_alloc, &overflow_stats);
	allocator_get_stats(lh->data_alloc, &data_stats);
	stats->bytes_in_data = data_stats.num_records * lh->record_size;
	stats->bytes_in_buckets = 
		(bucket_stats.num_records + overflow_stats.num_records) *
		sizeof(bucket);

	for (uint64_t i = 0; i < bucket_stats.num_records; i++) {
		entry_iterator iter = ITER_BUCKET(lh, i);
		uint64_t num_entries = 0;
		uint64_t num_filled = 0;
		while (entry_iterator_next(&iter, false)) {
			bucket_entry *entry = &iter.bucket.entries[iter.entry_ix];
			num_entries++;
			if (entry->record) {
				num_filled++;
			}
		}
		uint64_t chain_length = num_entries / ENTRIES_PER_BUCKET;
		if (chain_length >= MAX_CHAIN) {
			continue;
		}
		chain_stats *chain_stats = &stats->chains_by_length[chain_length];
		chain_stats->num_chains++;
		if (!num_filled) {
			chain_stats->num_empty_chains++;
		} else {
			chain_stats->num_slots_filled += num_filled;
		}
	}

	/* Summarize per-chain-length stats into rollup */
	for (uint64_t i = 0; i < MAX_CHAIN; i++) {
		chain_stats *cs = &stats->chains_by_length[i];
		stats->total_entries += cs->num_slots_filled;
		stats->total_chains += cs->num_chains;
		cs->pct_empty = (double) cs->num_empty_chains / cs->num_chains;
		cs->occupancy = (double) cs->num_slots_filled /
			(i * cs->num_chains * ENTRIES_PER_BUCKET);
		cs->nonempty_occupancy = (double) cs->num_slots_filled / 
			(i * (cs->num_chains - cs->num_empty_chains) * ENTRIES_PER_BUCKET);
	}

	stats->splits = lh->stats.splits;
	stats->inserts = lh->stats.inserts;
	stats->retrieves = lh->stats.retrieves;
	stats->occupancy = (double)stats->total_entries / stats->total_chains;
	stats->overall_occupancy = (double)stats->total_entries / 
		(ENTRIES_PER_BUCKET * stats->bytes_in_buckets / sizeof(bucket));
}

void
lh_print_stats(linear_hash_t lh) {
	lh_report_t stats;
	lh_get_stats(lh, &stats);
	fprintf(stderr, "%lu entries in %lu bucket chains (occupancy %.0f%%):\n",
		stats.total_entries, stats.total_chains, stats.occupancy);
	for (int i = 0; i < MAX_CHAIN; i++) {
		chain_stats *cs = &stats.chains_by_length[i];
		if (cs->num_chains) {
			fprintf(stderr, "    %lu bucket chains of length %d, ",
				cs->num_chains, i);
			if (cs->num_empty_chains == cs->num_chains) {
				fprintf(stderr, "all empty\n");
			} else {
				if (!cs->num_empty_chains) {
					fprintf(stderr, "none empty, ");
				} else {
					fprintf(stderr, "%lu (%.0f%%) empty, ",
						cs->num_empty_chains, 100 * cs->pct_empty);
				}
				fprintf(stderr, "nonempty occupancy avg %.0f%%\n", 
					cs->nonempty_occupancy * 100);
			}
		}
	}
	fprintf(stderr, "%lu inserts with %lu total I/O ops, %.2f ops/insert\n",
		stats.inserts.count, stats.inserts.num_io_ops, 
		(double)stats.inserts.num_io_ops / stats.inserts.count);
	fprintf(stderr, "%lu retrieve chains with %lu total I/O ops, %.2f "
		"ops/retrieve\n", stats.retrieves.count, stats.retrieves.num_io_ops, 
		(double)stats.retrieves.num_io_ops / stats.retrieves.count);
	fprintf(stderr, "%lu splits with %lu total I/O ops, %.2f ops/split\n",
		stats.splits.count, stats.splits.num_io_ops, 
		(double)stats.splits.num_io_ops / stats.splits.count);
}

uint64_t
lh_get_mem_highwater(linear_hash_t lh) {
	return lh->stats.mem_highwater;
}

uint64_t
total_io_ops(linear_hash_t lh) {
	allocator_stats data, bucket, over;
	allocator_get_stats(lh->data_alloc, &data);
	allocator_get_stats(lh->bucket_alloc, &bucket);
	allocator_get_stats(lh->overflow_alloc, &over);
	return data.num_ops + bucket.num_ops + over.num_ops;
}

void
begin_ops_tracking(linear_hash_t lh, op_stats *bin) {
	if (lh->ops_tracker.stat_bin) {
		complete_ops_tracking(lh);
	}
	lh->ops_tracker.stat_bin = bin;
	lh->ops_tracker.start_ops = total_io_ops(lh);
}

void
update_ops_tracking(linear_hash_t lh){
	if (lh->ops_tracker.stat_bin) {
		lh->ops_tracker.latest_ops = total_io_ops(lh);
	}
}

void
complete_ops_tracking(linear_hash_t lh) {
	ops_tracker *tracker = &lh->ops_tracker;
	if (tracker->stat_bin) {
		update_ops_tracking(lh);
		tracker->stat_bin->count++;
		tracker->stat_bin->num_io_ops += tracker->latest_ops - tracker->start_ops;
	}
}
