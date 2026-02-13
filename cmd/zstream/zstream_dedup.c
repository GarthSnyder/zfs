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
#include <threads.h>
#include <sys/blake3.h>
#include <sys/ddt.h>
#include <sys/debug.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/zfs_ioctl.h>
#include "zfs_fletcher.h"
#include "zstream.h"
#include "linear_hash.h"
#include "linear_hash_stats.h"
#include "zstream_shared.h"
#include "team.h"
#include "zstream_stream.h"

#define	DEFAULT_DEDUP_PHYSMEM_PERCENT	30
#define SMALLEST_REASONABLE_DEDUP_MB	128
#define	STATUS_UPDATE_INTERVAL			(5ULL * NANOSEC)

#define BLAKE3_64_BIT(full_hash) (*((uint64_t *)full_hash))

typedef struct drr_write drr_write;

typedef struct {
	drr_write		write_block;
	zio_cksum_t		checksum;  
	zio_cksum_t		blake3_hash;
	uint64_t		payload_length;
} dedup_entry;

typedef struct dedup_stats {
	uint64_t	total_records;
	uint64_t	write_records;
	uint64_t	dedup_records;
	uint64_t	bytes_read;
	uint64_t	bytes_saved;
	uint64_t	disqualified_records;
	hrtime_t	last_status_time;
} dedup_stats;

typedef struct {
	dmu_replay_record_t	drr;
	uint8_t			*payload;
	uint64_t		payload_size;
	zio_cksum_t		blake3_hash;
} blake3_queue_item;

typedef struct {
	dmu_replay_record_t	drr;
	uint8_t			*payload;
	uint64_t		payload_size;
	zio_cksum_t		fletcher4_record;
	zio_cksum_t		fletcher4_payload;
} fletcher4_queue_item;

/*
 * Print status update to stderr.
 */
static void
print_status(dedup_stats *stats, boolean_t force)
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

	fprintf(stderr, "\r%lu total blocks, %llu writes, %llu deduped | "
	    "%sB read / %sB saved (%.1f%%)    \n",
	    stats->total_records,
	    (unsigned long long)stats->write_records,
	    (unsigned long long)stats->dedup_records,
	    bytes_read_str, bytes_saved_str, saved_pct);
	fflush(stderr);

	stats->last_status_time = now;
}

static bool
writes_compatible(const drr_write *this, const drr_write *prev) {
	if (this->drr_type != prev->drr_type
		|| this->drr_logical_size != prev->drr_logical_size
		|| this->drr_compressiontype != prev->drr_compressiontype
		|| this->drr_compressed_size != prev->drr_compressed_size
		|| memcmp(this->drr_salt, prev->drr_salt, sizeof(this->drr_salt))
		|| memcmp(this->drr_iv, prev->drr_iv, sizeof(this->drr_iv))
		|| memcmp(this->drr_mac, prev->drr_mac, sizeof(this->drr_mac)))
	{
		return false;
	}
	return true;
}

static bool
dedup_table_lookup(void *ddt, zio_cksum_t *blake3, dedup_entry *dde) {
	void *iter = lh_initiate_retrieve(ddt, BLAKE3_64_BIT(blake3));
	/* 
	 * Dedup table hashes are only 64 bits, so a table match does not
	 * guarantee an actual BLAKE3 match.
	 */
	while (lh_retrieve_next(iter, dde)) {
		if (ZIO_CHECKSUM_EQUAL(dde->blake3_hash, *blake3)) {
			return true;
		}
	}
	return false;
}

static void
dedup_table_insert(void *ddt, zio_cksum_t *blake3, dmu_replay_record_t *drr)
{
	dedup_entry dedup;
	memcpy(&dedup.write_block, &drr->drr_u.drr_write,
		sizeof(drr->drr_u.drr_write));
	memcpy(&dedup.blake3_hash, blake3, sizeof(dedup.blake3_hash));
	memcpy(&dedup.checksum, &drr->drr_u.drr_checksum.drr_checksum,
		sizeof(dedup.checksum));
	dedup.payload_length = DRR_WRITE_PAYLOAD_SIZE(&drr->drr_u.drr_write);
	lh_insert(ddt, BLAKE3_64_BIT(blake3), &dedup);
}

/*
 * Deduplicate a ZFS stream.
 */
static void
zfs_dedup_stream(void *team, int outfd, void *ddt, bool verbose)
{
	work_unit_t				unit;
	dmu_replay_record_t		*drr = &unit.drr;
	zio_cksum_t 			stream_cksum;
	dedup_stats 			stats;
	dedup_entry			existing;

	struct drr_begin 		*drrb = &drr->drr_u.drr_begin;
	struct drr_end			*drre = &drr->drr_u.drr_end;
	struct drr_write		*drrw = &drr->drr_u.drr_write;
	struct drr_checksum		*drrc = &drr->drr_u.drr_checksum;

	memset(&stats, 0, sizeof (stats));
	stats.last_status_time = gethrtime();

	while (team_dequeue(team, &unit) == 0)
	{
		ZIO_SET_CHECKSUM(&drrc->drr_checksum, 0, 0, 0, 0);
		stats.bytes_read += sizeof(*drr) + unit.payload_size;

		switch (drr->drr_type) {

		case DRR_BEGIN:
			uint32_t fflags;
			ZIO_SET_CHECKSUM(&stream_cksum, 0, 0, 0, 0);
			fflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
			if (fflags & DMU_BACKUP_FEATURE_DEDUP) {
				fprintf(stderr, "Input stream is already deduplicated.\n"
					"To re-deduplicate, pipe through zstream redup first.\n");
				exit(1);
			}
			fflags |= DMU_BACKUP_FEATURE_DEDUP;
			/* cppcheck-suppress syntaxError */
			DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, fflags);
			break;

		case DRR_END:
			drre->drr_checksum = stream_cksum;
			break;

		case DRR_WRITE:
			stats.write_records++;
			/* Check if we've seen this block before */
			zio_cksum_t *blake3 = (zio_cksum_t *)unit.output;
			if (dedup_table_lookup(ddt, blake3, &existing)) {
				if (!writes_compatible(drrw, &existing.write_block)) {
					stats.disqualified_records++;
				} else {
					struct drr_write_byref byref;
					memset(&byref, 0, sizeof(byref));

					byref.drr_refguid = existing.write_block.drr_toguid;
					byref.drr_refobject = existing.write_block.drr_object;
					byref.drr_refoffset = existing.write_block.drr_offset;

					byref.drr_object = drrw->drr_object;
					byref.drr_offset = drrw->drr_offset;
					byref.drr_toguid = drrw->drr_toguid;

					byref.drr_length = drrw->drr_logical_size;
					byref.drr_checksumtype = drrw->drr_checksumtype;
					byref.drr_flags = drrw->drr_flags;
					byref.drr_key = drrw->drr_key;

					memset(drr, 0, sizeof(*drr));
					drr->drr_u.drr_write_byref = byref;
					drr->drr_type = DRR_WRITE_BYREF;
					drr->drr_payloadlen = 0;

					stats.dedup_records++;
					stats.bytes_saved += unit.payload_size;

					if (unit.payload_size) {
						free(unit.payload);
					}
					unit.payload = NULL;
					unit.payload_size = 0;
				}
			} else {
				/* First occurrence, insert into table and write as-is */
				dedup_table_insert(ddt, blake3, drr);
			}
			free(blake3);
			break;

		default:
			/* Pass through all other record types unchanged */
			break;
		}

		stats.total_records += 1;
		dump_record(drr, unit.payload, unit.payload_size, &stream_cksum, outfd);
		if (unit.payload_size) {
			free(unit.payload);
		}
		if (verbose) {
			print_status(&stats, false);
		}
	}

	if (verbose) {
		print_status(&stats, true);
		fprintf(stderr, "\n");
		char mem_str[32];
		zfs_nicenum(lh_get_mem_highwater(ddt), mem_str, sizeof (mem_str));
		fprintf(stderr,
		    "Processed %llu total records, including %llu write "
		    "records.\n",
		    (unsigned long long)stats.total_records,
		    (unsigned long long)stats.write_records);
		fprintf(stderr,
		    "Deduplicated %llu blocks, using %sB memory.\n",
		    (unsigned long long)stats.dedup_records, mem_str);
		if (stats.disqualified_records) {
			fprintf(stderr, "%lu write records were exempt from deduplication\n",
				stats.disqualified_records);
		}
		fprintf(stderr, "\n");
		lh_print_stats(ddt);
	}
}

/* Callback that enqueues records on the team */
static void
enqueue_record(dmu_replay_record_t *drr, uint8_t **payload,
	uint32_t *payload_size, void *context)
{
	work_unit_t unit;

	unit.drr = *drr;
	unit.payload = *payload;
	unit.payload_size = *payload_size;
	unit.output = NULL;
	unit.needs_processing = drr->drr_type == DRR_WRITE;

	team_enqueue(context, &unit);
}

/* This is the packet passed to the feeder thread */
typedef struct {
	void	 	*team;
	FILE		*input;
} dedup_context_t;

/* This is the feeder thread function */
static int
enqueue_stream(void *context_in) {
	dedup_context_t *context = (dedup_context_t *)context_in;
	stream_filter_t filter;
	memset(&filter, 0, sizeof(filter));
	filter.all_records_post = enqueue_record;
	read_stream(context->input, -1, &filter, 1, B_TRUE, context->team);
	team_fini(context->team);
	return 0;
}

/* This is the team worker function */
static void
calculate_blake3_hash(work_unit_t *unit)
{
	zio_cksum_t *checksum;
    BLAKE3_CTX ctx;

    if (!(checksum = malloc(sizeof(zio_cksum_t)))) {
    	fprintf(stderr, "Couldn't allocate checksum buffer\n");
    	exit(1);
    }
    Blake3_Init(&ctx);
    Blake3_Update(&ctx, unit->payload, unit->payload_size);
    Blake3_Final(&ctx, (uint8_t *)checksum);
    unit->output = checksum;
}

int
zstream_do_dedup(int argc, char *argv[])
{
	bool 			verbose = false;
	int 			mem_percent = DEFAULT_DEDUP_PHYSMEM_PERCENT;
	const char 		*cache_dir = NULL;
	FILE			*input;
	int 			c;
	dedup_context_t	feeder_context;
	void			*hasher_team;
	thrd_t			feeder_thread;
	void			*dedup_table;

	while ((c = getopt(argc, argv, "vm:c:")) != -1) {
		switch (c) {
		case 'v':
			verbose = true;
			break;
		case 'm':
			mem_percent = atoi(optarg);
			if (mem_percent < 0 || mem_percent > 100) {
				fprintf(stderr,
				    "invalid memory percentage '%s'\n", optarg);
				exit(1);
			}
			break;
		case 'c':
			cache_dir = optarg;
			(void) cache_dir; /* TODO: implement cache_dir */
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

	/* Calculate maximum memory for dedup table */
	uint64_t max_memory;
#ifdef _ILP32
	max_memory = SMALLEST_REASONABLE_DEDUP_MB << 20;
#else
	uint64_t physbytes = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
	max_memory = MAX((physbytes * mem_percent) / 100,
	    SMALLEST_REASONABLE_DEDUP_MB << 20);
#endif

	cache_dir = cache_dir ? cache_dir : "/tmp";
	struct stat statbuff;
	if (stat(cache_dir, &statbuff) < 0) {
		fprintf(stderr, "Cache directory %s does not exist.\n", cache_dir);
		exit(1);
	} else if ((statbuff.st_mode & S_IFMT) != S_IFDIR) {
		fprintf(stderr, "The -c argument requires a directory argument\n");
		exit(1);
	}
	dedup_table = lh_init(sizeof(dedup_entry), max_memory, cache_dir);

	if (isatty(STDOUT_FILENO)) {
		(void) fprintf(stderr,
		    "Error: Stream can not be written to a terminal.\n"
		    "You must redirect standard output.\n");
		exit(1);
	}

	/* If a filename is provided, open it; otherwise use stdin */
	if (argc == 1) {
		input = fopen(argv[0], "r");
		if (!input) {
			(void) fprintf(stderr, "Error while opening file '%s': %s\n",
			    argv[0], strerror(errno));
			exit(1);
		}
	} else if (isatty(STDIN_FILENO)) {
		(void) fprintf(stderr,
		    "Error: Stream can not be read from a terminal.\n"
		    "You must name a file or accept input from a pipe.\n");
		exit(1);
	} else {
		input = stdin;
	}

	int num_threads = sysconf(_SC_NPROCESSORS_ONLN);
	hasher_team = team_init(calculate_blake3_hash, 64 * 1024, 256,
		(num_threads < 6) ? 6 : num_threads);
	feeder_context.team = hasher_team;
	feeder_context.input = input;

	if (thrd_create(&feeder_thread, enqueue_stream, &feeder_context)
		!= thrd_success)
	{
		fprintf(stderr, "Error creating feeder thread\n");
		exit(1);
	}

	fletcher_4_init();
	zfs_dedup_stream(hasher_team, STDOUT_FILENO, dedup_table, verbose);
	thrd_join(feeder_thread, NULL);
	fletcher_4_fini();
	lh_destroy(dedup_table);
	return 0;
}


