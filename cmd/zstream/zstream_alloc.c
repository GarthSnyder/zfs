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
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stdtypes.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "zstream_alloc.h"
#include "zstream_alloc_impl.h"
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
 *   is never written to. Ergo, it occupies no storage space on filesystems
 *   that support holes. Since memory and disk have common addressing,
 *   clusters can be moved from memory to disk by writing them to their same
 *   offsets in the file.
 *
 *   If the backing file's filesystem does not support holes (unlikely but
 *   possible), the code is still correct. However, actual disk space
 *   consumption will be higher.
 *
 * - PROT_NONE virtual pages: These pages do not consume physical memory,
 *   TLB entries, or swap space, so they are essentially free. Because of
 *   that, allocator groups can request a large, contiguous VM allocation
 *   up front. (The pages do still count as address space, so the whole
 *   budget must fit within RLIMIT_AS.)
 *
 *   The group makes clusters writable one at a time, in address order, by
 *   changing their protection from PROT_NONE to PROT_READ | PROT_WRITE.
 *   Clusters never revert to PROT_NONE. Once the whole region is writable,
 *   memory moves between allocators only by transfer: an evicted cluster is
 *   zeroed and handed directly to the allocator that needs it.
 */

/*
 * In concept, an allocator_t splits storage between a memory portion and a
 * disk portion. However, there's a liminal third region between allocated
 * memory and disk that is nominally part of the memory region but for which
 * no memory has yet been allocated, and where no records are stored. This
 * region is delineated by a_cluster_count on the low end (which marks the
 * upper end of memory allocations) and a_first_disk_cluster on the high end
 * (which marks the start of the disk region).
 *
 * Writes into the unallocated region cause additional allocations (or
 * reshufflings) of memory. Those must be zero-filled on allocation because
 * never-written records are defined to read as zeros.
 */
typedef enum {
	ZA_ON_DISK,
	ZA_IN_WRITABLE_MEMORY,
	ZA_IN_UNALLOCATED_MEMORY
} region_t;

typedef struct {
	off_t		offset;
	uint64_t	cluster;
	off_t		offset_in_cluster;
	region_t	region;
} record_location_t;

static inline boolean_t
try_make_writable(allocator_t *alloc, uint64_t cluster);

/*
 * Each page cluster holds a_records_per_cluster records, stored back to
 * back with no gaps. The cluster size isn't necessarily divisible by
 * a_record_size, so the trailing bytes of a cluster may be waste space.
 * Since records are at most MAX_RECORD_SIZE bytes, the waste is always less
 * than 1% of the cluster at the default page cluster size of 1MB.
 */
static inline record_location_t
locate_record(allocator_t *alloc, record_ix_t rec)
{
	ASSERT(alloc != NULL);
	uint64_t cluster = rec / alloc->a_records_per_cluster;
	uint64_t record_in_cluster = rec % alloc->a_records_per_cluster;
	off_t cluster_offset = cluster * alloc->a_group->ag_cluster_size;
	off_t rec_offset = record_in_cluster * alloc->a_record_size;

	region_t region;
	if (cluster >= alloc->a_first_disk_cluster)
		region = ZA_ON_DISK;
	else if (cluster < alloc->a_cluster_count)
		region = ZA_IN_WRITABLE_MEMORY;
	else
		region = ZA_IN_UNALLOCATED_MEMORY;

	record_location_t loc = {
		.offset = cluster_offset + rec_offset,
		.cluster = cluster,
		.offset_in_cluster = rec_offset,
		.region = region
	};
	return (loc);
}

allocator_t **
allocator_group_init(const size_t *record_sizes, unsigned num_allocators,
    size_t cluster_size, size_t mem_limit, const char *dir_path)
{
	if (mem_limit == 0 && dir_path == NULL)
		errx(1, "allocator group needs either disk or memory backing");
	if (num_allocators == 0)
		errx(1, "invalid number of allocators: %u", num_allocators);
	size_t max_record_size = 0;
	for (int i = 0; i < num_allocators; i++) {
		if (record_sizes[i] == 0 || record_sizes[i] > MAX_RECORD_SIZE)
			errx(1, "requested allocator_t record size is invalid "
			    "(%llu bytes)", (u_longlong_t)record_sizes[i]);
		max_record_size = MAX(max_record_size, record_sizes[i]);
	}

	long pagesize = sysconf(_SC_PAGESIZE);
	if (pagesize <= 0)
		errx(1, "unable to get system page size");
	if (cluster_size == 0)
		cluster_size = MAX(DEFAULT_CLUSTER_SIZE, pagesize);
	else
		cluster_size = roundup(MAX(MAX(cluster_size, max_record_size),
		    pagesize), pagesize);
	if (mem_limit > SIZE_MAX / 2 - num_allocators * cluster_size)
		errx(1, "requested allocator memory size %llu is too large",
		    (u_longlong_t)mem_limit);

	uint8_t *base_address = NULL;
	if (mem_limit > 0) {
		mem_limit = MAX(mem_limit, num_allocators * cluster_size);
		mem_limit = roundup(mem_limit, cluster_size);
		base_address = mmap(NULL, mem_limit, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base_address == MAP_FAILED)
			err(1, "unable to reserve %llu bytes of address space",
			    (u_longlong_t)mem_limit);
	}

	allocator_group_t *group = safe_malloc(sizeof (allocator_group_t));
	*group = (allocator_group_t) {
		.ag_clusters_max = mem_limit / cluster_size,
		.ag_cluster_size = cluster_size,
		/*
		 * strdup() can fail, but ag_dir is only used as error
		 * message text; the error value (NULL) is already OK.
		 */
		.ag_dir = (dir_path != NULL) ? strdup(dir_path) : NULL,
		.ag_num_allocators = num_allocators,
		.ag_allocators =
		    safe_malloc(num_allocators * sizeof (allocator_t *)),
		.ag_base_addr = base_address
	};

	for (int i = 0; i < num_allocators; i++) {
		uint8_t **cluster_pointers = NULL;
		if (group->ag_clusters_max > 0)
			cluster_pointers = safe_malloc(sizeof (uint8_t *) *
			    group->ag_clusters_max);
		group->ag_allocators[i] = safe_malloc(sizeof (allocator_t));
		*group->ag_allocators[i] = (allocator_t) {
			.a_fd = (dir_path != NULL) ?
			    safe_create_temp_file(dir_path) : -1,
			.a_record_size = record_sizes[i],
			.a_records_per_cluster = cluster_size / record_sizes[i],
			.a_first_disk_cluster = group->ag_clusters_max,
			.a_clusters = cluster_pointers,
			.a_group = group
		};
	}
	return (group->ag_allocators);
}

void
allocator_store(allocator_t *alloc, record_ix_t record, const void *buff)
{
	VERIFY(alloc != NULL && buff != NULL);
	record_location_t loc;

retry:	loc = locate_record(alloc, record);
	switch (loc.region) {
	case ZA_ON_DISK:
		if (alloc->a_fd < 0)
			errx(1, "allocator memory limit exceeded, record %llu "
			    "(write)", (u_longlong_t)record);
		else
			safe_pwrite(alloc->a_fd, buff, alloc->a_record_size,
			    loc.offset, alloc->a_group->ag_dir);
		break;
	case ZA_IN_UNALLOCATED_MEMORY:
		if (try_make_writable(alloc, loc.cluster) == B_FALSE) {
			/*
			 * Retry as a disk operation. try_make_writable()
			 * already moved the split point, so locate_record()
			 * will now return ZA_ON_DISK.
			 */
			goto retry;
		}
		zfs_fallthrough;
	case ZA_IN_WRITABLE_MEMORY:
		memcpy(alloc->a_clusters[loc.cluster] + loc.offset_in_cluster,
		    buff, alloc->a_record_size);
	}
	alloc->a_record_count = MAX(alloc->a_record_count, record + 1);
}

void
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buff)
{
	VERIFY(alloc != NULL && buff != NULL);
	record_location_t loc = locate_record(alloc, record);

	switch (loc.region) {
	case ZA_ON_DISK:
		if (alloc->a_fd < 0)
			/*
			 * We may encounter reads beyond the memory region
			 * even if there is no disk region. That's fine for
			 * reading but will fail if attempted as a write.
			 */
			memset(buff, 0, alloc->a_record_size);
		else
			safe_pread_zero(alloc->a_fd, buff,
			    alloc->a_record_size, loc.offset);
		break;
	case ZA_IN_UNALLOCATED_MEMORY:
		/*
		 * Records in the unallocated region have never been written
		 * and we can safely "read" them back as zeros.
		 */
		memset(buff, 0, alloc->a_record_size);
		break;
	case ZA_IN_WRITABLE_MEMORY:
		memcpy(buff, alloc->a_clusters[loc.cluster] +
		    loc.offset_in_cluster, alloc->a_record_size);
	}
}

record_ix_t
allocator_append(allocator_t *alloc, const void *data)
{
	record_ix_t loc = alloc->a_record_count;
	allocator_store(alloc, loc, data);
	return (loc);
}

record_ix_t
allocator_skip(allocator_t *alloc)
{
	ASSERT(alloc != NULL);
	return (alloc->a_record_count++);
}

/*
 * Convert an allocator's last N memory clusters to disk storage and write
 * the starting addresses of those clusters to clist, which must have room
 * for num_clusters entries. Clusters are zeroed after being written out, so
 * the recipient can treat them like fresh memory.
 *
 * Since memory and disk segments share offset addresses, the process of
 * writing out memory contents to disk is relatively straightforward.
 * Ultimately, we lower both a_cluster_count and a_first_disk_cluster, the
 * former to reflect that we are releasing (transferring out) memory and the
 * latter to lower the memory/disk split point so that the allocator does not
 * attempt to reallocate memory on subsequent writes.
 *
 * safe_pwritev() subdivides pwritev() calls as needed, so it's fine if
 * IOVEC_BUFFSIZE > IOV_MAX.
 */
static inline void
release_clusters(allocator_t *alloc, uint64_t num_clusters, uint8_t **clist)
{
	VERIFY(alloc != NULL && num_clusters > 0);
	VERIFY3U(num_clusters, <=, alloc->a_cluster_count);
	VERIFY(alloc->a_fd >= 0);

	allocator_group_t *group = alloc->a_group;
	struct iovec clusters[IOVEC_BUFFSIZE];
	uint64_t first = alloc->a_cluster_count - num_clusters;
	off_t offset = first * group->ag_cluster_size;

	while (first < alloc->a_cluster_count) {
		uint64_t this_batch = MIN(alloc->a_cluster_count - first,
		    IOVEC_BUFFSIZE);
		for (uint64_t i = 0; i < this_batch; i++) {
			clusters[i] = (struct iovec) {
				.iov_base = alloc->a_clusters[first + i],
				.iov_len = group->ag_cluster_size
			};
			*clist++ = alloc->a_clusters[first + i];
		}
		safe_pwritev(alloc->a_fd, clusters, this_batch, offset,
		    group->ag_dir);
		for (uint64_t i = 0; i < this_batch; i++) {
			memset(alloc->a_clusters[first + i], 0,
			    group->ag_cluster_size);
		}
		first += this_batch;
		offset += this_batch * group->ag_cluster_size;
	}
	alloc->a_cluster_count -= num_clusters;
	alloc->a_first_disk_cluster = alloc->a_cluster_count;
}

/*
 * Attempt to poach num_clusters clusters from some combination of
 * allocators of lower priority and reassign them to the named allocator.
 * The clusters are appended to alloc->a_clusters, raising a_cluster_count
 * by num_clusters. Returns B_TRUE if successful or B_FALSE if not. If the
 * return value is B_FALSE, no changes to memory configuration have been
 * made for any allocator.
 */
static inline boolean_t
poach_clusters(allocator_t *alloc, uint64_t num_clusters)
{
	ASSERT(alloc != NULL && num_clusters > 0);
	allocator_t **allocators = alloc->a_group->ag_allocators;
	unsigned int num_allocators = alloc->a_group->ag_num_allocators;

	uint64_t num_poachable = 0;
	for (int i = 0; i < num_allocators; i++) {
		if (allocators[i] == alloc)
			break;
		if (allocators[i]->a_fd >= 0)
			num_poachable += allocators[i]->a_cluster_count;
	}
	if (num_poachable < num_clusters)
		return (B_FALSE);

	for (int i = 0; i < num_allocators; i++) {
		allocator_t *poachee = allocators[i];
		if (poachee == alloc) {
			/* This should not be possible, given the precheck */
			errx(1, "poached clusters fewer than expected in %s; "
			    "%llu outstanding", __func__,
			    (u_longlong_t)num_clusters);
		}
		if (poachee->a_fd < 0 || poachee->a_cluster_count == 0)
			continue;
		uint64_t n_to_poach =
		    MIN(num_clusters, poachee->a_cluster_count);
		release_clusters(poachee, n_to_poach,
		    &alloc->a_clusters[alloc->a_cluster_count]);
		alloc->a_cluster_count += n_to_poach;
		if ((num_clusters -= n_to_poach) == 0)
			return (B_TRUE);
	}
	errx(1, "inconsistent memory accounting found in %s", __func__);
}

/*
 * Attempt to expand an allocator's writable memory region to include a
 * given page cluster. If the allocator group has sufficient unused memory,
 * this can be done directly. If not, we call poach_clusters() to try and
 * recover memory from lower priority allocators.
 *
 * If the attempt to obtain memory from another allocator fails, we lower
 * the allocator's a_first_disk_cluster and return B_FALSE. Retrying the
 * transaction should then succeed as a disk operation.
 *
 * If mprotect() fails with ENOMEM in a disk-backed group, we handle this
 * case in essentially the same way. Any clusters already obtained, whether
 * poached or made writable, stay with the allocator, and the memory/disk
 * split lands just above them. Evictions already performed are not undone.
 */
static inline boolean_t
try_make_writable(allocator_t *alloc, uint64_t cluster)
{
	VERIFY(cluster < alloc->a_first_disk_cluster);
	VERIFY(cluster >= alloc->a_cluster_count);

	allocator_group_t *group = alloc->a_group;
	uint64_t clusters_needed = cluster + 1 - alloc->a_cluster_count;
	uint64_t clusters_avail =
	    group->ag_clusters_max - group->ag_clusters_current;

	/*
	 * Poach before allocating, so that a poaching failure leaves every
	 * allocator unchanged.
	 */
	if (clusters_needed > clusters_avail) {
		uint64_t deficit = clusters_needed - clusters_avail;
		if (!poach_clusters(alloc, deficit)) {
			alloc->a_first_disk_cluster = alloc->a_cluster_count;
			return (B_FALSE);
		}
		clusters_needed -= deficit;
	}

	/* Allocate the remainder from the group's VM region. */
	VERIFY3U(clusters_needed, ==, cluster + 1 - alloc->a_cluster_count);
	for (uint64_t i = alloc->a_cluster_count; i <= cluster; i++) {
		alloc->a_clusters[i] = group->ag_base_addr +
		    group->ag_clusters_current * group->ag_cluster_size;
		int rc = mprotect(alloc->a_clusters[i], group->ag_cluster_size,
		    PROT_READ | PROT_WRITE);
		if (rc != 0 && errno == ENOMEM && alloc->a_fd >= 0) {
			warnx("out of memory: spilling to %s...",
			    (group->ag_dir != NULL) ? group->ag_dir : "disk");
			alloc->a_first_disk_cluster = alloc->a_cluster_count;
			return (B_FALSE);
		} else if (rc != 0) {
			err(1, "mprotect() failed");
		}
		group->ag_clusters_current++;
		alloc->a_cluster_count++;
	}
	return (B_TRUE);
}

allocator_stats_t
allocator_get_stats(allocator_t *alloc)
{
	VERIFY(alloc != NULL);
	allocator_group_t *group = alloc->a_group;
	allocator_stats_t stats = {
		.as_memory_used =
		    alloc->a_cluster_count * group->ag_cluster_size,
		.as_num_records = alloc->a_record_count,
		.as_record_size = alloc->a_record_size
	};
	return (stats);
}

/*
 * May be called with any allocator in the group. Frees the whole group.
 */
void
allocator_group_fini(allocator_t *alloc)
{
	VERIFY(alloc != NULL);
	allocator_group_t *group = alloc->a_group;
	for (int i = 0; i < group->ag_num_allocators; i++) {
		allocator_t *galloc = group->ag_allocators[i];
		if (galloc->a_fd >= 0) {
			if (close(galloc->a_fd) != 0)
				warn("unable to close allocator backing file");
		}
		free(galloc->a_clusters);
		free(galloc);
	}
	if (group->ag_dir != NULL)
		free(group->ag_dir);
	free(group->ag_allocators);
	if (group->ag_base_addr != NULL) {
		int rc = munmap(group->ag_base_addr,
		    group->ag_clusters_max * group->ag_cluster_size);
		if (rc != 0)
			warn("munmap() failed, ignoring...");
	}
	free(group);
}
