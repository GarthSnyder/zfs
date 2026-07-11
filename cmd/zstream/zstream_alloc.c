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
	record_ix_t	a_first_on_disk;	/* Split point, LCM-aligned */

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
allocator_init(size_t record_size, size_t max_memory, int fd)
{
	VERIFY3U(record_size, >, 0);
	if (fd < 0 && max_memory <= 0) {
		errx(1, "allocator_init requires either a file or max_memory");
	}

	size_t rsize_rounded = P2ROUNDUP(record_size, RECORD_ALIGN);
	size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
	size_t pages = (size_t)sysconf(_SC_PHYS_PAGES);

	if (pagesize < 0 || pages < 0) {
		return NULL;
	}

	size_t vm_allocation = 4 * pagesize * pages;
	size_t granularity = P2ROUNDUP((size_t) 2 << 20, pagesize);

	void *base = mmap(NULL, vm_allocation, PROT_NONE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		return NULL;
	}

	size_t mem = P2ROUNDUP(max_memory, lcm(pagesize, rsize_rounded));
	record_ix_t first_on_disk = mem / rsize_rounded;

	allocator_t *alloc = safe_calloc(sizeof (allocator_t));
	*alloc = (allocator_t){
		.a_record_size = record_size,
		.a_record_size_rounded = rsize_rounded,
		.a_max_memory = maxmem_rounded,
		.a_fd = fd,
		.a_first_on_disk = first_on_disk,
		.a_base_addr = base,
		.a_pagesize = pagesize,
		.a_vm_allocated = vm_allocation,
		.a_vm_frontier = base,
		.a_frontier_granularity = granularity
	};
	return (alloc);
}

		/*
		 * Both old max_memory and new max_memory are at both page
		 * and record boundaries.
		 *
		 * 1) clamp to existing record range
		 * 2) calc covered record range
		 * 3) expand frontier
		 * 4) read from disk
		 * 5) set first_on_disk
		 */

static void
expand_max_memory(allocator_t *alloc, size_t new_size)
{
	record_ix_t new_fod = new_size / alloc->a_record_size_rounded;
	record_ix_t last_to_copy = MIN(alloc->a_count, new_fod) - 1;

	if (alloc->a_count > alloc->a_first_on_disk) {
		record_ix_t copy_tail = MIN(alloc->a_count, new_fod);
		size_t copy_bytes = (copy_tail - alloc->a_first_on_disk) *
		    alloc->a_record_size_rounded;
		off_t start_offset = alloc->a_first_on_disk *
		    alloc->a_record_size_rounded;
		void *start_addr = alloc->a_base_addr +
		    alloc->a_first_on_disk * alloc->a_record_size_rounded;
		safe_pread(alloc->a_fd, start_addr, copy_bytes, start_offset);
		punch_hole(alloc->a_fd, start_offset, copy_bytes);
	}
	alloc->a_first_on_disk = new_fod;
	alloc->a_max_memory = new_size;
}

void
allocator_set_max_memory(allocator_t *alloc, size_t max_memory)
{
	size_t new_size = P2ROUNDUP(max_memory,
	    lcm(alloc.a_pagesize, alloc.a_record_size_rounded));

	if (new_size > alloc->a_max_memory) {
		expand_max_memory(alloc, new_size);
	} else if (new_size < alloc->a_max_memory) {
		trim_max_memory(alloc, new_size);
	}


	max_memory = round_memory_up(max_memory, alloc->a_record_size);
	if (max_memory < alloc->a_max_memory) {
		if (alloc->a_count >= alloc->a_first_on_disk) {

		} else {

		}
	}
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
	void *first_byte = alloc->a_base_addr +
	    record * alloc->a_record_size_rounded;
	void *last_byte = first_byte + alloc->a_record_size_rounded - 1;
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

static record_location_t
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
