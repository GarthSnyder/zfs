/*
 * Simple linear allocator implementation
 */

#include "allocator.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* Platform detection */
#if defined(_WIN32) || defined(_WIN64)
    #define PLATFORM_WINDOWS
    #include <windows.h>
#else
    #define PLATFORM_POSIX
    #include <sys/mman.h>
    #include <unistd.h>
    #include <sys/stat.h>
#endif

/* Get system page size */
static size_t
get_page_size(void) {
#ifdef PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
#else
    long page_size = sysconf(_SC_PAGESIZE);
    return (page_size > 0) ? (size_t)page_size : 4096;
#endif
}

#ifdef PLATFORM_WINDOWS
/* Round up to next multiple of page_size */
static size_t round_up_to_page(size_t size, size_t page_size) {
    return ((size + page_size - 1) / page_size) * page_size;
}
#endif

static size_t
write_zeros(FILE *fp, size_t count)
{
    static const unsigned char zeros[4096] = { 0 };

    while (count > 0) {
        size_t chunk = count < sizeof (zeros) ? count : sizeof (zeros);
        if (fwrite(zeros, chunk, 1, fp) != chunk) {
            return (-1);
        }
        count -= chunk;
    }
    return (count);
}

static int
common_init(allocator_t *alloc, size_t record_size, size_t max_memory, FILE *file) 
{
    if (record_size == 0) { return -1; }

    memset(alloc, 0, sizeof(allocator_t));
    alloc->using_disk = (file && !max_memory);
    alloc->record_size = record_size;
    alloc->max_memory = max_memory;
    alloc->page_size = get_page_size();
    alloc->file = file;

    if (max_memory) {
#ifdef PLATFORM_WINDOWS
        /* Reserve address space without committing physical memory */
        alloc->base_addr = VirtualAlloc(NULL, max_memory,
                                        MEM_RESERVE, PAGE_READWRITE);
        if (!alloc->base_addr) {
            return (-1);
        }
        alloc->committed_bytes = 0;
#else
        /* Use mmap to reserve address space
         * MAP_NORESERVE on Linux prevents swap space reservation
         * Pages will be allocated on first write (demand paging)
         */
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
        flags |= MAP_NORESERVE;
#endif
        alloc->base_addr = mmap(NULL, max_memory, PROT_READ | PROT_WRITE,
                                flags, -1, 0);
        if (alloc->base_addr == MAP_FAILED) {
            return -1;
        }
#endif
    }
    return (0);
}


int
allocator_init_memory(allocator_t *alloc, size_t record_size, size_t max_memory) {
    return common_init(alloc, record_size, max_memory, NULL);
}

int
allocator_init_disk(allocator_t *alloc, size_t record_size, FILE *file) {
    if (!file) { return -1; }
    return common_init(alloc, record_size, 0, file);
}

int
allocator_init_convertible(allocator_t *alloc, size_t record_size, 
    size_t max_memory, FILE *file)
{
    return common_init(alloc, record_size, max_memory, file);
}

static void
free_memory(allocator_t *alloc) {
#ifdef PLATFORM_WINDOWS
    if (alloc->base_addr) {
        VirtualFree(alloc->base_addr, 0, MEM_RELEASE);
    }
#else
    if (alloc->base_addr && alloc->base_addr != MAP_FAILED) {
        munmap(alloc->base_addr, alloc->max_memory);
    }
#endif
    alloc->base_addr = NULL;
}

int
allocator_convert_to_disk(allocator_t *alloc) {
    if (alloc->using_disk) {
        return (0);
    }
    if (!alloc->file) {
        return -1;
    }
    if (alloc->count > 0) {
        size_t num_bytes = alloc->count * alloc->record_size;
        if (fwrite(alloc->base_addr, 1, num_bytes, alloc->file) != num_bytes) {
            fclose(alloc->file);
            return -3;
        }
    }
    free_memory(alloc);
    alloc->using_disk = true;
    return (0);
}

size_t
allocator_memory_use(allocator_t *alloc) {
    if (alloc->using_disk) {
        return 0;
    }
    return alloc->count * alloc->record_size;
}

record_ix
allocator_append(allocator_t* alloc, const void* data) {
    return allocator_store(alloc, data, alloc->count);
}

record_ix
allocator_skip(allocator_t* alloc) {
    char *buffer = calloc(1, alloc->record_size);
    if (!buffer) {
        return (-1);
    }
    record_ix ret = allocator_append(alloc, buffer);
    free(buffer);
    return ret;
}

record_ix
allocator_store(allocator_t *alloc, const void *data, record_ix record) {
    if (!alloc || !data) {
        return (-1);
    }

    uint64_t offset = record * alloc->record_size;
    uint64_t allocated_size = alloc->count * alloc->record_size;
    size_t gap_bytes = 0;

    /* Only compute gap if we're writing beyond current allocation */
    if (offset > allocated_size) {
        gap_bytes = offset - allocated_size;
    }

    if (alloc->using_disk) {
        if (gap_bytes > 0) {
            if (fseeko(alloc->file, (off_t)allocated_size, SEEK_SET) != 0) {
                return (-4);
            }
            write_zeros(alloc->file, gap_bytes);
        }
        if (fseeko(alloc->file, offset, SEEK_SET) != 0) {
            return (-5);
        }
        /* Write data record */
        size_t written = fwrite(data, alloc->record_size, 1, alloc->file);
        if (written != 1) {
            return (-6);  /* disk full or I/O error */
        }
    } else {
        /* Check if we have space */
        if (offset + alloc->record_size > alloc->max_memory) {
            return (-2);  /* out of space */
        }

#ifdef PLATFORM_WINDOWS
        /* Commit memory if needed */
        size_t required_bytes = offset + alloc->record_size;
        if (required_bytes > alloc->committed_bytes) {
            size_t new_committed = round_up_to_page(required_bytes,
                                                     alloc->page_size);
            if (new_committed > alloc->max_memory) {
                new_committed = alloc->max_memory;
            }

            size_t commit_size = new_committed - alloc->committed_bytes;
            void *commit_addr = (char *)alloc->base_addr + alloc->committed_bytes;

            if (!VirtualAlloc(commit_addr, commit_size,
                             MEM_COMMIT, PAGE_READWRITE)) {
                return (-3);
            }
            alloc->committed_bytes = new_committed;
        }
#endif
        /* On POSIX systems, pages are committed automatically on write */
        if (gap_bytes > 0) {
            memset(alloc->base_addr + allocated_size, 0, gap_bytes);
        }
        void* dest = (char*)alloc->base_addr + offset;
        memcpy(dest, data, alloc->record_size);
    }

    alloc->count = ((record_ix)alloc->count > record ? alloc->count : record) + 1;

    /* Return record number */
    return record;
}

record_ix 
allocator_retrieve(allocator_t* alloc, record_ix record, void* buffer) {
    if (!alloc || !buffer || record < 0) {
        return (-1);
    }

    uint64_t offset = record * alloc->record_size;

    /* Nonexistent records are returned zero-filled */
    if (record >= (record_ix)alloc->count) {
        memset(buffer, 0, alloc->record_size);
        return record;
    }

    if (alloc->using_disk) {
        if (fseeko(alloc->file, (off_t)offset, SEEK_SET) != 0) {
            return (-2);
        }
        size_t items_read = fread(buffer, alloc->record_size, 1, alloc->file);
        if (items_read != 1) {
            return (-3);
        }
    } else { 
        void* src = (char*)alloc->base_addr + offset;
        memcpy(buffer, src, alloc->record_size);
    }

    return record;
}

void allocator_destroy(allocator_t* alloc) {
    if (!alloc) {
        return;
    }

    if (!alloc->using_disk) {
#ifdef PLATFORM_WINDOWS
        if (alloc->base_addr) {
            VirtualFree(alloc->base_addr, 0, MEM_RELEASE);
        }
#else
        if (alloc->base_addr && alloc->base_addr != MAP_FAILED) {
            munmap(alloc->base_addr, alloc->max_memory);
        }
#endif
    } 
    if (alloc->file) {
        fclose(alloc->file);
    }
}
