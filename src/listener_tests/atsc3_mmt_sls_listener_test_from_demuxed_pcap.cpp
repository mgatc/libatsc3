/*
 * atsc3_mmt_sls_listener_test_from_demuxed_pcap.c
 *
 *  Created on: Mar 6, 2019
 *      Author: jjustman
 *
 * sample listener for MMT flow(s) to extract packet_id=0 sls data for testing
 */


#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <ncurses.h>
#include <limits.h>

#include <atsc3_utils.h>
#include <atsc3_listener_udp.h>
#include <atsc3_pcap_type.h>

#include <atsc3_lls.h>
#include <atsc3_lls_slt_parser.h>
#include <atsc3_lls_sls_monitor_output_buffer_utils.h>
#include <atsc3_mmtp_packet_types.h>
#include <atsc3_mmtp_parser.h>
#include <atsc3_ntp_utils.h>
#include <atsc3_mmt_mpu_utils.h>
#include <atsc3_logging_externs.h>

#include <atsc3_mmt_context_mfu_depacketizer.h>
#include <atsc3_mmt_context_mfu_depacketizer_callbacks_noop.h>

#define _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_ERROR(...)    __LIBATSC3_TIMESTAMP_ERROR(__VA_ARGS__);
#define _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_WARN(...)     __LIBATSC3_TIMESTAMP_WARN(__VA_ARGS__);
#define _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_INFO(...)     __LIBATSC3_TIMESTAMP_INFO(__VA_ARGS__);
#define _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_DEBUG(...)    __LIBATSC3_TIMESTAMP_DEBUG(__VA_ARGS__);
#define _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_TRACE(...) // __LIBATSC3_TIMESTAMP_TRACE(__VA_ARGS__);

int processed_count = 0;
int PACKET_COUNTER=0;

//commandline stream filtering
uint32_t* dst_ip_addr_filter = NULL;
uint16_t* dst_ip_port_filter = NULL;
uint16_t* dst_packet_id_filter = NULL;

lls_slt_monitor_t* lls_slt_monitor;

//jjustman-2019-10-03 - context event callbacks...
atsc3_mmt_mfu_context_t* atsc3_mmt_mfu_context;

mmtp_packet_header_t*  mmtp_parse_header_from_udp_packet(udp_packet_t* udp_packet) {

	mmtp_packet_header_t* mmtp_packet_header = mmtp_packet_header_parse_from_block_t(udp_packet->data);

    if(!mmtp_packet_header) {
        _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_ERROR("mmtp_parse_header_from_udp_packet: mmtp_packet_header_parse_from_block_t: raw packet ptr is null, parsing failed for flow: %d.%d.%d.%d:(%-10u):%-5u \t ->  %d.%d.%d.%d:(%-10u):%-5u ",
                __toipandportnonstruct(udp_packet->udp_flow.src_ip_addr, udp_packet->udp_flow.src_port),
                udp_packet->udp_flow.src_ip_addr,
                __toipandportnonstruct(udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port),
                udp_packet->udp_flow.dst_ip_addr);
        return NULL;
    }

    return mmtp_packet_header;
}

void mmtp_process_sls_from_payload(udp_packet_t *udp_packet, mmtp_signalling_packet_t* mmtp_signalling_packet, lls_sls_mmt_session_t* matching_lls_slt_mmt_session) {

    _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_INFO("mmtp_process_sls_from_payload: processing mmt flow: %d.%d.%d.%d:(%u) packet_sequence_number: %d, mmt_packet_id: %d, signalling message: %p",
			__toipandportnonstruct(udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port),
			mmtp_signalling_packet->packet_sequence_number,
            mmtp_signalling_packet->mmtp_packet_id,
			mmtp_signalling_packet);

	mmt_signalling_message_dump(mmtp_signalling_packet);
}

void process_packet(block_t* raw_ethernet_packet_blockt) {
	mmtp_packet_header_t* mmtp_packet_header = NULL;
	
    udp_packet_t* udp_packet = udp_packet_process_from_raw_ethernet_block_t(raw_ethernet_packet_blockt);
	if(!udp_packet) {
		return;
	}

	//drop mdNS
	if(udp_packet->udp_flow.dst_ip_addr == UDP_FILTER_MDNS_IP_ADDRESS && udp_packet->udp_flow.dst_port == UDP_FILTER_MDNS_PORT) {
		return udp_packet_free(&udp_packet);
	}

	if(udp_packet->udp_flow.dst_ip_addr == LLS_DST_ADDR && udp_packet->udp_flow.dst_port == LLS_DST_PORT) {
				//process as lls
		atsc3_lls_table_t* lls_table = lls_table_create_or_update_from_lls_slt_monitor(lls_slt_monitor, udp_packet->data);

		return udp_packet_free(&udp_packet);
	}

    if((dst_ip_addr_filter && udp_packet->udp_flow.dst_ip_addr != *dst_ip_addr_filter)) {
        return udp_packet_free(&udp_packet);
    }

    lls_sls_mmt_session_t* matching_lls_slt_mmt_session = lls_sls_mmt_session_find_from_udp_packet(lls_slt_monitor, udp_packet->udp_flow.src_ip_addr, udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port);
    if(matching_lls_slt_mmt_session) {

		int8_t mmtp_si_parsed_message_count = 0;
		mmtp_packet_header = mmtp_parse_header_from_udp_packet(udp_packet);
        if(mmtp_packet_header && mmtp_packet_header->mmtp_payload_type == 0x02) {

			//jjustman-2021-09-15 - TODO: fix me for fragmented parsing processing, refactor from atsc3_mmt_context_stpp_depacketizer_test.cpp
        	mmtp_signalling_packet_t* mmtp_signalling_packet = mmtp_signalling_packet_parse_and_free_packet_header_from_block_t(&mmtp_packet_header, udp_packet->data);
			if(mmtp_signalling_packet->si_fragmentation_indicator == 0x0) {

				mmtp_si_parsed_message_count = mmt_signalling_message_parse_packet(mmtp_signalling_packet, udp_packet->data);
				__INFO("mmt_signalling_message_parse_packet: mmtp_si_parsed_message_count is: %d", mmtp_si_parsed_message_count);
				mmtp_process_sls_from_payload(udp_packet, mmtp_signalling_packet, matching_lls_slt_mmt_session);
				
				if(mmtp_si_parsed_message_count > 0) {
					mmt_signalling_message_dump(mmtp_signalling_packet);

					//dispatch our wired callbacks
					mmt_signalling_message_dispatch_context_notification_callbacks(udp_packet, mmtp_signalling_packet, atsc3_mmt_mfu_context);

					mmtp_signalling_packet_free(&mmtp_signalling_packet);
				}
			} else {
				__INFO("mmt_signalling_message_parse_packet: TODO: inline mmtp_si fragment reassembly for si_fragmentation_indicator: %d", mmtp_signalling_packet->si_fragmentation_indicator );			
			}
        }
	}

cleanup:
	if(mmtp_packet_header) {
		mmtp_packet_header_free(&mmtp_packet_header);
	}

	if(udp_packet) {
		udp_packet_free(&udp_packet);
	}
}

/**
 *
 * atsc3_mmt_listener_test interface (dst_ip) (dst_port)
 *
 * arguments:
 */
int main(int argc,char **argv) {

	_MMTP_DEBUG_ENABLED = 1;
	_MMTP_TRACE_ENABLED = 0;
	_MMT_MPU_PARSER_DEBUG_ENABLED = 0;
	_MMT_CONTEXT_MPU_DEBUG_ENABLED = 1;

	_LLS_DEBUG_ENABLED = 0;

	_MMT_SIGNALLING_MESSAGE_DEBUG_ENABLED = 1;
	_MMT_SIGNALLING_MESSAGE_TRACE_ENABLED = 1;
	_MMT_SIGNALLING_MESSAGE_DESCRIPTOR_TRACE_ENABLED = 1;
	
	//jjustman-2021-09-15 - next level deeper is #define _MMT_SIGNALLING_MESSAGE_DUMP_HEX_PAYLOAD
	
    char* PCAP_FILENAME = "";
    atsc3_pcap_replay_context_t* atsc3_pcap_replay_context = NULL;

    char *filter_dst_ip = NULL;
    char *filter_dst_port = NULL;

    int dst_port_filter_int;
    int dst_ip_port_filter_int;

    if(argc == 1) {
            println("%s - DEMUXED pcap replay test harness for atsc3 mmt sls analysis", argv[0]);
            println("---");
            println("args: demuxed_pcap_file_name (dst_ip) (dst_port) (packet_id)");
            println(" pcap_file_name: pcap demuxed file to process MMT SLS emissions - note: must have both LLS and MMT emission in demuxed flow");
            println(" (dst_ip): optional, filter to specific ip address, or * for wildcard");
            println(" (dst_port): optional, filter to specific port, or * for wildcard");

            println("");
            exit(1);
    }
    
    //listen to all flows
    if(argc >= 2) {
        PCAP_FILENAME = argv[1];
    }
    
    if(argc>=4) {
    	//listen to a selected flow
    	filter_dst_ip = argv[2];

		//skip ip address filter if our params are * or -
    	if(!(strncmp("*", filter_dst_ip, 1) == 0 || strncmp("-", filter_dst_ip, 1) == 0)) {
			dst_ip_addr_filter = (uint32_t*)calloc(1, sizeof(uint32_t));
			char* pch = strtok (filter_dst_ip,".");
			int offset = 24;
			while (pch != NULL && offset>=0) {
				uint8_t octet = atoi(pch);
				*dst_ip_addr_filter |= octet << offset;
				offset-=8;
				pch = strtok (NULL, ".");
			}
		}

    	if(argc>=4) {
    		filter_dst_port = argv[3];
        	if(!(strncmp("*", filter_dst_port, 1) == 0 || strncmp("-", filter_dst_port, 1) == 0)) {

				dst_port_filter_int = atoi(filter_dst_port);
				dst_ip_port_filter = (uint16_t*)calloc(1, sizeof(uint16_t));
				*dst_ip_port_filter |= dst_port_filter_int & 0xFFFF;
        	}
    	}

        _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_INFO("reading file %s, dst_ip: %s (%p), dst_port: %s (%p)",
                                                                 PCAP_FILENAME,
                                                                 filter_dst_ip,
                                                                 dst_ip_addr_filter,
                                                                 filter_dst_port,
                                                                 dst_ip_port_filter);
    }

    /** setup global structs **/

    lls_slt_monitor = lls_slt_monitor_create();
	
	//callback contexts
	atsc3_mmt_mfu_context = atsc3_mmt_mfu_context_callbacks_noop_new();

    atsc3_pcap_replay_context = atsc3_pcap_replay_open_filename(PCAP_FILENAME);
    _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_DEBUG("Opening pcap: %s, context is: %p", PCAP_FILENAME, atsc3_pcap_replay_context);

    if(atsc3_pcap_replay_context) {
        while((atsc3_pcap_replay_context = atsc3_pcap_replay_iterate_packet(atsc3_pcap_replay_context))) {
            atsc3_pcap_replay_usleep_packet(atsc3_pcap_replay_context);

            _ATSC3_MMT_SLS_LISTENER_TEST_FROM_DEMUXED_PCAP_TEST_TRACE("pcap reader release: pos: %ld, Got packet len: %d, ts_sec: %u, ts_usec: %u",
                    ftell(atsc3_pcap_replay_context->pcap_fp),
                    atsc3_pcap_replay_context->atsc3_pcap_packet_instance.current_pcap_packet->p_size,
                    atsc3_pcap_replay_context->atsc3_pcap_packet_instance.atsc3_pcap_packet_header.ts_sec,
                    atsc3_pcap_replay_context->atsc3_pcap_packet_instance.atsc3_pcap_packet_header.ts_usec);
            
            process_packet(atsc3_pcap_replay_context->atsc3_pcap_packet_instance.current_pcap_packet);
        }
    }

    return 0;
}

