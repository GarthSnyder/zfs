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
#include "linear_hash.h"

#define	DEFAULT_DEDUP_PHYSMEM_PERCENT	30
#define SMALLEST_REASONABLE_DEDUP_MB	128
#define	STATUS_UPDATE_INTERVAL			(5 * 1000000000ULL)

#define BLAKE3_64_BIT(full_hash) (*((uint64_t *)full_hash))

typedef uint8_t blake3_hash_t[BLAKE3_OUT_LEN];

typedef struct dedup_entry {
	ddr_write		write_block;
	zio_cksum_t		checksum;  
	blake3_hash_t	hash;
	uint64_t		payload_length;
} dedup_entry_t;

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
	    "Size: %sB read / %sB saved (%.1f%%)    \n",
	    (unsigned long long)stats->write_records,
	    (unsigned long long)stats->dedup_records,
	    bytes_read_str, bytes_saved_str, saved_pct);
	fflush(stderr);

	stats->last_status_time = now;
}

static int
emit_record(dmu_replay_record_t *drr, void *payload, int payload_len,
    zio_cksum_t *zc, int outfd, dedup_stats_t *stats)
{
	stats->bytes_read += sizeof(dmu_replay_record_t);
	stats->bytes_read += payload_len;
	return dump_record(drr, payload, payload_len, zc, outfd);
}

static bool
writes_compatible(const drr_write *this, const drr_write *prev) {
	if (this->drr_type != prev->drr_type
		|| this->drr_logical_size != prev->drr_logical_size
		|| this->drr_compressiontype != prev->drr_compressiontype
		|| this->drr_compressed_size != prev->drr_compressed_size
		|| memcmp(this->drr_salt, prev->drr_salt, sizeof(this->drr_salt))
		|| memcmp(this->drr_iv, prev->drr_iv, sizeof(this->drr_iv))
		|| memcmp(this->drr_mac, prev->drr_mac, sizeof(this->drr_mac))
	) {
		return false;
	}
	return true;
}

static void
assemble_write_byref(dmu_replay_record_t *byref_drr, 
	dmu_replay_record_t *being_deduped, dedup_entry_t *prev)
{
	memset(byref_drr, 0, sizeof(*byref_drr));

	byref_drr->drr_type = DRR_WRITE_BYREF;
	byref_drr->drr_payloadlen = 0;

	drr_write_byref *drrwbr = &byref_drr->drr_u.drr_write_byref;
	drr_write *thisw = &being_deduped->drr_u.drr_write;

	drrwbr->drr_refguid = prev->write_block.drr_toguid;
	drrwbr->drr_refobject = prev->write_block.drr_object;
	drrwbr->drr_refoffset = prev->write_block.drr_offset;

	drrwbr->drr_object = thisw->drr_object;
	drrwbr->drr_offset = thisw->drr_offset;
	drrwbr->drr_toguid = thisw->drr_toguid;

	drrwbr->drr_length = thisw->drr_logical_size;
	drrwbr->drr_checksumtype = thisw->drr_checksumtype;
	drrwbr->drr_flags = thisw->drr_flags;
	drrwbr->drr_key = thisw->drr_key;

	memcpy(&drrwbr->drr_checksum, &being_deduped->drr_checksum,
		sizeof(drrwbr->drr_checksum));
	memcpy(&drrwbr->drr_key, &thisw->drr_key, sizeof(thisw->drr_key));
}

bool
dedup_table_lookup(linear_hash_t *ddt, blake3_hash_t hash, dedup_entry_t *dde)
{
	lh_iterator_t iter;

	int ret = lh_retrieve_setup(&ddt->linear_hash, BLAKE3_64_BIT(hash), &iter);
	if (ret < 0) {
		fprintf(stderr, "Unable to initiate read from dedup table, aborting...\n");
		exit(1);
	}

	/* 
	 * Dedup table hashes are only 64 bits, so a table match does not
	 * guarantee an actual BLAKE3 match.
	 */
	while (true) {
		ret = lh_retrieve_next(&iter, dde);
		if (ret < 0) {
			fprintf(stderr, "Read error on dedup table, aborting...\n");
			exit(1);
		}
		if (iter.iteration_complete) {
			return false;
		}
		if (memcmp(&dde->hash, hash, sizeof(hash)) == 0) {
			return true;
		}
	}
	return false;
}

void
dedup_table_insert(linear_hash_t *ddt, blake3_hash_t hash, 
	dmu_replay_record_t *drr)
{
	dedup_entry_t dedup;

	memcpy(&dedup.write_block, &drr->drr_u.drr_write, 
		sizeof(drr->drr_u.drr_write));
	memcpy(&dedup.hash, hash, sizeof(dedup.hash));
	memcpy(&dedup.checksum, &drr->drr_checksum.drr_checksum,
		sizeof(dedup.checksum));
	dedup.payload_length = drr->drr_payloadlen;

	if (lh_insert(ddt, BLAKE3_64_BIT(hash), &dedup) < 0) {
		fprintf(stderr, "Error writing to dedup hash table, aborting...\n");
		exit(1);
	}
}

/*
 * Process a DDR_WRITE record and possibly convert it to DDR_WRITE_BYREF.
 */
static int
process_write_record(const dmu_replay_record_t *drr, const void *payload,
    linear_hash_t *ddt, dedup_stats_t *stats, zio_cksum_t *stream_cksum,
    const int outfd)
{
	const drr_write *drrw = &drr->drr_u.drr_write;
	uint64_t payload_length = DRR_WRITE_PAYLOAD_SIZE(drrw);
	uint8_t blake3_hash[BLAKE3_OUT_LEN];
	BLAKE3_CTX blake3_ctx;
	dedup_entry_t existing;

	stats->write_records++;
	stats->bytes_read += payload_length;

	/* Compute Blake3 hash of the payload */
	Blake3_Init(&blake3_ctx);
	Blake3_Update(&blake3_ctx, payload, payload_length);
	Blake3_Final(&blake3_ctx, blake3_hash);

	/* Check if we've seen this block before */
	if (dedup_table_lookup(ddt, blake3_hash, &existing)) {

		if (!writes_compatible(drrw, &existing.write_block)) {
			stats->disqualified_records++;
			return emit_record(drr, payload, payload_length, 
				stream_cksum, outfd, stats);
		}

		dmu_replay_record_t byref_drr;
		assemble_write_byref(&byref_drr, drr, &existing);

		stats->dedup_records++;
		stats->bytes_saved += payload_length;

		return emit_record(&byref_drr, NULL, 0, stream_cksum, outfd, stats);

	} else {
		/* First occurrence, insert into table and write as-is */
		dedup_table_insert(ddt, blake3_hash, drr);
		return emit_record(drr, payload, payload_length, stream_cksum, outfd, stats);
	}
}

/*
 * Deduplicate a ZFS stream.
 */
static void
zfs_dedup_stream(int infd, int outfd, linear_hash_t *ddt, bool verbose)
{
	int bufsz = SPA_MAXBLOCKSIZE;
	dmu_replay_record_t thedrr;
	dmu_replay_record_t *drr = &thedrr;
	zio_cksum_t stream_cksum;
	dedup_stats_t stats;
	int begin = 0;
	bool saw_begin = false;

	memset(&thedrr, 0, sizeof (dmu_replay_record_t));
	memset(&stats, 0, sizeof (stats));
	stats.last_status_time = gethrtime();

	char *buf = safe_calloc(bufsz);
	FILE *input = fdopen(infd, "r");
	if (input == NULL) {
		fprintf(stderr, "Unable to open input file, aborting...\n");
		exit(1);
	}

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
			saw_begin = true;

			assert(drrb->drr_magic == DMU_BACKUP_MAGIC);

			/* set the DEDUP feature flag for this stream */
			fflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
			if (fflags & DMU_BACKUP_FEATURE_DEDUP) {
				fprintf(stderr, "Input stream is already deduplicated.\n"
					"To re-deduplicate, pipe through zstream redup first.\n");
				exit(1);
			}
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
			if (emit_record(drr, buf, payload_size, &stream_cksum, outfd, 
				&stats) != 0)
			{
				goto error;
			}
			break;
		}

		case DRR_END:
		{
			struct drr_end *drre = &drr->drr_u.drr_end;
			VERIFY3B(saw_begin, ==, true);
			begin--;
			/*
			 * Use the recalculated checksum, unless this is
			 * the END record of a stream package, which has
			 * no checksum.
			 */
			if (!ZIO_CHECKSUM_IS_ZERO(&drre->drr_checksum))
				drre->drr_checksum = stream_cksum;
			if (emit_record(drr, NULL, 0, &stream_cksum, outfd, &stats) != 0) {
				goto error;
			}
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
			if (emit_record(drr, buf, payload_size, &stream_cksum, outfd, 
				&stats) != 0)
			{
				goto error;
			}
			break;
		}

		case DRR_SPILL:
		{
			struct drr_spill *drrs = &drr->drr_u.drr_spill;
			VERIFY3S(begin, ==, 1);
			payload_size = DRR_SPILL_PAYLOAD_SIZE(drrs);
			(void) sfread(buf, payload_size, input);
			if (emit_record(drr, buf, payload_size, &stream_cksum, outfd, &stats) != 0) {
				goto error;
			}
			break;
		}

		case DRR_WRITE:
		{
			struct drr_write *drrw = &drr->drr_u.drr_write;
			VERIFY3S(begin, ==, 1);
			payload_size = DRR_WRITE_PAYLOAD_SIZE(drrw);
			(void) sfread(buf, payload_size, input);

			if (process_write_record(drr, buf, ddt, &stats,
			    &stream_cksum, outfd) != 0)
				goto error;
			break;
		}

		case DRR_WRITE_EMBEDDED:
		{
			struct drr_write_embedded *drrwe =
			    &drr->drr_u.drr_write_embedded;
			VERIFY3S(begin, ==, 1);
			payload_size = P2ROUNDUP((uint64_t)drrwe->drr_psize, 8);
			(void) sfread(buf, payload_size, input);
			if (emit_record(drr, buf, payload_size, &stream_cksum, outfd, &stats) != 0) {
				goto error;
			}
			break;
		}

		case DRR_WRITE_BYREF:
			/*
			 * If we encounter a WRITE_BYREF in the input stream,
			 * pass it through unchanged. This is already a dedup
			 * reference.
			 */
			VERIFY3S(begin, ==, 1);
			if (emit_record(drr, NULL, 0, &stream_cksum, outfd, &stats) != 0) {
				goto error;
			}
			break;

		case DRR_FREEOBJECTS:
		case DRR_FREE:
		case DRR_OBJECT_RANGE:
			VERIFY3S(begin, ==, 1);
			if (emit_record(drr, NULL, 0, &stream_cksum, outfd, &stats) != 0) {
				goto error;
			}
			break;

		default:
			(void) fprintf(stderr, "INVALID record type 0x%x\n",
			    drr->drr_type);
			assert(false);
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
		print_status(&stats, true);
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
	fprintf(stderr, "\nError while writing output: %s\n", strerror(errno));
	free(buf);
	exit(1);
}

int
zstream_do_dedup(int argc, char *argv[])
{
	bool verbose = false;
	int mem_percent = DEFAULT_DEDUP_PHYSMEM_PERCENT;
	char *cache_dir = NULL;
	int input, output;
	int c;

	while ((c = getopt(argc, argv, "vm:c:")) != -1) {
		switch (c) {
		case 'v':
			verbose = true;
			break;
		case 'm':
			mem_percent = atoi(optarg);
			if (mem_percent <= 0 || mem_percent > 100) {
				fprintf(stderr,
				    "invalid memory percentage '%s'\n", optarg);
				exit(1);
			}
			break;
		case 'c':
			cache_dir = optarg;
			/* TODO */
			break;
		case '?':
			fprintf(stderr, "invalid option '%c'\n", optopt);
			zstream_usage();
			exit(1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 1) {
		(void) fprintf(stderr, "too many arguments\n");
		zstream_usage();
		exit(1);
	}

	if (isatty(STDOUT_FILENO)) {
		(void) fprintf(stderr,
		    "Error: Stream can not be written to a terminal.\n"
		    "You must redirect standard output.\n");
		exit(1);
	}

	/* If a filename is provided, open it; otherwise use stdin */
	if (argc == 1) {
		const char *filename = argv[0];
		input = open(filename, O_RDONLY);
		if (input < 0) {
			(void) fprintf(stderr, "Error while opening file '%s': %s\n",
			    filename, strerror(errno));
			exit(1);
		}
	} else if (isatty(STDIN_FILENO)) {
		(void) fprintf(stderr,
		    "Error: Stream can not be read from a terminal.\n"
		    "You must name a file or accept input from a pipe.\n");
		exit(1);
	} else {
		input = STDIN_FILENO;
	}

	/* Calculate maximum memory for dedup table */
	uint64_t max_memory;
#ifdef _ILP32
	max_memory = SMALLEST_REASONABLE_DEDUP_MB << 20;
#else
	uint64_t physbytes = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
	max_memory = MAX((physbytes * mem_percent) / 100,
	    SMALLEST_REASONABLE_DEDUP_MB << 20);
#endif

	linear_hash_t dedup_table;
	if (lh_init(&dedup_table, sizeof(dedup_entry_t), max_memory) < 0) {
		fprintf(stderr, "Unable to initialize dedup hash table, aborting...\n");
		exit(1);
	}
	zfs_dedup_stream(input, STDOUT_FILENO, &dedup_table, verbose);
}


