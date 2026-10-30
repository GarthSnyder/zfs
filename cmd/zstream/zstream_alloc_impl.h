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
 * This is the internal header for allocator_t. It holds the struct
 * definitions that back the implementation but that shouldn't be part of
 * the public API in zstream_alloc.h. Conceptually it is part of
 * zstream_alloc.c; it exists only so that selftests can inspect group
 * state.
 */

#ifndef _ZSTREAM_ALLOC_IMPL_H
#define	_ZSTREAM_ALLOC_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "zstream_alloc.h"

/*
 * All allocators are members of an allocator group, which can be a group of 1.
 * Group members share memory accounting.
 *
 * The group's page cluster size is the granularity of memory management for
 * all the group's allocators. It is a whole number of pages: by default,
 * 1MB if the system page size is not larger. An allocator's memory-to-disk
 * transition always falls at a page cluster boundary.
 */
typedef struct {
	uint64_t	ag_clusters_max;	/* Total allowed for group */
	uint64_t	ag_clusters_current;	/* Currently used by group */
	size_t		ag_cluster_size;	/* Allocation unit */
	char		*ag_dir;		/* Spill dir, for error msgs */
	unsigned int	ag_num_allocators;
	allocator_t	**ag_allocators;
} allocator_group_t;

/*
 * Invariants:
 *
 *   a_cluster_count <= a_first_disk_cluster <= a_group->ag_clusters_max
 *   a_group->ag_clusters_current <= a_group->ag_clusters_max
 *   a_group->ag_clusters_current = sum_over_group(a_cluster_count)
 *   a_first_disk_cluster decreases monotonically (if at all)
 *
 * Given that you can read and write anywhere in the index space,
 * a_record_count is a bit vaguely defined. It's maintained to give
 * allocator_append() and allocator_skip() some reasonable definition of
 * "the next index." Functionally, it's 1 plus the highest index that was
 * ever written to or returned to the client. Reads don't affect it.
 *
 * a_clusters is an array of pointers to page-cluster-sized blocks of memory.
 * The maximum memory budget is fixed at init time, so we know exactly how
 * large to make that array.
 *
 * In concept, an allocator_t splits storage between a memory portion and
 * a disk portion. However, there's a liminal third region between allocated
 * memory and disk that is nominally part of the memory region but for which
 * no memory has yet been allocated and where no records are stored. This
 * region is delineated by a_cluster_count on the low end (which marks the
 * upper end of memory allocations) and a_first_disk_cluster on the high end
 * (which marks the start of the disk region).
 *
 * Writes into the liminal region cause additional allocations of memory.
 * Those must be zero-filled on allocation because never-written records
 * must read as zeros.
 */
struct allocator {
	int		   a_fd;		  /* On-disk file descriptor */
	size_t		   a_record_size;	  /* Client-specified */
	uint64_t	   a_records_per_cluster; /* Constant (minimize divs) */
	uint64_t	   a_first_disk_cluster;  /* Memory/disk split point */
	uint64_t	   a_record_count;	  /* See comments above */
	uint64_t	   a_cluster_count;	  /* Page clusters in use */
	uint8_t		   **a_clusters;	  /* Array of cluster ptrs */
	allocator_group_t  *a_group;		  /* Back-pointer to group */
};

#ifdef __cplusplus
}
#endif

#endif /* _ZSTREAM_ALLOC_IMPL_H */
