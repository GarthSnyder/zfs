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

#include <assert.h>
#include <err.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stdtypes.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#include "zstream_alloc.h"
#include "zstream_util.h"

/*
 * This implementation exploits two features common to most UNIX-like systems.
 *
 * - Filesystem write holes: For a dual-backed allocator, the
 *   memory-resident portion of the data is treated as an overlay of the
 *   first part of the backing file. Memory and disk share the same
 *   addressing scheme for records: record 100 is always at the same offset
 *   whether it's in memory or on disk.
 *
 *   The first part of the disk file hides underneath the memory overlay and
 *   is never written to. Ergo, it occupies no actual storage space. Since
 *   memory and disk have common addressing, they can be rebalanced with a
 *   single write when the memory budget changes.
 *
 *   If the backing file's filesystem does not support holes (unlikely but
 *   possible), the code is still correct. However, actual disk space
 *   consumption will be higher.
 *
 * - PROT_NONE virtual pages: These pages do not consume physical memory,
 *   TLB entries, or swap space, so they are essentially free. Because of
 *   that, allocators can request a large, contiguous VM allocation up front
 *   and need never change their addressing scheme, even as memory use evolves.
 *
 *   When an allocator needs more writable pages, it incrementally changes
 *   pages' protection from PROT_NONE to PROT_READ | PROT_WRITE, at which
 *   point the pages acquire swap reservations and are charged against the RSS.
 */

#define	OFFSET_TO_ADDR(alloc, off)	((off) + (alloc)->a_base_addr)
#define	ADDR_TO_OFFSET(alloc, addr)	((addr) - (alloc)->a_base_addr)
#define	OFFSET_IS_ON_DISK(alloc, off)	((off) >= (alloc)->a_first_on_disk)

#define	TARGET_PAGE_CLUSTER_SIZE	(1 << 20)	/* ~1MB */

/*
 * All allocators are members of an allocator group, which can be a group of
 * 1. Group members share memory accounting.
 *
 * The group's page cluster size is the granularity of memory management for
 * all the group's allocators. It's selected at the time the group is
 * created, taking into account the record sizes of the group's allocators,
 * which may vary.
 *
 * An allocator's memory-to-disk transition always falls at a page cluster
 * boundary, and the group's ag_mem_limit and ag_mem_current values are also
 * multiples of the cluster size.
 */
typedef struct {
	size_t		ag_mem_limit;		/* Total allowed for group */
	size_t		ag_mem_current;		/* Currently used by group */
	size_t		ag_cluster_size;	/* Allocation unit */
	unsigned int	ag_num_allocators;
	allocator_t	*ag_allocators[MAX_ALLOCATORS_PER_GROUP];
} allocator_group_t;

struct allocator {
	int		   a_fd;		  /* On-disk file descriptor */
	size_t		   a_record_size;	  /* Client-specified */
	uint64_t	   a_records_per_cluster; /* Constant (avoid divs) */
	off_t		   a_first_on_disk;	  /* Memory/disk split point */
	allocator_group_t  *a_group;		  /* Back-pointer to group */
	uint64_t	   a_count;		  /* Highest index stored +1 */
	void		   *a_base_addr;	  /* Start of VM allotment */
	void		   *a_writable_frontier;  /* Addr of 1st non-r/w byte */
};

static allocator_group_t groups[MAX_ALLOCATOR_GROUPS] = {{0}};

static boolean_t
try_make_readwrite(allocator_t *alloc, off_t end_offset);

/*
 * Each page cluster consists of N pages of memory with M records inside of
 * it. Records are stored back to back with no gaps. However, N * pagesize
 * isn't necessarily divisible by a_record_size, so the trailing bytes of a
 * cluster may be waste space.
 */
static inline off_t
record_offset(allocator_t *alloc, record_ix_t rec)
{
	ASSERT(alloc != NULL);
	uint64_t cluster = rec / alloc->a_records_per_cluster;
	uint64_t record_in_cluster = rec % alloc->a_records_per_cluster;
	off_t cluster_offset = cluster * alloc->a_group->ag_cluster_size;
	off_t record_offset = record_in_cluster * alloc->a_record_size;
	return (cluster_offset + record_offset);
}

/*
 * Here we search for a page cluster size that is roughly
 * TARGET_PAGE_CLUSTER_SIZE (1MB) and that minimizes waste. All the
 * allocators in a group share the same page cluster size, so we're looking
 * for a cluster size that minimizes waste over all record sizes in the
 * group. For records less than 8K in size, waste will always be less than
 * 0.5% of total storage for page sizes up to 64K.
 *
 * This is a simple brute-force search, but with a target cluster size of
 * ~1MB, there will be at most 256 options to examine.
 */
static size_t
optimal_cluster_size(const size_t *record_sizes, unsigned n)
{
	long pagesize = sysconf(_SC_PAGESIZE);
	VERIFY(pagesize > 0 && pagesize <= TARGET_PAGE_CLUSTER_SIZE && n > 0);
	size_t biggest = 0;
	for (int i = 0; i < n; i++) {
		biggest = MAX(biggest, record_sizes[i]);
		if (record_sizes[i] > MAX_PAGES_PER_RECORD * pagesize)
			errx(1, "requested allocator_t record size too large "
			    "(%llu bytes)", (u_longlong_t)record_sizes[i]);
	}

	size_t best_cluster = 0;
	size_t least_waste = TARGET_PAGE_CLUSTER_SIZE;
	int max_pages = TARGET_PAGE_CLUSTER_SIZE / pagesize;
	for (int np = (biggest / pagesize) + 1; np <= max_pages; np++) {
		size_t cluster_size = np * pagesize;
		size_t most_waste = 0;
		for (int i = 0; i < n; i++) {
			size_t waste = cluster_size % record_sizes[i];
			most_waste = MAX(most_waste, waste);
		}
		if (most_waste <= least_waste) {
			least_waste = most_waste;
			best_cluster = cluster_size;
		}
	}
	return (best_cluster);
}

static allocator_group_t *
find_empty_group_slot(void)
{
	int group;
	for (group = 0; group < MAX_ALLOCATOR_GROUPS; group++) {
		if (groups[group].ag_num_allocators == 0)
			return (&groups[group]);
	}
	errx(1, "only %d allocator groups may be active at once",
	    MAX_ALLOCATOR_GROUPS);
}

allocator_t **
allocator_group_init(const size_t *record_sizes, unsigned n,
    size_t memory_limit, const char *dir_path)
{
	if (memory_limit == 0 && dir_path == NULL)
		errx(1, "allocator group needs either disk or memory backing");
	if (n < 1 || n > MAX_ALLOCATORS_PER_GROUP)
		errx(1, "invalid number of allocators: %d", n);

	size_t cluster_size =
	    optimal_cluster_size(record_sizes, n);

	allocator_group_t *group = find_empty_group_slot();
	*group = (allocator_group_t) {
		.ag_mem_limit = ROUND_UP(memory_limit, cluster_size),
		.ag_mem_current = 0,
		.ag_cluster_size = cluster_size,
		.ag_num_allocators = n
	};

	for (int i = 0; i < n; i++) {
		void *base = NULL;
		if (group->ag_mem_limit > 0) {
			base = mmap(NULL, group->ag_mem_limit,
			    PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (base == MAP_FAILED)
				errx(1, "mmap failed in %s", __func__);
		}
		group->ag_allocators[i] = safe_malloc(sizeof (allocator_t));
		*group->ag_allocators[i] = (allocator_t) {
			.a_fd = (dir_path != NULL) ?
			    safe_create_temp_file(dir_path) : -1,
			.a_record_size = record_sizes[i],
			.a_records_per_cluster = cluster_size / record_sizes[i],
			.a_first_on_disk = group->ag_mem_limit,
			.a_group = group,
			.a_base_addr = base,
			.a_writable_frontier = base
		};
	}
	return (&group->ag_allocators[0]);
}

void
allocator_store(allocator_t *alloc, record_ix_t record, const void *buff)
{
	VERIFY(alloc != NULL && buff != NULL);
	off_t off = record_offset(alloc, record);
	off_t next_off = off + alloc->a_record_size;
	if (OFFSET_IS_ON_DISK(alloc, off)) {
		if (alloc->a_fd < 0)
			errx(1, "no disk file for allocator record %llu "
			    "(write)", (u_longlong_t)record);
		safe_pwrite(alloc->a_fd, buff, alloc->a_record_size, off);
	} else if (try_make_readwrite(alloc, next_off)) {
		memcpy(OFFSET_TO_ADDR(alloc, off), buff, alloc->a_record_size);
	} else {
		/* Restart if converted to a disk operation */
		return (allocator_store(alloc, record, buff));
	}
	/* a_count is one past the highest record known to have been written */
	alloc->a_count = MAX(alloc->a_count, record + 1);
}

void
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buff)
{
	VERIFY(alloc != NULL && buff != NULL);
	off_t off = record_offset(alloc, record);
	if (OFFSET_IS_ON_DISK(alloc, off)) {
		if (alloc->a_fd < 0)
			errx(1, "no disk file for allocator record %llu (read)",
			    (u_longlong_t)record);
		safe_pread_zero(alloc->a_fd, buff, alloc->a_record_size, off);
	} else {
		void *first = OFFSET_TO_ADDR(alloc, off);
		void *last_plus_one = first + alloc->a_record_size;
		/*
		 * Even though we are reading from below the memory/disk
		 * split point, we might still be trying to read from a
		 * region that is PROT_NONE. But in that case, the record
		 * has never actually been written, so we can safely "read"
		 * it back as zeros.
		 */
		if (last_plus_one <= alloc->a_writable_frontier)
			memcpy(buff, first, alloc->a_record_size);
		else
			memset(buff, 0, alloc->a_record_size);
	}
}

record_ix_t
allocator_append(allocator_t *alloc, const void *data)
{
	record_ix_t loc = alloc->a_count;
	allocator_store(alloc, loc, data);
	return (loc);
}

/*
 * We must write actual data to preserve the invariant that a_count == one
 * past the last record known to have been written.
 */
record_ix_t
allocator_skip(allocator_t *alloc)
{
	ASSERT(alloc != NULL);
	uint8_t buff[alloc->a_record_size];
	memset(buff, 0, sizeof (buff));
	record_ix_t ix = allocator_append(alloc, buff);
	return (ix);
}

/*
 * Try to free at least delta_bytes of memory from a given allocator. This
 * may not be possible if the allocator has insufficient memory to yield.
 * The return value is the amount actually freed. This value will always be
 * a multiple of the cluster size.
 *
 * a_writable_frontier is the end of the memory actually in use, but we need
 * to also lower a_first_on_disk when yielding memory. Otherwise, the
 * allocator will just try to reallocate memory when later records are
 * written.
 *
 * Since memory and disk segments share offset addresses, we only need to
 * perform one copy from memory to disk to change the split point. Only
 * bytes below the writable frontier are written out. Bytes between the
 * frontier and the old memory budget were never written and are logically
 * zero. The corresponding file region has never been written, so it already
 * reads back as zeros.
 */
static size_t
free_memory_from(allocator_t *alloc, size_t delta_bytes)
{
	ASSERT(alloc != NULL && delta_bytes > 0);
	if (alloc->a_base_addr == NULL || alloc->a_first_on_disk == 0)
		return (0);
	size_t delta = ROUND_UP(delta_bytes, alloc->a_group->ag_cluster_size);
	size_t bytes_used = ADDR_TO_OFFSET(alloc, alloc->a_writable_frontier);
	size_t new_fod = bytes_used - MIN(delta, bytes_used);

	void *eject_start = OFFSET_TO_ADDR(alloc, new_fod);
	void *eject_end = alloc->a_writable_frontier;
	size_t bytes_to_free = MAX(0, eject_end - eject_start);
	if (bytes_to_free > 0) {
		if (alloc->a_fd < 0)
			errx(1, "allocator has no disk backing, so %s would "
			    "lose data", __func__);
		safe_pwrite(alloc->a_fd, eject_start, bytes_to_free, new_fod);
		void *ret = mmap(eject_start, bytes_to_free, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (ret == MAP_FAILED)
			err(1, "mmap (frontier shrink) failed");
		alloc->a_writable_frontier = eject_start;
		alloc->a_first_on_disk = new_fod;
		alloc->a_group->ag_mem_current -= bytes_to_free;
	}
	return (bytes_to_free);
}

/*
 * Try to free at least delta_bytes of memory from some combination of
 * allocators of lower priority than the named allocator. Returns the amount
 * of memory actually freed, which may be less than delta_bytes (or 0).
 */
static inline size_t
free_memory_for(allocator_t *alloc, size_t delta_bytes)
{
	ASSERT(alloc != NULL && delta_bytes > 0);
	size_t freed = 0;
	allocator_group_t *group = alloc->a_group;
	for (int i = 0; i < group->ag_num_allocators; i++) {
		if (group->ag_allocators[i] == alloc)
			break;
		freed += free_memory_from(group->ag_allocators[i],
		    delta_bytes - freed);
		if (freed >= delta_bytes)
			break;
	}
	return (freed);
}

/*
 * Check whether an offset corresponds to writable memory, and if not,
 * attempt to expand the allocator's writable region to include it. If the
 * allocator group has sufficient unused memory, this can be done directly.
 * If not, we call free_memory_for() to try and recover memory from lower
 * priority allocators.
 *
 * If the attempt to obtain memory from another allocator fails, we lower
 * the allocator's a_first_on_disk offset to the current writable frontier
 * and return B_FALSE. Retrying the transaction should then succeed as a disk
 * operation.
 *
 * The end_offset parameter and the a_writable_frontier pointer are both
 * "+1" markers. I.e., everything below a_writable_fronter is already
 * writable, and try_make_readwrite() makes memory writable up to but not
 * including the end_offset. a_writable_frontier always points to the first
 * byte of an unwritable memory page.
 */
static inline boolean_t
try_make_readwrite(allocator_t *alloc, off_t end_offset)
{
	void *end_addr = OFFSET_TO_ADDR(alloc, end_offset);
	if (end_addr <= alloc->a_writable_frontier)
		return (B_TRUE);
	allocator_group_t *group = alloc->a_group;
	size_t needed = end_addr - alloc->a_writable_frontier;
	size_t avail = group->ag_mem_limit - group->ag_mem_current;
	if (needed > avail) {
		size_t deficit = needed - avail;
		size_t freed = free_memory_for(alloc, deficit);
		if (freed < deficit) {
			alloc->a_first_on_disk =
			    ADDR_TO_OFFSET(alloc, alloc->a_writable_frontier);
			return (B_FALSE);
		}
	}
	size_t length = ROUND_UP(needed, group->ag_cluster_size);
	/* Checked before, but we may have since done multiple frees */
	VERIFY3U(group->ag_mem_limit - group->ag_mem_current, >=, length);
	int rc = mprotect(alloc->a_writable_frontier, length,
	    PROT_READ | PROT_WRITE);
	if (rc != 0)
		err(1, "mprotect failed");
	alloc->a_writable_frontier += length;
	group->ag_mem_current += length;
	return (B_TRUE);
}

/*
 * May be called with any allocator in the group. Frees the entire group.
 */
void
allocator_group_fini(allocator_t *alloc)
{
	VERIFY(alloc != NULL);
	allocator_group_t *group = alloc->a_group;
	for (int i = 0; i < group->ag_num_allocators; i++) {
		allocator_t *group_alloc = group->ag_allocators[i];
		if (group_alloc->a_base_addr != NULL)
			munmap(group_alloc->a_base_addr, group->ag_mem_limit);
		if (group_alloc->a_fd >= 0) {
			if (close(group_alloc->a_fd) != 0)
				warn("unable to close allocator backing file");
		}
		free(group_alloc);
	}
	*group = (allocator_group_t) {
		.ag_num_allocators = 0
	};
}
