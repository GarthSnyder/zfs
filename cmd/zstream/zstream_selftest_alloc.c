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
 * - Only the member allocator being written may gain memory, and only up
 *   to the end of the cluster that holds the record.
 *
 * - Memory clawback victims give up memory in strict priority order.
 *
 * - Victims yield only as much memory as was requested, and nothing at all
 *   unless together they can yield all of it.
 *
 * - Reads and skips change nothing.
 *
 * - No allocator's memory/disk split point ever moves in the more-memory
 *   direction.
 *
 * Beyond those rules, predict_store() restates the allocator's policy, and
 * every store must move each member's clusters and split exactly as it
 * predicts.
 *
 * The layout arithmetic is restated here rather than called. Every stored
 * and verified record is also checked in place, in the allocator's memory
 * clusters or in its backing file, where test_record_offset() predicts.
 *
 * Paths that end the process, or that need resource limits the rest of the
 * run must not inherit, run in forked children (selftest_expect_exit() and
 * selftest_expect_success()).
 */

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>

#include "zstream_alloc.h"
#include "zstream_alloc_impl.h"
#include "zstream_selftest.h"
#include "zstream_util.h"

/*
 * A deliberate duplicate of DEFAULT_CLUSTER_SIZE. If the cluster policy
 * changes, this must be changed by hand.
 */
#define	TEST_DEFAULT_CLUSTER_SIZE	(1 << 20)

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
	size_t		   sg_cluster;	/* Cluster size, as predicted */
	uint64_t	   sg_next_tag;
	uint8_t		   *sg_buf;	/* Scratch: retrieved record */
	uint8_t		   *sg_expect;	/* Scratch: expected record */
	uint8_t		   *sg_raw;	/* Scratch: record read in place */
	shadow_member_t	   sg_members[TEST_MAX_MEMBERS];
} shadow_group_t;

typedef struct {
	uint64_t	ss_clusters[TEST_MAX_MEMBERS];
	uint64_t	ss_fdc[TEST_MAX_MEMBERS];
} group_snapshot_t;

#define	MEMBER(sg, m)	((sg)->sg_group->ag_allocators[m])
#define	CLUSTERS(sg, m)	(MEMBER(sg, m)->a_cluster_count)
#define	FDC(sg, m)	(MEMBER(sg, m)->a_first_disk_cluster)

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
page_size(void)
{
	return ((size_t)sysconf(_SC_PAGESIZE));
}

/*
 * The cluster size of a group created with a cluster_size of 0
 */
static size_t
cluster_size(void)
{
	return (MAX(TEST_DEFAULT_CLUSTER_SIZE, page_size()));
}

/*
 * The cluster size allocator_group_init() should choose, restated: 0 means
 * the default, and anything else is raised to hold the largest record and
 * rounded up to whole pages.
 */
static size_t
expected_cluster(size_t request, const size_t *sizes, unsigned n)
{
	if (request == 0)
		return (cluster_size());
	for (unsigned m = 0; m < n; m++)
		request = MAX(request, sizes[m]);
	return (roundup(request, page_size()));
}

/*
 * The memory limit allocator_group_init() should choose, restated: 0 stays
 * 0 (disk-only), and anything else is raised to one cluster per member and
 * rounded up to whole clusters.
 */
static size_t
expected_limit(size_t limit, unsigned n, size_t cluster)
{
	if (limit == 0)
		return (0);
	return (roundup(MAX(limit, n * cluster), cluster));
}

static uint64_t
recs_in_cluster(size_t cluster, size_t record_size)
{
	return (cluster / record_size);
}

/*
 * Records per default cluster
 */
static uint64_t
recs_per_cluster(size_t record_size)
{
	return (recs_in_cluster(cluster_size(), record_size));
}

/*
 * Memory needed to hold records [0, nrecs) entirely on the memory side.
 */
static size_t
bytes_for(size_t cluster, size_t record_size, uint64_t nrecs)
{
	uint64_t rpc = recs_in_cluster(cluster, record_size);
	return (((nrecs + rpc - 1) / rpc) * cluster);
}

/*
 * Byte offset of a record, restated from locate_record() in the allocator.
 * Records are packed into clusters and never straddle a cluster boundary.
 * The offset is the record's place in the backing file; in memory, the
 * record is at the same offset within its cluster.
 */
static uint64_t
test_record_offset(size_t cluster, size_t record_size, uint64_t ix)
{
	uint64_t rpc = recs_in_cluster(cluster, record_size);
	return ((ix / rpc) * cluster + (ix % rpc) * record_size);
}

/*
 * The cluster that holds record ix
 */
static uint64_t
test_record_cluster(size_t cluster, size_t record_size, uint64_t ix)
{
	return (ix / recs_in_cluster(cluster, record_size));
}

/*
 * The group's memory limit in clusters, as the model predicts it
 */
static uint64_t
limit_clusters(const shadow_group_t *sg)
{
	return (expected_limit(sg->sg_limit, sg->sg_n, sg->sg_cluster) /
	    sg->sg_cluster);
}

static void
take_snapshot(shadow_group_t *sg, group_snapshot_t *ss)
{
	for (unsigned m = 0; m < sg->sg_n; m++) {
		ss->ss_clusters[m] = CLUSTERS(sg, m);
		ss->ss_fdc[m] = FDC(sg, m);
	}
}

/*
 * The group invariants that are cheap enough to check after every
 * operation: exact accounting, splits within bounds, backing consistent
 * with the group's configuration, and allocator_get_stats() consistent with
 * all of it.
 */
static void
audit_accounting(allocator_group_t *g, allocator_t *member)
{
	size_t cluster = g->ag_cluster_size;
	uint64_t total = 0;
	boolean_t found = B_FALSE;

	/* Clusters are whole pages; a memory limit holds one per member */
	VERIFY3U(cluster, >=, page_size());
	VERIFY0(cluster % page_size());
	VERIFY3U(g->ag_num_allocators, >, 0);
	if (g->ag_clusters_max > 0)
		VERIFY3U(g->ag_clusters_max, >=, g->ag_num_allocators);
	for (unsigned m = 0; m < g->ag_num_allocators; m++) {
		allocator_t *a = g->ag_allocators[m];
		allocator_stats_t st = allocator_get_stats(a);

		if (a == member)
			found = B_TRUE;
		VERIFY3P(a->a_group, ==, g);
		VERIFY3U(a->a_record_size, <=, cluster);
		VERIFY3U(a->a_records_per_cluster, ==,
		    recs_in_cluster(cluster, a->a_record_size));
		VERIFY3S(g->ag_dir != NULL, ==, a->a_fd >= 0);
		/* There is a cluster table exactly when there is a limit */
		VERIFY3S(g->ag_clusters_max > 0, ==, a->a_clusters != NULL);
		VERIFY3U(a->a_cluster_count, <=, a->a_first_disk_cluster);
		VERIFY3U(a->a_first_disk_cluster, <=, g->ag_clusters_max);
		VERIFY3U(st.as_memory_used, ==, a->a_cluster_count * cluster);
		VERIFY3U(st.as_num_records, ==, a->a_record_count);
		VERIFY3U(st.as_record_size, ==, a->a_record_size);
		total += a->a_cluster_count;
	}
	VERIFY(found);
	VERIFY3U(g->ag_clusters_current, ==, total);
	VERIFY3U(g->ag_clusters_current, <=, g->ag_clusters_max);
}

static int
compare_addresses(const void *x, const void *y)
{
	uintptr_t a = (uintptr_t)*(uint8_t *const *)x;
	uintptr_t b = (uintptr_t)*(uint8_t *const *)y;
	return ((a > b) - (a < b));
}

/*
 * Every cluster that a member holds is a mapped, page-aligned block, and no
 * two clusters in the group overlap, whether they belong to the same member
 * or to different ones. This sorts every cluster in the group, so it runs
 * at sweeps rather than after every operation.
 */
static void
audit_cluster_table(allocator_group_t *g)
{
	uint64_t n = g->ag_clusters_current;
	uint64_t k = 0;

	if (n == 0)
		return;
	uint8_t **all = safe_malloc(n * sizeof (uint8_t *));
	for (unsigned m = 0; m < g->ag_num_allocators; m++) {
		allocator_t *a = g->ag_allocators[m];
		for (uint64_t c = 0; c < a->a_cluster_count; c++) {
			uint8_t *p = a->a_clusters[c];
			VERIFY3P(p, !=, NULL);
			VERIFY3P(p, !=, MAP_FAILED);
			VERIFY0((uintptr_t)p % page_size());
			VERIFY3U(k, <, n);
			all[k++] = p;
		}
	}
	VERIFY3U(k, ==, n);
	qsort(all, n, sizeof (uint8_t *), compare_addresses);
	for (uint64_t i = 1; i < n; i++) {
		VERIFY3U((uintptr_t)all[i] - (uintptr_t)all[i - 1], >=,
		    g->ag_cluster_size);
	}
	free(all);
}

/*
 * Invariants that hold for any group across any two operations. This sees
 * only the group's own state, so the hash suite uses it too; group_audit()
 * adds the shadow model's view.
 */
void
selftest_audit_group(allocator_t *member)
{
	audit_accounting(member->a_group, member);
	audit_cluster_table(member->a_group);
}

/*
 * audit_accounting(), plus agreement with the shadow model
 */
static void
group_audit(shadow_group_t *sg)
{
	allocator_group_t *g = sg->sg_group;

	audit_accounting(g, MEMBER(sg, 0));
	VERIFY3U(g->ag_num_allocators, ==, sg->sg_n);
	VERIFY3U(g->ag_cluster_size, ==, sg->sg_cluster);
	VERIFY3U(g->ag_clusters_max, ==, limit_clusters(sg));
	VERIFY3S(sg->sg_disk, ==, g->ag_dir != NULL);
	if (sg->sg_disk) {
		/* The group keeps its own copy of the directory name */
		VERIFY3P(g->ag_dir, !=, selftest_scratch_dir());
		VERIFY0(strcmp(g->ag_dir, selftest_scratch_dir()));
	}
	for (unsigned m = 0; m < sg->sg_n; m++) {
		VERIFY3U(MEMBER(sg, m)->a_record_size, ==,
		    sg->sg_members[m].sh_record_size);
		VERIFY3U(MEMBER(sg, m)->a_record_count, ==,
		    sg->sg_members[m].sh_count);
	}
}

/*
 * Check how one operation changed the group. The writer is the member that
 * was stored to, or -1 for an operation that must change nothing (a read or
 * a skip).
 */
static void
check_transition(shadow_group_t *sg, const group_snapshot_t *before,
    int writer)
{
	allocator_group_t *g = sg->sg_group;
	int highest_victim = -1;

	group_audit(sg);
	for (int m = 0; m < (int)sg->sg_n; m++) {
		uint64_t clusters = CLUSTERS(sg, m);
		uint64_t fdc = FDC(sg, m);

		/* The memory/disk split never moves up */
		VERIFY3U(fdc, <=, before->ss_fdc[m]);
		if (m == writer) {
			VERIFY3U(clusters, >=, before->ss_clusters[m]);
			continue;
		}
		/* Only the writer can gain memory... */
		VERIFY3U(clusters, <=, before->ss_clusters[m]);
		if (clusters == before->ss_clusters[m]) {
			/* ...and only victims move their split */
			VERIFY3U(fdc, ==, before->ss_fdc[m]);
			continue;
		}
		/* Victims have lower priority and lose memory for good */
		VERIFY3S(m, <, writer);
		VERIFY3U(fdc, ==, clusters);
		highest_victim = m;
	}

	/* Victims are drained in priority order */
	for (int m = 0; m < highest_victim; m++)
		VERIFY0(CLUSTERS(sg, m));

	if (writer < 0)
		return;
	uint64_t clusters = CLUSTERS(sg, writer);
	uint64_t fdc = FDC(sg, writer);
	if (fdc < before->ss_fdc[writer]) {
		/* Pinned: it gained nothing, and nobody gave anything up */
		VERIFY3U(clusters, ==, before->ss_clusters[writer]);
		VERIFY3U(fdc, ==, clusters);
		VERIFY3S(highest_victim, ==, -1);
	} else if (highest_victim >= 0) {
		/* An eviction takes exactly the shortfall */
		VERIFY3U(g->ag_clusters_current, ==, g->ag_clusters_max);
	}
}

/*
 * Predict, from the state before a store, exactly how the store moves
 * every member's clusters and split. This restates the allocator's policy:
 *
 * - A store into a member's clusters or above its split changes nothing.
 *
 * - A store between the two maps every cluster from the member's last one
 *   up to the record's, using free clusters first. Lower-priority members
 *   give up the rest, lowest first, with each victim's split dropping to
 *   its remaining clusters, but only if together they hold enough.
 *
 * - Otherwise nobody gives anything up, and the writer's split drops to
 *   its clusters, so the record goes to disk.
 *
 * Members of a memory-only group can never give anything up.
 */
static void
predict_store(const shadow_group_t *sg, const group_snapshot_t *before,
    unsigned writer, uint64_t ix, group_snapshot_t *after)
{
	uint64_t c = test_record_cluster(sg->sg_cluster,
	    sg->sg_members[writer].sh_record_size, ix);
	uint64_t used = 0;
	uint64_t poachable = 0;

	*after = *before;
	if (c < before->ss_clusters[writer] || c >= before->ss_fdc[writer])
		return;
	for (unsigned m = 0; m < sg->sg_n; m++) {
		used += before->ss_clusters[m];
		if (m < writer && sg->sg_disk)
			poachable += before->ss_clusters[m];
	}
	uint64_t needed = c + 1 - before->ss_clusters[writer];
	uint64_t avail = limit_clusters(sg) - used;
	if (needed > avail) {
		uint64_t deficit = needed - avail;
		if (poachable < deficit) {
			after->ss_fdc[writer] = before->ss_clusters[writer];
			return;
		}
		for (unsigned m = 0; deficit > 0; m++) {
			VERIFY3U(m, <, writer);
			uint64_t take = MIN(deficit, after->ss_clusters[m]);
			if (take == 0)
				continue;
			after->ss_clusters[m] -= take;
			after->ss_fdc[m] = after->ss_clusters[m];
			deficit -= take;
		}
	}
	after->ss_clusters[writer] = c + 1;
}

/*
 * Check that a record's bytes are where the restated layout says they
 * should be: in one of the member's clusters below its split, in its file
 * above it.
 */
static void
check_placement(shadow_group_t *sg, unsigned m, uint64_t ix,
    const uint8_t *expect)
{
	allocator_t *a = MEMBER(sg, m);
	size_t rsize = a->a_record_size;
	uint64_t off = test_record_offset(sg->sg_cluster, rsize, ix);
	uint64_t c = test_record_cluster(sg->sg_cluster, rsize, ix);

	if (c < a->a_first_disk_cluster) {
		VERIFY3U(c, <, a->a_cluster_count);
		VERIFY0(memcmp(a->a_clusters[c] + off % sg->sg_cluster,
		    expect, rsize));
	} else {
		VERIFY3S(a->a_fd, >=, 0);
		VERIFY3S(pread(a->a_fd, sg->sg_raw, rsize, off), ==, rsize);
		VERIFY0(memcmp(sg->sg_raw, expect, rsize));
	}
}

static shadow_group_t *
shadow_group_init(const size_t *sizes, unsigned n, size_t cluster,
    size_t limit, boolean_t disk, const uint64_t *capacities)
{
	shadow_group_t *sg = safe_calloc(sizeof (shadow_group_t));
	allocator_t **allocs = allocator_group_init(sizes, n, cluster, limit,
	    disk ? selftest_scratch_dir() : NULL);

	sg->sg_group = allocs[0]->a_group;
	VERIFY3P(allocs, ==, &sg->sg_group->ag_allocators[0]);
	sg->sg_n = n;
	sg->sg_disk = disk;
	sg->sg_limit = limit;
	sg->sg_cluster = expected_cluster(cluster, sizes, n);
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
		VERIFY0(CLUSTERS(sg, m));
		VERIFY3U(FDC(sg, m), ==, sg->sg_group->ag_clusters_max);
	}
	return (sg);
}

static shadow_group_t *
shadow_init1(size_t record_size, size_t cluster, size_t limit,
    boolean_t disk, uint64_t capacity)
{
	return (shadow_group_init(&record_size, 1, cluster, limit, disk,
	    &capacity));
}

/*
 * Destroy the group by way of member "via", which needn't be member 0.
 */
static void
shadow_group_fini(shadow_group_t *sg, unsigned via)
{
	group_audit(sg);
	audit_cluster_table(sg->sg_group);
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
	group_snapshot_t before, predicted, after;

	VERIFY3U(ix, <, sh->sh_capacity);
	take_snapshot(sg, &before);
	predict_store(sg, &before, m, ix, &predicted);
	uint64_t tag = sg->sg_next_tag++;
	selftest_fill(sg->sg_expect, sh->sh_record_size, tag);
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

	take_snapshot(sg, &after);
	for (unsigned i = 0; i < sg->sg_n; i++) {
		if (after.ss_clusters[i] != predicted.ss_clusters[i] ||
		    after.ss_fdc[i] != predicted.ss_fdc[i]) {
			errx(1, "store to member %u record %llu left member %u "
			    "with %llu clusters and split %llu, not %llu and "
			    "%llu", m, (u_longlong_t)ix, i,
			    (u_longlong_t)after.ss_clusters[i],
			    (u_longlong_t)after.ss_fdc[i],
			    (u_longlong_t)predicted.ss_clusters[i],
			    (u_longlong_t)predicted.ss_fdc[i]);
		}
	}
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
	uint64_t rpc = recs_in_cluster(sg->sg_cluster,
	    sg->sg_members[m].sh_record_size);
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
	/* A skip writes nothing, so nothing but the count may change */
	check_transition(sg, &before, -1);
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
		selftest_fill(sg->sg_expect, rsize, sh->sh_tags[ix]);
		if (memcmp(sg->sg_buf, sg->sg_expect, rsize) != 0) {
			errx(1, "member %u record %llu corrupted", m,
			    (u_longlong_t)ix);
		}
		/*
		 * Disk placement is checked at store time. Evicted records
		 * keep their offsets, so skip the extra syscall here.
		 */
		if (test_record_cluster(sg->sg_cluster, rsize, ix) <
		    FDC(sg, m))
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
	audit_cluster_table(sg->sg_group);
}

/*
 * Cluster selection and record layout for single-member groups (disk-only,
 * memory-only, dual-backed) at record sizes with awkward relationships
 * to the cluster size, with default clusters and with clusters of one and
 * three pages. A record bigger than the requested cluster gets a bigger
 * cluster.
 *
 * - The last record of each cluster and the first record of the next
 *   must land exactly where test_record_offset() says.
 *
 * - Clusters must be added one at a time, and the gaps must read as
 *   zeros.
 *
 * - Also check the floor and rounding of the memory limit with a group of
 *   mixed record sizes.
 */
static void
layout1(size_t request, size_t rsize)
{
	size_t cluster = expected_cluster(request, &rsize, 1);
	uint64_t rpc = recs_in_cluster(cluster, rsize);
	/* The last is in the fourth cluster, past its start if it can be */
	uint64_t ixs[] = { rpc - 1, 0, rpc, 2 * rpc - 1, 2 * rpc,
	    3 * rpc + MIN(1, rpc - 1) };

	/* Under 1% of each default cluster is wasted */
	if (request == 0)
		VERIFY3U(100 * (cluster % rsize), <, cluster);

	for (int config = 0; config < 3; config++) {
		/* Config 0 is dual, 1 is disk-only, 2 is memory-only */
		uint64_t nclusters = (config == 0) ? 2 : (config == 1) ? 0 : 4;
		boolean_t disk = (config != 2);
		shadow_group_t *sg = shadow_init1(rsize, request,
		    nclusters * cluster, disk, 3 * rpc + 3);

		/* The clusters cover the highest one written */
		uint64_t want = 0;
		for (int i = 0; i < ARRAY_SIZE(ixs); i++) {
			shadow_store(sg, 0, ixs[i]);
			want = MIN(nclusters, MAX(want, ixs[i] / rpc + 1));
			VERIFY3U(CLUSTERS(sg, 0), ==, want);
		}
		for (int i = 0; i < ARRAY_SIZE(ixs); i++)
			shadow_verify_around(sg, 0, ixs[i]);

		VERIFY3U(CLUSTERS(sg, 0), ==, nclusters);
		VERIFY3U(FDC(sg, 0), ==, nclusters);
		shadow_group_fini(sg, 0);
	}
}

static void
alloc_layout(void)
{
	static const size_t sizes[] = { 1, 3, 7, 8, 24, 40, 72, 104, 1000,
	    4095, 4096, 4097, 5000, 8191, MAX_RECORD_SIZE };
	const size_t requests[] = { 0, 1, 3 * page_size() };

	for (int r = 0; r < ARRAY_SIZE(requests); r++) {
		for (size_t s = 0; s < ARRAY_SIZE(sizes); s++)
			layout1(requests[r], sizes[s]);
	}

	/*
	 * The limit is raised to a cluster per member and rounded up to
	 * whole clusters, whatever the sizes
	 */
	static const size_t group_sizes[] = { 24, 104, 104 };
	static const uint64_t caps[] = { 16, 16, 16 };
	for (int r = 0; r < ARRAY_SIZE(requests); r++) {
		size_t cluster = expected_cluster(requests[r], group_sizes, 3);
		const size_t limits[] = { 0, 1, cluster - 1, cluster,
		    cluster + 1, 3 * cluster - 1, 3 * cluster, 3 * cluster + 1,
		    5 * cluster + 7 };
		for (int i = 0; i < ARRAY_SIZE(limits); i++) {
			shadow_group_t *sg = shadow_group_init(group_sizes, 3,
			    requests[r], limits[i], B_TRUE, caps);
			for (unsigned m = 0; m < 3; m++)
				shadow_append(sg, m);
			shadow_verify_all(sg);
			shadow_group_fini(sg, 2);
		}
	}
}

/*
 * Cluster sizes and memory limits, against values worked out by hand
 * rather than by expected_cluster() and expected_limit():
 *
 * - A request of 0 gets the default, 1MB or one page if pages are bigger.
 *   Anything else is raised to hold the largest record, wherever it is in
 *   the group, and rounded up to whole pages.
 *
 * - A nonzero limit is raised to one cluster per member and rounded up to
 *   whole clusters. A zero limit stays zero (disk-only).
 *
 * Then memory moves between the members of a dual-backed group with
 * three-page clusters, which no record size divides, so that every
 * rounding in the allocator works in units that aren't a power of two.
 */
static void
alloc_cluster_size(void)
{
	const size_t page = page_size();
	const size_t def = MAX(1 << 20, page);
	static const uint64_t caps[] = { 16, 16, 16 };
	const struct {
		size_t	request;
		size_t	rsize;		/* Member 1; the others are small */
		size_t	cluster;
	} cases[] = {
		{ 0, 24, def },
		{ 0, MAX_RECORD_SIZE, def },
		{ 1, 24, page },
		{ page - 1, 24, page },
		{ page, 24, page },
		{ page + 1, 24, 2 * page },
		{ 3 * page - 1, 24, 3 * page },
		{ 3 * page, 24, 3 * page },
		{ (1 << 20) + 1, 24, roundup((1 << 20) + 1, page) },
		{ 1, page / 2 + 1, page },
		{ 1, MAX_RECORD_SIZE, MAX(page, MAX_RECORD_SIZE) },
		{ page + 1, MAX_RECORD_SIZE, MAX(2 * page, MAX_RECORD_SIZE) },
		{ 1, MAX_RECORD_SIZE - 1, MAX(page, MAX_RECORD_SIZE) },
	};

	for (int i = 0; i < ARRAY_SIZE(cases); i++) {
		size_t sizes[] = { 24, cases[i].rsize, 104 };
		size_t cluster = cases[i].cluster;
		shadow_group_t *sg = shadow_group_init(sizes, 3,
		    cases[i].request, 1, B_FALSE, caps);
		allocator_group_t *g = sg->sg_group;

		VERIFY3U(g->ag_cluster_size, ==, cluster);
		VERIFY3U(g->ag_clusters_max, ==, 3);
		for (unsigned m = 0; m < 3; m++) {
			VERIFY3U(MEMBER(sg, m)->a_records_per_cluster, ==,
			    cluster / sizes[m]);
			shadow_append(sg, m);
		}
		shadow_verify_all(sg);
		shadow_group_fini(sg, 1);
	}

	/*
	 * Limits for a pair of members with three-page clusters, in bytes
	 * requested and clusters granted
	 */
	const size_t c = 3 * page;
	const size_t limits[][2] = {
		{ 0, 0 },
		{ 1, 2 },
		{ 2 * c - 1, 2 },
		{ 2 * c, 2 },
		{ 2 * c + 1, 3 },
		{ 5 * c + 7, 6 },
	};
	for (int i = 0; i < ARRAY_SIZE(limits); i++) {
		static const size_t sizes[] = { 1000, 1000 };
		shadow_group_t *sg = shadow_group_init(sizes, 2, c,
		    limits[i][0], B_TRUE, caps);
		VERIFY3U(sg->sg_group->ag_cluster_size, ==, c);
		VERIFY3U(sg->sg_group->ag_clusters_max, ==, limits[i][1]);
		shadow_append(sg, 0);
		shadow_append(sg, 1);
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/*
	 * Member 0 fills three of the four clusters. A sparse store to
	 * member 1 then needs three clusters at once, and takes the two
	 * it lacks from member 0. Member 1 then grows past the limit,
	 * draining member 0 and pinning itself.
	 */
	static const size_t sizes[] = { 1000, 4096 };
	const uint64_t rpc0 = recs_in_cluster(c, sizes[0]);
	const uint64_t rpc1 = recs_in_cluster(c, sizes[1]);
	const uint64_t pair_caps[] = { 3 * rpc0, 6 * rpc1 };
	shadow_group_t *sg = shadow_group_init(sizes, 2, c, 4 * c, B_TRUE,
	    pair_caps);
	shadow_append_clusters(sg, 0, 3);
	VERIFY3U(CLUSTERS(sg, 0), ==, 3);
	shadow_store(sg, 1, 2 * rpc1 + 1);
	VERIFY3U(CLUSTERS(sg, 0), ==, 1);
	VERIFY3U(FDC(sg, 0), ==, 1);
	VERIFY3U(CLUSTERS(sg, 1), ==, 3);
	while (sg->sg_members[1].sh_count < pair_caps[1])
		shadow_append(sg, 1);
	VERIFY0(CLUSTERS(sg, 0));
	VERIFY3U(CLUSTERS(sg, 1), ==, 4);
	VERIFY3U(FDC(sg, 1), ==, 4);
	shadow_verify_all(sg);
	shadow_group_fini(sg, 1);
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
	const size_t full = bytes_for(cluster_size(), rsize, cap);

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

		shadow_group_t *sg = shadow_init1(rsize, 0, mem, disk, cap);

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
			VERIFY0(CLUSTERS(sg, 0));
		else
			VERIFY3U(CLUSTERS(sg, 0), >, 0);

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
		shadow_group_t *sg = shadow_group_init(group_sizes, 2, 0,
		    3 * cluster, B_TRUE, caps);

		/* The overflow spills to disk: member 0 can't take memory */
		while (sg->sg_members[0].sh_count < caps[0])
			shadow_append(sg, 0);
		VERIFY3U(CLUSTERS(sg, 0), ==, 3);
		VERIFY3U(FDC(sg, 0), ==, 3);
		shadow_verify_all(sg);

		for (uint64_t k = 1; k <= 3; k++) {
			while (CLUSTERS(sg, 1) < k)
				shadow_append(sg, 1);
			VERIFY3U(CLUSTERS(sg, 0), ==, 3 - k);
			VERIFY3U(FDC(sg, 0), ==, 3 - k);
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
 * - Third, a request that the lower-priority members can just cover, which
 *   drains them all, and one that is a cluster more than they can cover.
 *   That one takes nothing from anyone: the requester's split is pinned,
 *   and the lower-priority members keep their memory until a
 *   higher-priority member takes it in the usual order.
 */
static void
alloc_reassignment(void)
{
	const size_t cluster = cluster_size();

	{
		size_t sizes[] = { 1000, 1000 };
		uint64_t rpc = recs_per_cluster(1000);
		uint64_t caps[] = { 8 * rpc, 8 * rpc };
		shadow_group_t *sg = shadow_group_init(sizes, 2, 0, 8 * cluster,
		    B_TRUE, caps);
		shadow_append_clusters(sg, 0, 8);
		shadow_store(sg, 1, 5 * rpc);
		VERIFY3U(CLUSTERS(sg, 1), ==, 6);
		VERIFY3U(CLUSTERS(sg, 0), ==, 2);
		VERIFY3U(FDC(sg, 0), ==, 2);
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
		shadow_group_t *sg = shadow_group_init(sizes, 2, 0, 8 * cluster,
		    B_TRUE, caps);
		while (sg->sg_members[0].sh_count < caps[0])
			shadow_append(sg, 0);

		for (int iter = 0; CLUSTERS(sg, 1) < 8; iter++) {
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
		VERIFY0(CLUSTERS(sg, 0));
		shadow_verify_all(sg);
		shadow_group_fini(sg, 1);
	}

	for (uint64_t extra = 0; extra < 2; extra++) {
		size_t sizes[] = { 4096, 4096, 4096, 4096 };
		uint64_t rpc = recs_per_cluster(4096);
		uint64_t caps[] = { rpc, 2 * rpc, 7 * rpc + 1, 5 * rpc };
		shadow_group_t *sg = shadow_group_init(sizes, 4, 0, 8 * cluster,
		    B_TRUE, caps);
		shadow_append_clusters(sg, 0, 1);
		shadow_append_clusters(sg, 1, 2);
		shadow_append_clusters(sg, 2, 4);
		shadow_append_clusters(sg, 3, 1);

		/* Needs 3 + extra clusters; members 0 and 1 hold 3 */
		shadow_store(sg, 2, (6 + extra) * rpc);
		VERIFY3U(sg->sg_group->ag_clusters_current, ==, 8);
		if (extra == 0) {
			VERIFY0(CLUSTERS(sg, 0));
			VERIFY0(CLUSTERS(sg, 1));
			VERIFY3U(CLUSTERS(sg, 2), ==, 7);
			VERIFY3U(FDC(sg, 2), ==, 8);

			/* Member 3 can now grow only at member 2's expense */
			shadow_append_clusters(sg, 3, 1);
			VERIFY3U(CLUSTERS(sg, 2), ==, 6);
			shadow_verify_all(sg);
			shadow_group_fini(sg, 3);
			continue;
		}
		VERIFY3U(CLUSTERS(sg, 0), ==, 1);
		VERIFY3U(CLUSTERS(sg, 1), ==, 2);
		VERIFY3U(FDC(sg, 0), ==, 8);
		VERIFY3U(FDC(sg, 1), ==, 8);
		VERIFY3U(CLUSTERS(sg, 2), ==, 4);
		VERIFY3U(FDC(sg, 2), ==, 4);

		/* Member 3 drains members 0 and 1, then turns to member 2 */
		shadow_append_clusters(sg, 3, 1);
		VERIFY0(CLUSTERS(sg, 0));
		VERIFY3U(CLUSTERS(sg, 1), ==, 2);
		shadow_append_clusters(sg, 3, 2);
		VERIFY0(CLUSTERS(sg, 1));
		VERIFY3U(CLUSTERS(sg, 2), ==, 4);
		shadow_append_clusters(sg, 3, 1);
		VERIFY3U(CLUSTERS(sg, 2), ==, 3);
		VERIFY3U(CLUSTERS(sg, 3), ==, 5);
		shadow_verify_all(sg);
		shadow_group_fini(sg, 3);
	}
}

/*
 * Evictions big enough to need several pwritev() calls. With page-sized
 * clusters, member 2's sparse stores take runs of 1, IOVEC_BUFFSIZE - 1,
 * IOVEC_BUFFSIZE, IOVEC_BUFFSIZE + 1 and 2 * IOVEC_BUFFSIZE + 1 clusters
 * from member 1, after a first store that takes all IOVEC_BUFFSIZE + 44 of
 * member 0's clusters and then one of member 1's. Every evicted record must
 * be at its own offset in its member's file.
 */
static void
alloc_eviction_batches(void)
{
	const uint64_t runs[] = { 1, IOVEC_BUFFSIZE - 1, IOVEC_BUFFSIZE,
	    IOVEC_BUFFSIZE + 1, 2 * IOVEC_BUFFSIZE + 1 };
	const uint64_t held0 = IOVEC_BUFFSIZE + 44;
	uint64_t held1 = 0;

	for (int i = 0; i < ARRAY_SIZE(runs); i++)
		held1 += runs[i];
	size_t sizes[] = { 1000, 1000, 4096 };
	size_t cluster = expected_cluster(1, sizes, 3);
	uint64_t rpc = recs_in_cluster(cluster, 1000);
	uint64_t rpc2 = recs_in_cluster(cluster, 4096);
	uint64_t caps[] = { held0 * rpc, held1 * rpc, (held0 + held1) * rpc2 };
	shadow_group_t *sg = shadow_group_init(sizes, 3, 1,
	    (held0 + held1) * cluster, B_TRUE, caps);

	shadow_append_clusters(sg, 0, held0);
	shadow_append_clusters(sg, 1, held1);
	shadow_verify_all(sg);
	uint64_t want = held0;
	for (int i = 0; i < ARRAY_SIZE(runs); i++) {
		want += runs[i];
		shadow_store(sg, 2, (want - 1) * rpc2);
		VERIFY0(CLUSTERS(sg, 0));
		VERIFY3U(CLUSTERS(sg, 1), ==, held0 + held1 - want);
		VERIFY3U(CLUSTERS(sg, 2), ==, want);
		shadow_verify_all(sg);
	}
	VERIFY0(CLUSTERS(sg, 1));
	shadow_group_fini(sg, 2);
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
	shadow_group_t *sg = shadow_group_init(sizes, 4, 0, 4 * cluster,
	    B_TRUE, caps);

	/* Fill the group, one cluster per member */
	for (unsigned m = 0; m < 4; m++)
		shadow_append_clusters(sg, m, 1);
	for (unsigned m = 0; m < 4; m++)
		VERIFY3U(CLUSTERS(sg, m), ==, 1);

	/* Member 0 has no one to take from, so it spills to disk */
	shadow_append(sg, 0);
	VERIFY3U(CLUSTERS(sg, 0), ==, 1);
	VERIFY3U(FDC(sg, 0), ==, 1);

	/* Member 1 takes from member 0 */
	shadow_append_clusters(sg, 1, 1);
	VERIFY0(CLUSTERS(sg, 0));
	VERIFY0(FDC(sg, 0));
	VERIFY3U(CLUSTERS(sg, 1), ==, 2);
	VERIFY3U(CLUSTERS(sg, 2), ==, 1);

	/* Member 3 drains member 1 before touching member 2 */
	shadow_append_clusters(sg, 3, 1);
	VERIFY3U(CLUSTERS(sg, 1), ==, 1);
	VERIFY3U(CLUSTERS(sg, 2), ==, 1);
	shadow_append_clusters(sg, 3, 1);
	VERIFY0(CLUSTERS(sg, 1));
	VERIFY3U(CLUSTERS(sg, 2), ==, 1);
	shadow_append_clusters(sg, 3, 1);
	VERIFY0(CLUSTERS(sg, 2));
	VERIFY3U(CLUSTERS(sg, 3), ==, 4);

	/* Drained members stay on disk */
	shadow_append_clusters(sg, 2, 1);
	VERIFY0(CLUSTERS(sg, 2));

	/* The last member, holding everything, spills past the limit */
	shadow_append_clusters(sg, 3, 1);
	VERIFY3U(CLUSTERS(sg, 3), ==, 4);
	VERIFY3U(FDC(sg, 3), ==, 4);

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
		shadow_group_t *sg = shadow_group_init(sizes, 2, 0, 2 * cluster,
		    B_TRUE, caps);
		shadow_append_clusters(sg, 1, 2);
		shadow_append(sg, 0);
		VERIFY0(CLUSTERS(sg, 0));
		VERIFY0(FDC(sg, 0));
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/* A single record chased onto disk */
	{
		size_t sizes[] = { rsize, 4096 };
		uint64_t caps[] = { 64, 2 * recs_per_cluster(4096) };
		shadow_group_t *sg = shadow_group_init(sizes, 2, 0,
		    2 * cluster, B_TRUE, caps);
		shadow_append(sg, 1);
		shadow_append(sg, 0);
		VERIFY3U(CLUSTERS(sg, 0), ==, 1);
		shadow_append_clusters(sg, 1, 1);
		VERIFY0(CLUSTERS(sg, 0));
		VERIFY0(FDC(sg, 0));
		shadow_verify_all(sg);
		shadow_group_fini(sg, 1);
	}

	/* Data that exactly fills the limit, then the first record past it */
	{
		shadow_group_t *sg = shadow_init1(rsize, 0, cluster, B_TRUE,
		    2 * rpc);
		shadow_append_clusters(sg, 0, 1);
		VERIFY3U(CLUSTERS(sg, 0), ==, 1);
		shadow_append(sg, 0);
		VERIFY3U(CLUSTERS(sg, 0), ==, 1);
		VERIFY3U(FDC(sg, 0), ==, 1);
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/*
	 * A sparse store on the memory side maps every cluster up to it.
	 * Records in the gap and above the last cluster read as zeros, and
	 * reading them maps nothing.
	 */
	for (int disk = 0; disk < 2; disk++) {
		shadow_group_t *sg = shadow_init1(rsize, 0, 8 * cluster, disk,
		    8 * rpc);
		shadow_store(sg, 0, 5 * rpc);
		VERIFY3U(CLUSTERS(sg, 0), ==, 6);
		VERIFY3U(sg->sg_group->ag_clusters_current, ==, 6);
		shadow_verify_around(sg, 0, 0);
		shadow_verify_around(sg, 0, 3 * rpc);
		shadow_verify_around(sg, 0, 7 * rpc);
		shadow_verify(sg, 0, 8 * rpc - 1);
		VERIFY3U(CLUSTERS(sg, 0), ==, 6);
		shadow_group_fini(sg, 0);
	}

	/* Disk-only from birth */
	{
		shadow_group_t *sg = shadow_init1(rsize, 0, 0, B_TRUE, 512);
		selftest_rng_t rng;
		selftest_rng_init(&rng, 43);
		for (int i = 0; i < 600; i++)
			shadow_store(sg, 0, selftest_rng_below(&rng, 512));
		VERIFY0(CLUSTERS(sg, 0));
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/* A limit far larger than anything ever written */
	{
		shadow_group_t *sg = shadow_init1(rsize, 0, 64 * cluster,
		    B_TRUE, 64);
		shadow_append(sg, 0);
		VERIFY3U(CLUSTERS(sg, 0), ==, 1);
		VERIFY3U(FDC(sg, 0), ==, 64);
		shadow_verify_all(sg);
		shadow_group_fini(sg, 0);
	}

	/*
	 * Records never written read as zeros and change nothing, in every
	 * configuration: in an empty group, above the last cluster, past a
	 * memory-only allocator's split (where there is no file to read),
	 * and far beyond anything ever written.
	 */
	for (int config = 0; config < 3; config++) {
		/* Config 0 is dual, 1 is disk-only, 2 is memory-only */
		shadow_group_t *sg = shadow_init1(rsize, 0,
		    (config == 1) ? 0 : cluster, config != 2, 3 * rpc);
		shadow_verify_all(sg);
		verify_unwritten(sg, 0, 1ULL << 30);
		shadow_append_clusters(sg, 0, 1);
		shadow_verify_around(sg, 0, rpc);
		shadow_verify_around(sg, 0, 3 * rpc - 2);
		verify_unwritten(sg, 0, 1ULL << 30);
		shadow_verify_all(sg);
		VERIFY3U(CLUSTERS(sg, 0), ==, (config == 1) ? 0 : 1);
		VERIFY3U(FDC(sg, 0), ==, sg->sg_group->ag_clusters_max);
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
 *
 * Backing files must not leak either. They are unlinked as soon as they are
 * created, so a group's private spill directory stays empty while the group
 * is live, even after records have spilled into it.
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
		shadow_group_t *sg = shadow_group_init(sizes, n, 0,
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
		sgs[i] = shadow_group_init(sizes, 2, 0, limit, i % 3 != 2,
		    caps);
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

	char dir[PATH_MAX];
	(void) snprintf(dir, sizeof (dir), "%s/zstream-selftest-XXXXXX",
	    selftest_scratch_dir());
	VERIFY3P(mkdtemp(dir), !=, NULL);
	size_t sizes[] = { 64, 4096, 104 };
	uint8_t buf[4096] = { 0 };
	allocator_t **a = allocator_group_init(sizes, 3, 0, 3 * cluster, dir);
	for (uint64_t i = 0; i < 4 * recs_per_cluster(4096); i++)
		(void) allocator_append(a[1], buf);
	VERIFY3U(a[1]->a_first_disk_cluster, ==, 3);
	DIR *dp = opendir(dir);
	VERIFY3P(dp, !=, NULL);
	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (strcmp(de->d_name, ".") != 0 &&
		    strcmp(de->d_name, "..") != 0)
			errx(1, "spill file %s/%s was not unlinked", dir,
			    de->d_name);
	}
	(void) closedir(dp);
	allocator_group_fini(a[2]);
	VERIFY0(rmdir(dir));

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

	shadow_group_t *sg = shadow_group_init(sizes, 2, 0, limit, B_TRUE,
	    caps);
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
	VERIFY0(CLUSTERS(sg, 0));
	VERIFY3U(CLUSTERS(sg, 1), ==, 8);
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
		}
		/* Half the groups get small clusters of any requested size */
		size_t request = (selftest_rng_below(&rng, 2) == 0) ? 0 :
		    1 + selftest_rng_below(&rng, 4 * page_size());
		size_t cluster = expected_cluster(request, sizes, n);
		for (unsigned m = 0; m < n; m++) {
			caps[m] = MAX(16, MIN(4096, (2 * cluster) / sizes[m]));
			full += bytes_for(cluster, sizes[m], caps[m]);
		}
		/*
		 * A memory-only group can never spill, so it has to be big
		 * enough for every index of every member. Disk-backed
		 * groups get an arbitrary limit, usually not a whole
		 * number of clusters.
		 */
		size_t limit = disk ?
		    selftest_rng_below(&rng, full + cluster) : full;

		shadow_group_t *sg = shadow_group_init(sizes, n, request, limit,
		    disk, caps);
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
	boolean_t	fa_cap_as;	/* Cap the address space at 1TB */
	const char	*fa_dir;	/* Overrides the scratch directory */
} fatal_args_t;

static void
child_group_init(void *arg)
{
	fatal_args_t *fa = arg;
	const char *dir = (fa->fa_dir != NULL) ? fa->fa_dir :
	    selftest_scratch_dir();

	if (fa->fa_cap_as) {
		struct rlimit as;
		VERIFY0(getrlimit(RLIMIT_AS, &as));
		as.rlim_cur = MIN(as.rlim_max, (rlim_t)1 << 40);
		VERIFY0(setrlimit(RLIMIT_AS, &as));
	}
	(void) allocator_group_init(fa->fa_sizes, fa->fa_n, 0, fa->fa_limit,
	    fa->fa_disk ? dir : NULL);
}

/*
 * Memory-only groups with one cluster per member, each filled by its
 * member. Then the last member needs another record. With one member, it
 * writes past the limit. With two, member 1 needs memory that member 0 can
 * only give up by losing its data.
 */
static void
child_memory_only_overflow(void *arg)
{
	unsigned n = *(unsigned *)arg;
	size_t sizes[] = { 1000, 1000 };
	uint8_t buf[1000] = { 0 };
	allocator_t **a = allocator_group_init(sizes, n, 0,
	    n * cluster_size(), NULL);

	for (unsigned m = 0; m < n; m++) {
		for (uint64_t i = 0; i < recs_per_cluster(1000); i++)
			(void) allocator_append(a[m], buf);
	}
	(void) allocator_append(a[n - 1], buf);
}

typedef struct {
	boolean_t	wf_evict;	/* Fail in an eviction, not a store */
	boolean_t	wf_enospc;	/* Fail with ENOSPC, not EFBIG */
} write_failure_t;

/*
 * A write to a backing file that fails must be fatal, whether it comes
 * from a store on the disk side or from an eviction.
 *
 * - With wf_enospc clear, writes fail because they cross RLIMIT_FSIZE;
 *   with SIGXFSZ ignored, they return EFBIG. The eviction writes a whole
 *   cluster, so its first pwritev() is partial, and the failure comes on
 *   the retry.
 *
 * - With wf_enospc set, member 0's backing file is replaced with /dev/full
 *   (Linux only), whose writes fail with ENOSPC. The message must then
 *   name the spill directory. The group keeps its own copy of the name, so
 *   the caller's copy is scribbled over first.
 */
static void
child_write_failure(void *arg)
{
	write_failure_t *wf = arg;
	size_t sizes[] = { 4096, 4096 };
	uint8_t buf[4096] = { 0 };
	struct rlimit fsize = { 16 * 4096, 16 * 4096 };
	char dir[PATH_MAX];

	if (!wf->wf_enospc) {
		(void) signal(SIGXFSZ, SIG_IGN);
		VERIFY0(setrlimit(RLIMIT_FSIZE, &fsize));
	}
	(void) snprintf(dir, sizeof (dir), "%s", selftest_scratch_dir());
	allocator_t **a = allocator_group_init(sizes, 2, 0,
	    wf->wf_evict ? 2 * cluster_size() : 0, dir);
	if (wf->wf_enospc) {
		int full = open("/dev/full", O_RDWR);
		VERIFY3S(full, >=, 0);
		VERIFY3S(dup2(full, a[0]->a_fd), ==, a[0]->a_fd);
		(void) memset(dir, 'X', strlen(dir));
	}
	if (wf->wf_evict) {
		/*
		 * Each member takes one of the two clusters, and member 0
		 * fills its own. Member 1 then evicts it, from offset 0.
		 */
		(void) allocator_append(a[1], buf);
		for (uint64_t i = 0; i < recs_per_cluster(4096); i++)
			(void) allocator_append(a[0], buf);
		for (uint64_t i = 0; i < recs_per_cluster(4096); i++)
			(void) allocator_append(a[1], buf);
	} else {
		/* Disk-only appends; RLIMIT_FSIZE stops record 16 */
		for (int i = 0; i < 32; i++)
			(void) allocator_append(a[0], buf);
	}
}

/*
 * An iovec array of n entries over src, which must hold 40 * n bytes. With
 * zeros set, about a third of the entries are empty, and so are the first
 * two, the last two, and the two on each side of every IOV_MAX boundary.
 * Returns the total length.
 */
static size_t
build_iovecs(struct iovec *iov, uint64_t n, uint8_t *src, boolean_t zeros,
    selftest_rng_t *rng)
{
	size_t total = 0;

	for (uint64_t i = 0; i < n; i++) {
		uint64_t pos = i % IOV_MAX;
		size_t len = 1 + selftest_rng_below(rng, 40);
		if (zeros && (selftest_rng_below(rng, 3) == 0 || i < 2 ||
		    i + 2 >= n || pos < 2 || pos + 2 >= IOV_MAX))
			len = 0;
		iov[i].iov_base = src + total;
		iov[i].iov_len = len;
		total += len;
	}
	return (total);
}

/*
 * Whether byte pos of the data falls between two entries, or at either end,
 * rather than inside an entry
 */
static boolean_t
iov_boundary(const struct iovec *iov, uint64_t n, size_t pos)
{
	size_t start = 0;

	for (uint64_t i = 0; i < n; i++) {
		if (pos == start)
			return (B_TRUE);
		if (pos < start + iov[i].iov_len)
			return (B_FALSE);
		start += iov[i].iov_len;
	}
	return (B_TRUE);
}

typedef struct {
	int		pw_fd;
	uint64_t	pw_n;
	off_t		pw_off;
	off_t		pw_fsize;	/* RLIMIT_FSIZE, or 0 for none */
	uint64_t	pw_stream;	/* For build_iovecs() */
	const char	*pw_where;
} pwritev_args_t;

static selftest_rng_t shim_rng;
static uint64_t shim_short_writes;

/*
 * A stand-in for pwritev() that checks what safe_pwritev() passes it, then
 * fails with EINTR one time in four and otherwise writes from 1 to 200
 * bytes, which may end anywhere, including partway through an entry
 */
static ssize_t
short_pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
	struct iovec part[IOV_MAX];
	size_t budget = 1 + selftest_rng_below(&shim_rng, 200);
	size_t offered = 0;
	int n;

	VERIFY3S(cnt, >, 0);
	VERIFY3S(cnt, <=, IOV_MAX);
	VERIFY3U(iov[0].iov_len, >, 0);
	if (selftest_rng_below(&shim_rng, 4) == 0) {
		errno = EINTR;
		return (-1);
	}
	for (n = 0; n < cnt; n++)
		offered += iov[n].iov_len;
	for (n = 0; n < cnt && budget > 0; n++) {
		part[n] = iov[n];
		part[n].iov_len = MIN(part[n].iov_len, budget);
		budget -= part[n].iov_len;
	}
	ssize_t written = pwritev(fd, part, n, off);
	if (written > 0 && (size_t)written < offered)
		shim_short_writes++;
	return (written);
}

/*
 * A stand-in for pwritev() that never makes progress
 */
static ssize_t
stuck_pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
	(void) fd;
	(void) iov;
	(void) cnt;
	(void) off;
	return (0);
}

static void
child_pwritev(void *arg)
{
	pwritev_args_t *pw = arg;
	struct iovec *iov = safe_malloc(pw->pw_n * sizeof (struct iovec));
	uint8_t *src = safe_malloc(40 * pw->pw_n);
	selftest_rng_t rng;

	selftest_rng_init(&rng, pw->pw_stream);
	selftest_fill(src, 40 * pw->pw_n, pw->pw_stream);
	(void) build_iovecs(iov, pw->pw_n, src, B_TRUE, &rng);
	if (pw->pw_fsize != 0) {
		struct rlimit fsize = { pw->pw_fsize, pw->pw_fsize };
		(void) signal(SIGXFSZ, SIG_IGN);
		VERIFY0(setrlimit(RLIMIT_FSIZE, &fsize));
	}
	safe_pwritev(pw->pw_fd, iov, pw->pw_n, pw->pw_off, pw->pw_where);
}

static void
child_stuck_pwritev(void *arg)
{
	safe_pwritev_call = stuck_pwritev;
	child_pwritev(arg);
}

/*
 * Check that a file holds exactly the first len bytes of src at offset
 * off, and nothing else.
 */
static void
check_file_contents(int fd, off_t off, const uint8_t *src, size_t len)
{
	struct stat st;
	uint8_t *buf = safe_malloc(off + len + 1);

	VERIFY0(fstat(fd, &st));
	VERIFY3S(st.st_size, ==, off + len);
	safe_pread_zero(fd, buf, off + len, 0);
	VERIFY(all_zero(buf, (size_t)off));
	if (memcmp(buf + off, src, len) != 0)
		errx(1, "pwritev() wrote the wrong data");
	free(buf);
}

/*
 * Write iovec arrays of many lengths, with and without empty entries, each
 * at its own offset in a new file, and check what lands there
 */
#define	PWRITEV_MAX_IOVECS	(2 * IOV_MAX + 7)

static void
pwritev_matrix(struct iovec *iov, uint8_t *src, uint64_t base_stream)
{
	const uint64_t counts[] = { 1, 2, IOV_MAX - 1, IOV_MAX, IOV_MAX + 1,
	    PWRITEV_MAX_IOVECS };
	selftest_rng_t rng;

	for (int zeros = 0; zeros < 2; zeros++) {
		for (int i = 0; i < ARRAY_SIZE(counts); i++) {
			uint64_t stream = base_stream + 10 * zeros + i;
			int fd = safe_create_temp_file(selftest_scratch_dir());
			off_t off = 4096 + 13 * i;

			selftest_rng_init(&rng, stream);
			selftest_fill(src, 40 * counts[i], stream);
			size_t total = build_iovecs(iov, counts[i], src, zeros,
			    &rng);
			safe_pwritev(fd, iov, counts[i], off, NULL);
			check_file_contents(fd, (total == 0) ? 0 : off, src,
			    total);
			VERIFY0(close(fd));
		}
	}
}

/*
 * safe_pwritev(), through iovec arrays that the allocator never builds:
 * more than IOV_MAX entries, so that one array takes several calls, with
 * and without empty entries, including runs of them that straddle each
 * IOV_MAX boundary. The data must land at the given offset, with nothing
 * written past it, and an array with no bytes in it writes nothing. Then
 * the same again, through a stand-in for pwritev() that makes short writes
 * and fails with EINTR, which the kernel never does for regular files.
 *
 * Then a write that RLIMIT_FSIZE cuts off partway through an entry of its
 * second call. The call after that one fails with EFBIG, which must be
 * fatal, but only after every byte below the limit has been written in
 * the right place. A call that writes nothing must be fatal rather than
 * spin. Last, a write that fails with ENOSPC (Linux only, on /dev/full) is
 * fatal without naming a directory when it is given none.
 */
static void
alloc_pwritev(void)
{
	const uint64_t max = PWRITEV_MAX_IOVECS;
	struct iovec *iov = safe_malloc(max * sizeof (struct iovec));
	uint8_t *src = safe_malloc(40 * max);
	selftest_rng_t rng;

	pwritev_matrix(iov, src, 100);
	selftest_rng_init(&shim_rng, 300);
	shim_short_writes = 0;
	safe_pwritev_call = short_pwritev;
	pwritev_matrix(iov, src, 150);
	safe_pwritev_call = pwritev;
	VERIFY3U(shim_short_writes, >, 0);

	/* Nothing at all, or only empty entries */
	int fd = safe_create_temp_file(selftest_scratch_dir());
	safe_pwritev(fd, iov, 0, 100, NULL);
	for (uint64_t i = 0; i < max; i++)
		iov[i].iov_len = 0;
	safe_pwritev(fd, iov, max, 100, NULL);
	check_file_contents(fd, 0, src, 0);
	VERIFY0(close(fd));

	/* Cut off partway through the second call */
	pwritev_args_t pw = {
		.pw_fd = safe_create_temp_file(selftest_scratch_dir()),
		.pw_n = max,
		.pw_off = 5000,
		.pw_stream = 200,
	};
	selftest_rng_init(&rng, pw.pw_stream);
	selftest_fill(src, 40 * max, pw.pw_stream);
	size_t total = build_iovecs(iov, max, src, B_TRUE, &rng);
	size_t first = 0;
	for (uint64_t i = 0; i < IOV_MAX; i++)
		first += iov[i].iov_len;
	size_t cut = first + (total - first) / 3;
	while (cut < total && iov_boundary(iov, max, cut))
		cut++;
	pw.pw_fsize = pw.pw_off + cut;
	selftest_expect_exit("pwritev() cut short", child_pwritev, &pw,
	    "pwritev() failed: File too large");
#if defined(__linux__)
	/* Linux writes up to the limit, so the second call is partial */
	check_file_contents(pw.pw_fd, pw.pw_off, src, cut);
#else
	/* Elsewhere, a write that crosses the limit might write nothing */
	struct stat st;
	VERIFY0(fstat(pw.pw_fd, &st));
	VERIFY3S(st.st_size, >=, pw.pw_off + first);
	VERIFY3S(st.st_size, <=, pw.pw_off + cut);
	check_file_contents(pw.pw_fd, pw.pw_off, src,
	    st.st_size - pw.pw_off);
#endif
	VERIFY0(close(pw.pw_fd));

	pw.pw_fd = safe_create_temp_file(selftest_scratch_dir());
	pw.pw_fsize = 0;
	selftest_expect_exit("pwritev() stuck", child_stuck_pwritev, &pw,
	    "pwritev() failed: Input/output error");
	check_file_contents(pw.pw_fd, 0, src, 0);
	VERIFY0(close(pw.pw_fd));
#if defined(__linux__)
	pw.pw_fd = open("/dev/full", O_WRONLY);
	VERIFY3S(pw.pw_fd, >=, 0);
	pw.pw_fsize = 0;
	selftest_expect_exit("pwritev() out of space", child_pwritev, &pw,
	    "pwritev() failed: No space left on device");
	VERIFY0(close(pw.pw_fd));
#endif
	free(iov);
	free(src);
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
 * Under RLIMIT_DATA, mmap() fails with ENOMEM long before the group's
 * budget runs out. A disk-backed allocator must pin its split at its last
 * cluster and carry on from disk with every record intact. A memory-only
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
	allocator_t **a = allocator_group_init(&size, 1, 0, 32 * cluster,
	    disk ? selftest_scratch_dir() : NULL);
	allocator_t *alloc = a[0];
	rlim_t lim = vm_data_bytes() + 3 * cluster;
	struct rlimit data = { lim, lim };

	VERIFY0(setrlimit(RLIMIT_DATA, &data));
	for (uint64_t i = 0; i < nrecs; i++) {
		selftest_fill(buf, size, i + 1);
		VERIFY3U(allocator_append(alloc, buf), ==, i);
	}

	/* The split was pinned at the last cluster, short of the budget */
	VERIFY3U(alloc->a_first_disk_cluster, ==, alloc->a_cluster_count);
	VERIFY3U(alloc->a_cluster_count, <=, 3);
	VERIFY3U(alloc->a_group->ag_clusters_current, ==,
	    alloc->a_cluster_count);
	for (uint64_t i = 0; i < nrecs; i++) {
		allocator_retrieve(alloc, i, buf);
		selftest_fill(expect, size, i + 1);
		if (memcmp(buf, expect, size) != 0) {
			errx(1, "record %llu corrupted after ENOMEM spill",
			    (u_longlong_t)i);
		}
	}
	allocator_group_fini(alloc);
}

static void
check_record(allocator_t *alloc, uint64_t ix, uint64_t tag)
{
	uint8_t buf[MAX_RECORD_SIZE];
	uint8_t expect[MAX_RECORD_SIZE];
	size_t size = alloc->a_record_size;

	(void) memset(buf, 0xa5, size);
	allocator_retrieve(alloc, ix, buf);
	if (tag == 0)
		(void) memset(expect, 0, size);
	else
		selftest_fill(expect, size, tag);
	if (memcmp(buf, expect, size) != 0)
		errx(1, "record %llu is corrupted", (u_longlong_t)ix);
}

/*
 * A sparse store needs six clusters at once, and RLIMIT_DATA lets only
 * some of them be mapped. Those stay mapped and counted, the writer's
 * split drops to them, and the record goes to disk. Once the limit is
 * lifted, the writer stays pinned, but member 0 can still grow into the
 * clusters that are free.
 */
static void
child_data_rlimit_sparse(void *arg)
{
	(void) arg;
	const size_t cluster = cluster_size();
	const uint64_t rpc = recs_per_cluster(4096);
	size_t sizes[] = { 4096, 4096 };
	uint8_t buf[4096];
	allocator_t **a = allocator_group_init(sizes, 2, 0, 8 * cluster,
	    selftest_scratch_dir());
	allocator_group_t *g = a[0]->a_group;
	struct rlimit saved, data;

	for (uint64_t i = 0; i < 2 * rpc; i++) {
		selftest_fill(buf, sizeof (buf), i + 1);
		VERIFY3U(allocator_append(a[0], buf), ==, i);
	}
	VERIFY0(getrlimit(RLIMIT_DATA, &saved));
	data = saved;
	data.rlim_cur = vm_data_bytes() + 3 * cluster + cluster / 2;
	VERIFY0(setrlimit(RLIMIT_DATA, &data));

	selftest_fill(buf, sizeof (buf), 1000);
	allocator_store(a[1], 5 * rpc, buf);
	uint64_t mapped = a[1]->a_cluster_count;
	VERIFY3U(mapped, >, 0);
	VERIFY3U(mapped, <, 6);
	VERIFY3U(a[1]->a_first_disk_cluster, ==, mapped);
	VERIFY3U(g->ag_clusters_current, ==, 2 + mapped);
	VERIFY3U(a[0]->a_cluster_count, ==, 2);
	VERIFY3U(a[0]->a_first_disk_cluster, ==, 8);
	audit_cluster_table(g);

	VERIFY0(setrlimit(RLIMIT_DATA, &saved));
	selftest_fill(buf, sizeof (buf), 1001);
	allocator_store(a[1], mapped * rpc, buf);
	VERIFY3U(a[1]->a_cluster_count, ==, mapped);
	for (uint64_t i = 2 * rpc; i < 3 * rpc; i++) {
		selftest_fill(buf, sizeof (buf), i + 1);
		VERIFY3U(allocator_append(a[0], buf), ==, i);
	}
	VERIFY3U(a[0]->a_cluster_count, ==, 3);
	VERIFY3U(g->ag_clusters_current, ==, 3 + mapped);
	selftest_audit_group(a[0]);

	for (uint64_t i = 0; i < 3 * rpc; i++)
		check_record(a[0], i, i + 1);
	check_record(a[1], 0, 0);
	check_record(a[1], mapped * rpc - 1, 0);
	check_record(a[1], mapped * rpc, 1001);
	check_record(a[1], mapped * rpc + 1, 0);
	check_record(a[1], 5 * rpc, 1000);
	allocator_group_fini(a[1]);
}
#endif

/*
 * Behavior when the operating system, rather than the group's budget,
 * refuses a request. The ENOSPC cases are Linux-only because they rely on
 * /dev/full. The RLIMIT_DATA cases are Linux-only, since other systems
 * apply that limit only to the heap.
 */
static void
alloc_resource_limits(void)
{
	write_failure_t wf = { .wf_evict = B_FALSE };
	selftest_expect_exit("disk write failure", child_write_failure,
	    &wf, "pwrite() failed");
	wf.wf_evict = B_TRUE;
	selftest_expect_exit("eviction write failure", child_write_failure,
	    &wf, "pwritev() failed");
#if defined(__linux__)
	char expect[PATH_MAX + 32];
	(void) snprintf(expect, sizeof (expect), "out of space in %s,",
	    selftest_scratch_dir());
	wf.wf_enospc = B_TRUE;
	wf.wf_evict = B_FALSE;
	selftest_expect_exit("disk write out of space", child_write_failure,
	    &wf, expect);
	wf.wf_evict = B_TRUE;
	selftest_expect_exit("eviction out of space", child_write_failure,
	    &wf, expect);

	boolean_t disk = B_TRUE;
	selftest_expect_success("ENOMEM spill", child_data_rlimit, &disk,
	    "out of memory: spilling to");
	selftest_expect_success("ENOMEM sparse spill",
	    child_data_rlimit_sparse, NULL,
	    "out of memory: spilling to");
	disk = B_FALSE;
	selftest_expect_exit("ENOMEM, memory-only", child_data_rlimit, &disk,
	    "mprotect() failed: Cannot allocate memory");
#endif
}

/*
 * The fatal paths, each run in a child process that must exit with the
 * expected message: invalid parameters, a missing spill directory, and
 * running out of memory in a memory-only group.
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

	/*
	 * A limit one past the largest that can be rounded up to a cluster
	 * without overflow (or exceeding an off_t) is rejected. The largest
	 * is accepted, and then the member's cluster table, with a pointer
	 * for each of 2^43 clusters, is too big to allocate. The child caps
	 * its address space so that the allocation fails however the system
	 * overcommits. On 32-bit systems the table is only 16KB, so that case
	 * is skipped.
	 */
	fa.fa_disk = B_TRUE;
	fa.fa_limit = SIZE_MAX / 2 - cluster + 1;
	selftest_expect_exit("oversized limit", child_group_init, &fa,
	    "is too large");
#if !defined(_ILP32)
	fa.fa_limit = SIZE_MAX / 2 - cluster;
	fa.fa_cap_as = B_TRUE;
	selftest_expect_exit("largest limit", child_group_init, &fa,
	    "unable to allocate");
	fa.fa_cap_as = B_FALSE;
#endif

	fa.fa_limit = cluster;
	fa.fa_dir = "/nonexistent/zstream-selftest";
	selftest_expect_exit("missing spill directory", child_group_init,
	    &fa, "No such file or directory");

	unsigned n = 1;
	selftest_expect_exit("memory-only overflow",
	    child_memory_only_overflow, &n, "memory limit exceeded");
	n = 2;
	selftest_expect_exit("memory-only eviction",
	    child_memory_only_overflow, &n, "memory limit exceeded");
}

const test_case_t selftest_alloc_cases[] = {
	{ "alloc_layout",		alloc_layout },
	{ "alloc_cluster_size",		alloc_cluster_size },
	{ "alloc_basic",		alloc_basic },
	{ "alloc_record_sizes",		alloc_record_sizes },
	{ "alloc_reassignment",		alloc_reassignment },
	{ "alloc_eviction_batches",	alloc_eviction_batches },
	{ "alloc_priority",		alloc_priority },
	{ "alloc_boundaries",		alloc_boundaries },
	{ "alloc_lifecycle",		alloc_lifecycle },
	{ "alloc_rss_bounded",		alloc_rss_bounded },
	{ "alloc_stress",		alloc_stress },
	{ "alloc_pwritev",		alloc_pwritev },
	{ "alloc_resource_limits",	alloc_resource_limits },
	{ "alloc_fatal",		alloc_fatal },
	{ NULL,				NULL },
};
