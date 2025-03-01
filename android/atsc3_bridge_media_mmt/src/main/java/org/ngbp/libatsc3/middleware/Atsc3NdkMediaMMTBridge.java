package org.ngbp.libatsc3.middleware;

import android.util.Log;

import androidx.annotation.Nullable;

import com.google.protobuf.InvalidProtocolBufferException;

import org.ngbp.libatsc3.middleware.android.ATSC3PlayerFlags;
import org.ngbp.libatsc3.middleware.android.application.interfaces.IAtsc3NdkMediaMMTBridgeCallbacks;
import org.ngbp.libatsc3.middleware.android.mmt.MfuByteBufferFragment;
import org.ngbp.libatsc3.middleware.android.mmt.MmtPacketIdContext;
import org.ngbp.libatsc3.middleware.android.mmt.MpuMetadata_HEVC_NAL_Payload;
import org.ngbp.libatsc3.middleware.android.mmt.models.MMTAudioDecoderConfigurationRecord;
import org.ngbp.libatsc3.middleware.mmt.pb.MmtAudioProperties;
import org.ngbp.libatsc3.middleware.mmt.pb.MmtCaptionProperties;
import org.ngbp.libatsc3.middleware.mmt.pb.MmtMpTable;
import org.ngbp.libatsc3.middleware.mmt.pb.MmtVideoProperties;

import java.nio.ByteBuffer;

/*
Atsc3NdkMediaMMTBridge: for ExoPlayer plugin support
 */

public class Atsc3NdkMediaMMTBridge extends org.ngbp.libatsc3.middleware.Atsc3NdkMediaMMTBridgeStaticJniLoader
{
    final static String TAG ="intf";

    IAtsc3NdkMediaMMTBridgeCallbacks mActivity;
    public static final Boolean MMT_DISCARD_CORRUPT_FRAMES = true;

    //native jni methods
    @Override
    public native int init(ByteBuffer fragmentBuffer, int maxFragmentCount);

    //free NDK/JNI bound AttachedThread, pseduo finalize()?
    @Override
    public native void release();

    public native int atsc3_process_mmtp_udp_packet(ByteBuffer byteBuffer, int length);

    public native void rewindBuffer();

    /**
     * Creates a Atsc3NdkMediaMMTBridge that uses fragmentBuffer to transfer media fragments or appropriate
     * iAtsc3NdkMediaMMTBridgeCallbacks methods if fragmentBuffer is null or maxFragmentCount is zero or less.
     * @param iAtsc3NdkMediaMMTBridgeCallbacks - bridge events callback
     * @param fragmentBuffer - ring buffer to receive media fragments
     * @param maxFragmentCount - count of fragments in buffer. Single buffer Page size will be calculated as
     *                         size of fragmentBuffer devided by maxFragmentCount.
     */
    public Atsc3NdkMediaMMTBridge(IAtsc3NdkMediaMMTBridgeCallbacks iAtsc3NdkMediaMMTBridgeCallbacks, @Nullable ByteBuffer fragmentBuffer, int maxFragmentCount) {
        Log.w("Atsc3NdkMediaMMTBridge", "Atsc3NdkMediaMMTBridge::cctor");
        mActivity = iAtsc3NdkMediaMMTBridgeCallbacks;
        init(fragmentBuffer, maxFragmentCount);
    }

    public int onLogMsg(String msg) {
        Log.d(TAG, msg);
        mActivity.showMsgFromNative(msg+"\n");
        return 0;
    }

    //jjustman-2021-05-19 - todo: add service_id method callback signature invocation
    public int atsc3_onInitHEVC_NAL_Packet(int packet_id, long mpu_sequence_number, ByteBuffer byteBuffer, int length) {
        Log.d("Atsc3NdkMediaMMTBridge", String.format("atsc3_onInitHEVC_NAL_Packet, packet_id: %d, mpu_sequence_number: %d, length: %d", packet_id, mpu_sequence_number, length));

        MpuMetadata_HEVC_NAL_Payload mpuMetadata_HEVC_NAL_Payload = new MpuMetadata_HEVC_NAL_Payload(packet_id, mpu_sequence_number, byteBuffer, length);

        mActivity.pushMpuMetadata_HEVC_NAL_Payload(mpuMetadata_HEVC_NAL_Payload);

        return 0;
    }

    //jjustman-2021-05-19 - todo: add service_id method callback signature invocation
    public int atsc3_OnInitAudioDecoderConfigurationRecord(int packet_id, long mpu_sequence_number, MMTAudioDecoderConfigurationRecord mmtAudioDecoderConfigurationRecord) {
        Log.d("Atsc3NdkMediaMMTBridge", String.format("atsc3_OnInitAudioDecoderConfigurationRecord, packet_id: %d, mpu_sequence_number: %d, mmtAudioDecoderConfigurationRecord: channel_count: %d, sample_depth: %d, sample_rate: %d, isAC4: %b",
                                                                        packet_id, mpu_sequence_number, mmtAudioDecoderConfigurationRecord.channel_count, mmtAudioDecoderConfigurationRecord.sample_depth, mmtAudioDecoderConfigurationRecord.sample_rate, mmtAudioDecoderConfigurationRecord.audioAC4SampleEntryBox != null));

        mActivity.pushAudioDecoderConfigurationRecord(mmtAudioDecoderConfigurationRecord);

        return 0;
    }

    public int atsc3_notify_sl_hdr_1_present(int service_id, int packet_id) {
        mActivity.notifySlHdr1Present(service_id, packet_id);
        return 0;
    }

    //jjustman-2020-08-10 - TODO - move these out of "global global" scope
    public int atsc3_signallingContext_notify_video_packet_id_and_mpu_timestamp_descriptor(int video_packet_id, long mpu_sequence_number, long mpu_presentation_time_ntp64, long mpu_presentation_time_seconds, int mpu_presentation_time_microseconds) {
        //jjustman-2021-06-02 - TODO: refactor this to packetStatistic collection
        MmtPacketIdContext.video_packet_id = video_packet_id;
        MmtPacketIdContext.video_packet_signalling_information.mpu_sequence_number = mpu_sequence_number;
        MmtPacketIdContext.video_packet_signalling_information.mpu_presentation_time_ntp64 = mpu_presentation_time_ntp64;
        MmtPacketIdContext.video_packet_signalling_information.mpu_presentation_time_seconds = mpu_presentation_time_seconds;
        MmtPacketIdContext.video_packet_signalling_information.mpu_presentation_time_microseconds = mpu_presentation_time_microseconds;

        return 0;
    }

    public int atsc3_signallingContext_notify_audio_packet_id_and_mpu_timestamp_descriptor(int audio_packet_id, long mpu_sequence_number, long mpu_presentation_time_ntp64, long mpu_presentation_time_seconds, int mpu_presentation_time_microseconds) {
        MmtPacketIdContext.createAudioPacketStatistic(audio_packet_id);
        //jjustman-2021-06-02 - TODO: refactor this on a per audio packet_id context basis rather than a single un-tracked packet_id
        MmtPacketIdContext.audio_packet_signalling_information.mpu_sequence_number = mpu_sequence_number;
        MmtPacketIdContext.audio_packet_signalling_information.mpu_presentation_time_ntp64 = mpu_presentation_time_ntp64;
        MmtPacketIdContext.audio_packet_signalling_information.mpu_presentation_time_seconds = mpu_presentation_time_seconds;
        MmtPacketIdContext.audio_packet_signalling_information.mpu_presentation_time_microseconds = mpu_presentation_time_microseconds;

        mActivity.atsc3_signallingContext_notify_audio_packet_id_and_mpu_timestamp_descriptor(audio_packet_id, mpu_sequence_number, mpu_presentation_time_ntp64, mpu_presentation_time_seconds, mpu_presentation_time_microseconds);
        return 0;
    }

    public int atsc3_signallingContext_notify_stpp_packet_id_and_mpu_timestamp_descriptor(int stpp_packet_id, long mpu_sequence_number, long mpu_presentation_time_ntp64, long mpu_presentation_time_seconds, int mpu_presentation_time_microseconds) {
        //jjustman-2021-06-02 - TODO: refactor this to packetStatistic collection
        MmtPacketIdContext.stpp_packet_id = stpp_packet_id;
        MmtPacketIdContext.stpp_packet_signalling_information.mpu_sequence_number = mpu_sequence_number;
        MmtPacketIdContext.stpp_packet_signalling_information.mpu_presentation_time_ntp64 = mpu_presentation_time_ntp64;
        MmtPacketIdContext.stpp_packet_signalling_information.mpu_presentation_time_seconds = mpu_presentation_time_seconds;
        MmtPacketIdContext.stpp_packet_signalling_information.mpu_presentation_time_microseconds = mpu_presentation_time_microseconds;

        return 0;
    }

    public void atsc3_onVideoStreamProperties(byte[] buffer) {
        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            try {
                MmtVideoProperties.MmtVideoPropertiesDescriptor descriptor = MmtVideoProperties.MmtVideoPropertiesDescriptor.parseFrom(buffer);
                mActivity.onVideoStreamProperties(descriptor);
            } catch (InvalidProtocolBufferException e) {
                e.printStackTrace();
            }
        } else {
            //discard...
        }
    }

    public void atsc3_onCaptionAssetProperties(byte[] buffer) {
        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            try {
                MmtCaptionProperties.MmtCaptionPropertiesDescriptor descriptor = MmtCaptionProperties.MmtCaptionPropertiesDescriptor.parseFrom(buffer);
                mActivity.onCaptionAssetProperties(descriptor);
            } catch (InvalidProtocolBufferException e) {
                e.printStackTrace();
            }
        } else {
            //discard...
        }
    }

    public void atsc3_onAudioStreamProperties(byte[] buffer) {
        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            try {
                MmtAudioProperties.MmtAudioPropertiesDescriptor descriptor = MmtAudioProperties.MmtAudioPropertiesDescriptor.parseFrom(buffer);
                mActivity.onAudioStreamProperties(descriptor);
            } catch (InvalidProtocolBufferException e) {
                e.printStackTrace();
            }
        } else {
            //discard...
        }
    }

    public void atsc3_onMpTableSubset(byte[] buffer) {
        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            try {
                MmtMpTable.MmtAssetTable table = MmtMpTable.MmtAssetTable.parseFrom(buffer);
                mActivity.onMpTableSubset(table);
            } catch (InvalidProtocolBufferException e) {
                e.printStackTrace();
            }
        } else {
            //discard...
        }
    }

    public void atsc3_onMpTableComplete(byte[] buffer) {
        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            try {
                MmtMpTable.MmtAssetTable table = MmtMpTable.MmtAssetTable.parseFrom(buffer);
                mActivity.onMpTableComplete(table);
            } catch (InvalidProtocolBufferException e) {
                e.printStackTrace();
            }
        } else {
            //discard...
        }
    }

    public int atsc3_onExtractedSampleDuration(int packet_id, long mpu_sequence_number, long extracted_sample_duration_us) {
        //jjustman-2020-08-19 - audio duration work-around for ac-4
        if (MmtPacketIdContext.isAudioPacket(packet_id) && extracted_sample_duration_us <= 0) {
            extracted_sample_duration_us = MmtPacketIdContext.video_packet_statistics.extracted_sample_duration_us;
            MmtPacketIdContext.getAudioPacketStatistic(packet_id).extracted_sample_duration_us = extracted_sample_duration_us;
            return 0;

        }
        if(extracted_sample_duration_us <= 0) {
            Log.e("atsc3_onExtractedSampleDuration", String.format("extracted sample duration for packet_id: %d, mpu_sequence_number: %d, value %d is invalid", packet_id, mpu_sequence_number, extracted_sample_duration_us));
            return 0;
        }

        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            if (MmtPacketIdContext.video_packet_id == packet_id) {
                MmtPacketIdContext.video_packet_statistics.extracted_sample_duration_us = extracted_sample_duration_us;
            } else if (MmtPacketIdContext.isAudioPacket(packet_id)) {
                MmtPacketIdContext.getAudioPacketStatistic(packet_id).extracted_sample_duration_us = extracted_sample_duration_us;
            } else if (MmtPacketIdContext.stpp_packet_id == packet_id) {
                MmtPacketIdContext.stpp_packet_statistics.extracted_sample_duration_us = extracted_sample_duration_us;
            }
        }
        return 0;
    }

    public int atsc3_setVideoWidthHeightFromTrak(int packet_id, int width, int height) {
        //jjustman-2020-12-17 - TODO: move this to map<packet_id, pair<w, h>>

        MmtPacketIdContext.video_packet_id = packet_id;
        MmtPacketIdContext.video_packet_statistics.width = width;
        MmtPacketIdContext.video_packet_statistics.height = height;

        return 0;
    }

    public int atsc3_onMfuPacket(int packet_id, long mpu_sequence_number, int sample_number, ByteBuffer byteBuffer, int length, long presentationTimeUs, int mfu_fragment_count_expected) {

        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            if(length > 0) {
                MfuByteBufferFragment mfuByteBufferFragment = new MfuByteBufferFragment(packet_id, mpu_sequence_number, sample_number, byteBuffer, length, presentationTimeUs, mfu_fragment_count_expected, mfu_fragment_count_expected);
                mActivity.pushMfuByteBufferFragment(mfuByteBufferFragment);
            } else {
                Log.e("atsc3_onMfuPacket", String.format("packetId: %d, mpu_sequence_number: %d, sample_number: %d has no length!",
                        packet_id,
                        mpu_sequence_number,
                        sample_number));
            }
        } else {
            //discard...
        }
        return 0;
    }

    public int atsc3_onMfuPacketCorrupt(int packet_id, long mpu_sequence_number, int sample_number, ByteBuffer byteBuffer, int length, long presentationTimeUs, int mfu_fragment_count_expected, int mfu_fragment_count_rebuilt) {
        Log.e("atsc3_onMfuPacketCorrupt", String.format("packetId: %d, mpu_sequence_number: %d, sample_number: %d has no length!",
                packet_id,
                mpu_sequence_number,
                sample_number));

        if(MMT_DISCARD_CORRUPT_FRAMES) {
            return -1;
        }

        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            if(length > 0) {
                MfuByteBufferFragment mfuByteBufferFragment = new MfuByteBufferFragment(packet_id, mpu_sequence_number, sample_number, byteBuffer, length, presentationTimeUs, mfu_fragment_count_expected, mfu_fragment_count_rebuilt);

                mActivity.pushMfuByteBufferFragment(mfuByteBufferFragment);
            } else {
                Log.e("atsc3_onMfuPacketCorrupt", String.format("packetId: %d, mpu_sequence_number: %d, sample_number: %d has no length!",
                        packet_id,
                        mpu_sequence_number,
                        sample_number));
            }
        } else {
            //discard...
        }
        return 0;
    }

    public int atsc3_onMfuPacketCorruptMmthSampleHeader(int packet_id, long mpu_sequence_number, int sample_number, ByteBuffer byteBuffer, int length, long presentationTimeUs,  int mfu_fragment_count_expected, int mfu_fragment_count_rebuilt) {
        Log.e("atsc3_onMfuPacketCorruptMmthSampleHeader", String.format("packetId: %d, mpu_sequence_number: %d, sample_number: %d has no length!",
                packet_id,
                mpu_sequence_number,
                sample_number));

        if(MMT_DISCARD_CORRUPT_FRAMES) {
            return -1;
        }
        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            if(length > 0 ) {
                MfuByteBufferFragment mfuByteBufferFragment = new MfuByteBufferFragment(packet_id, mpu_sequence_number, sample_number, byteBuffer, length, presentationTimeUs, mfu_fragment_count_expected, mfu_fragment_count_rebuilt);

                mActivity.pushMfuByteBufferFragment(mfuByteBufferFragment);
            } else {
                Log.e("atsc3_onMfuPacketCorruptMmthSampleHeader", String.format("packetId: %d, mpu_sequence_number: %d, sample_number: %d has no length!",
                        packet_id,
                        mpu_sequence_number,
                        sample_number));
            }
        } else {
            //discard...
        }
        return 0;
    }

    public int atsc3_onMfuSampleMissing(int packet_id, long mpu_sequence_number, int sample_number) {
        Log.e("atsc3_onMfuSampleMissing", String.format("packetId: %d, mpu_sequence_number: %d, sample_number: %d has no length!",
                packet_id,
                mpu_sequence_number,
                sample_number));
        if(ATSC3PlayerFlags.ATSC3PlayerStartPlayback) {
            if (MmtPacketIdContext.video_packet_id == packet_id) {
                MmtPacketIdContext.video_packet_statistics.missing_mfu_samples_count++;
            } else if (MmtPacketIdContext.isAudioPacket(packet_id)) {
                MmtPacketIdContext.getAudioPacketStatistic(packet_id).missing_mfu_samples_count++;
            } else {
                //jjustman-2021-06-02 - TODO: add in other generic packet statistic tracking here..MmtPacketIdContext
                // ...
            }
        }
        return 0;
    }
}

