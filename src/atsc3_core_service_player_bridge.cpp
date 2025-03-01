/*
 * atsc3_core_service_player_bridge.cpp
 *
 *  Created on: Oct 3, 2019
 *      Author: jjustman
 *
 * Android MMT MFU Playback with SLS event driven callbacks
 *
 *
 * Note: Atsc3NdkPHYBridge - Android NDK Binding against Lowasys API are not included
 */

#ifndef __JJ_PHY_MMT_PLAYER_BRIDGE_DISABLED

#define __ADO_25189_WORKAROUND_FOR_TRANSIENT_SLT_SERVICES
//#define __ADO_25189_WORKAROUND_FOR_TRANSIENT_SLT_SERVICES_DEBUGGING

#include "atsc3_core_service_player_bridge.h"

//jjustman-2020-12-02 - restrict this include to local cpp, as downstream projects otherwise would need to have <pcre2.h> on their include path
#include "atsc3_alc_utils.h"
#include "atsc3_aeat_parser.h"

int _ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO_ENABLED = 1;
int _ATSC3_CORE_SERVICE_PLAYER_BRIDGE_DEBUG_ENABLED = 0;
int _ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE_ENABLED = 0;

IAtsc3NdkApplicationBridge* Atsc3NdkApplicationBridge_ptr = NULL;
IAtsc3NdkPHYBridge*         Atsc3NdkPHYBridge_ptr = NULL;

//commandline stream filtering
uint32_t* dst_ip_addr_filter = NULL;
uint16_t* dst_ip_port_filter = NULL;
uint16_t* dst_packet_id_filter = NULL;

int _ATSC3_CORE_SERVICE_PLAYER_BRIDGE_atsc3_core_service_bridge_process_packet_phy_got_mdi_counter = 0;

//jjustman-2021-01-21 - special 'friend' mutex access so we can get lls_slt_monitor as needed, etc..
recursive_mutex atsc3_core_service_player_bridge_context_mutex;

recursive_mutex& atsc3_core_service_player_bridge_get_context_mutex() {
    return atsc3_core_service_player_bridge_context_mutex;
}

//jjustman-2019-10-03 - context event callbacks...
lls_slt_monitor_t* lls_slt_monitor = NULL;
//context for MMT - mmtp/sls flow management
atsc3_mmt_mfu_context_t*    atsc3_mmt_mfu_context = NULL;
//context for LMT
atsc3_link_mapping_table*   atsc3_link_mapping_table_last = NULL;
uint32_t                    atsc3_link_mapping_table_missing_dropped_packets = 0;

//friend accessor for our lls_slt_monitor, until we are refactored to have a proper context
lls_slt_monitor_t* atsc3_core_service_player_bridge_get_lls_slt_montior() {
    return lls_slt_monitor;
}

std::string atsc3_ndk_cache_temp_folder_path = "";
std::string atsc3_ndk_cache_temp_folder_route_path = "";

IAtsc3NdkApplicationBridge* atsc3_ndk_application_bridge_get_instance() {

//jjustman-2022-02-16 - testing
//    _MMT_CONTEXT_MPU_DEBUG_ENABLED = 1;
//   _MMT_CONTEXT_MPU_TRACE_ENABLED = 1;

    return Atsc3NdkApplicationBridge_ptr;
}

IAtsc3NdkPHYBridge* atsc3_ndk_phy_bridge_get_instance() {
    return Atsc3NdkPHYBridge_ptr;
}

void atsc3_core_service_application_bridge_init(IAtsc3NdkApplicationBridge* atsc3NdkApplicationBridge) {
    Atsc3NdkApplicationBridge_ptr = atsc3NdkApplicationBridge;
    printf("atsc3_core_service_application_bridge_init with Atsc3NdkApplicationBridge_ptr: %p", Atsc3NdkApplicationBridge_ptr);
    Atsc3NdkApplicationBridge_ptr->LogMsgF("atsc3_core_service_application_bridge_init - Atsc3NdkApplicationBridge_ptr: %p", Atsc3NdkApplicationBridge_ptr);

    //set global logging levels
    //jjustman-2021-01-19 - testing for mpu_timestamp_descriptor patching
    _MMT_CONTEXT_MPU_DEBUG_ENABLED = 0;
    _ALC_UTILS_IOTRACE_ENABLED = 0;
    _ROUTE_SLS_PROCESSOR_INFO_ENABLED = 0;
    _ROUTE_SLS_PROCESSOR_DEBUG_ENABLED = 0;
    _ALC_UTILS_IOTRACE_ENABLED = 0;

#ifdef __SIGNED_MULTIPART_LLS_DEBUGGING__
        _LLS_TRACE_ENABLED = 1;
    _LLS_DEBUG_ENABLED = 1;

    _LLS_SLT_PARSER_DEBUG_ENABLED = 1;
    _LLS_SLT_PARSER_TRACE_ENABLED = 1;
#endif
    _LLS_ALC_UTILS_DEBUG_ENABLED = 0;
    _ALC_UTILS_DEBUG_ENABLED = 0;
    _ALC_RX_TRACE_ENABLED = 0;

#ifdef __ADO_25189_WORKAROUND_FOR_TRANSIENT_SLT_SERVICES_DEBUGGING
    _ALC_UTILS_INFO_ENABLED=1;
    _ALC_UTILS_DEBUG_ENABLED=1;
    _ALC_UTILS_TRACE_ENABLED=1;
    _ALC_UTILS_IOTRACE_ENABLED=1;

    _LLS_SLT_PARSER_DEBUG_ENABLED = 1;

#endif


    //jjustman-2020-04-23 - TLV parsing metrics enable inline ALP parsing
    __ATSC3_SL_TLV_USE_INLINE_ALP_PARSER_CALL__ = 1;

    atsc3_core_service_application_bridge_reset_context();

    atsc3_ndk_cache_temp_folder_path = Atsc3NdkApplicationBridge_ptr->get_android_temp_folder();

    //jjustman-2020-04-16 - hack to clean up cache directory payload and clear out any leftover cache objects (e.g. ROUTE/DASH toi's)
    //no linkage forfs::remove_all(atsc3_ndk_cache_temp_folder_path + "/");
    //https://github.com/android/ndk/issues/609

    // __ALC_DUMP_OUTPUT_PATH__ -> route
    atsc3_ndk_cache_temp_folder_route_path = atsc3_ndk_cache_temp_folder_path + "/" + __ALC_DUMP_OUTPUT_PATH__;

    atsc3_ndk_cache_temp_folder_purge((char*)(atsc3_ndk_cache_temp_folder_route_path).c_str());

    chdir(atsc3_ndk_cache_temp_folder_path.c_str());

    Atsc3NdkApplicationBridge_ptr->LogMsgF("atsc3_phy_player_bridge_init - completed, cache temp folder path: %s", atsc3_ndk_cache_temp_folder_path.c_str());
    /**
     * additional SLS monitor related callbacks wired up in
     *
        lls_sls_alc_monitor->atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location = &atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_ndk;
        //write up event callback for alc MPD patching
        lls_sls_alc_monitor->atsc3_lls_sls_alc_on_route_mpd_patched = &atsc3_lls_sls_alc_on_route_mpd_patched_ndk;
     */
}

/*
 * jjustman-2020-08-31 - todo: refactor this into a context handle
 */
void atsc3_core_service_application_bridge_reset_context() {
    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_application_bridge_reset_context!");

    lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

    if(atsc3_mmt_mfu_context) {
        atsc3_mmt_mfu_context_free(&atsc3_mmt_mfu_context);
    }

    if (lls_slt_monitor) {
        atsc3_lls_slt_monitor_free(&lls_slt_monitor);
        /*
           atsc3_lls_slt_monitor_free chains calls to:
                lls_slt_monitor_free_lls_sls_mmt_monitor(lls_slt_monitor); ->  MISSING IMPL lls_sls_mmt_monitor_free(), but not needed as only ptr ref's are transient
            -and-
                lls_slt_monitor_free_lls_sls_alc_monitor(lls_slt_monitor); -> lls_sls_alc_monitor_free

            so just clear out our local ref:
                lls_sls_mmt_monitor and lls_sls_alc_monitor
        */
    }

    if(atsc3_link_mapping_table_last) {
        atsc3_link_mapping_table_free(&atsc3_link_mapping_table_last);
    }
    atsc3_link_mapping_table_missing_dropped_packets = 0;

    lls_slt_monitor = lls_slt_monitor_create();
    //wire up a lls event for SLS table
    lls_slt_monitor->atsc3_lls_on_sls_table_present_callback = &atsc3_lls_on_sls_table_present_ndk;
    lls_slt_monitor->atsc3_lls_on_aeat_table_present_callback = &atsc3_lls_on_aeat_table_present_ndk;

    //MMT/MFU callback contexts
    atsc3_mmt_mfu_context = atsc3_mmt_mfu_context_callbacks_default_jni_new();

    //jjustman-2020-12-08 - wire up atsc3_mmt_signalling_information_on_routecomponent_message_present and atsc3_mmt_signalling_information_on_held_message_present
    atsc3_mmt_mfu_context->atsc3_mmt_signalling_information_on_routecomponent_message_present = &atsc3_mmt_signalling_information_on_routecomponent_message_present_ndk;
    atsc3_mmt_mfu_context->atsc3_mmt_signalling_information_on_held_message_present = &atsc3_mmt_signalling_information_on_held_message_present_ndk;
}

void atsc3_lls_sls_alc_on_metadata_fragments_updated_callback_ndk(lls_sls_alc_monitor_t* lls_sls_alc_monitor) {
    //jjustman-2021-07-07 - walk thru our S-TSID and ensure all RS dstIpAddr and dstPort flows are assigned into our lls_slt_monitor alc flows
    int ip_mulitcast_flows_added_count = 0;

    ip_mulitcast_flows_added_count = lls_sls_alc_add_additional_ip_flows_from_route_s_tsid(lls_slt_monitor, lls_sls_alc_monitor, lls_sls_alc_monitor->atsc3_sls_metadata_fragments->atsc3_route_s_tsid);

    if(ip_mulitcast_flows_added_count) {
        //jjustman-2021-07-28 - TODO: re-calculate our distinct IP flows here.. and listen to any additional PLP's as needed
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_lls_sls_alc_on_metadata_fragments_updated_callback_ndk: added %d ip mulitcast flows for alc, TODO: refresh listen plps as needed", ip_mulitcast_flows_added_count);

		atsc3_phy_build_plp_listeners_from_lls_slt_monitor(lls_slt_monitor);
	}
}

lls_sls_alc_monitor_t* atsc3_lls_sls_alc_monitor_create_with_core_service_player_bridge_default_callbacks(atsc3_lls_slt_service_t* atsc3_lls_slt_service) {
    lls_sls_alc_monitor_t* lls_sls_alc_monitor_new = lls_sls_alc_monitor_create();

    lls_sls_alc_monitor_new->lls_sls_monitor_output_buffer_mode.file_dump_enabled = true;
    lls_sls_alc_monitor_new->has_discontiguous_toi_flow = true; //jjustman-2020-07-27 - hack-ish

    lls_sls_alc_monitor_new->atsc3_lls_slt_service = atsc3_lls_slt_service;

    //process any unmapped s-tsid RS dstIpAddr/dPort tuples into our alc flow
	//jjustman-2021-07-28 - process this as a lls_sls_alc_monitor callback so we can listen to any additional plps as needed
    lls_sls_alc_monitor_new->atsc3_lls_sls_alc_on_metadata_fragments_updated_callback = &atsc3_lls_sls_alc_on_metadata_fragments_updated_callback_ndk;

    //wire up event callback for alc close_object notification
    lls_sls_alc_monitor_new->atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_callback = &atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_ndk;

    //wire up event callback for alc MPD patching
    lls_sls_alc_monitor_new->atsc3_lls_sls_alc_on_route_mpd_patched_callback = &atsc3_lls_sls_alc_on_route_mpd_patched_ndk;

    //jjustman-2020-08-05 - also atsc3_lls_sls_alc_on_route_mpd_patched_with_filename_callback
    lls_sls_alc_monitor_new->atsc3_lls_sls_alc_on_package_extract_completed_callback = &atsc3_lls_sls_alc_on_package_extract_completed_callback_ndk;

    //#1569
    lls_sls_alc_monitor_new->atsc3_sls_on_held_trigger_received_callback = &atsc3_sls_on_held_trigger_received_callback_impl;

    return lls_sls_alc_monitor_new;
}


void atsc3_core_service_phy_bridge_init(IAtsc3NdkPHYBridge* atsc3NdkPHYBridge) {
	Atsc3NdkPHYBridge_ptr = atsc3NdkPHYBridge;
	if(Atsc3NdkApplicationBridge_ptr) {
		Atsc3NdkApplicationBridge_ptr->LogMsgF("atsc3_core_service_phy_bridge_init - Atsc3NdkPHYBridge_ptr: %p", Atsc3NdkPHYBridge_ptr);
	}
}



atsc3_slt_broadcast_svc_signalling_t* atsc3_slt_broadcast_svc_signalling_find_from_service_id(uint16_t service_id) {
    atsc3_slt_broadcast_svc_signalling_t* atsc3_slt_broadcast_svc_signalling = NULL;

    bool found_atsc3_slt_broadcast_svc_signalling = false;

    atsc3_lls_slt_service_t* atsc3_lls_slt_service = lls_slt_monitor_find_lls_slt_service_id_group_id_cache_entry(lls_slt_monitor, service_id);
    if(!atsc3_lls_slt_service) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_slt_broadcast_svc_signalling_find_from_service_id: unable to find service_id: %d", service_id);
        return NULL;
    }

    //broadcast_svc_signalling has cardinality (0..1), any other signalling location is represented by SvcInetUrl (0..N)
    for(int i=0; i < atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.count && !found_atsc3_slt_broadcast_svc_signalling; i++) {
        atsc3_slt_broadcast_svc_signalling = atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.data[i];

        if(atsc3_slt_broadcast_svc_signalling->sls_destination_ip_address && atsc3_slt_broadcast_svc_signalling->sls_destination_udp_port) {
            found_atsc3_slt_broadcast_svc_signalling = true;
        }
    }

    if(found_atsc3_slt_broadcast_svc_signalling) {
        return atsc3_slt_broadcast_svc_signalling;
    } else {
        return NULL;
    }
}

//jjustman-2020-11-17 - TODO: also walk thru lls_slt_monitor->lls_slt_service_id
//jjustman-2020-11-18 - TODO - we also need to iterate over our S-TSID for our monitored service_id's to ensure
//                         all IP flows and their corresponding PLP's are listened for
//jjustman-2020-12-08 - TODO: we will also need to monitor for MMT HELD component to check if we need to add its flow for PLP listening

/*
 * jjustman-2021-01-21: NOTE: final PLP selection is not completed until invoking:
 *
 *      Atsc3NdkApplicationBridge_ptr->atsc3_phy_notify_plp_selection_changed(plps_to_listen);
 *
 *   with plps_to_listen as the returned vector from this call, allowing the callee to adjust as needed
 */
vector<uint8_t>  atsc3_phy_build_plp_listeners_from_lls_slt_monitor(lls_slt_monitor_t* lls_slt_monitor) {
    atsc3_slt_broadcast_svc_signalling_t* atsc3_slt_broadcast_svc_signalling = NULL;

    bool found_atsc3_slt_broadcast_svc_signalling = false;
    bool atsc3_phy_notify_plp_selection_changed_called = false;

    int8_t first_plp = -1; //keep track of our "first" PLP for priority use cases, e.g. with LG3307

    set<uint8_t> plps_to_check;         //use this as a temporary collection of de-dup'd PLPs
    set<uint8_t>::iterator it;

    vector<uint8_t> plps_to_listen;
    vector<uint8_t>::iterator it_l;

    if(!atsc3_link_mapping_table_last) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: atsc3_link_mapping_table_last is NULL!");
        return plps_to_listen;
    }

    if(!lls_slt_monitor) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: lls_slt_monitor is NULL!");
        return plps_to_listen;
    }

    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_phy_build_plp_listeners_from_lls_slt_monitor: lls_slt_monitor: %p, lls_slt_monitor->lls_slt_service_id_v.count: %d",
                                             lls_slt_monitor,
                                             lls_slt_monitor->lls_slt_service_id_v.count);

    //acquire our mutex for "pseudo-context" w/ lls_slt_monitor so it won't change out from under (hopefully)..
    {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: before acquiring atsc3_core_service_player_bridge_context_mutex_local - RAI block open");
        lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: acquired atsc3_core_service_player_bridge_context_mutex_local - RAI block open");

        for(int i=0; i < lls_slt_monitor->lls_slt_service_id_v.count; i++) {
            lls_slt_service_id_t* lls_slt_service_id = lls_slt_monitor->lls_slt_service_id_v.data[i];

            //find our cache entry from this service_id
            atsc3_lls_slt_service_t* atsc3_lls_slt_service = lls_slt_monitor_find_lls_slt_service_id_group_id_cache_entry(lls_slt_monitor, lls_slt_service_id->service_id);
            if(!atsc3_lls_slt_service) {
                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: unable to find service_id: %d, skipping from PLP selection!", lls_slt_service_id->service_id);
                continue;
            }

            uint16_t service_id = lls_slt_service_id->service_id;

            //broadcast_svc_signalling has cardinality (0..1), any other signalling location is represented by SvcInetUrl (0..N)
            //jjustman-2021-01-21 - TODO: fix this cardinality mistake
            for(int j=0; j < atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.count; j++) {
                atsc3_slt_broadcast_svc_signalling = atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.data[j];
                uint32_t sls_destination_ip_address = parseIpAddressIntoIntval(atsc3_slt_broadcast_svc_signalling->sls_destination_ip_address);
                uint16_t sls_destination_udp_port = parsePortIntoIntval(atsc3_slt_broadcast_svc_signalling->sls_destination_udp_port);

                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: checking atsc3_slt_broadcast_svc_signalling: sls_protocol: 0x%02x, flow: %u.%u.%u.%u:%u",
                                                         atsc3_slt_broadcast_svc_signalling->sls_protocol,
                                                         __toipandportnonstruct(sls_destination_ip_address, sls_destination_udp_port));

                //jjustman-2020-11-18 - relax this check - for sls_protocol to just ensuring that we have a dst_ip and dst_port and then proceed with LMT matching
                // if(atsc3_slt_broadcast_svc_signalling->sls_protocol == SLS_PROTOCOL_MMTP || atsc3_slt_broadcast_svc_signalling->sls_protocol == SLS_PROTOCOL_ROUTE) {

                if(atsc3_slt_broadcast_svc_signalling->sls_destination_ip_address && atsc3_slt_broadcast_svc_signalling->sls_destination_udp_port) {
                    for(int k=0; k < atsc3_link_mapping_table_last->atsc3_link_mapping_table_plp_v.count && !found_atsc3_slt_broadcast_svc_signalling; k++) {
                        atsc3_link_mapping_table_plp_t* atsc3_link_mapping_table_plp = atsc3_link_mapping_table_last->atsc3_link_mapping_table_plp_v.data[k];

                        for(int l=0; l < atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.count && !found_atsc3_slt_broadcast_svc_signalling; l++) {
                            atsc3_link_mapping_table_multicast_t* atsc3_link_mapping_table_multicast = atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.data[l];

                            if(!atsc3_link_mapping_table_multicast) {
                                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: atsc3_link_mapping_table_plp: %p, index: %d, atsc3_link_mapping_table_multicast is NULL!", atsc3_link_mapping_table_plp, l);
                                continue;
                            }


                            if(atsc3_link_mapping_table_multicast->dst_ip_add == sls_destination_ip_address && atsc3_link_mapping_table_multicast->dst_udp_port == sls_destination_udp_port) {
                                Atsc3NdkApplicationBridge_ptr->LogMsgF("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: SLS adding PLP_id: %d (%s:%s)",
                                        atsc3_link_mapping_table_plp->PLP_ID,
                                        atsc3_slt_broadcast_svc_signalling->sls_destination_ip_address,
                                        atsc3_slt_broadcast_svc_signalling->sls_destination_udp_port);

                                if(first_plp == -1 ) {
                                    first_plp = atsc3_link_mapping_table_plp->PLP_ID;
                                } else {
                                    plps_to_check.insert(atsc3_link_mapping_table_plp->PLP_ID);
                                }
                            }
                        }
                    }
                }
				
				if(atsc3_slt_broadcast_svc_signalling->sls_protocol == SLS_PROTOCOL_MMTP) {
                    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_phy_build_plp_listeners_from_lls_slt_monitor: processing SLS_PROTOCOL_MMTP for service_id: %d, lls_slt_monitor: %p, lls_slt_monitor->lls_slt_service_id_v.count: %d",
                                                             service_id,
                                                             lls_slt_monitor,
                                                             lls_slt_monitor->lls_slt_service_id_v.count);
					//iterate over our lls_slt_monitor's child lls_sls_mmt_monitor and assign any lls_mmt_sessions
					for(int l=0; l < lls_slt_monitor->lls_sls_mmt_session_flows_v.count; l++) {
						lls_sls_mmt_session_flows_t* lls_sls_mmt_session_flows = lls_slt_monitor->lls_sls_mmt_session_flows_v.data[l];
						
						for(int m=0; m < lls_sls_mmt_session_flows->lls_sls_mmt_session_v.count; m++) {
							lls_sls_mmt_session_t* lls_sls_mmt_session = lls_sls_mmt_session_flows->lls_sls_mmt_session_v.data[m];
							if(!lls_sls_mmt_session || !lls_sls_mmt_session->atsc3_lls_slt_service) {
                                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("lls_sls_mmt_session index m: %d, %p is missing atsc3_lls_slt_service", m, lls_sls_mmt_session);
                                continue;
							}

							if(lls_sls_mmt_session->atsc3_lls_slt_service->service_id == service_id) {
								//add any atsc3_mmt_sls_mpt_location_info flows into our lmt
								
								for(int n=0; n < lls_sls_mmt_session->atsc3_mmt_sls_mpt_location_info_v.count; n++) {
									atsc3_mmt_sls_mpt_location_info_t * atsc3_mmt_sls_mpt_location_info = lls_sls_mmt_session->atsc3_mmt_sls_mpt_location_info_v.data[n];
									if(atsc3_mmt_sls_mpt_location_info->location_type == MMT_GENERAL_LOCATION_INFO_LOCATION_TYPE_MMTP_PACKET_FLOW_UDP_IP_V4) {


                                        for(int k=0; k < atsc3_link_mapping_table_last->atsc3_link_mapping_table_plp_v.count; k++) {
                                            atsc3_link_mapping_table_plp_t* atsc3_link_mapping_table_plp = atsc3_link_mapping_table_last->atsc3_link_mapping_table_plp_v.data[k];

                                            for(int p=0; p < atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.count; p++) {
                                                atsc3_link_mapping_table_multicast_t* atsc3_link_mapping_table_multicast = atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.data[p];

                                                if(!atsc3_link_mapping_table_multicast) {
                                                    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: atsc3_link_mapping_table_plp: %p, index: %d, atsc3_link_mapping_table_multicast is NULL!", atsc3_link_mapping_table_plp, p);
                                                    continue;
                                                }

                                                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: MMT checking PLP: %d, with atsc3_link_mapping_table_multicast: %p, atsc3_mmt_sls_mpt_location_info: %p",
                                                                                            atsc3_link_mapping_table_plp->PLP_ID,
                                                                                            atsc3_link_mapping_table_multicast,
                                                                                            atsc3_mmt_sls_mpt_location_info);


                                                if(atsc3_link_mapping_table_multicast->dst_ip_add == atsc3_mmt_sls_mpt_location_info->ipv4_dst_addr && atsc3_link_mapping_table_multicast->dst_udp_port == atsc3_mmt_sls_mpt_location_info->ipv4_dst_port) {
                                                    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: MMT - adding PLP_id: %d (%u.%u.%u.%u:%u) for packet_id: %d",
                                                                                           atsc3_link_mapping_table_plp->PLP_ID,
                                                                                           __toipandportnonstruct(atsc3_mmt_sls_mpt_location_info->ipv4_dst_addr, atsc3_mmt_sls_mpt_location_info->ipv4_dst_port),
                                                                                           atsc3_mmt_sls_mpt_location_info->packet_id);

                                                    if(first_plp == -1 ) {
                                                        first_plp = atsc3_link_mapping_table_plp->PLP_ID;
                                                    } else {
                                                        plps_to_check.insert(atsc3_link_mapping_table_plp->PLP_ID);
                                                    }
                                                }
                                            }
                                        }
									}
								}
							}
						}
					}
				}
            }
        }

        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: releasing atsc3_core_service_player_bridge_context_mutex_local - RAI block closure");
    } //finally, release our RAII context mutex

     if(first_plp == -1) {
         __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: first_plp is -1, this should never happen, bailing!");
         return plps_to_listen; //empty set
     }

     bool has_plp0_in_set = false;

     for(it = plps_to_check.begin(); !has_plp0_in_set && it != plps_to_check.end(); it++) {
         if((*it) == 0) {
             has_plp0_in_set = true;
         }
     }

     //jjustman-2021-02-03 - treat our selected PLP's as priority fifo (e.g selected service, then additional service(s)) to ensure LG3307 demod (which can only reliably decode 1 plp at a time) can pick plps_to_listen.first()

     plps_to_listen.push_back(first_plp);

     if(!has_plp0_in_set) {
        if(plps_to_check.size() < 4) {
            plps_to_listen.push_back(0);
        } else {
            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: plps_to_check contains 4 entries, but is missing plp0!");
        }
    }

    for(it = plps_to_check.begin(); it != plps_to_check.end(); it++) {
        plps_to_listen.push_back(*it);
    }

    sort(plps_to_listen.begin(), plps_to_listen.end());

    //jjustman-2022-07-28 - make sure we don't have duplciate plp's in here, it may 'confuse' some demod devices..
    it_l = unique(plps_to_listen.begin(), plps_to_listen.end());

    plps_to_listen.resize(distance(plps_to_listen.begin(), it_l));

    //if we don't have plp0 in our set, add it first if our size is less than 4 entries (e.g. 0 + 3plps < max of 4)


    //ugh, hack
    if(!plps_to_listen.size()) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: plps_to_check contains 0 entries?! forcing plp[0]: 0");
        plps_to_listen.push_back(0);
    } else if(plps_to_listen.size() == 1) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: final plps_to_listen is: %lu entries: plp[0]: %d", plps_to_listen.size(), plps_to_listen[0]);
    } else if(plps_to_listen.size() == 2) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: final plps_to_listen is: %lu entries: plp[0]: %d, plp[1]: %d", plps_to_listen.size(), plps_to_listen[0], plps_to_listen[1]);
    } else if(plps_to_listen.size() == 3) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: final plps_to_listen is: %lu entries: plp[0]: %d, plp[1]: %d, plp[2]: %d", plps_to_listen.size(), plps_to_listen[0], plps_to_listen[1], plps_to_listen[2]);
    } else if(plps_to_listen.size() == 4) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_phy_update_plp_listeners_from_lls_slt_monitor: final plps_to_listen is: %lu entries: plp[0]: %d, plp[1]: %d, plp[2]: %d, plp[3]: %d", plps_to_listen.size(), plps_to_listen[0], plps_to_listen[1], plps_to_listen[2], plps_to_listen[3]);
    }

    return plps_to_listen;
}

/*
 * atsc3_core_service_player_bridge_set_single_monitor_a331_service_id: set a single service to be monitored
 *
 *      NOTE: will _NOT_ attempt to manage PLP selections, must be configured after this (and any additional services via add_monitor) by:
 *              atsc3_phy_build_plp_listeners_from_lls_slt_monitor(lls_slt_monitor);
 */
atsc3_lls_slt_service_t* atsc3_core_service_player_bridge_set_single_monitor_a331_service_id(int service_id) {
    lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

    //no more global refs
    lls_sls_alc_monitor_t* lls_sls_alc_monitor = NULL;
    lls_sls_mmt_monitor_t* lls_sls_mmt_monitor = NULL;

    //clear out our lls_slt_monitor->lls_slt_service_id
    if(lls_slt_monitor) {
        lls_slt_monitor_clear_lls_slt_service_id(lls_slt_monitor);
    }

    atsc3_lls_slt_service_t* atsc3_lls_slt_service = lls_slt_monitor_find_lls_slt_service_id_group_id_cache_entry(lls_slt_monitor, service_id);
    if(!atsc3_lls_slt_service) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_player_bridge_set_single_monitor_a331_service_id: unable to find service_id: %d", service_id);
        return NULL;
    }

    //find our matching LLS service, then assign a monitor reference
    atsc3_slt_broadcast_svc_signalling_t* atsc3_slt_broadcast_svc_signalling = NULL;
    atsc3_slt_broadcast_svc_signalling_t* atsc3_slt_broadcast_svc_signalling_mmt = NULL;
    atsc3_slt_broadcast_svc_signalling_t* atsc3_slt_broadcast_svc_signalling_route = NULL;

    atsc3_slt_broadcast_svc_signalling = atsc3_slt_broadcast_svc_signalling_find_from_service_id(service_id);
    if(!atsc3_slt_broadcast_svc_signalling) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_player_bridge_set_single_monitor_a331_service_id: atsc3_slt_broadcast_svc_signalling_find_from_service_id: unable to find atsc3_slt_broadcast_svc_signalling_t for service_id: %d", service_id);
        return NULL;
    }

    if(atsc3_slt_broadcast_svc_signalling->sls_protocol == SLS_PROTOCOL_MMTP) {
        atsc3_slt_broadcast_svc_signalling_mmt = atsc3_slt_broadcast_svc_signalling;
    } else if(atsc3_slt_broadcast_svc_signalling->sls_protocol == SLS_PROTOCOL_ROUTE) {
        atsc3_slt_broadcast_svc_signalling_route = atsc3_slt_broadcast_svc_signalling;
    }

    //wire up MMT, watch out for potentally free'd sessions that aren't NULL'd out properly..
    if(atsc3_slt_broadcast_svc_signalling_mmt != NULL) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_core_service_player_bridge_set_single_monitor_a331_service_id: service_id: %d - using MMT with flow: sip: %s, dip: %s:%s",
               service_id,
               atsc3_slt_broadcast_svc_signalling_mmt->sls_source_ip_address,
               atsc3_slt_broadcast_svc_signalling_mmt->sls_destination_ip_address,
               atsc3_slt_broadcast_svc_signalling_mmt->sls_destination_udp_port);

        //clear any active SLS monitors, don't destroy our serviceId flows
        lls_slt_monitor_clear_lls_sls_mmt_monitor(lls_slt_monitor);

        //TODO - remove this logic to a unified process...
        lls_slt_monitor_clear_lls_sls_alc_monitor(lls_slt_monitor);
        lls_slt_monitor->lls_sls_alc_monitor = NULL;

        lls_sls_mmt_monitor = lls_sls_mmt_monitor_create();
        lls_sls_mmt_monitor->transients.atsc3_lls_slt_service = atsc3_lls_slt_service; //transient HACK!
        lls_slt_service_id_t* lls_slt_service_id = lls_slt_service_id_new_from_atsc3_lls_slt_service(atsc3_lls_slt_service);

        lls_slt_monitor_add_lls_slt_service_id(lls_slt_monitor, lls_slt_service_id);

        //we may not be initialized yet, so re-check again later
        //this should _never_happen...
        lls_sls_mmt_session_t* lls_sls_mmt_session = lls_slt_mmt_session_find_from_service_id(lls_slt_monitor, atsc3_lls_slt_service->service_id);

        if(!lls_sls_mmt_session) {
            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_player_bridge_set_single_monitor_a331_service_id: lls_slt_mmt_session_find_from_service_id: lls_sls_mmt_session is NULL!");
        }
        lls_sls_mmt_monitor->transients.lls_mmt_session = lls_sls_mmt_session;
        lls_slt_monitor->lls_sls_mmt_monitor = lls_sls_mmt_monitor;

        lls_slt_monitor_add_lls_sls_mmt_monitor(lls_slt_monitor, lls_sls_mmt_monitor);

        //clear out atsc3_mmt_mfu_context elements, e.g. our ROUTEComponent entry (if present)
        if(atsc3_mmt_mfu_context) {
            if(atsc3_mmt_mfu_context->mmt_atsc3_route_component_monitored) {
                atsc3_mmt_mfu_context->mmt_atsc3_route_component_monitored->__is_pinned_to_context = false;
                mmt_atsc3_route_component_free(&atsc3_mmt_mfu_context->mmt_atsc3_route_component_monitored);
            }
        }

    } else {
        //jjustman-2020-09-17 - use _clear, but keep our lls_sls_mmt_session_flows
        //todo: release any internal lls_sls_mmt_monitor handles
        lls_slt_monitor_clear_lls_sls_mmt_monitor(lls_slt_monitor);
        lls_slt_monitor->lls_sls_mmt_monitor = NULL;
        lls_sls_mmt_monitor = NULL;
    }

    //wire up ROUTE
    if(atsc3_slt_broadcast_svc_signalling_route != NULL) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_core_service_player_bridge_set_single_monitor_a331_service_id: service_id: %d - using ROUTE with flow: sip: %s, dip: %s:%s",
               service_id,
               atsc3_slt_broadcast_svc_signalling_route->sls_source_ip_address,
               atsc3_slt_broadcast_svc_signalling_route->sls_destination_ip_address,
               atsc3_slt_broadcast_svc_signalling_route->sls_destination_udp_port);

        lls_slt_monitor_clear_lls_sls_alc_monitor(lls_slt_monitor);

        lls_sls_alc_monitor = atsc3_lls_sls_alc_monitor_create_with_core_service_player_bridge_default_callbacks(atsc3_lls_slt_service);

        lls_slt_service_id_t* lls_slt_service_id = lls_slt_service_id_new_from_atsc3_lls_slt_service(atsc3_lls_slt_service);
        lls_slt_monitor_add_lls_slt_service_id(lls_slt_monitor, lls_slt_service_id);

        lls_sls_alc_session_t* lls_sls_alc_session = lls_slt_alc_session_find_from_service_id(lls_slt_monitor, atsc3_lls_slt_service->service_id);
        if(!lls_sls_alc_session) {
            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_player_bridge_set_single_monitor_a331_service_id: lls_slt_alc_session_find_from_service_id: lls_sls_alc_session is NULL!");
        }
        lls_sls_alc_monitor->lls_alc_session = lls_sls_alc_session;
        lls_slt_monitor->lls_sls_alc_monitor = lls_sls_alc_monitor;

        lls_slt_monitor_add_lls_sls_alc_monitor(lls_slt_monitor, lls_sls_alc_monitor);

    } else {
        lls_slt_monitor_clear_lls_sls_alc_monitor(lls_slt_monitor);
        if(lls_slt_monitor->lls_sls_alc_monitor) {
            lls_sls_alc_monitor_free(&lls_slt_monitor->lls_sls_alc_monitor);
        }
        lls_sls_alc_monitor = NULL;
    }

    return atsc3_lls_slt_service;
}
//jjustman-2020-11-17 - NOTE: middleware app should invoke this any supplimental services needed for full ATSC 3.0 reception, e.g:
//      ESG, which would be SLT.Service@serviceCategory == 4, or
//      others as defined in A/331:2020 Table 6.4

atsc3_lls_slt_service_t* atsc3_core_service_player_bridge_add_monitor_a331_service_id(int service_id) {
    lls_sls_alc_monitor_t* lls_sls_alc_monitor_to_add = NULL;

    lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_core_service_player_bridge_add_monitor_a331_service_id: with service_id: %d", service_id);

    atsc3_lls_slt_service_t* atsc3_lls_slt_service = lls_slt_monitor_find_lls_slt_service_id_group_id_cache_entry(lls_slt_monitor, service_id);
    if(!atsc3_lls_slt_service) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_player_bridge_add_monitor_a331_service_id: unable to find service_id: %d", service_id);
        return NULL;
    }

    //find our matching LLS service, then assign a monitor reference
    atsc3_slt_broadcast_svc_signalling_t* atsc3_slt_broadcast_svc_signalling_route_to_add_monitor = NULL;

    atsc3_slt_broadcast_svc_signalling_route_to_add_monitor = atsc3_slt_broadcast_svc_signalling_find_from_service_id(service_id);
    if(!atsc3_slt_broadcast_svc_signalling_route_to_add_monitor) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_player_bridge_add_monitor_a331_service_id: atsc3_slt_broadcast_svc_signalling_find_from_service_id: unable to find atsc3_slt_broadcast_svc_signalling_route_to_add_monitor for service_id: %d", service_id);
        return NULL;
    }

    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_core_service_player_bridge_add_monitor_a331_service_id: service_id: %d - adding ROUTE with flow: sip: %s, dip: %s:%s",
           service_id,
           atsc3_slt_broadcast_svc_signalling_route_to_add_monitor->sls_source_ip_address,
           atsc3_slt_broadcast_svc_signalling_route_to_add_monitor->sls_destination_ip_address,
           atsc3_slt_broadcast_svc_signalling_route_to_add_monitor->sls_destination_udp_port);

    lls_sls_alc_monitor_to_add = atsc3_lls_sls_alc_monitor_create_with_core_service_player_bridge_default_callbacks(atsc3_lls_slt_service);

    lls_slt_service_id_t* lls_slt_service_id = lls_slt_service_id_new_from_atsc3_lls_slt_service(atsc3_lls_slt_service);
    lls_slt_monitor_add_lls_slt_service_id(lls_slt_monitor, lls_slt_service_id);

    //jjustman-2020-11-17 - add lls_sls_alc_session_flows_v

    lls_sls_alc_session_t* lls_sls_alc_session = lls_slt_alc_session_find_from_service_id(lls_slt_monitor, atsc3_lls_slt_service->service_id);
    if(!lls_sls_alc_session) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_player_bridge_add_monitor_a331_service_id: lls_slt_alc_session_find_from_service_id: %d, lls_sls_alc_session is NULL!", service_id);
        return NULL;
    }
    lls_sls_alc_monitor_to_add->lls_alc_session = lls_sls_alc_session;
    lls_slt_monitor_add_lls_sls_alc_monitor(lls_slt_monitor, lls_sls_alc_monitor_to_add);

    //add in supplimentary callback hook for additional ALC emissions
    lls_sls_alc_monitor_to_add->atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_callback = &atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_ndk;

    //add a supplimentry sls_alc monitor
    // TODO: fix me? NOTE: do not replace the primary lls_slt_monitor->lls_sls_alc_monitor entry if set
    if(!lls_slt_monitor->lls_sls_alc_monitor) {
        lls_slt_monitor->lls_sls_alc_monitor = lls_sls_alc_monitor_to_add;
    }

    return atsc3_lls_slt_service;
}

atsc3_lls_slt_service_t* atsc3_core_service_player_bridge_remove_monitor_a331_service_id(int service_id) {
    lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

    if(!lls_slt_monitor->lls_sls_alc_monitor || !lls_slt_monitor->lls_sls_alc_monitor_v.count) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_player_bridge_remove_monitor_a331_service_id: unable to remove service_id: %d, lls_slt_monitor->lls_sls_alc_monitor is: %p, lls_slt_monitor->lls_sls_alc_monitor_v.count is: %d",
                service_id,
                lls_slt_monitor->lls_sls_alc_monitor,
                lls_slt_monitor->lls_sls_alc_monitor_v.count);
        return NULL;
    }

    atsc3_lls_slt_service_t* lls_service_removed_lls_sls_alc_monitor = NULL;
    lls_sls_alc_monitor_t* my_lls_sls_alc_monitor_entry_to_release = NULL;
    if(lls_slt_monitor->lls_sls_alc_monitor && lls_slt_monitor->lls_sls_alc_monitor->atsc3_lls_slt_service->service_id == service_id) {
        my_lls_sls_alc_monitor_entry_to_release = lls_slt_monitor->lls_sls_alc_monitor;
        lls_slt_monitor->lls_sls_alc_monitor = NULL;
    }

    for(int i=0; i < lls_slt_monitor->lls_sls_alc_monitor_v.count && !my_lls_sls_alc_monitor_entry_to_release; i++) {
        lls_sls_alc_monitor_t* lls_sls_alc_monitor = lls_slt_monitor->lls_sls_alc_monitor_v.data[i];
        if(lls_sls_alc_monitor->atsc3_lls_slt_service->service_id == service_id) {
            //clear out last entry
            lls_slt_monitor_remove_lls_sls_alc_monitor(lls_slt_monitor, lls_sls_alc_monitor);
        }
    }

    if(my_lls_sls_alc_monitor_entry_to_release) {
        lls_service_removed_lls_sls_alc_monitor = my_lls_sls_alc_monitor_entry_to_release->atsc3_lls_slt_service;
        lls_sls_alc_monitor_free(&my_lls_sls_alc_monitor_entry_to_release);
    }

    return lls_service_removed_lls_sls_alc_monitor;
}

//jjustman-2020-09-02 - note: non-mutex protected method, expects caller to already own a mutex for lls_slt_monitor
lls_sls_alc_monitor_t* atsc3_lls_sls_alc_monitor_get_from_service_id(int service_id) {

    if(!lls_slt_monitor->lls_sls_alc_monitor || !lls_slt_monitor->lls_sls_alc_monitor_v.count) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_lls_sls_alc_monitor_get_from_service_id: error searching for service_id: %d, alc_monitor is null: lls_slt_monitor->lls_sls_alc_monitor is: %p, lls_slt_monitor->lls_sls_alc_monitor_v.count is: %d",
                service_id,
                lls_slt_monitor->lls_sls_alc_monitor,
                lls_slt_monitor->lls_sls_alc_monitor_v.count);
        return NULL;
    }

    lls_sls_alc_monitor_t* lls_sls_alc_monitor_to_return = NULL;
    for(int i=0; i < lls_slt_monitor->lls_sls_alc_monitor_v.count && !lls_sls_alc_monitor_to_return; i++) {
        lls_sls_alc_monitor_t* lls_sls_alc_monitor = lls_slt_monitor->lls_sls_alc_monitor_v.data[i];
        if(lls_sls_alc_monitor->atsc3_lls_slt_service->service_id == service_id) {
            lls_sls_alc_monitor_to_return = lls_sls_alc_monitor;
            continue;
        }
    }

    if(!lls_sls_alc_monitor_to_return) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_lls_sls_alc_monitor_get_from_service_id: service_id: %d, lls_sls_alc_monitor_to_return is null!",
                service_id);
        return NULL;
    }

    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_lls_sls_alc_monitor_get_from_service_id: %d, returning lls_sls_alc_monitor_to_return: %p",
           service_id,
           lls_sls_alc_monitor_to_return);

    return lls_sls_alc_monitor_to_return;
}

atsc3_sls_metadata_fragments_t* atsc3_slt_alc_get_sls_metadata_fragments_from_monitor_service_id(int service_id) {
    lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

    lls_sls_alc_monitor_t* lls_sls_alc_monitor = atsc3_lls_sls_alc_monitor_get_from_service_id(service_id);

    if(!lls_sls_alc_monitor || !lls_sls_alc_monitor->atsc3_sls_metadata_fragments) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_slt_alc_get_sls_metadata_fragments_from_monitor_service_id: service_id: %d, alc_monitor or fragments were null, lls_sls_alc_monitor: %p", service_id, lls_sls_alc_monitor);
        return NULL;
    }

    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_slt_alc_get_sls_metadata_fragments_from_monitor_service_id: %d, returning atsc3_sls_metadata_fragments: %p",
           service_id,
           lls_sls_alc_monitor->atsc3_sls_metadata_fragments);

    return lls_sls_alc_monitor->atsc3_sls_metadata_fragments;
}

atsc3_route_s_tsid_t* atsc3_slt_alc_get_sls_route_s_tsid_from_monitor_service_id(int service_id) {
    lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

    lls_sls_alc_monitor_t* lls_sls_alc_monitor = atsc3_lls_sls_alc_monitor_get_from_service_id(service_id);

    if(!lls_sls_alc_monitor || !lls_sls_alc_monitor->atsc3_sls_metadata_fragments || !lls_sls_alc_monitor->atsc3_sls_metadata_fragments->atsc3_route_s_tsid) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_slt_alc_get_sls_route_s_tsid_from_monitor_service_id: service_id: %d, alc_monitor or fragments or route_s_tsid were null, lls_sls_alc_monitor: %p", service_id, lls_sls_alc_monitor);
        return NULL;
    }

    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_slt_alc_get_sls_route_s_tsid_from_monitor_service_id: %d, returning atsc3_route_s_tsid: %p",
           service_id,
           lls_sls_alc_monitor->atsc3_sls_metadata_fragments->atsc3_route_s_tsid);

    return lls_sls_alc_monitor->atsc3_sls_metadata_fragments->atsc3_route_s_tsid;
}

/**
 * else {
        lls_slt_monitor_clear_lls_sls_alc_monitor(lls_slt_monitor);
        if(lls_slt_monitor->lls_sls_alc_monitor) {
            lls_sls_alc_monitor_free(&lls_slt_monitor->lls_sls_alc_monitor);
        }

 * @param packet
 */


//jjustman-2020-08-18 - todo: keep track of plp_num's?
void atsc3_core_service_bridge_process_packet_from_plp_and_block(uint8_t plp_num, block_t* block) {
    //jjustman-2021-01-19 - only process packets if we have a link mapping table for <flow, PLP> management
    if(!atsc3_link_mapping_table_last) {
        if((atsc3_link_mapping_table_missing_dropped_packets++ % 1000) == 0) {
            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_from_plp_and_block: dropping due to atsc3_link_mappping_table_last == NULL, plp: %d, dropped_packets: %d", plp_num, atsc3_link_mapping_table_missing_dropped_packets);
        }
        return;
    }

	atsc3_core_service_bridge_process_packet_phy(block);
}

int mdi_multicast_emission_fd = -1;
struct sockaddr_in mdi_multicast_emission_addr = { 0 };


//void process_packet(u_char *user, const struct pcap_pkthdr *pkthdr, const u_char *packet) {
void atsc3_core_service_bridge_process_packet_phy(block_t* packet) {
    lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

    //alc types
    lls_sls_alc_session_t*                  matching_lls_slt_alc_session = NULL;
    atsc3_alc_packet_t*                     alc_packet = NULL;
    bool                                    has_matching_lls_slt_service_id = false;

    //mmt types
    lls_sls_mmt_session_t*                  matching_lls_sls_mmt_session = NULL;
    mmtp_packet_header_t*                   mmtp_packet_header = NULL;
    mmtp_asset_t*                           mmtp_asset = NULL;
    mmtp_packet_id_packets_container_t*     mmtp_packet_id_packets_container = NULL;

    mmtp_mpu_packet_t*                      mmtp_mpu_packet = NULL;
    mmtp_signalling_packet_t*               mmtp_signalling_packet = NULL;
    int8_t                                  mmtp_si_parsed_message_count = 0;

    //udp_packet_t* udp_packet = udp_packet_process_from_ptr_raw_ethernet_packet(block_Get(packet), packet->p_size);
    udp_packet_t* udp_packet = udp_packet_process_from_ptr(block_Get(packet), packet->p_size);

    if(Atsc3NdkApplicationBridge_ptr->atsc3_get_demod_pcap_capture()) {
        atsc3_pcap_writer_iterate_packet(Atsc3NdkApplicationBridge_ptr->atsc3_pcap_writer_context_get(), packet);
    }

    if(!udp_packet) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_bridge_process_packet_phy: after udp_packet_process_from_ptr: unable to extract packet size: %d, i_pos: %d, 0x%02x 0x%02x",
                packet->p_size,
                packet->i_pos,
                packet->p_buffer[0],
                packet->p_buffer[1]);

        goto error;
    }

    //jjustman-2021-02-18 - special udp:port for MDI (DRM) support flow from lab at 239.255.50.69:31337
    if(udp_packet->udp_flow.dst_ip_addr == 4026479173 && udp_packet->udp_flow.dst_port == 31337) {
        if((_ATSC3_CORE_SERVICE_PLAYER_BRIDGE_atsc3_core_service_bridge_process_packet_phy_got_mdi_counter++ < 10) || ((_ATSC3_CORE_SERVICE_PLAYER_BRIDGE_atsc3_core_service_bridge_process_packet_phy_got_mdi_counter % 100) == 0)) {
            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_core_service_bridge_process_packet_phy: got MDI/DRM packet at 239.255.50.69:31337, raw packet: len: %d, i_pos: %d, 0x%02x 0x%02x, datagram: len: %d, i_pos: %d, 0x%02x 0x%02x, got_mdi_counter: %d",
                                                 packet->p_size,
                                                 packet->i_pos,
                                                 packet->p_buffer[0],
                                                 packet->p_buffer[1],
                                                 udp_packet->data->p_size,
                                                 udp_packet->data->i_pos,
                                                 udp_packet->data->p_buffer[0],
                                                 udp_packet->data->p_buffer[1],
                                                 _ATSC3_CORE_SERVICE_PLAYER_BRIDGE_atsc3_core_service_bridge_process_packet_phy_got_mdi_counter);
        }

        if(mdi_multicast_emission_fd == -1) {
            mdi_multicast_emission_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (mdi_multicast_emission_fd < 0) {
                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_bridge_process_packet_phy: unable to open socket(AF_INET, SOCK_DGRAM, 0)");

                goto error;
            }

            // set up destination address
            memset(&mdi_multicast_emission_addr, 0, sizeof(mdi_multicast_emission_addr));
            mdi_multicast_emission_addr.sin_family = AF_INET;
            mdi_multicast_emission_addr.sin_addr.s_addr = inet_addr("239.255.50.69");
            mdi_multicast_emission_addr.sin_port = htons(31337);
        }


        // now just sendto() our destination!
        char ch = 0;
        int nbytes = sendto(mdi_multicast_emission_fd, block_Get(udp_packet->data), block_Remaining_size(udp_packet->data), 0, (struct sockaddr*) &mdi_multicast_emission_addr, sizeof(mdi_multicast_emission_addr));
        if (nbytes < 0) {
            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_bridge_process_packet_phy: unable to call sendto, nbytes: %d", nbytes);
        }

        goto cleanup;
    }

    //drop mdNS
    if(udp_packet->udp_flow.dst_ip_addr == UDP_FILTER_MDNS_IP_ADDRESS && udp_packet->udp_flow.dst_port == UDP_FILTER_MDNS_PORT) {
        goto cleanup;
    }

    //don't auto-select service here, let the lls_slt_monitor->atsc3_lls_on_sls_table_present event callback trigger in a service selection
    if(udp_packet->udp_flow.dst_ip_addr == LLS_DST_ADDR && udp_packet->udp_flow.dst_port == LLS_DST_PORT) {
        //note: lls_slt_table_perform_update will be called via lls_table_create_or_update_from_lls_slt_monitor if the SLT version changes
        atsc3_lls_table_t* original_lls_table = lls_table_create_or_update_from_lls_slt_monitor(lls_slt_monitor, udp_packet->data);
        atsc3_lls_table_t* lls_table = atsc3_lls_table_find_slt_if_signedMultiTable(original_lls_table);

        if(lls_table) {
            if(lls_table->lls_table_id == SLT) {
                lls_dump_instance_table(lls_table);

#ifdef __ADO_25189_WORKAROUND_FOR_TRANSIENT_SLT_SERVICES
                //jjustman-2022-02-09 - workaround for transient SLT entries that we are still monitoring but may have 'dissapeared' temporarly...
                //keep track with a vector, as lls_slt_monitor will be cleared on first call to atsc3_core_service_player_bridge_set_single_monitor_a331_service_id,
                // or contain duplicate entries from atsc3_core_service_player_bridge_add_monitor_a331_service_id

                vector<uint16_t> slt_service_ids_missing;
                vector<uint16_t>::iterator it;

                if(lls_slt_monitor->lls_slt_service_id_v.count) {
                    for(int i=0; i < lls_slt_monitor->lls_slt_service_id_v.count; i++) {
                        bool has_matching_alc_or_mmt_monitor = false;

                        lls_slt_service_id_t* lls_slt_service_id = lls_slt_monitor->lls_slt_service_id_v.data[i];

                        //see if we can find this service_id in our current slt table
                        atsc3_lls_slt_service_t* atsc3_lls_slt_service = lls_slt_monitor_find_lls_slt_service_id_group_id_cache_entry(lls_slt_monitor, lls_slt_service_id->service_id);
                        if(!atsc3_lls_slt_service) {
                            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: after SLT update, unable to find service_id: %d", lls_slt_service_id->service_id);
                            continue;
                        }

                        for(int j=0; !has_matching_alc_or_mmt_monitor && j < lls_slt_monitor->lls_sls_alc_monitor_v.count; j++) {
                            lls_sls_alc_monitor_t* lls_sls_alc_monitor = lls_slt_monitor->lls_sls_alc_monitor_v.data[j];
                            if(lls_sls_alc_monitor->atsc3_lls_slt_service && lls_sls_alc_monitor->atsc3_lls_slt_service->service_id == lls_slt_service_id->service_id) {
                                has_matching_alc_or_mmt_monitor = true;
                                break;
                            }
                        }

                        for(int j=0; !has_matching_alc_or_mmt_monitor && j < lls_slt_monitor->lls_sls_mmt_monitor_v.count; j++) {
                            lls_sls_mmt_monitor_t* lls_sls_mmt_monitor = lls_slt_monitor->lls_sls_mmt_monitor_v.data[j];
                            if(lls_sls_mmt_monitor->transients.atsc3_lls_slt_service && lls_sls_mmt_monitor->transients.atsc3_lls_slt_service->service_id == lls_slt_service_id->service_id) {
                                has_matching_alc_or_mmt_monitor = true;
                                break;
                            }
                        }

                        if(!has_matching_alc_or_mmt_monitor) {
                            slt_service_ids_missing.push_back(lls_slt_service_id->service_id);
                        }
                    }
                }

                if(slt_service_ids_missing.size()) {
                    bool has_called_set_single_monitor = false;
                    for(it = slt_service_ids_missing.begin(); it != slt_service_ids_missing.end(); it++) {
                        uint16_t to_add_service_id = *it;
                        atsc3_lls_slt_service_t* atsc3_lls_slt_service_added_to_monitor = NULL;

                        if(!has_called_set_single_monitor) {
                            atsc3_lls_slt_service_added_to_monitor = atsc3_core_service_player_bridge_set_single_monitor_a331_service_id(to_add_service_id);
                            if(atsc3_lls_slt_service_added_to_monitor) {
                                has_called_set_single_monitor = true;
                                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: after SLT update, missing monitor for service_id: %d, after atsc3_core_service_player_bridge_set_single_monitor_a331_service_id, atsc3_lls_slt_service_added_to_monitor: %p",
                                                                        to_add_service_id, atsc3_lls_slt_service_added_to_monitor);
                            } else {
                                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_bridge_process_packet_phy: after SLT update, missing monitor for service_id: %d, failed atsc3_core_service_player_bridge_set_single_monitor_a331_service_id, atsc3_lls_slt_service_added_to_monitor is NULL",
                                                                         to_add_service_id);
                            }
                        } else {
                            atsc3_lls_slt_service_added_to_monitor = atsc3_core_service_player_bridge_add_monitor_a331_service_id(to_add_service_id);
                            if(atsc3_lls_slt_service_added_to_monitor) {
                                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: after SLT update, missing monitor for service_id: %d, after atsc3_core_service_player_bridge_add_monitor_a331_service_id, atsc3_lls_slt_service_added_to_monitor: %p",
                                                                        to_add_service_id, atsc3_lls_slt_service_added_to_monitor);
                            } else {
                                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_bridge_process_packet_phy: after SLT update, missing monitor for service_id: %d, failed atsc3_core_service_player_bridge_add_monitor_a331_service_id, atsc3_lls_slt_service_added_to_monitor is NULL",
                                                                         to_add_service_id);
                            }

                        }
                    }
                }
#endif
            } else {
                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_INFO("atsc3_core_service_bridge_process_packet_phy: lls_table_id: %d, lls_group_id: %d, lls_table_version: %d", lls_table->lls_table_id, lls_table->lls_group_id, lls_table->lls_table_version);
            }
        } else {
            //LLS_table may not have been updated (e.g. lls_table_version has not changed), or unable to be parsed
        }
        goto cleanup;
    }

    __DEBUG("IP flow packet: dst_ip_addr: %u.%u.%u.%u:%u, pkt_len: %d",
           __toipnonstruct(udp_packet->udp_flow.dst_ip_addr),
           udp_packet->udp_flow.dst_port,
           udp_packet->data->p_size);

    //ALC: Find a matching SLS service from this packet flow, and if the selected atsc3_lls_slt_service is monitored, write MBMS/MPD and MDE's out to disk
    matching_lls_slt_alc_session = lls_slt_alc_session_find_from_udp_packet(lls_slt_monitor, udp_packet->udp_flow.src_ip_addr, udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port);

    //jjustman-2020-11-17 - TODO: filter this out to lls_slt_monitor->lls_slt_service_id_v for discrimination
    //(lls_sls_alc_monitor->atsc3_lls_slt_service->service_id == matching_lls_slt_alc_session->atsc3_lls_slt_service->service_id)
//       ((dst_ip_addr_filter != NULL && dst_ip_port_filter != NULL) && (udp_packet->udp_flow.dst_ip_addr == *dst_ip_addr_filter && udp_packet->udp_flow.dst_port == *dst_ip_port_filter))) {
//    if((lls_sls_alc_monitor && matching_lls_slt_alc_session && lls_sls_alc_monitor->atsc3_lls_slt_service && has_matching_lls_slt_service_id)  ||

    if(lls_slt_monitor->lls_sls_alc_monitor && matching_lls_slt_alc_session) {
        for (int i = 0; i < lls_slt_monitor->lls_slt_service_id_v.count && !has_matching_lls_slt_service_id; i++) {
            lls_slt_service_id_t *lls_slt_service_id_to_check = lls_slt_monitor->lls_slt_service_id_v.data[i];
            /*
             * jjustman-2021-03-23 - add additional checks
             *
                Build fingerprint: 'qti/sdm660_64/sdm660_64:9/jjj/root02040230:userdebug/test-keys'
                Revision: '0'
                ABI: 'arm64'
                pid: 5327, tid: 5681, name: SaankhyaPHYAndr  >>> com.nextgenbroadcast.mobile.middleware.sample <<<
                signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0
                Cause: null pointer dereference
                    x0  0000000000000000  x1  00000000ac10c801  x2  00000000efff4601  x3  000000000000138d
                    x4  00000073ed6d9054  x5  00000073ed6d67f8  x6  746163696c707061  x7  736d626d2f6e6f69
                    x8  00000073ed055b90  x9  0000000000000000  x10 000000000000138b  x11 0000000000000001
                    x12 00000073eeaae851  x13 746163696c707061  x14 aaaaaaaaaaaaaaab  x15 0000000000000001
                    x16 00000073eeaed6d0  x17 00000073ee9dd8b0  x18 0000000000000000  x19 00000073ed0574f0
                    x20 00000073ed0574f0  x21 00000073ed0574f0  x22 0000007402a9be90  x23 00000073edee8c10
                    x24 00000073ed057570  x25 00000073ecf5a000  x26 00000074915535e0  x27 000000000000000b
                    x28 0000007fcbc4fc20  x29 00000073ed056e70
                    sp  00000073ed055430  lr  00000073eea9c528  pc  00000073eea9c5e0

                backtrace:
                    #00 pc 00000000001155e0  /data/app/com.nextgenbroadcast.mobile.middleware.sample-OXzmqynlUoyraucDnZ_FZg==/lib/arm64/libatsc3_core.so (atsc3_core_service_bridge_process_packet_phy+4192)
                    #01 pc 0000000000114570  /data/app/com.nextgenbroadcast.mobile.middleware.sample-OXzmqynlUoyraucDnZ_FZg==/lib/arm64/libatsc3_core.so (atsc3_core_service_bridge_process_packet_from_plp_and_block+524)
             */
            if(!lls_slt_service_id_to_check) {
                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("lls_slt_service_id_to_check is NULL! index: %d, lls_slt_monitor->lls_slt_service_id_v.count: %d", i, lls_slt_monitor->lls_slt_service_id_v.count)
            } else if(!matching_lls_slt_alc_session->atsc3_lls_slt_service) {
                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("matching_lls_slt_alc_session->atsc3_lls_slt_service is NULL! matching_lls_slt_alc_session: %p", matching_lls_slt_alc_session);
            } else if (lls_slt_service_id_to_check->service_id == matching_lls_slt_alc_session->atsc3_lls_slt_service->service_id) {
                has_matching_lls_slt_service_id = true;
            }
        }

        if (has_matching_lls_slt_service_id) {

            //parse and process ALC flow
            int retval = alc_rx_analyze_packet_a331_compliant((char *) block_Get(udp_packet->data), block_Remaining_size(udp_packet->data), &alc_packet);
            if (!retval) {
                //__DEBUG("atsc3_core_service_bridge_process_packet_phy: alc_packet: %p, tsi: %d, toi: %d", alc_packet, alc_packet->def_lct_hdr->tsi, alc_packet->def_lct_hdr->toi);

                //check our alc_packet for a wrap-around TOI value, if it is a monitored TSI, and re-patch the MBMS MPD for updated availabilityStartTime and startNumber with last closed TOI values
                atsc3_alc_packet_check_monitor_flow_for_toi_wraparound_discontinuity(alc_packet, lls_slt_monitor->lls_sls_alc_monitor);

                //keep track of our EXT_FTI and update last_toi as needed for TOI length and manual set of the close_object flag
                atsc3_route_object_t* atsc3_route_object = atsc3_alc_persist_route_object_lct_packet_received_for_lls_sls_alc_monitor_all_flows(alc_packet, lls_slt_monitor->lls_sls_alc_monitor);

                //persist to disk, process sls mbms and/or emit ROUTE media_delivery_event complete to the application tier if
                //the full packet has been recovered (e.g. no missing data units in the forward transmission)
                if (atsc3_route_object) {
                    atsc3_alc_packet_persist_to_toi_resource_process_sls_mbms_and_emit_callback(&udp_packet->udp_flow, alc_packet, lls_slt_monitor->lls_sls_alc_monitor, atsc3_route_object);
                    goto cleanup;
                } else {
                    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_bridge_process_packet_phy: Error in ALC persist, atsc3_route_object is NULL!");
                }
            } else {
                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_ERROR("atsc3_core_service_bridge_process_packet_phy: Error in ALC decode: %d", retval);
            }
            goto error;
        }
    }

    //jjustman-2020-11-12 - TODO: extract this core MMT logic out for ExoPlayer MMT native depacketization

    //MMT: Find a matching SLS service from this packet flow, and if the selected atsc3_lls_slt_service is monitored, enqueue for MFU DU re-constituion and emission
    matching_lls_sls_mmt_session = lls_sls_mmt_session_find_from_udp_packet(lls_slt_monitor, udp_packet->udp_flow.src_ip_addr, udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port);

    if(matching_lls_sls_mmt_session && lls_slt_monitor && lls_slt_monitor->lls_sls_mmt_monitor && lls_slt_monitor->lls_sls_mmt_monitor->transients.atsc3_lls_slt_service) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy: found candidate matching_lls_sls_mmt_session: %p, matching_lls_sls_mmt_session->atsc3_lls_slt_service->service_id: %d, lls_sls_mmt_monitor service_id: %d, checking flow: %u.%u.%u.%u:%u,",
                                                 matching_lls_sls_mmt_session,
                                                 matching_lls_sls_mmt_session->atsc3_lls_slt_service ? matching_lls_sls_mmt_session->atsc3_lls_slt_service->service_id : -1,
                                                 lls_slt_monitor->lls_sls_mmt_monitor->transients.atsc3_lls_slt_service->service_id,
                                                 __toipandportnonstruct(udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port));

    }

	if(matching_lls_sls_mmt_session && matching_lls_sls_mmt_session->atsc3_lls_slt_service && lls_slt_monitor && lls_slt_monitor->lls_sls_mmt_monitor && lls_slt_monitor->lls_sls_mmt_monitor->transients.atsc3_lls_slt_service && matching_lls_sls_mmt_session->atsc3_lls_slt_service->service_id == lls_slt_monitor->lls_sls_mmt_monitor->transients.atsc3_lls_slt_service->service_id) {

        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy: USING candidate matching_lls_sls_mmt_session: %p, matching_lls_sls_mmt_session->atsc3_lls_slt_service->service_id: %d, lls_sls_mmt_monitor service_id: %d, checking flow: %u.%u.%u.%u:%u,",
                                                 matching_lls_sls_mmt_session,
                                                 matching_lls_sls_mmt_session->atsc3_lls_slt_service ? matching_lls_sls_mmt_session->atsc3_lls_slt_service->service_id : -1,
                                                 lls_slt_monitor->lls_sls_mmt_monitor->transients.atsc3_lls_slt_service->service_id,
                                                 __toipandportnonstruct(udp_packet->udp_flow.dst_ip_addr, udp_packet->udp_flow.dst_port));
        if(!atsc3_mmt_mfu_context) {
            goto error;
        }

        mmtp_packet_header = mmtp_packet_header_parse_from_block_t(udp_packet->data);

        if(!mmtp_packet_header) {
            goto error;
        }
        mmtp_packet_header_dump(mmtp_packet_header);

        //for filtering MMT flows by a specific packet_id
        if(dst_packet_id_filter && *dst_packet_id_filter != mmtp_packet_header->mmtp_packet_id) {
            goto error;
        }

        mmtp_asset = atsc3_mmt_mfu_context_mfu_depacketizer_context_update_find_or_create_mmtp_asset(atsc3_mmt_mfu_context, udp_packet, lls_slt_monitor, matching_lls_sls_mmt_session);
        mmtp_packet_id_packets_container = atsc3_mmt_mfu_context_mfu_depacketizer_update_find_or_create_mmtp_packet_id_packets_container(atsc3_mmt_mfu_context, mmtp_asset, mmtp_packet_header);

        if(mmtp_packet_header->mmtp_payload_type == 0x0) {
            mmtp_mpu_packet = mmtp_mpu_packet_parse_and_free_packet_header_from_block_t(&mmtp_packet_header, udp_packet->data);
            if(!mmtp_mpu_packet) {
                goto error;
            }

            if(mmtp_mpu_packet->mpu_timed_flag == 1) {
                mmtp_mpu_dump_header(mmtp_mpu_packet);

                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy: mmtp_mfu_process_from_payload_with_context with udp_packet: %p, mmtp_mpu_packet: %p, atsc3_mmt_mfu_context: %p,", udp_packet, mmtp_mpu_packet, atsc3_mmt_mfu_context);

                mmtp_mfu_process_from_payload_with_context(udp_packet, mmtp_mpu_packet, atsc3_mmt_mfu_context);
                goto cleanup;

            } else {
                //non-timed -
                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: mmtp_packet_header_parse_from_block_t - non-timed payload: packet_id: %u", mmtp_mpu_packet->mmtp_packet_id);
                goto error;
            }
        } else if(mmtp_packet_header->mmtp_payload_type == 0x2) {

            mmtp_signalling_packet = mmtp_signalling_packet_parse_and_free_packet_header_from_block_t(&mmtp_packet_header, udp_packet->data);
            if(!mmtp_signalling_packet) {
                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: mmtp_signalling_packet_parse_and_free_packet_header_from_block_t - mmtp_signalling_packet was NULL for udp_packet: %p, udp_packet->p_size: %d", udp_packet, udp_packet->data->p_size);
                goto error;
            }

            mmtp_packet_id_packets_container = mmtp_asset_find_or_create_packets_container_from_mmtp_signalling_packet(mmtp_asset, mmtp_signalling_packet);

            if(mmtp_signalling_packet->si_fragmentation_indicator == 0x0) {
                //process this SI message in-line, no need for re-assembly
                mmtp_si_parsed_message_count = mmt_signalling_message_parse_packet_with_sls_monitor(lls_slt_monitor->lls_sls_mmt_monitor, mmtp_signalling_packet, udp_packet->data);

                //but clear out any possible mmtp_signalling_packets being queued for re-assembly in mmtp_packet_id_packets_container
                mmtp_packet_id_packets_container_free_mmtp_signalling_packet(mmtp_packet_id_packets_container);

            } else {
                //TODO: jjustman-2019-10-03 - if signalling_packet == MP_table, set atsc3_mmt_mfu_context->mp_table_last;

                //if mmtp_signalling_packet.sl_fragmentation_indicator != 00, then
                //  handle any fragmented signallling_information packets by packet_id,
                //  persisting for depacketization into packet_buffer[] when si_fragmentation_indicator:
                //       must start with f_i: 0x1 (01)
                //          any non 0x0 (00) or 0x1 (01) with no packet_buffer[].length should be discarded
                //       next packet contains: congruent packet_sequence_number (eg old + 1 (mod 2^32 for wraparound) == new)
                //          f_i: 0x2 (10) -> append
                //          f_i: 0x3 (11) -> check for completeness
                //                          -->should have packet_buffer[0].fragmentation_counter == packet_buffer[].length
                //   mmtp_signalling_packet_process_from_payload_with_context(udp_packet, mmtp_signalling_packet, atsc3_mmt_mfu_context);

                if(mmtp_signalling_packet->si_fragmentation_indicator != 0x1 && mmtp_packet_id_packets_container->mmtp_signalling_packet_v.count == 0) {
                    //we should never have a case where fragmentation_indicator is _not_ the first fragment of a signalling message and have 0 packets in the re-assembly vector,
                    //it means we lost at least one previous DU for this si_messgae, so discard and goto error
                    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: mmtp_signalling_packet->si_fragmentation_indicator is: 0x%02x while mmtp_packet_id_packets_container->mmtp_signalling_packet_v.count is 0, discarding!", mmtp_signalling_packet->si_fragmentation_indicator);

                    goto error;
                }

                //push our first mmtp_signalling_packet for re-assembly (explicit mmtp_signalling_packet->si_fragmentation_indicator == 0x1 (01))
                if(mmtp_signalling_packet->si_fragmentation_indicator == 0x1 && mmtp_packet_id_packets_container->mmtp_signalling_packet_v.count == 0) {
                    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_DEBUG("atsc3_core_service_bridge_process_packet_phy: mmtp_packet_id_packets_container_add_mmtp_signalling_packet - adding first entry, mmtp_signalling_packet: %p, mmtp_signalling_packet->si_fragmentation_indicator: 0x%02x, mmtp_signalling_packet->si_fragmentation_counter: %d (A: %d, H: %d)",
                                 mmtp_signalling_packet,
                                 mmtp_signalling_packet->si_fragmentation_indicator,
                                 mmtp_signalling_packet->si_fragmentation_counter,
                                 mmtp_signalling_packet->si_aggregation_flag,
                                 mmtp_signalling_packet->si_additional_length_header);

                    mmtp_packet_id_packets_container_add_mmtp_signalling_packet(mmtp_packet_id_packets_container, mmtp_signalling_packet);
                    mmtp_signalling_packet = NULL;
                    goto cleanup; //continue on

                } else {
                    mmtp_signalling_packet_t *last_mmtp_signalling_packet = mmtp_packet_id_packets_container->mmtp_signalling_packet_v.data[mmtp_packet_id_packets_container->mmtp_signalling_packet_v.count - 1];

                    //make sure our packet_sequence_number is sequential to our last mmtp_signalling_packet
                    if((last_mmtp_signalling_packet->packet_sequence_number + 1) != mmtp_signalling_packet->packet_sequence_number) {
                        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: packet_sequence_number mismatch, discarding! last_mmtp_signalling_packet: %p, psn: %d (next: %d), frag: 0x%02x, frag_counter: %d, mmtp_signalling_packet: %p, psn: %d, frag: 0x%02x, frag_counter: %d",
                                    last_mmtp_signalling_packet,
                                    last_mmtp_signalling_packet->packet_sequence_number,
                                    (uint32_t)(last_mmtp_signalling_packet->packet_sequence_number+1),
                                    last_mmtp_signalling_packet->si_fragmentation_indicator,
                                    last_mmtp_signalling_packet->si_fragmentation_counter,
                                    mmtp_signalling_packet,
                                    mmtp_signalling_packet->packet_sequence_number,
                                    mmtp_signalling_packet->si_fragmentation_indicator,
                                    mmtp_signalling_packet->si_fragmentation_counter);

                        mmtp_packet_id_packets_container_free_mmtp_signalling_packet(mmtp_packet_id_packets_container);

                        goto error;
                    }

                    bool     mmtp_signalling_packet_vector_valid = true;
                    bool     mmtp_signalling_packet_vector_complete = false;
                    uint32_t mmtp_message_payload_final_size = 0;

                    //check our vector sanity, and if we are "complete"
                    mmtp_packet_id_packets_container_add_mmtp_signalling_packet(mmtp_packet_id_packets_container, mmtp_signalling_packet);
                    int mmtp_signalling_packet_vector_count = mmtp_packet_id_packets_container->mmtp_signalling_packet_v.count;

                    mmtp_signalling_packet_t* mmtp_signalling_packet_temp = NULL;
                    mmtp_signalling_packet_t* mmtp_signalling_packet_last_temp = NULL;

                    for(int i=0; i < mmtp_signalling_packet_vector_count && mmtp_signalling_packet_vector_valid; i++) {
                        mmtp_signalling_packet_t* mmtp_signalling_packet_temp = mmtp_packet_id_packets_container->mmtp_signalling_packet_v.data[i];

                        if(!mmtp_signalling_packet_temp || !mmtp_signalling_packet_temp->udp_packet_inner_msg_payload) {
                            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: checking mmtp_signalling_packet vector sanity, mmtp_signalling_packet_temp is NULL, bailing!");
                            mmtp_signalling_packet_vector_valid = false;
                            break;
                        }

                        if(mmtp_signalling_packet_last_temp && mmtp_signalling_packet_last_temp->mmtp_packet_id != mmtp_signalling_packet_temp->mmtp_packet_id) {
                            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: checking mmtp_signalling_packet vector sanity, mmtp_signalling_packet_last_temp->mmtp_packet_id != mmtp_signalling_packet_temp->mmtp_packet_id, %d != %d",
                                         mmtp_signalling_packet_last_temp->mmtp_packet_id, mmtp_signalling_packet_temp->mmtp_packet_id);
                            mmtp_signalling_packet_vector_valid = false;
                            break;
                        }

                        if(i == 0 && mmtp_signalling_packet_temp->si_fragmentation_indicator != 0x1) { //sanity check (01)
                            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: checking mmtp_signalling_packet vector sanity, i == 0 but mmtp_signalling_packet_temp->si_fragmentation_indicator is: 0x%02x", mmtp_signalling_packet_temp->si_fragmentation_indicator);
                            mmtp_signalling_packet_vector_valid = false;
                            break;
                        }

                        if(mmtp_signalling_packet_temp->si_fragmentation_counter != (mmtp_signalling_packet_vector_count - 1 - i)) {
                            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: checking mmtp_signalling_packet vector sanity, mmtp_signalling_packet_temp->si_fragmentation_counter != (mmtp_signalling_packet_vector_count - 1 - i), %d != %d, bailing!",
                                         mmtp_signalling_packet_temp->si_fragmentation_counter,
                                         mmtp_signalling_packet_vector_count - 1 - i);

                            mmtp_signalling_packet_vector_valid = false;
                            break;
                        }

                        //anything less than the "last" packet should be fi==0x2 (10) (fragment that is neither the first nor the last fragment)
                        if(i < (mmtp_signalling_packet_vector_count - 2) && mmtp_signalling_packet_temp->si_fragmentation_indicator != 0x2) {
                            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: checking mmtp_signalling_packet vector sanity, mmtp_signalling_packet_temp->si_fragmentation_indicator: 0x%02x at index: %d, vector_count: %d",
                                         mmtp_signalling_packet_temp->si_fragmentation_indicator, i, mmtp_signalling_packet_vector_count);

                            mmtp_signalling_packet_vector_valid = false;
                            break;
                        }

                        mmtp_message_payload_final_size += mmtp_signalling_packet_temp->udp_packet_inner_msg_payload->p_size;

                        //if we are the last index in the vector AND our fi==0x3 (11) (last fragment of a signalling message), then mark us as complete, otherwise
                        if(i == (mmtp_signalling_packet_vector_count - 1) && mmtp_signalling_packet_temp->si_fragmentation_indicator == 0x3) {
                            mmtp_signalling_packet_vector_complete = true;
                            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy: mmtp_signalling_packet vector is complete, packet_id: %d, vector size: %d",
                                         mmtp_signalling_packet_temp->mmtp_packet_id, mmtp_signalling_packet_vector_count);
                        } else {
                            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy: mmtp_signalling_packet vector not yet complete, i: %d, packet_id: %d, vector size: %d",
                                         i, mmtp_signalling_packet_temp->mmtp_packet_id, mmtp_signalling_packet_vector_count);
                        }

                        mmtp_signalling_packet_last_temp = mmtp_signalling_packet_temp;
                    }

                    if(!mmtp_signalling_packet_vector_valid) {
                        mmtp_packet_id_packets_container_free_mmtp_signalling_packet(mmtp_packet_id_packets_container);
                        mmtp_signalling_packet = NULL; //we will have already freed this packet by clearing the container
                        goto error;

                    } else if(mmtp_signalling_packet_vector_complete) {
                        //re-assemble into MSG payload for parsing
                        block_t* msg_payload_final = block_Alloc(mmtp_message_payload_final_size);
                        mmtp_signalling_packet_t* mmtp_signalling_packet_temp = NULL;

                        for(int i=0; i < mmtp_signalling_packet_vector_count; i++) {
                            mmtp_signalling_packet_temp = mmtp_packet_id_packets_container->mmtp_signalling_packet_v.data[i];

                            block_AppendFromSrciPos(msg_payload_final, mmtp_signalling_packet_temp->udp_packet_inner_msg_payload);
                        }

                        //finally, we can now process our signalling_messagae
                        mmtp_signalling_packet = mmtp_packet_id_packets_container_pop_mmtp_signalling_packet(mmtp_packet_id_packets_container);
                        block_Destroy(&mmtp_signalling_packet->udp_packet_inner_msg_payload);
                        mmtp_signalling_packet->udp_packet_inner_msg_payload = msg_payload_final;
                        block_Rewind(mmtp_signalling_packet->udp_packet_inner_msg_payload);

                        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_DEBUG("atsc3_core_service_bridge_process_packet_phy: mmtp_signalling_packet msg_payload re-assembly is complete, using first mmtp_signalling_packet: %p, udp_packet_inner_msg_payload size: %d, value: %s",
                                     mmtp_signalling_packet,
                                     mmtp_signalling_packet->udp_packet_inner_msg_payload->p_size,
                                     mmtp_signalling_packet->udp_packet_inner_msg_payload->p_buffer);

                        mmtp_si_parsed_message_count = mmt_signalling_message_parse_packet_with_sls_monitor(lls_slt_monitor->lls_sls_mmt_monitor, mmtp_signalling_packet, mmtp_signalling_packet->udp_packet_inner_msg_payload);

                    } else {
                        //noop: continue to accumulate
                        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy: mmtp_signalling_packet - adding to vector, size: %d", mmtp_signalling_packet_vector_count + 1);
                        mmtp_signalling_packet = NULL; //so we don't free pending accumulated packets
                    }
                }
            }

            if(mmtp_si_parsed_message_count > 0) {
                mmt_signalling_message_dump(mmtp_signalling_packet);

                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("process_packet: calling mmt_signalling_message_dispatch_context_notification_callbacks with udp_packet: %p, mmtp_signalling_packet: %p, atsc3_mmt_mfu_context: %p,",
                        udp_packet,
                        mmtp_signalling_packet,
                        atsc3_mmt_mfu_context);


                //dispatch our wired callbacks
                mmt_signalling_message_dispatch_context_notification_callbacks(udp_packet, mmtp_signalling_packet, atsc3_mmt_mfu_context);

                //update our internal sls_mmt_session info
				bool has_updated_atsc3_mmt_sls_mpt_location_info = false;

                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - before mmt_signalling_message_update_lls_sls_mmt_session with matching_lls_sls_mmt_session: %p", matching_lls_sls_mmt_session);

				has_updated_atsc3_mmt_sls_mpt_location_info = mmt_signalling_message_update_lls_sls_mmt_session(mmtp_signalling_packet, matching_lls_sls_mmt_session);

                __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - after mmt_signalling_message_update_lls_sls_mmt_session with matching_lls_sls_mmt_session: %p, has_updated_atsc3_mmt_sls_mpt_location_info: %d", matching_lls_sls_mmt_session, has_updated_atsc3_mmt_sls_mpt_location_info);

                if(has_updated_atsc3_mmt_sls_mpt_location_info) {

                    vector<uint8_t> updated_plp_listeners;

                    //RAII acquire our context mutex
					  {
						  //make it safe for us to have a reference of lls_slt_monitor

                          __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - before atsc3_core_service_player_bridge_get_context_mutex");
                          recursive_mutex& atsc3_core_service_player_bridge_context_mutex = atsc3_core_service_player_bridge_get_context_mutex();
						  lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);
                          __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - after atsc3_core_service_player_bridge_get_context_mutex");

                          lls_slt_monitor_t* lls_slt_monitor = atsc3_core_service_player_bridge_get_lls_slt_montior();
                          __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - after atsc3_core_service_player_bridge_get_lls_slt_montior");

                          updated_plp_listeners = atsc3_phy_build_plp_listeners_from_lls_slt_monitor(lls_slt_monitor);
                          __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - after atsc3_phy_build_plp_listeners_from_lls_slt_monitor, with lls_slt_monitor: %p, updated_plp_listeners.size: %lu", lls_slt_monitor, updated_plp_listeners.size());

                      } //release our context mutex before invoking atsc3_phy_notify_plp_selection_changed with our updated_plp_listeners values

					  if(updated_plp_listeners.size()) {
                          __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - before atsc3_phy_notify_plp_selection_changed");
                          Atsc3NdkApplicationBridge_ptr->atsc3_phy_notify_plp_selection_changed(updated_plp_listeners);
                          __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - after atsc3_phy_notify_plp_selection_changed");
                      }
                    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_core_service_bridge_process_packet_phy - exit has_updated_atsc3_mmt_sls_mpt_location_info logic");

                }
				
                //clear and flush out our mmtp_packet_id_packets_container if we came from re-assembly,
                // otherwise, final free of mmtp_signalling_packet packet in :cleanup
                mmtp_packet_id_packets_container_free_mmtp_signalling_packet(mmtp_packet_id_packets_container);
                goto cleanup;
            }
        } else {
            __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("process_packet: mmtp_packet_header_parse_from_block_t - unknown payload type of 0x%x", mmtp_packet_header->mmtp_payload_type);
            goto error;
        }

        goto cleanup;
    }

	//catchall - not really an error, just un-processed datagram
	goto cleanup;

error:
    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_core_service_bridge_process_packet_phy: error, bailing processing!");

cleanup:

    //jjustman-2020-11-12 - this should be freed already from mmtp_*_free_packet_header_from_block_t, but just in case...
	if(mmtp_packet_header) {
		mmtp_packet_header_free(&mmtp_packet_header);
	}

	//jjustman-2020-11-12 - note: do not free mmtp_mpu_packet or mmtp_signalling_packet as they may have been added to a mmtp_*_packet_collection for re-assembly
	//unless si_fragmentation_indicator == 0x0, then we can safely release, as we do not push single units to the mmtp_packet_id_packets_container->mmtp_signalling_packet_v
	if(mmtp_signalling_packet && mmtp_signalling_packet->si_fragmentation_indicator == 0x0) {
	    mmtp_signalling_packet_free(&mmtp_signalling_packet);
	}

	if(alc_packet) {
	    alc_packet_free(&alc_packet);
	}

	if(udp_packet) {
        udp_packet_free(&udp_packet);
    }
    return;

}

/**
 * NDK to JNI bridiging methods defined here
 * @param lls_table
 */


void atsc3_lls_on_sls_table_present_ndk(atsc3_lls_table_t* lls_table) {

    printf("atsc3_lls_on_sls_table_present_ndk: lls_table is: %p, val: %s", lls_table, lls_table->raw_xml.xml_payload);
    if(!lls_table) {
        Atsc3NdkApplicationBridge_ptr->LogMsg("E: atsc3_lls_on_sls_table_present_ndk: no lls_table for SLS!");
        return;
    }
    if(!lls_table->raw_xml.xml_payload || !lls_table->raw_xml.xml_payload_size) {
        Atsc3NdkApplicationBridge_ptr->LogMsg("E: atsc3_lls_on_sls_table_present_ndk: no raw_xml.xml_payload for SLS!");
        return;
    }

    //copy over our xml_payload.size +1 with a null
    int len_aligned = lls_table->raw_xml.xml_payload_size + 1;
    len_aligned += 8-(len_aligned%8);
    char* xml_payload_copy = (char*)calloc(len_aligned , sizeof(char));
    strncpy(xml_payload_copy, (char*)lls_table->raw_xml.xml_payload, lls_table->raw_xml.xml_payload_size);

    Atsc3NdkApplicationBridge_ptr->atsc3_onSltTablePresent(lls_table->lls_table_id, lls_table->lls_table_version, lls_table->lls_group_id, (const char*)xml_payload_copy);

    free(xml_payload_copy);
}



void atsc3_lls_on_aeat_table_present_ndk(atsc3_lls_table_t* lls_table) {
    printf("atsc3_lls_on_aeat_table_present_ndk: lls_table is: %p, val: %s", lls_table, lls_table->raw_xml.xml_payload);
    if(!lls_table) {
        Atsc3NdkApplicationBridge_ptr->LogMsg("E: atsc3_lls_on_aeat_table_present_ndk: no lls_table for AEAT!");
        return;
    }

    if(!lls_table->raw_xml.xml_payload || !lls_table->raw_xml.xml_payload_size) {
        Atsc3NdkApplicationBridge_ptr->LogMsg("E: atsc3_lls_on_aeat_table_present_ndk: no raw_xml.xml_payload for AEAT!");
        return;
    }

    //copy over our xml_payload.size +1 with a null
    int len_aligned = lls_table->raw_xml.xml_payload_size + 1;
    len_aligned += 8-(len_aligned%8);
    char* xml_payload_copy = (char*)calloc(len_aligned , sizeof(char));
    strncpy(xml_payload_copy, (char*)lls_table->raw_xml.xml_payload, lls_table->raw_xml.xml_payload_size);

    Atsc3NdkApplicationBridge_ptr->atsc3_onAeatTablePresent((const char*)xml_payload_copy);

    free(xml_payload_copy);
}


//TODO: jjustman-2019-11-08: wire up the service_id in which this alc_emission originated from in addition to tsi/toi
void atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_ndk(uint16_t service_id, uint32_t tsi, uint32_t toi, char* s_tsid_content_location, char* s_tsid_content_type, char* cache_file_path) {
    Atsc3NdkApplicationBridge_ptr->atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_jni(service_id, tsi, toi, s_tsid_content_location, s_tsid_content_type, cache_file_path);
}

void atsc3_lls_sls_alc_on_route_mpd_patched_ndk(uint16_t service_id) {
    Atsc3NdkApplicationBridge_ptr->atsc3_lls_sls_alc_on_route_mpd_patched_jni(service_id);
}

void atsc3_lls_sls_alc_on_package_extract_completed_callback_ndk(atsc3_route_package_extracted_envelope_metadata_and_payload_t* atsc3_route_package_extracted_envelope_metadata_and_payload) {
    Atsc3NdkApplicationBridge_ptr->atsc3_lls_sls_alc_on_package_extract_completed_callback_jni(atsc3_route_package_extracted_envelope_metadata_and_payload);
}

bool atsc3_mmt_signalling_information_on_routecomponent_message_present_ndk(atsc3_mmt_mfu_context_t* atsc3_mmt_mfu_context, mmt_atsc3_route_component_t* mmt_atsc3_route_component) {
    //no global refs
    lls_sls_alc_monitor_t* lls_sls_alc_monitor = NULL;

    //jjustman-2020-12-08 - TODO - add this route_component into our SLT monitoring
    //borrowed from atsc3_core_service_player_bridge_set_single_monitor_a331_service_id
    if(atsc3_mmt_mfu_context->mmt_atsc3_route_component_monitored) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_mmt_signalling_information_on_routecomponent_message_present_ndk: atsc3_mmt_mfu_context->mmt_atsc3_route_component_monitored is set, ignoring!");
        return false;
    }

    lls_sls_alc_monitor = atsc3_lls_sls_alc_monitor_create_with_core_service_player_bridge_default_callbacks(atsc3_mmt_mfu_context->matching_lls_sls_mmt_session->atsc3_lls_slt_service);

    lls_slt_service_id_t* lls_slt_service_id = lls_slt_service_id_new_from_atsc3_lls_slt_service(atsc3_mmt_mfu_context->matching_lls_sls_mmt_session->atsc3_lls_slt_service);
    lls_slt_monitor_add_lls_slt_service_id(lls_slt_monitor, lls_slt_service_id);

    //kick start our ROUTE SLS for s-tsid processing
    lls_sls_alc_session_t* lls_sls_alc_session = lls_slt_alc_session_find_or_create_from_ip_udp_values(lls_slt_monitor, atsc3_mmt_mfu_context->matching_lls_sls_mmt_session->atsc3_lls_slt_service, mmt_atsc3_route_component->stsid_destination_ip_address, mmt_atsc3_route_component->stsid_destination_udp_port, mmt_atsc3_route_component->stsid_source_ip_address);
    if(!lls_sls_alc_session) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_mmt_signalling_information_on_routecomponent_message_present_ndk: lls_slt_alc_session_find_from_service_id: lls_sls_alc_session is NULL!");
    }
    lls_sls_alc_monitor->lls_alc_session = lls_sls_alc_session;

    //jjustman-2021-07-07 - TODO: deprecate me
    lls_slt_monitor->lls_sls_alc_monitor = lls_sls_alc_monitor;
	lls_slt_monitor_add_lls_sls_alc_monitor(lls_slt_monitor, lls_sls_alc_monitor);

	vector<uint8_t> updated_plp_listeners = atsc3_phy_build_plp_listeners_from_lls_slt_monitor(lls_slt_monitor);
	__ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_mmt_signalling_information_on_routecomponent_message_present_ndk - after atsc3_phy_build_plp_listeners_from_lls_slt_monitor, with lls_slt_monitor: %p, updated_plp_listeners.size: %lu", lls_slt_monitor, updated_plp_listeners.size());

	if(updated_plp_listeners.size()) {
		__ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_mmt_signalling_information_on_routecomponent_message_present_ndk - before atsc3_phy_notify_plp_selection_changed");
		Atsc3NdkApplicationBridge_ptr->atsc3_phy_notify_plp_selection_changed(updated_plp_listeners);
	    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_TRACE("atsc3_mmt_signalling_information_on_routecomponent_message_present_ndk - after atsc3_phy_notify_plp_selection_changed");
    }
    return true;
}

void atsc3_sls_on_held_trigger_received_callback_impl(uint16_t service_id, block_t* held_payload) {
    block_Rewind(held_payload);
    uint8_t* block_ptr = block_Get(held_payload);
    uint32_t block_len = block_Len(held_payload);

    if(!str_is_utf8((const char*)block_ptr)) {
        __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_WARN("atsc3_sls_on_held_trigger_received_callback_impl: HELD: is not utf-8!: %s", block_ptr);
        return;
    }

    int len_aligned = block_len + 1;
    len_aligned += 8-(len_aligned%8);
    char* xml_payload_copy = (char*)calloc(len_aligned , sizeof(char));
    strncpy(xml_payload_copy, (char*)block_ptr, block_len);
    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_DEBUG("atsc3_sls_on_held_trigger_received_callback_impl: HELD: payload: %s", xml_payload_copy);

    Atsc3NdkApplicationBridge_ptr->atsc3_onSlsHeldEmissionPresent(service_id, (const char*)xml_payload_copy);

    free(xml_payload_copy);
}

void atsc3_mmt_signalling_information_on_held_message_present_ndk(atsc3_mmt_mfu_context_t* atsc3_mmt_mfu_context, mmt_atsc3_held_message_t* mmt_atsc3_held_message) {
    if(atsc3_mmt_mfu_context->matching_lls_sls_mmt_session) {
        atsc3_sls_on_held_trigger_received_callback_impl(atsc3_mmt_mfu_context->matching_lls_sls_mmt_session->service_id, mmt_atsc3_held_message->held_message);
    }
}


/*
 *
note for Android MediaCodec:

https://developer.android.com/reference/android/media/MediaCodec

Android uses the following codec-specific data buffers.
These are also required to be set in the track format for proper MediaMuxer track configuration.
Each parameter set and the codec-specific-data sections marked with (*) must start with a start code of "\x00\x00\x00\x01".

CSD buffer #0:

H.265 HEVC	VPS (Video Parameter Sets*) +
SPS (Sequence Parameter Sets*) +
PPS (Picture Parameter Sets*)

*/
block_t* __INTERNAL_LAST_NAL_PACKET_TODO_FIXME = NULL;

atsc3_link_mapping_table_t* atsc3_phy_jni_bridge_notify_link_mapping_table(atsc3_link_mapping_table_t* atsc3_link_mapping_table_pending) {
    atsc3_link_mapping_table_t* atsc3_link_mapping_table_to_free = NULL;

    //jjustman-2021-01-21 - acquire our context mutex
    lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

    //no last link mapping table, so take ownership of pending ptr
    if(!atsc3_link_mapping_table_last) {
        atsc3_link_mapping_table_last = atsc3_link_mapping_table_pending;
    } else {
        //if we have a pending table version that matches our last table version, so return our pending version to be freed (discarded)
        if(atsc3_link_mapping_table_pending->alp_additional_header_for_signaling_information_signaling_version ==
            atsc3_link_mapping_table_last->alp_additional_header_for_signaling_information_signaling_version) {
            atsc3_link_mapping_table_to_free = atsc3_link_mapping_table_pending;
        } else {
            //pending table version is not the saemm as our last version, so discard our last ptr ref and update pending to last
            atsc3_link_mapping_table_to_free = atsc3_link_mapping_table_last;
            atsc3_link_mapping_table_last = atsc3_link_mapping_table_pending;
        }
    }

    __ATSC3_CORE_SERVICE_PLAYER_BRIDGE_DEBUG("atsc3_phy_jni_bridge_notify_link_mapping_table: callback complete, atsc3_link_mapping_table_last is: %p, invoked with atsc3_link_mapping_table_pending: %p, returning to free: %p",
            atsc3_link_mapping_table_last, atsc3_link_mapping_table_pending, atsc3_link_mapping_table_to_free);

    return atsc3_link_mapping_table_to_free;
}


string atsc3_ndk_cache_temp_folder_path_get(int service_id) {
    return atsc3_ndk_cache_temp_folder_path + to_string(service_id) + "/";
}


string atsc3_route_service_context_temp_folder_name(int service_id) {
    return __ALC_DUMP_OUTPUT_PATH__ + to_string(service_id) + "/";
}

int atsc3_ndk_cache_temp_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    int rv = 0;

    if(strstr(fpath, atsc3_ndk_cache_temp_folder_path.c_str()) == fpath && strstr(fpath, "/route/") ==  (fpath + atsc3_ndk_cache_temp_folder_path.length())) {
        printf("atsc3_ndk_cache_temp_unlink_cb: removing cache path object: %s", fpath);

        rv = remove(fpath);

        if (rv) {
            printf("atsc3_ndk_cache_temp_unlink_cb: unable to remove path: %s, err from remove is: %d", fpath, rv);
        }
    } else {
        printf("atsc3_ndk_cache_temp_unlink_cb: persisting cache path object: %s", fpath);
    }
    return rv;
}

int atsc3_ndk_cache_temp_folder_purge(char *path)
{
    printf("atsc3_ndk_cache_temp_folder_purge: invoked with path: %s", path);

    return nftw(path, atsc3_ndk_cache_temp_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
}


#endif
