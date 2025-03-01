/*
 * atsc3_alp_types.c
 *
 *  Created on: Jul 31, 2019
 *      Author: jjustman
 */

#include "atsc3_alp_types.h"

int _ATSC3_ALP_TYPES_INFO_ENABLED  = 0;
int _ATSC3_ALP_TYPES_DUMP_ENABLED  = 0;
int _ATSC3_ALP_TYPES_DEBUG_ENABLED = 0;
int _ATSC3_ALP_TYPES_TRACE_ENABLED = 0;


ATSC3_VECTOR_BUILDER_METHODS_PARENT_IMPLEMENTATION(atsc3_alp_packet_collection);
ATSC3_VECTOR_BUILDER_METHODS_IMPLEMENTATION(atsc3_alp_packet_collection, atsc3_baseband_packet);
ATSC3_VECTOR_BUILDER_METHODS_IMPLEMENTATION(atsc3_alp_packet_collection, atsc3_alp_packet);

ATSC3_VECTOR_BUILDER_METHODS_PARENT_IMPLEMENTATION(atsc3_link_mapping_table);
ATSC3_VECTOR_BUILDER_METHODS_IMPLEMENTATION(atsc3_link_mapping_table, atsc3_link_mapping_table_plp);
ATSC3_VECTOR_BUILDER_METHODS_IMPLEMENTATION(atsc3_link_mapping_table_plp, atsc3_link_mapping_table_multicast);
ATSC3_VECTOR_BUILDER_METHODS_ITEM_FREE(atsc3_link_mapping_table_plp);
ATSC3_VECTOR_BUILDER_METHODS_ITEM_FREE(atsc3_link_mapping_table_multicast);

//
//atsc3_alp_packet_t* atsc3_alp_packet_new() {
//    atsc3_alp_packet_t* atsc3_alp_packet = calloc(1, sizeof(atsc3_alp_packet_t));
//    
//    return atsc3_alp_packet;
//}

//TODO: jjustman-2019-08-08 - fixme: clone is a shallow clone (memcpy) and MAY leave dangling pointer references

atsc3_alp_packet_t* atsc3_alp_packet_clone(atsc3_alp_packet_t* atsc3_alp_packet) {
    atsc3_alp_packet_t* atsc3_alp_packet_new = calloc(1, sizeof(atsc3_alp_packet_t));
    atsc3_alp_packet_new->bootstrap_timing_data_timestamp_short_reference = atsc3_alp_packet->bootstrap_timing_data_timestamp_short_reference;
    atsc3_alp_packet_new->plp_num = atsc3_alp_packet->plp_num;

    memcpy(&atsc3_alp_packet_new->alp_packet_header, &atsc3_alp_packet->alp_packet_header, sizeof(atsc3_alp_packet_header_t));
    
    //jjustman-2022-08-11 - additional fields for HEF
    if(atsc3_alp_packet->alp_packet_header.alp_packet_header_mode.alp_single_packet_header.header_extension.extension_byte) {
        atsc3_alp_packet_new->alp_packet_header.alp_packet_header_mode.alp_single_packet_header.header_extension.extension_byte = calloc(atsc3_alp_packet->alp_packet_header.alp_packet_header_mode.alp_single_packet_header.header_extension.extension_length_minus1 + 1, sizeof(uint8_t));
        memcpy(atsc3_alp_packet_new->alp_packet_header.alp_packet_header_mode.alp_single_packet_header.header_extension.extension_byte, atsc3_alp_packet->alp_packet_header.alp_packet_header_mode.alp_single_packet_header.header_extension.extension_byte, atsc3_alp_packet->alp_packet_header.alp_packet_header_mode.alp_single_packet_header.header_extension.extension_length_minus1 + 1);
    }
    
    if(atsc3_alp_packet->alp_packet_header.alp_packet_segmentation_concatenation.alp_segmentation_header.header_extension.extension_byte) {
        atsc3_alp_packet_new->alp_packet_header.alp_packet_segmentation_concatenation.alp_segmentation_header.header_extension.extension_byte = calloc(atsc3_alp_packet->alp_packet_header.alp_packet_segmentation_concatenation.alp_segmentation_header.header_extension.extension_length_minus1 + 1, sizeof(uint8_t));
        memcpy(atsc3_alp_packet_new->alp_packet_header.alp_packet_segmentation_concatenation.alp_segmentation_header.header_extension.extension_byte, atsc3_alp_packet->alp_packet_header.alp_packet_segmentation_concatenation.alp_segmentation_header.header_extension.extension_byte, atsc3_alp_packet->alp_packet_header.alp_packet_segmentation_concatenation.alp_segmentation_header.header_extension.extension_length_minus1 + 1);
    }
    
    atsc3_alp_packet_new->alp_payload = block_Duplicate(atsc3_alp_packet->alp_payload);
    atsc3_alp_packet_new->alp_packet_header.alp_header_payload = block_Duplicate(atsc3_alp_packet->alp_packet_header.alp_header_payload);
    atsc3_alp_packet_new->is_alp_payload_complete = atsc3_alp_packet->is_alp_payload_complete;


    return atsc3_alp_packet_new;
}

void atsc3_alp_packet_dump(atsc3_alp_packet_t* atsc3_alp_packet) {
	_ATSC3_ALP_TYPES_DUMP("alp:      PLP: %d, seconds_pre: 0x%06x, a_milli_pre: 0x%04x, size: %d",
				atsc3_alp_packet->plp_num,
				atsc3_alp_packet->bootstrap_timing_data_timestamp_short_reference.seconds_pre,
				atsc3_alp_packet->bootstrap_timing_data_timestamp_short_reference.a_milliseconds_pre,
				block_Remaining_size(atsc3_alp_packet->alp_payload)
		);
}

void atsc3_alp_packet_free(atsc3_alp_packet_t** atsc3_alp_packet_p) {
    if(atsc3_alp_packet_p) {
        atsc3_alp_packet_t* atsc3_alp_packet = *atsc3_alp_packet_p;
        if(atsc3_alp_packet) {
            if(atsc3_alp_packet->alp_packet_header.alp_packet_header_mode.alp_single_packet_header.header_extension.extension_byte) {
                freeclean((void**)&atsc3_alp_packet->alp_packet_header.alp_packet_header_mode.alp_single_packet_header.header_extension.extension_byte);
            }
            
            if(atsc3_alp_packet->alp_packet_header.alp_packet_segmentation_concatenation.alp_segmentation_header.header_extension.extension_byte) {
                freeclean((void**)&atsc3_alp_packet->alp_packet_header.alp_packet_segmentation_concatenation.alp_segmentation_header.header_extension.extension_byte);
            }
            
        	if(atsc3_alp_packet->alp_packet_header.alp_header_payload) {
        		block_Destroy(&atsc3_alp_packet->alp_packet_header.alp_header_payload);
        	}
            
            if(atsc3_alp_packet->alp_payload) {
                block_Destroy(&atsc3_alp_packet->alp_payload);
            }
            
            free(atsc3_alp_packet);
            atsc3_alp_packet = NULL;
        }
        *atsc3_alp_packet_p = NULL;
    }
}

void atsc3_alp_packet_free_alp_payload(atsc3_alp_packet_t* atsc3_alp_packet) {
	if(atsc3_alp_packet) {
		if(atsc3_alp_packet->alp_packet_header.alp_header_payload) {
			block_Destroy(&atsc3_alp_packet->alp_packet_header.alp_header_payload);
		}
		if(atsc3_alp_packet->alp_payload) {
			//block_Release(&atsc3_alp_packet->alp_payload);
            block_Destroy(&atsc3_alp_packet->alp_payload);
		}
	}
}
//jjustman-2022-07-23 - todo - reimpl using atsc3_link_mapping_table_multicast_free
void atsc3_link_mapping_table_free_atsc3_link_mapping_table_plp_internal(atsc3_link_mapping_table_t* atsc3_link_mapping_table) {
	if(atsc3_link_mapping_table) {
		for (int i = 0; i < atsc3_link_mapping_table->atsc3_link_mapping_table_plp_v.count; i++) {
			atsc3_link_mapping_table_plp_t *atsc3_link_mapping_table_plp = atsc3_link_mapping_table->atsc3_link_mapping_table_plp_v.data[i];

			for (int j = 0; j < atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.count; j++) {
				atsc3_link_mapping_table_multicast_t *atsc3_link_mapping_table_multicast = atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.data[j];
				if (atsc3_link_mapping_table_multicast) {
					freesafe(atsc3_link_mapping_table_multicast);
				}
				atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.data[j] = NULL;
			}

			freesafe(atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.data);
			atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.count = 0;
			atsc3_link_mapping_table_plp->atsc3_link_mapping_table_multicast_v.size = 0;

			freesafe(atsc3_link_mapping_table_plp);
		}

		freesafe(atsc3_link_mapping_table->atsc3_link_mapping_table_plp_v.data);
		atsc3_link_mapping_table->atsc3_link_mapping_table_plp_v.count = 0;
		atsc3_link_mapping_table->atsc3_link_mapping_table_plp_v.size = 0;
	}
}

void atsc3_link_mapping_table_free(atsc3_link_mapping_table_t** atsc3_link_mapping_table_p) {
    if(atsc3_link_mapping_table_p) {
        atsc3_link_mapping_table_t* atsc3_link_mapping_table = *atsc3_link_mapping_table_p;
        if(atsc3_link_mapping_table) {
            //chain destructors
			atsc3_link_mapping_table_free_atsc3_link_mapping_table_plp_internal(atsc3_link_mapping_table);
            free(atsc3_link_mapping_table);
            atsc3_link_mapping_table = NULL;
        }
        *atsc3_link_mapping_table_p = NULL;
    }
}
