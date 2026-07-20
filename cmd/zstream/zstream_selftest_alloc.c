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

/*
 * Selftests for the zstream_alloc record-store API.
 *
 * All tests are built on a shadow-model harness. A shadow_t wraps an
 * allocator_t together with a full record of what each record index should
 * contain: a deterministic pattern derived from a tag (for stored records),
 * zeros (for never-written records), or "unspecified" (for records claimed
 * with allocator_skip()). After any sequence of operations, the entire
 * allocator can be swept and compared against the model byte for byte.
 *
 * The most delicate allocator operation is allocator_set_max_memory(),
 * which moves the split point between the memory-resident and disk-resident
 * portions of the record space. The tests here move the split across every
 * boundary we could think of - and then re-verify all content, since a
 * botched transfer shows up as exactly one wrong byte somewhere.
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "zstream_alloc.h"
#include "zstream_selftest.h"
#include "zstream_util.h"

typedef enum {
	REC_EMPTY = 0,		/* Never written; must read as zeros */
	REC_KNOWN,		/* Stored; content derived from sr_tags[] */
	REC_UNSPECIFIED,	/* Skipped; content is not defined */
} rec_state_t;

typedef struct {
	allocator_t	*sh_alloc;
	int		sh_fd;		/* Backing fd, or -1; owned by alloc */
	size_t		sh_record_size;
	uint64_t	sh_capacity;	/* Model size, in records */
	uint64_t	sh_count;	/* Expected as_num_records */
	uint64_t	sh_next_tag;
	uint64_t	*sh_tags;
	uint8_t		*sh_state;
	uint8_t		*sh_buf;	/* Scratch: retrieved record */
	uint8_t		*sh_expect;	/* Scratch: expected record */
} shadow_t;

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

static shadow_t *
shadow_init(size_t record_size, size_t mem_size, boolean_t disk_backed,
    uint64_t capacity)
{
	shadow_t *sh = safe_calloc(sizeof (shadow_t));
	sh->sh_record_size = record_size;
	sh->sh_capacity = capacity;
	sh->sh_next_tag = 1;
	sh->sh_tags = safe_calloc(capacity * sizeof (uint64_t));
	sh->sh_state = safe_calloc(capacity);
	sh->sh_buf = safe_malloc(record_size);
	sh->sh_expect = safe_malloc(record_size);
	sh->sh_fd = disk_backed ? selftest_create_tempfile() : -1;
	sh->sh_alloc = allocator_init(record_size, mem_size, sh->sh_fd);
	VERIFY(sh->sh_alloc != NULL);
	return (sh);
}

static void
shadow_fini(shadow_t *sh)
{
	allocator_destroy(sh->sh_alloc);	/* Closes sh_fd */
	free(sh->sh_tags);
	free(sh->sh_state);
	free(sh->sh_buf);
	free(sh->sh_expect);
	free(sh);
}

static void
shadow_store(shadow_t *sh, uint64_t ix)
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
shadow_append(shadow_t *sh)
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
shadow_skip(shadow_t *sh)
{
	VERIFY3U(sh->sh_count, <, sh->sh_capacity);
	record_ix_t ix = allocator_skip(sh->sh_alloc);
	VERIFY3U(ix, ==, sh->sh_count);
	sh->sh_state[ix] = REC_UNSPECIFIED;
	sh->sh_count++;
}

static void
shadow_verify(shadow_t *sh, uint64_t ix)
{
	VERIFY3U(ix, <, sh->sh_capacity);
	if (sh->sh_state[ix] == REC_UNSPECIFIED)
		return;
	allocator_retrieve(sh->sh_alloc, ix, sh->sh_buf);
	if (sh->sh_state[ix] == REC_KNOWN) {
		fill_record(sh->sh_expect, sh->sh_record_size, sh->sh_tags[ix]);
		if (memcmp(sh->sh_buf, sh->sh_expect,
		    sh->sh_record_size) != 0) {
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
shadow_verify_all(shadow_t *sh)
{
	allocator_stats_t stats = allocator_get_stats(sh->sh_alloc);
	VERIFY3U(stats.as_num_records, ==, sh->sh_count);

	for (uint64_t ix = 0; ix < sh->sh_count; ix++)
		shadow_verify(sh, ix);

	if (sh->sh_fd >= 0) {
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
 * The memory/disk transition must land on a boundary that is a multiple of
 * both the page size and the rounded record size; replicate that math here
 * so tests can aim resizes at exact boundaries.
 */
static size_t
split_unit(size_t record_size)
{
	size_t rounded = P2ROUNDUP(record_size, 8);
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	size_t a = page;
	size_t b = rounded;
	while (b != 0) {
		size_t r = a % b;
		a = b;
		b = r;
	}
	return (page / a * rounded);
}

/*
 * Basic operation of all three allocator configurations: memory-only,
 * disk-only, and dual-backed. Round-trip integrity, append/skip index
 * sequencing, zero-fill of unwritten records, overwrite of existing
 * records, and stats accounting.
 */
static void
alloc_basic(void)
{
	const size_t rsize = 24;
	const uint64_t cap = 4096;

	for (int config = 0; config < 3; config++) {
		boolean_t disk = (config != 0);
		size_t mem;
		switch (config) {
		case 0:		/* Memory-only */
			mem = 2 * cap * rsize;
			break;
		case 1:		/* Disk-only */
			mem = 0;
			break;
		default:	/* Dual: about a quarter of the data fits */
			mem = cap * rsize / 4;
			break;
		}

		shadow_t *sh = shadow_init(rsize, mem, disk, cap);

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

		/* Overwrite the same record repeatedly */
		for (int i = 0; i < 10; i++)
			shadow_store(sh, 77);
		shadow_verify(sh, 77);

		allocator_stats_t stats = allocator_get_stats(sh->sh_alloc);
		VERIFY3U(stats.as_num_records, ==, cap);
		VERIFY3U(stats.as_io_ops_mem + stats.as_io_ops_disk, >, 0);
		if (config == 0)
			VERIFY3U(stats.as_io_ops_disk, ==, 0);
		if (config == 1)
			VERIFY3U(stats.as_io_ops_mem, ==, 0);

		shadow_fini(sh);
	}
}

/*
 * Record sizes with awkward relationships to the page size: tiny, odd,
 * non-power-of-2 multiples of 8 (which make the memory/disk boundary land
 * at an lcm that is not a power of 2), exact page size, and page+.
 */
static void
alloc_record_sizes(void)
{
	static const size_t sizes[] =
	    { 1, 3, 7, 8, 12, 24, 56, 104, 512, 1000, 4096, 5000, 12288 };

	for (size_t s = 0; s < sizeof (sizes) / sizeof (sizes[0]); s++) {
		size_t rsize = sizes[s];
		size_t unit = split_unit(rsize);
		size_t rounded = P2ROUNDUP(rsize, 8);
		uint64_t cap = 3 * (unit / rounded) + 17;

		shadow_t *sh = shadow_init(rsize, unit, B_TRUE, cap);
		while (sh->sh_count < cap)
			shadow_append(sh);
		shadow_verify_all(sh);

		allocator_set_max_memory(sh->sh_alloc, 0);
		shadow_verify_all(sh);
		allocator_set_max_memory(sh->sh_alloc, 2 * unit);
		shadow_verify_all(sh);
		allocator_set_max_memory(sh->sh_alloc, 5 * unit);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}
}

/*
 * The centerpiece: sweep the memory/disk split point across the whole
 * record space and back, hitting exact boundaries and both sides of every
 * boundary, verifying full content after each move. Then do a randomized
 * walk with interleaved mutations so the split moves through *changing*
 * data.
 */
static void
alloc_split_sweep(void)
{
	const size_t rsize = 24;
	const size_t unit = split_unit(rsize);
	const int max_units = 10;
	const uint64_t cap = 8 * (unit / P2ROUNDUP(rsize, 8));

	shadow_t *sh = shadow_init(rsize, 4 * unit, B_TRUE, cap);
	while (sh->sh_count < cap)
		shadow_append(sh);
	shadow_verify_all(sh);

	/* Deterministic up-sweep and down-sweep across unit boundaries */
	for (int k = 0; k <= max_units; k++) {
		size_t targets[3] = { k * unit, k * unit + 1,
		    k > 0 ? k * unit - 1 : 0 };
		for (int t = 0; t < 3; t++) {
			allocator_set_max_memory(sh->sh_alloc, targets[t]);
			shadow_verify_all(sh);
		}
	}
	for (int k = max_units; k >= 0; k--) {
		allocator_set_max_memory(sh->sh_alloc, k * unit);
		shadow_verify_all(sh);
	}

	/* Randomized walk with mutation between moves */
	selftest_rng_t rng;
	selftest_rng_init(&rng, 42);
	for (int iter = 0; iter < 300; iter++) {
		for (int i = 0; i < 8; i++)
			shadow_store(sh, selftest_rng_below(&rng, cap));
		size_t budget = selftest_rng_below(&rng,
		    (max_units + 1) * unit);
		allocator_set_max_memory(sh->sh_alloc, budget);
		for (int i = 0; i < 32; i++)
			shadow_verify(sh, selftest_rng_below(&rng, cap));
		if (iter % 25 == 24)
			shadow_verify_all(sh);
	}
	shadow_verify_all(sh);
	shadow_fini(sh);
}

/*
 * Corner cases for resizing that deserve individual attention.
 */
static void
alloc_boundaries(void)
{
	const size_t rsize = 24;
	const size_t unit = split_unit(rsize);
	const uint64_t recs_per_unit = unit / P2ROUNDUP(rsize, 8);

	/* Resizing an empty allocator in every direction */
	{
		shadow_t *sh = shadow_init(rsize, unit, B_TRUE, 64);
		allocator_set_max_memory(sh->sh_alloc, 4 * unit);
		allocator_set_max_memory(sh->sh_alloc, 0);
		allocator_set_max_memory(sh->sh_alloc, unit);
		shadow_verify_all(sh);
		VERIFY3U(sh->sh_count, ==, 0);
		shadow_fini(sh);
	}

	/* A single record chased up and down by the split point */
	{
		shadow_t *sh = shadow_init(rsize, unit, B_TRUE, 64);
		shadow_append(sh);
		allocator_set_max_memory(sh->sh_alloc, 0);
		shadow_verify_all(sh);
		allocator_set_max_memory(sh->sh_alloc, unit);
		shadow_verify_all(sh);
		allocator_set_max_memory(sh->sh_alloc, 2 * unit);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	/*
	 * Data that exactly fills the memory budget, plus the first record
	 * on the far side of the split.
	 */
	{
		shadow_t *sh = shadow_init(rsize, unit, B_TRUE,
		    recs_per_unit * 4);
		while (sh->sh_count < recs_per_unit)
			shadow_append(sh);
		shadow_verify_all(sh);
		shadow_store(sh, recs_per_unit);	/* First disk record */
		shadow_verify_all(sh);
		allocator_set_max_memory(sh->sh_alloc, 0);
		shadow_verify_all(sh);
		allocator_set_max_memory(sh->sh_alloc, 2 * unit);
		shadow_verify_all(sh);
		/* Budget exactly equal to bytes in use */
		allocator_set_max_memory(sh->sh_alloc, unit);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	/*
	 * Sparse data: one record stored high above a sea of never-written
	 * records. Exercises resize paths where the memory region below the
	 * split contains untouched (never-reified) pages.
	 */
	{
		shadow_t *sh = shadow_init(rsize, 0, B_TRUE,
		    recs_per_unit * 8);
		shadow_store(sh, recs_per_unit * 6);	/* Disk-side */
		shadow_verify_all(sh);
		for (int k = 0; k <= 8; k += 2) {
			allocator_set_max_memory(sh->sh_alloc, k * unit);
			shadow_verify_all(sh);
		}
		allocator_set_max_memory(sh->sh_alloc, 0);
		shadow_verify_all(sh);
		shadow_fini(sh);
	}

	/* Disk-only -> dual -> disk-only cycles with fresh data each stop */
	{
		shadow_t *sh = shadow_init(rsize, 0, B_TRUE, 512);
		selftest_rng_t rng;
		selftest_rng_init(&rng, 43);
		for (int cycle = 0; cycle < 6; cycle++) {
			for (int i = 0; i < 100; i++)
				shadow_store(sh, selftest_rng_below(&rng, 512));
			allocator_set_max_memory(sh->sh_alloc,
			    (cycle % 3) * unit);
			shadow_verify_all(sh);
		}
		shadow_fini(sh);
	}

	/* Budget far beyond the highest record ever written */
	{
		shadow_t *sh = shadow_init(rsize, 0, B_TRUE, 64);
		shadow_append(sh);
		allocator_set_max_memory(sh->sh_alloc, 64 * unit);
		shadow_verify_all(sh);
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
		shadow_t *sh = shadow_init(64, i % 2 ? 0 : 1 << 16, B_TRUE,
		    256);
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

static uint64_t
file_disk_usage(int fd)
{
	struct stat st;
	VERIFY3S(fsync(fd), ==, 0);
	VERIFY3S(fstat(fd, &st), ==, 0);
	return ((uint64_t)st.st_blocks * 512);
}

/*
 * Wait for a file's actual disk consumption to drop below (or rise above)
 * a threshold. On some filesystems - ZFS included - st_blocks reflects
 * space accounting only after the current transaction group syncs, so an
 * immediate check after a write or hole punch can be misleading.
 */
static boolean_t
wait_for_disk_usage(int fd, boolean_t below, uint64_t threshold)
{
	for (int i = 0; i < 100; i++) {
		uint64_t usage = file_disk_usage(fd);
		if (below ? usage < threshold : usage > threshold)
			return (B_TRUE);
		(void) usleep(100 * 1000);
	}
	return (B_FALSE);
}

/*
 * Direct test of punch_hole() from zstream_util.c: punch a misaligned
 * range in the middle of a pattern-filled file. The punched range must
 * read as zeros, bytes outside it must be untouched, and (if the punch
 * succeeded) the file must actually consume less disk space.
 */
static void
alloc_punch_hole(void)
{
	const size_t fsize = 8 << 20;
	const off_t hole_off = (1 << 20) + 123;
	const off_t hole_len = (4 << 20) + 55;

	int fd = selftest_create_tempfile();
	uint8_t *pattern = safe_malloc(fsize);
	fill_record(pattern, fsize, 0xdeadbeef);
	safe_pwrite(fd, pattern, fsize, 0);
	uint64_t usage_before = file_disk_usage(fd);

	if (punch_hole(fd, hole_off, hole_len) != 0) {
		(void) printf("(punch_hole unsupported here: %s) ",
		    strerror(errno));
		free(pattern);
		(void) close(fd);
		return;
	}

	uint8_t *readback = safe_malloc(fsize);
	safe_pread(fd, readback, fsize, 0);
	VERIFY0(memcmp(readback, pattern, hole_off));
	if (!all_zero(readback + hole_off, hole_len))
		errx(1, "punched range does not read as zeros");
	VERIFY0(memcmp(readback + hole_off + hole_len,
	    pattern + hole_off + hole_len, fsize - hole_off - hole_len));

	if (!wait_for_disk_usage(fd, B_TRUE, usage_before - (2 << 20))) {
		errx(1, "punch_hole reported success but disk usage did not "
		    "drop (%ju bytes before, %ju after)",
		    (uintmax_t)usage_before, (uintmax_t)file_disk_usage(fd));
	}

	free(pattern);
	free(readback);
	(void) close(fd);
}

/*
 * When the memory budget grows over a disk-resident region, the file
 * region ends up hidden under the memory overlay and should be
 * hole-punched so it stops consuming disk space. Shrinking the budget
 * again must bring the disk usage back.
 */
static void
alloc_holes(void)
{
	const size_t rsize = 4096;
	const uint64_t cap = 2304;		/* 9MB of records */
	const size_t data_bytes = cap * rsize;

	/* Probe for hole support first */
	{
		int fd = selftest_create_tempfile();
		uint8_t buf[4096] = { 1 };
		safe_pwrite(fd, buf, sizeof (buf), 1 << 20);
		int ret = punch_hole(fd, 0, (1 << 20) + sizeof (buf));
		(void) close(fd);
		if (ret != 0) {
			(void) printf("(no hole support, skipping) ");
			return;
		}
	}

	shadow_t *sh = shadow_init(rsize, 1 << 20, B_TRUE, cap);
	while (sh->sh_count < cap)
		shadow_append(sh);
	shadow_verify_all(sh);

	/* Nearly all the data should be on disk right now */
	if (!wait_for_disk_usage(sh->sh_fd, B_FALSE, data_bytes / 2)) {
		errx(1, "expected most of %zu data bytes on disk, found "
		    "%ju", data_bytes, (uintmax_t)file_disk_usage(sh->sh_fd));
	}

	/* Growing memory over the whole file should leave it a hole */
	allocator_set_max_memory(sh->sh_alloc, data_bytes + (1 << 20));
	shadow_verify_all(sh);
	if (!wait_for_disk_usage(sh->sh_fd, B_TRUE, data_bytes / 4)) {
		errx(1, "file not deallocated after memory growth "
		    "(%ju bytes still allocated)",
		    (uintmax_t)file_disk_usage(sh->sh_fd));
	}

	/* Shrinking back should rematerialize the data on disk */
	allocator_set_max_memory(sh->sh_alloc, 0);
	shadow_verify_all(sh);
	if (!wait_for_disk_usage(sh->sh_fd, B_FALSE, data_bytes / 2)) {
		errx(1, "data not written back to disk after memory "
		    "shrink (%ju bytes allocated)",
		    (uintmax_t)file_disk_usage(sh->sh_fd));
	}

	shadow_fini(sh);
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
 * Shrinking the memory budget must actually return pages to the OS, and
 * pages that are re-exposed by a later grow must read as zeros (stale
 * content must not survive the round trip). The RSS check is Linux-only;
 * the zero-fill and integrity checks run everywhere.
 */
static void
alloc_memory_release(void)
{
	const size_t rsize = 4096;
	const uint64_t cap = 8192;		/* 32MB of records */
	const size_t data_bytes = cap * rsize;

	shadow_t *sh = shadow_init(rsize, data_bytes, B_TRUE, cap + 8);
	while (sh->sh_count < cap)
		shadow_append(sh);
	shadow_verify_all(sh);

	allocator_stats_t stats = allocator_get_stats(sh->sh_alloc);
	VERIFY3U(stats.as_mem_used, ==, data_bytes);

#if defined(__linux__)
	size_t rss_full = current_rss();
#endif
	allocator_set_max_memory(sh->sh_alloc, 2 << 20);
	stats = allocator_get_stats(sh->sh_alloc);
	VERIFY3U(stats.as_mem_used, ==, 2 << 20);
#if defined(__linux__)
	size_t rss_shrunk = current_rss();
	if (rss_full < rss_shrunk ||
	    rss_full - rss_shrunk < data_bytes / 2) {
		errx(1, "shrinking budget did not release memory "
		    "(RSS %zu -> %zu)", rss_full, rss_shrunk);
	}
#endif
	shadow_verify_all(sh);

	/* Round trip: everything to disk, then everything back to memory */
	allocator_set_max_memory(sh->sh_alloc, 0);
	stats = allocator_get_stats(sh->sh_alloc);
	VERIFY3U(stats.as_mem_used, ==, 0);
	allocator_set_max_memory(sh->sh_alloc, data_bytes + (1 << 20));
	shadow_verify_all(sh);		/* Includes beyond-end zero probes */

	shadow_fini(sh);
}

/*
 * I/O operations must be attributed to the correct side of the split.
 */
static void
alloc_io_stats(void)
{
	const size_t rsize = 8192;

	shadow_t *sh = shadow_init(rsize, rsize, B_TRUE, 16);
	shadow_store(sh, 0);			/* Memory side */
	shadow_store(sh, 1);			/* Disk side */
	allocator_stats_t stats = allocator_get_stats(sh->sh_alloc);
	VERIFY3U(stats.as_io_ops_mem, ==, 1);
	VERIFY3U(stats.as_io_ops_disk, ==, 1);

	shadow_verify(sh, 0);
	shadow_verify(sh, 1);
	stats = allocator_get_stats(sh->sh_alloc);
	VERIFY3U(stats.as_io_ops_mem, ==, 2);
	VERIFY3U(stats.as_io_ops_disk, ==, 2);
	VERIFY3U(stats.as_num_records, ==, 2);
	VERIFY3U(stats.as_disk_used, ==, rsize);
	VERIFY3U(stats.as_mem_used, ==, rsize);
	VERIFY3U(stats.as_max_memory, ==, rsize);
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
		size_t rounded = P2ROUNDUP(rsize, 8);
		uint64_t cap = 1024 + selftest_rng_below(&rng, 1024);
		boolean_t disk = selftest_rng_below(&rng, 4) != 0;
		size_t max_budget = 2 * cap * rounded;
		size_t budget = disk ?
		    selftest_rng_below(&rng, max_budget) : max_budget;

		shadow_t *sh = shadow_init(rsize, budget, disk, cap);

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
			} else if (k < 85) {
				shadow_verify(sh,
				    selftest_rng_below(&rng, cap));
			} else if (disk) {
				allocator_set_max_memory(sh->sh_alloc,
				    selftest_rng_below(&rng, max_budget));
			} else {
				/*
				 * Memory-only: the whole addressable range
				 * (any index below cap) must stay within
				 * the budget, since there is no disk to
				 * spill to.
				 */
				size_t floor = cap * rounded;
				allocator_set_max_memory(sh->sh_alloc,
				    floor + selftest_rng_below(&rng,
				    max_budget - floor));
			}
		}
		shadow_verify_all(sh);
		shadow_fini(sh);
	}
}

const test_case_t selftest_alloc_cases[] = {
	{ "alloc_basic",		alloc_basic },
	{ "alloc_record_sizes",		alloc_record_sizes },
	{ "alloc_split_sweep",		alloc_split_sweep },
	{ "alloc_boundaries",		alloc_boundaries },
	{ "alloc_lifecycle",		alloc_lifecycle },
	{ "alloc_punch_hole",		alloc_punch_hole },
	{ "alloc_holes",		alloc_holes },
	{ "alloc_memory_release",	alloc_memory_release },
	{ "alloc_io_stats",		alloc_io_stats },
	{ "alloc_stress",		alloc_stress },
	{ NULL,				NULL },
};
