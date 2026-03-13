// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Portions Copyright 2012 Martin Matuska <martin@matuska.org>
 * Copyright (c) 2013, 2015 by Delphix. All rights reserved.
 * Portions copyright 2026 by Garth Snyder <garth@garthsnyder.com>
 */

#include <ctype.h>		/* isprint				*/
#include <libnvpair.h>		/* nvlist_print				*/
#include <stdint.h>		/* uint8_t, uint64_t, uint32_t		*/
#include <stdio.h>		/* printf, fprintf, perror...		*/
#include <string.h>		/* strerror				*/
#include <sys/param.h>		/* MIN					*/
#include <sys/nvpair.h>		/* nvlist_free, nvlist_unpack		*/
#include <sys/spa_checksum.h>	/* zio_cksum_t				*/
#include <sys/stdtypes.h>	/* B_TRUE, boolean_t, uint_t		*/
#include <sys/zio.h>		/* ZIO_DATA_IV_LEN...			*/
#include <sys/zfs_ioctl.h>	/* drr_object, dmu_replay_record...	*/
#include <unistd.h>		/* getopt, optind, optopt		*/

#include "zstream.h"		/* zstream_usage, zstream_do_dump	*/
#include "zstream_modules.h"	/* STANDARD_INPUT_STACK			*/

/*
 * If dump mode is enabled, the number of bytes to print per line
 */
#define	BYTES_PER_LINE	16
/*
 * If dump mode is enabled, the number of bytes to group together, separated
 * by newlines or spaces
 */
#define	DUMP_GROUPING	4

typedef struct {
	uint8_t drr_salt[ZIO_DATA_SALT_LEN];
	uint8_t drr_iv[ZIO_DATA_IV_LEN];
	uint8_t drr_mac[ZIO_DATA_MAC_LEN];
} crypto_fields_t;

typedef void dumper_f(drr_packet_t *item, chain_attrs_t *attrs);

typedef struct {
	const char	*rt_typename;
	dumper_f	*rt_dumper;
} record_type_t;

/*
 * Print part of a block in ASCII characters
 */
static void
print_ascii_block(uint8_t *subbuf, int length)
{
	int i;

	for (i = 0; i < length; i++) {
		char char_print = isprint(subbuf[i]) ? subbuf[i] : '.';
		if (i != 0 && i % DUMP_GROUPING == 0) {
			(void) printf(" ");
		}
		(void) printf("%c", char_print);
	}
	(void) printf("\n");
}

/*
 * print_block - Dump the contents of a modified block to STDOUT
 *
 * Assume that buf has capacity evenly divisible by BYTES_PER_LINE
 */
static void
print_block(uint8_t *buf, uint32_t length)
{
	int i;
	/*
	 * Start printing ASCII characters at a constant offset, after
	 * the hex prints. Leave 3 characters per byte on a line (2 digit
	 * hex number plus 1 space) plus spaces between characters and
	 * groupings.
	 */
	int ascii_start = BYTES_PER_LINE * 3 +
	    BYTES_PER_LINE / DUMP_GROUPING + 2;

	for (i = 0; i < length; i += BYTES_PER_LINE) {
		int j;
		int this_line_length = MIN(BYTES_PER_LINE, length - i);
		int print_offset = 0;

		for (j = 0; j < this_line_length; j++) {
			int buf_offset = i + j;

			/*
			 * Separate every DUMP_GROUPING bytes by a space.
			 */
			if (buf_offset % DUMP_GROUPING == 0) {
				print_offset += printf(" ");
			}

			/*
			 * Print the two-digit hex value for this byte.
			 */
			unsigned char hex_print = buf[buf_offset];
			print_offset += printf("%02x ", hex_print);
		}

		(void) printf("%*s", ascii_start - print_offset, " ");

		print_ascii_block(buf + i, this_line_length);
	}
}

/*
 * Print an array of bytes to stdout as hexadecimal characters. str must
 * have buf_len * 2 + 1 bytes of space.
 */
static void
sprintf_bytes(char *str, uint8_t *buf, uint_t buf_len)
{
	int i, n;

	for (i = 0; i < buf_len; i++) {
		n = sprintf(str, "%02x", buf[i] & 0xff);
		str += n;
	}

	str[0] = '\0';
}

static void
maybe_dump_payload(drr_packet_t *item, chain_attrs_t *attrs)
{
	if (OPTION_ENABLED(attrs, CA_DUMP_DATA)) {
		print_block(item->dp_payload, item->dp_payload_size);
	}
}

static char *
stringify_encryption_fields(void *crypto_in)
{
	crypto_fields_t *crypto = crypto_in;
	char salt[ZIO_DATA_SALT_LEN * 2 + 1];
	char iv[ZIO_DATA_IV_LEN * 2 + 1];
	char mac[ZIO_DATA_MAC_LEN * 2 + 1];
	static char buff[sizeof (salt) + sizeof (iv) + sizeof (mac) + 32];

	sprintf_bytes(salt, crypto->drr_salt, ZIO_DATA_SALT_LEN);
	sprintf_bytes(iv, crypto->drr_iv, ZIO_DATA_IV_LEN);
	sprintf_bytes(mac, crypto->drr_mac, ZIO_DATA_MAC_LEN);
	snprintf(buff, sizeof (buff), "salt = %s iv = %s mac = %s",
	    salt, iv, mac);
	return buff;
}

static void
dump_begin_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_begin *drrb = &item->dp_drr.drr_u.drr_begin;

	printf("BEGIN record\n");
	printf("\thdrtype = %llu\n",
	    DMU_GET_STREAM_HDRTYPE(drrb->drr_versioninfo));
	printf("\tfeatures = %llx\n",
	    DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo));
	printf("\tmagic = %zx\n", drrb->drr_magic);
	printf("\tcreation_time = %zx\n", drrb->drr_creation_time);
	printf("\ttype = %u\n", drrb->drr_type);
	printf("\tflags = 0x%x\n", drrb->drr_flags);
	printf("\ttoguid = %zx\n", drrb->drr_toguid);
	printf("\tfromguid = %zx\n", drrb->drr_fromguid);
	printf("\ttoname = %s\n", drrb->drr_toname);
	printf("\tpayloadlen = %u\n", drr->drr_payloadlen);

	if (OPTION_ENABLED(attrs, CA_VERBOSE))
		printf("\n");

	if (drr->drr_payloadlen != 0) {
		nvlist_t *nv;
		size_t sz = drr->drr_payloadlen;
		int err = nvlist_unpack((char *)item->dp_payload, sz, &nv, 0);
		if (err) {
			perror(strerror(err));
		} else {
			nvlist_print(stdout, nv);
			nvlist_free(nv);
		}
	}
}

static void
dump_end_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	(void) attrs;
	struct drr_end *drre = &item->dp_drr.drr_u.drr_end;

	printf("END checksum = %zx/%zx/%zx/%zx\n",
	    drre->drr_checksum.zc_word[0],
	    drre->drr_checksum.zc_word[1],
	    drre->drr_checksum.zc_word[2],
	    drre->drr_checksum.zc_word[3]);
}

static void
dump_object_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_object *drro = &item->dp_drr.drr_u.drr_object;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("OBJECT object = %zu type = %u "
		    "bonustype = %u blksz = %u bonuslen = %u "
		    "dn_slots = %u raw_bonuslen = %u "
		    "flags = %u maxblkid = %zu "
		    "indblkshift = %u nlevels = %u "
		    "nblkptr = %u\n",
		    drro->drr_object,
		    drro->drr_type,
		    drro->drr_bonustype,
		    drro->drr_blksz,
		    drro->drr_bonuslen,
		    drro->drr_dn_slots,
		    drro->drr_raw_bonuslen,
		    drro->drr_flags,
		    drro->drr_maxblkid,
		    drro->drr_indblkshift,
		    drro->drr_nlevels,
		    drro->drr_nblkptr);
	}
	if (drro->drr_bonuslen > 0) {
		maybe_dump_payload(item, attrs);
	}
}

static void
dump_freeobjects_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_freeobjects *drrfo = &item->dp_drr.drr_u.drr_freeobjects;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("FREEOBJECTS firstobj = %zu numobjs = %zu\n",
		    drrfo->drr_firstobj, drrfo->drr_numobjs);
	}
}

static void
dump_write_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_write *drrw = &item->dp_drr.drr_u.drr_write;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("WRITE object = %zu type = %u "
		    "checksum type = %u compression type = %u "
		    "flags = %u offset = %zu "
		    "logical_size = %zu "
		    "compressed_size = %zu "
		    "payload_size = %u props = %zx "
		    "%s\n",
		    drrw->drr_object,
		    drrw->drr_type,
		    drrw->drr_checksumtype,
		    drrw->drr_compressiontype,
		    drrw->drr_flags,
		    drrw->drr_offset,
		    drrw->drr_logical_size,
		    drrw->drr_compressed_size,
		    item->dp_payload_size,
		    drrw->drr_key.ddk_prop,
		    stringify_encryption_fields(&drrw->drr_salt));
	}
	maybe_dump_payload(item, attrs);
}

static void
dump_write_byref_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_write_byref *drrwbr = &item->dp_drr.drr_u.drr_write_byref;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("WRITE_BYREF object = %zu "
		    "checksum type = %u props = %zx "
		    "offset = %zu length = %zu "
		    "toguid = %zx refguid = %zx "
		    "refobject = %zu refoffset = %zu\n",
		    drrwbr->drr_object,
		    drrwbr->drr_checksumtype,
		    drrwbr->drr_key.ddk_prop,
		    drrwbr->drr_offset,
		    drrwbr->drr_length,
		    drrwbr->drr_toguid,
		    drrwbr->drr_refguid,
		    drrwbr->drr_refobject,
		    drrwbr->drr_refoffset);
	}
}

static void
dump_free_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_free *drrf = &item->dp_drr.drr_u.drr_free;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("FREE object = %zu "
		    "offset = %zu length = %zd\n",
		    drrf->drr_object,
		    drrf->drr_offset,
		    drrf->drr_length);
	}
}

static void
dump_spill_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_spill *drrs = &item->dp_drr.drr_u.drr_spill;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("SPILL block for object = %zu "
		    "length = %zu flags = %u "
		    "compression type = %u "
		    "compressed_size = %zu "
		    "payload_size = %u "
		    "%s\n",
		    drrs->drr_object,
		    drrs->drr_length,
		    drrs->drr_flags,
		    drrs->drr_compressiontype,
		    drrs->drr_compressed_size,
		    item->dp_payload_size,
		    stringify_encryption_fields(&drrs->drr_salt));
	}
	maybe_dump_payload(item, attrs);
}

static void
dump_write_embedded_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_write_embedded *drrwe =
	    &item->dp_drr.drr_u.drr_write_embedded;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("WRITE_EMBEDDED object = %zu "
		    "offset = %zu length = %zu "
		    "toguid = %zx comp = %u etype = %u "
		    "lsize = %u psize = %u\n",
		    drrwe->drr_object,
		    drrwe->drr_offset,
		    drrwe->drr_length,
		    drrwe->drr_toguid,
		    drrwe->drr_compression,
		    drrwe->drr_etype,
		    drrwe->drr_lsize,
		    drrwe->drr_psize);
	}
	maybe_dump_payload(item, attrs);
}

static void
dump_object_range_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_object_range *drror = &item->dp_drr.drr_u.drr_object_range;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("OBJECT_RANGE firstobj = %zu "
		    "numslots = %zu flags = %u "
		    "%s\n",
		    drror->drr_firstobj,
		    drror->drr_numslots,
		    drror->drr_flags,
		    stringify_encryption_fields(&drror->drr_salt));
	}
}

static void
dump_redact_record(drr_packet_t *item, chain_attrs_t *attrs)
{
	struct drr_redact *drrr = &item->dp_drr.drr_u.drr_redact;

	if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
		printf("REDACT object = %zu offset = "
		    "%zu length = %zu\n",
		    drrr->drr_object,
		    drrr->drr_offset,
		    drrr->drr_length);
	}
}

static boolean_t
chain_dump_record(drr_packet_t *item, record_type_t *context,
    chain_attrs_t *attrs)
{
	if (!item) {
		return (B_TRUE);
	}

	dmu_replay_record_t *drr = &item->dp_drr;
	zio_cksum_t *cksum = &drr->drr_u.drr_checksum.drr_checksum;
	int type = (int)drr->drr_type;

	context[type].rt_dumper(item, attrs);

	if (type != DRR_BEGIN && OPTION_ENABLED(attrs, CA_VERY_VERBOSE)) {
		printf("    checksum = %zx/%zx/%zx/%zx\n",
		    cksum->zc_word[0],
		    cksum->zc_word[1],
		    cksum->zc_word[2],
		    cksum->zc_word[3]);
	}

	return (B_TRUE);
}

static chain_step_t
serial_dump_records(record_type_t *context)
{
	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = context,
		.cs_serial = {
			.process = (zc_serial_process_f *)chain_dump_record
		}
	});
}

int
zstream_do_dump(int argc, char *argv[])
{
	chain_attrs_t attrs = {0};
	const char *input_file = NULL;
	int c;

	record_type_t record_types[DRR_NUMTYPES] = {
		{ "DRR_BEGIN", 		dump_begin_record },
		{ "DRR_OBJECT", 	dump_object_record },
		{ "DRR_FREEOBJECTS", 	dump_freeobjects_record },
		{ "DRR_WRITE", 		dump_write_record },
		{ "DRR_FREE", 		dump_free_record },
		{ "DRR_END", 		dump_end_record },
		{ "DRR_WRITE_BYREF", 	dump_write_byref_record },
		{ "DRR_SPILL", 		dump_spill_record },
		{ "DRR_WRITE_EMBEDDED",	dump_write_embedded_record },
		{ "DRR_OBJECT_RANGE",	dump_object_range_record },
		{ "DRR_REDACT",		dump_redact_record }
	};

	while ((c = getopt(argc, argv, ":vCd")) != -1) {
		switch (c) {
		case 'C':
			ENABLE_OPTION(&attrs, CA_IGNORE_CKSUMS);
			break;
		case 'v':
			if (OPTION_ENABLED(&attrs, CA_VERBOSE)) {
				ENABLE_OPTION(&attrs, CA_VERY_VERBOSE);
			} else {
				ENABLE_OPTION(&attrs, CA_VERBOSE);
			}
			break;
		case 'd':
			ENABLE_OPTION(&attrs, CA_VERBOSE);
			ENABLE_OPTION(&attrs, CA_VERY_VERBOSE);
			ENABLE_OPTION(&attrs, CA_DUMP_DATA);
			break;
		case ':':
			fprintf(stderr,
			    "missing argument for '%c' option\n", optopt);
			zstream_usage();
			break;
		case '?':
			fprintf(stderr, "invalid option '%c'\n", optopt);
			zstream_usage();
			break;
		}
	}

	if (argc > optind) {
		input_file = argv[optind];
	}

	zstream_chain_t dump_chain = {
		STANDARD_INPUT_STACK(input_file, 1024),
		serial_dump_records(record_types),
		chain_terminator()
	};

	zstream_chain_exec(dump_chain, &attrs);

	{
		/* Match previous order */
		int print_order[] = {
			DRR_BEGIN, DRR_END, DRR_OBJECT, DRR_FREEOBJECTS,
			DRR_WRITE, DRR_WRITE_BYREF, DRR_WRITE_EMBEDDED,
			DRR_FREE, DRR_SPILL, DRR_OBJECT_RANGE, DRR_REDACT
		};

		printf("SUMMARY:\n");
		for (int i = 0; i < DRR_NUMTYPES; i++) {
			int type = print_order[i];
			record_type_t *rec = &record_types[type];
			record_stats_t *stats = &attrs.ca_stats_in[type];
			printf("\tTotal %s records = %zd (%zu bytes)\n",
			    rec->rt_typename,
			    stats->rs_num_records,
			    stats->rs_total_payload_bytes);
		}

		uint64_t total_payload =
		    attrs.ca_totals_in.rs_total_payload_bytes;
		uint64_t total_header =
		    attrs.ca_totals_in.rs_total_header_bytes;

		printf("\tTotal records = %zu\n",
		    attrs.ca_totals_in.rs_num_records);
		printf("\tTotal payload size = %zu (0x%zx)\n",
		    total_payload, total_payload);
		printf("\tTotal header overhead = %zu (0x%zx)\n",
		    total_header, total_header);
		printf("\tTotal stream length = %zu (0x%zx)\n",
		    total_header + total_payload, total_header + total_payload);
	}
	return (0);
}
