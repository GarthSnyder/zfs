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

#include <libzfs.h>
#include <libzutil.h>
#include <libspl.h>

#include "linear_hash_stats.h"
#include "zstream.h"
#include "zstream_modules.h"

#define	DEFAULT_DEDUP_PHYSMEM_PERCENT	30
#define SMALLEST_REASONABLE_DEDUP_MB	128
#define	STATUS_UPDATE_INTERVAL		(5ULL * NANOSEC)

#define BLAKE3_64_BIT(full_hash) ((full_hash)->zc_word[0])

typedef struct drr_write drr_write_t;

typedef struct {
	drr_write_t	write_block;
	zio_cksum_t	blake3_hash;
	uint64_t	payload_length;
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

typedef long long unsigned int llu;

/*
 * The goal is that "zstream dedup | zstream redup" yields an identical
 * stream. There are a few odd cases that make this not work unless
 * deduplication is implemented with more aggressive changes to zstream
 * redup. That's probably possible, but it would need more intensive review.
 * For now, punt on deduplicating any record pairs that look "weird."
 */
static boolean_t
writes_compatible(const drr_write_t *cur, const drr_write_t *prev)
{
	return !(cur->drr_type 		 != prev->drr_type ||
		cur->drr_logical_size 	 != prev->drr_logical_size ||
		cur->drr_compressiontype != prev->drr_compressiontype ||
		cur->drr_compressed_size != prev->drr_compressed_size ||

		memcmp(cur->drr_salt, prev->drr_salt, sizeof(cur->drr_salt)) ||
		memcmp(cur->drr_iv, prev->drr_iv, sizeof(cur->drr_iv)) ||
		memcmp(cur->drr_mac, prev->drr_mac, sizeof(cur->drr_mac)));
}

static boolean_t
dedup_table_lookup(linear_hash_t ddt, drr_blake3_t *item, dedup_entry_t *dde)
{
	zio_cksum_t *long_hash = &item->dp_blake3_payload;
	uint64_t short_hash = BLAKE3_64_BIT(long_hash);
	lh_iterator_t iter = lh_initiate_retrieve(ddt, short_hash);
	while (lh_retrieve_next(iter, dde)) {
		if (ZIO_CHECKSUM_EQUAL(dde->blake3_hash, *long_hash)) {
			return B_TRUE;
		}
	}
	return B_FALSE;
}

static void
dedup_table_insert(linear_hash_t ddt, drr_blake3_t *item)
{
	dedup_entry_t dedup = {
		.write_block = item->dp_base.dp_drr.drr_u.drr_write,
		.blake3_hash = item->dp_blake3_payload,
		.payload_length = item->dp_base.dp_payload_size
	};
	lh_insert(ddt, BLAKE3_64_BIT(&item->dp_blake3_payload), &dedup);
}

static void
maybe_print_update(dedup_context_t *context, boolean_t force)
{
	dedup_stats_t *stats = &context->dc_stats;
	hrtime_t now = getlrtime();
	char bytes_read_str[32];
	char bytes_saved_str[32];
	double saved_pct;

	if (!force && now - stats->last_status_time < STATUS_UPDATE_INTERVAL) {
		return;
	}

	zfs_nicenum(stats->bytes_read, bytes_read_str, sizeof (bytes_read_str));
	zfs_nicenum(stats->bytes_saved, bytes_saved_str,
		sizeof (bytes_saved_str));
	saved_pct = !stats->bytes_read ? 0 :
		stats->bytes_saved * 100.0 / stats->bytes_read;

	fprintf(stderr, "\r%llu total blocks, %llu writes, %llu deduped | "
		"%sB read / %sB saved (%.1f%%)    \n",
		(llu)stats->total_records, (llu)stats->write_records,
		(llu)stats->dedup_records, bytes_read_str, bytes_saved_str,
		saved_pct);

	stats->last_status_time = now;
}

/*
 * The "exempt" records are those that didn't pass writes_compatible(). This
 * probably isn't the best term, but I didn't want it to sound like a problem was
 * being reported.
 */
static void
print_summary(dedup_context_t *context)
{
	char mem_str[32];
	dedup_stats_t *stats = &context->dc_stats;
	uint64_t mem_highwater = lh_get_mem_highwater(context->dc_table);

	maybe_print_update(context, B_TRUE);
	zfs_nicenum(mem_highwater, mem_str, sizeof (mem_str));
	if (stats->disqualified_records) {
		fprintf(stderr, "%llu write records were exempt from "
			"deduplication\n", (llu)stats->disqualified_records);
	}
#ifdef DEBUG
	fprintf(stderr, "Used %sB of hash table memory.\n\n", mem_str);
	lh_print_stats(context->dc_table);
#endif
}

static boolean_t
chain_dedup_writes(drr_blake3_t *item, dedup_context_t *context,
	chain_attrs_t chain)
{
	dmu_replay_record_t *drr = &item->dp_base.dp_drr;
	struct drr_write *drrw   = &drr->drr_u.drr_write;
	struct drr_begin *drrb   = &drr->drr_u.drr_begin;
	dedup_stats_t *stats     = &context->dc_stats;
	linear_hash_t dd_table   = context->dc_table;
	dedup_entry_t existing;

	if (item == NULL) {
		if (chain->ca_flags & CA_VERBOSE) { print_summary(context); }
		return B_TRUE;
	}

	stats->total_records++;
	stats->bytes_read += sizeof(*drr) + item->dp_base.dp_payload_size;

	if (drr->drr_type == DRR_BEGIN) {
		/* Set the DEDUP feature flag for this stream */
		uint64_t fflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
		fflags |= DMU_BACKUP_FEATURE_DEDUP;
		fflags |= DMU_BACKUP_FEATURE_DEDUPPROPS;
		/* cppcheck-suppress syntaxError */
		DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, fflags);
		return B_TRUE;
	}

	if (drr->drr_type != DRR_WRITE) {
		return B_TRUE;
	}
	stats->write_records++;

	if (!dedup_table_lookup(dd_table, item, &existing)) {
		dedup_table_insert(dd_table, item);
		return B_TRUE;
	}
	if (!writes_compatible(drrw, &existing.write_block)) {
		stats->disqualified_records++;
		return B_TRUE;
	}

	struct drr_write_byref byref = {
		.drr_refguid	= existing.write_block.drr_toguid,
		.drr_refobject	= existing.write_block.drr_object,
		.drr_refoffset	= existing.write_block.drr_offset,
		.drr_object	= drrw->drr_object,
		.drr_offset	= drrw->drr_offset,
		.drr_toguid	= drrw->drr_toguid,
		.drr_length	= drrw->drr_logical_size,
		.drr_checksumtype = drrw->drr_checksumtype,
		.drr_flags	= drrw->drr_flags,
		.drr_key	= drrw->drr_key
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
serial_dedup_writes(linear_hash_t dedup_table)
{
	static dedup_context_t context = {};
	context.dc_table = dedup_table;
	context.dc_stats.last_status_time = getlrtime();
	return (chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof(drr_blake3_t),
		.cs_out_size = sizeof(drr_packet_t),
		.serial = {
			.css_process = (zc_serial_process_f *)chain_dedup_writes,
			.css_context = &context
		}
	};
}

static void
validate_cache_dir(const char *cache_dir) {
	struct stat statbuff;
	if (stat(cache_dir, &statbuff) < 0) {
		fprintf(stderr, "Directory %s does not exist.\n", cache_dir);
		exit(1);
	} else if ((statbuff.st_mode & S_IFMT) != S_IFDIR) {
		fprintf(stderr, "The -c flag requires a directory argument\n");
		exit(1);
	}
}

int
zstream_do_dedup(int argc, char *argv[])
{
	const char *cache_dir = NULL;
	struct chain_attrs attrs = {};
	int mem_percent = DEFAULT_DEDUP_PHYSMEM_PERCENT;
	int c;
	linear_hash_t dedup_table;

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

	libspl_init();

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
	validate_cache_dir(cache_dir);

	dedup_table = lh_init(sizeof(dedup_entry_t), max_memory, cache_dir);
	verify(dedup_table != NULL);

	zstream_chain_t dedup_chain = {
		serial_read_stream((argc == 1) ? argv[0] : NULL),
		parallel_calc_fletcher4(),
		serial_validate_fletcher4(),
		serial_byteswap(),
		parallel_validate_records(),
		parallel_calc_blake3(),
		serial_dedup_writes(dedup_table),
		parallel_calc_fletcher4(),
		serial_add_fletcher4(),
		serial_write_stream(NULL)
	};

	zstream_chain_exec(dedup_chain, &attrs,
		sizeof(dedup_chain) / sizeof(chain_step_t));
	lh_destroy(dedup_table);
	return 0;
}


