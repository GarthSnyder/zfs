// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

/*
 * Selftests for linear_hash_t, defined in zstream_hash.c
 *
 * The verification strategy is multiset equality against a shadow model.
 * Every inserted record's payload begins with a unique tag, and the rest of
 * the payload is a deterministic pattern derived from that tag. The model
 * is simply the list of (hash, tag) pairs inserted. To verify a table,
 * entries are grouped by hash value and each group is retrieved. Every
 * returned payload must be internally consistent and must match exactly one
 * not-yet-seen model entry, and the number of returned records must equal
 * the group size. Hashes never inserted must return nothing. (Inserted keys
 * always have bit 63 clear, which gives absent-probe tests an inexhaustible
 * supply of known-absent keys.)
 *
 * lh_validate() below complements that black-box view with a structural
 * audit of the table itself. It used to live in zstream_hash_extras.c; it
 * is kept here, and deliberately walks bucket chains with its own code
 * rather than with the table's own entry_iterator_t, so that a bug in the
 * iterator cannot hide itself from the validator.
 *
 * Beyond store-and-retrieve correctness, the tests exercise the table's
 * supra-allocator memory management: with small budgets, margins, and check
 * intervals (see lh_memory_margin and lh_mem_check_interval), the table
 * must reclaim memory from its three allocators in priority order (data
 * first, then overflow buckets, then main buckets) while remaining fully
 * correct and keeping total memory use bounded.
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zstream_selftest.h"
#include "zstream_util.h"
#include "zstream_hash_impl.h"

#define	ABSENT_BIT	(1ULL << 63)

/* A corrupt overflow index must not turn a chain walk into a hang */
#define	MAX_CHAIN_WALK	100000

typedef struct {
	uint64_t	ht_hash;
	uint64_t	ht_tag;
	boolean_t	ht_found;
} htest_entry_t;

typedef struct {
	size_t		hc_record_size;	/* >= 8; leading 8 bytes are tag */
	size_t		hc_max_memory;
	uint64_t	hc_count;
	uint64_t	hc_key_space;	/* 0 = unique-ish random keys */
	uint64_t	hc_fixed_mask;	/* Hash bits forced to hc_fixed_bits */
	uint64_t	hc_fixed_bits;
	uint64_t	hc_rng_stream;
	size_t		hc_margin;	/* lh_memory_margin; 0 = leave */
	int		hc_interval;	/* lh_mem_check_interval; 0 = leave */
	uint64_t	hc_spot_every;	/* Mid-insert probes; 0 = off */
	boolean_t	hc_no_disk;	/* Pass a NULL cache directory */
	boolean_t	hc_validate;	/* lh_validate() when done */
	boolean_t	hc_keep;	/* Return live table to caller */
} htest_config_t;

/*
 * The destination bucket for a hash, restated independently of
 * bucket_for_hash() in zstream_hash.c. Below the split pointer the mask is
 * one bit longer than above it.
 */
static uint64_t
expected_bucket(linear_hash_t *lh, uint64_t hash)
{
	uint64_t mask = (1ULL << lh->lh_hash_suffix_length) - 1;

	if ((hash & mask) < lh->lh_split_pointer)
		mask = (mask << 1) | 1;
	return (hash & mask);
}

/*
 * Structural audit of an entire table. Walks every top-level bucket and its
 * overflow chain directly through the allocators, and checks that:
 *
 * - entries are packed at the front of each bucket, with no live entry
 *   following an empty one (the scan for a free slot stops at the first
 *   zero be_record, so a gap would strand everything after it);
 * - every entry sits in the bucket its hash currently maps to;
 * - the total number of entries is what the caller inserted; and
 * - lh_num_top_level_entries and lh_num_top_level_buckets agree with what
 *   is actually on disk. Those two drive the split decision, so drift in
 *   either one silently stops the table from growing.
 *
 * Returns B_TRUE if the table is sound, warning about each problem found.
 */
static boolean_t
lh_validate(linear_hash_t *lh, uint64_t expected_entries)
{
	uint64_t total_entries = 0;
	uint64_t total_top_level = 0;
	boolean_t warned = B_FALSE;

	for (uint64_t b = 0; b < lh->lh_num_top_level_buckets; b++) {
		bucket_t bucket;
		boolean_t top_level = B_TRUE;
		uint64_t links = 0;

		allocator_retrieve(lh->lh_alloc.bucket, b, &bucket);
		for (;;) {
			boolean_t seen_empty = B_FALSE;
			for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
				bucket_entry_t *e = &bucket.b_entries[i];
				if (e->be_record == 0) {
					seen_empty = B_TRUE;
					continue;
				}
				if (seen_empty) {
					warnx("bucket %ju has uncompacted "
					    "entries", (uintmax_t)b);
					warned = B_TRUE;
				}
				uint64_t want = expected_bucket(lh,
				    e->be_hash);
				if (want != b) {
					warnx("bucket %ju contains hash %jx, "
					    "which belongs in bucket %ju",
					    (uintmax_t)b,
					    (uintmax_t)e->be_hash,
					    (uintmax_t)want);
					warned = B_TRUE;
				}
				total_entries++;
				if (top_level)
					total_top_level++;
			}
			if (bucket.b_overflow == 0)
				break;
			if (++links > MAX_CHAIN_WALK) {
				warnx("bucket %ju has a runaway overflow "
				    "chain", (uintmax_t)b);
				warned = B_TRUE;
				break;
			}
			allocator_retrieve(lh->lh_alloc.overflow,
			    bucket.b_overflow, &bucket);
			top_level = B_FALSE;
		}
	}

	if (total_entries != expected_entries) {
		warnx("linear hash is supposed to have %ju entries, but "
		    "actually has %ju", (uintmax_t)expected_entries,
		    (uintmax_t)total_entries);
		warned = B_TRUE;
	}
	if (total_top_level != lh->lh_num_top_level_entries) {
		warnx("linear hash is supposed to have %ju top-level entries, "
		    "but actually has %ju",
		    (uintmax_t)lh->lh_num_top_level_entries,
		    (uintmax_t)total_top_level);
		warned = B_TRUE;
	}
	if (total_top_level > total_entries) {
		warnx("more top-level entries (%ju) than entries (%ju)",
		    (uintmax_t)total_top_level, (uintmax_t)total_entries);
		warned = B_TRUE;
	}

	/*
	 * The table is 2^suffix buckets wide at the start of each split
	 * cycle and has grown by one bucket for each split performed since.
	 */
	uint64_t want_buckets = (1ULL << lh->lh_hash_suffix_length) +
	    lh->lh_split_pointer;
	if (lh->lh_num_top_level_buckets != want_buckets) {
		warnx("linear hash has %ju top-level buckets, but its split "
		    "state implies %ju",
		    (uintmax_t)lh->lh_num_top_level_buckets,
		    (uintmax_t)want_buckets);
		warned = B_TRUE;
	}
	return (!warned);
}

static void
fill_pattern(uint8_t *buf, size_t len, uint64_t tag)
{
	uint64_t x = selftest_mix64(selftest_seed ^ tag);
	for (size_t i = 0; i < len; i++) {
		if ((i & 7) == 0)
			x = selftest_mix64(x + i);
		buf[i] = (uint8_t)(x >> ((i & 7) << 3));
	}
}

static void
make_payload(uint8_t *buf, size_t record_size, uint64_t tag)
{
	memcpy(buf, &tag, sizeof (tag));
	fill_pattern(buf + sizeof (tag), record_size - sizeof (tag), tag);
}

static void
verify_payload(const uint8_t *buf, size_t record_size)
{
	uint64_t tag;

	memcpy(&tag, buf, sizeof (tag));
	uint8_t *whole = safe_malloc(record_size);
	make_payload(whole, record_size, tag);
	if (memcmp(whole, buf, record_size) != 0)
		errx(1, "retrieved payload with tag %ju is corrupted",
		    (uintmax_t)tag);
	free(whole);
}

static size_t
total_mem_used(linear_hash_t *lh)
{
	size_t total = 0;

	for (int i = 0; i < NUM_ALLOC; i++)
		total += allocator_memory_used(lh->lh_alloc.all[i]);
	return (total);
}

/*
 * Generate the workload described by a config: hc_count entries with
 * unique tags and keys drawn either at random or from a bounded key space
 * (to force duplicates), optionally with some hash bits pinned (to force
 * bucket collisions).
 */
static htest_entry_t *
generate_entries(const htest_config_t *cfg, selftest_rng_t *rng)
{
	htest_entry_t *entries =
	    safe_calloc(cfg->hc_count * sizeof (htest_entry_t));
	uint64_t *keys = NULL;

	if (cfg->hc_key_space > 0) {
		keys = safe_malloc(cfg->hc_key_space * sizeof (uint64_t));
		for (uint64_t i = 0; i < cfg->hc_key_space; i++)
			keys[i] = selftest_rng_next(rng) & ~ABSENT_BIT;
	}

	for (uint64_t i = 0; i < cfg->hc_count; i++) {
		uint64_t h;
		if (keys != NULL) {
			h = keys[selftest_rng_below(rng, cfg->hc_key_space)];
		} else {
			h = selftest_rng_next(rng) & ~ABSENT_BIT;
		}
		if (cfg->hc_fixed_mask != 0) {
			h = (h & ~cfg->hc_fixed_mask) |
			    (cfg->hc_fixed_bits & ~ABSENT_BIT &
			    cfg->hc_fixed_mask);
		}
		entries[i].ht_hash = h;
		entries[i].ht_tag = i + 1;
	}
	free(keys);
	return (entries);
}

/*
 * Retrieve one previously inserted entry at random. This function is used
 * to catch corruption early, in mid-workload, rather than only during the
 * final sweep.
 */
static void
spot_check(linear_hash_t *lh, htest_entry_t *entries, uint64_t n_inserted,
    selftest_rng_t *rng, uint8_t *buf)
{
	htest_entry_t *e = &entries[selftest_rng_below(rng, n_inserted)];
	lh_iterator_t *iter = lh_initiate_retrieve(lh, e->ht_hash);
	while (lh_retrieve_next(iter, buf)) {
		uint64_t tag;
		memcpy(&tag, buf, sizeof (tag));
		if (tag == e->ht_tag)
			return;
	}
	errx(1, "record with hash %jx (tag %ju) lost after %ju inserts",
	    (uintmax_t)e->ht_hash, (uintmax_t)e->ht_tag,
	    (uintmax_t)n_inserted);
}

static void
insert_entries(linear_hash_t *lh, htest_entry_t *entries, uint64_t count,
    uint64_t spot_every, selftest_rng_t *rng)
{
	uint8_t *buf = safe_malloc(lh->lh_record_size);

	for (uint64_t i = 0; i < count; i++) {
		make_payload(buf, lh->lh_record_size, entries[i].ht_tag);
		lh_insert(lh, entries[i].ht_hash, buf);
		if (spot_every != 0 && (i + 1) % spot_every == 0)
			spot_check(lh, entries, i + 1, rng, buf);
	}
	free(buf);
}

static int
htest_entry_cmp(const void *va, const void *vb)
{
	const htest_entry_t *a = va;
	const htest_entry_t *b = vb;

	if (a->ht_hash != b->ht_hash)
		return (a->ht_hash < b->ht_hash ? -1 : 1);
	if (a->ht_tag != b->ht_tag)
		return (a->ht_tag < b->ht_tag ? -1 : 1);
	return (0);
}

/*
 * The full multiset-equality sweep, plus absent-key probes. Sorts the
 * entries array as a side effect.
 */
static void
verify_table(linear_hash_t *lh, htest_entry_t *entries, uint64_t count,
    selftest_rng_t *rng)
{
	uint8_t *buf = safe_malloc(lh->lh_record_size);

	qsort(entries, count, sizeof (htest_entry_t), htest_entry_cmp);

	uint64_t i = 0;
	while (i < count) {
		uint64_t hash = entries[i].ht_hash;
		uint64_t j = i;
		while (j < count && entries[j].ht_hash == hash) {
			entries[j].ht_found = B_FALSE;
			j++;
		}

		uint64_t nfound = 0;
		lh_iterator_t *iter = lh_initiate_retrieve(lh, hash);
		while (lh_retrieve_next(iter, buf)) {
			verify_payload(buf, lh->lh_record_size);
			uint64_t tag;
			memcpy(&tag, buf, sizeof (tag));
			boolean_t matched = B_FALSE;
			for (uint64_t k = i; k < j; k++) {
				if (!entries[k].ht_found &&
				    entries[k].ht_tag == tag) {
					entries[k].ht_found = B_TRUE;
					matched = B_TRUE;
					break;
				}
			}
			if (!matched) {
				errx(1, "hash %jx returned unexpected or "
				    "duplicate record (tag %ju)",
				    (uintmax_t)hash, (uintmax_t)tag);
			}
			nfound++;
		}
		if (nfound != j - i) {
			errx(1, "hash %jx: expected %ju records, "
			    "retrieved %ju", (uintmax_t)hash,
			    (uintmax_t)(j - i), (uintmax_t)nfound);
		}
		i = j;
	}

	/* Keys that were never inserted must return nothing */
	for (int p = 0; p < 1000; p++) {
		uint64_t h = selftest_rng_next(rng) | ABSENT_BIT;
		lh_iterator_t *iter = lh_initiate_retrieve(lh, h);
		VERIFY(!lh_retrieve_next(iter, buf));
	}
	free(buf);
}

/*
 * Run a complete generate/insert/verify workload. If hc_keep is set, the
 * table is returned live (for extra caller-side assertions) and the caller
 * must lh_destroy() it; otherwise NULL is returned.
 */
static linear_hash_t *
run_hash_workload(const htest_config_t *cfg)
{
	size_t saved_margin = lh_memory_margin;
	int saved_interval = lh_mem_check_interval;
	selftest_rng_t rng;

	if (cfg->hc_margin != 0)
		lh_memory_margin = cfg->hc_margin;
	if (cfg->hc_interval != 0)
		lh_mem_check_interval = cfg->hc_interval;
	selftest_rng_init(&rng, cfg->hc_rng_stream);

	linear_hash_t *lh = lh_init(cfg->hc_record_size, cfg->hc_max_memory,
	    cfg->hc_no_disk ? NULL : selftest_scratch_dir());
	htest_entry_t *entries = generate_entries(cfg, &rng);

	insert_entries(lh, entries, cfg->hc_count, cfg->hc_spot_every, &rng);
	if (cfg->hc_validate)
		VERIFY(lh_validate(lh, cfg->hc_count));
	verify_table(lh, entries, cfg->hc_count, &rng);

	free(entries);
	lh_memory_margin = saved_margin;
	lh_mem_check_interval = saved_interval;
	if (cfg->hc_keep)
		return (lh);
	lh_destroy(lh);
	return (NULL);
}

/*
 * Basic operation: mostly unique random keys, no memory pressure. Also
 * covers boundary hash values (0 and ~0) and multiple records under one
 * key, with hand-rolled verification.
 */
static void
hash_basic(void)
{
	htest_config_t cfg = {
		.hc_record_size = 64,
		.hc_max_memory = 256 << 20,
		.hc_count = 20000,
		.hc_rng_stream = 1,
		.hc_spot_every = 512,
		.hc_validate = B_TRUE,
	};
	run_hash_workload(&cfg);

	/* Boundary hash values and duplicate keys, by hand */
	linear_hash_t *lh = lh_init(16, 64 << 20, selftest_scratch_dir());
	uint8_t buf[16];
	make_payload(buf, sizeof (buf), 111);
	lh_insert(lh, 0, buf);
	make_payload(buf, sizeof (buf), 222);
	lh_insert(lh, 0, buf);
	make_payload(buf, sizeof (buf), 333);
	lh_insert(lh, ~0ULL, buf);

	uint64_t seen = 0;
	lh_iterator_t *iter = lh_initiate_retrieve(lh, 0);
	while (lh_retrieve_next(iter, buf)) {
		uint64_t tag;
		verify_payload(buf, sizeof (buf));
		memcpy(&tag, buf, sizeof (tag));
		VERIFY(tag == 111 || tag == 222);
		seen |= tag;
	}
	VERIFY3U(seen, ==, 111 | 222);

	iter = lh_initiate_retrieve(lh, ~0ULL);
	VERIFY(lh_retrieve_next(iter, buf));
	verify_payload(buf, sizeof (buf));
	VERIFY(!lh_retrieve_next(iter, buf));

	iter = lh_initiate_retrieve(lh, 1234);
	VERIFY(!lh_retrieve_next(iter, buf));

	VERIFY(lh_validate(lh, 3));
	lh_destroy(lh);
}

/*
 * Different record sizes, including the 8-byte minimum (tag only).
 */
static void
hash_record_sizes(void)
{
	static const size_t sizes[] = { 8, 24, 40, 512 };

	for (size_t s = 0; s < sizeof (sizes) / sizeof (sizes[0]); s++) {
		htest_config_t cfg = {
			.hc_record_size = sizes[s],
			.hc_max_memory = 256 << 20,
			.hc_count = 5000,
			.hc_rng_stream = 100 + s,
			.hc_validate = B_TRUE,
		};
		run_hash_workload(&cfg);
	}
}

/*
 * Many records per key: moderate duplication across a bounded key space,
 * and extreme duplication across a tiny one.
 */
static void
hash_duplicates(void)
{
	htest_config_t cfg = {
		.hc_record_size = 32,
		.hc_max_memory = 256 << 20,
		.hc_count = 25000,
		.hc_key_space = 500,
		.hc_rng_stream = 200,
		.hc_spot_every = 512,
		.hc_validate = B_TRUE,
	};
	run_hash_workload(&cfg);

	cfg.hc_count = 3000;
	cfg.hc_key_space = 3;
	cfg.hc_rng_stream = 201;
	run_hash_workload(&cfg);
}

/*
 * Enough unique keys to force thousands of bucket splits and to carry the
 * table through several complete split cycles (suffix-length increments).
 */
static void
hash_splits(void)
{
	htest_config_t cfg = {
		.hc_record_size = 32,
		.hc_max_memory = 256 << 20,
		.hc_count = 60000,
		.hc_rng_stream = 300,
		.hc_spot_every = 1024,
		.hc_validate = B_TRUE,
		.hc_keep = B_TRUE,
	};
	linear_hash_t *lh = run_hash_workload(&cfg);

	/*
	 * 60000 entries at 75% occupancy of 6-slot buckets require at
	 * least ~13000 top-level buckets, so the table must have grown
	 * from 2^10 through 2^13: at least three full split cycles.
	 */
	VERIFY3U(lh->lh_hash_suffix_length, >=, 13);

	/*
	 * Every split adds exactly one top-level bucket, so the number of
	 * splits performed is implied by the width of the table. (This is
	 * the same identity lh_validate() checks, stated as a floor on the
	 * amount of growth the workload should have forced.)
	 */
	VERIFY3U(lh->lh_num_top_level_buckets, >, 1ULL << 13);
	lh_destroy(lh);
}

/*
 * Adversarial key distributions. First: every key identical in the low 22
 * bits, so all entries land in a single bucket whose overflow chain grows
 * into the hundreds of buckets. Then: half adversarial, half random, so
 * that bucket splits must repeatedly repartition a monster chain while
 * normal traffic continues around it.
 */
static void
hash_adversarial(void)
{
	htest_config_t cfg = {
		.hc_record_size = 32,
		.hc_max_memory = 256 << 20,
		.hc_count = 4000,
		.hc_fixed_mask = (1ULL << 22) - 1,
		.hc_fixed_bits = 0x2a5,
		.hc_rng_stream = 400,
		.hc_spot_every = 256,
		.hc_validate = B_TRUE,
	};
	run_hash_workload(&cfg);

	/* Mixed: build entries by hand from two sub-configs */
	selftest_rng_t rng;
	selftest_rng_init(&rng, 401);

	const uint64_t count = 20000;
	linear_hash_t *lh = lh_init(32, 256 << 20, selftest_scratch_dir());
	htest_entry_t *entries =
	    safe_calloc(count * sizeof (htest_entry_t));
	for (uint64_t i = 0; i < count; i++) {
		uint64_t h = selftest_rng_next(&rng) & ~ABSENT_BIT;
		if (i % 2 == 0) {
			h = (h & ~((1ULL << 22) - 1)) | 0x2a5;
		}
		entries[i].ht_hash = h;
		entries[i].ht_tag = i + 1;
	}
	insert_entries(lh, entries, count, 512, &rng);
	VERIFY(lh_validate(lh, count));
	verify_table(lh, entries, count, &rng);
	free(entries);
	lh_destroy(lh);
}

/*
 * A table constructed with no memory budget at all: every allocator is
 * disk-only from the first insert.
 */
static void
hash_no_memory(void)
{
	htest_config_t cfg = {
		.hc_record_size = 64,
		.hc_max_memory = 0,
		.hc_count = 5000,
		.hc_rng_stream = 500,
		.hc_spot_every = 512,
		.hc_validate = B_TRUE,
	};
	run_hash_workload(&cfg);
}

/*
 * The mirror image: no cache directory, so the table is memory-only and can
 * never spill. The budget has to be generous, because a memory-only table
 * that reaches its budget aborts rather than degrading - there is nowhere
 * for a clawback to put the evicted records.
 */
static void
hash_no_disk(void)
{
	htest_config_t cfg = {
		.hc_record_size = 64,
		.hc_max_memory = 256 << 20,
		.hc_count = 20000,
		.hc_rng_stream = 550,
		.hc_spot_every = 512,
		.hc_no_disk = B_TRUE,
		.hc_validate = B_TRUE,
		.hc_keep = B_TRUE,
	};
	linear_hash_t *lh = run_hash_workload(&cfg);

	/* Nothing was ever written to disk, so everything is resident */
	VERIFY3U(total_mem_used(lh), >, 0);
	lh_destroy(lh);
}

/*
 * Heavy memory pressure: the data alone is more than ten times the memory
 * budget. The table must stay correct throughout, keep its total memory use
 * bounded near the budget, and end with the data allocator fully evicted.
 */
static void
hash_memory_pressure(void)
{
	const size_t budget = 2 << 20;
	const uint64_t count = 100000;
	const size_t record_size = 256;
	size_t saved_margin = lh_memory_margin;
	int saved_interval = lh_mem_check_interval;
	selftest_rng_t rng;

	lh_memory_margin = 1 << 20;
	lh_mem_check_interval = 256;
	selftest_rng_init(&rng, 600);

	linear_hash_t *lh = lh_init(record_size, budget,
	    selftest_scratch_dir());
	htest_config_t gen_cfg = {
		.hc_record_size = record_size,
		.hc_count = count,
	};
	htest_entry_t *entries = generate_entries(&gen_cfg, &rng);

	/*
	 * Insert in slices so total memory use can be sampled along the
	 * way, not just at the end. The bound is loose - each of the three
	 * allocators may take one frontier-granularity step past the
	 * budget before the next check notices - but total use must stay in
	 * the budget's neighborhood rather than tracking the data size.
	 */
	const size_t slack = 8 << 20;
	uint8_t *buf = safe_malloc(record_size);
	size_t max_seen = 0;
	for (uint64_t i = 0; i < count; i++) {
		make_payload(buf, record_size, entries[i].ht_tag);
		lh_insert(lh, entries[i].ht_hash, buf);
		if ((i + 1) % 1024 == 0)
			max_seen = MAX(max_seen, total_mem_used(lh));
		if ((i + 1) % 2048 == 0)
			spot_check(lh, entries, i + 1, &rng, buf);
	}
	free(buf);
	if (max_seen > budget + slack) {
		errx(1, "memory use reached %zu against a budget of %zu",
		    max_seen, budget);
	}

	/* The data allocator is the first eviction target and the largest */
	VERIFY3U(allocator_memory_used(lh->lh_alloc.data), ==, 0);
	VERIFY3U(total_mem_used(lh), <=, budget);

	VERIFY(lh_validate(lh, count));
	verify_table(lh, entries, count, &rng);

	free(entries);
	lh_destroy(lh);
	lh_memory_margin = saved_margin;
	lh_mem_check_interval = saved_interval;
}

/*
 * Moderate memory pressure: the budget is big enough for the bucket array
 * but not for the data. Eviction must follow the documented priority order,
 * so the data allocator gets squeezed while the bucket and overflow
 * allocators are never touched. "Never touched" is checked as monotonicity:
 * an allocator that was trimmed has less memory at the end than it had at
 * its high-water mark.
 */
static void
hash_pressure_priority(void)
{
	const size_t budget = 24 << 20;
	const uint64_t count = 40000;
	const size_t record_size = 1024;	/* 40MB of data */
	size_t saved_margin = lh_memory_margin;
	int saved_interval = lh_mem_check_interval;
	selftest_rng_t rng;

	lh_memory_margin = 2 << 20;
	lh_mem_check_interval = 512;
	selftest_rng_init(&rng, 700);

	linear_hash_t *lh = lh_init(record_size, budget,
	    selftest_scratch_dir());
	htest_config_t gen_cfg = {
		.hc_record_size = record_size,
		.hc_count = count,
	};
	htest_entry_t *entries = generate_entries(&gen_cfg, &rng);

	size_t peak[NUM_ALLOC] = { 0 };
	uint8_t *buf = safe_malloc(record_size);
	for (uint64_t i = 0; i < count; i++) {
		make_payload(buf, record_size, entries[i].ht_tag);
		lh_insert(lh, entries[i].ht_hash, buf);
		if ((i + 1) % 512 == 0) {
			for (int k = 0; k < NUM_ALLOC; k++) {
				peak[k] = MAX(peak[k], allocator_memory_used(
				    lh->lh_alloc.all[k]));
			}
		}
		if ((i + 1) % 1024 == 0)
			spot_check(lh, entries, i + 1, &rng, buf);
	}
	free(buf);

	size_t data = allocator_memory_used(lh->lh_alloc.data);
	size_t overflow = allocator_memory_used(lh->lh_alloc.overflow);
	size_t bucket = allocator_memory_used(lh->lh_alloc.bucket);

	/* The data allocator took the hit... */
	VERIFY3U(data, <, peak[0]);
	VERIFY3U(data, <, count * record_size);
	/* ...but is still partially memory-resident: gradual degradation */
	VERIFY3U(data, >, 0);
	/* Bucket and overflow allocators were left alone */
	VERIFY3U(overflow, ==, peak[1]);
	VERIFY3U(bucket, ==, peak[2]);
	VERIFY3U(bucket, >, 0);

	VERIFY(lh_validate(lh, count));
	verify_table(lh, entries, count, &rng);

	free(entries);
	lh_destroy(lh);
	lh_memory_margin = saved_margin;
	lh_mem_check_interval = saved_interval;
}

/*
 * The full complement of MAX_LH_ITERATORS concurrent iterators, stepped
 * round-robin over distinct heavily-duplicated keys, and then iterators
 * used against two live tables in alternation. Iterator state must not
 * bleed between iterators or tables.
 *
 * Note that no insert may happen while any of these iterators is live: an
 * insert invalidates every outstanding iterator, and lh_retrieve_next()
 * treats a stale one as fatal.
 */
static void
hash_iterators(void)
{
	const uint64_t count = 6400;
	const uint64_t key_space = 64;
	selftest_rng_t rng;
	selftest_rng_init(&rng, 800);

	htest_config_t cfg = {
		.hc_record_size = 32,
		.hc_max_memory = 256 << 20,
		.hc_count = count,
		.hc_key_space = key_space,
		.hc_rng_stream = 800,
	};
	linear_hash_t *lh = lh_init(cfg.hc_record_size, cfg.hc_max_memory,
	    selftest_scratch_dir());
	htest_entry_t *entries = generate_entries(&cfg, &rng);
	insert_entries(lh, entries, count, 0, &rng);

	/* Pick the distinct keys and count the expected dups of each */
	uint64_t hashes[MAX_LH_ITERATORS];
	uint64_t expect[MAX_LH_ITERATORS] = { 0 };
	uint64_t got[MAX_LH_ITERATORS] = { 0 };
	int nkeys = 0;
	for (uint64_t i = 0; i < count && nkeys < MAX_LH_ITERATORS; i++) {
		int k;
		for (k = 0; k < nkeys; k++) {
			if (hashes[k] == entries[i].ht_hash)
				break;
		}
		if (k == nkeys)
			hashes[nkeys++] = entries[i].ht_hash;
	}
	VERIFY3S(nkeys, ==, MAX_LH_ITERATORS);
	for (uint64_t i = 0; i < count; i++) {
		for (int k = 0; k < nkeys; k++) {
			if (hashes[k] == entries[i].ht_hash)
				expect[k]++;
		}
	}

	lh_iterator_t *iters[MAX_LH_ITERATORS];
	boolean_t done[MAX_LH_ITERATORS] = { B_FALSE };
	for (int k = 0; k < nkeys; k++)
		iters[k] = lh_initiate_retrieve(lh, hashes[k]);

	uint8_t buf[32];
	boolean_t any = B_TRUE;
	while (any) {
		any = B_FALSE;
		for (int k = 0; k < nkeys; k++) {
			if (done[k])
				continue;
			if (lh_retrieve_next(iters[k], buf)) {
				uint64_t tag;
				verify_payload(buf, sizeof (buf));
				memcpy(&tag, buf, sizeof (tag));
				VERIFY3U(entries[tag - 1].ht_hash, ==,
				    hashes[k]);
				got[k]++;
				any = B_TRUE;
			} else {
				done[k] = B_TRUE;
			}
		}
	}
	for (int k = 0; k < nkeys; k++)
		VERIFY3U(got[k], ==, expect[k]);

	/* Two live tables with interleaved iterator use */
	linear_hash_t *lh2 = lh_init(48, 64 << 20, selftest_scratch_dir());
	uint8_t buf2[48];
	for (uint64_t t = 1; t <= 100; t++) {
		make_payload(buf2, sizeof (buf2), t);
		lh_insert(lh2, 999, buf2);
	}
	lh_iterator_t *ia = lh_initiate_retrieve(lh, hashes[0]);
	lh_iterator_t *ib = lh_initiate_retrieve(lh2, 999);
	uint64_t na = 0, nb = 0;
	boolean_t more_a = B_TRUE, more_b = B_TRUE;
	while (more_a || more_b) {
		if (more_a && lh_retrieve_next(ia, buf))
			na++;
		else
			more_a = B_FALSE;
		if (more_b && lh_retrieve_next(ib, buf2)) {
			verify_payload(buf2, sizeof (buf2));
			nb++;
		} else {
			more_b = B_FALSE;
		}
	}
	VERIFY3U(na, ==, expect[0]);
	VERIFY3U(nb, ==, 100);

	free(entries);
	lh_destroy(lh2);
	lh_destroy(lh);
}

/*
 * Seeded chaos: randomized record sizes, counts, duplication levels,
 * budgets, and memory-management pacing. Whatever the targeted tests
 * miss, this net catches over many CI runs; failures replay with -s.
 */
static void
hash_stress(void)
{
	selftest_rng_t rng;
	selftest_rng_init(&rng, 900);

	for (int round = 0; round < 5; round++) {
		uint64_t count = 5000 + selftest_rng_below(&rng, 35000);
		htest_config_t cfg = {
			.hc_record_size =
			    8 * (1 + selftest_rng_below(&rng, 32)),
			.hc_max_memory = (selftest_rng_below(&rng, 4) == 0) ?
			    (size_t)(1 + selftest_rng_below(&rng, 8)) << 20 :
			    (size_t)(64 + selftest_rng_below(&rng, 192))
			    << 20,
			.hc_count = count,
			.hc_key_space = (selftest_rng_below(&rng, 3) == 0) ?
			    200 + selftest_rng_below(&rng, 2000) : 0,
			.hc_rng_stream = 1000000 + round * 1000,
			.hc_margin = (size_t)(1 + selftest_rng_below(&rng, 8))
			    << 19,
			.hc_interval =
			    64 + (int)selftest_rng_below(&rng, 2048),
			.hc_spot_every = 512,
			.hc_validate = count < 30000,
		};
		run_hash_workload(&cfg);
	}
}

const test_case_t selftest_hash_cases[] = {
	{ "hash_basic",			hash_basic },
	{ "hash_record_sizes",		hash_record_sizes },
	{ "hash_duplicates",		hash_duplicates },
	{ "hash_splits",		hash_splits },
	{ "hash_adversarial",		hash_adversarial },
	{ "hash_no_memory",		hash_no_memory },
	{ "hash_no_disk",		hash_no_disk },
	{ "hash_memory_pressure",	hash_memory_pressure },
	{ "hash_pressure_priority",	hash_pressure_priority },
	{ "hash_iterators",		hash_iterators },
	{ "hash_stress",		hash_stress },
	{ NULL,				NULL },
};
