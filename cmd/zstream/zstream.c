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
 * Copyright (c) 2020 by Delphix. All rights reserved.
 * Copyright (c) 2020 by Datto Inc. All rights reserved.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <stddef.h>
#include <libzfs.h>
#include <libspl.h>
#include "zstream.h"

void
zstream_usage(void)
{
	(void) fprintf(stderr,
	    "usage: zstream command args ...\n"
	    "Available commands are:\n"
	    "\n"
	    "    zstream dump [-vCd] [FILE]\n"
	    "         -v  Verbose. Print metadata for each record\n"
	    "         -C  Suppress checksum validation\n"
	    "         -d  Dump data for each record (implies -v)\n"
	    "       FILE  Dump contents of FILE, otherwise stdin\n"
	    "\n"
	    "    zstream decompress [-v] [OBJECT,OFFSET[,TYPE]] ...\n"
	    "         -v  Verbose.  Print summary of decompressed records\n"
	    "     OBJECT  Object number to decompress\n"
	    "     OFFSET  Offset within the object's data stream\n"
	    "       TYPE  Compression type as specified in 'zfs set compression'\n"
	    "\n"
	    "    zstream recompress [-l level] TYPE\n"
	    "         -l  Compression level for types that support it (e.g., zstd)\n"
	    "       TYPE  Compression type as specified in 'zfs set compression'\n"
	    "\n"
	    "    zstream token TOKEN\n"
	    "      TOKEN  Resume token to be analyzed\n"
	    "\n"
	    "    zstream redup [-v] FILE | ...\n"
	    "         -v  Verbose. Print a summary of converted records\n"
	    "\n"
	    "    zstream dedup [-v] [-m PCT_MEM] [-c CACHEDIR] [FILE]\n"
	    "         -v  Verbose. Print periodic status updates\n"
	    "         -m  Specify the percent of system RAM zstream may use (default 30)\n"
	    "         -c  Cache directory to use if RAM is insufficient (default /tmp)\n"
	    "       FILE  Stream file to dedup to stdout (default stdin)\n"
	    "\n"
	);
	exit(1);
}

int
main(int argc, char *argv[])
{
	char *basename = strrchr(argv[0], '/');
	basename = basename ? (basename + 1) : argv[0];
	if (argc >= 1 && strcmp(basename, "zstreamdump") == 0)
		return (zstream_do_dump(argc, argv));

	if (argc < 2)
		zstream_usage();

	char *subcommand = argv[1];

	if (strcmp(subcommand, "dump") == 0) {
		return (zstream_do_dump(argc - 1, argv + 1));
	} else if (strcmp(subcommand, "decompress") == 0) {
		return (zstream_do_decompress(argc - 1, argv + 1));
	} else if (strcmp(subcommand, "recompress") == 0) {
		return (zstream_do_recompress(argc - 1, argv + 1));
	} else if (strcmp(subcommand, "token") == 0) {
		return (zstream_do_token(argc - 1, argv + 1));
	} else if (strcmp(subcommand, "redup") == 0) {
		return (zstream_do_redup(argc - 1, argv + 1));
	} else if (strcmp(subcommand, "dedup") == 0) {
		return (zstream_do_dedup(argc - 1, argv + 1));
	} else {
		zstream_usage();
	}
}
