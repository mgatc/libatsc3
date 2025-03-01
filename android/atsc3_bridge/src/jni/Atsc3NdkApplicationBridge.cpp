#include "Atsc3NdkApplicationBridge.h"

Atsc3NdkApplicationBridge* apiAppBridge;

Atsc3NdkApplicationBridge::Atsc3NdkApplicationBridge(JNIEnv* env, jobject jni_instance) {
    this->env = env;
    this->jni_instance_globalRef = env->NewGlobalRef(jni_instance);

    libatsc3_android_test_populate_system_properties(&this->libatsc3_android_system_properties);

    _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::cctor - libatsc3_android_test_populate_system_properties:\n boot_serialno: %s, serialno: %s\n sdk_ver_str: %s, sdk_ver: %d",
                                 libatsc3_android_system_properties.boot_serialno_str,
                                 libatsc3_android_system_properties.serialno_str,
                                 libatsc3_android_system_properties.sdk_ver_str,
                                 libatsc3_android_system_properties.sdk_ver);
}

//jjustman-2020-08-19: TODO: get (or create) a pinned Atsc3JniEnv from pthread_cur
void Atsc3NdkApplicationBridge::LogMsg(const char *msg)
{
    if (!mJavaVM) {
        _NDK_APPLICATION_BRIDGE_ERROR("LogMsg: mJavaVM is NULL!");
        return;
    }

    Atsc3JniEnv env(mJavaVM);
    if (!env) {
        _NDK_APPLICATION_BRIDGE_ERROR("LogMsg: error creating env pin!");
        return;
    }
    jstring js = env.Get()->NewStringUTF(msg);
    int r = env.Get()->CallIntMethod(jni_instance_globalRef, mOnLogMsgId, js);
    env.Get()->DeleteLocalRef(js);
}

void Atsc3NdkApplicationBridge::LogMsg(const std::string &str)
{
    LogMsg(str.c_str());
}

void Atsc3NdkApplicationBridge::LogMsgF(const char *fmt, ...)
{
    va_list v;
    char msg[1024];
    va_start(v, fmt);
    vsnprintf(msg, sizeof(msg)-1, fmt, v);
    msg[sizeof(msg)-1] = 0;
    va_end(v);

    LogMsg(msg);
}

//alcCompleteObjectMsg
void Atsc3NdkApplicationBridge::atsc3_onAlcObjectStatusMessage(const char *fmt, ...)
{
    va_list v;
    char msg[1024];
    va_start(v, fmt);
    vsnprintf(msg, sizeof(msg)-1, fmt, v);
    msg[sizeof(msg)-1] = 0;
    va_end(v);

    if (!bridgeConsumerJniEnv) {
        _NDK_APPLICATION_BRIDGE_ERROR("atsc3_onAlcObjectStatusMessage: bridgeConsumerJniEnv is NULL!");
        return;
    }

    jstring js = bridgeConsumerJniEnv->Get()->NewStringUTF(msg);

    int r = bridgeConsumerJniEnv->Get()->CallIntMethod(jni_instance_globalRef, atsc3_on_alc_object_status_message_ID, js);
    bridgeConsumerJniEnv->Get()->DeleteLocalRef(js);
}

void Atsc3NdkApplicationBridge::atsc3_onAlcObjectClosed(uint16_t service_id, uint32_t tsi, uint32_t toi, char* s_tsid_content_location, char* s_tsid_content_type, char* cache_file_path) {
    int r = 0;
	if (!bridgeConsumerJniEnv) {
		  _NDK_APPLICATION_BRIDGE_ERROR("atsc3_onAlcObjectClosed: bridgeConsumerJniEnv is NULL!");
		  return;
	}

	jstring s_tsid_content_location_jstring = NULL;
	jstring s_tsid_content_type_jstring = NULL;
	jstring cache_file_path_jstring = NULL;
	
	if(s_tsid_content_location) {
		s_tsid_content_location_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(s_tsid_content_location);
	}
	
	if(s_tsid_content_type) {
		s_tsid_content_type_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(s_tsid_content_type);
	}
	
	if(cache_file_path) {
		cache_file_path_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(cache_file_path);
	}

	r = bridgeConsumerJniEnv->Get()->CallIntMethod(jni_instance_globalRef, atsc3_on_alc_object_closed_ID, service_id, tsi, toi, s_tsid_content_location_jstring, s_tsid_content_type_jstring, cache_file_path_jstring);

	if(s_tsid_content_location) {
		bridgeConsumerJniEnv->Get()->DeleteLocalRef(s_tsid_content_location_jstring);
	}
	
	if(s_tsid_content_type_jstring) {
		bridgeConsumerJniEnv->Get()->DeleteLocalRef(s_tsid_content_type_jstring);
	}
	
	if(cache_file_path_jstring) {
		bridgeConsumerJniEnv->Get()->DeleteLocalRef(cache_file_path_jstring);
	}
}

int Atsc3NdkApplicationBridge::pinConsumerThreadAsNeeded() {
    _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::pinConsumerThreadAsNeeded: mJavaVM: %p, atsc3_ndk_media_mmt_bridge_get_instance: %p", mJavaVM, atsc3_ndk_media_mmt_bridge_get_instance());
    if(bridgeConsumerJniEnv) {
        _NDK_APPLICATION_BRIDGE_WARN("Atsc3NdkApplicationBridge::pinConsumerThreadAsNeeded: mJavaVM: %p, atsc3_ndk_media_mmt_bridge_get_instance: %p, bridgeConsumerJniEnv is NOT NULL: %p - This will cause JNI pinned thread issues!", mJavaVM, atsc3_ndk_media_mmt_bridge_get_instance(), bridgeConsumerJniEnv);
    }

    bridgeConsumerJniEnv = new Atsc3JniEnv(mJavaVM);

    //hack
    IAtsc3NdkMediaMMTBridge* iAtsc3NdkMediaMMTBridge = atsc3_ndk_media_mmt_bridge_get_instance();
    if(iAtsc3NdkMediaMMTBridge) {
        iAtsc3NdkMediaMMTBridge->pinConsumerThreadAsNeeded(); //referenceConsumerJniEnvAsNeeded(bridgeConsumerJniEnv);
    }

    return 0;
}

int Atsc3NdkApplicationBridge::releasePinnedConsumerThreadAsNeeded() {
    _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::releasePinnedConsumerThreadAsNeeded: bridgeConsumerJniEnv is: %p:", bridgeConsumerJniEnv);
    if(bridgeConsumerJniEnv) {
        delete bridgeConsumerJniEnv;
    }

    //hack
    IAtsc3NdkMediaMMTBridge* iAtsc3NdkMediaMMTBridge = atsc3_ndk_media_mmt_bridge_get_instance();
    if(iAtsc3NdkMediaMMTBridge) {
        iAtsc3NdkMediaMMTBridge->releasePinnedConsumerThreadAsNeeded();
    }

    return 0;
}

int Atsc3NdkApplicationBridge::atsc3_slt_select_service(int service_id) {
    int ret = -1;
    vector<uint8_t> updated_plp_listeners;

    atsc3_lls_slt_service_t* atsc3_lls_slt_service = atsc3_core_service_player_bridge_set_single_monitor_a331_service_id(service_id);
    if(atsc3_lls_slt_service && atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.count) {
        ret = atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.data[0]->sls_protocol;

        //refresh our listened PLPs based upon the updated SLS ip:port flow now added to lls_slt_monitor->lls_slt_service_id_v collection from the LMT
        if(ret) {

            //RAII acquire our context mutex
            {
                _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::atsc3_slt_select_service - before atsc3_core_service_player_bridge_get_context_mutex with service_id: %d", service_id);

                //make it safe for us to have a reference of lls_slt_monitor
                recursive_mutex& atsc3_core_service_player_bridge_context_mutex = atsc3_core_service_player_bridge_get_context_mutex();
                lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

                _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::atsc3_slt_select_service - before atsc3_core_service_player_bridge_get_lls_slt_montior with service_id: %d", service_id);

                lls_slt_monitor_t* lls_slt_monitor = atsc3_core_service_player_bridge_get_lls_slt_montior();
                _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::atsc3_slt_select_service - before atsc3_phy_build_plp_listeners_from_lls_slt_monitor with service_id: %d", service_id);

                updated_plp_listeners = atsc3_phy_build_plp_listeners_from_lls_slt_monitor(lls_slt_monitor);
                _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::atsc3_slt_select_service - after atsc3_phy_build_plp_listeners_from_lls_slt_monitor with service_id: %d", service_id);

            } //release our context mutex before invoking atsc3_phy_notify_plp_selection_changed with our updated_plp_listeners values

            if(updated_plp_listeners.size()) {
                _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::atsc3_slt_select_service - before from atsc3_phy_notify_plp_selection_changed with service_id: %d", service_id);
                atsc3_phy_notify_plp_selection_changed(updated_plp_listeners);
                _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::atsc3_slt_select_service - return from atsc3_phy_notify_plp_selection_changed with service_id: %d", service_id);
            }

        }
    }

    _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::atsc3_slt_select_service - return service_id: %d", service_id);

    return ret;
}

//add an additional service_id for monitoring, e.g. for ESG use cases to capture the OMA-BCAST payload while presenting the linear a/v route emission
//
// sucessful:
//      returns the a/331 sls_protocol if service selection was successful,
//
//  otherwise:
//      returns -1, for service_id not found (or other failure)

int Atsc3NdkApplicationBridge::atsc3_slt_alc_select_additional_service(int service_id) {
    int ret = -1;
    vector<uint8_t> updated_plp_listeners;

    atsc3_lls_slt_service_t* atsc3_lls_slt_service = atsc3_core_service_player_bridge_add_monitor_a331_service_id(service_id);
    if(atsc3_lls_slt_service && atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.count) {
        ret = atsc3_lls_slt_service->atsc3_slt_broadcast_svc_signalling_v.data[0]->sls_protocol;

        //refresh our listened PLPs based upon the updated SLS ip:port flow now added to lls_slt_monitor->lls_slt_service_id_v collection from the LMT
        if(ret) {

            //RAII acquire our context mutex
            {
                //make it safe for us to have a reference of lls_slt_monitor
                recursive_mutex& atsc3_core_service_player_bridge_context_mutex = atsc3_core_service_player_bridge_get_context_mutex();
                lock_guard<recursive_mutex> atsc3_core_service_player_bridge_context_mutex_local(atsc3_core_service_player_bridge_context_mutex);

                lls_slt_monitor_t* lls_slt_monitor = atsc3_core_service_player_bridge_get_lls_slt_montior();
                updated_plp_listeners = atsc3_phy_build_plp_listeners_from_lls_slt_monitor(lls_slt_monitor);
            } //release our context mutex before invoking atsc3_phy_notify_plp_selection_changed with our updated_plp_listeners values

            if(updated_plp_listeners.size()) {
                atsc3_phy_notify_plp_selection_changed(updated_plp_listeners);
            }
        }
    }

    return ret;
}

//TODO: jjustman-2019-11-07 - add mutex here around additional_services_monitored collection
int Atsc3NdkApplicationBridge::atsc3_slt_alc_clear_additional_service_selections() {

//    for(int i=0; i < atsc3_slt_alc_additional_services_monitored.size(); i++) {
//        int to_remove_monitor_service_id = atsc3_slt_alc_additional_services_monitored.at(i);
//        atsc3_phy_mmt_player_bridge_remove_monitor_a331_service_id(to_remove_monitor_service_id);
//    }
//
//    atsc3_slt_alc_additional_services_monitored.clear();

    return 0;
}



std::string Atsc3NdkApplicationBridge::get_android_temp_folder() {
    if(!apiAppBridge) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge::get_android_temp_folder - apiAppBridge is NULL!");
        return "";
    }
    Atsc3JniEnv env(mJavaVM);

    jmethodID getCacheDir = env.Get()->GetMethodID( jni_class_globalRef, "getCacheDir", "()Ljava/io/File;" );
    jobject cache_dir = env.Get()->CallObjectMethod(jni_instance_globalRef, getCacheDir );

    jclass fileClass = env.Get()->FindClass( "java/io/File" );
    jmethodID getPath = env.Get()->GetMethodID( fileClass, "getPath", "()Ljava/lang/String;" );
    jstring path_string = (jstring)env.Get()->CallObjectMethod( cache_dir, getPath );

    const char *path_chars = env.Get()->GetStringUTFChars( path_string, NULL );
    std::string temp_folder( path_chars );

    env.Get()->ReleaseStringUTFChars( path_string, path_chars );
    //app->activity->vm->DetachCurrentThread();
    return temp_folder;
}


//use 2nd param to_match_content_type to filter down to MPD via const char* to_match_content_type
//application/dash+xml

/*
 * jjustman-2019-11-08 - note mbms_envelope might not have the proper content_type set, so check
 */
vector<string>
Atsc3NdkApplicationBridge::atsc3_slt_alc_get_sls_metadata_fragments_content_locations_from_monitor_service_id(int service_id, const char* to_match_content_type) {

    vector<string> my_mbms_metadata_uri_values;
    atsc3_sls_metadata_fragments_t* atsc3_sls_metadata_fragments = atsc3_slt_alc_get_sls_metadata_fragments_from_monitor_service_id(service_id);

    if(atsc3_sls_metadata_fragments && atsc3_sls_metadata_fragments->atsc3_mbms_metadata_envelope && atsc3_sls_metadata_fragments->atsc3_mbms_metadata_envelope->atsc3_mbms_metadata_item_v.count) {
        for(int i=0; i < atsc3_sls_metadata_fragments->atsc3_mbms_metadata_envelope->atsc3_mbms_metadata_item_v.count; i++) {
            atsc3_mbms_metadata_item_t* atsc3_mbms_metadata_item = atsc3_sls_metadata_fragments->atsc3_mbms_metadata_envelope->atsc3_mbms_metadata_item_v.data[i];
            if(atsc3_mbms_metadata_item->metadata_uri) {
                //if to_match_content_type is supplied, filter by match
                if(to_match_content_type && atsc3_mbms_metadata_item->content_type &&
                   strncasecmp(to_match_content_type, atsc3_mbms_metadata_item->content_type, strlen(to_match_content_type)) == 0 ) {
                    string my_metadata_route_service_temp_folder_name = atsc3_route_service_context_temp_folder_name(service_id) + atsc3_mbms_metadata_item->metadata_uri;
                    my_mbms_metadata_uri_values.push_back(my_metadata_route_service_temp_folder_name);
                }
            }
        }

    }

    //also walk thru

    if(atsc3_sls_metadata_fragments && atsc3_sls_metadata_fragments->atsc3_mime_multipart_related_instance && atsc3_sls_metadata_fragments->atsc3_mime_multipart_related_instance->atsc3_mime_multipart_related_payload_v.count) {
        for(int i=0; i < atsc3_sls_metadata_fragments->atsc3_mime_multipart_related_instance->atsc3_mime_multipart_related_payload_v.count; i++) {
            atsc3_mime_multipart_related_payload_t* atsc3_mime_multipart_related_payload = atsc3_sls_metadata_fragments->atsc3_mime_multipart_related_instance->atsc3_mime_multipart_related_payload_v.data[i];
            if(atsc3_mime_multipart_related_payload->sanitizied_content_location) {
                //if to_match_content_type is supplied, filter by match
                if(to_match_content_type && atsc3_mime_multipart_related_payload->content_type &&
                   strncasecmp(to_match_content_type, atsc3_mime_multipart_related_payload->content_type, strlen(to_match_content_type)) == 0 ) {
                    string my_metadata_route_service_temp_folder_name = atsc3_route_service_context_temp_folder_name(service_id) + atsc3_mime_multipart_related_payload->sanitizied_content_location;
                    my_mbms_metadata_uri_values.push_back(my_metadata_route_service_temp_folder_name);
                }
            }
        }

    }

    return my_mbms_metadata_uri_values;
}


vector<string> Atsc3NdkApplicationBridge::atsc3_slt_alc_get_sls_route_s_tsid_fdt_file_content_locations_from_monitor_service_id(int service_id) {
    vector<string> my_fdt_file_content_location_values;
    atsc3_route_s_tsid_t* atsc3_route_s_tsid = atsc3_slt_alc_get_sls_route_s_tsid_from_monitor_service_id(service_id);

    if(atsc3_route_s_tsid) {
        for(int i=0; i < atsc3_route_s_tsid->atsc3_route_s_tsid_RS_v.count; i++) {
            atsc3_route_s_tsid_RS_t* atsc3_route_s_tsid_RS = atsc3_route_s_tsid->atsc3_route_s_tsid_RS_v.data[i];

            for(int j=0; j < atsc3_route_s_tsid_RS->atsc3_route_s_tsid_RS_LS_v.count; j++) {
                atsc3_route_s_tsid_RS_LS_t* atsc3_route_s_tsid_RS_LS = atsc3_route_s_tsid_RS->atsc3_route_s_tsid_RS_LS_v.data[j];

                if(atsc3_route_s_tsid_RS_LS->atsc3_route_s_tsid_RS_LS_SrcFlow) {
                    atsc3_fdt_instance_t* atsc3_fdt_instance = atsc3_route_s_tsid_RS_LS->atsc3_route_s_tsid_RS_LS_SrcFlow->atsc3_fdt_instance;

                    if(atsc3_fdt_instance) {
                        for(int k=0; k < atsc3_fdt_instance->atsc3_fdt_file_v.count; k++) {
                            atsc3_fdt_file_t* atsc3_fdt_file = atsc3_fdt_instance->atsc3_fdt_file_v.data[k];
                            if(atsc3_fdt_file && atsc3_fdt_file->content_location ) {

                                string my_fdt_file_content_location_local_context_path = atsc3_route_service_context_temp_folder_name(service_id) + atsc3_fdt_file->content_location;
                                my_fdt_file_content_location_values.push_back(my_fdt_file_content_location_local_context_path);
                                //TODO: jjustman-2019-11-07 - re-factor to use atsc3_fdt_file_t struct
                            }
                        }
                    }
                }
            }
        }
    }

    return my_fdt_file_content_location_values;
}

void Atsc3NdkApplicationBridge::atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_jni(uint16_t service_id, uint32_t tsi, uint32_t toi, char* s_tsid_content_location, char* s_tsid_content_type, char* cache_file_path) {
    //jjustman-2020-01-07: add in alc flow debugging
    _NDK_APPLICATION_BRIDGE_DEBUG("atsc3_lls_sls_alc_on_object_close_flag_s_tsid_content_location_jni: service_id: %d, tsi: %d, toi: %d, s_tsid_content_location: %s, s_tsid_content_type: %s, cache_file_path: %s",
                                 service_id, tsi, toi, s_tsid_content_location, s_tsid_content_type, cache_file_path);

	atsc3_onAlcObjectClosed(service_id, tsi, toi, s_tsid_content_location, s_tsid_content_type, cache_file_path);
}

void Atsc3NdkApplicationBridge::atsc3_lls_sls_alc_on_route_mpd_patched_jni(uint16_t service_id) {
    if (!bridgeConsumerJniEnv) {
        _NDK_APPLICATION_BRIDGE_ERROR("ats3_onMfuPacket: bridgeConsumerJniEnv is NULL!");
        return;
    }

    int r = bridgeConsumerJniEnv->Get()->CallIntMethod(jni_instance_globalRef, atsc3_lls_sls_alc_on_route_mpd_patched_ID, service_id);
}

// https://stackoverflow.com/questions/6343459/get-strings-used-in-java-from-jni
void Atsc3NdkApplicationBridge::atsc3_lls_sls_alc_on_package_extract_completed_callback_jni(atsc3_route_package_extracted_envelope_metadata_and_payload_t* atsc3_route_package_extracted_envelope_metadata_and_payload) {
    if (!atsc3_lls_sls_alc_on_package_extract_completed_ID)
        return;

    if (!bridgeConsumerJniEnv) {
        _NDK_APPLICATION_BRIDGE_ERROR("atsc3_lls_sls_alc_on_package_extract_completed_callback_jni::err on get jni env: bridgeConsumerJniEnv");
        return;
    }

    if(!atsc3_route_package_extracted_envelope_metadata_and_payload) {
        _NDK_APPLICATION_BRIDGE_ERROR("atsc3_lls_sls_alc_on_package_extract_completed_callback_jni::err atsc3_route_package_extracted_envelope_metadata_and_payload is NULL");
        return;
    }

    if(!atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mbms_metadata_envelope_raw_xml || !atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mbms_metadata_envelope_raw_xml->p_buffer) {
        _NDK_APPLICATION_BRIDGE_ERROR("atsc3_lls_sls_alc_on_package_extract_completed_callback_jni::err atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mbms_metadata_envelope_raw_xml (or p_buffer) is NULL");
        return;
    }

    //jjustman-2021-05-04 -     if(!str_is_utf8((const char*)block_ptr)) { sanity check for ndk/jni UTF-8 marshalling across boundary / unhandled exceptions

    if(!str_is_utf8((const char*)atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mbms_metadata_envelope_raw_xml->p_buffer)) {
        _NDK_APPLICATION_BRIDGE_ERROR("atsc3_lls_sls_alc_on_package_extract_completed_callback_jni::err atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mbms_metadata_envelope_raw_xml->p_buffer fails str_is_utf8 check for NDK/JNI UTF-8 marshalling!");
        printf("atsc3_lls_sls_alc_on_package_extract_completed_callback_jni::atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mbms_metadata_envelope_raw_xml->p_buffer is:\n%s", atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mbms_metadata_envelope_raw_xml->p_buffer);
        return;
    }


    if(!atsc3_route_package_extracted_envelope_metadata_and_payload->package_name) {
    	_NDK_APPLICATION_BRIDGE_ERROR("atsc3_lls_sls_alc_on_package_extract_completed_callback_jni::err atsc3_route_package_extracted_envelope_metadata_and_payload->package_name is NULL");
        return;
    }

    std::list<jstring> to_clean_jstrings;
    std::list<jobject> to_clean_jobject;

    //org.ngbp.libatsc3.middleware.android.a331.PackageExtractEnvelopeMetadataAndPayload
    jclass jcls = apiAppBridge->packageExtractEnvelopeMetadataAndPayload_jclass_global_ref;
    jobject jobj = bridgeConsumerJniEnv->Get()->AllocObject(jcls);

    if(!jobj) {
        _NDK_APPLICATION_BRIDGE_ERROR("atsc3_lls_sls_alc_on_package_extract_completed_callback_jni::err unable to allocate packageExtractEnvelopeMetadataAndPayload_jclass_global_ref instance jobj!");
        return;
    }

    jfieldID packageName_valId = bridgeConsumerJniEnv->Get()->GetFieldID(jcls, "packageName", "Ljava/lang/String;");
    jstring packageName_payload = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_route_package_extracted_envelope_metadata_and_payload->package_name);
    bridgeConsumerJniEnv->Get()->SetObjectField(jobj, packageName_valId, packageName_payload);
    to_clean_jstrings.push_back(packageName_payload);

    jfieldID tsi_valId = bridgeConsumerJniEnv->Get()->GetFieldID(jcls, "tsi", "I");
    bridgeConsumerJniEnv->Get()->SetIntField(jobj, tsi_valId, atsc3_route_package_extracted_envelope_metadata_and_payload->tsi);

    jfieldID toi_valId = bridgeConsumerJniEnv->Get()->GetFieldID(jcls, "toi", "I");
    bridgeConsumerJniEnv->Get()->SetIntField(jobj, toi_valId, atsc3_route_package_extracted_envelope_metadata_and_payload->toi);

    jfieldID appContextIdList_valId = bridgeConsumerJniEnv->Get()->GetFieldID(jcls, "appContextIdList", "Ljava/lang/String;");
    jstring appContextIdList_payload = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_route_package_extracted_envelope_metadata_and_payload->app_context_id_list);
    bridgeConsumerJniEnv->Get()->SetObjectField(jobj, appContextIdList_valId, appContextIdList_payload);
    to_clean_jstrings.push_back(appContextIdList_payload);


    jfieldID packageExtractPath_valId = bridgeConsumerJniEnv->Get()->GetFieldID(jcls, "packageExtractPath", "Ljava/lang/String;");
    jstring packageExtractPath_payload = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_route_package_extracted_envelope_metadata_and_payload->package_extract_path);
    bridgeConsumerJniEnv->Get()->SetObjectField(jobj, packageExtractPath_valId, packageExtractPath_payload);
    to_clean_jstrings.push_back(packageExtractPath_payload);


    jfieldID mbmsEnvelopeRawXml_valId = bridgeConsumerJniEnv->Get()->GetFieldID(jcls, "mbmsEnvelopeRawXml", "Ljava/lang/String;");
    jstring mbmsEnvelopeRawXml_payload = bridgeConsumerJniEnv->Get()->NewStringUTF((char*)atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mbms_metadata_envelope_raw_xml->p_buffer);
    bridgeConsumerJniEnv->Get()->SetObjectField(jobj, mbmsEnvelopeRawXml_valId, mbmsEnvelopeRawXml_payload);
    to_clean_jstrings.push_back(mbmsEnvelopeRawXml_payload);

    if(atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mime_multipart_related_payload_v.count > 0) {

        jobject multipartRelatedPayloadList_jobject = bridgeConsumerJniEnv->Get()->NewObject(apiAppBridge->jni_java_util_ArrayList, apiAppBridge->jni_java_util_ArrayList_cctor, atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mime_multipart_related_payload_v.count);

        for(int i=0; i < atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mime_multipart_related_payload_v.count; i++) {
            atsc3_mime_multipart_related_payload_t* atsc3_mime_multipart_related_payload = atsc3_route_package_extracted_envelope_metadata_and_payload->atsc3_mime_multipart_related_payload_v.data[i];
            jobject jobj_multipart_related_payload_jobject = bridgeConsumerJniEnv->Get()->AllocObject(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref);

            to_clean_jobject.push_back(jobj_multipart_related_payload_jobject);

            jfieldID contentLocation_valId = bridgeConsumerJniEnv->Get()->GetFieldID(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref, "contentLocation", "Ljava/lang/String;");
            jstring contentLocation_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_mime_multipart_related_payload->sanitizied_content_location);
            bridgeConsumerJniEnv->Get()->SetObjectField(jobj_multipart_related_payload_jobject, contentLocation_valId, contentLocation_jstring);
            to_clean_jstrings.push_back(contentLocation_jstring);

            if(atsc3_mime_multipart_related_payload->content_type) {
                jfieldID contentType_valId = bridgeConsumerJniEnv->Get()->GetFieldID(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref, "contentType", "Ljava/lang/String;");
                jstring contentType_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_mime_multipart_related_payload->content_type);
                bridgeConsumerJniEnv->Get()->SetObjectField(jobj_multipart_related_payload_jobject, contentType_valId, contentType_jstring);
                to_clean_jstrings.push_back(contentType_jstring);
            }

            if(atsc3_mime_multipart_related_payload->valid_from_string) {
                jfieldID validFrom_valId = bridgeConsumerJniEnv->Get()->GetFieldID(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref, "validFrom", "Ljava/lang/String;");
                jstring validFrom_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_mime_multipart_related_payload->valid_from_string);
                bridgeConsumerJniEnv->Get()->SetObjectField(jobj_multipart_related_payload_jobject, validFrom_valId, validFrom_jstring);
                to_clean_jstrings.push_back(validFrom_jstring);
            }

            if(atsc3_mime_multipart_related_payload->valid_until_string) {
                jfieldID validUntil_valId = bridgeConsumerJniEnv->Get()->GetFieldID(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref, "validUntil", "Ljava/lang/String;");
                jstring validUntil_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_mime_multipart_related_payload->valid_until_string);
                bridgeConsumerJniEnv->Get()->SetObjectField(jobj_multipart_related_payload_jobject, validUntil_valId, validUntil_jstring);
                to_clean_jstrings.push_back(validUntil_jstring);
            }

            //version

            jfieldID version_valId = bridgeConsumerJniEnv->Get()->GetFieldID(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref, "version", "I");
            bridgeConsumerJniEnv->Get()->SetIntField(jobj_multipart_related_payload_jobject, version_valId, atsc3_mime_multipart_related_payload->version);


            if(atsc3_mime_multipart_related_payload->next_url_string) {
                jfieldID nextUrl_valId = bridgeConsumerJniEnv->Get()->GetFieldID(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref, "nextUrl", "Ljava/lang/String;");
                jstring nextUrl_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_mime_multipart_related_payload->next_url_string);
                bridgeConsumerJniEnv->Get()->SetObjectField(jobj_multipart_related_payload_jobject, nextUrl_valId, nextUrl_jstring);
                to_clean_jstrings.push_back(nextUrl_jstring);
            }

            if(atsc3_mime_multipart_related_payload->avail_at_string) {
                jfieldID availAt_valId = bridgeConsumerJniEnv->Get()->GetFieldID(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref, "availAt", "Ljava/lang/String;");
                jstring availAt_jstring = bridgeConsumerJniEnv->Get()->NewStringUTF(atsc3_mime_multipart_related_payload->avail_at_string);
                bridgeConsumerJniEnv->Get()->SetObjectField(jobj_multipart_related_payload_jobject, availAt_valId, availAt_jstring);
                to_clean_jstrings.push_back(availAt_jstring);
            }

            //extractedSize
            jfieldID extractedSize_valId = bridgeConsumerJniEnv->Get()->GetFieldID(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref, "extractedSize", "I");
            bridgeConsumerJniEnv->Get()->SetIntField(jobj_multipart_related_payload_jobject, extractedSize_valId, atsc3_mime_multipart_related_payload->extracted_size);

            bridgeConsumerJniEnv->Get()->CallBooleanMethod(multipartRelatedPayloadList_jobject, apiAppBridge->jni_java_util_ArrayList_add, jobj_multipart_related_payload_jobject);
        }

        jfieldID multipartRelatedPayloadList_valId = bridgeConsumerJniEnv->Get()->GetFieldID(jcls, "multipartRelatedPayloadList", "Ljava/util/List;");
        bridgeConsumerJniEnv->Get()->SetObjectField(jobj, multipartRelatedPayloadList_valId, multipartRelatedPayloadList_jobject);
    }

    int r = bridgeConsumerJniEnv->Get()->CallIntMethod(jni_instance_globalRef, atsc3_lls_sls_alc_on_package_extract_completed_ID, jobj);

    for (std::list<jstring>::iterator it=to_clean_jstrings.begin(); it != to_clean_jstrings.end(); ++it) {
        bridgeConsumerJniEnv->Get()->DeleteLocalRef(*it);
    }
    to_clean_jstrings.clear();

    for (std::list<jobject>::iterator it=to_clean_jobject.begin(); it != to_clean_jobject.end(); ++it) {
        bridgeConsumerJniEnv->Get()->DeleteLocalRef(*it);
    }
    to_clean_jobject.clear();

    bridgeConsumerJniEnv->Get()->DeleteLocalRef(jobj);
}


void Atsc3NdkApplicationBridge::atsc3_onSltTablePresent(uint8_t lls_table_id, uint8_t lls_table_version, uint8_t lls_group_id, const char* slt_payload_xml) {
    if (!atsc3_onSltTablePresent_ID) {
        _NDK_APPLICATION_BRIDGE_ERROR("atsc3_onSltTablePresent_ID: %p", atsc3_onSltTablePresent_ID);
        return;
    }

    if (!bridgeConsumerJniEnv) {
		_NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge::atsc3_onSltTablePresent: bridgeConsumerJniEnv is NULL");
        return;
    }

    if (!slt_payload_xml || !strlen(slt_payload_xml)) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge::atsc3_onSltTablePresent: slt_payload_xml is NULL!");
        return;
    }

    _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge::atsc3_onSltTablePresent: slt_payload_xml is: %s", slt_payload_xml);


    jstring xml_payload = bridgeConsumerJniEnv->Get()->NewStringUTF(slt_payload_xml);
    int r = bridgeConsumerJniEnv->Get()->CallIntMethod(jni_instance_globalRef, atsc3_onSltTablePresent_ID, lls_table_id, lls_table_version, lls_group_id, xml_payload);
    bridgeConsumerJniEnv->Get()->DeleteLocalRef(xml_payload);
}



void Atsc3NdkApplicationBridge::atsc3_onAeatTablePresent(const char *aeat_payload_xml) {
    if (!atsc3_onAeatTablePresent_ID) {
        _NDK_APPLICATION_BRIDGE_ERROR("atsc3_onAeatTablePresent: %p", atsc3_onAeatTablePresent_ID);

        return;
    }

    if (!bridgeConsumerJniEnv) {
		_NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge::atsc3_onAeatTablePresent: bridgeConsumerJniEnv is NULL");
        return;
    }

    jstring xml_payload = bridgeConsumerJniEnv->Get()->NewStringUTF(aeat_payload_xml);
    int r = bridgeConsumerJniEnv->Get()->CallIntMethod(jni_instance_globalRef, atsc3_onAeatTablePresent_ID, xml_payload);
    bridgeConsumerJniEnv->Get()->DeleteLocalRef(xml_payload);
}

void Atsc3NdkApplicationBridge::atsc3_onSlsHeldEmissionPresent(uint16_t service_id, const char *held_payload_xml) {
	if (!atsc3_onSlsHeldEmissionPresent_ID)
		return;

	if (!bridgeConsumerJniEnv) {
		_NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge::atsc3_onSlsHeldEmissionPresent: bridgeConsumerJniEnv is NULL");
		return;
	}

    jstring xml_payload = bridgeConsumerJniEnv->Get()->NewStringUTF(held_payload_xml);
    int r = bridgeConsumerJniEnv->Get()->CallIntMethod(jni_instance_globalRef, atsc3_onSlsHeldEmissionPresent_ID, service_id, xml_payload);
    bridgeConsumerJniEnv->Get()->DeleteLocalRef(xml_payload);
}


void Atsc3NdkApplicationBridge::atsc3_phy_notify_plp_selection_change_set_callback(atsc3_phy_notify_plp_selection_change_f atsc3_phy_notify_plp_selection_change, void* context) {
    this->atsc3_phy_notify_plp_selection_change = atsc3_phy_notify_plp_selection_change;
    this->atsc3_phy_notify_plp_selection_change_context = context;
}

void Atsc3NdkApplicationBridge::atsc3_phy_notify_plp_selection_change_clear_callback() {
    this->atsc3_phy_notify_plp_selection_change = nullptr;
}

void Atsc3NdkApplicationBridge::atsc3_phy_notify_plp_selection_changed(vector<uint8_t> plps_to_listen) {
    if(this->atsc3_phy_notify_plp_selection_change && this->atsc3_phy_notify_plp_selection_change_context) {
        this->atsc3_phy_notify_plp_selection_change(plps_to_listen, this->atsc3_phy_notify_plp_selection_change_context);
    }
}


bool Atsc3NdkApplicationBridge::atsc3_get_demod_pcap_capture() {
    return is_enabled_demod_pcap_capture;
}

#define ATSC3_NDK_APPLICATION_BRIDGE_PCAP_FILENAME_MAX_LEN 128
void Atsc3NdkApplicationBridge::atsc3_set_demod_pcap_capture(bool new_state) {
    if(new_state) {
        if(!is_enabled_demod_pcap_capture) {
            //create a new pcap file handle here, and assign our header payload

            char pcap_writer_filename_string[ATSC3_NDK_APPLICATION_BRIDGE_PCAP_FILENAME_MAX_LEN + 1] = {0};
            double launch_timestamp = gt();

			//jjustman-2022-07-12 - todo: add in frequency and PLP's here...
            snprintf((char*)&pcap_writer_filename_string, ATSC3_NDK_APPLICATION_BRIDGE_PCAP_FILENAME_MAX_LEN, "%s.%.4f.demuxed.pcap", "atsc3_demod_pcap_dump", launch_timestamp);

            atsc3_pcap_writer_context = atsc3_pcap_writer_open_filename(pcap_writer_filename_string);

            _NDK_APPLICATION_BRIDGE_INFO("atsc3_set_demod_pcap_capture: new_state is true, creating new pcap file: %s, fp: %p", atsc3_pcap_writer_context->pcap_file_name, atsc3_pcap_writer_context->pcap_fp);

			vector<uint8_t> plps_0_to_3_to_listen{0, 1, 2, 3};
			atsc3_phy_notify_plp_selection_changed(plps_0_to_3_to_listen);
			_NDK_APPLICATION_BRIDGE_INFO("atsc3_set_demod_pcap_capture: setting plps to listen to: %d, %d, %d, %d",
										 plps_0_to_3_to_listen[0],
										 plps_0_to_3_to_listen[1],
										 plps_0_to_3_to_listen[2],
										 plps_0_to_3_to_listen[3]);


            is_enabled_demod_pcap_capture = true;
        } else {
            //todo - we shouldn't do anything if we are already open...
            _NDK_APPLICATION_BRIDGE_WARN("atsc3_set_demod_pcap_capture: new_state is true, but we are already enabled, file: %s, fp: %p", atsc3_pcap_writer_context->pcap_file_name, atsc3_pcap_writer_context->pcap_fp);

        }
    } else {
        _NDK_APPLICATION_BRIDGE_INFO("atsc3_set_demod_pcap_capture: new_state is false, closing out demod_pcap_file_name: %s", atsc3_pcap_writer_context->pcap_file_name);

        //we should close out our file handle
        if(atsc3_pcap_writer_context) {
            atsc3_pcap_writer_context_close(atsc3_pcap_writer_context);

            atsc3_pcap_writer_context_free(&atsc3_pcap_writer_context);
        }

        is_enabled_demod_pcap_capture = false;
    }

};

atsc3_pcap_writer_context_t* Atsc3NdkApplicationBridge::atsc3_pcap_writer_context_get() {
    return atsc3_pcap_writer_context;
}


//--------------------------------------------------------------------------

extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_init(JNIEnv *env, jobject instance)
{
    apiAppBridge = new Atsc3NdkApplicationBridge(env, instance);
    _NDK_APPLICATION_BRIDGE_INFO("Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_init:  env: %p", env);
    apiAppBridge->setJniClassReference("org/ngbp/libatsc3/middleware/Atsc3NdkApplicationBridge");
    apiAppBridge->mJavaVM = atsc3_bridge_ndk_static_loader_get_javaVM();

    if(apiAppBridge->mJavaVM == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: apiAppBridge->mJavaVM is NULL!");
        return -1;
    }

    jclass jniClassReference = apiAppBridge->getJniClassReference();

    if (jniClassReference == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find Atsc3NdkApplicationBridge java class reference!");
        return -2;
    }

    apiAppBridge->mOnLogMsgId = env->GetMethodID(jniClassReference, "onLogMsg", "(Ljava/lang/String;)I");
    if (apiAppBridge->mOnLogMsgId == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("AppBridge_init - cannot find 'onLogMsg' method id");
        return -1;
    }

    //atsc3_onSltTablePresent_ID
    apiAppBridge->atsc3_onSltTablePresent_ID = env->GetMethodID(jniClassReference, "atsc3_onSltTablePresent", "(IIILjava/lang/String;)I");
    if (apiAppBridge->atsc3_onSltTablePresent_ID == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'atsc3_onSltTablePresent_ID' method id");
        return -1;
    }

    //atsc3_onAeatTablePresent_ID
     apiAppBridge->atsc3_onAeatTablePresent_ID = env->GetMethodID(jniClassReference, "atsc3_onAeatTablePresent", "(Ljava/lang/String;)I");
     if (apiAppBridge->atsc3_onAeatTablePresent_ID == NULL) {
         _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'atsc3_onAeatTablePresent_ID' method id");
         return -1;
     }

     //atsc3_onSlsHeldEmissionPresent
     apiAppBridge->atsc3_onSlsHeldEmissionPresent_ID = env->GetMethodID(jniClassReference, "atsc3_onSlsHeldEmissionPresent", "(ILjava/lang/String;)I");
     if (apiAppBridge->atsc3_onSlsHeldEmissionPresent_ID == NULL) {
     	_NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'atsc3_onSlsHeldEmissionPresent_ID' method id");
        	return -1;
     }


    //atsc3_lls_sls_alc_on_route_mpd_patched_ID
    apiAppBridge->atsc3_lls_sls_alc_on_route_mpd_patched_ID = env->GetMethodID(jniClassReference, "atsc3_lls_sls_alc_on_route_mpd_patched", "(I)I");
    if (apiAppBridge->atsc3_lls_sls_alc_on_route_mpd_patched_ID == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'atsc3_lls_sls_alc_on_route_mpd_patched_ID' method id");
        return -1;
    }

    //atsc3_on_alc_object_status_message_ID
    apiAppBridge->atsc3_on_alc_object_status_message_ID = env->GetMethodID(jniClassReference, "atsc3_on_alc_object_status_message", "(Ljava/lang/String;)I");
    if (apiAppBridge->atsc3_on_alc_object_status_message_ID == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'atsc3_on_alc_object_status_message_ID' method id");
        return -1;
    }

    //jjustman-2020-07-27 - atsc3_lls_sls_alc_on_package_extract_completed_ID
    //org.ngbp.libatsc3.middleware.android.a331.PackageExtractEnvelopeMetadataAndPayload
	apiAppBridge->atsc3_lls_sls_alc_on_package_extract_completed_ID = env->GetMethodID(jniClassReference, "atsc3_lls_sls_alc_on_package_extract_completed", "(Lorg/ngbp/libatsc3/middleware/android/a331/PackageExtractEnvelopeMetadataAndPayload;)I");
	if (apiAppBridge->atsc3_lls_sls_alc_on_package_extract_completed_ID == NULL) {
	   _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'atsc3_lls_sls_alc_on_package_extract_completed_ID' method id");
	   return -1;
	}
	
    //jjustman-2020-11-23 - atsc3_on_alc_object_closed_ID
	apiAppBridge->atsc3_on_alc_object_closed_ID = env->GetMethodID(jniClassReference, "atsc3_on_alc_object_closed", "(IIILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
	if (apiAppBridge->atsc3_on_alc_object_closed_ID == NULL) {
	   _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'atsc3_on_alc_object_closed_ID' method id");
	   return -1;
	}

	apiAppBridge->packageExtractEnvelopeMetadataAndPayload_jclass_init_env = env->FindClass("org/ngbp/libatsc3/middleware/android/a331/PackageExtractEnvelopeMetadataAndPayload");

    if (apiAppBridge->packageExtractEnvelopeMetadataAndPayload_jclass_init_env == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'packageExtractEnvelopeMetadataAndPayload_jclass' class reference");
        return -1;
    } else {
        apiAppBridge->packageExtractEnvelopeMetadataAndPayload_jclass_global_ref = (jclass)(env->NewGlobalRef(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_jclass_init_env));
    }

    apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_init_env = env->FindClass("org/ngbp/libatsc3/middleware/android/a331/PackageExtractEnvelopeMetadataAndPayload$MultipartRelatedPayload");
    if (apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_init_env == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'packageExtractEnvelopeMetadataAndPayload$MultipartRelatedPayload_jclass_init_env' class reference");
        return -1;
    } else {
       apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_global_ref = (jclass)(env->NewGlobalRef(apiAppBridge->packageExtractEnvelopeMetadataAndPayload_MultipartRelatedPayload_jclass_init_env));
    }

    apiAppBridge->atsc3_nkd_app_bridge_system_properties_jclass_init_env = env->FindClass("org/ngbp/libatsc3/middleware/android/application/models/AndroidSystemProperties");
    if (apiAppBridge->atsc3_nkd_app_bridge_system_properties_jclass_init_env == NULL) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge_init: cannot find 'SystemProperties' class reference");
        return -1;
    } else {
        apiAppBridge->atsc3_nkd_app_bridge_system_properties_jclass_global_ref = (jclass)(env->NewGlobalRef(apiAppBridge->atsc3_nkd_app_bridge_system_properties_jclass_init_env));
    }

    apiAppBridge->jni_java_util_ArrayList = (jclass) env->NewGlobalRef(env->FindClass("java/util/ArrayList"));
    _NDK_APPLICATION_BRIDGE_TRACE("creating apiAppBridge->jni_java_util_ArrayList");

    apiAppBridge->jni_java_util_ArrayList_cctor = env->GetMethodID(apiAppBridge->jni_java_util_ArrayList, "<init>", "(I)V");
    _NDK_APPLICATION_BRIDGE_TRACE("Atsc3NdkApplicationBridge_init: creating method ref for apiAppBridge->jni_java_util_ArrayList_cctor");
    apiAppBridge->jni_java_util_ArrayList_add  = env->GetMethodID(apiAppBridge->jni_java_util_ArrayList, "add", "(Ljava/lang/Object;)Z");
    _NDK_APPLICATION_BRIDGE_TRACE("Atsc3NdkApplicationBridge_init: creating method ref for  apiAppBridge->jni_java_util_ArrayList_add");

    atsc3_core_service_application_bridge_init(apiAppBridge);
    _NDK_APPLICATION_BRIDGE_INFO("Atsc3NdkApplicationBridge_init: done, with apiAppBridge: %p", apiAppBridge);



    return 0;
}

extern "C"
JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1selectService(JNIEnv *env, jobject thiz,
                                                         jint service_id) {
    _NDK_APPLICATION_BRIDGE_INFO("Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1selectService, service_id: %d\n", (int)service_id);
    int ret = apiAppBridge->atsc3_slt_select_service((int)service_id);

    return ret;
}

extern "C"
JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1select_1additional_1service(JNIEnv *env,
                                                                                jobject thiz,
                                                                                jint service_id) {

    _NDK_APPLICATION_BRIDGE_INFO("Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1select_1additional_1service, additional service_id: %d\n", (int)service_id);
    int ret = apiAppBridge->atsc3_slt_alc_select_additional_service((int)service_id);

    return ret;
}

extern "C"
JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1clear_1additional_1service_1selections(
        JNIEnv *env, jobject thiz) {
    _NDK_APPLICATION_BRIDGE_INFO("Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1clear_1additional_1service_1selections");
    int ret = apiAppBridge->atsc3_slt_alc_clear_additional_service_selections();
    return ret;
}

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1get_1sls_1metadata_1fragments_1content_1locations_1from_1monitor_1service_1id(JNIEnv *env, jobject thiz, jint service_id, jstring to_match_content_type_string) {
    _NDK_APPLICATION_BRIDGE_INFO("Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1get_1sls_1metadata_1fragments_1content_1locations_1from_1monitor_1service_1id, service_id: %d\n", (int)service_id);
    const char* to_match_content_type_weak = env->GetStringUTFChars(to_match_content_type_string, 0);

    vector<string> slt_alc_sls_metadata_fragment_content_locations = apiAppBridge->atsc3_slt_alc_get_sls_metadata_fragments_content_locations_from_monitor_service_id((int)service_id, to_match_content_type_weak);

    jstring str;
    int i;

    jobjectArray slt_alc_sls_metadata_fragment_content_locations_jni = env->NewObjectArray(slt_alc_sls_metadata_fragment_content_locations.size(), env->FindClass("java/lang/String"),0);

    for(i=0; i < slt_alc_sls_metadata_fragment_content_locations.size(); i++) {
        str = env->NewStringUTF(slt_alc_sls_metadata_fragment_content_locations.at(i).c_str());
        env->SetObjectArrayElement(slt_alc_sls_metadata_fragment_content_locations_jni, i, str);
    }
    env->ReleaseStringUTFChars( to_match_content_type_string, to_match_content_type_weak );

    return slt_alc_sls_metadata_fragment_content_locations_jni;
}

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1get_1sls_1route_1s_1tsid_1fdt_1file_1content_1locations_1from_1monitor_1service_1id(
        JNIEnv *env, jobject thiz, jint service_id) {
    _NDK_APPLICATION_BRIDGE_INFO("Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1get_1sls_1route_1s_1tsid_1fdt_1file_1content_1locations_1from_1monitor_1service_1id, service_id: %d\n", (int)service_id);

    vector<string> slt_alc_sls_route_s_tsid_fdt_file_content_locations = apiAppBridge->atsc3_slt_alc_get_sls_route_s_tsid_fdt_file_content_locations_from_monitor_service_id((int)service_id);

    jstring str;
    int i;

    jobjectArray slt_alc_sls_route_s_tsid_fdt_file_content_locations_jni = env->NewObjectArray(slt_alc_sls_route_s_tsid_fdt_file_content_locations.size(), env->FindClass("java/lang/String"),0);

    for(i=0; i < slt_alc_sls_route_s_tsid_fdt_file_content_locations.size(); i++) {
        str = env->NewStringUTF(slt_alc_sls_route_s_tsid_fdt_file_content_locations.at(i).c_str());
        env->SetObjectArrayElement(slt_alc_sls_route_s_tsid_fdt_file_content_locations_jni, i, str);
    }

    return slt_alc_sls_route_s_tsid_fdt_file_content_locations_jni;
}

extern "C"
JNIEXPORT jobject JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1slt_1alc_1get_1system_1properties(
        JNIEnv *env, jobject thiz) {

    jclass jcls = apiAppBridge->atsc3_nkd_app_bridge_system_properties_jclass_global_ref;
    jobject jobj = env->AllocObject(jcls);

    if(!jobj) {
        _NDK_APPLICATION_BRIDGE_ERROR("Atsc3NdkApplicationBridge:get_system_properties::err unable to allocate atsc3_nkd_app_bridge_system_properties_jclass_global_ref instance jobj!");
        return nullptr;
    }

    libatsc3_android_system_properties_t properties = apiAppBridge->getAndroidSystemProperties();

    jstring boot_serialno = env->NewStringUTF(properties.boot_serialno_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "boot_serialno_str", "Ljava/lang/String;"), boot_serialno);

    jstring serialno = env->NewStringUTF(properties.serialno_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "serialno_str", "Ljava/lang/String;"), serialno);

    jstring board_platform = env->NewStringUTF(properties.board_platform_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "board_platform_str", "Ljava/lang/String;"), board_platform);

    jstring build_description = env->NewStringUTF(properties.build_description_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "build_description_str", "Ljava/lang/String;"), build_description);

    jstring build_flavor = env->NewStringUTF(properties.build_flavor_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "build_flavor_str", "Ljava/lang/String;"), build_flavor);

    jstring build_product = env->NewStringUTF(properties.build_product_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "build_product_str", "Ljava/lang/String;"), build_product);

    jstring build_version_incremental = env->NewStringUTF(properties.build_version_incremental_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "build_version_incremental_str", "Ljava/lang/String;"), build_version_incremental);

    jstring product_cpu_abi = env->NewStringUTF(properties.product_cpu_abi_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "product_cpu_abi_str", "Ljava/lang/String;"), product_cpu_abi);

    jstring product_mfg = env->NewStringUTF(properties.product_mfg_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "product_mfg_str", "Ljava/lang/String;"), product_mfg);

    jstring build_version_release = env->NewStringUTF(properties.build_version_release_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "build_version_release_str", "Ljava/lang/String;"), build_version_release);
    env->SetIntField(jobj, env->GetFieldID(jcls, "android_version", "I"), properties.android_version);

    jstring sdk_ver = env->NewStringUTF(properties.sdk_ver_str);
    env->SetObjectField(jobj, env->GetFieldID(jcls, "sdk_ver_str", "Ljava/lang/String;"), sdk_ver);
    env->SetIntField(jobj, env->GetFieldID(jcls, "sdk_ver", "I"), properties.sdk_ver);

    return jobj;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1get_1demod_1pcap_1capture(JNIEnv *env, jobject thiz) {

    jboolean ret = apiAppBridge->atsc3_get_demod_pcap_capture();
    return ret;
}



extern "C"
JNIEXPORT void JNICALL
Java_org_ngbp_libatsc3_middleware_Atsc3NdkApplicationBridge_atsc3_1set_1demod_1pcap_1capture(JNIEnv *env, jobject thiz, jboolean enabled) {
	// TODO: implement atsc3_set_demod_pcap_capture()
	//public native Boolean atsc3_get_demod_pcap_capture();
	//public native void atsc3_set_demod_pcap_capture(Boolean enabled);
	apiAppBridge->atsc3_set_demod_pcap_capture((bool) (enabled == JNI_TRUE));
}