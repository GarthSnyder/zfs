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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzfs.h>
#include <libzutil.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <umem.h>
#include <unistd.h>
#include <sys/blake3.h>
#include <sys/ddt.h>
#include <sys/debug.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/zfs_ioctl.h>
#include "zfs_fletcher.h"
#include "zstream.h"

#define	DEFAULT_DEDUP_PHYSMEM_PERCENT	20
#define	SMALLEST_POSSIBLE_DEDUP_MB		128
#define	DISK_CACHE_PHYSMEM_PERCENT		5
#define	STATUS_UPDATE_INTERVAL			(5 * 1000000000ULL)

typedef struct drr_write_subset { /* TODO: Check sizes and aligments and update pad */
	uint64_t 	drr_object;
	uint64_t 	drr_offset;
	uint64_t 	drr_toguid;
	uint64_t 	drr_logical_size;
	uint8_t 	drr_compressiontype;
	uint64_t 	drr_compressed_size;
	uint8_t 	drr_checksumtype;
	uint8_t 	drr_flags;
	ddt_key_t	drr_key;
	uint8_t 	drr_salt[ZIO_DATA_SALT_LEN];
	uint8_t 	drr_iv[ZIO_DATA_IV_LEN];
	uint8_t 	drr_mac[ZIO_DATA_MAC_LEN];
} drr_write_subset_t;

typedef struct dedup_entry {
	struct dedup_entry	*next;
	drr_write_subset_t	block_data
	uint8_t				hash[BLAKE3_OUT_LEN];
	uint64_t			payload_length;
} dedup_entry_t;

typedef struct dedup_table {
	dedup_entry_t	**hash_array;
	umem_cache_t	*entry_cache;
	uint64_t		num_entries;
	int				num_hash_bits;
	uint64_t		max_memory;
	cache_dir		*cache_dir;
} dedup_table_t;

typedef struct dedup_stats {
	uint64_t	total_records;
	uint64_t	write_records;
	uint64_t	dedup_records;
	uint64_t	bytes_read;
	uint64_t	bytes_saved;
	uint64_t	disqualified_records;
	hrtime_t	last_status_time;
} dedup_stats_t;

/*
 * Reduce a 256-bit Blake3 hash to a 64-bit hash key.
 */
static uint64_t
blake3_to_hash_key(const uint8_t *blake3_hash)
{
	uint64_t result;
	memcpy(&result, blake3_hash, sizeof (result));
}

/*
 * Compare two Blake3 hashes for equality.
 */
static boolean_t
blake3_equal(const uint8_t *hash1, const uint8_t *hash2)
{
	return (memcmp(hash1, hash2, BLAKE3_OUT_LEN) == 0);
}

/*
 * Initialize the deduplication table.
 */
static void
dedup_table_init(dedup_table_t *ddt, uint64_t max_memory,
    const char *cache_dir)
{
	uint64_t num_buckets;

	memset(ddt, 0, sizeof (*ddt));
	ddt->max_memory = max_memory;

	num_buckets = max_memory / sizeof (dedup_entry_t);

	/*
	 * num_buckets must be a power of 2.  Increase number to
	 * a power of 2 if necessary.
	 */
	if (!ISP2(num_buckets))
		num_buckets = 1ULL << highbit64(num_buckets);

	ddt->hash_array = safe_calloc(num_buckets * sizeof (dedup_entry_t *));
	ddt->entry_cache = umem_cache_create("dedup_entry",
	    sizeof (dedup_entry_t), 0, NULL, NULL, NULL, NULL, NULL, 0);
	ddt->num_hash_bits = highbit64(num_buckets) - 1;
	ddt->num_entries = 0;
}

/*
 * Look up an entry in the disk cache.
 * Returns B_TRUE if found, B_FALSE otherwise.
 * If found, populates the provided result structure.
 */
/*
static boolean_t
disk_cache_lookup(dedup_table_t *ddt, const uint8_t *blake3_hash,
    dedup_entry_t *result)
{
	uint64_t hash_key;
	disk_entry_t disk_entry;
	off_t offset;
	ssize_t bytes_read;

	if (ddt->disk_fd < 0)
		return (B_FALSE);

	memcpy(&hash_key, blake3_hash, sizeof (hash_key));

	offset = 0;
	while ((bytes_read = pread(ddt->disk_fd, &disk_entry,
	    sizeof (disk_entry), offset)) == sizeof (disk_entry)) {
		if (blake3_equal(disk_entry.hash, blake3_hash)) {
			memcpy(result->hash, disk_entry.hash, BLAKE3_OUT_LEN);
			result->guid = disk_entry.guid;
			result->object = disk_entry.object;
			result->offset = disk_entry.offset;
			result->length = disk_entry.length;
			result->checksumtype = disk_entry.checksumtype;
			result->flags = disk_entry.flags;
			result->compressiontype = disk_entry.compressiontype;
			result->compressed_size = disk_entry.compressed_size;
			result->next = NULL;
			return (B_TRUE);
		}
		offset += sizeof (disk_entry);
	}

	return (B_FALSE);
}
*/

/*
 * Look up an entry in the deduplication table by Blake3 hash.
 * Returns the entry if found, NULL otherwise.
 */
static dedup_entry_t *
dedup_table_lookup(dedup_table_t *ddt, const uint8_t *blake3_hash)
{
	uint64_t hashcode;
	dedup_entry_t *entry;
	static dedup_entry_t disk_result;

	/*
	if (ddt->disk_only) {
		if (disk_cache_lookup(ddt, blake3_hash, &disk_result)) {
			return (&disk_result);
		}
		return (NULL);
	}
	*/

	hashcode = blake3_to_hash_key(blake3_hash, ddt->num_hash_bits);

	for (entry = ddt->hash_array[hashcode]; entry != NULL;
	    entry = entry->next) {
		if (blake3_equal(entry->hash, blake3_hash)) {
			return (entry);
		}
	}

	/* If using disk cache, check there too */
	/*
	if (ddt->using_disk) {
		if (disk_cache_lookup(ddt, blake3_hash, &disk_result)) {
			return (&disk_result);
		}
	}
	*/

	return (NULL);
}

/*
 * Write an entry to the disk cache.
 */
/*
static void
disk_cache_insert(dedup_table_t *ddt, const uint8_t *blake3_hash,
    uint64_t guid, uint64_t object, uint64_t offset, uint64_t length,
    uint8_t checksumtype, uint8_t flags, uint8_t compressiontype,
    uint64_t compressed_size)
{
	disk_entry_t disk_entry;
	ssize_t written;

	if (ddt->disk_fd < 0)
		return;

	memcpy(disk_entry.hash, blake3_hash, BLAKE3_OUT_LEN);
	disk_entry.guid = guid;
	disk_entry.object = object;
	disk_entry.offset = offset;
	disk_entry.length = length;
	disk_entry.checksumtype = checksumtype;
	disk_entry.flags = flags;
	disk_entry.compressiontype = compressiontype;
	disk_entry.compressed_size = compressed_size;
	disk_entry.pad = 0;

	written = pwrite(ddt->disk_fd, &disk_entry, sizeof (disk_entry),
	    ddt->disk_offset);
	if (written != sizeof (disk_entry)) {
		fprintf(stderr, "Error writing to cache file: %s\n",
		    strerror(errno));
		exit(1);
	}

	ddt->disk_offset += sizeof (disk_entry);
}
*/

static void
drr_write_to_drr_write_subset(const drr_write *drrw, 
	drr_write_subset_t *wb)
{
	wb->drr_toguid = drrw->drr_toguid;
	wb->drr_object = drrw->drr_object;
	wb->drr_offset = drrw->drr_offset;
	wb->drr_type = drrw->drr_type;
	wb->drr_logical_size = drrw->drr_logical_size;
	wb->drr_checksumtype = drrw->drr_checksumtype;
	wb->drr_compressiontype = drrw->drr_compressiontype;
	wb->drr_flags = drrw->drr_flags;
	wb->drr_compressed_size = drrw->ddr_compressed_size;
	wb->ddt_key = drrw->ddt_key;
}

/*
 * Insert an entry into the deduplication table.
 */
static void
dedup_table_insert(dedup_table_t *ddt, const uint8_t *blake3_hash,
	ddr_write *drrw, uint64_t payload_length)
{
	uint64_t hashcode;
	dedup_entry_t *entry;

	/*
	if (ddt->disk_only) {
		disk_cache_insert(ddt, blake3_hash, guid, object, offset,
		    length, checksumtype, flags, compressiontype,
		    compressed_size);
		ddt->num_entries++;
		return;
	}
	*/

	hashcode = blake3_to_hash_key(blake3_hash, ddt->num_hash_bits);

	entry = umem_cache_alloc(ddt->entry_cache, UMEM_NOFAIL);
	memcpy(entry->hash, blake3_hash, BLAKE3_OUT_LEN);
	entry->payload_length = payload_length;
	drr_write_to_drr_write_subset(drrw, &entry->block_data);
	ddt->hash_array[hashcode] = entry;
	ddt->num_entries++;

	/* If using disk cache, also write to disk */
	/*
	if (ddt->using_disk) {
		disk_cache_insert(ddt, blake3_hash, guid, object, offset,
		    length, checksumtype, flags, compressiontype,
		    compressed_size);
	}
	*/
}

/*
 * Check if we should extend the cache to disk.
 */
/*
static boolean_t
should_extend_to_disk(dedup_table_t *ddt)
{
	uint64_t current_memory;

	if (ddt->using_disk)
		return (B_FALSE);

	current_memory = ddt->num_entries * sizeof (dedup_entry_t);
	return (current_memory >= ddt->max_memory);
}
*/

/*
 * Check if we should convert to disk-only cache.
 */
/*
static boolean_t
should_convert_to_disk_only(dedup_table_t *ddt)
{
	uint64_t current_memory;
	uint64_t disk_threshold;

#ifdef _ILP32
	return (B_FALSE);
#else
	if (ddt->disk_only || !ddt->using_disk)
		return (B_FALSE);

	current_memory = ddt->num_entries * sizeof (dedup_entry_t);
	disk_threshold = ddt->max_memory +
	    (sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE) *
	    DISK_CACHE_PHYSMEM_PERCENT) / 100;

	return (current_memory >= disk_threshold);
#endif
}
*/

/*
 * Extend the cache to disk.
 */
/*
static void
extend_to_disk(dedup_table_t *ddt)
{
	if (ddt->using_disk)
		return;

	if (strstr(ddt->disk_path, "XXXXXX") != NULL) {
		ddt->disk_fd = mkstemp(ddt->disk_path);
	} else {
		ddt->disk_fd = open(ddt->disk_path,
		    O_RDWR | O_CREAT | O_TRUNC, 0600);
	}

	if (ddt->disk_fd < 0) {
		fprintf(stderr, "Error: could not create cache file '%s': %s\n",
		    ddt->disk_path, strerror(errno));
		exit(1);
	}

	ddt->using_disk = B_TRUE;
	ddt->disk_offset = 0;

	fprintf(stderr, "Extending block cache to disk...\n");
}
*/

/*
 * Convert to disk-only cache.
 */
/*
static void
convert_to_disk_only(dedup_table_t *ddt)
{
	uint64_t i;
	dedup_entry_t *entry, *next;
	uint64_t num_buckets;

	if (ddt->disk_only)
		return;

	fprintf(stderr, "Converting to disk-only cache...\n");
*/
	/*
	 * All entries should already be in the disk cache if we're using disk.
	 * We just need to free the in-memory structures.
	 */
/*
	num_buckets = 1ULL << ddt->num_hash_bits;
	for (i = 0; i < num_buckets; i++) {
		entry = ddt->hash_array[i];
		while (entry != NULL) {
			next = entry->next;
			umem_cache_free(ddt->entry_cache, entry);
			entry = next;
		}
		ddt->hash_array[i] = NULL;
	}

	ddt->disk_only = B_TRUE;
}
*/

/*
 * Print status update to stderr.
 */
static void
print_status(dedup_stats_t *stats, boolean_t force)
{
	hrtime_t now = gethrtime();
	char bytes_read_str[32];
	char bytes_saved_str[32];
	double saved_pct;

	if (!force && (now - stats->last_status_time) < STATUS_UPDATE_INTERVAL)
		return;

	zfs_nicenum(stats->bytes_read, bytes_read_str, sizeof (bytes_read_str));
	zfs_nicenum(stats->bytes_saved, bytes_saved_str,
	    sizeof (bytes_saved_str));

	if (stats->bytes_read > 0) {
		saved_pct = (double)stats->bytes_saved * 100.0 /
		    (double)stats->bytes_read;
	} else {
		saved_pct = 0.0;
	}

	fprintf(stderr, "\rBlocks: %llu write, %llu dedup | "
	    "Size: %sB read / %sB saved (%.1f%%)    ",
	    (unsigned long long)stats->write_records,
	    (unsigned long long)stats->dedup_records,
	    bytes_read_str, bytes_saved_str, saved_pct);
	fflush(stderr);

	stats->last_status_time = now;
}

/*
 * Write a record to the output stream.
 */
static int
write_record(dmu_replay_record_t *drr, void *payload, uint32_t payload_length,
    zio_cksum_t *zc, int outfd)
{
	assert(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum)
	    == sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	fletcher_4_incremental_native(drr,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum), zc);
	if (drr->drr_type != DRR_BEGIN) {
		assert(ZIO_CHECKSUM_IS_ZERO(
			&drr->drr_u.drr_checksum.drr_checksum));
		drr->drr_u.drr_checksum.drr_checksum = *zc;
	}
	fletcher_4_incremental_native(&drr->drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t), zc);
	if (write(outfd, drr, sizeof (*drr)) == -1)
		return (errno);
	if (payload_length != 0) {
		fletcher_4_incremental_native(payload, payload_len, zc);
		if (write(outfd, payload, payload_length) == -1)
			return (errno);
	}
	return (0);
}

static boolean_t
writes_compatible(const drr_write *drrw, const drr_write_subset_t *wbh) {
	return (
		drrw->drr_type == wbh->drr_type &&
		drrw->drr_compressiontype == wbh->drr_compressiontype &&
		drrw->drr_checksumtype == wbh->drr_compressiontype &&
		drrw->drr_flags = wbh->drr_flags
	);
}

static void
assemble_write_byref(dmu_replay_record_t *byref_drr, drr_write *write_drr,
	drr_write_subset_t *wbh)
{
	memset(byref_drr, 0, sizeof (*byref_drr));

	byref_drr.drr_type = DRR_WRITE_BYREF;
	byref_drr.drr_payloadlen = 0;

	drr_write_byref *drrwbr = &byref_drr->drr_u.drr_write_byref;

	drrwbr->drr_object = drrw->drr_object;
	drrwbr->drr_offset = drrw->drr_offset;
	drrwbr->drr_length = drrw->drr_logical_size; /* TODO: Correct? */
	drrwbr->drr_toguid = drrw->drr_toguid;
	drrwbr->drr_checksumtype = drrw->drr_checksumtype;
	drrwbr->drr_flags = drrw->drr_flags;
	drrwbr->drr_key = drrw->drr_key;

	drrwbr->drr_refguid = wbh->drr_toguid;
	drrwbr->drr_refobject = wbh->drr_object;
	drrwbr->drr_refoffset = wbh->drr_offset;
}

/*
 * Process a DDR_WRITE record and possibly convert it to DDR_WRITE_BYREF.
 */
static int
process_write_record(const dmu_replay_record_t *drr, const void *payload,
    dedup_table_t *ddt, dedup_stats_t *stats, zio_cksum_t *stream_cksum,
    const int outfd)
{
	const drr_write *drrw = &drr->drr_u.drr_write;
	uint64_t payload_length = DRR_WRITE_PAYLOAD_SIZE(drrw);
	uint8_t blake3_hash[BLAKE3_OUT_LEN];
	BLAKE3_CTX blake3_ctx;
	dedup_entry_t *existing;

	stats->write_records++;
	stats->bytes_read += payload_length;

	/* Compute Blake3 hash of the payload */
	Blake3_Init(&blake3_ctx);
	Blake3_Update(&blake3_ctx, payload, payload_length);
	Blake3_Final(&blake3_ctx, blake3_hash);

	/* Check if we've seen this block before */
	existing = dedup_table_lookup(ddt, blake3_hash);

	if (existing != NULL) {
		drr_write_subset_t *wbh = &existing->block_data;

		if (!writes_compatible(drrw, wbh)) {
			return write_record(drr, payload, payload_length, 
				stream_cksum, outfd);
		}

		/* Convert to DDR_WRITE_BYREF */
		dmu_replay_record_t byref_drr;
		struct drr_write_byref *drrwbr;

		assemble_write_byref(&byref_drr, drrw, wbh);

		stats->dedup_records++;
		stats->bytes_saved += payload_length;

		return write_record(&byref_drr, NULL, 0, stream_cksum, outfd);
	} else {
		/* First occurrence, insert into table and write as-is */
		dedup_table_insert(ddt, blake3_hash, drrw);

		/* Check if we need to extend to disk or convert to disk-only 
		if (should_extend_to_disk(ddt)) {
			extend_to_disk(ddt);
		} else if (should_convert_to_disk_only(ddt)) {
			convert_to_disk_only(ddt);
		}
		*/

		return write_record(drr, payload, payload_size, stream_cksum, outfd);
	}
}

/*
 * Deduplicate a ZFS stream.
 */
static void
zfs_dedup_stream(FILE *input, int outfd, dedup_table_t *ddt, boolean_t verbose)
{
	int bufsz = SPA_MAXBLOCKSIZE;
	dmu_replay_record_t thedrr;
	dmu_replay_record_t *drr = &thedrr;
	zio_cksum_t stream_cksum;
	dedup_stats_t stats;
	int begin = 0;
	boolean_t seen = B_FALSE;

	memset(&thedrr, 0, sizeof (dmu_replay_record_t));
	memset(&stats, 0, sizeof (stats));
	stats.last_status_time = gethrtime();

	char *buf = safe_calloc(bufsz);

	while (sfread(drr, sizeof (*drr), input) != 0) {
		stats.total_records++;

		/*
		 * We need to regenerate the checksum.
		 */
		if (drr->drr_type != DRR_BEGIN) {
			memset(&drr->drr_u.drr_checksum.drr_checksum, 0,
			    sizeof (drr->drr_u.drr_checksum.drr_checksum));
		}

		uint64_t payload_size = 0;
		switch (drr->drr_type) {
		case DRR_BEGIN:
		{
			struct drr_begin *drrb = &drr->drr_u.drr_begin;
			int fflags;
			ZIO_SET_CHECKSUM(&stream_cksum, 0, 0, 0, 0);
			VERIFY0(begin++);
			seen = B_TRUE;

			assert(drrb->drr_magic == DMU_BACKUP_MAGIC);

			/* set the DEDUP feature flag for this stream */
			fflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
			fflags |= DMU_BACKUP_FEATURE_DEDUP;
			/* cppcheck-suppress syntaxError */
			DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, fflags);

			uint32_t sz = drr->drr_payloadlen;

			VERIFY3U(sz, <=, 1U << 28);

			if (sz != 0) {
				if (sz > bufsz) {
					free(buf);
					buf = safe_calloc(sz);
					bufsz = sz;
				}
				(void) sfread(buf, sz, input);
			}
			payload_size = sz;
			if (write_record(drr, buf, payload_size,
			    &stream_cksum, outfd) != 0)
				goto error;
			break;
		}

		case DRR_END:
		{
			struct drr_end *drre = &drr->drr_u.drr_end;
			VERIFY3B(seen, ==, B_TRUE);
			begin--;
			/*
			 * Use the recalculated checksum, unless this is
			 * the END record of a stream package, which has
			 * no checksum.
			 */
			if (!ZIO_CHECKSUM_IS_ZERO(&drre->drr_checksum))
				drre->drr_checksum = stream_cksum;
			if (write_record(drr, NULL, 0, &stream_cksum, outfd)
			    != 0)
				goto error;
			if (begin == 0) {
				ZIO_SET_CHECKSUM(&stream_cksum, 0, 0, 0, 0);
			}
			break;
		}

		case DRR_OBJECT:
		{
			struct drr_object *drro = &drr->drr_u.drr_object;
			VERIFY3S(begin, ==, 1);

			if (drro->drr_bonuslen > 0) {
				payload_size = DRR_OBJECT_PAYLOAD_SIZE(drro);
				(void) sfread(buf, payload_size, input);
			}
			if (write_record(drr, buf, payload_size,
			    &stream_cksum, outfd) != 0)
				goto error;
			break;
		}

		case DRR_SPILL:
		{
			struct drr_spill *drrs = &drr->drr_u.drr_spill;
			VERIFY3S(begin, ==, 1);
			payload_size = DRR_SPILL_PAYLOAD_SIZE(drrs);
			(void) sfread(buf, payload_size, input);
			if (write_record(drr, buf, payload_size,
			    &stream_cksum, outfd) != 0)
				goto error;
			break;
		}

		case DRR_WRITE:
		{
			struct drr_write *drrw = &drr->drr_u.drr_write;
			VERIFY3S(begin, ==, 1);
			payload_size = DRR_WRITE_PAYLOAD_SIZE(drrw);
			(void) sfread(buf, payload_size, input);

			if (process_write_record(&drr->drr_u.drr_write, buf, ddt, &stats,
			    &stream_cksum, outfd) != 0)
				goto error;

			if (verbose) {
				print_status(&stats, B_FALSE);
			}
			break;
		}

		case DRR_WRITE_EMBEDDED:
		{
			struct drr_write_embedded *drrwe =
			    &drr->drr_u.drr_write_embedded;
			VERIFY3S(begin, ==, 1);
			payload_size = P2ROUNDUP((uint64_t)drrwe->drr_psize, 8);
			(void) sfread(buf, payload_size, input);
			if (write_record(drr, buf, payload_size,
			    &stream_cksum, outfd) != 0)
				goto error;
			break;
		}

		case DRR_WRITE_BYREF:
			/*
			 * If we encounter a WRITE_BYREF in the input stream,
			 * pass it through unchanged. This is already a dedup
			 * reference.
			 */
			VERIFY3S(begin, ==, 1);
			if (write_record(drr, NULL, 0, &stream_cksum, outfd)
			    != 0)
				goto error;
			break;

		case DRR_FREEOBJECTS:
		case DRR_FREE:
		case DRR_OBJECT_RANGE:
			VERIFY3S(begin, ==, 1);
			if (write_record(drr, NULL, 0, &stream_cksum, outfd)
			    != 0)
				goto error;
			break;

		default:
			(void) fprintf(stderr, "INVALID record type 0x%x\n",
			    drr->drr_type);
			assert(B_FALSE);
		}

		if (feof(input)) {
			fprintf(stderr, "\nError: unexpected end-of-file\n");
			exit(1);
		}
		if (ferror(input)) {
			fprintf(stderr, "\nError while reading file: %s\n",
			    strerror(errno));
			exit(1);
		}
	}

	if (verbose) {
		print_status(&stats, B_TRUE);
		fprintf(stderr, "\n");

		char mem_str[32];
		zfs_nicenum(ddt->num_entries * sizeof (dedup_entry_t),
		    mem_str, sizeof (mem_str));
		fprintf(stderr,
		    "Processed %llu total records, including %llu write "
		    "records.\n",
		    (unsigned long long)stats.total_records,
		    (unsigned long long)stats.write_records);
		fprintf(stderr,
		    "Deduplicated %llu blocks, using %sB memory.\n",
		    (unsigned long long)stats.dedup_records, mem_str);
	}

	free(buf);
	return;

error:
	fprintf(stderr, "\nError while writing output: %s\n",
	    strerror(errno));
	free(buf);
	exit(1);
}

/*
 * Clean up the deduplication table.
 */
static void
dedup_table_fini(dedup_table_t *ddt)
{
	if (ddt->disk_fd >= 0) {
		close(ddt->disk_fd);
		if (strstr(ddt->disk_path, "/tmp/") != NULL) {
			unlink(ddt->disk_path);
		}
	}
	if (ddt->disk_path != NULL) {
		free(ddt->disk_path);
	}
	if (ddt->entry_cache != NULL) {
		umem_cache_destroy(ddt->entry_cache);
	}
	if (ddt->hash_array != NULL) {
		free(ddt->hash_array);
	}
}

int
zstream_do_dedup(int argc, char *argv[])
{
	boolean_t verbose = B_FALSE;
	int mem_percent = DEFAULT_DEDUP_PHYSMEM_PERCENT;
	char *cache_file = NULL;
	int in_fd = 0;
	int c;

	while ((c = getopt(argc, argv, "vm:c:")) != -1) {
		switch (c) {
		case 'v':
			verbose = B_TRUE;
			break;
		case 'm':
			mem_percent = atoi(optarg);
			if (mem_percent <= 0 || mem_percent > 100) {
				(void) fprintf(stderr,
				    "invalid memory percentage '%s'\n",
				    optarg);
				return (1);
			}
			break;
		case 'c':
			cache_file = optarg;
			break;
		case '?':
			(void) fprintf(stderr, "invalid option '%c'\n",
			    optopt);
			zstream_usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 1) {
		(void) fprintf(stderr, "too many arguments\n");
		zstream_usage();
		return (1);
	}

	if (isatty(STDOUT_FILENO)) {
		(void) fprintf(stderr,
		    "Error: Stream can not be written to a terminal.\n"
		    "You must redirect standard output.\n");
		return (1);
	}

	/* If a filename is provided, open it; otherwise use stdin */
	if (argc == 1) {
		const char *filename = argv[0];
		in_fd = open(filename, "r");
		if (input == -1) {
			(void) fprintf(stderr,
			    "Error while opening file '%s': %s\n",
			    filename, perror(errno));
			return (1);
		}
	}

	/* Calculate maximum memory for dedup table */
	uint64_t max_memory;
#ifdef _ILP32
	max_memory = SMALLEST_POSSIBLE_DEDUP_MB << 20;
#else
	uint64_t physbytes = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
	max_memory = MAX((physbytes * mem_percent) / 100,
	    SMALLEST_POSSIBLE_DEDUP_MB << 20);
#endif

	/* Initialize dedup table */
	dedup_table_t ddt;
	dedup_table_init(&ddt, max_memory, cache_file);

	fletcher_4_init();
	zfs_dedup_stream(input, STDOUT_FILENO, &ddt, verbose);
	fletcher_4_fini();

	dedup_table_fini(&ddt);

	if (input != stdin) {
		fclose(input);
	}

	return (0);
}
