#ifndef LINEAR_HASH_STATS_H
#define LINEAR_HASH_STATS_H

#include "linear_hash.h"

#define MAX_CHAIN 16

typedef struct {
  uint64_t    num_chains;
  uint64_t    num_empty_chains;
  uint64_t    num_slots_filled;
  double      pct_empty;
  double      occupancy;          /* Full slots / Total slots */
  double      nonempty_occupancy; /* Full slots / Slots in nonempty chains */
} chain_stats;

typedef struct {
  uint64_t  count;
  uint64_t  num_io_ops;
} op_stats;

typedef struct {
  chain_stats   chains_by_length[MAX_CHAIN];
  uint64_t    	total_entries;
  uint64_t    	total_chains;
  uint64_t    	bytes_in_data;
  uint64_t    	bytes_in_buckets;
  op_stats      splits;
  op_stats      inserts;
  op_stats      retrieves;
  double      	occupancy;        /* Entries / number of chains */
  double      	overall_occupancy;    /* Full slots / Total slots */
} lh_report;

void
lh_get_stats(linear_hash lh, lh_report *stats);

void
lh_print_stats(linear_hash lh);

uint64_t
lh_get_mem_highwater(linear_hash lh);

#endif /* LINEAR_HASH_STATS_H */
