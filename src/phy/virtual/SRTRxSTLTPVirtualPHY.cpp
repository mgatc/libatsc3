#include "SRTRxSTLTPVirtualPHY.h"

int _ATSC3_SRTRXSTLTPVIRTUALPHY_INFO_ENABLED = 1;
int _ATSC3_SRTRXSTLTPVIRTUALPHY_DEBUG_ENABLED = 0;
int _ATSC3_SRTRXSTLTPVIRTUALPHY_TRACE_ENABLED = 0;

std::hash<std::thread::id> __SRTRxSTLTPVirtualPHY_thread_hasher__;

SRTRxSTLTPVirtualPHY::SRTRxSTLTPVirtualPHY() {

    //jjustman-2020-08-31 - TODO: add in impld' callback for     atsc3_core_service_application_bridge_reset_context();
	atsc3_srt_live_receiver_context = atsc3_srt_live_receiver_context_new();
	atsc3_srt_live_receiver_context_set_rx_udp_packet_process_callback_with_context(atsc3_srt_live_receiver_context, SRTRxSTLTPVirtualPHY::Atsc3_srt_live_rx_udp_packet_process_callback_with_context, (void*) this);

	atsc3_stltp_depacketizer_context = atsc3_stltp_depacketizer_context_new();

	atsc3_stltp_depacketizer_context->atsc3_stltp_baseband_alp_packet_collection_callback_with_context = &SRTRxSTLTPVirtualPHY::Atsc3_stltp_baseband_alp_packet_collection_callback_with_context;
	atsc3_stltp_depacketizer_context->atsc3_stltp_baseband_alp_packet_collection_callback_context = (void*)this;

	atsc3_stltp_depacketizer_context_set_all_plps(atsc3_stltp_depacketizer_context);
}

SRTRxSTLTPVirtualPHY::SRTRxSTLTPVirtualPHY(string srtConnectionSource) : SRTRxSTLTPVirtualPHY() {
    atsc3_srt_live_receiver_context_set_srt_source_connection_string(atsc3_srt_live_receiver_context, srtConnectionSource.c_str());
}

/*
 * default IPHY impl's here
 */

int SRTRxSTLTPVirtualPHY::init()
{
    return 0;
}

int SRTRxSTLTPVirtualPHY::run()
{
    int ret = 0;
    if(this->atsc3_srt_live_receiver_context) {
        ret = this->atsc3_srt_thread_run();
    }
    return ret;
}

bool SRTRxSTLTPVirtualPHY::is_running() {
    return this->is_srt_running();
}

int SRTRxSTLTPVirtualPHY::stop()
{
    int ret = 0;
    ret = this->atsc3_srt_thread_stop();

    return ret;
}

int SRTRxSTLTPVirtualPHY::deinit()
{
    this->stop();
    delete this;
    return 0;
}

void SRTRxSTLTPVirtualPHY::set_srt_source_connection_string(const char* srt_source_connection_string) {

	atsc3_srt_live_receiver_context_set_srt_source_connection_string(atsc3_srt_live_receiver_context, srt_source_connection_string);
}


//jjustman-2020-08-10: todo - mutex guard this
int SRTRxSTLTPVirtualPHY::atsc3_srt_thread_run() {
	atsc3_srt_live_receiver_context->should_run = false;
    _SRTRXSTLTP_VIRTUAL_PHY_INFO("atsc3_srt_thread_run: checking for previous srt_thread: producerShutdown: %d, consumerShutdown: %d", srtProducerShutdown, srtConsumerShutdown);

    //e.g. must contain at least srt://
    if(atsc3_srt_live_receiver_context->source_connection_string == NULL || strlen(atsc3_srt_live_receiver_context->source_connection_string) < 7) {
        _SRTRXSTLTP_VIRTUAL_PHY_ERROR("srtConnectionSource is empty or too short!");
        return -1;
    }

    while(!srtProducerShutdown || !srtConsumerShutdown) {
    	usleep(100000);
        _SRTRXSTLTP_VIRTUAL_PHY_INFO("atsc3_srt_thread_run: waiting for shutdown for previous srt_thread: producerShutdown: %d, consumerShutdown: %d", srtProducerShutdown, srtConsumerShutdown);
    }

    if(srtProducerThreadPtr.joinable()) {
		srtProducerThreadPtr.join();
	}
	if(srtConsumerThreadPtr.joinable()) {
		srtConsumerThreadPtr.join();
	}

	atsc3_srt_live_receiver_context->should_run = true;
    _SRTRXSTLTP_VIRTUAL_PHY_INFO("atsc3_srt_thread_run: setting atsc3_srt_live_receiver_context->should_run: %d", atsc3_srt_live_receiver_context->should_run);

    srtProducerThreadPtr = std::thread([this](){
		srtProducerShutdown = false;
    	pinProducerThreadAsNeeded();

        _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::atsc3_srt_producer_thread_run with this: %p", this);
        this->srtProducerThreadRun();
        releasePinnedProducerThreadAsNeeded();
    });

    srtConsumerThreadPtr = std::thread([this](){
    	srtConsumerShutdown = false;
    	printf("SRTRxSTLTPVirtualPHY::atsc3_srt_thread_run - before pinConsumerThreadAsNeeded, this: %p", this);
		pinConsumerThreadAsNeeded();
        _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::atsc3_srt_consumer_thread_run, after pinConsumerThreadAsNeeded, with this: %p", this);

        this->srtConsumerThreadRun();
        releasePinnedConsumerThreadAsNeeded();
    });

    //jjustman-2021-09-14 - use %zu to print size_t
    _SRTRXSTLTP_VIRTUAL_PHY_INFO("atsc3_srt_thread_run: threads created, srtProducerThreadPtr id: %zu, srtConsumerThreadPtr id: %lu",
                                 __SRTRxSTLTPVirtualPHY_thread_hasher__(srtProducerThreadPtr.get_id()),
                                 __SRTRxSTLTPVirtualPHY_thread_hasher__(srtConsumerThreadPtr.get_id()));

    return 0;
}

/*
 * jjustman-2020-10-20 - wait up to 10s for SRT RX connection to unwind
 */
int SRTRxSTLTPVirtualPHY::srtLocalCleanup() {
    int spinlock_count = 0;
    while(spinlock_count++ < 100 && (!srtProducerShutdown || !srtConsumerShutdown)) {
        _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::srtLocalCleanup: waiting for srtProducerShutdown: %d, srtConsumerShutdown: %d, atsc3_srt_live_receiver_context->should_run: %d",
                                     srtProducerShutdown, srtConsumerShutdown, atsc3_srt_live_receiver_context->should_run);
        usleep(100000);
    }

    if(!srtProducerShutdown || !srtConsumerShutdown) {
        _SRTRXSTLTP_VIRTUAL_PHY_WARN("SRTRxSTLTPVirtualPHY::srtLocalCleanup: expired spinlock count: %d, bailing waiting for srtProducerShutdown: %d, srtConsumerShutdown: %d, atsc3_srt_live_receiver_context->should_run: %d",
                                     spinlock_count, srtProducerShutdown, srtConsumerShutdown, atsc3_srt_live_receiver_context->should_run);
    }

    //release any local resources held in our context
	atsc3_stltp_depacketizer_context_free(&atsc3_stltp_depacketizer_context);
    atsc3_srt_live_receiver_context_free(&atsc3_srt_live_receiver_context);

    //release any remaining block_t* payloads in srt_replay_buffer_queue
    while(srt_rx_buffer_queue.size()) {
        block_t* to_free = srt_rx_buffer_queue.front();
        srt_rx_buffer_queue.pop();
        block_Destroy(&to_free);
    }

    return 0;
}

bool SRTRxSTLTPVirtualPHY::is_srt_running() {
	return atsc3_srt_live_receiver_context && !atsc3_srt_live_receiver_context->is_shutdown && !srtProducerShutdown && !srtConsumerShutdown;
}


int SRTRxSTLTPVirtualPHY::atsc3_srt_thread_stop() {

	if(atsc3_srt_live_receiver_context && !atsc3_srt_live_receiver_context->is_shutdown) {
		atsc3_srt_live_receiver_notify_shutdown(atsc3_srt_live_receiver_context);
	}

    _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::atsc3_srt_thread_stop with this: %p", &srtProducerThreadPtr);
    if(srtProducerThreadPtr.joinable()) {
        srtProducerThreadPtr.join();
    }

    if(srtConsumerThreadPtr.joinable()) {
        srtConsumerThreadPtr.join();
    }
    _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::atsc3_srt_thread_stop: stopped with this: %p", &srtProducerThreadPtr);

    srtLocalCleanup();
    return 0;
}


/**
 * TODO:  jjustman-2019-10-10: implement srt replay in new superclass
 *         -D__MOCK_srt_REPLAY__ in the interim
 *
 * @return 0
 *
 *
 * borrowed from libatsc3/test/atsc3_srt_replay_test.c
 */

int SRTRxSTLTPVirtualPHY::srtProducerThreadRun() {
	int res = 0;

    int packet_push_count = 0;

    _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::srtProducerThreadRun with this: %p", this);

    if(!atsc3_stltp_depacketizer_context) {
        _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::srtProducerThreadRun - ERROR - no atsc3_stltp_depacketizer_context!");
        atsc3_srt_live_receiver_context->should_run = false;
        return -1;
    }

    atsc3_srt_live_receiver_context->should_run = true;
    res = atsc3_srt_live_receiver_start_in_proc(this->atsc3_srt_live_receiver_context);

    atsc3_srt_live_receiver_context->should_run = false;
    _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::RunsrtThreadParser - unwinding thread, atsc3_srt_live_receiver_context->should_run is false, res: %d", res);

    //unlock our consumer thread
    lock_guard<mutex> srt_replay_buffer_queue_guard(srt_rx_buffer_queue_mutex);
    srt_rx_condition.notify_one();

    srtProducerShutdown = true;

    //thread unwound here
    return res;
}

int SRTRxSTLTPVirtualPHY::srtConsumerThreadRun() {

    queue<block_t *> to_dispatch_queue; //perform a shallow copy so we can exit critical section asap
    queue<block_t *> to_purge_queue; //perform a shallow copy so we can exit critical section asap
    while (atsc3_srt_live_receiver_context->should_run) {
        {
            //critical section, locks are auto-acquired
            unique_lock<mutex> condition_lock(srt_rx_buffer_queue_mutex);
            srt_rx_condition.wait(condition_lock);
            unique_lock<mutex> srt_replay_buffer_queue_guard(srt_rx_live_receiver_buffer_queue_mutex);

            while (srt_rx_live_receiver_buffer_queue.size()) {
                to_dispatch_queue.push(srt_rx_live_receiver_buffer_queue.front());
                srt_rx_live_receiver_buffer_queue.pop();
            }
            srt_replay_buffer_queue_guard.unlock();
            condition_lock.unlock();
        }

        _SRTRXSTLTP_VIRTUAL_PHY_DEBUG("SRTRxSTLTPVirtualPHY::srtConsumerThreadRun: atsc3_stltp_depacketizer_from_blockt: to_dispatch_queue size is: %ld", to_dispatch_queue.size());
        while(to_dispatch_queue.size()) {
            block_t* phy_payload_to_process = to_dispatch_queue.front();

            //jjustman-2020-08-11 - dispatch this for processing against our stltp_depacketizer context
            if(!atsc3_stltp_depacketizer_from_blockt(&phy_payload_to_process, atsc3_stltp_depacketizer_context)) {
                //we were unable to process this block
                _SRTRXSTLTP_VIRTUAL_PHY_INFO("SRTRxSTLTPVirtualPHY::srtConsumerThreadRun: atsc3_stltp_depacketizer_from_blockt returned false, block: %p, i_pos: %d, p_size: %d",
                                              phy_payload_to_process, phy_payload_to_process->i_pos, phy_payload_to_process->p_size);
            }

            //jjustman-2021-01-13 - push this in our queue to purge, since it was created via block_Duplicate(...)
            to_purge_queue.push(phy_payload_to_process);

            to_dispatch_queue.pop();
        }

        _SRTRXSTLTP_VIRTUAL_PHY_DEBUG("SRTRxSTLTPVirtualPHY::srtConsumerThreadRun: to_purge_queue size is: %ld", to_purge_queue.size());

        while(to_purge_queue.size()) {
            block_t *phy_payload_to_purge = to_purge_queue.front();
            block_Destroy(&phy_payload_to_purge);
            to_purge_queue.pop();
        }
        this_thread::yield();

    }
    srtConsumerShutdown = true;

    return 0;
}



void SRTRxSTLTPVirtualPHY::Atsc3_srt_live_rx_udp_packet_process_callback_with_context(block_t* block, void* context) {
	SRTRxSTLTPVirtualPHY* srtRxSTLTPVirtualPHY = (SRTRxSTLTPVirtualPHY*)context;
	srtRxSTLTPVirtualPHY->atsc3_srt_live_rx_udp_packet_received(block);
}

#define _ATSC3_SRT_STLTP_LIVE_BUFFER_QUEUE_RX_CONDITION_NOTIFY_QUEUE_SIZE_ 25
//hand this SRT datagram off to our STLTP listener queue
//jjustman-2020-08-17 - TODO: SRT flows are only a single dip:dport, so we will need to configure the STLTP context accordingly with the first packet from our received flow...
//jjustman-2020-08-17 - TODO: buffer this as needed with an internal queue and then push to srt_rx_buffer_queue
void SRTRxSTLTPVirtualPHY::atsc3_srt_live_rx_udp_packet_received(block_t* block) {
	lock_guard<mutex> srt_replay_buffer_queue_guard(srt_rx_live_receiver_buffer_queue_mutex);
	srt_rx_live_receiver_buffer_queue.push(block_Duplicate(block));
	if(srt_rx_live_receiver_buffer_queue.size() > _ATSC3_SRT_STLTP_LIVE_BUFFER_QUEUE_RX_CONDITION_NOTIFY_QUEUE_SIZE_) {
		_SRTRXSTLTP_VIRTUAL_PHY_DEBUG("SRTRxSTLTPVirtualPHY::srtProducerThreadCallback::atsc3_srt_live_rx_udp_packet_received,  srt_rx_live_receiver_buffer_queue size is: %d", srt_rx_live_receiver_buffer_queue.size());

		srt_rx_condition.notify_one();
	}
}


void SRTRxSTLTPVirtualPHY::Atsc3_stltp_baseband_alp_packet_collection_callback_with_context(atsc3_alp_packet_collection_t* atsc3_alp_packet_collection, void* context) {
	SRTRxSTLTPVirtualPHY* srtRxSTLTPVirtualPHY = (SRTRxSTLTPVirtualPHY*)context;
	srtRxSTLTPVirtualPHY->atsc3_stltp_baseband_alp_packet_collection_received(atsc3_alp_packet_collection);
}

//jjustman-2021-01-20 - push our decoded LMT for atsc3_core_service_player_bridge::atsc3_core_service_bridge_process_packet_from_plp_and_block
// which now gates on having a LMT before processing _any_ packets to prevent a premature dispatch
// from the application layer of SLT for service selection without having a mapping of IP flow to PLP
//
// copy/paste warning: see PcapSTLTPVirtualPHY.cpp

void SRTRxSTLTPVirtualPHY::atsc3_stltp_baseband_alp_packet_collection_received(atsc3_alp_packet_collection_t* atsc3_alp_packet_collection) {

	for(int i=0; i < atsc3_alp_packet_collection->atsc3_alp_packet_v.count; i++) {
		atsc3_alp_packet_t* atsc3_alp_packet = atsc3_alp_packet_collection->atsc3_alp_packet_v.data[i];
		if(atsc3_alp_packet && atsc3_alp_packet->alp_payload) {
			block_Rewind(atsc3_alp_packet->alp_payload);

			//if we are an IP packet, push this via our IAtsc3NdkPHYClient callback
			if(atsc3_phy_rx_udp_packet_process_callback && atsc3_alp_packet && atsc3_alp_packet->alp_packet_header.packet_type == 0x0) {
				atsc3_phy_rx_udp_packet_process_callback(atsc3_alp_packet->plp_num, atsc3_alp_packet->alp_payload);
			} else if (atsc3_phy_rx_link_mapping_table_process_callback && atsc3_alp_packet && atsc3_alp_packet->alp_packet_header.packet_type == 0x4) {

                atsc3_link_mapping_table_t *atsc3_link_mapping_table_pending = atsc3_alp_packet_extract_lmt(atsc3_alp_packet);

                if (atsc3_link_mapping_table_pending) {
                    atsc3_link_mapping_table_t *atsc3_link_mapping_table_to_free = atsc3_phy_rx_link_mapping_table_process_callback(atsc3_link_mapping_table_pending);

                    if (atsc3_link_mapping_table_to_free) {
                        atsc3_link_mapping_table_free(&atsc3_link_mapping_table_to_free);
                    }
                }
			}
		}
	}
}



