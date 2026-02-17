zstream_chain_t chain = {
	{ chain_read_file(fp),		&info,		sizeof(drr_record),		INPUT 		},
	{ chain_precalc_cksums, 	&info, 		sizeof(drr_with_checksums),	PARALLEL	},
	{ chain_validate_cksums, 	&cc_input,	sizeof(drr_record),		SERIAL		},
	{ chain_byteswap,		&info,		sizeof(drr_record),		PARALLEL	},
	{ chain_validate_records,	&info,		sizeof(drr_record),		PARALLEL	},
	{ chain_add_blake3,		&info,		sizeof(drr_with_blake3),	PARALLEL	},
	{ chain_dedup_writes,		&dc_dedup,	sizeof(drr_record),		SERIAL		},
	{ chain_precalc_cksums,		&info,		sizeof(drr_with_checksums),	PARALLEL	},
	{ chain_finalize_cksums,	&cc_output,	sizeof(drr_record),		SERIAL		},
	{ chain_write_file(fd),		&info,		sizeof(drr_record),		OUTPUT		}
}

