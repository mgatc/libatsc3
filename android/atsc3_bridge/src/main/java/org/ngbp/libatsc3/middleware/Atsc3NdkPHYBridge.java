package org.ngbp.libatsc3.middleware;

import android.util.Log;

import org.ngbp.libatsc3.middleware.android.phy.interfaces.IAtsc3NdkPHYBridgeCallbacks;
import org.ngbp.libatsc3.middleware.android.phy.models.BwPhyStatistics;
import org.ngbp.libatsc3.middleware.android.phy.models.L1D_timePhyInformation;
import org.ngbp.libatsc3.middleware.android.phy.models.RfPhyStatistics;

/**
 *    /Library/Java/JavaVirtualMachines/jdk1.8.0_231.jdk/Contents/Home/bin/javah -d cpp -classpath ../../build/intermediates/classes/debug org.ngbp.libatsc3.middleware.Atsc3NdkPHYBridge
 *
 *  javah -classpath ./bin/classes -d jni org.ngbp.libatsc3.middleware.Atsc3NdkPHYBridge
 *
 *  /Users/jjustman/Desktop/libatsc3/android/sample_app_no_phy/app/src/main/java/org/ngbp/libatsc3/middlewarea
 *
 *  Object ndkStaticJniLoader = null;
 *         try {
 *             ndkStaticJniLoader = Class.forName("org.ngbp.libatsc3.middleware.Atsc3NdkStaticJniLoader");
 *         } catch (ClassNotFoundException e) {
 *             e.printStackTrace();
 *         }
 *         Log.d("Atsc3NdkPHYBridge", "ndkStaticJniLoader class is: "+ndkStaticJniLoader);
 */

public class Atsc3NdkPHYBridge extends Atsc3BridgeNdkStaticJniLoader {

    final static String TAG ="intf";

    IAtsc3NdkPHYBridgeCallbacks mActivity;

    //native jni methods
    @Override
    public native int init();

    public native int setRfPhyStatisticsViewVisible(boolean isRfPhyStatisticsVisible);

    public Atsc3NdkPHYBridge(IAtsc3NdkPHYBridgeCallbacks iAtsc3NdkPHYBridgeCallbacks) {
        mActivity = iAtsc3NdkPHYBridgeCallbacks;
        init();
    }

    int onLogMsg(String msg) {
        Log.d(TAG, msg);
        mActivity.onPhyLogMessage(msg+"\n");
        return 0;
    }

    int onPhyError(String msg) {
        Log.e(TAG, msg);
        mActivity.onPhyError(msg+"\n");
        return 0;
    }


    int atsc3_rf_phy_status_callback(int rfstat_lock,
                                     int rssi_1000,
                                     int modcod_valid,
                                     int plp_fec_type,
                                     int plp_mod,
                                     int plp_cod,
                                     int nSnr1000,
                                     int ber_pre_ldpc_e7,
                                     int ber_pre_bch_e9,
                                     int fer_post_bch_e6,
                                     int demod_lock_status,
                                     int cpu_status,
                                     int plp_any,
                                     int plp_all) {

        RfPhyStatistics rfPhyStatistics = new RfPhyStatistics(rfstat_lock,
                rssi_1000,
                modcod_valid,
                plp_fec_type,
                plp_mod,
                plp_cod,
                nSnr1000,
                ber_pre_ldpc_e7,
                ber_pre_bch_e9,
                fer_post_bch_e6,
                demod_lock_status,
                cpu_status,
                plp_any,
                plp_all);

        mActivity.pushRfPhyStatisticsUpdate(rfPhyStatistics);

        return 0;
    }

    int atsc3_rf_phy_status_callback_with_rf_phy_statistics_type(RfPhyStatistics rfPhyStatistics) {
        mActivity.pushRfPhyStatisticsUpdate(rfPhyStatistics);
        return 0;
    }

    int atsc3_updateRfBwStats(long total_pkts, long total_bytes, int total_lmts) {
        BwPhyStatistics bwPhyStatistics = new BwPhyStatistics(total_pkts, total_bytes, total_lmts);
        mActivity.pushBwPhyStatistics(bwPhyStatistics);
        return 0;
    }

    int atsc3_l1d_time_information_callback(byte l1B_time_info_flag, long l1D_time_sec, int l1D_time_msec, int l1D_time_usec, int l1D_time_nsec) {
        L1D_timePhyInformation l1d_timePhyInformation = new L1D_timePhyInformation(l1B_time_info_flag, l1D_time_sec, l1D_time_msec, l1D_time_usec, l1D_time_nsec);
        mActivity.pushL1d_TimeInfo(l1d_timePhyInformation);
        return 0;
    }
}
