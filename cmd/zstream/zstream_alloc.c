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

#include <errno.h>
#include <math.h>
#include <stddef.h>
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

struct allocator {
	boolean_t	a_using_disk;
	size_t		a_record_size;
	uint64_t	a_count;	/* number of records allocated */
	uint64_t	a_io_ops;	/* Number of reads and writes */
	void		*a_base_addr;	/* Memory allocator fields */
	size_t		a_max_memory;
	FILE		*a_file;	/* Disk allocator field */
};

allocator_t *
allocator_init(size_t record_size, size_t max_memory, FILE *file) {
	assert(record_size);
	allocator_t *alloc = safe_calloc(sizeof(struct allocator));
	alloc->a_using_disk = (file && !max_memory);
	alloc->a_record_size = record_size;
	alloc->a_file = file;
	if (max_memory) {
		/*
		 * Use mmap to reserve address space.MAP_NORESERVE on Linux prevents
		 * swap space reservation. Pages are allocated on write (demand
		 * paging)
		 */
		int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
		long page_size = sysconf(_SC_PAGESIZE);
		page_size = page_size <= 0 ? 4096 : page_size;
		alloc->a_max_memory = max_memory;
		max_memory = page_size *
			((uint64_t)ceil((double)max_memory / page_size) + 1);
		alloc->a_base_addr = mmap(NULL, max_memory, PROT_READ | PROT_WRITE,
			flags, -1, 0);
		if (alloc->a_base_addr == MAP_FAILED) {
			free(alloc);
			return NULL;
		}
	}
	return alloc;
}

static void
free_memory(allocator_t *alloc) {
	if (alloc->a_base_addr) {
		munmap(alloc->a_base_addr, alloc->a_max_memory);
	}
	alloc->a_base_addr = NULL;
}

int
allocator_convert_to_disk(allocator_t *alloc) {
	assert(alloc);
	fprintf(stderr, "Converting allocator from memory to disk\n");
	if (!alloc->a_using_disk) {
		if (!alloc->a_file) {
			return -1;
		}
		if (fwrite(alloc->a_base_addr, alloc->a_record_size,
			alloc->a_count, alloc->a_file) != alloc->a_count)
		{
			return -3;
		}
		free_memory(alloc);
		alloc->a_using_disk = B_TRUE;
	}
	return (0);
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
	assert(alloc && data);
	return allocator_store(alloc, alloc->a_count, data);
}

record_ix_t
allocator_skip(allocator_t *alloc) {
	assert(alloc);
	char *buffer = safe_calloc(alloc->a_record_size);
	record_ix_t ret = allocator_append(alloc, buffer);
	free(buffer);
	return ret;
}

record_ix_t
allocator_store(allocator_t *alloc, record_ix_t record, const void *data)
{
	assert(alloc && data);
	alloc->a_io_ops++;

	if (!alloc->a_using_disk &&
		(record + 1) * alloc->a_record_size > alloc->a_max_memory)
	{
		int ret = allocator_convert_to_disk(alloc);
		if (ret < 0) { return ret; }
	}
	uint64_t offset = record * alloc->a_record_size;
	// Both file holes and unwritten mmap pages are guaranteed to
	// return zeros under POSIX, so we needn't fill gaps manually.
	if (alloc->a_using_disk) {
		if (fseeko(alloc->a_file, offset, SEEK_SET)) {
			return (-3);
		}
		if (fwrite(data, alloc->a_record_size, 1, alloc->a_file) != 1) {
			return (-4);
		}
	} else {
		memcpy(alloc->a_base_addr + offset, data, alloc->a_record_size);
	}
	if (record >= alloc->a_count) {
		alloc->a_count = record + 1;
	}
	return record;
}

record_ix_t
allocator_retrieve(allocator_t *alloc, record_ix_t record, void* buffer)
{
	uint64_t offset = record * alloc->a_record_size;
	assert(alloc && buffer && (record >= 0));
	alloc->a_io_ops++;
	/* Nonexistent records are returned zero-filled */
	if (record >= (record_ix_t)alloc->a_count) {
		memset(buffer, 0, alloc->a_record_size);
		return record;
	}
	if (alloc->a_using_disk) {
		if (fseeko(alloc->a_file, (off_t)offset, SEEK_SET)) {
			return (-3);
		}
		if (fread(buffer, alloc->a_record_size, 1, alloc->a_file) != 1) {
			return (-4);
		}
	} else { 
		void *src = (char*)alloc->a_base_addr + offset;
		memcpy(buffer, src, alloc->a_record_size);
	}
	return record;
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
