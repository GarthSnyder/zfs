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
 * Selftests for the zstream_alloc.[ch] record-store API.
 *
 * All tests are built on a shadow-model harness. A shadow_allocator_t wraps
 * an allocator_t together with a full record of what each record index
 * should contain: a deterministic pattern derived from a tag (for stored
 * records) or zeros (for never-written records). After any sequence of
 * operations, the entire allocator can be swept and compared against the
 * model byte for byte.
 *
 * The most delicate allocator operation is allocator_trim_memory(), which
 * lowers the split point between the memory-resident and disk-resident
 * portions of the record space. Trimming is relative to memory actually in
 * use, it only ever moves in one direction, and its arithmetic has to
 * survive a delta larger than the amount in use. predict_trim() below
 * duplicates the intended arithmetic independently, so the tests can pin
 * the resulting split point exactly rather than just bounding it.
 */

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zstream_alloc.h"
#include "zstream_selftest.h"
#include "zstream_util.h"

/*
 * Mirrors of allocator-internal constants. They are duplicated rather than
 * exported because a test that asks the code under test what to expect
 * tests nothing; if allocator_init() changes its rounding policy, these
 * have to be updated to match deliberately.
 */
#define	TEST_TARGET_GRANULARITY		(32 << 20)
#define	TEST_FRONTIER_GRANULARITY	(8 << 20)

typedef enum {
	REC_EMPTY = 0,		/* Never written; must read as zeros */
	REC_KNOWN,		/* Stored; content derived from sr_tags[] */
} record_state_t;

typedef struct {
	allocator_t	*sh_alloc;
	boolean_t	sh_disk;	/* Has a backing file */
	size_t		sh_record_size;
	uint64_t	sh_capacity;	/* Model size, in records */
	uint64_t	sh_count;	/* Highest index written, plus one */
	uint64_t	sh_next_tag;
	uint64_t	*sh_tags;
	uint8_t		*sh_state;
	uint8_t		*sh_buf;	/* Scratch: retrieved record */
	uint8_t		*sh_expect;	/* Scratch: expected record */
} shadow_allocator_t;

/*
 * Deterministic record content: a function of the tag alone, so the model
 * only needs to remember one uint64_t per record.
 */
static void
fill_record(uint8_t *buf, size_t len, uint64_t tag)
{
	uint64_t x = selftest_mix64(selftest_seed ^ tag);
	for (size_t i = 0; i < len; i++) {
		if ((i & 7) == 0)
			x = selftest_mix64(x + i);
		buf[i] = (uint8_t)(x >> ((i & 7) << 3));
	}
}

static boolean_t
all_zero(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != 0)
			return (B_FALSE);
	}
	return (B_TRUE);
}

static size_t
gcd_of(size_t a, size_t b)
{
	while (b != 0) {
		size_t r = a % b;
		a = b;
		b = r;
	}
	return (a);
}

static size_t
lcm_of(size_t a, size_t b)
{
	return (a / gcd_of(a, b) * b);
}

/*
 * The record-to-record stride allocator_init() will settle on: the record
 * size rounded up to the smallest power-of-2 alignment that brings the
 * memory/disk split unit down to TEST_TARGET_GRANULARITY or below. Note
 * that alignment starts at 1, so most record sizes are not rounded at all.
 */
static size_t
record_stride(size_t record_size)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);

	for (size_t align = 1; align != 0; align <<= 1) {
		size_t rounded = P2ROUNDUP(record_size, align);
		if (lcm_of(page, rounded) <= TEST_TARGET_GRANULARITY)
			return (rounded);
	}
	errx(1, "no usable stride for record size %zu", record_size);
}

/*
 * The memory/disk transition must land on a boundary that is a multiple of
 * both the page size and the stride, so that no record straddles it. Every
 * budget the allocator reports is a multiple of this unit.
 */
static size_t
split_unit(size_t record_size)
{
	return (lcm_of((size_t)sysconf(_SC_PAGESIZE),
	    record_stride(record_size)));
}

/*
 * How far the writable frontier jumps on first touch. Reifying stops at the
 * memory budget, so a budget smaller than the granularity is consumed whole.
 */
static size_t
frontier_step(size_t budget)
{
	size_t step = MAX((size_t)sysconf(_SC_PAGESIZE),
	    (size_t)TEST_FRONTIER_GRANULARITY);
	return (MIN(step, budget));
}

/*
 * An independent restatement of what allocator_trim_memory() should leave
 * behind, given the bytes in use before the call. Trimming rounds the
 * survivors up to a whole split unit and then, if that would not have freed
 * anything, gives back one unit anyway.
 */
static size_t
predict_trim(size_t used, size_t delta, size_t unit)
{
	if (used == 0)
		return (0);
	size_t keep = (delta >= used) ? 0 : used - delta;
	size_t new_max = ROUND_UP(keep, unit);
	if (new_max >= used)
		new_max -= unit;
	return (new_max);
}

/*
 * Trim, then check the documented contract: the return value accounts for
 * the whole change in memory use, the split lands where predict_trim() says
 * and on a unit boundary, and memory use never grows.
 */
static void
shadow_trim(shadow_allocator_t *sh, size_t delta)
{
	size_t unit = split_unit(sh->sh_record_size);
	size_t before = allocator_memory_used(sh->sh_alloc);
	size_t want = predict_trim(before, delta, unit);

	size_t freed = allocator_trim_memory(sh->sh_alloc, delta);
	size_t after = allocator_memory_used(sh->sh_alloc);

	if (after != want) {
		errx(1, "trim of %zu from %zu bytes left %zu, expected %zu "
		    "(unit %zu)", delta, before, after, want, unit);
	}
	VERIFY3U(freed, ==, before - after);
	VERIFY3U(after, <=, before);
	VERIFY0(after % unit);
	if (before > 0)
		VERIFY3U(freed, >, 0);
}

static shadow_allocator_t *
shadow_init(size_t record_size, size_t mem_size, boolean_t disk_backed,
    uint64_t capacity)
{
	shadow_allocator_t sh = {
		.sh_alloc = allocator_init(record_size, mem_size,
		    disk_backed ? selftest_scratch_dir() : NULL),
		.sh_disk = disk_backed,
		.sh_record_size = record_size,
		.sh_capacity = capacity,
		.sh_next_tag = 1,
		.sh_tags = safe_calloc(capacity * sizeof (uint64_t)),
		.sh_state = safe_calloc(capacity),
		.sh_buf = safe_malloc(record_size),
		.sh_expect = safe_malloc(record_size)
	};
	VERIFY(sh.sh_alloc != NULL);
	shadow_allocator_t *shadow = safe_malloc(sizeof (shadow_allocator_t));
	*shadow = sh;
	return (shadow);
}

static void
shadow_fini(shadow_allocator_t *sh)
{
	allocator_destroy(sh->sh_alloc);	/* Closes the backing file */
	free(sh->sh_tags);
	free(sh->sh_state);
	free(sh->sh_buf);
	free(sh->sh_expect);
	free(sh);
}

static void
shadow_store(shadow_allocator_t *sh, uint64_t ix)
{
	VERIFY3U(ix, <, sh->sh_capacity);
	uint64_t tag = sh->sh_next_tag++;
	fill_record(sh->sh_buf, sh->sh_record_size, tag);
	allocator_store(sh->sh_alloc, ix, sh->sh_buf);
	sh->sh_tags[ix] = tag;
	sh->sh_state[ix] = REC_KNOWN;
	sh->sh_count = MAX(sh->sh_count, ix + 1);
}

static void
shadow_append(shadow_allocator_t *sh)
{
	VERIFY3U(sh->sh_count, <, sh->sh_capacity);
	uint64_t tag = sh->sh_next_tag++;
	fill_record(sh->sh_buf, sh->sh_record_size, tag);
	record_ix_t ix = allocator_append(sh->sh_alloc, sh->sh_buf);
	VERIFY3U(ix, ==, sh->sh_count);
	sh->sh_tags[ix] = tag;
	sh->sh_state[ix] = REC_KNOWN;
	sh->sh_count++;
}

static void
shadow_skip(shadow_allocator_t *sh)
{
	VERIFY3U(sh->sh_count, <, sh->sh_capacity);
	record_ix_t ix = allocator_skip(sh->sh_alloc);
	VERIFY3U(ix, ==, sh->sh_count);
	sh->sh_state[ix] = REC_EMPTY;
	sh->sh_count++;
}

static void
shadow_verify(shadow_allocator_t *sh, uint64_t ix)
{
	VERIFY3U(ix, <, sh->sh_capacity);
	allocator_retrieve(sh->sh_alloc, ix, sh->sh_buf);
	if (sh->sh_state[ix] == REC_KNOWN) {
		fill_record(sh->sh_expect, sh->sh_record_size, sh->sh_tags[ix]);
		int ret = memcmp(sh->sh_buf, sh->sh_expect, sh->sh_record_size);
		if (ret != 0) {
			errx(1, "record %ju corrupted", (uintmax_t)ix);
		}
	} else if (!all_zero(sh->sh_buf, sh->sh_record_size)) {
		errx(1, "unwritten record %ju is not zero-filled",
		    (uintmax_t)ix);
	}
}

/*
 * Sweep the entire record space against the model: every record below
 * sh_count, plus (for disk-backed allocators, where any index is always
 * readable) a few probes beyond the end, which must read as zeros.
 */
static void
shadow_verify_all(shadow_allocator_t *sh)
{
	for (uint64_t ix = 0; ix < sh->sh_count; ix++)
		shadow_verify(sh, ix);

	if (sh->sh_disk) {
		for (uint64_t ix = sh->sh_count;
		    ix < MIN(sh->sh_count + 3, sh->sh_capacity); ix++) {
			allocator_retrieve(sh->sh_alloc, ix, sh->sh_buf);
			if (!all_zero(sh->sh_buf, sh->sh_record_size)) {
				errx(1, "read beyond end of records (index "
				    "%ju) is not zero-filled", (uintmax_t)ix);
			}
		}
	}
}

/*
 * Basic operation of all three allocator configurations: memory-only,
 * disk-only, and dual-backed. Round-trip integrity, append/skip index
 * sequencing, zero-fill of unwritten records, overwrite of existing
 * records, and which side of the split the bytes actually landed on.
 */
static void
alloc_basic(void)
{
	const size_t rsize = 24;
	const uint64_t cap = 4096;
	const size_t stride = record_stride(rsize);

	for (int config = 0; config < 3; config++) {
		boolean_t disk = (config != 0);
		size_t mem;
		switch (config) {
		case 0:		/* Memory-only */
			mem = 2 * cap * stride;
			break;
		case 1:		/* Disk-only */
			mem = 0;
			break;
		default:	/* Dual: about a quarter of the data fits */
			mem = cap * stride / 4;
			break;
		}

		shadow_allocator_t *sh = shadow_init(rsize, mem, disk, cap);

		for (int i = 0; i < 200; i++)
			shadow_append(sh);
		for (int i = 0; i < 3; i++)
			shadow_skip(sh);
		for (int i = 0; i < 5; i++)
			shadow_append(sh);

		/* Overwrites and stores that leave gaps */
		shadow_store(sh, 0);
		shadow_store(sh, 100);
		shadow_store(sh, 300);
		shadow_store(sh, cap - 1);
		shadow_verify_all(sh);
		VERIFY3U(sh->sh_count, ==, cap);

		/* Overwrite the same record repeatedly */
		for (int i = 0; i < 10; i++)
			shadow_store(sh, 77);
		shadow_verify(sh, 77);

		/*
		 * A disk-only allocator must never reify a page; the other
		 * two must, since record 0 is always on the memory side.
		 */
		size_t used = allocator_memory_used(sh->sh_alloc);
		if (config == 1)
			VERIFY3U(used, ==, 0);
		else
			VERIFY3U(used, >, 0);

		shadow_fini(sh);
	}
}

/*
 * Record sizes with awkward relationships to the page size: tiny, odd,
 * non-power-of-2 multiples of 8 (which make the memory/disk boundary land
 * at an lcm that is not a power of 2), exact page size, and page+. Each is
 * filled past its budget and then walked down to disk-only.
 */
static void
alloc_record_sizes(void)
{
	static const size_t sizes[] =
	    { 1, 3, 7, 8, 12, 24, 56, 104, 512, 1000, 4096, 5000, 12288 };

	for (size_t s = 0; s < sizeof (sizes) / sizeof (sizes[0]); s++) {
		size_t rsize = sizes[s];
		size_t unit = split_unit(rsize);
		size_t stride = record_stride(rsize);
		uint64_t cap = 3 * (unit / stride) + 17;

		shadow_allocator_t *sh = shadow_init(rsize, 3 * unit, B_TRUE,
		    cap);
		while (sh->sh_count < cap)
			shadow_append(sh);
		shadow_verify_all(sh);

		/* Down one unit at a time, then all the way to disk-only */
		while (allocator_memory_used(sh->sh_alloc) > unit) {
			shadow_trim(sh, unit);
			shadow_verify_all(sh);
		}
		shadow_trim(sh, SIZE_MAX / 2);
		VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, 0);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}
}

/*
 * The trim arithmetic itself, at the deltas most likely to be mishandled:
 * zero, one byte, either side of a unit boundary, and a delta far larger
 * than the memory actually in use. The last of these underflowed in an
 * earlier version of the allocator and produced a negative file offset, so
 * it is checked from a fresh allocator every time rather than only as the
 * tail of a longer sequence.
 */
static void
alloc_trim_arithmetic(void)
{
	const size_t rsize = 24;
	const size_t unit = split_unit(rsize);
	const size_t stride = record_stride(rsize);
	const size_t budget = 16 * unit;
	static const size_t deltas[] = { 0, 1, 4096 };

	/* Deltas expressed relative to the unit are added below */
	for (size_t d = 0; d < sizeof (deltas) / sizeof (deltas[0]) + 4; d++) {
		size_t delta;
		switch (d) {
		case 3: delta = unit - 1;	break;
		case 4: delta = unit;		break;
		case 5: delta = unit + 1;	break;
		case 6: delta = SIZE_MAX / 2;	break;	/* Far beyond use */
		default: delta = deltas[d];	break;
		}

		uint64_t cap = budget / stride;
		shadow_allocator_t *sh = shadow_init(rsize, budget, B_TRUE,
		    cap);
		while (sh->sh_count < cap)
			shadow_append(sh);

		size_t used = allocator_memory_used(sh->sh_alloc);
		VERIFY3U(used, >, 0);
		/* Repeat until memory is gone; each call must make progress */
		while (allocator_memory_used(sh->sh_alloc) > 0) {
			shadow_trim(sh, delta);
			shadow_verify_all(sh);
		}
		VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, 0);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	/*
	 * A budget far above the high-water mark. Here the split point
	 * starts at one frontier step rather than at the budget, so a small
	 * delta has to be measured against memory actually in use. Testing
	 * this only against a saturated allocator would miss it, because
	 * there the two quantities coincide.
	 */
	{
		const size_t big = 64 << 20;
		shadow_allocator_t *sh = shadow_init(rsize, big, B_TRUE, 64);
		shadow_append(sh);
		size_t used = allocator_memory_used(sh->sh_alloc);
		VERIFY3U(used, ==, frontier_step(big));
		VERIFY3U(used, <, big);

		/* Each small trim must still give back at least one unit */
		for (int i = 0; i < 4; i++) {
			size_t before = allocator_memory_used(sh->sh_alloc);
			shadow_trim(sh, 1);
			VERIFY3U(allocator_memory_used(sh->sh_alloc), <,
			    before);
			shadow_verify_all(sh);
		}
		/* And the descent still terminates */
		while (allocator_memory_used(sh->sh_alloc) > 0)
			shadow_trim(sh, 4 * unit);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	/* Trimming an allocator that never reified anything is a no-op */
	{
		shadow_allocator_t *sh = shadow_init(rsize, budget, B_TRUE, 8);
		VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, 0);
		shadow_trim(sh, unit);
		shadow_trim(sh, SIZE_MAX / 2);
		VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, 0);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	/* And so is trimming a disk-only allocator, which owns no pages */
	{
		shadow_allocator_t *sh = shadow_init(rsize, 0, B_TRUE, 64);
		for (int i = 0; i < 64; i++)
			shadow_append(sh);
		shadow_trim(sh, SIZE_MAX / 2);
		VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, 0);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}
}

/*
 * Walk the memory/disk split all the way down through a full record space,
 * verifying every record after each move; then do a randomized descent with
 * interleaved mutations so the split passes through *changing* data.
 */
static void
alloc_trim_sweep(void)
{
	const size_t rsize = 24;
	const size_t unit = split_unit(rsize);
	const size_t stride = record_stride(rsize);
	const uint64_t cap = 8 * (unit / stride);

	/* Deterministic descent, one unit at a time */
	{
		shadow_allocator_t *sh = shadow_init(rsize, 8 * unit, B_TRUE,
		    cap);
		while (sh->sh_count < cap)
			shadow_append(sh);
		shadow_verify_all(sh);

		int steps = 0;
		while (allocator_memory_used(sh->sh_alloc) > 0) {
			shadow_trim(sh, unit);
			shadow_verify_all(sh);
			steps++;
			VERIFY3S(steps, <, 1000);	/* Must make progress */
		}
		shadow_fini(sh);
	}

	/* Randomized descent with mutation between moves */
	{
		selftest_rng_t rng;
		selftest_rng_init(&rng, 42);
		shadow_allocator_t *sh = shadow_init(rsize, 8 * unit, B_TRUE,
		    cap);
		while (sh->sh_count < cap)
			shadow_append(sh);

		for (int iter = 0; iter < 300; iter++) {
			for (int i = 0; i < 8; i++)
				shadow_store(sh, selftest_rng_below(&rng, cap));
			if (allocator_memory_used(sh->sh_alloc) > 0 &&
			    selftest_rng_below(&rng, 4) == 0) {
				shadow_trim(sh,
				    selftest_rng_below(&rng, 3 * unit));
			}
			for (int i = 0; i < 32; i++)
				shadow_verify(sh,
				    selftest_rng_below(&rng, cap));
			if (iter % 25 == 24)
				shadow_verify_all(sh);
		}
		shadow_verify_all(sh);
		shadow_fini(sh);
	}
}

/*
 * Corner cases that deserve individual attention.
 */
static void
alloc_boundaries(void)
{
	const size_t rsize = 24;
	const size_t unit = split_unit(rsize);
	const uint64_t recs_per_unit = unit / record_stride(rsize);

	/* Trimming an empty allocator, repeatedly */
	{
		shadow_allocator_t *sh = shadow_init(rsize, 4 * unit, B_TRUE,
		    64);
		for (int i = 0; i < 4; i++)
			shadow_trim(sh, unit);
		shadow_verify_all(sh);
		VERIFY3U(sh->sh_count, ==, 0);
		/* Still usable afterwards, just on disk now */
		shadow_append(sh);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	/* A single record chased onto disk by the split point */
	{
		shadow_allocator_t *sh = shadow_init(rsize, 4 * unit, B_TRUE,
		    64);
		shadow_append(sh);
		shadow_verify_all(sh);
		while (allocator_memory_used(sh->sh_alloc) > 0) {
			shadow_trim(sh, unit);
			shadow_verify_all(sh);
		}
		shadow_fini(sh);
	}

	/*
	 * Data that exactly fills one split unit, plus the first record on
	 * the far side of that boundary.
	 */
	{
		shadow_allocator_t *sh = shadow_init(rsize, unit, B_TRUE,
		    recs_per_unit * 4);
		while (sh->sh_count < recs_per_unit)
			shadow_append(sh);
		shadow_verify_all(sh);
		shadow_store(sh, recs_per_unit);	/* First disk record */
		shadow_verify_all(sh);
		shadow_trim(sh, unit);
		VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, 0);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	/*
	 * Sparse data: one record stored high above a sea of never-written
	 * records, with the memory region below the split left untouched.
	 */
	{
		shadow_allocator_t *sh = shadow_init(rsize, 4 * unit, B_TRUE,
		    recs_per_unit * 8);
		shadow_store(sh, recs_per_unit * 6);	/* Disk-side */
		shadow_verify_all(sh);
		while (allocator_memory_used(sh->sh_alloc) > 0) {
			shadow_trim(sh, unit);
			shadow_verify_all(sh);
		}
		shadow_fini(sh);
	}

	/* Disk-only from birth: no memory to trim, everything still works */
	{
		shadow_allocator_t *sh = shadow_init(rsize, 0, B_TRUE, 512);
		selftest_rng_t rng;
		selftest_rng_init(&rng, 43);
		for (int cycle = 0; cycle < 6; cycle++) {
			for (int i = 0; i < 100; i++)
				shadow_store(sh, selftest_rng_below(&rng, 512));
			shadow_trim(sh, unit);
			shadow_verify_all(sh);
		}
		VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, 0);
		shadow_fini(sh);
	}

	/* A budget far larger than anything ever written */
	{
		shadow_allocator_t *sh = shadow_init(rsize, 64 * unit, B_TRUE,
		    64);
		shadow_append(sh);
		shadow_verify_all(sh);
		VERIFY3U(allocator_memory_used(sh->sh_alloc), ==,
		    frontier_step(64 * unit));
		shadow_fini(sh);
	}
}

/*
 * Repeated create/destroy cycles must not leak file descriptors. (VM
 * mappings are covered implicitly: each allocator reserves several times
 * physical RAM in address space, so leaking those would fail fast.)
 */
static void
alloc_lifecycle(void)
{
	int probe = open("/dev/null", O_RDONLY);
	VERIFY3S(probe, >=, 0);
	int baseline_fd = probe;
	(void) close(probe);

	for (int i = 0; i < 50; i++) {
		shadow_allocator_t *sh = shadow_init(64, (i % 2) ? 0 : 1 << 16,
		    B_TRUE, 256);
		for (int j = 0; j < 50; j++)
			shadow_append(sh);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	probe = open("/dev/null", O_RDONLY);
	VERIFY3S(probe, >=, 0);
	if (probe > baseline_fd + 2) {
		errx(1, "file descriptors leaked: probe fd went from %d to %d",
		    baseline_fd, probe);
	}
	(void) close(probe);
}

#if defined(__linux__)
static size_t
current_rss(void)
{
	FILE *fp = fopen("/proc/self/statm", "r");
	unsigned long total, resident;

	VERIFY(fp != NULL);
	VERIFY3S(fscanf(fp, "%lu %lu", &total, &resident), ==, 2);
	(void) fclose(fp);
	return ((size_t)resident * (size_t)sysconf(_SC_PAGESIZE));
}
#endif

/*
 * Trimming must actually return pages to the OS, not merely move a
 * bookkeeping pointer, and the data that was resident must survive the trip
 * to disk intact. The RSS check is Linux-only; the integrity check runs
 * everywhere.
 */
static void
alloc_memory_release(void)
{
	const size_t rsize = 4096;
	const uint64_t cap = 8192;		/* 32MB of records */
	const size_t data_bytes = cap * rsize;

	shadow_allocator_t *sh =
	    shadow_init(rsize, data_bytes, B_TRUE, cap + 8);
	while (sh->sh_count < cap)
		shadow_append(sh);
	shadow_verify_all(sh);

	VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, data_bytes);

#if defined(__linux__)
	size_t rss_full = current_rss();
#endif
	shadow_trim(sh, data_bytes - (2 << 20));
	VERIFY3U(allocator_memory_used(sh->sh_alloc), <=, 2 << 20);
#if defined(__linux__)
	size_t rss_shrunk = current_rss();
	if (rss_full < rss_shrunk ||
	    rss_full - rss_shrunk < data_bytes / 2) {
		errx(1, "trimming did not release memory (RSS %zu -> %zu)",
		    rss_full, rss_shrunk);
	}
#endif
	shadow_verify_all(sh);

	/* And the rest of the way to disk-only */
	shadow_trim(sh, SIZE_MAX / 2);
	VERIFY3U(allocator_memory_used(sh->sh_alloc), ==, 0);
	shadow_verify_all(sh);		/* Includes beyond-end zero probes */

	shadow_fini(sh);
}

/*
 * Seeded chaos: random operation mixes against the shadow model across
 * randomized configurations. Failures replay with -s.
 */
static void
alloc_stress(void)
{
	static const size_t sizes[] = { 8, 24, 104, 512, 4096 };
	selftest_rng_t rng;
	selftest_rng_init(&rng, 4242);

	for (int round = 0; round < 6; round++) {
		size_t rsize = sizes[selftest_rng_below(&rng, 5)];
		size_t stride = record_stride(rsize);
		size_t unit = split_unit(rsize);
		uint64_t cap = 1024 + selftest_rng_below(&rng, 1024);
		boolean_t disk = selftest_rng_below(&rng, 4) != 0;
		/*
		 * A memory-only allocator can never spill, so it has to be
		 * born large enough for every index below cap and can never
		 * be trimmed. Disk-backed ones get an arbitrary budget.
		 */
		size_t full = cap * stride;
		size_t budget = disk ?
		    selftest_rng_below(&rng, 2 * full) : 2 * full;

		shadow_allocator_t *sh = shadow_init(rsize, budget, disk, cap);

		for (int op = 0; op < 4000; op++) {
			uint64_t k = selftest_rng_below(&rng, 100);
			if (k < 35) {
				shadow_store(sh,
				    selftest_rng_below(&rng, cap));
			} else if (k < 50) {
				if (sh->sh_count < cap)
					shadow_append(sh);
			} else if (k < 55) {
				if (sh->sh_count < cap)
					shadow_skip(sh);
			} else if (k < 85 || !disk) {
				shadow_verify(sh,
				    selftest_rng_below(&rng, cap));
			} else {
				shadow_trim(sh,
				    selftest_rng_below(&rng, 4 * unit));
			}
		}
		shadow_verify_all(sh);
		shadow_fini(sh);
	}
}

const test_case_t selftest_alloc_cases[] = {
	{ "alloc_basic",		alloc_basic },
	{ "alloc_record_sizes",		alloc_record_sizes },
	{ "alloc_trim_arithmetic",	alloc_trim_arithmetic },
	{ "alloc_trim_sweep",		alloc_trim_sweep },
	{ "alloc_boundaries",		alloc_boundaries },
	{ "alloc_lifecycle",		alloc_lifecycle },
	{ "alloc_memory_release",	alloc_memory_release },
	{ "alloc_stress",		alloc_stress },
	{ NULL,				NULL },
};
