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
 * These tests are built around a shadow model. A shadow_group_t wraps an
 * allocator group together with a record of what every index of every
 * member should contain: a deterministic pattern derived from a tag (for
 * stored records) or zeros (for records never written). After any sequence
 * of operations, the whole record space can be swept and compared against
 * the model byte for byte.
 *
 * The tests also look inside the group through zstream_alloc_impl.h.
 * group_audit() checks the invariants that hold at all times, and every
 * shadowed operation checks how that operation was allowed to change the
 * group:
 *
 * - Only the member allocator being written may gain memory.
 * - Memory clawback victims give up memory in strict priority order.
 * - Victims yield only as much memory as was requested.
 * - Reads change nothing.
 * - No allocator's memory/disk split point ever moves in the more-memory
 *   direction.
 *
 * The layout arithmetic is restated here rather than called. Every stored
 * and verified record is also checked in place, in the allocator's memory
 * or in its backing file, at the offset test_record_offset() predicts.
 *
 * Paths that end the process, or that need resource limits the rest of the
 * run must not inherit, run in forked children (selftest_expect_exit() and
 * selftest_expect_success()).
 */

#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/sysmacros.h>

#include "zstream_alloc.h"
#include "zstream_alloc_impl.h"
#include "zstream_selftest.h"
#include "zstream_util.h"

/*
 * A deliberate duplicate of PAGE_CLUSTER_SIZE. If the cluster policy
 * changes, this must be changed by hand.
 */
#define	TEST_PAGE_CLUSTER_SIZE	(1 << 20)

/*
 * Groups have no size limit, but the shadow model needs one. This is more
 * than the hash table's three members, so larger groups get exercised too.
 */
#define	TEST_MAX_MEMBERS	8

typedef enum {
	REC_EMPTY = 0,		/* Never written; must read as zeros */
	REC_KNOWN,		/* Stored; content derived from sh_tags[] */
} record_state_t;

typedef struct {
	size_t		sh_record_size;
	uint64_t	sh_capacity;	/* Model size, in records */
	uint64_t	sh_count;	/* Highest index written, plus one */
	uint64_t	*sh_tags;
	uint8_t		*sh_state;
} shadow_member_t;

typedef struct {
	allocator_group_t  *sg_group;
	unsigned	   sg_n;
	boolean_t	   sg_disk;	/* Has backing files */
	size_t		   sg_limit;	/* Memory limit as requested */
	uint64_t	   sg_next_tag;
	uint8_t		   *sg_buf;	/* Scratch: retrieved record */
	uint8_t		   *sg_expect;	/* Scratch: expected record */
	uint8_t		   *sg_raw;	/* Scratch: record read in place */
	shadow_member_t	   sg_members[TEST_MAX_MEMBERS];
} shadow_group_t;

typedef struct {
	off_t		ss_frontier[TEST_MAX_MEMBERS];
	off_t		ss_fod[TEST_MAX_MEMBERS];
} group_snapshot_t;

#define	MEMBER(sg, m)	((sg)->sg_group->ag_allocators[m])
#define	FRONTIER(sg, m)	((size_t)MEMBER(sg, m)->a_writable_frontier)
#define	FOD(sg, m)	((size_t)MEMBER(sg, m)->a_first_on_disk)

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
cluster_size(void)
{
	return (MAX(TEST_PAGE_CLUSTER_SIZE, (size_t)sysconf(_SC_PAGESIZE)));
}

static uint64_t
recs_per_cluster(size_t record_size)
{
	return (cluster_size() / record_size);
}

/*
 * Memory needed to hold records [0, nrecs) entirely on the memory side.
 */
static size_t
bytes_for(size_t record_size, uint64_t nrecs)
{
	uint64_t rpc = recs_per_cluster(record_size);
	return (((nrecs + rpc - 1) / rpc) * cluster_size());
}

/*
 * Byte offset of a record, restated from record_offset() in the allocator.
 * Records are packed into clusters and never straddle a cluster boundary.
 */
static uint64_t
test_record_offset(size_t record_size, uint64_t ix)
{
	uint64_t rpc = recs_per_cluster(record_size);
	return ((ix / rpc) * cluster_size() + (ix % rpc) * record_size);
}

static void
take_snapshot(shadow_group_t *sg, group_snapshot_t *ss)
{
	for (unsigned m = 0; m < sg->sg_n; m++) {
		ss->ss_frontier[m] = MEMBER(sg, m)->a_writable_frontier;
		ss->ss_fod[m] = MEMBER(sg, m)->a_first_on_disk;
	}
}

/*
 * Invariants that hold across any two operations.
 */
static void
group_audit(shadow_group_t *sg)
{
	allocator_group_t *g = sg->sg_group;
	size_t cluster = cluster_size();
	size_t total = 0;

	VERIFY3U(g->ag_cluster_size, ==, cluster);
	VERIFY3U(g->ag_num_allocators, ==, sg->sg_n);
	VERIFY3U(g->ag_mem_limit, ==, roundup(sg->sg_limit, cluster));
	VERIFY3S(sg->sg_disk, ==, g->ag_dir != NULL);
	if (sg->sg_disk)
		VERIFY0(strcmp(g->ag_dir, selftest_scratch_dir()));
	for (unsigned m = 0; m < sg->sg_n; m++) {
		allocator_t *a = MEMBER(sg, m);
		shadow_member_t *sh = &sg->sg_members[m];

		VERIFY3P(a->a_group, ==, g);
		VERIFY3U(a->a_record_size, ==, sh->sh_record_size);
		VERIFY3U(a->a_records_per_cluster, ==,
		    recs_per_cluster(sh->sh_record_size));
		VERIFY3U(a->a_count, ==, sh->sh_count);
		VERIFY3S(sg->sg_disk, ==, a->a_fd >= 0);
		VERIFY3S(g->ag_mem_limit > 0, ==, a->a_base_addr != NULL);
		VERIFY3S(a->a_writable_frontier, >=, 0);
		VERIFY3S(a->a_writable_frontier, <=, a->a_first_on_disk);
		VERIFY3U(a->a_first_on_disk, <=, g->ag_mem_limit);
		VERIFY0(a->a_writable_frontier % cluster);
		VERIFY0(a->a_first_on_disk % cluster);
		total += a->a_writable_frontier;
	}
	VERIFY3U(g->ag_mem_current, ==, total);
	VERIFY3U(g->ag_mem_current, <=, g->ag_mem_limit);
}

/*
 * Check how one operation changed the group. The writer is the member that
 * was stored to, or -1 for an operation that must change nothing (a read).
 */
static void
check_transition(shadow_group_t *sg, const group_snapshot_t *before,
    int writer)
{
	allocator_group_t *g = sg->sg_group;
	int highest_victim = -1;

	group_audit(sg);
	for (int m = 0; m < (int)sg->sg_n; m++) {
		off_t frontier = MEMBER(sg, m)->a_writable_frontier;
		off_t fod = MEMBER(sg, m)->a_first_on_disk;

		/* The memory/disk split never moves up */
		VERIFY3S(fod, <=, before->ss_fod[m]);
		if (m == writer) {
			VERIFY3S(frontier, >=, before->ss_frontier[m]);
			continue;
		}
		/* Only the writer can gain memory... */
		VERIFY3S(frontier, <=, before->ss_frontier[m]);
		if (frontier == before->ss_frontier[m]) {
			/* ...and only victims move their split */
			VERIFY3S(fod, ==, before->ss_fod[m]);
			continue;
		}
		/* Victims have lower priority and lose memory for good */
		VERIFY3S(m, <, writer);
		VERIFY3S(fod, ==, frontier);
		highest_victim = m;
	}

	/* Victims are drained strictly in priority order */
	for (int m = 0; m < highest_victim; m++)
		VERIFY0(FRONTIER(sg, m));

	if (writer < 0)
		return;
	off_t frontier = MEMBER(sg, writer)->a_writable_frontier;
	off_t fod = MEMBER(sg, writer)->a_first_on_disk;
	if (fod < before->ss_fod[writer]) {
		/* Pinned: it gained nothing, and there was no memory to take */
		VERIFY3S(frontier, ==, before->ss_frontier[writer]);
		VERIFY3S(fod, ==, frontier);
		for (int m = 0; m < writer; m++)
			VERIFY0(FRONTIER(sg, m));
	} else if (highest_victim >= 0) {
		/* An eviction takes exactly the shortfall */
		VERIFY3U(g->ag_mem_current, ==, g->ag_mem_limit);
	}
}

/*
 * Check that a record's bytes are where the restated layout says they
 * should be: in memory below the member's split, in its file above it.
 */
static void
check_placement(shadow_group_t *sg, unsigned m, uint64_t ix,
    const uint8_t *expect)
{
	allocator_t *a = MEMBER(sg, m);
	size_t rsize = a->a_record_size;
	uint64_t off = test_record_offset(rsize, ix);

	if (off < (uint64_t)a->a_first_on_disk) {
		VERIFY3U(off + rsize, <=, a->a_writable_frontier);
		VERIFY0(memcmp((char *)a->a_base_addr + off, expect, rsize));
	} else {
		VERIFY3S(a->a_fd, >=, 0);
		VERIFY3S(pread(a->a_fd, sg->sg_raw, rsize, off), ==, rsize);
		VERIFY0(memcmp(sg->sg_raw, expect, rsize));
	}
}

static shadow_group_t *
shadow_group_init(const size_t *sizes, unsigned n, size_t limit,
    boolean_t disk, const uint64_t *capacities)
{
	shadow_group_t *sg = safe_calloc(sizeof (shadow_group_t));
	allocator_t **allocs = allocator_group_init(sizes, n, limit,
	    disk ? selftest_scratch_dir() : NULL);

	sg->sg_group = allocs[0]->a_group;
	VERIFY3P(allocs, ==, &sg->sg_group->ag_allocators[0]);
	sg->sg_n = n;
	sg->sg_disk = disk;
	sg->sg_limit = limit;
	sg->sg_next_tag = 1;
	sg->sg_buf = safe_malloc(MAX_RECORD_SIZE);
	sg->sg_expect = safe_malloc(MAX_RECORD_SIZE);
	sg->sg_raw = safe_malloc(MAX_RECORD_SIZE);
	for (unsigned m = 0; m < n; m++) {
		shadow_member_t *sh = &sg->sg_members[m];
		sh->sh_record_size = sizes[m];
		sh->sh_capacity = capacities[m];
		sh->sh_tags = safe_calloc(capacities[m] * sizeof (uint64_t));
		sh->sh_state = safe_calloc(capacities[m]);
	}

	/* A new group starts with no memory and an unmoved split */
	group_audit(sg);
	for (unsigned m = 0; m < n; m++) {
		VERIFY0(FRONTIER(sg, m));
		VERIFY3U(FOD(sg, m), ==, sg->sg_group->ag_mem_limit);
	}
	return (sg);
}

static shadow_group_t *
shadow_init1(size_t record_size, size_t limit, boolean_t disk,
    uint64_t capacity)
{
	return (shadow_group_init(&record_size, 1, limit, disk, &capacity));
}

/*
 * Destroy the group by way of member "via", which needn't be member 0.
 */
static void
shadow_group_fini(shadow_group_t *sg, unsigned via)
{
	group_audit(sg);
	allocator_group_fini(MEMBER(sg, via));
	for (unsigned m = 0; m < sg->sg_n; m++) {
		free(sg->sg_members[m].sh_tags);
		free(sg->sg_members[m].sh_state);
	}
	free(sg->sg_buf);
	free(sg->sg_expect);
	free(sg->sg_raw);
	free(sg);
}

static void
store_and_check(shadow_group_t *sg, unsigned m, uint64_t ix,
    boolean_t append)
{
	shadow_member_t *sh = &sg->sg_members[m];
	group_snapshot_t before;

	VERIFY3U(ix, <, sh->sh_capacity);
	take_snapshot(sg, &before);
	uint64_t tag = sg->sg_next_tag++;
	fill_record(sg->sg_expect, sh->sh_record_size, tag);
	if (append) {
		VERIFY3U(allocator_append(MEMBER(sg, m), sg->sg_expect), ==,
		    ix);
	} else {
		allocator_store(MEMBER(sg, m), ix, sg->sg_expect);
	}
	sh->sh_tags[ix] = tag;
	sh->sh_state[ix] = REC_KNOWN;
	sh->sh_count = MAX(sh->sh_count, ix + 1);
	check_transition(sg, &before, m);
	check_placement(sg, m, ix, sg->sg_expect);
}

static void
shadow_store(shadow_group_t *sg, unsigned m, uint64_t ix)
{
	store_and_check(sg, m, ix, B_FALSE);
}

static void
shadow_append(shadow_group_t *sg, unsigned m)
{
	store_and_check(sg, m, sg->sg_members[m].sh_count, B_TRUE);
}

/*
 * Append exactly n clusters' worth of records. Starting from a cluster
 * boundary, this extends the member by exactly n clusters.
 */
static void
shadow_append_clusters(shadow_group_t *sg, unsigned m, uint64_t n)
{
	uint64_t rpc = recs_per_cluster(sg->sg_members[m].sh_record_size);
	for (uint64_t i = 0; i < n * rpc; i++)
		shadow_append(sg, m);
}

static void
shadow_skip(shadow_group_t *sg, unsigned m)
{
	shadow_member_t *sh = &sg->sg_members[m];
	group_snapshot_t before;

	VERIFY3U(sh->sh_count, <, sh->sh_capacity);
	take_snapshot(sg, &before);
	VERIFY3U(allocator_skip(MEMBER(sg, m)), ==, sh->sh_count);
	sh->sh_state[sh->sh_count++] = REC_EMPTY;
	check_transition(sg, &before, m);
}

static void
shadow_verify(shadow_group_t *sg, unsigned m, uint64_t ix)
{
	shadow_member_t *sh = &sg->sg_members[m];
	size_t rsize = sh->sh_record_size;
	group_snapshot_t before;

	VERIFY3U(ix, <, sh->sh_capacity);
	take_snapshot(sg, &before);
	(void) memset(sg->sg_buf, 0xa5, rsize);
	allocator_retrieve(MEMBER(sg, m), ix, sg->sg_buf);
	check_transition(sg, &before, -1);
	if (sh->sh_state[ix] == REC_KNOWN) {
		fill_record(sg->sg_expect, rsize, sh->sh_tags[ix]);
		if (memcmp(sg->sg_buf, sg->sg_expect, rsize) != 0) {
			errx(1, "member %u record %llu corrupted", m,
			    (u_longlong_t)ix);
		}
		/*
		 * Disk placement is checked at store time. Evicted records
		 * keep their offsets, so skip the extra syscall here.
		 */
		if (test_record_offset(rsize, ix) < FOD(sg, m))
			check_placement(sg, m, ix, sg->sg_expect);
	} else if (!all_zero(sg->sg_buf, rsize)) {
		errx(1, "member %u unwritten record %llu is not zero-filled",
		    m, (u_longlong_t)ix);
	}
}

/*
 * Read a record that has never been written, at any index, including ones
 * beyond the model's capacity. It must read as zeros and change nothing.
 */
static void
verify_unwritten(shadow_group_t *sg, unsigned m, uint64_t ix)
{
	shadow_member_t *sh = &sg->sg_members[m];
	group_snapshot_t before;

	VERIFY3U(ix, >=, sh->sh_count);
	take_snapshot(sg, &before);
	(void) memset(sg->sg_buf, 0xa5, sh->sh_record_size);
	allocator_retrieve(MEMBER(sg, m), ix, sg->sg_buf);
	check_transition(sg, &before, -1);
	if (!all_zero(sg->sg_buf, sh->sh_record_size)) {
		errx(1, "member %u unwritten record %llu is not zero-filled",
		    m, (u_longlong_t)ix);
	}
}

/*
 * Check a record and its immediate neighbors.
 */
static void
shadow_verify_around(shadow_group_t *sg, unsigned m, uint64_t ix)
{
	uint64_t cap = sg->sg_members[m].sh_capacity;
	for (uint64_t i = (ix > 0) ? ix - 1 : 0; i <= ix + 1 && i < cap; i++)
		shadow_verify(sg, m, i);
}

/*
 * Sweep every member's record space against the model: every record below
 * the member's count, plus a few probes beyond the end, which must read as
 * zeros.
 */
static void
shadow_verify_all(shadow_group_t *sg)
{
	for (unsigned m = 0; m < sg->sg_n; m++) {
		shadow_member_t *sh = &sg->sg_members[m];
		for (uint64_t ix = 0; ix < sh->sh_count; ix++)
			shadow_verify(sg, m, ix);
		for (uint64_t ix = sh->sh_count;
		    ix < MIN(sh->sh_count + 3, sh->sh_capacity); ix++)
			shadow_verify(sg, m, ix);
	}
}

/*
 * Cluster selection and record layout for single-member groups (disk-only,
 * memory-only, dual-backed) at record sizes with awkward relationships
 * to the cluster size.
 *
 * - The last record of each cluster and the first record of the next
 *   must land exactly where test_record_offset() says.
 *
 * - The frontier must advance a cluster at a time, and the gaps must
 *   read as zeros.
 *
 * - Also check rounding of the memory limit with a group of mixed
 *   record sizes.
 */
static void
alloc_layout(void)
{
	static const size_t sizes[] = { 1, 3, 7, 8, 24, 40, 72, 104, 1000,
	    4095, 4096, 4097, 5000, 8191, MAX_RECORD_SIZE };
	const size_t cluster = cluster_size();

	for (size_t s = 0; s < ARRAY_SIZE(sizes); s++) {
		size_t rsize = sizes[s];
		uint64_t rpc = recs_per_cluster(rsize);
		uint64_t ixs[] = { rpc - 1, 0, rpc, 2 * rpc - 1, 2 * rpc,
		    3 * rpc + 1 };

		/* Under 1% of each cluster is wasted */
		VERIFY3U(100 * (cluster % rsize), <, cluster);

		for (int config = 0; config < 3; config++) {
			/* Config 0 is dual, 1 is disk-only, 2 is memory-only */
			size_t limit = (config == 0) ? 2 * cluster :
			    (config == 1) ? 0 : 4 * cluster;
			boolean_t disk = (config != 2);
			shadow_group_t *sg = shadow_init1(rsize, limit, disk,
			    3 * rpc + 3);

			/* The frontier covers the highest cluster written */
			size_t want = 0;
			for (int i = 0; i < ARRAY_SIZE(ixs); i++) {
				shadow_store(sg, 0, ixs[i]);
				want = MIN(limit,
				    MAX(want, (ixs[i] / rpc + 1) * cluster));
				VERIFY3U(FRONTIER(sg, 0), ==, want);
			}
			for (int i = 0; i < ARRAY_SIZE(ixs); i++)
				shadow_verify_around(sg, 0, ixs[i]);

			VERIFY3U(FRONTIER(sg, 0), ==, limit);
			VERIFY3U(FOD(sg, 0), ==, sg->sg_group->ag_mem_limit);
			shadow_group_fini(sg, 0);
		}
	}

	/* The limit is rounded up to whole clusters, whatever the sizes */
	static const size_t group_sizes[] = { 24, 104, 104 };
	static const uint64_t caps[] = { 16, 16, 16 };
	const size_t limits[] = { 0, 1, cluster - 1, cluster, cluster + 1,
	    5 * cluster + 7 };
	for (int i = 0; i < ARRAY_SIZE(limits); i++) {
		shadow_group_t *sg = shadow_group_init(group_sizes, 3,
		    limits[i], B_TRUE, caps);
		for (unsigned m = 0; m < 3; m++)
			shadow_append(sg, m);
		shadow_verify_all(sg);
		shadow_group_fini(sg, 2);
	}
}

/*
 * Basic operation of all three configurations: memory-only, disk-only,
 * and dual-backed. Checks round-trip integrity, append/skip index
 * sequencing, zero-fill of unwritten records, overwrites, and stores that
 * leave gaps.
 */
static void
alloc_basic(void)
{
	const size_t rsize = 1000;
	const uint64_t cap = 4 * recs_per_cluster(rsize);
	const size_t full = bytes_for(rsize, cap);

	for (int config = 0; config < 3; config++) {
		boolean_t disk = (config != 0);
		size_t mem;
		switch (config) {
		case 0:
			/* Memory-only */
			mem = 2 * full;
			break;
		case 1:
			/* Disk-only */
			mem = 0;
			break;
		default:
			/* Dual: about a quarter of the data fits */
			mem = full / 4;
			break;
		}

		shadow_group_t *sg = shadow_init1(rsize, mem, disk, cap);

		for (int i = 0; i < 200; i++)
			shadow_append(sg, 0);
		for (int i = 0; i < 3; i++)
			shadow_skip(sg, 0);
		for (int i = 0; i < 5; i++)
			shadow_append(sg, 0);

		/* Overwrites and stores that leave gaps */
		shadow_store(sg, 0, 0);
		shadow_store(sg, 0, 100);
		shadow_store(sg, 0, 1500);
		shadow_store(sg, 0, cap - 1);
		shadow_verify_all(sg);
		VERIFY3U(sg->sg_members[0].sh_count, ==, cap);

		/* Overwrite the same record repeatedly */
		for (int i = 0; i < 10; i++)
			shadow_store(sg, 0, 77);
		shadow_verify(sg, 0, 77);

		/*
		 * A disk-only allocator must never use memory; the other
		 * two must, since record 0 is always on the memory side.
		 */
		if (config == 1)
			VERIFY0(FRONTIER(sg, 0));
		else
			VERIFY3U(FRONTIER(sg, 0), >, 0);

		shadow_group_fini(sg, 0);
	}
}

/*
 * A spectrum of record sizes, each filled past a three-cluster budget and
 * then pushed out to disk one cluster at a time by a second member of the
 * group, verifying every record after each step.
 */
static void
alloc_record_sizes(void)
{
	static const size_t sizes[] = { 24, 56, 104, 512, 1000, 4095, 4096,
	    4097, 5000, 8191, MAX_RECORD_SIZE };
	const size_t cluster = cluster_size();

	for (size_t s = 0; s < ARRAY_SIZE(sizes); s++) {
		size_t group_sizes[] = { sizes[s], 4096 };
		uint64_t caps[] = { 3 * recs_per_cluster(sizes[s]) + 17,
		    3 * recs_per_cluster(4096) + 1 };
		shadow_group_t *sg = shadow_group_init(group_sizes, 2,
		    3 * cluster, B_TRUE, caps);

		/* The overflow spills to disk: member 0 can't take memory */
		while (sg->sg_members[0].sh_count < caps[0])
			shadow_append(sg, 0);
		VERIFY3U(FRONTIER(sg, 0), ==, 3 * cluster);
		VERIFY3U(FOD(sg, 0), ==, 3 * cluster);
		shadow_verify_all(sg);

		for (uint64_t k = 1; k <= 3; k++) {
			while (FRONTIER(sg, 1) < k * cluster)
				shadow_append(sg, 1);
			VERIFY3U(FRONTIER(sg, 0), ==, (3 - k) * cluster);
			VERIFY3U(FOD(sg, 0), ==, (3 - k) * cluster);
			shadow_verify_all(sg);
		}
		shadow_group_fini(sg, 1);
	}
}

/*
 * Moving memory among members.
 *
 * - First, a sparse store that needs several clusters at once and gets
 *   them all from one victim.
 *
 * - Second, a randomized eviction of a member whose records keep changing
 *   as the split passes through them.
 *
 * - Third, a request that drains every lower-priority member and still
 *   falls short: the requester's split is pinned, the memory it freed
 *   stays in the group, and a higher-priority member can then use that
 *   memory without evicting anyone.
 */
static void
alloc_reassignment(void)
{
	const size_t cluster = cluster_size();

	{
		size_t sizes[] = { 1000, 1000 };
		uint64_t rpc = recs_per_cluster(1000);
		uint64_t caps[] = { 8 * rpc, 8 * rpc };
		shadow_group_t *sg = shadow_group_init(sizes, 2, 8 * cluster,
		    B_TRUE, caps);
		shadow_append_clusters(sg, 0, 8);
		shadow_store(sg, 1, 5 * rpc);
		VERIFY3U(FRONTIER(sg, 1), ==, 6 * cluster);
		VERIFY3U(FRONTIER(sg, 0), ==, 2 * cluster);
		VERIFY3U(FOD(sg, 0), ==, 2 * cluster);
		shadow_verify_all(sg);
		shadow_verify_around(sg, 1, 3 * rpc);
		shadow_group_fini(sg, 0);
	}

	{
		selftest_rng_t rng;
		selftest_rng_init(&rng, 42);
		size_t sizes[] = { 104, 4096 };
		uint64_t caps[] = { 8 * recs_per_cluster(104),
		    8 * recs_per_cluster(4096) + 64 };
		shadow_group_t *sg = shadow_group_init(sizes, 2, 8 * cluster,
		    B_TRUE, caps);
		while (sg->sg_members[0].sh_count < caps[0])
			shadow_append(sg, 0);

		for (int iter = 0; FRONTIER(sg, 1) < 8 * cluster; iter++) {
			for (int i = 0; i < 8; i++)
				shadow_store(sg, 0,
				    selftest_rng_below(&rng, caps[0]));
			if (selftest_rng_below(&rng, 4) == 0) {
				uint64_t n = selftest_rng_below(&rng, 64);
				for (uint64_t i = 0; i < n; i++)
					shadow_append(sg, 1);
			}
			for (int i = 0; i < 32; i++)
				shadow_verify(sg, 0,
				    selftest_rng_below(&rng, caps[0]));
			if (iter % 25 == 24)
				shadow_verify_all(sg);
		}
		VERIFY0(FRONTIER(sg, 0));
		shadow_verify_all(sg);
		shadow_group_fini(sg, 1);
	}

	{
		size_t sizes[] = { 4096, 4096, 4096, 4096 };
		uint64_t rpc = recs_per_cluster(4096);
		uint64_t caps[] = { rpc, 2 * rpc, 7 * rpc + 1, 5 * rpc + 1 };
		shadow_group_t *sg = shadow_group_init(sizes, 4, 8 * cluster,
		    B_TRUE, caps);
		shadow_append_clusters(sg, 0, 1);
		shadow_append_clusters(sg, 1, 2);
		shadow_append_clusters(sg, 2, 4);
		shadow_append_clusters(sg, 3, 1);

		/* Needs 4 clusters; members 0 and 1 can only give 3 */
		shadow_store(sg, 2, 7 * rpc);
		VERIFY0(FRONTIER(sg, 0));
		VERIFY0(FRONTIER(sg, 1));
		VERIFY3U(FRONTIER(sg, 2), ==, 4 * cluster);
		VERIFY3U(FOD(sg, 2), ==, 4 * cluster);
		VERIFY3U(sg->sg_group->ag_mem_current, ==, 5 * cluster);

		/* The stranded memory goes to member 3 at no one's expense */
		shadow_append_clusters(sg, 3, 3);
		VERIFY3U(FRONTIER(sg, 2), ==, 4 * cluster);
		VERIFY3U(sg->sg_group->ag_mem_current, ==, 8 * cluster);

		/* After that, member 3 grows at member 2's expense */
		shadow_append(sg, 3);
		VERIFY3U(FRONTIER(sg, 2), ==, 3 * cluster);
		shadow_verify_all(sg);
		shadow_group_fini(sg, 3);
	}
}

/*
 * Test the eviction order in a four-member group, step by step:
 *
 * - Allocator 0 can never take memory.
 * - Others can confiscate it from lower-numbered members (lowest first).
 * - Under sustained load, the last member ends up with all memory.
 */
static void
alloc_priority(void)
{
	const size_t cluster = cluster_size();
	const uint64_t rpc = recs_per_cluster(4096);
	size_t sizes[] = { 4096, 4096, 4096, 4096 };
	uint64_t caps[] = { 2 * rpc, 3 * rpc, 2 * rpc, 6 * rpc };
	shadow_group_t *sg = shadow_group_init(sizes, 4, 4 * cluster,
	    B_TRUE, caps);

	/* Fill the group, one cluster per member */
	for (unsigned m = 0; m < 4; m++)
		shadow_append_clusters(sg, m, 1);
	for (unsigned m = 0; m < 4; m++)
		VERIFY3U(FRONTIER(sg, m), ==, cluster);

	/* Member 0 has no one to take from, so it spills to disk */
	shadow_append(sg, 0);
	VERIFY3U(FRONTIER(sg, 0), ==, cluster);
	VERIFY3U(FOD(sg, 0), ==, cluster);

	/* Member 1 takes from member 0 */
	shadow_append_clusters(sg, 1, 1);
	VERIFY0(FRONTIER(sg, 0));
	VERIFY0(FOD(sg, 0));
	VERIFY3U(FRONTIER(sg, 1), ==, 2 * cluster);
	VERIFY3U(FRONTIER(sg, 2), ==, cluster);

	/* Member 3 drains member 1 before touching member 2 */
	shadow_append_clusters(sg, 3, 1);
	VERIFY3U(FRONTIER(sg, 1), ==, cluster);
	VERIFY3U(FRONTIER(sg, 2), ==, cluster);
	shadow_append_clusters(sg, 3, 1);
	VERIFY0(FRONTIER(sg, 1));
	VERIFY3U(FRONTIER(sg, 2), ==, cluster);
	shadow_append_clusters(sg, 3, 1);
	VERIFY0(FRONTIER(sg, 2));
	VERIFY3U(FRONTIER(sg, 3), ==, 4 * cluster);

	/* Drained members stay on disk */
	shadow_append_clusters(sg, 2, 1);
	VERIFY0(FRONTIER(sg, 2));

	/* The last member, holding everything, spills past the limit */
	shadow_append_clusters(sg, 3, 1);
	VERIFY3U(FRONTIER(sg, 3), ==, 4 * cluster);
	VERIFY3U(FOD(sg, 3), ==, 4 * cluster);

	shadow_verify_all(sg);
	shadow_group_fini(sg, 2);
}

/*
 * Corner cases that deserve individual attention.
 */
static void
alloc_boundaries(void)
{
	const size_t cluster = cluster_size();
	const size_t rsize = 24;
	const uint64_t rpc = recs_per_cluster(rsize);

	/* A member that never had memory, in a full group */
	{
		size_t sizes[] = { rsize, 4096 };
		uint64_t caps[] = { 64, 2 * recs_per_cluster(4096) };
		shadow_group_t *sg = shadow_group_init(sizes, 2, 2 * cluster,
		    B_TRUE, caps);
		shadow_append_clusters(sg, 1, 2);
		shadow_append(sg, 0);
		VERIFY0(FRONTIER(sg, 0));
		VERIFY0(FOD(sg, 0));
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/* A single record chased onto disk */
	{
		size_t sizes[] = { rsize, 4096 };
		uint64_t caps[] = { 64, 64 };
		shadow_group_t *sg = shadow_group_init(sizes, 2, cluster,
		    B_TRUE, caps);
		shadow_append(sg, 0);
		VERIFY3U(FRONTIER(sg, 0), ==, cluster);
		shadow_append(sg, 1);
		VERIFY0(FRONTIER(sg, 0));
		VERIFY0(FOD(sg, 0));
		shadow_verify_all(sg);
		shadow_group_fini(sg, 1);
	}

	/* Data that exactly fills the limit, then the first record past it */
	{
		shadow_group_t *sg = shadow_init1(rsize, cluster, B_TRUE,
		    2 * rpc);
		shadow_append_clusters(sg, 0, 1);
		VERIFY3U(FRONTIER(sg, 0), ==, cluster);
		shadow_append(sg, 0);
		VERIFY3U(FRONTIER(sg, 0), ==, cluster);
		VERIFY3U(FOD(sg, 0), ==, cluster);
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/*
	 * A sparse store on the memory side reifies every cluster up to it.
	 * Records in the gap and above the frontier read as zeros, and
	 * reading them reifies nothing.
	 */
	for (int disk = 0; disk < 2; disk++) {
		shadow_group_t *sg = shadow_init1(rsize, 8 * cluster, disk,
		    8 * rpc);
		shadow_store(sg, 0, 5 * rpc);
		VERIFY3U(FRONTIER(sg, 0), ==, 6 * cluster);
		VERIFY3U(sg->sg_group->ag_mem_current, ==, 6 * cluster);
		shadow_verify_around(sg, 0, 0);
		shadow_verify_around(sg, 0, 3 * rpc);
		shadow_verify_around(sg, 0, 7 * rpc);
		shadow_verify(sg, 0, 8 * rpc - 1);
		VERIFY3U(FRONTIER(sg, 0), ==, 6 * cluster);
		shadow_group_fini(sg, 0);
	}

	/* Disk-only from birth */
	{
		shadow_group_t *sg = shadow_init1(rsize, 0, B_TRUE, 512);
		selftest_rng_t rng;
		selftest_rng_init(&rng, 43);
		for (int i = 0; i < 600; i++)
			shadow_store(sg, 0, selftest_rng_below(&rng, 512));
		VERIFY0(FRONTIER(sg, 0));
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/* A limit far larger than anything ever written */
	{
		shadow_group_t *sg = shadow_init1(rsize, 64 * cluster, B_TRUE,
		    64);
		shadow_append(sg, 0);
		VERIFY3U(FRONTIER(sg, 0), ==, cluster);
		VERIFY3U(FOD(sg, 0), ==, 64 * cluster);
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/*
	 * Records never written read as zeros and change nothing, in every
	 * configuration: in an empty group, above the frontier, past a
	 * memory-only allocator's split (where there is no file to read),
	 * and far beyond anything ever written.
	 */
	for (int config = 0; config < 3; config++) {
		/* Config 0 is dual, 1 is disk-only, 2 is memory-only */
		shadow_group_t *sg = shadow_init1(rsize,
		    (config == 1) ? 0 : cluster, config != 2, 3 * rpc);
		shadow_verify_all(sg);
		verify_unwritten(sg, 0, 1ULL << 30);
		shadow_append_clusters(sg, 0, 1);
		shadow_verify_around(sg, 0, rpc);
		shadow_verify_around(sg, 0, 3 * rpc - 2);
		verify_unwritten(sg, 0, 1ULL << 30);
		shadow_verify_all(sg);
		VERIFY3U(FRONTIER(sg, 0), ==, (config == 1) ? 0 : cluster);
		VERIFY3U(FOD(sg, 0), ==, sg->sg_group->ag_mem_limit);
		shadow_group_fini(sg, 0);
	}
}

#if defined(__linux__)
/*
 * Total and resident process size, in bytes.
 */
static void
process_size(size_t *total_out, size_t *resident_out)
{
	FILE *fp = fopen("/proc/self/statm", "r");
	unsigned long total, resident;

	VERIFY(fp != NULL);
	VERIFY3S(fscanf(fp, "%lu %lu", &total, &resident), ==, 2);
	(void) fclose(fp);
	size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
	if (total_out != NULL)
		*total_out = (size_t)total * pagesize;
	if (resident_out != NULL)
		*resident_out = (size_t)resident * pagesize;
}
#endif

/*
 * Repeated create/destroy cycles must not leak file descriptors or (on
 * Linux) address space. There is no limit on the number of live groups, so
 * many groups are also kept live at once, used in interleaved fashion, and
 * destroyed in scrambled order through arbitrary members.
 */
#define	MANY_GROUPS	24

static void
alloc_lifecycle(void)
{
	const size_t cluster = cluster_size();
	selftest_rng_t rng;
	selftest_rng_init(&rng, 45);
	int probe = open("/dev/null", O_RDONLY);
	VERIFY3S(probe, >=, 0);
	int baseline_fd = probe;
	(void) close(probe);
#if defined(__linux__)
	size_t vm_before;
	process_size(&vm_before, NULL);
#endif

	for (int i = 0; i < 60; i++) {
		unsigned n = 1 + i % TEST_MAX_MEMBERS;
		size_t sizes[TEST_MAX_MEMBERS];
		uint64_t caps[TEST_MAX_MEMBERS];
		for (unsigned m = 0; m < n; m++) {
			sizes[m] = 64 * (m + 1);
			caps[m] = 64;
		}
		shadow_group_t *sg = shadow_group_init(sizes, n,
		    (i % 2) ? 0 : 4 * cluster, B_TRUE, caps);
		for (unsigned m = 0; m < n; m++) {
			for (int j = 0; j < 50; j++)
				shadow_append(sg, m);
		}
		shadow_verify_all(sg);
		shadow_group_fini(sg, i % n);
	}

	/*
	 * Each group gets six half-cluster appends, split randomly between
	 * its two members: at most four clusters in all, so memory-only
	 * groups fit in four, while dual-backed groups (two clusters) must
	 * evict.
	 */
	shadow_group_t *sgs[MANY_GROUPS];
	for (int i = 0; i < MANY_GROUPS; i++) {
		size_t sizes[] = { 4096, 2048 + 8 * i };
		uint64_t caps[] = { 3 * recs_per_cluster(sizes[0]) + 1,
		    3 * recs_per_cluster(sizes[1]) + 1 };
		/* Mix dual-backed, disk-only, and memory-only groups */
		size_t limit = (i % 3 == 0) ? 2 * cluster :
		    (i % 3 == 1) ? 0 : 4 * cluster;
		sgs[i] = shadow_group_init(sizes, 2, limit, i % 3 != 2, caps);
		for (int j = 0; j < i; j++)
			VERIFY3P(sgs[i]->sg_group, !=, sgs[j]->sg_group);
	}
	for (int round = 0; round < 6; round++) {
		for (int i = 0; i < MANY_GROUPS; i++) {
			shadow_group_t *sg = sgs[i];
			unsigned m = selftest_rng_below(&rng, 2);
			uint64_t half = recs_per_cluster(
			    sg->sg_members[m].sh_record_size) / 2;
			for (uint64_t j = 0; j < half; j++)
				shadow_append(sg, m);
		}
	}
	for (int left = MANY_GROUPS; left > 0; left--) {
		int i = selftest_rng_below(&rng, left);
		shadow_verify_all(sgs[i]);
		shadow_group_fini(sgs[i], selftest_rng_below(&rng, 2));
		sgs[i] = sgs[left - 1];
	}

	probe = open("/dev/null", O_RDONLY);
	VERIFY3S(probe, >=, 0);
	if (probe > baseline_fd + 2) {
		errx(1, "file descriptors leaked: probe fd went from %d to %d",
		    baseline_fd, probe);
	}
	(void) close(probe);
#if defined(__linux__)
	/* Leaked groups of up to 8 x 4MB would be hard to miss */
	size_t vm_after;
	process_size(&vm_after, NULL);
	if (vm_after > vm_before + 16 * cluster) {
		errx(1, "address space leaked: %zu bytes before, %zu after",
		    vm_before, vm_after);
	}
#endif
}

/*
 * Evicted memory must actually be returned to the OS, not just accounted
 * for. A member is filled to the group limit and then pushed out to disk
 * by another member that writes eight times as much. Resident memory must
 * stay near the limit throughout, and every record must survive. The RSS
 * checks are Linux-only; the integrity checks run everywhere.
 */
static void
alloc_rss_bounded(void)
{
	const size_t cluster = cluster_size();
	const size_t limit = 8 * cluster;
	const uint64_t rpc = recs_per_cluster(4096);
	size_t sizes[] = { 4096, 4096 };
	uint64_t caps[] = { 8 * rpc, 64 * rpc };
	selftest_rng_t rng;
	selftest_rng_init(&rng, 44);

	shadow_group_t *sg = shadow_group_init(sizes, 2, limit, B_TRUE, caps);
#if defined(__linux__)
	size_t rss_base, rss;
	process_size(NULL, &rss_base);
#endif
	shadow_append_clusters(sg, 0, 8);
	for (uint64_t i = 0; i < 64; i++) {
		shadow_append_clusters(sg, 1, 1);
		shadow_verify(sg, 0, selftest_rng_below(&rng, caps[0]));
#if defined(__linux__)
		process_size(NULL, &rss);
		if (rss > rss_base + limit + 4 * cluster) {
			errx(1, "evicted memory was not released "
			    "(RSS %zu -> %zu, limit %zu)", rss_base, rss,
			    limit);
		}
#endif
	}
	VERIFY0(FRONTIER(sg, 0));
	VERIFY3U(FRONTIER(sg, 1), ==, limit);
	shadow_verify_all(sg);
	shadow_group_fini(sg, 0);
}

/*
 * Random operation mixes against the shadow model across randomized
 * groups. Every operation is audited. Failures replay with -s.
 */
static void
alloc_stress(void)
{
	const size_t cluster = cluster_size();
	selftest_rng_t rng;
	selftest_rng_init(&rng, 4242);

	for (int round = 0; round < 8; round++) {
		unsigned n = 1 + selftest_rng_below(&rng,
		    TEST_MAX_MEMBERS);
		boolean_t disk = selftest_rng_below(&rng, 4) != 0;
		size_t sizes[TEST_MAX_MEMBERS];
		uint64_t caps[TEST_MAX_MEMBERS];
		size_t full = 0;
		for (unsigned m = 0; m < n; m++) {
			sizes[m] = (selftest_rng_below(&rng, 2) == 0) ?
			    1 + selftest_rng_below(&rng, 128) :
			    1 + selftest_rng_below(&rng, MAX_RECORD_SIZE);
			caps[m] = MAX(16, MIN(4096, (2 * cluster) / sizes[m]));
			full += bytes_for(sizes[m], caps[m]);
		}
		/*
		 * A memory-only group can never spill, so it has to be big
		 * enough for every index of every member. Disk-backed
		 * groups get an arbitrary limit, usually not a whole
		 * number of clusters.
		 */
		size_t limit = disk ?
		    selftest_rng_below(&rng, full + cluster) : full;

		shadow_group_t *sg = shadow_group_init(sizes, n, limit, disk,
		    caps);
		for (int op = 0; op < 4000; op++) {
			unsigned m = selftest_rng_below(&rng, n);
			shadow_member_t *sh = &sg->sg_members[m];
			uint64_t k = selftest_rng_below(&rng, 100);
			if (k < 35) {
				shadow_store(sg, m,
				    selftest_rng_below(&rng, sh->sh_capacity));
			} else if (k < 50) {
				if (sh->sh_count < sh->sh_capacity)
					shadow_append(sg, m);
			} else if (k < 55) {
				if (sh->sh_count < sh->sh_capacity)
					shadow_skip(sg, m);
			} else {
				shadow_verify(sg, m,
				    selftest_rng_below(&rng, sh->sh_capacity));
			}
			if (op % 1000 == 999)
				shadow_verify_all(sg);
		}
		shadow_verify_all(sg);
		shadow_group_fini(sg, selftest_rng_below(&rng, n));
	}
}

typedef struct {
	size_t		fa_sizes[TEST_MAX_MEMBERS];
	unsigned	fa_n;
	size_t		fa_limit;
	boolean_t	fa_disk;
	const char	*fa_dir;	/* Overrides the scratch directory */
} fatal_args_t;

static void
child_group_init(void *arg)
{
	fatal_args_t *fa = arg;
	const char *dir = (fa->fa_dir != NULL) ? fa->fa_dir :
	    selftest_scratch_dir();
	(void) allocator_group_init(fa->fa_sizes, fa->fa_n, fa->fa_limit,
	    fa->fa_disk ? dir : NULL);
}

/*
 * Memory-only groups with a single cluster. With one member, the member
 * writes past the limit. With two, member 1 needs memory that member 0
 * can only give up by losing its data.
 */
static void
child_memory_only_overflow(void *arg)
{
	unsigned n = *(unsigned *)arg;
	size_t sizes[] = { 1000, 1000 };
	uint8_t buf[1000] = { 0 };
	allocator_t **a = allocator_group_init(sizes, n, cluster_size(),
	    NULL);

	(void) allocator_append(a[0], buf);
	if (n == 1) {
		for (uint64_t i = 0; i < recs_per_cluster(1000); i++)
			(void) allocator_append(a[0], buf);
	} else {
		(void) allocator_append(a[1], buf);
	}
}

/*
 * A write to a backing file that fails must be fatal, whether it comes
 * from a store on the disk side or from an eviction. Writes fail here
 * because they cross RLIMIT_FSIZE; with SIGXFSZ ignored, they return EFBIG.
 * The eviction writes a whole cluster, so its first pwrite() is partial,
 * and the failure comes on the retry.
 */
static void
child_write_failure(void *arg)
{
	boolean_t evict = *(boolean_t *)arg;
	size_t sizes[] = { 4096, 4096 };
	uint8_t buf[4096] = { 0 };
	struct rlimit fsize = { 16 * 4096, 16 * 4096 };

	(void) signal(SIGXFSZ, SIG_IGN);
	VERIFY0(setrlimit(RLIMIT_FSIZE, &fsize));
	allocator_t **a = allocator_group_init(sizes, 2,
	    evict ? cluster_size() : 0, selftest_scratch_dir());
	if (evict) {
		/* Member 0 fills the only cluster; member 1 evicts it */
		for (uint64_t i = 0; i < recs_per_cluster(4096); i++)
			(void) allocator_append(a[0], buf);
		(void) allocator_append(a[1], buf);
	} else {
		/* Disk-only appends cross the limit at record 16 */
		for (int i = 0; i < 32; i++)
			(void) allocator_append(a[0], buf);
	}
}

#if defined(__linux__)
/*
 * The process's data size, in the terms that RLIMIT_DATA counts
 */
static size_t
vm_data_bytes(void)
{
	FILE *fp = fopen("/proc/self/status", "r");
	char line[256];
	unsigned long kb = 0;

	VERIFY(fp != NULL);
	while (fgets(line, sizeof (line), fp) != NULL) {
		if (sscanf(line, "VmData: %lu kB", &kb) == 1)
			break;
	}
	(void) fclose(fp);
	VERIFY3U(kb, >, 0);
	return ((size_t)kb << 10);
}

/*
 * Under RLIMIT_DATA, mprotect() fails with ENOMEM long before the group's
 * budget runs out. A disk-backed allocator must pin its split at its
 * frontier and carry on from disk with every record intact. A memory-only
 * allocator has nowhere to go and must exit.
 */
static void
child_data_rlimit(void *arg)
{
	boolean_t disk = *(boolean_t *)arg;
	const size_t cluster = cluster_size();
	const uint64_t nrecs = 8 * recs_per_cluster(4096);
	size_t size = 4096;
	uint8_t buf[4096];
	uint8_t expect[4096];
	allocator_t **a = allocator_group_init(&size, 1, 32 * cluster,
	    disk ? selftest_scratch_dir() : NULL);
	allocator_t *alloc = a[0];
	rlim_t lim = vm_data_bytes() + 3 * cluster;
	struct rlimit data = { lim, lim };

	VERIFY0(setrlimit(RLIMIT_DATA, &data));
	for (uint64_t i = 0; i < nrecs; i++) {
		fill_record(buf, size, i + 1);
		VERIFY3U(allocator_append(alloc, buf), ==, i);
	}

	/* The split was pinned at the frontier, well short of the budget */
	VERIFY3S(alloc->a_first_on_disk, ==, alloc->a_writable_frontier);
	VERIFY3S(alloc->a_writable_frontier, <=, 3 * cluster);
	VERIFY3U(alloc->a_group->ag_mem_current, ==,
	    alloc->a_writable_frontier);
	for (uint64_t i = 0; i < nrecs; i++) {
		allocator_retrieve(alloc, i, buf);
		fill_record(expect, size, i + 1);
		if (memcmp(buf, expect, size) != 0) {
			errx(1, "record %llu corrupted after ENOMEM spill",
			    (u_longlong_t)i);
		}
	}
	allocator_group_fini(alloc);
}
#endif

/*
 * Behavior when the operating system, rather than the group's budget,
 * refuses a request. The RLIMIT_DATA cases are Linux-only, since other
 * systems apply that limit only to the heap.
 */
static void
alloc_resource_limits(void)
{
	boolean_t evict = B_FALSE;
	selftest_expect_exit("disk write failure", child_write_failure,
	    &evict, "pwrite() failed");
	evict = B_TRUE;
	selftest_expect_exit("eviction write failure", child_write_failure,
	    &evict, "pwrite() failed");
#if defined(__linux__)
	boolean_t disk = B_TRUE;
	selftest_expect_success("ENOMEM spill", child_data_rlimit, &disk,
	    "memory allocation failed, spilling to disk");
	disk = B_FALSE;
	selftest_expect_exit("ENOMEM, memory-only", child_data_rlimit, &disk,
	    "memory limit exceeded");
#endif
}

/*
 * The fatal paths, each run in a child process that must exit with the
 * expected message.
 */
static void
alloc_fatal(void)
{
	const size_t cluster = cluster_size();
	fatal_args_t fa = {
		.fa_sizes = { MAX_RECORD_SIZE + 1 },
		.fa_n = 1,
		.fa_limit = cluster,
		.fa_disk = B_TRUE
	};
	selftest_expect_exit("oversized record", child_group_init, &fa,
	    "record size is invalid");

	fa.fa_sizes[0] = 64;
	fa.fa_sizes[1] = 0;
	fa.fa_n = 2;
	selftest_expect_exit("zero-size record", child_group_init, &fa,
	    "record size is invalid");

	fa.fa_sizes[1] = 64;
	fa.fa_n = 0;
	selftest_expect_exit("empty group", child_group_init, &fa,
	    "invalid number of allocators");

	fa.fa_n = 1;
	fa.fa_limit = 0;
	fa.fa_disk = B_FALSE;
	selftest_expect_exit("no backing", child_group_init, &fa,
	    "needs either disk or memory");

	fa.fa_limit = cluster;
	fa.fa_disk = B_TRUE;
	fa.fa_dir = "/nonexistent/zstream-selftest";
	selftest_expect_exit("missing spill directory", child_group_init,
	    &fa, "No such file or directory");

	unsigned n = 1;
	selftest_expect_exit("memory-only overflow",
	    child_memory_only_overflow, &n, "memory limit exceeded");
	n = 2;
	selftest_expect_exit("memory-only eviction",
	    child_memory_only_overflow, &n, "would lose data");
}

const test_case_t selftest_alloc_cases[] = {
	{ "alloc_layout",		alloc_layout },
	{ "alloc_basic",		alloc_basic },
	{ "alloc_record_sizes",		alloc_record_sizes },
	{ "alloc_reassignment",		alloc_reassignment },
	{ "alloc_priority",		alloc_priority },
	{ "alloc_boundaries",		alloc_boundaries },
	{ "alloc_lifecycle",		alloc_lifecycle },
	{ "alloc_rss_bounded",		alloc_rss_bounded },
	{ "alloc_stress",		alloc_stress },
	{ "alloc_resource_limits",	alloc_resource_limits },
	{ "alloc_fatal",		alloc_fatal },
	{ NULL,				NULL },
};
