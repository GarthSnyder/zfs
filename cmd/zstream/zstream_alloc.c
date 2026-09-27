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
 * This implementation exploits two features common to most systems in the
 * UNIX lineage,
 *
 * - The first is support for write holes in filesystems. For a dual-backed
 *   allocator, the memory-resident portion of the data is treated as an
 *   overlay of the first part of the backing file. Memory and disk share the
 *   same offset addressing scheme for records: record 100 is always at 100 *
 *   a_record_size_rounded, whether it's in memory or on disk.
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
 * - The second feature is the use of PROT_NONE for virtual pages. You can't
 *   do anything with these pages, so they are essentially free. They do not
 *   consume physical memory, TLB entries, or swap space. Because of that,
 *   allocators can request a large, contiguous VM allocation up front and
 *   never need to change their addressing scheme, even as memory use
 *   parameters change.
 *
 *   When the allocator needs more pages to work with, it incrementally
 *   changes their protection from PROT_NONE to PROT_READ | PROT_WRITE, at
 *   which point they acquire swap reservations and are charged against
 *   the RSS.
 *
 * If the memory budget is reduced, trailing pages are transferred to
 * disk and then replaced with a fresh PROT_NONE anonymous mapping
 * (MAP_FIXED). Remapping, unlike a bare mprotect(PROT_NONE), both returns
 * the physical pages to the kernel and guarantees that the region reads
 * as zeros if it is later re-exposed.
 */

/*
 * Inputs to the record-size rounding calculation in allocator_init(). See
 * the discussion there for details. TARGET_GRANULARITY is an upper bound.
 */
#define	MAX_PAGES_PER_MEMORY_UNIT	33	    /* ~128K with 4K pages */
#define TARGET_FRONTIER_MOVEMENT	(4 << 20)   /* 4MB */

/*
 * Granularity at which memory pages are converted from PROT_NONE to
 * PROT_READ | PROT_WRITE, expressed as a divisor relative to the calculated
 * memory granularity. The calculated value will be rounded up to the system
 * page size. There is no guarantee that this value will be smaller than the
 * memory granularity, although it virtually always will be.
 */
#define	FRONTIER_DIVISOR	8		/* ~4MB */

#define	OFFSET_TO_ADDR(alloc, off) ((off) + (alloc)->a_base_addr)
#define	ADDR_TO_OFFSET(alloc, addr) ((addr) - (alloc)->a_base_addr)
#define	REC_TO_ADDR(alloc, rec) OFFSET_TO_ADDR(alloc, \
	    record_offset(alloc, rec))

#define	OFFSET_ON_DISK(alloc, off) ((off) >= (alloc)->a_max_memory)

/*
 * Allocators that use memory have two different allocation granularities
 * that are conceptually separate but that sometimes interact.
 *
 * "memory" granularity is the increment by which a_max_memory, the boundary
 * between memory storage and disk storage, moves. This transition must
 * always fall on an address that's both a page boundary and a record
 * boundary. That way, every record is either completely on disk or
 * completely in memory.
 *
 * The in-memory region is further subdivided at a_writable_frontier, which
 * points to the first byte of unwritable (PROT_NONE) memory. If a memory
 * byte we want to access lies beyond the frontier, we need to move the
 * frontier and mark the intervening pages as PROT_READ | PROT_WRITE. The
 * frontier advances in multiples of a_granularity.frontier to keep
 * mprotect() calls infrequent.
 *
 * a_granularity.memory is a "hard" value that's always enforced.
 * a_granularity.frontier is a vaguer "how much memory do you want to
 * allocate at once?" guideline. Conceptually, the frontier granularity is
 * finer than the memory granularity. But both are chosen with an eye to the
 * system page size, and in odd cases, the frontier granularity may be as
 * large as the memory granularity. No matter; the code is designed to
 * handle two arbitrary (but page-aligned) values and will do the right
 * thing.
 */

typedef struct {
	size_t		memory;			/* Memory/disk boundary */
	size_t		frontier;		/* Writable frontier w/in mem */
	size_t		records_per_mem_unit;	/* # of records that fit */
} granularity_t;

struct allocator {
	int		a_fd;			/* On-disk file descriptor */
	size_t		a_max_memory;		/* Current memory limit */
	size_t		a_record_size;		/* As specified by the client */

	uint64_t	a_count;		/* Highest index stored +1 */
	void		*a_base_addr;		/* Start of memory segment */
	void		*a_writable_frontier;	/* Addr of 1st non-r/w byte */

	granularity_t	a_granularity;
	size_t		a_vm_allocated;		/* Total VM space reserved */
};

static inline off_t
record_offset(allocator_t *alloc, record_ix_t rec)
{
	granularity_t *g = &alloc->a_granularity;
	uint64_t granule = rec / g->records_per_mem_unit;
	uint64_t granule_start = granule * g->memory;
	uint64_t record_in_granule = rec - granule * g->records_per_mem_unit;
	return (granule_start + record_in_granule * alloc->a_record_size);
}

/*
 * Here we determine both a memory-increment granularity (that is, the
 * granularity at which the memory/disk boundary can move) and a
 * frontier-increment granularity. Page sizes and record sizes can both
 * vary, so these values have to be calculated rather than fixed.
 *
 * Each unit of memory granularity is N pages of memory with M records
 * inside it. Records are stored back to back with no gaps. However, N *
 * pagesize isn't necessarily divisible by a_record_size, so the trailing
 * bytes of an allocation unit may be empty waste space.
 *
 * We check each possible value of N up to MAX_PAGES_PER_MEMORY_UNIT and
 * select the smallest N that achieves the least waste. A maximum N of 33
 * guarantees less than 5% waste as long as the record size is smaller than
 * the page size. (Zero waste is typical for small struct payloads.)
 *
 * The frontier granularity is TARGET_FRONTIER_MOVEMENT rounded up to an
 * integral number of pages. It's generally larger than the memory increment
 * granularity, but the code assumes no particular relationship between
 * them. Frontier allocations are always clipped to a_max_memory.
 */
static granularity_t
calc_granularities(size_t record_size)
{
	ssize_t pagesize = (ssize_t)sysconf(_SC_PAGESIZE);
	if (pagesize < 0) {
		err(1, "unable to read system page size");
	}
	granularity_t g;
	size_t unit_size = 0;
	double least_waste = 1.0;
	for (int i = 1; i <= MAX_PAGES_PER_MEMORY_UNIT; i++) {
		unit_size += pagesize;
		size_t waste = unit_size % record_size;
		if (waste == 0) {
			g.memory = unit_size;
			break;
		}
		double waste_pct = (double)waste / unit_size;
		if (waste_pct < least_waste) {
			least_waste = waste_pct;
			g.memory = unit_size;
		}
	}
	g.records_per_mem_unit = g.memory / record_size;
	g.frontier = P2ROUNDUP(TARGET_FRONTIER_MOVEMENT, pagesize);
	return (g);
}

allocator_t *
allocator_init(size_t record_size, size_t mem_size, const char *dir_path)
{
	int fd = -1;
	if (dir_path != NULL) {
		fd = safe_create_temp_file(dir_path);
	} else if (mem_size == 0) {
		errx(1, "allocator_t needs either disk or memory backing");
	}
	granularity_t granularity = calc_granularities(record_size);
	/*
	 * Allocate a full-size region of PROT_NONE address space.
	 */
	void *base = NULL;
	size_t vm_allocation = ROUND_UP(mem_size, granularity.memory);
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
		.a_granularity = granularity,
		.a_vm_allocated = vm_allocation,
	};
	return (alloc);
}

/*
 * Reify: to make something abstract more concrete or real. Here it means to
 * convert an address range we already own into writable pages. That is, we
 * re-protect it from PROT_NONE to PROT_READ | PROT_WRITE.
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
		/*
		 * Should never happen in practice because on-disk records
		 * go down a separate path and don't call this function.
		 */
		errx(1, "allocator out of memory");
	size_t length =
	    MIN(ROUND_UP(needed, alloc->a_granularity.frontier), avail);
	int rc = mprotect(alloc->a_writable_frontier, length,
	    PROT_READ | PROT_WRITE);
	if (rc != 0)
		err(1, "mprotect failed");
	alloc->a_writable_frontier += length;
}

void
allocator_store(allocator_t *alloc, record_ix_t record, const void *buff)
{
	VERIFY(buff != NULL);
	off_t off = record_offset(alloc, record);
	if (OFFSET_ON_DISK(alloc, off)) {
		if (alloc->a_fd < 0)
			errx(1, "no disk file for allocator record %llu "
			    "(write)", (u_longlong_t)record);
		safe_pwrite(alloc->a_fd, buff, alloc->a_record_size, off);
	} else {
		reify_memory_up_to(alloc, off + alloc->a_record_size);
		memcpy(OFFSET_TO_ADDR(alloc, off), buff, alloc->a_record_size);
	}
	/* a_count is one past the highest record known to have been written */
	alloc->a_count = MAX(alloc->a_count, record + 1);
}

void
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buff)
{
	VERIFY(buff != NULL);
	off_t off = record_offset(alloc, record);
	if (OFFSET_ON_DISK(alloc, off)) {
		if (alloc->a_fd < 0)
			errx(1, "no disk file for allocator record %llu (read)",
			    (u_longlong_t)record);
		safe_pread_zero(alloc->a_fd, buff, alloc->a_record_size, off);
	} else {
		void *first = OFFSET_TO_ADDR(alloc, off);
		void *last_plus_one = first + alloc->a_record_size;
		/*
		 * Unlike a_max_memory, a_writable_frontier may fall in the
		 * middle of a record. But if the entire record is not
		 * beneath a_writable_frontier, it cannot have ever been
		 * written. So, we can safely "read" it back as zeros.
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
 * past last record known to have been written.
 */
record_ix_t
allocator_skip(allocator_t *alloc)
{
	ASSERT(alloc->a_record_size > 0);
	uint8_t buff[alloc->a_record_size];
	memset(buff, 0, sizeof (buff));
	record_ix_t ix = allocator_append(alloc, buff);
	return (ix);
}

size_t
allocator_memory_used(allocator_t *alloc)
{
	return ((alloc->a_base_addr == NULL) ? 0 :
	    (alloc->a_writable_frontier - alloc->a_base_addr));
}

/*
 * Free at least delta_bytes of memory, relative to a_writable_frontier;
 * that is, the amount of memory actually in use rather than the allocator's
 * theoretical memory limit as found in a_max_memory.
 *
 * Since memory and disk segments share offset addresses, we only need to
 * perform one copy from memory to disk to change the split point. 9 Only
 * bytes below the writable frontier are written out. Bytes between the
 * frontier and the old memory budget were never written and are logically
 * zero. The corresponding file region has never been written, so it already
 * reads back as zeros.
 *
 * Since we are limiting future memory use as well as current use, we need
 * to lower a_max_memory, and since we're doing that, we need to calculate
 * in terms of memory granularity rather than frontier granularity.
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
	    alloc->a_granularity.memory);
	/* Always free at least one granule */
	if (new_max >= bytes_used && bytes_used > 0) {
		new_max -= alloc->a_granularity.memory;
		ASSERT3U(new_max, >=, 0);
	}

	void *eject_start = alloc->a_base_addr + new_max;
	void *eject_end = alloc->a_writable_frontier;
	size_t bytes_to_free = MAX(0, eject_end - eject_start);
	if (bytes_to_free > 0) {
		if (alloc->a_fd < 0)
			errx(1, "no disk backing for allocator, so "
			    "%s would lose data", __func__);
		safe_pwrite(alloc->a_fd, eject_start, bytes_to_free, new_max);
		void *ret = mmap(eject_start, bytes_to_free, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (ret == MAP_FAILED)
			err(1, "mmap (frontier shrink) failed");
		alloc->a_writable_frontier = eject_start;
	}

	alloc->a_max_memory = new_max;
	return (bytes_to_free);
}

void
allocator_fini(allocator_t *alloc)
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
