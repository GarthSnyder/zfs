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
#include "zstream_dedup.h"
#include "linear_hash.h"
#include "linear_hash_stats.h"
#include "zstream_fletcher4.h"
#include "zstream_io.h"
#include "zstream_shared.h"
#include "zstream_blake3.h"

#define	DEFAULT_DEDUP_PHYSMEM_PERCENT	30
#define SMALLEST_REASONABLE_DEDUP_MB	128
#define	STATUS_UPDATE_INTERVAL			(5ULL * NANOSEC)

#define BLAKE3_64_BIT(full_hash) (*((uint64_t *)full_hash))

typedef struct drr_write drr_write_t;

typedef struct {
	drr_write_t		write_block;
	zio_cksum_t		blake3_hash;
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

typedef struct {
	linear_hash_t	dc_table;
	dedup_stats_t	dc_stats;
} dedup_context_t;

static boolean_t
writes_compatible(const drr_write_t *this, const drr_write_t *prev) {
	if (this->drr_type != prev->drr_type
		|| this->drr_logical_size != prev->drr_logical_size
		|| this->drr_compressiontype != prev->drr_compressiontype
		|| this->drr_compressed_size != prev->drr_compressed_size
		|| memcmp(this->drr_salt, prev->drr_salt, sizeof(this->drr_salt))
		|| memcmp(this->drr_iv, prev->drr_iv, sizeof(this->drr_iv))
		|| memcmp(this->drr_mac, prev->drr_mac, sizeof(this->drr_mac)))
	{
		return B_FALSE;
	}
	return B_TRUE;
}

static boolean_t
dedup_table_lookup(linear_hash_t ddt, zio_cksum_t *blake3, dedup_entry_t *dde) {
	lh_iterator_t iter = lh_initiate_retrieve(ddt, BLAKE3_64_BIT(blake3));
	/* 
	 * linear_hash hashes are only 64 bits, so a table match does not
	 * guarantee an actual BLAKE3 match.
	 */
	while (lh_retrieve_next(iter, dde)) {
		if (ZIO_CHECKSUM_EQUAL(dde->blake3_hash, *blake3)) {
			return B_TRUE;
		}
	}
	return B_FALSE;
}

static void
dedup_table_insert(linear_hash_t ddt, zio_cksum_t *blake3,
	dmu_replay_record_t *drr)
{
	dedup_entry_t dedup = {
		.write_block = drr->drr_u.drr_write,
		.blake3_hash = *blake3,
		.payload_length = DRR_WRITE_PAYLOAD_SIZE(&drr->drr_u.drr_write)
	};
	lh_insert(ddt, BLAKE3_64_BIT(blake3), &dedup);
}

static void
maybe_print_update(dedup_context_t *context, boolean_t force)
{
	dedup_stats_t *stats = &context->dc_stats;
	hrtime_t now = gethrtime();
	char bytes_read_str[32];
	char bytes_saved_str[32];
	double saved_pct;

	if (!force && now - stats->last_status_time < STATUS_UPDATE_INTERVAL) {
		return;
	}

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

static void
print_summary(dedup_context_t *context) {
	dedup_stats_t *stats = &context->dc_stats;
	maybe_print_update(context, B_TRUE);
	fprintf(stderr, "\n");
	char mem_str[32];
	zfs_nicenum(lh_get_mem_highwater(context->dc_table), mem_str,
		sizeof (mem_str));
	fprintf(stderr,
	    "Processed %llu total records, including %llu write "
	    "records.\n",
	    (unsigned long long)stats->total_records,
	    (unsigned long long)stats->write_records);
	fprintf(stderr,
	    "Deduplicated %llu blocks, using %sB memory.\n",
	    (unsigned long long)stats->dedup_records, mem_str);
	if (stats->disqualified_records) {
		fprintf(stderr, "%lu write records were exempt from deduplication\n",
			stats->disqualified_records);
	}
	fprintf(stderr, "\n");
	lh_print_stats(context->dc_table);
}

static boolean_t
chain_dedup_writes(drr_blake3_t *item, dedup_context_t *context,
	chain_attrs_t chain)
{
	dmu_replay_record_t	*drr = &item->dp_base.dp_drr;
	struct drr_write	*drrw = &drr->drr_u.drr_write;
	dedup_stats_t		*stats = &context->dc_stats;
	zio_cksum_t			*blake3 = &item->dp_blake3_payload;
	linear_hash_t		dd_table = context->dc_table;
	dedup_entry_t		existing;

	if (item == NULL && (chain->ca_flags & CA_VERBOSE)) {
		print_summary(context);
		return B_TRUE;
	}

	stats->total_records++;
	stats->bytes_read += sizeof(*drr) + item->dp_base.dp_payload_size;
	stats->last_status_time = gethrtime();

	if (drr->drr_type != DRR_WRITE) {
		return B_TRUE;
	}

	stats->write_records++;
	if (!dedup_table_lookup(dd_table, blake3, &existing)) {
		dedup_table_insert(dd_table, blake3, drr);
		return B_TRUE;
	}
	if (!writes_compatible(drrw, &existing.write_block)) {
		stats->disqualified_records++;
		return B_TRUE;
	}

	struct drr_write_byref byref = {
		.drr_refguid = existing.write_block.drr_toguid,
		.drr_refobject = existing.write_block.drr_object,
		.drr_refoffset = existing.write_block.drr_offset,
		.drr_object = drrw->drr_object,
		.drr_offset = drrw->drr_offset,
		.drr_toguid = drrw->drr_toguid,
		.drr_length = drrw->drr_logical_size,
		.drr_checksumtype = drrw->drr_checksumtype,
		.drr_flags = drrw->drr_flags,
		.drr_key = drrw->drr_key
	};

	memset(drr, 0, sizeof(*drr));
	drr->drr_u.drr_write_byref = byref;
	drr->drr_type = DRR_WRITE_BYREF;
	drr->drr_payloadlen = 0;

	stats->dedup_records++;
	stats->bytes_saved += item->dp_base.dp_payload_size;

	if (item->dp_base.dp_payload_size) {
		free(item->dp_base.dp_payload);
	}
	item->dp_base.dp_payload = NULL;
	item->dp_base.dp_payload_size = 0;

	if (chain->ca_flags & CA_VERBOSE) {
		maybe_print_update(context, B_FALSE);
	}
	return B_TRUE;
}

static chain_step_t
serial_dedup_writes(linear_hash_t dedup_table) {
	static dedup_context_t context = {};
	context.dc_table = dedup_table;
	return (chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_out_size = sizeof(drr_packet_t),
		.serial = {
			.css_process = (zc_serial_process_f *)chain_dedup_writes,
			.css_context = &context
		}
	};
}

int
zstream_do_dedup(int argc, char *argv[])
{
	struct chain_attrs	attrs = {};
	int 				mem_percent = DEFAULT_DEDUP_PHYSMEM_PERCENT;
	const char 			*cache_dir = NULL;
	FILE				*input;
	int 				c;
	linear_hash_t		dedup_table;

	while ((c = getopt(argc, argv, "vm:c:")) != -1) {
		switch (c) {
		case 'v':
			attrs.ca_flags |= CA_VERBOSE;
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
		fprintf(stderr, "The -c flag requires a directory argument\n");
		exit(1);
	}
	dedup_table = lh_init(sizeof(dedup_entry_t), max_memory, cache_dir);

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

	zstream_chain_t dedup_chain = {
		serial_read_stream((argc == 1) ? argv[0] : NULL),
		parallel_calc_fletcher4(),
		serial_validate_fletcher4(),
		parallel_calc_blake3(),
		serial_dedup_writes(dedup_table),
		parallel_calc_fletcher4(),
		serial_add_fletcher4(),
		serial_write_stream(NULL)
	};

	// serialize_chains = B_TRUE;
	zstream_chain_exec(dedup_chain, &attrs,
		sizeof(dedup_chain) / sizeof(chain_step_t));
	lh_destroy(dedup_table);
	return 0;
}


