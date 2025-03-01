/*
 * atsc3_mmt_context_callback_driver_test.c
 *
 *  Created on: Oct 1, 2019
 *      Author: jjustman
 *
 * sample listener for MMT flow(s) to extract MFU emissions for decoder buffer handoff and robustness validation
 */

int PACKET_COUNTER=0;

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

#include "../atsc3_listener_udp.h"
#include "../atsc3_utils.h"

#include "../atsc3_lls.h"

#include "../atsc3_lls_slt_parser.h"
#include "../atsc3_lls_sls_monitor_output_buffer_utils.h"

#include "../atsc3_mmtp_packet_types.h"
#include "../atsc3_mmtp_parser.h"
#include "../atsc3_ntp_utils.h"
#include "../atsc3_mmt_mpu_utils.h"

#include "../atsc3_logging_externs.h"

#include "../atsc3_mmt_context_mfu_depacketizer.h"
#include "../atsc3_mmt_context_mfu_depacketizer_callbacks_noop.h"

#define _ENABLE_DEBUG true

//commandline stream filtering

uint32_t* dst_ip_addr_filter = NULL;
uint16_t* dst_ip_port_filter = NULL;
uint16_t* dst_packet_id_filter = NULL;

//dump essences out


#define __MMT_CONTEXT_LISTENER_TEST_ERROR(...)   			 { __LIBATSC3_TIMESTAMP_ERROR(__VA_ARGS__); };
#define __MMT_CONTEXT_LISTENER_TEST_DEBUG(...)   			 { __LIBATSC3_TIMESTAMP_DEBUG(__VA_ARGS__); };

uint32_t extracted_sample_duration_us_video = 0;

void atsc3_mmt_mpu_mfu_on_sample_complete_dump(atsc3_mmt_mfu_context_t* atsc3_mmt_mfu_context, uint16_t packet_id, uint32_t mmtp_timestamp, uint32_t mpu_sequence_number, uint32_t sample_number, block_t* mmt_mfu_sample, uint32_t mfu_fragment_count_rebuilt) {

    atsc3_mmt_mfu_mpu_timestamp_descriptor_t* atsc3_mmt_mfu_mpu_timestamp_descriptor = atsc3_mmt_mfu_context->get_mpu_timestamp_from_packet_id_mpu_sequence_number_with_mmtp_timestamp_recovery_differential(atsc3_mmt_mfu_context, packet_id, mmtp_timestamp, mpu_sequence_number, sample_number);

    __MMT_CONTEXT_LISTENER_TEST_DEBUG("atsc3_mmt_mpu_mfu_on_sample_complete_dump: PTS: %"PRId64", packet_id: %u, mpu_sequence_number: %u, sample_number: %u, mmt_mfu_sample: %p, len: %d",
									  atsc3_mmt_mfu_mpu_timestamp_descriptor?
									  (atsc3_mmt_mfu_mpu_timestamp_descriptor->mpu_presentation_time_as_us_value + extracted_sample_duration_us_video * sample_number)  : 0,
            packet_id,
            mpu_sequence_number,
            sample_number,
            mmt_mfu_sample,
            mmt_mfu_sample->p_size);


    __MMT_CONTEXT_LISTENER_TEST_DEBUG(" 0x%02x 0x%02x 0x%02x 0x%02x",
                                      mmt_mfu_sample->p_buffer[0],
                                      mmt_mfu_sample->p_buffer[1],
                                      mmt_mfu_sample->p_buffer[2],
                                      mmt_mfu_sample->p_buffer[3]);


	//push to decoder surface (a/v/s)

}

void atsc3_mmt_mpu_on_sequence_movie_fragment_metadata_present_local(atsc3_mmt_mfu_context_t* atsc3_mmt_mfu_context, uint16_t packet_id, uint32_t mpu_sequence_number, block_t* mmt_movie_fragment_metadata) {
	uint32_t decoder_configuration_timebase = 1000000; //set as default to uS
	uint32_t extracted_sample_duration_us = 0;

	if (!mmt_movie_fragment_metadata || !mmt_movie_fragment_metadata->p_size) {
		__MMT_CONTEXT_LISTENER_TEST_DEBUG("atsc3_mmt_mpu_on_sequence_movie_fragment_metadata_present_ndk: packet_id: %d, mpu_sequence_number: %d, mmt_movie_fragment_metadata: %p: returned null or no length!",
										  packet_id, mpu_sequence_number, mmt_movie_fragment_metadata);
		return;
	}

	__MMT_CONTEXT_LISTENER_TEST_DEBUG("atsc3_mmt_mpu_on_sequence_movie_fragment_metadata_present_ndk: packet_id: %d, mpu_sequence_number: %d, asset_type: %s, atsc3_video_decoder_configuration_record: %p, atsc3_audio_decoder_configuration_record: %p, atsc3_stpp_decoder_configuration_record: %p",
									  packet_id,
									  mpu_sequence_number,
									  atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->asset_type,
									  atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_video_decoder_configuration_record,
									  atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_audio_decoder_configuration_record,
									  atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_stpp_decoder_configuration_record)

	if (atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_video_decoder_configuration_record && atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_video_decoder_configuration_record->timebase) {
		decoder_configuration_timebase = atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_video_decoder_configuration_record->timebase;
	} else if (atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_audio_decoder_configuration_record && atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_audio_decoder_configuration_record->timebase) {
		decoder_configuration_timebase = atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_audio_decoder_configuration_record->timebase;
	} else if (atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_stpp_decoder_configuration_record && atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_stpp_decoder_configuration_record->timebase) {
		decoder_configuration_timebase = atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_stpp_decoder_configuration_record->timebase;
	} else {
		__MMT_CONTEXT_LISTENER_TEST_DEBUG("atsc3_mmt_mpu_on_sequence_movie_fragment_metadata_present_ndk: packet_id: %d, mpu_sequence_number: %d, mmt_movie_fragment_metadata: %p: using default decoder timebase of: %u",
										  packet_id, mpu_sequence_number, mmt_movie_fragment_metadata, decoder_configuration_timebase);
	}


	extracted_sample_duration_us = atsc3_mmt_movie_fragment_extract_sample_duration_us(mmt_movie_fragment_metadata, decoder_configuration_timebase);

	if (!extracted_sample_duration_us) {
		__MMT_CONTEXT_LISTENER_TEST_DEBUG("atsc3_mmt_mpu_on_sequence_movie_fragment_metadata_present_ndk: packet_id: %d, mpu_sequence_number: %d, mmt_movie_fragment_metadata: %p, computed extracted_sample_duration_us was 0!",
										  packet_id, mpu_sequence_number, mmt_movie_fragment_metadata);
		return;
	}

	extracted_sample_duration_us_video = extracted_sample_duration_us;
	//jjustman-2021-10-05 - ado yoga #16967 - fixup to keep track of our extracted sample duration for mpu_presentation_timestamp descriptor recovery in atsc3_get_mpu_timestamp_from_packet_id_mpu_sequence_number_with_mmtp_timestamp_recovery_differential
	if (atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_video_decoder_configuration_record) {
		atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_video_decoder_configuration_record->sample_duration_us = extracted_sample_duration_us;
	} else if (atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_audio_decoder_configuration_record) {
		atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_audio_decoder_configuration_record->sample_duration_us = extracted_sample_duration_us;
	} else if (atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_stpp_decoder_configuration_record) {
		atsc3_mmt_mfu_context->mmtp_packet_id_packets_container->atsc3_stpp_decoder_configuration_record->sample_duration_us = extracted_sample_duration_us;
	}
	__MMT_CONTEXT_LISTENER_TEST_DEBUG("packet_id: %d, mpu_sequence_number: %d, extracted_sample_duration_us: %d",
									  packet_id, mpu_sequence_number, extracted_sample_duration_us);

}



void atsc3_lls_on_sls_table_present_local(atsc3_lls_table_t* lls_table) {
    if(!lls_table) {
        __MMT_CONTEXT_LISTENER_TEST_ERROR("no LLS table for update!")
        return;
    }

    if(!lls_table->raw_xml.xml_payload || !lls_table->raw_xml.xml_payload_size) {
        __MMT_CONTEXT_LISTENER_TEST_ERROR("E: atsc3_lls_on_sls_table_present_ndk: no raw_xml.xml_payload for SLS!");
        return;
    }

    __MMT_CONTEXT_LISTENER_TEST_DEBUG("atsc3_lls_on_sls_table_present_local: lls_table is: %p, val: %s", lls_table, lls_table->raw_xml.xml_payload);
}

//jjustman-2019-09-18: refactored MMTP flow collection management
mmtp_flow_t* mmtp_flow;

//todo: jjustman-2019-09-18 refactor me out for mpu recon persitance
udp_flow_latest_mpu_sequence_number_container_t* udp_flow_latest_mpu_sequence_number_container;

lls_slt_monitor_t* lls_slt_monitor;

lls_sls_mmt_monitor_t* lls_sls_mmt_monitor = NULL;

//jjustman-2019-10-03 - context event callbacks...
atsc3_mmt_mfu_context_t* atsc3_mmt_mfu_context;

mmtp_packet_header_t*  mmtp_parse_header_from_udp_packet(udp_packet_t* udp_packet) {

    mmtp_packet_header_t* mmtp_packet_header = mmtp_packet_header_parse_from_block_t(udp_packet->data);

    if(!mmtp_packet_header) {
        __ERROR("mmtp_parse_header_from_udp_packet: mmtp_packet_header_parse_from_block_t: raw packet ptr is null, parsing failed for flow: %d.%d.%d.%d:(%-10u):%-5u \t ->  %d.%d.%d.%d:(%-10u):%-5u ",
                __toipandportnonstruct(udp_packet->udp_flow.src_ip_addr, udp_packet->udp_flow.src_port),
                udp_packet->udp_flow.src_ip_addr,
                __toipandportnonstruct(udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port),
                udp_packet->udp_flow.dst_ip_addr);
        return NULL;
    }

    return mmtp_packet_header;
}

void mmtp_process_sls_from_payload(udp_packet_t *udp_packet, mmtp_signalling_packet_t* mmtp_signalling_packet, lls_sls_mmt_session_t* matching_lls_slt_mmt_session) {

    __INFO("mmtp_process_sls_from_payload: processing mmt flow: %d.%d.%d.%d:(%u) packet_id: %d, signalling message: %p",
            __toipandportnonstruct(udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port),
            mmtp_signalling_packet->mmtp_packet_id,
            mmtp_signalling_packet);

    mmt_signalling_message_dump(mmtp_signalling_packet);
}

void process_packet(u_char *user, const struct pcap_pkthdr *pkthdr, const u_char *packet) {
    mmtp_packet_header_t* mmtp_packet_header = NULL;
	mmtp_asset_t*                           mmtp_asset = NULL;
	mmtp_packet_id_packets_container_t*     mmtp_packet_id_packets_container = NULL;

	lls_sls_mmt_session_t* matching_lls_sls_mmt_session = NULL;

    udp_packet_t* udp_packet = process_packet_from_pcap(user, pkthdr, packet);
    if(!udp_packet) {
        return;
    }

    //drop mdNS
    if(udp_packet->udp_flow.dst_ip_addr == UDP_FILTER_MDNS_IP_ADDRESS && udp_packet->udp_flow.dst_port == UDP_FILTER_MDNS_PORT) {
        goto cleanup;
    }

    if(udp_packet->udp_flow.dst_ip_addr == LLS_DST_ADDR && udp_packet->udp_flow.dst_port == LLS_DST_PORT) {
        //auto-monitor code here for MMT
        //process as lls.sst, dont free as we keep track of our object in the lls_slt_monitor
        atsc3_lls_table_t* original_lls_table = lls_table_create_or_update_from_lls_slt_monitor(lls_slt_monitor, udp_packet->data);
        atsc3_lls_table_t* lls_table = atsc3_lls_table_find_slt_if_signedMultiTable(original_lls_table);

        if(lls_table) {
            if(lls_table->lls_table_id == SLT) {
                //pretty sure this is redundant, as it is already called in lls_table_create_or_update_from_lls_slt_monitor
                int retval = lls_slt_table_perform_update(lls_table, lls_slt_monitor);

                if(!retval) {
                    lls_dump_instance_table(lls_table);
                    for(int i=0; i < lls_table->slt_table.atsc3_lls_slt_service_v.count; i++) {
                        atsc3_lls_slt_service_t* atsc3_lls_slt_service = lls_table->slt_table.atsc3_lls_slt_service_v.data[i];
                        if(atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.count && atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.data[0]->sls_protocol == SLS_PROTOCOL_MMTP) {
                            if(lls_sls_mmt_monitor) {
                                //re-configure
                            } else {
                                //TODO:  make sure
                                //lls_service->broadcast_svc_signaling.sls_destination_ip_address && lls_service->broadcast_svc_signaling.sls_destination_udp_port
                                //match our dst_ip_addr_filter && udp_packet->udp_flow.dst_ip_addr != *dst_ip_addr_filter and port filter
                                __INFO("Adding service: %d", atsc3_lls_slt_service->service_id);

                                lls_sls_mmt_monitor = lls_sls_mmt_monitor_create();
                                lls_sls_mmt_monitor->transients.atsc3_lls_slt_service = atsc3_lls_slt_service; //HACK!
                                lls_slt_service_id_t* lls_slt_service_id = lls_slt_service_id_new_from_atsc3_lls_slt_service(atsc3_lls_slt_service);

                                lls_slt_monitor_add_lls_slt_service_id(lls_slt_monitor, lls_slt_service_id);

                                //we may not be initialized yet, so re-check again later
                                //this should _never_happen...
                                lls_sls_mmt_session_t* lls_sls_mmt_session = lls_slt_mmt_session_find_from_service_id(lls_slt_monitor, atsc3_lls_slt_service->service_id);
                                if(!lls_sls_mmt_session) {
                                    __WARN("lls_slt_mmt_session_find_from_service_id: lls_sls_mmt_session is NULL!");
                                }
                                lls_sls_mmt_monitor->transients.lls_mmt_session = lls_sls_mmt_session;
                                lls_slt_monitor->lls_sls_mmt_monitor = lls_sls_mmt_monitor;

                                lls_slt_monitor_add_lls_sls_mmt_monitor(lls_slt_monitor, lls_sls_mmt_monitor);
                            }
                        }
                    }
                }
            }
        }

        __INFO("Checking lls_sls_mmt_monitor: %p,", lls_sls_mmt_monitor);

        if(lls_sls_mmt_monitor && lls_sls_mmt_monitor->transients.lls_mmt_session) {
            __INFO("Checking lls_sls_mmt_monitor->lls_mmt_session: %p,", lls_sls_mmt_monitor->transients.lls_mmt_session);
        }

    
        goto cleanup;
    }

    if((dst_ip_addr_filter && udp_packet->udp_flow.dst_ip_addr != *dst_ip_addr_filter)) {
        goto cleanup;
    }

    //TODO: jjustman-2019-10-03 - packet header parsing to dispatcher mapping
    matching_lls_sls_mmt_session = lls_sls_mmt_session_find_from_udp_packet(lls_slt_monitor, udp_packet->udp_flow.src_ip_addr, udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port);
    __TRACE("Checking matching_lls_sls_mmt_session: %p,", matching_lls_sls_mmt_session);


    if(matching_lls_sls_mmt_session && lls_slt_monitor && lls_slt_monitor->lls_sls_mmt_monitor && matching_lls_sls_mmt_session->atsc3_lls_slt_service->service_id == lls_slt_monitor->lls_sls_mmt_monitor->transients.atsc3_lls_slt_service->service_id) {

        mmtp_packet_header = mmtp_packet_header_parse_from_block_t(udp_packet->data);
        
        if(!mmtp_packet_header) {
            goto cleanup;
        }

        //for filtering MMT flows by a specific packet_id
        if(dst_packet_id_filter && *dst_packet_id_filter != mmtp_packet_header->mmtp_packet_id) {
            goto cleanup;
        }

        mmtp_packet_header_dump(mmtp_packet_header);

		mmtp_asset = atsc3_mmt_mfu_context_mfu_depacketizer_context_update_find_or_create_mmtp_asset(atsc3_mmt_mfu_context, udp_packet, lls_slt_monitor, matching_lls_sls_mmt_session);
		mmtp_packet_id_packets_container = atsc3_mmt_mfu_context_mfu_depacketizer_update_find_or_create_mmtp_packet_id_packets_container(atsc3_mmt_mfu_context, mmtp_asset, mmtp_packet_header);


		//dump header, then dump applicable packet type
        if(mmtp_packet_header->mmtp_payload_type == 0x0) {
            //mmtp_mpu_packet_t* mmtp_mpu_packet = mmtp_mpu_packet_parse_from_block_t();
            mmtp_mpu_packet_t* mmtp_mpu_packet = mmtp_mpu_packet_parse_and_free_packet_header_from_block_t(&mmtp_packet_header, udp_packet->data);
            if(!mmtp_mpu_packet) {
                goto error;
            }
            
            if(mmtp_mpu_packet->mpu_timed_flag == 1) {
                mmtp_mpu_dump_header(mmtp_mpu_packet);

                //TODO: jjustman-2019-10-03 - handle context parameters better
                // mmtp_flow, lls_slt_monitor, , udp_flow_latest_mpu_sequence_number_container, matching_lls_sls_mmt_session);

                atsc3_mmt_mfu_context->mmtp_flow = mmtp_flow;
                atsc3_mmt_mfu_context->udp_flow_latest_mpu_sequence_number_container = udp_flow_latest_mpu_sequence_number_container;
                atsc3_mmt_mfu_context->transients.lls_slt_monitor = lls_slt_monitor;
                atsc3_mmt_mfu_context->matching_lls_sls_mmt_session = matching_lls_sls_mmt_session;

                __TRACE("process_packet: mmtp_mfu_process_from_payload_with_context with udp_packet: %p, mmtp_mpu_packet: %p, atsc3_mmt_mfu_context: %p,",
                        udp_packet, mmtp_mpu_packet, atsc3_mmt_mfu_context);

                mmtp_mfu_process_from_payload_with_context(udp_packet, mmtp_mpu_packet, atsc3_mmt_mfu_context);

            } else {
                //non-timed
                __ATSC3_WARN("process_packet: mmtp_packet_header_parse_from_block_t - non-timed payload: packet_id: %u", mmtp_packet_header->mmtp_packet_id);
            }
        } else if(mmtp_packet_header->mmtp_payload_type == 0x2) {

            mmtp_signalling_packet_t* mmtp_signalling_packet = mmtp_signalling_packet_parse_and_free_packet_header_from_block_t(&mmtp_packet_header, udp_packet->data);
            uint8_t parsed_count = mmt_signalling_message_parse_packet(mmtp_signalling_packet, udp_packet->data);
            if(parsed_count) {
                mmt_signalling_message_dump(mmtp_signalling_packet);

                __TRACE("process_packet: calling mmt_signalling_message_dispatch_context_notification_callbacks with udp_packet: %p, mmtp_signalling_packet: %p, atsc3_mmt_mfu_context: %p,",
                        udp_packet,
                        mmtp_signalling_packet,
                        atsc3_mmt_mfu_context);

                mmt_signalling_message_dispatch_context_notification_callbacks(udp_packet, mmtp_signalling_packet, atsc3_mmt_mfu_context);


                //internal hacks below


                //TODO: jjustman-2019-10-03 - if signalling_packet == MP_table, set atsc3_mmt_mfu_context->mp_table_last;
                mmtp_asset_flow_t* mmtp_asset_flow = mmtp_flow_find_or_create_from_udp_packet(mmtp_flow, udp_packet);
                mmtp_asset_t* mmtp_asset = mmtp_asset_flow_find_or_create_asset_from_lls_sls_mmt_session(mmtp_asset_flow, matching_lls_sls_mmt_session);

                //TODO: FIX ME!!! HACK - jjustman-2019-09-05
                mmtp_mpu_packet_t* mmtp_mpu_packet = mmtp_mpu_packet_new();
                mmtp_mpu_packet->mmtp_packet_id = mmtp_signalling_packet->mmtp_packet_id;

                mmtp_packet_id_packets_container_t* mmtp_packet_id_packets_container = mmtp_asset_find_or_create_packets_container_from_mmt_mpu_packet(mmtp_asset, mmtp_mpu_packet);
                mmtp_packet_id_packets_container_add_mmtp_signalling_packet(mmtp_packet_id_packets_container, mmtp_signalling_packet);

                //TODO: FIX ME!!! HACK - jjustman-2019-09-05
                mmtp_mpu_packet_free(&mmtp_mpu_packet);

                //update our sls_mmt_session info
                mmt_signalling_message_update_lls_sls_mmt_session(mmtp_signalling_packet, matching_lls_sls_mmt_session);

                //TODO - remap this
                //add in flows                 lls_sls_mmt_session_t* lls_sls_mmt_session = lls_slt_mmt_session_find_from_service_id(lls_slt_monitor, lls_sls_mmt_monitor->lls_mmt_session->service_id);

                if(lls_sls_mmt_monitor && lls_sls_mmt_monitor->transients.lls_mmt_session && matching_lls_sls_mmt_session) {
                    __INFO("mmt_signalling_information: from atsc3 service_id: %u, patching: seting audio_packet_id/video_packet_id/stpp_packet_id: %u, %u, %u",
                                                matching_lls_sls_mmt_session->atsc3_lls_slt_service->service_id,
                                                matching_lls_sls_mmt_session->audio_packet_id,
                                                matching_lls_sls_mmt_session->video_packet_id,
                                                matching_lls_sls_mmt_session->stpp_packet_id);

                }
            }

        } else {
            __ATSC3_WARN("process_packet: mmtp_packet_header_parse_from_block_t - unknown payload type of 0x%x", mmtp_packet_header->mmtp_payload_type);
            goto cleanup;
        }
    }

cleanup:
    if(mmtp_packet_header) {
        mmtp_packet_header_free(&mmtp_packet_header);
    }
    
    if(udp_packet) {
        udp_packet_free(&udp_packet);
    }
    
    return;

error:
    __ATSC3_WARN("process_packet: error, bailing loop!");
    return;
}


void* pcap_loop_run_thread(void* dev_pointer) {
    char* dev = (char*) dev_pointer;

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t* descr;
    struct bpf_program fp;
    bpf_u_int32 maskp;
    bpf_u_int32 netp;

    pcap_lookupnet(dev, &netp, &maskp, errbuf);
    descr = pcap_open_live(dev, MAX_PCAP_LEN, 1, 1, errbuf);

    if(descr == NULL) {
        printf("pcap_open_live(): %s",errbuf);
        exit(1);
    }

    char filter[] = "udp";
    if(pcap_compile(descr,&fp, filter,0,netp) == -1) {
        fprintf(stderr,"Error calling pcap_compile");
        exit(1);
    }

    if(pcap_setfilter(descr,&fp) == -1) {
        fprintf(stderr,"Error setting filter");
        exit(1);
    }

    pcap_loop(descr,-1,process_packet,NULL);

    return 0;
}


/**
 *
 * atsc3_mmt_listener_test interface (dst_ip) (dst_port)
 *
 * arguments:
 */
int main(int argc,char **argv) {


#ifdef __lots_of_logging_
	_MMT_CONTEXT_MPU_DEBUG_ENABLED = 1;

    _LLS_DEBUG_ENABLED = 1;

    _MMT_SIGNALLING_MESSAGE_DEBUG_ENABLED = 1;
    _MMT_SIGNALLING_MESSAGE_TRACE_ENABLED = 1;

    _LLS_SLT_PARSER_INFO_MMT_ENABLED = 1;
    _LLS_MMT_UTILS_TRACE_ENABLED = 1;
    
    _MMTP_DEBUG_ENABLED = 1;
    _MMT_MPU_PARSER_DEBUG_ENABLED = 1;

    _MMT_CONTEXT_MPU_DEBUG_ENABLED = 1;
    _MMT_CONTEXT_MPU_SIGNAL_INFO_ENABLED = 1;

#endif
    
    
    char *dev;

    char *filter_dst_ip = NULL;
    char *filter_dst_port = NULL;
    char *filter_packet_id = NULL;

    int dst_port_filter_int;
    int dst_ip_port_filter_int;
    int dst_packet_id_filter_int;

    //listen to all flows
    if(argc == 2) {
        dev = argv[1];
        __INFO("listening on dev: %s", dev);
    } else if(argc>=4) {
        //listen to a selected flow
        dev = argv[1];
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

        if(argc>=5) {
            filter_packet_id = argv[4];
            if(!(strncmp("*", filter_packet_id, 1) == 0 || strncmp("-", filter_packet_id, 1) == 0)) {
                dst_packet_id_filter_int = atoi(filter_packet_id);
                dst_packet_id_filter = (uint16_t*)calloc(1, sizeof(uint16_t));
                *dst_packet_id_filter |= dst_packet_id_filter_int & 0xFFFF;
            }
        }

        __INFO("listening on dev: %s, dst_ip: %s (%p), dst_port: %s (%p), dst_packet_id: %s (%p)", dev, filter_dst_ip, dst_ip_addr_filter, filter_dst_port, dst_ip_port_filter, filter_packet_id, dst_packet_id_filter);


    } else {
        println("%s - a udp mulitcast listener test harness for atsc3 mmt sls", argv[0]);
        println("---");
        println("args: dev (dst_ip) (dst_port) (packet_id)");
        println(" dev: device to listen for udp multicast, default listen to 0.0.0.0:0");
        println(" (dst_ip): optional, filter to specific ip address");
        println(" (dst_port): optional, filter to specific port");

        println("");
        exit(1);
    }

    /** setup global structs **/

    lls_slt_monitor = lls_slt_monitor_create();
    lls_slt_monitor->atsc3_lls_on_sls_table_present_callback = &atsc3_lls_on_sls_table_present_local;

    mmtp_flow = mmtp_flow_new();
    udp_flow_latest_mpu_sequence_number_container = udp_flow_latest_mpu_sequence_number_container_t_init();

    //callback contexts
    atsc3_mmt_mfu_context = atsc3_mmt_mfu_context_callbacks_noop_new();

    //MFU related callbacks
    atsc3_mmt_mfu_context->atsc3_mmt_mpu_mfu_on_sample_complete = &atsc3_mmt_mpu_mfu_on_sample_complete_dump;
	atsc3_mmt_mfu_context->atsc3_mmt_mpu_on_sequence_movie_fragment_metadata_present = &atsc3_mmt_mpu_on_sequence_movie_fragment_metadata_present_local;


#ifndef _TEST_RUN_VALGRIND_OSX_

    pthread_t global_pcap_thread_id;
    int pcap_ret = pthread_create(&global_pcap_thread_id, NULL, pcap_loop_run_thread, (void*)dev);
    assert(!pcap_ret);


    pthread_join(global_pcap_thread_id, NULL);

#else
    pcap_loop_run_thread(dev);
#endif


    return 0;
}




