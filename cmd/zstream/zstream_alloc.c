// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#include <err.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "zstream_alloc.h"
#include "zstream_util.h"

#ifndef MAP_ANONYMOUS
#	ifdef MAP_ANON
#		define MAP_ANONYMOUS MAP_ANON
#	else
#		error "Neither MAP_ANONYMOUS nor MAP_ANON is defined"
#	endif
#endif

#ifndef MAP_NORESERVE
#	define MAP_NORESERVE 0
#endif

/*
 * Record sizes are rounded up to this power of 2 internally.
 */
#define	RECORD_ALIGN 8

/*
 * Round to ensure memory/disk transition is at both a record boundary and a
 * page boundary.
 */
#define MEM_ROUNDUP(size, pagesize, recsize) \
	    P2ROUNDUP(size, lcm(pagesize, recsize))

#define	REC_TO_OFFSET(alloc, rec) ((rec) * (alloc)->a_record_size_rounded)
#define OFFSET_TO_REC(alloc, off) ((off) / (alloc)->a_record_size_rounded)

#define OFFSET_TO_ADDR(alloc, off) ((off) + (alloc)->a_base_addr)
#define ADDR_TO_OFFSET(alloc, addr) ((addr) - (alloc)->a_base_addr)

#define	REC_TO_ADDR(alloc, rec) OFFSET_TO_ADDR(alloc, \
	    REC_TO_OFFSET(alloc, rec))
#define	ADDR_TO_REC(alloc, addr) OFFSET_TO_REC(alloc, \
	    ADDR_TO_OFFSET(alloc, addr))

/*
 * This implementation relies on two OS features that are common to most
 * systems in the UNIX lineage, including Linux and FreeBSD.
 *
 * - The first is support for write holes in filesystems. For a dual-backed
 * allocator, the memory-resident portion of the data is treated as a sort
 * of overlay of the first part of the backing file. Memory and disk share
 * the same offset addressing scheme for records: record 100 is always at
 * 100 * record_size, whether it's in memory or on disk.
 *
 * The first part of the disk file hides underneath the overlay and is never
 * written to. Ergo, it occupies no actual storage space. Since memory and
 * disk have common addressing, they can be rebalanced with a single read or
 * write when the memory budget changes.
 *
 * If the backing file's filesystem does not support holes (unlikely!), the
 * code is still correct. However, actual disk space consumption will be
 * higher.
 *
 * - The second feature is the use of PROT_NONE for virtual pages. PROT_NONE
 * pages are neither readable nor writable nor executable, so OSes largely
 * ignore their existence outside of address-map maintenance. They do not
 * consume physical memory, TLB entries, or swap space. Because of that,
 * allocators can request a preposterously ambitious VM allocation up front,
 * and they never need to change their address space.
 *
 * As the allocator needs more pages to actually work with, it incrementally
 * changes their protection from PROT_NONE to PROT_READ | PROT_WRITE, at
 * which point they acquire swap reservations and the other normal trappings
 * of memory. If the memory budget is reduced, the trailing pages are
 * transferred to disk and their protection is reset to PROT_NONE. That
 * makes the kernel free them immediately.
 *
 * A more general point is that file I/O occurs only through reads and
 * writes to buffers. OS-level file descriptors are used, so the only
 * cacheing is that of the filesystem page cache.
 */

struct allocator {

	size_t		a_record_size;
	size_t		a_record_size_rounded;	/* Multiple of 8 */
	size_t		a_max_memory;
	int		a_fd;			/* On-disk file descriptor */

	uint64_t	a_count;		/* Number of records stored */
	void		*a_base_addr;		/* Start of memory segment */
	size_t		a_pagesize;		/* System page size */
	size_t		a_vm_allocated;		/* Total VM space reserved */
	void		*a_vm_frontier;		/* Offset of 1st non-r/w byte */
	size_t		a_frontier_granularity;	/* Frontier expansion, ~1MB */

	uint64_t	a_io_ops_mem;		/* Number of reads and writes */
	uint64_t	a_io_ops_disk;
};

typedef struct {
	boolean_t	l_on_disk;
	union {
		off_t	l_off;		/* Valid when l_on_disk */
		void	*l_addr;	/* Valid otherwise */
	};
} record_location_t;

/*
 * Least common multiple - Euclid's algorithm
 */
size_t lcm(size_t a, size_t b)
{
	size_t a_orig = a;
	size_t b_orig = b;

	while (b != 0) {
		size_t r = a % b;
		a = b;
		b = r;
	}
	return (a_orig / a) * b_orig;
}

/*
 * We always allocate a large chunk of VM address space, even if we're
 * starting with a max_memory of zero. It's free because it's PROT_NONE.
 * We'll gradually convert this address space into real pages as records are
 * added.
 */
allocator_t *
allocator_init(size_t record_size, size_t mem_size, int fd)
{
	VERIFY3U(record_size, >, 0);
	if (fd < 0 && mem_size <= 0) {
		errx(1, "allocator_init requires either a file or max_memory");
	}

	size_t rsize_rounded = P2ROUNDUP(record_size, RECORD_ALIGN);
	ssize_t pagesize = (ssize_t)sysconf(_SC_PAGESIZE);
	ssize_t pages = (ssize_t)sysconf(_SC_PHYS_PAGES);

	if (pagesize < 0 || pages < 0) {
		return NULL;
	}

	size_t vm_allocation = 4 * pagesize * pages;
	size_t granularity = 1 << 20;  /* 1MB */

	void *base = mmap(NULL, vm_allocation, PROT_NONE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		return NULL;
	}

	size_t mem = MEM_ROUNDUP(mem_size, pagesize, rsize_rounded);
	allocator_t *alloc = safe_calloc(sizeof (allocator_t));
	*alloc = (allocator_t){
		.a_record_size = record_size,
		.a_record_size_rounded = rsize_rounded,
		.a_max_memory = mem,
		.a_fd = fd,
		.a_base_addr = base,
		.a_pagesize = pagesize,
		.a_vm_allocated = vm_allocation,
		.a_vm_frontier = base,
		.a_frontier_granularity = granularity
	};
	return (alloc);
}

static void
expand_max_memory(allocator_t *alloc, size_t new_size)
{
	off_t first = alloc->a_max_memory;
	off_t last = MIN(REC_TO_OFFSET(alloc, alloc->a_count), new_size) - 1;
	ssize_t length = last - first;
	if (length > 0) {
		void *start_addr = OFFSET_TO_ADDR(alloc, first);
		safe_pread(alloc->a_fd, start_addr, length, first);
		punch_hole(alloc->a_fd, first, length);
	}
	alloc->a_max_memory = new_size;
}

static void
trim_max_memory(allocator_t *alloc, size_t new_size)
{
	off_t first_off = new_size;
	off_t last_off = MIN(alloc->a_max_memory,
	    REC_TO_OFFSET(alloc, alloc->a_count)) - 1;
	ssize_t length = last_off - first_off;
	if (length > 0) {
		void *start_addr = OFFSET_TO_ADDR(first_off);
		safe_pwrite(alloc->a_fd, start_addr, length, first_off);
	}
	alloc->a_max_memory = new_size;
	shrink_frontier(alloc);
}

/*
 * Since memory and disk segments share offset addresses, we only need to do
 * one copy from memory to disk or vice versa to change the split point.
 * Note that because of memory allocation rounding, this may be a no-op even
 * if new_size != current size.
 */
void
allocator_set_max_memory(allocator_t *alloc, size_t new_size)
{
	size_t new_size = MEM_ROUNDUP(new_size, alloc->a_pagesize,
	    alloc->a_record_size_rounded);
	off_t last_off_used = REC_TO_OFFSET(alloc, alloc->a_count) - 1;
	if (alloc->a_fd < 0 && new_size < last_off_used + 1)
		errx(1, "resize of allocator would lose data");
	if (alloc->a_fd >= 0 && new_size > alloc->a_max_memory) {
		off_t first_byte = alloc->a_max_memory;
		off_t last_byte = MIN(last_off_used, new_size - 1);
		ssize_t len = last_byte - first_byte;
		if (len > 0) {
			void *start_addr = OFFSET_TO_ADDR(alloc, first_byte);
			safe_pread(alloc->a_fd, start_addr, len, first_byte);
			punch_hole(alloc->a_fd, first_byte, len);
		}
	} else if (alloc->a_fd >= 0 && new_size < alloc->a_max_memory) {
		off_t first_byte = new_size;
		off_t last_byte = MIN(last_off_used, alloc->a_max_memory - 1);
		ssize_t len = last_byte - first_byte;
		if (len > 0) {
			void *start_addr = OFFSET_TO_ADDR(alloc, first_byte);
			safe_pwrite(alloc->a_fd, start_addr, len, first_byte);
			shrink_frontier(alloc);
		}
	}
	alloc->a_max_memory = new_size;
}

static void
free_memory(allocator_t *alloc) {
	if (alloc->a_base_addr) {
		munmap(alloc->a_base_addr, alloc->a_max_memory);
	}
	alloc->a_base_addr = NULL;
}

void
allocator_get_stats(allocator_t *alloc, allocator_stats_t *stats) {
	assert(alloc && stats);
	stats->as_num_ops = alloc->a_io_ops;
	stats->as_num_records = alloc->a_count;
	stats->as_mem_used = alloc->a_using_disk ?
		0 : (alloc->a_count * alloc->a_record_size);
}

record_ix_t
allocator_append(allocator_t *alloc, const void *data) {
	return allocator_store(alloc, alloc->a_count, data);
}

record_ix_t
allocator_skip(allocator_t *alloc) {
	VERIFY(alloc != NULL);
	return (alloc->a_count++);
}

/*
 * Memory pages beyond a_vm_frontier are initially PROT_NONE, so they don't
 * really exist and can't be written to or read. If a record we want to
 * access lies beyond the frontier, we need to move the frontier and mark
 * the intervening pages as PROT_READ | PROT_WRITE.
 *
 * The record number passed in must already have been checked to be sure it
 * goes in memory rather than on disk.
 */
static void *
reify_memory_for_record(allocator_t *alloc, record_ix_t record)
{
	void *last_byte = REC_TO_ADDR(alloc, record) +
	    alloc->a_record_size_rounded - 1;
	while (last_byte >= alloc->a_vm_frontier) {
		void *new_frontier = alloc->a_vm_frontier
		    + alloc->a_frontier_granularity;
		int ret = mprotect(alloc->a_vm_frontier,
		    new_frontier - alloc->a_vm_frontier,
		    PROT_READ | PROT_WRITE);
		if (ret != 0)
			err(1, "mprotect failed");
		alloc->a_vm_frontier = new_frontier;
	}
	return (first_byte);
}

static void
shrink_frontier(allocator_t *alloc)
{
	off_t last_byte_in_use = MIN(alloc->a_max_memory,
	    REC_TO_OFFSET(alloc, alloc->a_count)) - 1;
	void *last_addr = OFFSET_TO_ADDR(alloc, last_byte_in_use);
	void *new_frontier = P2ROUNDUP(last_addr, alloc->a_pagesize);
	off_t frontier_off = ADDR_TO_OFFSET(alloc, new_frontier);
	ssize_t length = alloc->a_vm_allocated - frontier_off;
	if (mprotect(new_frontier, length, PROT_NONE) != 0)
		err(1, "mprotect failed");
	alloc->a_vm_frontier = new_frontier;
}

static inline record_location_t
locate_record(allocator_t *alloc, record_ix_t record)
{
	VERIFY(alloc != NULL && record >= 0);
	if (alloc->a_fd >= 0 && record >= alloc->a_first_on_disk) {
		alloc->a_io_ops_disk++;
		off_t offset = record * alloc->a_record_size_rounded;
		record_location_t loc = { B_TRUE, .l_off = offset };
		return (loc);
	} else if (record < alloc->a_first_on_disk) {
		alloc->a_io_ops_mem++;
		void *addr = alloc->a_base_addr +
		    record * alloc->a_record_size_rounded;
		void *addr = reify_memory_for_record(alloc, record);
		record_location_t loc = { B_FALSE, .l_addr = addr };
		return (loc);
	} else {
		errx(1, "no allocator backing file for record %llu", record);
	}
}

void
allocator_store(allocator_t *alloc, record_ix_t record, const void *buff)
{
	VERIFY(buff != NULL);
	record_location_t loc = locate_record(alloc, record);
	if (loc.l_on_disk)
		safe_pwrite(alloc->a_fd, buff, alloc->a_record_size, loc.l_off);
	else
		memcpy(loc.l_addr, buff, alloc->a_record_size);
	alloc->a_count = MAX(alloc->a_count, record);
}

void
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buff)
{
	VERIFY(buff != NULL);
	record_location_t loc = locate_record(alloc, record);
	if (loc.l_on_disk)
		safe_pread(alloc->a_fd, buff, alloc->a_record_size, loc.l_off);
	else
		memcpy(buff, loc.l_addr, alloc->a_record_size);
}

void
allocator_destroy(allocator_t *alloc) {
	assert(alloc);
	if (!alloc->a_using_disk) {
		free_memory(alloc);
	}
	if (alloc->a_file) {
		fclose(alloc->a_file);
	}
	free(alloc);
}
