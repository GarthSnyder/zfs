/*
 * Simple linear allocator implementation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>
#include "allocator.h"
#include "zstream_shared.h"

#ifndef MAP_ANONYMOUS
#  ifdef MAP_ANON
#    define MAP_ANONYMOUS MAP_ANON
#  else
#    error "Neither MAP_ANONYMOUS nor MAP_ANON is defined"
#  endif
#endif

#ifndef MAP_NORESERVE
#  define MAP_NORESERVE 0
#endif

struct allocator {
	bool        using_disk;
	uint64_t    record_size;
	uint64_t    count;          /* number of records allocated */
	uint64_t    io_ops;         /* Number of reads and writes */
	void        *base_addr;     /* Memory allocator fields */
	uint64_t    max_memory;
	FILE				*file;			/* Disk allocator field */
};

allocator_t
allocator_init(uint64_t record_size, uint64_t max_memory, FILE *file) {
	assert(record_size);
	allocator_t alloc = safe_calloc(sizeof(struct allocator));
	alloc->using_disk = (file && !max_memory);
	alloc->record_size = record_size;
	alloc->file = file;
	if (max_memory) {
		// Use mmap to reserve address space MAP_NORESERVE on Linux prevents
		//swap space reservation. Pages are allocated on write (demand paging)
		int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
		long page_size = sysconf(_SC_PAGESIZE);
		page_size = page_size <= 0 ? 4096 : page_size;
		alloc->max_memory = max_memory;
		max_memory = page_size * 
			((uint64_t)ceil((double)max_memory / page_size) + 1);
		alloc->base_addr = mmap(NULL, max_memory, PROT_READ | PROT_WRITE,
			flags, -1, 0);
		if (alloc->base_addr == MAP_FAILED) {
			free(alloc);
			return NULL;
		}
	}
	return alloc;
}

static void
free_memory(allocator_t alloc) {
	if (alloc->base_addr) {
		munmap(alloc->base_addr, alloc->max_memory);
	}
	alloc->base_addr = NULL;
}

int
allocator_convert_to_disk(allocator_t alloc) {
	assert(alloc);
	fprintf(stderr, "Converting allocator from memory to disk\n");
	if (!alloc->using_disk) {
		if (!alloc->file) {
			return -1;
		}
		if (fwrite(alloc->base_addr, alloc->record_size, alloc->count,
			alloc->file) != alloc->count)
		{
			return -3;
		}
		free_memory(alloc);
		alloc->using_disk = true;
	}
	return 0;
}

void
allocator_get_stats(allocator_t alloc, allocator_stats_t *stats) {
	assert(alloc && stats);
	stats->num_ops = alloc->io_ops;
	stats->num_records = alloc->count;
	stats->mem_used = alloc->using_disk ? 
		0 : (alloc->count * alloc->record_size);
}

record_ix
allocator_append(allocator_t alloc, const void *data) {
	assert(alloc && data);
	return allocator_store(alloc, alloc->count, data);
}

record_ix
allocator_skip(allocator_t alloc) {
	assert(alloc);
	char *buffer = safe_calloc(alloc->record_size);
	record_ix ret = allocator_append(alloc, buffer);
	free(buffer);
	return ret;
}

record_ix
allocator_store(allocator_t alloc, record_ix record, const void *data)
{
	assert(alloc && data);
	alloc->io_ops++;

	if (!alloc->using_disk &&
		(record + 1) * alloc->record_size > alloc->max_memory)
	{
		int ret = allocator_convert_to_disk(alloc);
		if (ret < 0) { return ret; }
	}
	uint64_t offset = record * alloc->record_size;
	// Both file holes and unwritten mmap pages are guaranteed to
	// return zeros under POSIX, so we needn't fill gaps manually.
	if (alloc->using_disk) {
		if (fseeko(alloc->file, offset, SEEK_SET) ||
			fwrite(data, alloc->record_size, 1, alloc->file) != 1)
		{
			return -3;
		}
	} else {
		memcpy(alloc->base_addr + offset, data, alloc->record_size);
	}
	if (record >= alloc->count) {
		alloc->count = record + 1;
	}
	return record;
}

record_ix 
allocator_retrieve(allocator_t alloc, record_ix record, void* buffer) 
{
	uint64_t offset = record * alloc->record_size;
	assert(alloc && buffer && (record >= 0));
	alloc->io_ops++;
	/* Nonexistent records are returned zero-filled */
	if (record >= (record_ix)alloc->count) {
		memset(buffer, 0, alloc->record_size);
		return record;
	}
	if (alloc->using_disk) {
		if (fseeko(alloc->file, (off_t)offset, SEEK_SET) ||
			fread(buffer, alloc->record_size, 1, alloc->file) != 1)
		{
			return -3;
		}
	} else { 
		void *src = (char*)alloc->base_addr + offset;
		memcpy(buffer, src, alloc->record_size);
	}
	return record;
}

void
allocator_destroy(allocator_t alloc) {
	assert(alloc);
	if (!alloc->using_disk) {
		free_memory(alloc);
	}
	if (alloc->file) {
		fclose(alloc->file);
	}
	free(alloc);
}
