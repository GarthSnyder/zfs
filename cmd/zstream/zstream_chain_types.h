typedef struct {
	dmu_replay_record_t	dp_drr;
	uint8_t			*dp_payload;
	size_t			dp_payload_size;
	off_t			dp_stream_position;
} drr_packet_t;

typedef struct {
	drr_packet_t		dp_base;
	zio_cksum_t		dp_blake3_payload;
} drr_with_blake3_t;

typedef struct {
	drr_packet_t		dp_base;
	zio_cksum_t		dp_fletcher4_header;
	zio_cksum_t		dp_fletcher4_payload;
} drr_with_fletcher4_t;


