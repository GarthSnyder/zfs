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
 * Inputs to the record-size rounding calculation in allocator_init(). See
 * the discussion there for details. TARGET_GRANULARITY is an upper bound.
 */
#define	TARGET_GRANULARITY	(32 << 20)	/* 32MB */
#define	MAX_WASTE		0.5

/*
 * Granularity at which memory pages are converted from PROT_NONE to
 * PROT_READ | PROT_WRITE. If the system page size is larger, that
 * becomes the granularity.
 */
#define	FRONTIER_GRANULARITY	(8 << 20)	/* 8MB */

#define	REC_TO_OFFSET(alloc, rec) ((rec) * (alloc)->a_record_size_rounded)
#define	OFFSET_TO_ADDR(alloc, off) ((off) + (alloc)->a_base_addr)
#define	ADDR_TO_OFFSET(alloc, addr) ((addr) - (alloc)->a_base_addr)
#define	REC_TO_ADDR(alloc, rec) OFFSET_TO_ADDR(alloc, \
	    REC_TO_OFFSET(alloc, rec))

#define	RECORD_IS_ON_DISK(alloc, rec) (REC_TO_OFFSET(alloc, rec) >= \
	    (alloc)->a_max_memory)

/*
 * This implementation exploits two features common to most systems in
 * the UNIX lineage,
 *
 * - The first is support for write holes in filesystems. For a dual-backed
 *   allocator, the memory-resident portion of the data is treated as an
 *   overlay of the first part of the backing file. Memory and disk share
 *   the same offset addressing scheme for records: record 100 is always at
 *   100 * a_record_size_rounded, whether it's in memory or on disk.
 *
 *   The first part of the disk file hides underneath the overlay and is never
 *   written to. Ergo, it occupies no actual storage space. Since memory and
 *   disk have common addressing, they can be rebalanced with a single write
 *   when the memory budget changes.
 *
 *   If the backing file's filesystem does not support holes (unlikely but
 *   possible), the code is still correct. However, actual disk space
 *   consumption will be higher.
 *
 * - The second feature is the use of PROT_NONE for virtual pages. You can't
 *   do anything with these pages, so they are essentially free. They do not
 *   consume physical memory, TLB entries, or swap space. Because of that,
 *   allocators can request a large, contiguous VM allocation up front and
 *   never need to change their addressing scheme, even as memory use
 *   parameters change.
 *
 *   When the allocator needs more pages to work with, it incrementally
 *   changes their protection from PROT_NONE to PROT_READ | PROT_WRITE, at
 *   which point they acquire swap reservations and the other normal trappings
 *   of memory.
 *
 *   If the memory budget is reduced, the trailing pages are transferred to
 *   disk and then replaced with a fresh PROT_NONE anonymous mapping
 *   (MAP_FIXED). Remapping, unlike a bare mprotect(PROT_NONE), both returns
 *   the physical pages to the kernel immediately and guarantees that the
 *   region reads as zeros if it is later re-exposed.
 */

struct allocator {
	int		a_fd;			/* On-disk file descriptor */
	size_t		a_max_memory;		/* Current memory limit */
	size_t		a_record_size;		/* As specified by the client */

	uint64_t	a_count;		/* Number of records stored */
	void		*a_base_addr;		/* Start of memory segment */
	void		*a_writable_frontier;	/* Addr of 1st non-r/w byte */

	size_t		a_record_size_rounded;	/* Record-to-record offset */
	size_t		a_memory_granularity;	/* Memory/disk boundary chunk */
	size_t		a_frontier_granularity;	/* Memory reification chunk */
	size_t		a_vm_allocated;		/* Total VM space reserved */
};

allocator_t *
allocator_init(size_t record_size, size_t mem_size, const char *dir_path)
{
	int fd = -1;
	if (dir_path != NULL) {
		fd = safe_create_temp_file(dir_path);
	} else if (mem_size == 0) {
		errx(1, "allocator needs disk or memory backing");
	}

	ssize_t pagesize = (ssize_t)sysconf(_SC_PAGESIZE);
	if (pagesize < 0) {
		err(1, "unable to read system page size");
	}

	/*
	 * We want the memory-to-disk transition to occur at an address that
	 * is both a page boundary and a record boundary. That way, every
	 * record is either completely on disk or completely in memory.
	 *
	 * However, page sizes and record sizes can both vary, so we need
	 * some idea of what allocation granularity we're trying to achieve
	 * (TARGET_GRANULARITY). If the natural LCM of the record size and
	 * page size is larger than this value, we can start to round up
	 * record sizes, trading some storage efficiency for a lower LCM.
	 */
	size_t granularity, rsize_rounded;
	size_t align = 1;
	/*
	 * Waste (storage lost by rounding up record sizes) grows
	 * monotonically with increasing alignment multiple, so this
	 * calculation is guaranteed to terminate.
	 */
	while (B_TRUE) {
		rsize_rounded = P2ROUNDUP(record_size, align);
		size_t waste_bytes = rsize_rounded - record_size;
		double waste_pct = (double)waste_bytes / rsize_rounded;
		if (waste_pct > MAX_WASTE)
			errx(1, "unable to find an efficient rounding for "
			    "record_size = %llu, page_size = %llu",
			    (u_longlong_t)record_size, (u_longlong_t)pagesize);
		granularity = least_common_multiple(pagesize, rsize_rounded);
		if (granularity <= TARGET_GRANULARITY)
			break;
		align = align << 1;
	}

	/*
	 * Allocate a full-size chunk of PROT_NONE address space.
	 */
	void *base = NULL;
	size_t vm_allocation = ROUND_UP(mem_size, granularity);
	if (vm_allocation > 0) {
		base = mmap(NULL, vm_allocation, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			errx(1, "mmap failed in %s", __func__);
		}
	}

	allocator_t *alloc = safe_malloc(sizeof (allocator_t));
	*alloc = (allocator_t) {
		.a_fd = fd,
		.a_max_memory = vm_allocation,
		.a_record_size = record_size,
		.a_base_addr = base,
		.a_writable_frontier = base,
		.a_record_size_rounded = rsize_rounded,
		.a_memory_granularity = granularity,
		.a_frontier_granularity = MAX(pagesize, FRONTIER_GRANULARITY),
		.a_vm_allocated = vm_allocation,
	};
	return (alloc);
}

/*
 * Reify: to make something abstract more concrete or real. Here it means to
 * convert an address range we already own into writable pages.
 *
 * a_max_memory locates the boundary between memory and disk storage. The
 * memory region is further subdivided at a_writable_frontier, which points
 * to the first byte of the current PROT_NONE region.
 *
 * PROT_NONE pages can't be read or written to. If a byte we want to access
 * lies beyond the frontier, we need to move the frontier and mark the
 * intervening pages as PROT_READ | PROT_WRITE. The frontier advances in
 * multiples of a_frontier_granularity to keep mprotect() calls infrequent.
 *
 * The end_offset parameter and the a_writable_frontier pointer are both
 * "+1" markers. That is, everything below a_writable_fronter is already
 * writable, and reify_memory_up_to() reifies up to but not including the
 * end_offset. Because of this accounting convention, a_writable_frontier
 * always points to the first byte of an unreified memory page.
 */
static void
reify_memory_up_to(allocator_t *alloc, off_t end_offset)
{
	void *end_addr = OFFSET_TO_ADDR(alloc, end_offset);
	if (end_addr <= alloc->a_writable_frontier)
		return;
	size_t needed = end_addr - alloc->a_writable_frontier;
	size_t avail = alloc->a_max_memory -
	    ADDR_TO_OFFSET(alloc, alloc->a_writable_frontier);
	if (needed > avail)
		errx(1, "allocator out of memory");
	size_t length =
	    MIN(P2ROUNDUP(needed, alloc->a_frontier_granularity), avail);
	int rc = mprotect(alloc->a_writable_frontier, length,
	    PROT_READ | PROT_WRITE);
	if (rc != 0)
		err(1, "mprotect failed");
	alloc->a_writable_frontier += length;
}

/*
 * Free at least delta_bytes of memory, relative to the amount of memory
 * actually in use (not the allocator's theoretical memory limit). Since
 * memory and disk segments share offset addresses, we only need to do one
 * copy from memory to disk to change the split point.
 *
 * Only bytes below the writable frontier are written out. Bytes between the
 * frontier and the old memory budget were never written and are logically
 * zero. The corresponding file region has never been written, so it already
 * reads back as zeros.
 *
 * Returns the amount of memory actually freed.
 */
size_t
allocator_trim_memory(allocator_t *alloc, size_t delta_bytes)
{
	ASSERT(alloc != NULL);
	if (alloc->a_base_addr == NULL)
		return (0);
	ssize_t bytes_used = ADDR_TO_OFFSET(alloc, alloc->a_writable_frontier);
	ssize_t new_max = ROUND_UP(MAX(bytes_used - (ssize_t)delta_bytes, 0),
	    alloc->a_memory_granularity);
	/* Always free at least one granule */
	if (new_max >= bytes_used && bytes_used > 0) {
		ASSERT3U(new_max, >=, alloc->a_memory_granularity);
		new_max -= alloc->a_memory_granularity;
	}

	void *eject_start = alloc->a_base_addr + new_max;
	void *eject_end = alloc->a_writable_frontier;
	size_t freed_bytes = MAX(0, eject_end - eject_start);
	if (freed_bytes > 0) {
		if (alloc->a_fd < 0) {
			errx(1, "no disk backing for allocator, so "
			    "%s would lose data", __func__);
		}
		safe_pwrite(alloc->a_fd, eject_start, freed_bytes, new_max);
		void *ret = mmap(eject_start, freed_bytes, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (ret == MAP_FAILED)
			err(1, "mmap (frontier shrink) failed");
		alloc->a_writable_frontier = eject_start;
	}

	alloc->a_max_memory = new_max;
	return (freed_bytes);
}

void
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buff)
{
	VERIFY(buff != NULL);
	if (RECORD_IS_ON_DISK(alloc, record)) {
		if (alloc->a_fd < 0)
			errx(1, "no disk file for allocator record %llu",
			    (u_longlong_t)record);
		off_t loc = REC_TO_OFFSET(alloc, record);
		safe_pread_zero(alloc->a_fd, buff, alloc->a_record_size, loc);
	} else {
		reify_memory_up_to(alloc, REC_TO_OFFSET(alloc, record) +
		    alloc->a_record_size_rounded);
		memcpy(buff, REC_TO_ADDR(alloc, record), alloc->a_record_size);
	}
}

void
allocator_store(allocator_t *alloc, record_ix_t record, const void *buff)
{
	VERIFY(buff != NULL);
	if (RECORD_IS_ON_DISK(alloc, record)) {
		if (alloc->a_fd < 0)
			errx(1, "no disk file for allocator record %llu",
			    (u_longlong_t)record);
		off_t loc = REC_TO_OFFSET(alloc, record);
		safe_pwrite(alloc->a_fd, buff, alloc->a_record_size, loc);
	} else {
		reify_memory_up_to(alloc, REC_TO_OFFSET(alloc, record) +
		    alloc->a_record_size_rounded);
		memcpy(REC_TO_ADDR(alloc, record), buff, alloc->a_record_size);
	}
	/* a_count is one past the highest record known to have been written */
	alloc->a_count = MAX(alloc->a_count, record + 1);
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
 * past last record known to have been written.
 */
record_ix_t
allocator_skip(allocator_t *alloc)
{
	void *buff = safe_calloc(alloc->a_record_size);
	record_ix_t ix = allocator_append(alloc, buff);
	free(buff);
	return (ix);
}

size_t
allocator_memory_used(allocator_t *alloc)
{
	return ((alloc->a_base_addr == NULL) ? 0 :
	    (alloc->a_writable_frontier - alloc->a_base_addr));
}

void
allocator_destroy(allocator_t *alloc)
{
	VERIFY(alloc != NULL);
	if (alloc->a_base_addr != NULL)
		munmap(alloc->a_base_addr, alloc->a_vm_allocated);
	if (alloc->a_fd >= 0) {
		if (close(alloc->a_fd) != 0)
			warn("unable to close allocator backing file");
	}
	free(alloc);
}
