
struct drr_work_unit {
	dmu_replay_record_t	drr;
	uint8_t				*payload;	
	uint64_t			payload_size;
	zio_checksum_t 		blake3;
	uint64_t			sequence_num;
}
