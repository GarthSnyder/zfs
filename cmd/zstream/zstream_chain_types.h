

typedef struct {
	dmu_replay_record_t	dp_drr;
	uint8_t			*dp_payload;
	uint32_t		dp_payload_size;
	off_t			dp_stream_offset;
} drr_packet_t;

typedef struct {
	drr_packet_t		dp_base;
	zio_cksum_t		dp_blake3_payload;
} drr_blake3_t;

/*
 * Fletcher 4 incremental blocks are limited to 8MB in size, and some ZFS
 * payloads can be significantly larger than this, notably DRR_WRITE blocks
 * and DRR_BEGIN blocks with long nvlists. The single preallocated checksum
 * block will capture the majority of cases. If more checksum blocks are
 * needed, they must be allocated dynamically.
 *
 * It's likely pointless to precalculate the header checksum since the
 * amount of data involved is small. This allows payloadless records to
 * circumvent multithreaded dispatching altogether.
 */
typedef struct {
	drr_packet_t	dp_base;
	zio_cksum_t	dp_fletcher4_payload;
	zio_cksum_t	*dp_fletcher4_overflow;
} drr_fletcher4_t;


