/*
 * atsc3_ntp_utils.c
 *
 *  Created on: Jan 8, 2019
 *      Author: jjustman
 */

#include "atsc3_ntp_utils.h"

/**
 * convert ntp "short-format" packet time into a future re-clocked pts
 * short-format is:
 *
 * 	int16_t seconds
 * 	int16_t s_fragment
 *
 * 	seconds rolls over at 65535 from every ntp epoc, so we need to un-bias this when computing a future offset
 *
 * 	instead
 */

void compute_ntp32_to_seconds_microseconds(uint32_t timestamp, uint16_t *seconds, uint16_t *microseconds) {
	//->mmtp_packet_header->mmtp_timestamp, &mmtp_packet_header->mmtp_timestamp_s, &mmtp_packet_header->mmtp_timestamp_us);

	*seconds = (timestamp >> 16) & 0xFFFF;

	//this is where things get messsy..
	uint16_t tmp_mmtp_fractional_s =  (timestamp & 0xFFFF);
	//1329481807 * (10 ^ 6) / 2 ^ 32 = 309544 (roughtly)
	*microseconds = (uint16_t)( (double)tmp_mmtp_fractional_s * 1.0e6 / (double)(1LL<<16) );

}


void compute_ntp64_to_seconds_microseconds(uint64_t timestamp, uint32_t *seconds, uint32_t *microseconds) {
	//->mmtp_packet_header->mmtp_timestamp, &mmtp_packet_header->mmtp_timestamp_s, &mmtp_packet_header->mmtp_timestamp_us);

	*seconds = (timestamp >> 32) & 0xFFFFFFFF;

	//this is where things get messsy..
	uint32_t tmp_mmtp_fractional_s =  (timestamp & 0xFFFFFFFF);
	//1329481807 * (10 ^ 6) / 2 ^ 32 = 309544 (roughtly)
	*microseconds = (uint32_t)( (double)tmp_mmtp_fractional_s * 1.0e6 / (double)(1LL<<32) );

}

uint32_t ntp32_compute_from_seconds_microseconds(uint16_t seconds, uint16_t microseconds) {
    return ((uint32_t)(seconds) << 16) | (uint16_t)( (double)(microseconds+1) * (double)(1LL<<16) * 1.0e-6 );
}

uint64_t ntp64_compute_from_seconds_microseconds(uint16_t seconds, uint16_t microseconds) {
    return ((uint64_t)(seconds) << 32) | (uint32_t)( (double)(microseconds+1) * (double)(1LL<<32) * 1.0e-6 );
}

/*
 *
 * make sure to call above to un-fractionalize fractions
 */


uint64_t compute_relative_ntp32_pts(uint64_t first_pts, uint16_t mmtp_timestamp_s, uint16_t mmtp_timestamp_microseconds) {

	uint64_t pts = REBASE_PTS_OFFSET + (mmtp_timestamp_s * uS) + mmtp_timestamp_microseconds - first_pts;

//	printf("%d:compute_relative_ntp32_pts: pts is: %"PRIu64"\n", __LINE__, pts);

	return pts;
}

int64_t rebase_now_with_ntp32(uint16_t mmtp_timestamp_s, uint16_t mmtp_timestamp_microseconds) {
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);

	uint64_t now_t = ((ts.tv_sec) * uS) + ((ts.tv_nsec) / 1000ULL) ; // convert tv_sec & tv_usec to millisecond

	//convert to timespec with rolled over bias
	uint64_t quantized = REBASE_PTS_OFFSET + ((((ts.tv_sec / 65535)) * 65535) * uS) + ((ts.tv_nsec) / 1000ULL) ; // convert tv_sec & tv_usec to millisecond
	//printf("%d:now_t: %"PRIu64", quantized: %"PRIu64", mmtp_timestamp_s: %d, \n", __LINE__, now_t, quantized, mmtp_timestamp_s);

	uint64_t pts = quantized + (mmtp_timestamp_s * uS) + mmtp_timestamp_microseconds;

	//printf("%d:utc_now_t is: %"PRIu64", rebase_now_with_ntp32: re-quantized is %"PRIu64", computed jitter is: %"PRIu64" \n", __LINE__, now_t, pts, (pts - now_t));

	return pts;
}
