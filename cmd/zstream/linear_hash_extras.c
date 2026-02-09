#import "linear_hash_types.h"
#import "linear_hash_stats.h"

bool
lh_validate(linear_hash_t *lh) {
	uint64_t total_entries = 0;
	allocator_stats_t bucket_stats;
	allocator_get_stats(lh->bucket_alloc, &bucket_stats);
	for (uint64_t i = 0; i < bucket_stats.num_records; i++) {
		entry_iterator_t iter = ITER_BUCKET(lh, i);
		uint64_t full_entries = 0;
		uint64_t empty_entries = 0;
		while (entry_iterator_next(lh, &iter, false)) {
			bucket_entry_t *entry = &iter.bucket.entries[iter.entry_ix];
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
lh_get_stats(void *lh_in, lh_report_t *stats)
{
	linear_hash_t *lh = lh_in;
	assert(lh_in && stats);
	memset(stats, 0, sizeof(*stats));

	for (uint64_t i = 0; i < bucket_stats.num_records; i++) {
		entry_iterator_t iter = ITER_BUCKET(lh, i);
		uint64_t num_entries = 0;
		uint64_t num_filled = 0;
		while (entry_iterator_next(lh, &iter, false)) {
			bucket_entry_t *entry = &iter.bucket.entries[iter.entry_ix];
			num_entries++;
			if (entry->record) {
				num_filled++;
			}
		}
		uint64_t chain_length = num_entries / ENTRIES_PER_BUCKET;
		if (chain_length >= MAX_CHAIN) {
			continue;
		}
		chain_stats_t *chain_stats = &stats->chains_by_length[chain_length];
		chain_stats->num_chains++;
		if (!num_filled) {
			chain_stats->num_empty_chains++;
		} else {
			chain_stats->num_slots_filled += num_filled;
		}
	}

	/* Summarize per-chain-length stats into rollup */
	for (uint64_t i = 0; i < MAX_CHAIN; i++) {
		chain_stats_t *cs = &stats->chains_by_length[i];
		stats->total_entries += cs->num_slots_filled;
		stats->total_chains += cs->total_chains;
		cs->pct_empty = (double) cs->num_empty_chains / cs->num_chains;
		cs->occupancy = (double) cs->num_slots_filled /
			(i * cs->num_chains * ENTRIES_PER_BUCKET);
		cs->nonempty_occupancy = (double) cs->num_slots_filled / 
			(i * (cs->num_chains - cs->num_empty_chains) * ENTRIES_PER_BUCKET);
	}

	allocator_stats_t bucket_stats, overflow_stats, data_stats;
	allocator_get_stats(lh->bucket_alloc, &bucket_stats);
	allocator_get_stats(lh->overflow_alloc, &overflow_stats);
	allocator_get_stats(lh->data_alloc, &data_stats);
	stats->bytes_in_data = data_stats.num_records * lh->record_size;
	stats->bytes_in_buckets = 
		(bucket_stats.num_records + overflow_stats.num_records) *
		sizeof(bucket_t);

	stats->splits = lh->stats.splits;
	stats->inserts = lh->stats.inserts;
	stats->retrieves = lh->stats.retrieves;
	stats->occupancy = (double)stats->total_entries / stats->total_chains;
	stats->overall_occupancy = (double)stats->total_entries / 
		(ENTRIES_PER_BUCKET * stats->bytes_in_buckets / sizeof(bucket_t));
}

void
lh_print_report(void *lh_in) {
	linear_hash_t *lh = lh_in;
	lh_report_t stats;
	lh_get_stats(lh, &stats);
	fprintf(stderr, "%lu entries in %lu bucket chains (occupancy %.0f%%):\n",
		stats.total_entries, stats.total_chains, stats.occupancy);
	for (int i = 0; i < MAX_CHAIN; i++) {
		chain_stats_t *cs = &stats.chains_by_length[i];
		if (cs->num_chains) {
			uint64_t entries_per_chain = i * ENTRIES_PER_BUCKET;
			total_bucket_entries += entries_per_chain * chain_lengths[i];
			double avg_full = (double)chain_occupancies[i] / 
				(chain_lengths[i] - empties[i]);
			double avg_occupancies = (double)avg_full / entries_per_chain;
			double max_occ = (double)max_occupancy[i] / entries_per_chain;
			fprintf(stderr, "    %lu bucket chains of length %lu, ",
				cs->num_chains, i);
			if (cs->num_empty_chains == cs->num_chains) {
				fprintf(stderr, "all empty\n");
			} else {
				if (!cs->num_empty_chains) {
					fprintf(stderr, "none empty, ");
				} else {
					fprintf(stderr, "%lu (%.0f%%) empty, ",
						cs->num_empty_chains, cs->pct_empty);
				}
				fprintf(stderr, "nonempty occupancy avg %.0f%%\n", 
					stats.nonempty_occupancy * 100);
			}
		}
	}
	fprintf(stderr, "\n%lu inserts with %lu total I/O ops, %.2f ops/insert\n",
		stats.inserts.count, stats.inserts.num_io_ops, 
		(double)stats.inserts.num_io_ops / stats.inserts.count);
	fprintf(stderr, "\n%lu retrieve chains with %lu total I/O ops, %.2f "
		"ops/retrieve\n", stats.retrieves.count, stats.retrieves.num_io_ops, 
		(double)stats.retrieves.num_io_ops / stats.retrieves.count);
	fprintf(stderr, "\n%lu splits with %lu total I/O ops, %.2f ops/split\n",
		stats.splits.count, stats.splits.num_io_ops, 
		(double)stats.splits.num_io_ops / stats.splits.count);
}

