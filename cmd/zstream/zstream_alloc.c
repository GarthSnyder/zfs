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

#define REC_ON_DISK(alloc, rec) (REC_TO_ADDR(alloc, rec) >= \
	    (alloc)->a_max_memory);

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
 * If the backing file's filesystem does not support holes (unlikely), the
 * code is still correct. However, actual disk space consumption will be
 * higher.
 *
 * - The second feature is the use of PROT_NONE for virtual pages. You can't
 * do anything with these pages, so OSes largely ignore their existence
 * aside from maintaining the address map. They do not consume physical
 * memory, TLB entries, or swap space. Because of that, allocators can
 * request a preposterously large VM allocation up front, and they never
 * need to change their address space.
 *
 * As the allocator needs more pages to actually work with, it incrementally
 * changes their protection from PROT_NONE to PROT_READ | PROT_WRITE, at
 * which point they acquire swap reservations and the other normal trappings
 * of memory. If the memory budget is reduced, the trailing pages are
 * transferred to disk and their protection is reset to PROT_NONE, which
 * makes the kernel free them immediately.
 *
 * A more general point is that file I/O occurs only through reads and
 * writes to buffers. Only OS-level file descriptors are used, so the only
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

/*
 * Shrink-fit memory page allocations to those that are actually in use. We
 * "free" memory pages by setting them to PROT_NONE. However, we always keep
 * the original allocation of VM address space.
 */
static void
shrink_frontier(allocator_t *alloc)
{
	off_t last_byte_in_use = MIN(alloc->a_max_memory,
	    REC_TO_OFFSET(alloc, alloc->a_count)) - 1;
	off_t frontier_off = P2ROUNDUP(last_byte_in_use, alloc->a_pagesize);
	ssize_t length = alloc->a_vm_allocated - frontier_off;
	alloc->a_vm_frontier = OFFSET_TO_ADDR(alloc, frontier_off);
	if (mprotect(alloc->a_vm_frontier, length, PROT_NONE) != 0)
		err(1, "mprotect failed");
}

/*
 * Since memory and disk segments share offset addresses, we only need to do
 * one copy from memory to disk or vice versa to change the split point.
 * Note that because of memory allocation rounding, this function may be a
 * no-op even if new_size != current size.
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

/*
 * Memory pages starting at a_vm_frontier are initially PROT_NONE, so they
 * don't really exist and can't be written to or read. If a record we want
 * to access lies beyond the frontier, we need to move the frontier and mark
 * the intervening pages as PROT_READ | PROT_WRITE.
 *
 * The record number passed in must already have been checked to be sure it
 * goes in memory rather than on disk.
 */
static void
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
}

void
allocator_retrieve(allocator_t *alloc, record_ix_t record, void *buff)
{
	VERIFY(buff != NULL);
	if (REC_ON_DISK(alloc, record)) {
		if (alloc->a_fd < 0)
			errx(1, "no file for allocator record %llu", record);
		off_t loc = REC_TO_OFFSET(alloc, record);
		safe_pread(alloc->a_fd, buff, alloc->a_record_size, loc);
		alloc->a_io_ops_disk++;
	} else {
		reify_memory_for_record(alloc, record);
		memcpy(buff, REC_TO_ADDR(alloc, record), alloc->a_record_size);
		alloc->a_io_ops_mem++;
	}
}

void
allocator_store(allocator_t *alloc, record_ix_t record, const void *buff)
{
	VERIFY(buff != NULL);
	if (REC_ON_DISK(alloc, record)) {
		if (alloc->a_fd < 0)
			errx(1, "no file for allocator record %llu", record);
		off_t loc = REC_TO_OFFSET(alloc, record);
		safe_pwrite(alloc->a_fd, buff, alloc->a_record_size, loc);
		alloc->a_io_ops_disk++;
	} else {
		reify_memory_for_record(alloc, record);
		memcpy(REC_TO_ADDR(alloc, record), buff, alloc->a_record_size);
		alloc->a_io_ops_mem++;
	}
	alloc->a_count = MAX(alloc->a_count, record);
}

record_ix_t
allocator_append(allocator_t *alloc, const void *data) {
	record_ix_t rec = alloc->a_count;
	allocator_store(alloc, loc, data);
	return (loc);
}

record_ix_t
allocator_skip(allocator_t *alloc) {
	VERIFY(alloc != NULL);
	return (alloc->a_count++);
}

allocator_stats_t
allocator_get_stats(allocator_t *alloc) {
	VERIFY(alloc != NULL && stats != NULL);
	allocator_stats_t stats = {
		.as_io_ops_mem = alloc->a_io_ops_mem,
		.as_io_ops_disk = alloc->a_io_ops_disk,
		.as_mem_used = alloc->a_vm_frontier - alloc->a_base_addr,
		.as_num_records = alloc->a_count
	}
	if (alloc->a_fd >= 0) {
		off_t last_byte = REC_TO_OFFSET(alloc, alloc->a_count) +
		    alloc->a_record_size_rounded - 1;
		if (last_byte > alloc->a_max_memory) {
			stats.as_disk_used = last_byte - alloc->a_max_memory;
		}
	}
	return (stats);
}

void
allocator_destroy(allocator_t *alloc) {
	VERIFY(alloc != NULL);
	munmap(alloc->a_base_addr, alloc->a_vm_allocated);
	if (alloc->a_fd >= 0) {
		if (close(alloc->a_fd) != 0)
			warn("unable to close allocator backing file");
	}
	free(alloc);
}
