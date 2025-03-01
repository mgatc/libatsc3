//
// Created by Jason Justman on 8/19/20.
//

/*  MarkONE "workarounds" for /dev handle permissions

ADB_IP_ADDRESS="192.168.4.57:5555"
adb connect $ADB_IP_ADDRESS
adb root
adb shell

adb remount
adb push jjlibatsc3 /

--->jjlibatsc3 contents:

setenforce 0
cd /dev
chmod 777 ion
chmod 777 i2c-3
chmod 777 saankhya_dev
while :
do
chmod 777 input/event7
chmod 777 saankhya_sdio_drv
sleep 1
done

 */


//set in android.mk LOCAL_CFLAGS to compute I Q offset values
#ifdef __JJ_CALIBRATION_ENABLED
//jjustman-2021-09-01 - original value
// #define CALIBRATION_BLOCK_SIZE (122 * 8192)        // I&Q DC Calibration Block Size
#define CALIBRATION_BLOCK_SIZE (1024 * 8192)        // I&Q DC Calibration Block Size

int                       calibrationStatus;
#endif


//jjustman-2021-06-07 - uncomment to enable 100ms sleep between tuner status thread i2c polling commands
//#define _JJ_I2C_TUNER_STATUS_THREAD_SLEEP_MS_ENABLED_
//#define _JJ_TUNER_STATUS_THREAD_PRINT_PERF_DIAGNOSTICS_ENABLED_

//poll BSR for ea_wakeup bits
//#define __JJ_DEBUG_BSR_EA_WAKEUP
#define __JJ_DEBUG_BSR_EA_WAKEUP_USLEEP 250000
#define __JJ_DEBUG_BSR_EA_WAKEUP_ITERATIONS 2000000 / __JJ_DEBUG_BSR_EA_WAKEUP_USLEEP

//poll L1D for timeinfo_* fields
//#define __JJ_DEBUG_L1D_TIMEINFO
#define __JJ_DEBUG_L1D_TIMEINFO_USLEEP 500000
#define __JJ_DEBUG_L1D_TIMEINFO_DEMOD_GET_ITERATIONS 2000000 / __JJ_DEBUG_L1D_TIMEINFO_USLEEP


#include "SaankhyaPHYAndroid.h"
SaankhyaPHYAndroid* saankhyaPHYAndroid = nullptr;

CircularBuffer SaankhyaPHYAndroid::cb = nullptr;
mutex SaankhyaPHYAndroid::CircularBufferMutex;

mutex SaankhyaPHYAndroid::CS_global_mutex;
atomic_bool SaankhyaPHYAndroid::cb_should_discard;

int _SAANKHYA_PHY_ANDROID_DEBUG_ENABLED = 1;
int _SAANKHYA_PHY_ANDROID_TRACE_ENABLED = 0;

//jjustman-2021-02-04 - global error flag if i2c txn fails, usually due to demod crash
//      TODO: reset SL demod and re-initalize automatically if this error is SL_ERR_CMD_IF_FAILURE

SL_Result_t     SaankhyaPHYAndroid::global_sl_result_error_flag = SL_OK;
SL_I2cResult_t  SaankhyaPHYAndroid::global_sl_i2c_result_error_flag = SL_I2C_OK;

int SaankhyaPHYAndroid::Last_download_bootloader_firmware_device_id = -1;
int SaankhyaPHYAndroid::Last_tune_freq = -1;

SaankhyaPHYAndroid::SaankhyaPHYAndroid(JNIEnv* env, jobject jni_instance) {
    this->env = env;
    this->jni_instance_globalRef = this->env->NewGlobalRef(jni_instance);
    this->setRxUdpPacketProcessCallback(atsc3_core_service_bridge_process_packet_from_plp_and_block);
    this->setRxLinkMappingTableProcessCallback(atsc3_phy_jni_bridge_notify_link_mapping_table);

    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->atsc3_phy_notify_plp_selection_change_set_callback(&SaankhyaPHYAndroid::NotifyPlpSelectionChangeCallback, this);
    }

    _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::SaankhyaPHYAndroid - created with this: %p", this);
    SaankhyaPHYAndroid::cb_should_discard = false;
}

SaankhyaPHYAndroid::~SaankhyaPHYAndroid() {

    _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::~SaankhyaPHYAndroid - enter: deleting with this: %p", this);

    this->stop();

    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->atsc3_phy_notify_plp_selection_change_clear_callback();
    }

    if(this->atsc3_sl_tlv_block) {
        block_Destroy(&this->atsc3_sl_tlv_block);
    }

    if(atsc3_sl_tlv_payload) {
        atsc3_sl_tlv_payload_free(&atsc3_sl_tlv_payload);
    }

    unique_lock<mutex> CircularBufferMutex_local(CircularBufferMutex);

    if(cb) {
        CircularBufferFree(cb);
    }
    cb = nullptr;
    CircularBufferMutex_local.unlock();


    _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::~SaankhyaPHYAndroid - exit: deleting with this: %p", this);
}

void SaankhyaPHYAndroid::pinProducerThreadAsNeeded() {
    producerJniEnv = new Atsc3JniEnv(atsc3_ndk_phy_saankhya_static_loader_get_javaVM(), "SaankhyaPHYAndroid::producerThread");
}

void SaankhyaPHYAndroid::releasePinnedProducerThreadAsNeeded() {
    if(producerJniEnv) {
        delete producerJniEnv;
        producerJniEnv = nullptr;
    }
}

void SaankhyaPHYAndroid::pinConsumerThreadAsNeeded() {
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::pinConsumerThreadAsNeeded: mJavaVM: %p, atsc3_ndk_application_bridge instance: %p", atsc3_ndk_phy_saankhya_static_loader_get_javaVM(), atsc3_ndk_application_bridge_get_instance());

    consumerJniEnv = new Atsc3JniEnv(atsc3_ndk_phy_saankhya_static_loader_get_javaVM(), "SaankhyaPHYAndroid::consumerThread");
    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->pinConsumerThreadAsNeeded();
    }
}

void SaankhyaPHYAndroid::releasePinnedConsumerThreadAsNeeded() {
    if(consumerJniEnv) {
        delete consumerJniEnv;
        consumerJniEnv = nullptr;
    }

    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->releasePinnedConsumerThreadAsNeeded();
    }
}

void SaankhyaPHYAndroid::pinStatusThreadAsNeeded() {
    statusJniEnv = new Atsc3JniEnv(atsc3_ndk_phy_saankhya_static_loader_get_javaVM(), "SaankhyaPHYAndroid::statusThread");

    if(atsc3_ndk_phy_bridge_get_instance()) {
        atsc3_ndk_phy_bridge_get_instance()->pinStatusThreadAsNeeded();
    }
}

void SaankhyaPHYAndroid::releasePinnedStatusThreadAsNeeded() {
    if(statusJniEnv) {
        delete statusJniEnv;
        statusJniEnv = nullptr;
    }

    if(atsc3_ndk_phy_bridge_get_instance()) {
        atsc3_ndk_phy_bridge_get_instance()->releasePinnedStatusThreadAsNeeded();
    }
}


void SaankhyaPHYAndroid::resetProcessThreadStatistics() {
    alp_completed_packets_parsed = 0;
    alp_total_bytes = 0;
    alp_total_LMTs_recv = 0;
}

int SaankhyaPHYAndroid::init()
{
    statusMetricsResetFromTuneChange();
    return 0;
}

int SaankhyaPHYAndroid::run()
{
    return 0;
}

bool SaankhyaPHYAndroid::is_running() {

    return (captureThreadIsRunning && processThreadIsRunning && statusThreadIsRunning);
}

int SaankhyaPHYAndroid::stop()
{
    SL_I2cResult_t sl_res_uninit = SL_I2C_OK;
    SL_Result_t sl_result = SL_OK;
    SL_TunerResult_t sl_tuner_result = SL_TUNER_OK;

    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: enter with this: %p", this);
    _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::stop: enter with this: %p, slUnit: %d, tUnit: %d, captureThreadIsRunning: %d, statusThreadIsRunning: %d, processThreadIsRunning: %d",
                              this,
                              this->slUnit,
                              this->tUnit,
                              this->captureThreadIsRunning,
                              this->statusThreadIsRunning,
                              this->processThreadIsRunning);

    SaankhyaPHYAndroid::cb_should_discard = true;
    statusThreadShouldRun = false;
    captureThreadShouldRun = false;
    processThreadShouldRun = false;

    //tear down status thread first, as its the most 'problematic' with the saankhya i2c i/f processing
    while(this->statusThreadIsRunning) {
        SL_SleepMS(100);
        _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: this->statusThreadIsRunning: %d", this->statusThreadIsRunning);
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: before join for statusThreadHandle");
    if(statusThreadHandle.joinable()) {
        statusThreadHandle.join();
    }
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: after join for statusThreadHandle");

    if(captureThreadIsRunning) {
        _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: setting captureThreadShouldRun: false");
        SL_RxDataStop();
    }
    while(this->captureThreadIsRunning) {
        SL_SleepMS(100);
        _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: this->captureThreadIsRunning: %d", this->captureThreadIsRunning);
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: before join for captureThreadHandle");
    if(captureThreadHandle.joinable()) {
        captureThreadHandle.join();
    }
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: after join for captureThreadHandle");

    if(processThreadIsRunning) {
        _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: setting processThreadShouldRun: false");
        while(this->processThreadIsRunning) {
            SL_SleepMS(100);
            _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: this->processThreadIsRunning: %d", this->processThreadIsRunning);
        }
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: before join for processThreadHandle");
    if(processThreadHandle.joinable()) {
        processThreadHandle.join();
    }
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: after join for processThreadHandle");

    //SL_I2c doesnt use refcounts
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: before SL_I2cUnInit");
    sl_res_uninit = SL_I2cUnInit();
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: after SL_I2cUnInit, sl_res_uninit is: %d", sl_res_uninit);

    //jjustman-2020-10-30 - decrement our sl instance count
    sl_result = SL_DemodUnInit(slUnit);
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: after SL_DemodUnInit, slUnit now: %d, sl_result: %d", slUnit, sl_result);
    sl_result = SL_DemodDeleteInstance(slUnit);
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: after SL_DemodDeleteInstance, slUnit now: %d, sl_result: %d", slUnit, sl_result);

    sl_tuner_result = SL_TunerUnInit(tUnit);
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: after SL_TunerUnInit, tUnit now: %d, sl_tuner_result: %d", tUnit, sl_tuner_result);

    sl_tuner_result = SL_TunerDeleteInstance(tUnit);
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: after SL_TunerDeleteInstance, tUnit now: %d, sl_tuner_result: %d", tUnit, sl_tuner_result);
    tUnit = -1;

    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->atsc3_phy_notify_plp_selection_change_clear_callback();
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::stop: return with this: %p", this);
    return 0;
}
double SaankhyaPHYAndroid::compute_snr(int snr_linear_scale) {
    double snr = (float)snr_linear_scale / 16384;
    snr = 10000.0 * log10(snr); //10

    return snr;
}
/*
 * jjustman-2020-08-23: NOTE - do NOT call delete slApi*, only call deinit() otherwise you will get fortify crashes, ala:
 *  08-24 08:29:32.717 18991 18991 F libc    : FORTIFY: pthread_mutex_destroy called on a destroyed mutex (0x783b5c87b8)
 */

int SaankhyaPHYAndroid::deinit()
{
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::deinit: enter with this: %p", this);

    this->stop();
    delete this;
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::deinit: return after delete this, with this: %p", this);

    return 0;
}

string SaankhyaPHYAndroid::get_sdk_version()  {
    return SL_SDK_VERSION;
};

string SaankhyaPHYAndroid::get_firmware_version() {
    return demodVersion;
};

SL_ConfigResult_t SaankhyaPHYAndroid::configPlatformParams_autodetect(int device_type, string device_path) {

    SL_ConfigResult_t res = SL_CONFIG_OK;
    _SAANKHYA_PHY_ANDROID_DEBUG("configPlatformParams_autodetect:: open with core type: %d, device_path: %s", device_type, device_path.c_str());

    if(device_type == SL_DEVICE_TYPE_MARKONE && device_path.c_str() && !strcasecmp(SL_HOSTINTERFACE_TYPE_MARKONE_PATH, device_path.c_str())) {
        //check device configuration type
        SL_ConfigureLog_markone();
        SL_ConfigureUtils_markone();

        SL_ConfigureGpio_markone();
        SL_GpioInit();

        char platform = 0;
        SL_GetHwRev(&platform);

        //copy+paste warning from sl_gpio_markone SL_Query_HWREV
        markone_evt_version = (platform) & 0xFF;
        markone_evt_version++; //jjustman-2021-11-09: TODO - fixme in kernel, fallback will assume we are running on AA kernel

        if(markone_evt_version == 1) {
            res = configPlatformParams_aa_markone();
        } else if(markone_evt_version == 2) {
            res = configPlatformParams_bb_markone();
        }


    } else if (device_type == SL_DEVICE_TYPE_FX3_KAILASH) {
        //configure as aa FX3
        res = configPlatformParams_aa_fx3();

    } else if (device_type == SL_DEVICE_TYPE_FX3_SILISA) {
        //configure as bb FX3
        res = configPlatformParams_silisa_bb_fx3();

    } else if (device_type == SL_DEVICE_TYPE_FX3_YOGA) {
        //configure as bb FX3
        res = configPlatformParams_yoga_bb_fx3();
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("configPlatformParams_autodetect::return res: %d", res);

    return res;
}
int SaankhyaPHYAndroid::open(int fd, int device_type, string device_path)
{
    //jjustman-2021-11-09 - TODO: add in null initializers
    SL_I2cResult_t                  i2cres;
    SL_Result_t                     slres;
    SL_ConfigResult_t               cres;
    SL_TunerResult_t                tres;
    SL_UtilsResult_t                utilsres;

    unsigned int                    lnaGpioNum = 0x00000000;

    SL_ConfigResult_t               sl_configResult = SL_CONFIG_ERR_NOT_CONFIGURED;

    SL_DemodBootStatus_t            bootStatus = SL_DEMOD_BOOT_STATUS_UNKNOWN;

    int swMajorNo, swMinorNo;
    unsigned int cFrequency = 0;

    if(device_type == JJ_DEVICE_TYPE_USE_FROM_LAST_DOWNLOAD_BOOTLOADER_FIRMWARE) {
        //jjustman-2021-10-24 - hack!
        device_type = SaankhyaPHYAndroid::Last_download_bootloader_firmware_device_id;
        _SAANKHYA_PHY_ANDROID_INFO("open: JJ_DEVICE_TYPE_USE_FROM_LAST_DOWNLOAD_BOOTLOADER_FIRMWARE, with fd: %d, updated to device_type: %d, device_path: %s", fd, device_type, device_path.c_str());
    } else {
        _SAANKHYA_PHY_ANDROID_DEBUG("open: with fd: %d, device_type: %d, device_path: %s", fd, device_type, device_path.c_str());
    }

    sl_configResult = configPlatformParams_autodetect(device_type, device_path);

    if(sl_configResult != SL_CONFIG_OK) {
        _SAANKHYA_PHY_ANDROID_DEBUG("open: configPlatformParams_autodetect failed, with fd: %d, device_path: %s, configResult failed, res: %d", fd, device_path.c_str(), sl_configResult);
        return -1;
    }

    //jjustman-2021-11-09 - don't set libusb fd handle if we are using sdio i/f
    if(fd >= SL_HOSTINTERFACE_TYPE_MARKONE_FD) {
        SL_SetUsbFd(fd);
    }

    /* Tuner Config */
    tunerCfg.bandwidth = SL_TUNER_BW_6MHZ;
    //jjustman-2021-08-18 - testing for 8mhz rasters
    //tunerCfg.bandwidth = SL_TUNER_BW_8MHZ;
    tunerCfg.std = SL_TUNERSTD_ATSC3_0;

    cres = SL_ConfigGetPlatform(&getPlfConfig);
    if (cres == SL_CONFIG_OK)
    {
        printToConsolePlfConfiguration(getPlfConfig);
    }
    else
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("ERROR : SL_ConfigGetPlatform Failed ");
        goto ERROR;
    }

    cres = SL_ConfigSetBbCapture(BB_CAPTURE_DISABLE);
    if (cres != SL_CONFIG_OK)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("ERROR : SL_ConfigSetBbCapture Failed ");
        goto ERROR;
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("%s:%d - before SL_I2cInit()", __FILE__, __LINE__);

    if (getPlfConfig.demodControlIf == SL_DEMOD_CMD_CONTROL_IF_I2C)
    {
        i2cres = SL_I2cInit();
        if (i2cres != SL_I2C_OK)
        {
            if(i2cres == SL_I2C_DEV_NODE_NOT_FOUND) {
                //this is most likely markone auto-probing on a non markone handset
                _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::open() - unable to find /dev/i2c markone handle - (e.g /dev/i2c-3)");
            } else {
                global_sl_i2c_result_error_flag = i2cres;

                printToConsoleI2cError("SL_I2cInit", i2cres);
            }

            goto ERROR;
        }
        else
        {
            cmdIf = SL_CMD_CONTROL_IF_I2C;
            _SAANKHYA_PHY_ANDROID_DEBUG("atsc3NdkClientSlImpl: setting cmdIf: %d", cmdIf);
        }
    }
    else if (getPlfConfig.demodControlIf == SL_DEMOD_CMD_CONTROL_IF_SDIO)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Error:SL_SdioInit failed :Not Supported");
        goto ERROR;
    }
    else if (getPlfConfig.demodControlIf == SL_DEMOD_CMD_CONTROL_IF_SPI)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Error:SL_SpiInit failed :Not Supported");
        goto ERROR;
    }

    //jjustman-2021-01-13 - set our demod as not started
    demodStartStatus = 0;

    /* Demod Config */
    switch (getPlfConfig.boardType)
    {
        case SL_EVB_3000:
            if (getPlfConfig.tunerType == TUNER_NXP)
            {
                afeInfo.spectrum = SL_SPECTRUM_INVERTED;
                afeInfo.iftype = SL_IFTYPE_LIF;
                afeInfo.ifreq = 4.4 + IF_OFFSET;
            }
            else if (getPlfConfig.tunerType == TUNER_SI)
            {
                afeInfo.spectrum = SL_SPECTRUM_NORMAL;
                afeInfo.iftype = SL_IFTYPE_ZIF;
                afeInfo.ifreq = 0.0;
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid Tuner Selection");
            }

            if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_TS)
            {
                outPutInfo.oif = SL_OUTPUTIF_TSSERIAL_LSB_FIRST;
                /* CPLD Reset */
                SL_GpioSetPin(getPlfConfig.cpldResetGpioPin, 0x00);          // Low
                //jjustman-2021-04-13 - updating to 250ms as 100ms may cause issues on ICCM download on KAILASH
                SL_SleepMS(250); // jjustman-2021-06-23 - FIXUP AGAIN?!  was 100ms delay for Toggle
                SL_GpioSetPin(getPlfConfig.cpldResetGpioPin, 0x01);          // High
            }
            else if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_SDIO)
            {
                outPutInfo.oif = SL_OUTPUTIF_SDIO;
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid OutPut Interface Selection");
            }

            afeInfo.iswap = SL_IPOL_SWAP_DISABLE;
            afeInfo.qswap = SL_QPOL_SWAP_DISABLE;
            iqOffSetCorrection.iCoeff1 = 1.0;
            iqOffSetCorrection.qCoeff1 = 1.0;
            iqOffSetCorrection.iCoeff2 = 0.0;
            iqOffSetCorrection.qCoeff2 = 0.0;
            break;

        case SL_EVB_3010:
            if (getPlfConfig.tunerType == TUNER_NXP) {
                afeInfo.spectrum = SL_SPECTRUM_INVERTED;
                afeInfo.iftype = SL_IFTYPE_LIF;
                afeInfo.ifreq = 4.4 + IF_OFFSET;
                _SAANKHYA_PHY_ANDROID_DEBUG("using TUNER_NXP, ifreq: %f", afeInfo.ifreq);

            }  else if (getPlfConfig.tunerType == TUNER_SI) {
                afeInfo.spectrum = SL_SPECTRUM_NORMAL;
                afeInfo.iftype = SL_IFTYPE_ZIF;
                afeInfo.ifreq = 0.0;
                _SAANKHYA_PHY_ANDROID_DEBUG("using TUNER_SI, ifreq: 0");
            } else if (getPlfConfig.tunerType == TUNER_SILABS) {
                afeInfo.spectrum = SL_SPECTRUM_INVERTED;
                afeInfo.iftype = SL_IFTYPE_LIF;
                afeInfo.ifreq = 4.4;
                _SAANKHYA_PHY_ANDROID_DEBUG("using TUNER_SILABS, ifreq: %f", afeInfo.ifreq);
            } else {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid Tuner Selection");
            }

            if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_TS)
            {
                outPutInfo.oif = SL_OUTPUTIF_TSPARALLEL_LSB_FIRST;
            }
            else if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_SDIO)
            {
                outPutInfo.oif = SL_OUTPUTIF_SDIO;
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid Output Interface Selection");
            }

            afeInfo.iswap = SL_IPOL_SWAP_DISABLE;
            afeInfo.qswap = SL_QPOL_SWAP_DISABLE;
            iqOffSetCorrection.iCoeff1 = 1.0;
            iqOffSetCorrection.qCoeff1 = 1.0;
            iqOffSetCorrection.iCoeff2 = 0.0;
            iqOffSetCorrection.qCoeff2 = 0.0;
            break;

        case SL_EVB_4000:
            if (getPlfConfig.tunerType == TUNER_SI  || getPlfConfig.tunerType == TUNER_SI_P)
            {
                afeInfo.spectrum = SL_SPECTRUM_NORMAL;
                afeInfo.iftype = SL_IFTYPE_ZIF;
                afeInfo.ifreq = 0.0;
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid Tuner Selection");
            }

            if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_TS)
            {
                outPutInfo.oif = SL_OUTPUTIF_TSSERIAL_LSB_FIRST;
                /* CPLD Reset */
                SL_GpioSetPin(getPlfConfig.cpldResetGpioPin, 0x00); // Low
                //jjustman-2021-06-23 - additional adjustments for sleep to 250ms
                SL_SleepMS(250); // 100ms delay for Toggle
                SL_GpioSetPin(getPlfConfig.cpldResetGpioPin, 0x01); // High
            }
            else if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_SDIO)
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("%s:%d - SL4000 using SL_DEMOD_OUTPUTIF_SDIO", __FILE__, __LINE__);

                outPutInfo.oif = SL_OUTPUTIF_SDIO;
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid Output Interface Selection");
            }

            afeInfo.iswap = SL_IPOL_SWAP_DISABLE;
            afeInfo.qswap = SL_QPOL_SWAP_ENABLE;
            iqOffSetCorrection.iCoeff1 = 1;
            iqOffSetCorrection.qCoeff1 = 1;
            iqOffSetCorrection.iCoeff2 = 0;
            iqOffSetCorrection.qCoeff2 = 0;


            //jjustman-2022-01-07 - testcase, change from SL_EXT_LNA_CFG_MODE_MANUAL_ENABLE to AUTO and
           //             lnaInfo.lnaMode = SL_EXT_LNA_CFG_MODE_MANUAL_ENABLE;
           lnaInfo.lnaMode = SL_EXT_LNA_CFG_MODE_AUTO;
            //lnaInfo.lnaMode = SL_EXT_LNA_CFG_MODE_MANUAL_BYPASS;

            lnaInfo.lnaGpioNum = (0x00000A00 >> 8); //should be 0xA after shift, d10

            break;

        case SL_KAILASH_DONGLE:
        case SL_KAILASH_DONGLE_2:
            if (getPlfConfig.tunerType == TUNER_SI || getPlfConfig.tunerType == TUNER_SI_P)
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("using SL_KAILASH with SPECTRUM_NORMAL and ZIF");
                afeInfo.spectrum = SL_SPECTRUM_NORMAL;
                afeInfo.iftype = SL_IFTYPE_ZIF;
                afeInfo.ifreq = 0.0;
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid Tuner Type selected ");
            }

            if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_TS)
            {
                outPutInfo.oif = SL_OUTPUTIF_TSPARALLEL_LSB_FIRST;
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid OutPut Interface Selection");
            }

            afeInfo.iswap = SL_IPOL_SWAP_DISABLE;
            afeInfo.qswap = SL_QPOL_SWAP_ENABLE;
            iqOffSetCorrection.iCoeff1 = (float)1.00724023045574;
            iqOffSetCorrection.qCoeff1 = (float)0.998403791546105;
            iqOffSetCorrection.iCoeff2 = (float)0.0432678874719328;
            iqOffSetCorrection.qCoeff2 = (float)0.0436508327768608;
            break;


        case SL_SILISA_DONGLE:

            _SAANKHYA_PHY_ANDROID_DEBUG("Configuring as: SL_SILISA_DONGLE");

            if (getPlfConfig.tunerType == TUNER_SILABS)
            {
                afeInfo.spectrum = SL_SPECTRUM_INVERTED;
                afeInfo.iftype = SL_IFTYPE_LIF;
                if(tunerCfg.bandwidth == SL_TUNER_BW_6MHZ) {
                    afeInfo.ifreq = 4.4;
                } else if(tunerCfg.bandwidth == SL_TUNER_BW_8MHZ) {
                    afeInfo.ifreq = 4.0;
                }
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid Tuner Selection");
            }
            if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_SDIO)
            {
                outPutInfo.oif = SL_OUTPUTIF_SDIO;
            }
            else
            {
                _SAANKHYA_PHY_ANDROID_DEBUG("Invalid OutPut Interface Selection");
            }
            afeInfo.iswap = SL_IPOL_SWAP_DISABLE;
            afeInfo.qswap = SL_QPOL_SWAP_DISABLE;
            iqOffSetCorrection.iCoeff1 = 1.0;
            iqOffSetCorrection.qCoeff1 = 1.0;
            iqOffSetCorrection.iCoeff2 = 0.0;
            iqOffSetCorrection.qCoeff2 = 0.0;
            //sanjay
            sl_silisa_rssi = true;
            break;

        case SL_YOGA_DONGLE:

            _SAANKHYA_PHY_ANDROID_DEBUG("Configuring as: SL_YOGA_DONGLE");

            if (getPlfConfig.tunerType == TUNER_SI_P)
            {
                afeInfo.spectrum = SL_SPECTRUM_NORMAL;
                afeInfo.iftype = SL_IFTYPE_ZIF;
                afeInfo.ifreq = 0.0;
            }
            else
            {
                SL_Printf("\n Invalid Tuner Selection");
            }

            if (getPlfConfig.demodOutputIf == SL_DEMOD_OUTPUTIF_SDIO)
            {
                outPutInfo.oif = SL_OUTPUTIF_SDIO;
            }
            else
            {
                SL_Printf("\n Invalid Output Interface Selection");
            }

            afeInfo.iswap = SL_IPOL_SWAP_DISABLE;
            afeInfo.qswap = SL_QPOL_SWAP_ENABLE;
            iqOffSetCorrection.iCoeff1 = 1;
            iqOffSetCorrection.qCoeff1 = 1;
            iqOffSetCorrection.iCoeff2 = 0;
            iqOffSetCorrection.qCoeff2 = 0;

            lnaInfo.lnaMode = SL_EXT_LNA_CFG_MODE_AUTO;
            lnaInfo.lnaGpioNum = 12;

            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("Invalid Board Type Selected ");
            break;
    }

    if (lnaInfo.lnaMode != SL_EXT_LNA_CFG_MODE_NOT_PRESENT)
    {
        /*
         * GPIO12 is used for LNA Bypass/Enable in Yoga Dongle.
         * It may be different for other boards. Use bits 8 to 15 to specify the same
         */

        lnaInfo.lnaMode = static_cast<SL_ExtLnaModeConfig_t>(lnaInfo.lnaMode | (lnaInfo.lnaGpioNum << 8));
    }

    afeInfo.iqswap = SL_IQSWAP_DISABLE;
    afeInfo.agcRefValue = 125; //afcRefValue in mV //125?
    outPutInfo.TsoClockInvEnable = SL_TSO_CLK_INV_ON;

    _SAANKHYA_PHY_ANDROID_DEBUG("%s:%d - before SL_ConfigGetBbCapture", __FILE__, __LINE__);

    cres = SL_ConfigGetBbCapture(&getbbValue);
    if (cres != SL_CONFIG_OK)
    {
        _SAANKHYA_PHY_ANDROID_ERROR("ERROR : SL_ConfigGetPlatform Failed");
        _SAANKHYA_PHY_ANDROID_DEBUG("%s:%d - ERROR : SL_ConfigGetPlatform Failed", __FILE__, __LINE__);
        _SAANKHYA_PHY_ANDROID_DEBUG("ERROR : SL_ConfigGetPlatform Failed ");
        goto ERROR;
    }

    if (getbbValue)
    {
        atsc3ConfigParams.plpConfig.plp0 = 0x00;
    }
    else
    {
        atsc3ConfigParams.plpConfig.plp0 = 0x00;
    }
    atsc3ConfigParams.plpConfig.plp1 = 0xFF;
    atsc3ConfigParams.plpConfig.plp2 = 0xFF;
    atsc3ConfigParams.plpConfig.plp3 = 0xFF;

    atsc3ConfigParams.region = SL_ATSC3P0_REGION_US;

    _SAANKHYA_PHY_ANDROID_DEBUG("SL_DemodCreateInstance: before invocation, slUnit: %d",slUnit);
    slres = SL_DemodCreateInstance(&slUnit);
    if (slres != SL_OK)
    {
        printToConsoleDemodError("SL_DemodCreateInstance", slres);
        goto ERROR;
    }

    //jjustman-2020-07-20 - create thread for libusb_handle_events for context callbacks
    //jjustman-2020-07-29 - disable
    //pthread_create(&pThreadID, NULL, (THREADFUNCPTR)&atsc3NdkClientSlImpl::LibUSB_Handle_Events_Callback, (void*)this);

#ifdef __JJ_CALIBRATION_ENABLED
    _SAANKHYA_PHY_ANDROID_DEBUG("SL_DemodInit: before, slUnit: %d, cmdIf: %d, std: %d", slUnit, cmdIf, SL_DEMODSTD_INT_CALIBRATION);
    slres = SL_DemodInit(slUnit, cmdIf, SL_DEMODSTD_INT_CALIBRATION);
#else
    _SAANKHYA_PHY_ANDROID_DEBUG("SL_DemodInit: before, slUnit: %d, cmdIf: %d, std: %d", slUnit, cmdIf, SL_DEMODSTD_ATSC3_0);
    slres = SL_DemodInit(slUnit, cmdIf, SL_DEMODSTD_ATSC3_0);
#endif

    if (slres != SL_OK)
    {
        printToConsoleDemodError("SL_DemodInit", slres);
        goto ERROR;
    }
    else
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("SL_DemodInit: SUCCESS, slUnit: %d, slres: %d", slUnit, slres);
    }

    do
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("before SL_DemodGetStatus: slUnit: %d, slres is: %d", slUnit, slres);
        slres = SL_DemodGetStatus(slUnit, SL_DEMOD_STATUS_TYPE_BOOT, (SL_DemodBootStatus_t*)&bootStatus);
        _SAANKHYA_PHY_ANDROID_DEBUG("SL_DemodGetStatus: slUnit: %d, slres is: %d", slUnit, slres);
        if (slres != SL_OK)
        {
            printToConsoleDemodError("SL_DemodGetStatus:Boot", slres);
        }
        SL_SleepMS(250);
    } while (bootStatus != SL_DEMOD_BOOT_STATUS_COMPLETE);

    _SAANKHYA_PHY_ANDROID_DEBUG("Demod Boot Status  : ");
    if (bootStatus == SL_DEMOD_BOOT_STATUS_INPROGRESS)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("%s", "INPROGRESS");
    }
    else if (bootStatus == SL_DEMOD_BOOT_STATUS_COMPLETE)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("%s", "COMPLETED");
    }
    else if (bootStatus == SL_DEMOD_BOOT_STATUS_ERROR)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("%s", "ERROR");
        goto ERROR;
    }

    slres = SL_DemodGetSoftwareVersion(slUnit, &swMajorNo, &swMinorNo);
    if (slres == SL_OK)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Demod SW Version: %d.%d", swMajorNo, swMinorNo);
        demodVersion = to_string(swMajorNo) + "." + to_string(swMinorNo);
    }

    slres = SL_DemodConfigure(slUnit, SL_CONFIGTYPE_AFEIF, &afeInfo);
    if (slres != 0)
    {
        printToConsoleDemodError("SL_DemodConfigure:SL_CONFIGTYPE_AFEIF", slres);
        goto ERROR;
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("SL_DemodConfigure: slUnit: %d, afeInfo: iftype: %d, iqswap: %d, iswap: %d, qswap: %d, spectrum: %d, ifreq: %f, agcRefVal: %d",
                                slUnit,
                                afeInfo.iftype,
                                afeInfo.iqswap,
                                afeInfo.iswap,
                                afeInfo.qswap,
                                afeInfo.spectrum,
                                afeInfo.ifreq,
                                afeInfo.agcRefValue);

    slres = SL_DemodConfigure(slUnit, SL_CONFIGTYPE_IQ_OFFSET_CORRECTION, &iqOffSetCorrection);
    if (slres != 0)
    {
        printToConsoleDemodError("SL_DemodConfigure:SL_CONFIG_TYPE_IQ_OFFSET_CORRECTION", slres);
        goto ERROR;
    }

    slres = SL_DemodConfigure(slUnit, SL_CONFIGTYPE_OUTPUTIF, &outPutInfo);
    if (slres != 0)
    {
        printToConsoleDemodError("SL_DemodConfigure:SL_CONFIGTYPE_OUTPUTIF", slres);
        goto ERROR;
    }

#ifndef __JJ_CALIBRATION_ENABLED


    slres = SL_DemodConfigureEx(slUnit, demodStandard, &atsc3ConfigParams);

    if (slres != 0)
    {
        _SAANKHYA_PHY_ANDROID_ERROR("SL_DemodConfigureEx(%d, demodStandard: %d, %p) returned: %d", slUnit, demodStandard, &atsc3ConfigParams, slres);
        goto ERROR;
    }
#endif

    _SAANKHYA_PHY_ANDROID_DEBUG("SL_DemodConfigure: SL_CONFIGTYPE_EXT_LNA, value: 0x%02x", lnaInfo.lnaMode)
    slres = SL_DemodConfigure(slUnit, SL_CONFIGTYPE_EXT_LNA, (unsigned int *)&lnaInfo.lnaMode);
    if (slres != 0)
    {
        printToConsoleDemodError("SL_DemodConfigure:SL_CONFIGTYPE_EXT_LNA", slres);
        goto ERROR;
    }


    tres = SL_TunerCreateInstance(&tUnit);
    if (tres != 0)
    {
        printToConsoleTunerError("SL_TunerCreateInstance", tres);
        goto ERROR;
    }

    tres = SL_TunerInit(tUnit);
    if (tres != 0)
    {
        printToConsoleTunerError("SL_TunerInit", tres);
        goto ERROR;
    }

    tres = SL_TunerConfigure(tUnit, &tunerCfg);
    if (tres != 0)
    {
        printToConsoleTunerError("SL_TunerConfigure", tres);
        goto ERROR;
    }

    if (getPlfConfig.boardType == SL_EVB_4000 || getPlfConfig.boardType == SL_YOGA_DONGLE || getPlfConfig.tunerType == TUNER_SI_P)
    {
        _SAANKHYA_PHY_ANDROID_INFO("Calling SL_TunerExSetDcOffset with i: %d, q: %d", tunerIQDcOffSet.iOffSet, tunerIQDcOffSet.qOffSet);

        tres = SL_TunerExSetDcOffSet(tUnit, &tunerIQDcOffSet);
        if (tres != 0)
        {
            printToConsoleTunerError("SL_TunerExSetDcOffSet", tres);
            if (getPlfConfig.tunerType == TUNER_SI)
            {
                goto ERROR;
            }
        }
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("OPEN COMPLETE!");
    return 0;

ERROR:

    return -1;
}

int SaankhyaPHYAndroid::tune(int freqKHz, int plpid)
{
    int ret = 0;
    unsigned int cFrequency = 0;
    int isRxDataStartedSpinCount = 0;

    bool has_unlocked_sl_tlv_block_and_cb_mutex = false;

    if(freqKHz == Last_tune_freq) {
        _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::tune - re-tuning to frequency freqKHz (%d)", Last_tune_freq);

//        _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::tune - returning because Last_tune_freq == freqKHz (%d)", Last_tune_freq);
//        return 0;
    }

    //tell any RXDataCallback or process event that we should discard
    SaankhyaPHYAndroid::cb_should_discard = true;

    //acquire our CB mutex so we don't push stale TLV packets
    unique_lock<mutex> CircularBufferMutex_local(CircularBufferMutex);

    //jjustman-2021-03-10 - also acquire our atsc3_sl_tlv_block_mutex so we can safely discard any pending TLV frames
    atsc3_sl_tlv_block_mutex.lock();

    if(cb) {
        //forcibly flush any in-flight TLV packets in cb here by calling, need (cb_should_discard == true),
        // as our type is atomic_bool and we can't printf its value here due to:
        //                  call to implicitly-deleted copy constructor of 'std::__ndk1::atomic_bool' (aka 'atomic<bool>')
        _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::tune - cb_should_discard: %u, cb_GetDataSize: %zu, calling CircularBufferReset(), cb: %p, early in tune() call",
                                   (cb_should_discard == true), CircularBufferGetDataSize(this->cb), cb);
        CircularBufferReset(cb);
    }

    //jjustman-2021-03-10 - destroy (and let processTLVFromCallback) recreate our atsc3_sl_tlv_block and atsc3_sl_tlv_payload to avoid race condition between cb mutex and sl_tlv block processing
    if(atsc3_sl_tlv_block) {
        block_Destroy(&atsc3_sl_tlv_block);
    }

    if(atsc3_sl_tlv_payload) {
        atsc3_sl_tlv_payload_free(&atsc3_sl_tlv_payload);
    }

    //acquire our lock for setting tuning parameters (including re-tuning)
    unique_lock<mutex> SL_I2C_command_mutex_tuner_tune(SL_I2C_command_mutex);
    unique_lock<mutex> SL_PlpConfigParams_mutex_update_plps(SL_PlpConfigParams_mutex, std::defer_lock);

    atsc3_core_service_application_bridge_reset_context();

    /*

       jjustman-2021-05-04 - testing for (seemingly) random YOGA cpu crashes:

          2021-05-04 19:17:38.042 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 627:DEBUG:1620170258.0428:SL_DemodInit: SUCCESS, slUnit: 0, slres: 0
          2021-05-04 19:17:38.043 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 632:DEBUG:1620170258.0432:before SL_DemodGetStatus: slUnit: 0, slres is: 0
          2021-05-04 19:17:38.045 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 634:DEBUG:1620170258.0454:SL_DemodGetStatus: slUnit: 0, slres is: 0
          2021-05-04 19:17:38.295 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 643:DEBUG:1620170258.2957:Demod Boot Status  :
          2021-05-04 19:17:38.296 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 650:DEBUG:1620170258.2960:COMPLETED
          2021-05-04 19:17:38.316 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SL_DemodConfigPlps, instance: 0, plp [0]: 0x00, [1]: 0xff, [2]: 0xff, [3]: 0xff
          2021-05-04 19:17:38.325 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 701:DEBUG:1620170258.3259:Demod SW Version: 3.24
          2021-05-04 19:17:38.669 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 763:DEBUG:1620170258.6698:OPEN COMPLETE!
          2021-05-04 19:17:38.670 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :2431:DEBUG:1620170258.6701:Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_open: fd: 145, return: 0
          2021-05-04 19:17:38.670 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/UsbAtsc3Source: prepareDevices: open with org.ngbp.libatsc3.middleware.android.phy.SaankhyaPHYAndroid@d66354a for path: /dev/bus/usb/002/002, fd: 145, success
          2021-05-04 19:17:38.710 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: atsc3_core_service_player_bridge: 122:WARN :1620170258.7103:atsc3_core_service_application_bridge_reset_context!
          2021-05-04 19:17:38.710 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid::tune: Frequency: 593000, PLP: 0
          2021-05-04 19:17:38.740 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 815:DEBUG:1620170258.7400:Error:SL_TunerSetFrequency :
          2021-05-04 19:17:38.740 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :1412:DEBUG:1620170258.7401: Sl Tuner Operation Failed
    */

    if (getPlfConfig.boardType == SL_SILISA_DONGLE) {
        _SAANKHYA_PHY_ANDROID_DEBUG("::tune - before SL_DemodStart - sleeping for 500ms to avoid double SL_DemodConfigPlps call(s)");
        usleep(500000);
    }

    printf("SaankhyaPHYAndroid::tune: Frequency: %d, PLP: %d", freqKHz, plpid);
    //jjustman-2022-06-03 - hack!

#ifdef __JJ_CALIBRATION_ENABLED
    freqKHz = 575;
    usleep(10000000);


#endif
#ifndef __JJ_CALIBRATION_ENABLED

    if(demodStartStatus) {
        SL_DemodStop(slUnit);
        usleep(1000000);
    }

    demodStartStatus = 0;
#endif

    tres = SL_TunerSetFrequency(tUnit, freqKHz*1000);
    if (tres != 0)
    {
        printToConsoleTunerError("SL_TunerSetFrequency", tres);
        goto ERROR;
    }
    usleep(1000000);

    //jjustman-2022-08-10 - demod start occurs below...

    tres = SL_TunerGetConfiguration(tUnit, &tunerGetCfg);
    if (tres != 0)
    {
        printToConsoleTunerError("SL_TunerGetConfiguration", tres);
        goto ERROR;
    } else {
        if (tunerGetCfg.std == SL_TUNERSTD_ATSC3_0)
        {
            _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Config Std: ATSC3.0");
        }
        else
        {
            _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Config Std: NA");
        }
        switch (tunerGetCfg.bandwidth)
        {
            case SL_TUNER_BW_6MHZ:
                _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Config Bandwidth : 6MHz");
                break;

            case SL_TUNER_BW_7MHZ:
                _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Config Bandwidth : 7MHz");
                break;

            case SL_TUNER_BW_8MHZ:
                _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Config Bandwidth : 8MHz");
                break;

            default:
                _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Config Bandwidth : NA");
        }
    }

    tres = SL_TunerGetFrequency(tUnit, &cFrequency);
    if (tres != 0)
    {
        printToConsoleTunerError("SL_TunerGetFrequency", tres);
        goto ERROR;
    }
    else
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Locked Frequency : %dHz", cFrequency);

    }

    tres = SL_TunerGetStatus(tUnit, &tunerInfo);
    if (tres != 0)
    {
        printToConsoleTunerError("SL_TunerGetStatus", tres);
        goto ERROR;
    }
    else
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Lock status  : ");
        _SAANKHYA_PHY_ANDROID_DEBUG((tunerInfo.status == 1) ? "LOCKED" : "NOT LOCKED");
        _SAANKHYA_PHY_ANDROID_DEBUG("Tuner RSSI: %3.2f dBm", tunerInfo.signalStrength);

        _SAANKHYA_PHY_ANDROID_DEBUG("tuner frequency: %d", cFrequency);
    }

#ifdef __JJ_CALIBRATION_ENABLED
    tunerIQDcOffSet.iOffSet = 00;
    tunerIQDcOffSet.qOffSet = 00;

    if (getPlfConfig.tunerType == TUNER_SI || getPlfConfig.tunerType == TUNER_SI_P)
    {
        SL_Printf("\n\n-------------------------SL Demod Calibration-");
        do
        {
            tres = SL_TunerExSetDcOffSet(tUnit, &tunerIQDcOffSet);
            if (tres != 0)
            {
                printToConsoleTunerError("SL_TunerExSetDcOffSet", tres);
                goto TEST_ERROR;
            }
            SL_SleepMS(50); // Delay to accomdate set configurations at tuner to take effect
            slres = SL_DemodStartCalibration(slUnit, CALIBRATION_BLOCK_SIZE);
            if (tres != 0)
            {
                printToConsoleDemodError("SL_DemodStartCalibration", slres);
                goto TEST_ERROR;
            }

            do
            {
                SL_SleepMS(50); // Delay to accomdate wait for calibration Complete
                slres = SL_DemodGetCalibrationStatus(slUnit, &calibrationStatus);
                if (tres != 0)
                {
                    printToConsoleDemodError("SL_DemodGetCalibrationStatus", slres);
                    goto TEST_ERROR;
                }
            } while (calibrationStatus == 0);
            slres = SL_DemodGetIQOffSet(slUnit, &tunerIQDcOffSet.iOffSet, &tunerIQDcOffSet.qOffSet);
            if (tres != 0)
            {
                printToConsoleDemodError("SL_DemodGetIQOffSet", slres);
                goto TEST_ERROR;
            }
            SL_Printf("-");
        } while (calibrationStatus == 0 || calibrationStatus == 1);

        _SAANKHYA_PHY_ANDROID_INFO("Completing calibration with status: %d", calibrationStatus);
        _SAANKHYA_PHY_ANDROID_INFO("I Off Set Value        : %d", tunerIQDcOffSet.iOffSet);
        _SAANKHYA_PHY_ANDROID_INFO("Q Off Set Value        : %d", tunerIQDcOffSet.qOffSet);

        usleep(10000000);
        tres = SL_TunerExSetDcOffSet(tUnit, &tunerIQDcOffSet);
        //jjustman-2021-09-01 - after getting our IQ offset values, we need to re-initialize and re-configure sdr (including hex f/w linkaage)

    }
    else
    {
TEST_ERROR:
        SL_Printf("\n Invalid Tuner Calibration Failed");
    }


#endif

    //setup shared memory for cb callback (or reset if already allocated)
    if(!cb) {
        cb = CircularBufferCreate(TLV_CIRCULAR_BUFFER_SIZE);
    } else {
        //jjustman-2021-01-19 - clear out our current cb on re-tune
        CircularBufferReset(cb);
        //just in case any last pending SDIO transactions arent completed yet...
        SL_SleepMS(100);
    }

    //check if we were re-initalized and might have an open threads to wind-down
#ifdef __RESPWAN_THREAD_WORKERS
    if(captureThreadHandle.joinable()) {
        captureThreadShouldRun = false;
        _SAANKHYA_PHY_ANDROID_INFO("::Open() - setting captureThreadShouldRun to false, Waiting for captureThreadHandle to join()");
        captureThreadHandle.join();
    }

    if(processThreadHandle.joinable()) {
        processThreadShouldRun = false;
        _SAANKHYA_PHY_ANDROID_INFO("::Open() - setting processThreadShouldRun to false, Waiting for processThreadHandle to join()");
        processThreadHandle.join();
    }

    if(statusThreadHandle.joinable()) {
        statusThreadShouldRun = false;
        _SAANKHYA_PHY_ANDROID_INFO("::Open() - setting statusThreadShouldRun to false, Waiting for statusThreadHandle to join()");
        statusThreadHandle.join();
    }
#endif

    if(!this->captureThreadIsRunning) {
        captureThreadShouldRun = true;
        captureThreadHandle = std::thread([this]() {
            this->captureThread();
        });

        //micro spinlock
        int threadStartupSpinlockCount = 0;
        while(!this->captureThreadIsRunning && threadStartupSpinlockCount++ < 100) {
            usleep(10000);
        }

        if(threadStartupSpinlockCount > 50) {
            _SAANKHYA_PHY_ANDROID_WARN("::Open() - starting captureThread took %d spins, final state: %d",
                    threadStartupSpinlockCount,
                    this->captureThreadIsRunning);
        }
    }

    if(!this->processThreadIsRunning) {
        processThreadShouldRun = true;
        processThreadHandle = std::thread([this]() {
            this->processThread();
        });

        //micro spinlock
        int threadStartupSpinlockCount = 0;
        while (!this->processThreadIsRunning && threadStartupSpinlockCount++ < 100) {
            usleep(10000);
        }

        if (threadStartupSpinlockCount > 50) {
            _SAANKHYA_PHY_ANDROID_WARN("::Open() - starting processThreadIsRunning took %d spins, final state: %d",
                                       threadStartupSpinlockCount,
                                       this->processThreadIsRunning);
        }
    }

    if(!this->statusThreadIsRunning) {
        statusThreadShouldRun = true;
        statusThreadHandle = std::thread([this]() {
            this->statusThread();
        });

        //micro spinlock
        int threadStartupSpinlockCount = 0;
        while (!this->statusThreadIsRunning && threadStartupSpinlockCount++ < 100) {
            usleep(10000);
        }

        if (threadStartupSpinlockCount > 50) {
            _SAANKHYA_PHY_ANDROID_WARN("::Open() - starting statusThread took %d spins, final state: %d",
                                       threadStartupSpinlockCount,
                                       this->statusThreadIsRunning);
        }
    }

    //jjustman-2022-08=10 - set our plps first..

    SL_PlpConfigParams_mutex_update_plps.lock();

    atsc3ConfigParams.plpConfig.plp0 = plpid;
    atsc3ConfigParams.plpConfig.plp1 = 0xFF;
    atsc3ConfigParams.plpConfig.plp2 = 0xFF;
    atsc3ConfigParams.plpConfig.plp3 = 0xFF;


    SL_PlpConfigParams_mutex_update_plps.unlock();
    slres = SL_DemodConfigureEx(slUnit, demodStandard, &atsc3ConfigParams);
    if (slres != 0) {
        printToConsoleDemodError("SL_DemodConfigPlps", slres);
        goto ERROR;
    }

    if(!demodStartStatus) {
        while (SL_IsRxDataStarted() != 1) {
            SL_SleepMS(100);

            if (((isRxDataStartedSpinCount++) % 100) == 0) {
                _SAANKHYA_PHY_ANDROID_WARN("::Open() - waiting for SL_IsRxDataStarted, spinCount: %d", isRxDataStartedSpinCount);
                //jjustman-2020-10-21 - todo: reset demod?
            }
        }

        /*

         jjustman-2021-05-04 - testing for (seemingly) random YOGA cpu crashes:

            2021-05-04 19:17:38.042 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 627:DEBUG:1620170258.0428:SL_DemodInit: SUCCESS, slUnit: 0, slres: 0
            2021-05-04 19:17:38.043 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 632:DEBUG:1620170258.0432:before SL_DemodGetStatus: slUnit: 0, slres is: 0
            2021-05-04 19:17:38.045 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 634:DEBUG:1620170258.0454:SL_DemodGetStatus: slUnit: 0, slres is: 0
            2021-05-04 19:17:38.295 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 643:DEBUG:1620170258.2957:Demod Boot Status  :
            2021-05-04 19:17:38.296 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 650:DEBUG:1620170258.2960:COMPLETED
            2021-05-04 19:17:38.316 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SL_DemodConfigPlps, instance: 0, plp [0]: 0x00, [1]: 0xff, [2]: 0xff, [3]: 0xff
            2021-05-04 19:17:38.325 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 701:DEBUG:1620170258.3259:Demod SW Version: 3.24
            2021-05-04 19:17:38.669 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 763:DEBUG:1620170258.6698:OPEN COMPLETE!
            2021-05-04 19:17:38.670 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :2431:DEBUG:1620170258.6701:Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_open: fd: 145, return: 0
            2021-05-04 19:17:38.670 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/UsbAtsc3Source: prepareDevices: open with org.ngbp.libatsc3.middleware.android.phy.SaankhyaPHYAndroid@d66354a for path: /dev/bus/usb/002/002, fd: 145, success
            2021-05-04 19:17:38.710 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: atsc3_core_service_player_bridge: 122:WARN :1620170258.7103:atsc3_core_service_application_bridge_reset_context!
            2021-05-04 19:17:38.710 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid::tune: Frequency: 593000, PLP: 0
            2021-05-04 19:17:38.740 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          : 815:DEBUG:1620170258.7400:Error:SL_TunerSetFrequency :
            2021-05-04 19:17:38.740 4722-4865/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :1412:DEBUG:1620170258.7401: Sl Tuner Operation Failed


        if (getPlfConfig.boardType == SL_SILISA_DONGLE) {
            _SAANKHYA_PHY_ANDROID_DEBUG("::tune - before SL_DemodStart - sleeping for 2000 seconds to avoid double SL_DemodConfigPlps call(s)");
            usleep(2000000);

        }
         */


        _SAANKHYA_PHY_ANDROID_DEBUG("Starting SLDemod: ");

        slres = SL_DemodStart(slUnit);

        if (!(slres == SL_OK || slres == SL_ERR_ALREADY_STARTED)) {
            _SAANKHYA_PHY_ANDROID_ERROR("Saankhya Demod Start Failed");
            demodStartStatus = 0;
            goto ERROR;
        } else {
            demodStartStatus = 1;
            _SAANKHYA_PHY_ANDROID_DEBUG("SUCCESS");
            //_SAANKHYA_PHY_ANDROID_DEBUG("SL Demod Output Capture: STARTED : sl-tlv.bin");
        }
    } else {
        _SAANKHYA_PHY_ANDROID_DEBUG("SLDemod: already running");
    }

    statusMetricsResetFromTuneChange();

    atsc3_sl_tlv_block_mutex.unlock();
    CircularBufferMutex_local.unlock();
    has_unlocked_sl_tlv_block_and_cb_mutex = true;

    //jjustman-2021-01-19 - allow for cb to start acumulating TLV frames
    SaankhyaPHYAndroid::cb_should_discard = false;
    ret = 0;
    Last_tune_freq = freqKHz;

    goto UNLOCK;

ERROR:
    ret = -1;
    Last_tune_freq = -1;

    //unlock our i2c mutex
UNLOCK:
    SL_I2C_command_mutex_tuner_tune.unlock();
    //jjustman-2022-08-10 - as a failsafe, make sure to unlock atsc3_sl_tlv_block_mutex and CBmutex locks
    if(!has_unlocked_sl_tlv_block_and_cb_mutex) {
        atsc3_sl_tlv_block_mutex.unlock();
        CircularBufferMutex_local.unlock();
    }
    return ret;

}

void SaankhyaPHYAndroid::statusMetricsResetFromTuneChange() {
    _SAANKHYA_PHY_ANDROID_INFO("statusMetricsResetFromContextChange - resetting statusThreadFirstLoopAfterTuneComplete");

    statusThreadFirstLoopAfterTuneComplete = true; //will dump DemodGetconfiguration from statusThread

    statusMetricsResetFromPLPListenChange(); //clear our diag flags and metrics types also...
}

void SaankhyaPHYAndroid::statusMetricsResetFromPLPListenChange() {
    _SAANKHYA_PHY_ANDROID_INFO("statusMetricsResetFromPLPListenChange - resetting statusThreadFirstLoop_*Lock flags and clearing TunerSignalInfo/_Diag's");

    statusThreadFirstLoopAfterTuneComplete_HasBootstrapLock_for_BSR_Diag = false;
    statusThreadFirstLoopAfterTuneComplete_HasL1B_DemodLock_for_L1B_Diag = false;
    statusThreadFirstLoopAfterTuneComplete_HasL1D_DemodLock_for_L1D_Diag = false;

    demodLockStatus = 0;
    cpuStatus = 0;

    //hack for re-initializing our status structs/diag after a tune()
    memset(&tunerInfo,  0, sizeof(SL_TunerSignalInfo_t));
    memset(&perfDiag,   0, sizeof(SL_Atsc3p0Perf_Diag_t));
    memset(&bsrDiag,    0, sizeof(SL_Atsc3p0Bsr_Diag_t));
    memset(&l1bDiag,    0, sizeof(SL_Atsc3p0L1B_Diag_t));
    memset(&l1dDiag,    0, sizeof(SL_Atsc3p0L1D_Diag_t));
}

int SaankhyaPHYAndroid::listen_plps(vector<uint8_t> plps_original_list)
{
    vector<uint8_t> plps;
    for(int i=0; i < plps_original_list.size(); i++) {
        if(plps_original_list.at(i) == 0) {
            //skip, duplicate plp0 will cause demod to fail
        } else {
            bool duplicate = false;
            for(int j=0; j < plps.size(); j++) {
                 if(plps.at(j) == plps_original_list.at(i)) {
                     duplicate = true;
                 }
            }
            if(!duplicate) {
                plps.push_back(plps_original_list.at(i));
            }
        }
    }

    unique_lock<mutex> SL_PlpConfigParams_mutex_update_plps(SL_PlpConfigParams_mutex);

    //jjustman-2022-05-17 - TODO - listen to plp that contains LLS info, may not always be plp0
    if(plps.size() == 0) {
        //we always need to listen to plp0...kinda
        atsc3ConfigParams.plpConfig.plp0 = 0;
        atsc3ConfigParams.plpConfig.plp1 = 0xFF;
        atsc3ConfigParams.plpConfig.plp2 = 0xFF;
        atsc3ConfigParams.plpConfig.plp3 = 0xFF;
    } else if(plps.size() == 1) {
        atsc3ConfigParams.plpConfig.plp0 = 0;
        atsc3ConfigParams.plpConfig.plp1 = plps.at(0);
        atsc3ConfigParams.plpConfig.plp2 = 0xFF;
        atsc3ConfigParams.plpConfig.plp3 = 0xFF;
    } else if(plps.size() == 2) {
        atsc3ConfigParams.plpConfig.plp0 = 0;
        atsc3ConfigParams.plpConfig.plp1 = plps.at(0);
        atsc3ConfigParams.plpConfig.plp2 = plps.at(1);
        atsc3ConfigParams.plpConfig.plp3 = 0xFF;
    } else if(plps.size() == 3) {
        atsc3ConfigParams.plpConfig.plp0 = 0;
        atsc3ConfigParams.plpConfig.plp1 = plps.at(0);
        atsc3ConfigParams.plpConfig.plp2 = plps.at(1);
        atsc3ConfigParams.plpConfig.plp3 = plps.at(2);
    } else if(plps.size() == 4) {
        atsc3ConfigParams.plpConfig.plp0 = plps.at(0);
        atsc3ConfigParams.plpConfig.plp1 = plps.at(1);
        atsc3ConfigParams.plpConfig.plp2 = plps.at(2);
        atsc3ConfigParams.plpConfig.plp3 = plps.at(3);
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("calling SL_DemodConfigPLPS with 0: %02x, 1: %02x, 2: %02x, 3: %02x",
            atsc3ConfigParams.plpConfig.plp0,
            atsc3ConfigParams.plpConfig.plp1,
            atsc3ConfigParams.plpConfig.plp2,
            atsc3ConfigParams.plpConfig.plp3);

    unique_lock<mutex> SL_I2C_command_mutex_demod_configure_plps(SL_I2C_command_mutex);

    slres = SL_DemodConfigureEx(slUnit, demodStandard, &atsc3ConfigParams);

    SL_I2C_command_mutex_demod_configure_plps.unlock();
    SL_PlpConfigParams_mutex_update_plps.unlock();

    if (slres != 0)
    {
        printToConsoleDemodError("SL_DemodConfigPLP", slres);
    }

    statusMetricsResetFromPLPListenChange();

    return slres;
}

void SaankhyaPHYAndroid::dump_plp_list() {
    unique_lock<mutex> SL_PlpConfigParams_mutex_update_plps(SL_PlpConfigParams_mutex);
    unique_lock<mutex> SL_I2C_command_mutex_demod_configure_plps(SL_I2C_command_mutex);


    slres = SL_DemodGetDiagnostics(slUnit, SL_DEMOD_DIAG_TYPE_ATSC3P0_PLP_LLS_INFO, &llsPlpInfo);
    if (slres != SL_OK)
    {
        printToConsoleDemodError("SL_DemodGetLlsPlpList", slres);
        if (slres == SL_ERR_CMD_IF_FAILURE)
        {
            handleCmdIfFailure();
            goto ERROR;
        }
    }

    plpllscount = 0;
    for (int plpIndx = 0; (plpIndx < 64) && (plpllscount < 4); plpIndx++)
    {
        plpInfoVal = ((llsPlpInfo & (llsPlpMask << plpIndx)) == pow(2, plpIndx)) ? 0x01 : 0xFF;

        _SAANKHYA_PHY_ANDROID_DEBUG("PLP: %d, atsc3ConfigParams.plpConfigVal: %d", plpIndx, plpInfoVal);

        if (plpInfoVal == 0x01)
        {
            plpllscount++;
            if (plpllscount == 1)
            {
                atsc3ConfigParams.plpConfig.plp0 = plpIndx;
            }
            else if (plpllscount == 2)
            {
                atsc3ConfigParams.plpConfig.plp1 = plpIndx;
            }
            else if (plpllscount == 3)
            {
                atsc3ConfigParams.plpConfig.plp2 = plpIndx;
            }
            else if (plpllscount == 4)
            {
                atsc3ConfigParams.plpConfig.plp3 = plpIndx;
            }
            else
            {
                plpllscount++;
            }
        }
    }

    if (atsc3ConfigParams.plpConfig.plp0 == -1)
    {
        atsc3ConfigParams.plpConfig.plp0 = 0x00;
    }

ERROR:
    _SAANKHYA_PHY_ANDROID_ERROR("Error:dump_plp_list failed!");

UNLOCK:
    SL_I2C_command_mutex_demod_configure_plps.unlock();
    SL_PlpConfigParams_mutex_update_plps.unlock();
}

int SaankhyaPHYAndroid::download_bootloader_firmware(int fd, int device_type, string device_path) {

    //sanjay
    if (device_type == 3) {
        sl_silisa_rssi = true;
    } else {
        sl_silisa_rssi = false;
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("download_bootloader_firmware, path: %s, device_type: %d, fd: %d",
                                device_path.c_str(), device_type, fd);

    //jjustman-2021-10-24 - super-hacky workaround for preboot firmware d/l and proper device type open on re-enumeration call for now..
    SaankhyaPHYAndroid::Last_download_bootloader_firmware_device_id = device_type;

    SL_ConfigResult_t sl_configResult = SL_CONFIG_OK;
    sl_configResult = configPlatformParams_autodetect(device_type, device_path);

    if(sl_configResult != SL_CONFIG_OK) {
        _SAANKHYA_PHY_ANDROID_DEBUG("download_bootloader_firmware: configPlatformParams_autodetect failed - fd: %d, device_path: %s, configResult failed, res: %d", fd, device_path.c_str(), sl_configResult);
        return -1;
    }

    SL_SetUsbFd(fd);
    SL_I2cResult_t i2cres;

    i2cres = SL_I2cPreInit();
    _SAANKHYA_PHY_ANDROID_DEBUG("download_bootloader_firmware: SL_I2cPreInit returned: %d", i2cres);

    if (i2cres != SL_I2C_OK)
    {
        if(i2cres == SL_I2C_AWAITING_REENUMERATION) {
            _SAANKHYA_PHY_ANDROID_DEBUG("download_bootloader_firmware: INFO:SL_I2cPreInit SL_FX3S_I2C_AWAITING_REENUMERATION");
            return 0;
        } else {
            printToConsoleI2cError("SL_I2cPreInit", i2cres);
        }
    }
    return -1;
}

//jjustman-2020-09-09 KAILASH dongle specific configuration
SL_ConfigResult_t SaankhyaPHYAndroid::configPlatformParams_aa_fx3() {

    SL_ConfigResult_t res = SL_CONFIG_OK;

    sPlfConfig.chipType = SL_CHIP_3010;
    sPlfConfig.chipRev = SL_CHIP_REV_AA;
    sPlfConfig.boardType = SL_KAILASH_DONGLE;
    sPlfConfig.tunerType = TUNER_SI;
    sPlfConfig.hostInterfaceType = SL_HostInterfaceType_FX3;

    sPlfConfig.demodControlIf = SL_DEMOD_CMD_CONTROL_IF_I2C;
    sPlfConfig.demodOutputIf = SL_DEMOD_OUTPUTIF_TS;
    sPlfConfig.demodI2cAddr = 0x30; /* SLDemod 7-bit Physical I2C Address */

    sPlfConfig.demodResetGpioPin = 47;      /* FX3S GPIO 47 connected to Demod Reset Pin */
    sPlfConfig.cpldResetGpioPin = 43;       /* FX3S GPIO 43 connected to CPLD Reset Pin and used only for serial TS Interface  */
    sPlfConfig.tunerResetGpioPin = 23;    /* FX3S GPIO 23 connected to Tuner Reset Pin */

    sPlfConfig.slsdkPath = "."; //jjustman-2020-09-09 use extern object linkages for fx3/hex firmware

    //jjustman-2021-12-11 - todo: fixme
    //reconfigure method callbacks for AA FX3
    sPlfConfig.dispConf = SL_DispatcherConfig_slref;

    /* Set Configuration Parameters */
    res = SL_ConfigSetPlatform(sPlfConfig);

    _SAANKHYA_PHY_ANDROID_DEBUG("configPlatformParams_aa_fx3: with chipType: %d, chipRev: %d, boardType: %d, tunerType: %d, hostInterfaceType: %d, ",
                                sPlfConfig.chipType,
                                sPlfConfig.chipRev,
                                sPlfConfig.boardType,
                                sPlfConfig.tunerType,
                                sPlfConfig.hostInterfaceType);


    return res;
}

SL_ConfigResult_t SaankhyaPHYAndroid::configPlatformParams_aa_markone() {

    SL_ConfigResult_t res = SL_CONFIG_OK;

    sPlfConfig.chipType = SL_CHIP_4000;
    sPlfConfig.chipRev = SL_CHIP_REV_AA;
    sPlfConfig.boardType = SL_EVB_4000; //from venky 2020-09-07 - SL_BORQS_EVT;
    sPlfConfig.tunerType = TUNER_SI;
    sPlfConfig.hostInterfaceType = SL_HostInterfaceType_MarkONE;

    sPlfConfig.demodControlIf = SL_DEMOD_CMD_CONTROL_IF_I2C;
    sPlfConfig.demodOutputIf = SL_DEMOD_OUTPUTIF_SDIO;
    sPlfConfig.demodI2cAddr = 0x30; /* SLDemod 7-bit Physical I2C Address */

    sPlfConfig.slsdkPath = "."; //jjustman-2020-09-09 use extern object linkages for fx3/hex firmware

    sPlfConfig.demodResetGpioPin = 12;   /* 09-10 03:25:56.498     0     0 E SAANKHYA: Reset low GPIO: 12 */
    sPlfConfig.demodI2cAddr3GpioPin = 0;   /* not used on markone AA ? */

    //from: sdm660-qrd-kf.dtsi
    sPlfConfig.tunerResetGpioPin = 13;

    //jjustman-2021-12-11 - todo: fixme
    sPlfConfig.dispConf = SL_DispatcherConfig_markone;

    /* Set Configuration Parameters */
    res = SL_ConfigSetPlatform(sPlfConfig);

    _SAANKHYA_PHY_ANDROID_DEBUG("configPlatformParams_aa_markone: with chipType: %d, chipRev: %d, boardType: %d, tunerType: %d, hostInterfaceType: %d, res: %d",
            sPlfConfig.chipType,
            sPlfConfig.chipRev,
            sPlfConfig.boardType,
            sPlfConfig.tunerType,
            sPlfConfig.hostInterfaceType,
            res);

    //jjustman-2021-11-09 - calibrated values on AA @ 533mhz
    tunerIQDcOffSet.iOffSet = 14;
    tunerIQDcOffSet.qOffSet = 14;

    return res;
}


//jjustman-2020-09-09 silisa dongle specific configuration
SL_ConfigResult_t SaankhyaPHYAndroid::configPlatformParams_silisa_bb_fx3() {

    SL_ConfigResult_t res = SL_CONFIG_OK;

    sPlfConfig.chipType = SL_CHIP_3000;
    sPlfConfig.chipRev = SL_CHIP_REV_BB;
    sPlfConfig.boardType = SL_SILISA_DONGLE;
    sPlfConfig.tunerType = TUNER_SILABS;
    sPlfConfig.hostInterfaceType = SL_HostInterfaceType_FX3;

    sPlfConfig.demodControlIf = SL_DEMOD_CMD_CONTROL_IF_I2C;
    sPlfConfig.demodOutputIf = SL_DEMOD_OUTPUTIF_SDIO;
    sPlfConfig.demodI2cAddr = 0x30; /* SLDemod 7-bit Physical I2C Address */

    sPlfConfig.demodResetGpioPin = 47;      /* FX3S GPIO 47 connected to Demod Reset Pin - S1_SD1/UART_CTS*/
    sPlfConfig.cpldResetGpioPin = 43;       /* FX3S GPIO 43 connected to CPLD Reset Pin and used only for serial TS Interface  */
    sPlfConfig.tunerResetGpioPin = 23;    /* FX3S GPIO 23 connected to Tuner Reset Pin */
    sPlfConfig.demodI2cAddr3GpioPin = 37; /* FX3S GPIO 37 connected to SL3000_I2CADDR3 */

    sPlfConfig.slsdkPath = "."; //jjustman-2020-09-09 use extern object linkages for fx3/hex firmware

    //jjustman-2021-12-11 - todo: fixme
    sPlfConfig.dispConf = SL_DispatcherConfig_slref;

    /* Set Configuration Parameters */
    res = SL_ConfigSetPlatform(sPlfConfig);

    _SAANKHYA_PHY_ANDROID_DEBUG("configPlatformParams_bb_fx3: with chipType: %d, chipRev: %d, boardType: %d, tunerType: %d, hostInterfaceType: %d, ",
                                sPlfConfig.chipType,
                                sPlfConfig.chipRev,
                                sPlfConfig.boardType,
                                sPlfConfig.tunerType,
                                sPlfConfig.hostInterfaceType);


    //reconfigure method callbacks for kailash BB FX3
    SL_DispatcherConfig_slref();

    return res;
}

//jjustman-2020-09-09 yoga dongle specific configuration
SL_ConfigResult_t SaankhyaPHYAndroid::configPlatformParams_yoga_bb_fx3() {

    SL_ConfigResult_t res = SL_CONFIG_OK;

    sPlfConfig.chipType = SL_CHIP_4000;
    sPlfConfig.chipRev = SL_CHIP_REV_BB;
    sPlfConfig.boardType = SL_YOGA_DONGLE;
    sPlfConfig.tunerType = TUNER_SI_P;
    sPlfConfig.hostInterfaceType = SL_HostInterfaceType_FX3;

    sPlfConfig.demodControlIf = SL_DEMOD_CMD_CONTROL_IF_I2C;
    sPlfConfig.demodOutputIf = SL_DEMOD_OUTPUTIF_SDIO;
    sPlfConfig.demodI2cAddr = 0x30; /* SLDemod 7-bit Physical I2C Address */

    sPlfConfig.demodResetGpioPin = 47;      /* FX3S GPIO 47 connected to Demod Reset Pin - S1_SD1/UART_CTS*/
    sPlfConfig.cpldResetGpioPin = 43;       /* FX3S GPIO 43 connected to CPLD Reset Pin and used only for serial TS Interface  */
    sPlfConfig.tunerResetGpioPin = 23;    /* FX3S GPIO 23 connected to Tuner Reset Pin */
    sPlfConfig.demodI2cAddr3GpioPin = 37; /* FX3S GPIO 37 connected to SL3000_I2CADDR3 */

    sPlfConfig.slsdkPath = "."; //jjustman-2020-09-09 use extern object linkages for fx3/hex firmware

    //jjustman-2021-12-11 - todo: fixme
    sPlfConfig.dispConf = SL_DispatcherConfig_slref;

    /* Set Configuration Parameters */
    res = SL_ConfigSetPlatform(sPlfConfig);

    _SAANKHYA_PHY_ANDROID_DEBUG("configPlatformParams_bb_fx3: with chipType: %d, chipRev: %d, boardType: %d, tunerType: %d, hostInterfaceType: %d, ",
                                sPlfConfig.chipType,
                                sPlfConfig.chipRev,
                                sPlfConfig.boardType,
                                sPlfConfig.tunerType,
                                sPlfConfig.hostInterfaceType);


    //reconfigure method callbacks for yoga dongle (bb)
    SL_DispatcherConfig_slref();

    return res;
}

SL_ConfigResult_t SaankhyaPHYAndroid::configPlatformParams_bb_markone() {

    SL_ConfigResult_t res = SL_CONFIG_OK;

    sPlfConfig.chipType = SL_CHIP_4000;
    sPlfConfig.chipRev = SL_CHIP_REV_BB;
    sPlfConfig.boardType = SL_EVB_4000; //from venky 2020-09-07 - SL_BORQS_EVT;
    sPlfConfig.tunerType = TUNER_SI_P;
    sPlfConfig.hostInterfaceType = SL_HostInterfaceType_MarkONE;

    sPlfConfig.demodControlIf = SL_DEMOD_CMD_CONTROL_IF_I2C;
    sPlfConfig.demodOutputIf = SL_DEMOD_OUTPUTIF_SDIO;
    sPlfConfig.demodI2cAddr = 0x30; /* SLDemod 7-bit Physical I2C Address */

    sPlfConfig.slsdkPath = "."; //jjustman-2020-09-09 use extern object linkages for fx3/hex firmware

    sPlfConfig.demodResetGpioPin = 12;   /* 09-10 03:25:56.498     0     0 E SAANKHYA: Reset low GPIO: 12 */
    sPlfConfig.demodI2cAddr3GpioPin = 0;   /* not used on markone AA ? */

    //from: sdm660-qrd-kf.dtsi
    sPlfConfig.tunerResetGpioPin = 13;

    //jjustman-2021-12-11 - todo: fixme
    sPlfConfig.dispConf = SL_DispatcherConfig_markone;

    /* Set Configuration Parameters */
    res = SL_ConfigSetPlatform(sPlfConfig);

    _SAANKHYA_PHY_ANDROID_DEBUG("configPlatformParams_bb_markone: with chipType: %d, chipRev: %d, boardType: %d, tunerType: %d, hostInterfaceType: %d, ",
                                sPlfConfig.chipType,
                                sPlfConfig.chipRev,
                                sPlfConfig.boardType,
                                sPlfConfig.tunerType,
                                sPlfConfig.hostInterfaceType);

    //jjustman-2021-11-09 - calibrated values on evt2 - pre AGND fix
//    tunerIQDcOffSet.iOffSet = 11;
//    tunerIQDcOffSet.qOffSet = 12;

    //jjustman-2022-01-07 - EVT2 SMT1/FA values @575:
    //
    //2022-01-07 16:29:43.724 21558-21758/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :1078:INFO :1641601783.7241:Completing calibration with status: 2
    //2022-01-07 16:29:43.724 21558-21758/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :1079:INFO :1641601783.7242:I Off Set Value        : 13
    //2022-01-07 16:29:43.724 21558-21758/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :1080:INFO :1641601783.7242:Q Off Set Value        : 9

//    tunerIQDcOffSet.iOffSet = 13;
//    tunerIQDcOffSet.qOffSet = 9;

//      jjustman-2022-03-30 - evt2 smt1.5 from borqs assy
//    2022-03-30 12:35:12.187 7751-8148/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :1080:INFO :1648668912.1878:Completing calibration with status: 2
//    2022-03-30 12:35:12.187 7751-8148/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :1081:INFO :1648668912.1878:I Off Set Value        : 12
//    2022-03-30 12:35:12.187 7751-8148/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SaankhyaPHYAndroid.cpp          :1082:INFO :1648668912.1878:Q Off Set Value        : 12
//    2022-03-30 12:35:18.990 7751-7768/com.nextgenbroadcast.mobile.middleware.sample I/ddleware.sampl: Background concurrent copying GC freed 705142(21MB) AllocSpace objects, 28(560KB) LOS objects, 50% free, 22MB/44MB, paused 377us total 1.468s
//    2022-03-30 12:35:22.188 7751-8148/com.nextgenbroadcast.mobile.middleware.sample D/NDK: SL_SiTunerExSetDcOffSet: setting iOffset to: 12, qOffset to: 12

//    tunerIQDcOffSet.iOffSet = 12;
//    tunerIQDcOffSet.qOffSet = 12;
//

//jjustman-2022-06-29 - for 9501c690 BB

//    tunerIQDcOffSet.iOffSet = 10;
//    tunerIQDcOffSet.qOffSet = 12;

//jjustman-2022-07-12 - for 9501c690 BB @ 575mhz

    tunerIQDcOffSet.iOffSet = 10;
    tunerIQDcOffSet.qOffSet = 12;


    return res;
}

void SaankhyaPHYAndroid::handleCmdIfFailure(void)
{
    if(slCmdIfFailureCount < 30) {

        _SAANKHYA_PHY_ANDROID_DEBUG("SL CMD IF FAILURE: cmdIfFailureCount: %d, Cannot continue - leaving demod init for now...", ++slCmdIfFailureCount);
    } else {
        _SAANKHYA_PHY_ANDROID_DEBUG("SL CMD IF FAILURE: cmdIfFailureCount: %d, TODO: reset demod", ++slCmdIfFailureCount);

        //..reset()
    }
}

void SaankhyaPHYAndroid::printToConsoleI2cError(const char* methodName, SL_I2cResult_t err)
{
    switch (err)
    {
        case SL_I2C_ERR_TRANSFER_FAILED:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl I2C Transfer Failed", err);
            break;

        case SL_I2C_ERR_NOT_INITIALIZED:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl I2C Not Initialized", err);
            break;

        case SL_I2C_ERR_BUS_TIMEOUT:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl I2C Bus Timeout", err);
            break;

        case SL_I2C_ERR_LOST_ARBITRATION:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl I2C Lost Arbitration", err);
            break;

        default:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl I2C other error", err);
            break;
    }
}

void SaankhyaPHYAndroid::printToConsoleTunerError(const char* methodName, SL_TunerResult_t err)
{
    switch (err)
    {
        case SL_TUNER_ERR_OPERATION_FAILED:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Tuner Operation Failed", err);
            break;

        case SL_TUNER_ERR_INVALID_ARGS:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Tuner Invalid Argument", err);
            break;

        case SL_TUNER_ERR_NOT_SUPPORTED:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Tuner Not Supported", err);
            break;

        case SL_TUNER_ERR_MAX_INSTANCES_REACHED:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Tuner Maximum Instance Reached", err);
            break;
        default:
            break;
    }
}

void SaankhyaPHYAndroid::printToConsolePlfConfiguration(SL_PlatFormConfigParams_t cfgInfo)
{
    _SAANKHYA_PHY_ANDROID_DEBUG("SL Platform Configuration");
    switch (cfgInfo.boardType)
    {
        case SL_EVB_3000:
            _SAANKHYA_PHY_ANDROID_DEBUG("Board Type  : SL_EVB_3000");
            break;

        case SL_EVB_3010:
            _SAANKHYA_PHY_ANDROID_DEBUG("Board Type  : SL_EVB_3010");
            break;

        case SL_EVB_4000:
            _SAANKHYA_PHY_ANDROID_DEBUG("Board Type  : SL_EVB_4000");
            break;

        case SL_KAILASH_DONGLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("Board Type  : SL_KAILASH_DONGLE");
            break;

        case SL_SILISA_DONGLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("Board Type  : SL_SILISA_DONGLE");
            break;

        case SL_YOGA_DONGLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("Board Type  : SL_YOGA_DONGLE");
            break;

        case SL_BORQS_EVT:
            _SAANKHYA_PHY_ANDROID_DEBUG("Board Type  : SL_BORQS_EVT");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("Board Type  : NA");
    }

    switch (cfgInfo.chipType)
    {
        case SL_CHIP_3000:
            _SAANKHYA_PHY_ANDROID_DEBUG("Chip Type: SL_CHIP_3000");
            break;

        case SL_CHIP_3010:
            _SAANKHYA_PHY_ANDROID_DEBUG("Chip Type: SL_CHIP_3010");
            break;

        case SL_CHIP_4000:
            _SAANKHYA_PHY_ANDROID_DEBUG("Chip Type: SL_CHIP_4000");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("Chip Type : NA");
    }

    if (cfgInfo.chipRev == SL_CHIP_REV_AA)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Chip Revision: SL_CHIP_REV_AA");
    }
    else if (cfgInfo.chipRev == SL_CHIP_REV_BB)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Chip Revision: SL_CHIP_REV_BB");
    }
    else
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Chip Revision: NA");
    }

    if (cfgInfo.tunerType == TUNER_NXP)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Type: TUNER_NXP");
    }
    else if (cfgInfo.tunerType == TUNER_SI)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Type: TUNER_SI");
    }
    else if (cfgInfo.tunerType == TUNER_SI_P)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Type: TUNER_SI_P");
    }
    else if(cfgInfo.tunerType == TUNER_SILABS)
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Type: TUNER_SILABS");
    }
    else
    {
        _SAANKHYA_PHY_ANDROID_DEBUG("Tuner Type: NA");
    }

    switch (cfgInfo.demodControlIf)
    {
        case SL_DEMOD_CMD_CONTROL_IF_I2C:
            _SAANKHYA_PHY_ANDROID_DEBUG("Command Interface: SL_DEMOD_CMD_CONTROL_IF_I2C");
            break;

        case SL_DEMOD_CMD_CONTROL_IF_SDIO:
            _SAANKHYA_PHY_ANDROID_DEBUG("Command Interface: SL_DEMOD_CMD_CONTROL_IF_SDIO");
            break;

        case SL_DEMOD_CMD_CONTROL_IF_SPI:
            _SAANKHYA_PHY_ANDROID_DEBUG("Command Interface: SL_DEMOD_CMD_CONTROL_IF_SPI");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("Command Interface: NA");
    }

    switch (cfgInfo.demodOutputIf)
    {
        case SL_DEMOD_OUTPUTIF_TS:
            _SAANKHYA_PHY_ANDROID_DEBUG("Output Interface: SL_DEMOD_OUTPUTIF_TS");
            break;

        case SL_DEMOD_OUTPUTIF_SDIO:
            _SAANKHYA_PHY_ANDROID_DEBUG("Output Interface: SL_DEMOD_OUTPUTIF_SDIO");
            break;

        case SL_DEMOD_OUTPUTIF_SPI:
            _SAANKHYA_PHY_ANDROID_DEBUG("Output Interface: SL_DEMOD_OUTPUTIF_SPI");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("Output Interface: NA");
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("Demod I2C Address: 0x%x", cfgInfo.demodI2cAddr);
}

/*
 * NOTE: jjustman-2021-01-19 - moved critical section mutex from outer wrapper to only CircularBufferPop critical section
 *          moved CS from processTLVFromCallback which was acquired for all of the TLV processing, which is not necessary
 *
 */

void SaankhyaPHYAndroid::processTLVFromCallback()
{
    int innerLoopCount = 0;
    int bytesRead = 0;

    unique_lock<mutex> CircularBufferMutex_local(CircularBufferMutex, std::defer_lock);
    //promoted from unique_lock, std::defer_lock to recursive_mutex: atsc3_sl_tlv_block_mutex

    //jjustman-2020-12-07 - loop while we have data in our cb, or break if cb_should_discard is true
    while(true && !SaankhyaPHYAndroid::cb_should_discard) {
        CircularBufferMutex_local.lock();
        bytesRead = CircularBufferPop(cb, TLV_CIRCULAR_BUFFER_PROCESS_BLOCK_SIZE, (char*)&processDataCircularBufferForCallback);
        CircularBufferMutex_local.unlock();

        //jjustman-2021-01-19 - if we don't get any data back, or the cb_should_discard flag is set, bail
        if(!bytesRead || SaankhyaPHYAndroid::cb_should_discard) {
            break; //we need to issue a matching sl_tlv_block_mutex ublock...
        }

        atsc3_sl_tlv_block_mutex.lock();
        if (!atsc3_sl_tlv_block) {
            _SAANKHYA_PHY_ANDROID_WARN("ERROR: atsc3NdkClientSlImpl::processTLVFromCallback - atsc3_sl_tlv_block is NULL!");
            allocate_atsc3_sl_tlv_block();
        }

        block_Write(atsc3_sl_tlv_block, (uint8_t *) &processDataCircularBufferForCallback, bytesRead);
        if (bytesRead < TLV_CIRCULAR_BUFFER_PROCESS_BLOCK_SIZE) {
            //_SAANKHYA_PHY_ANDROID_TRACE("atsc3NdkClientSlImpl::processTLVFromCallback() - short read from CircularBufferPop, got %d bytes, but expected: %d", bytesRead, TLV_CIRCULAR_BUFFER_PROCESS_BLOCK_SIZE);
            //release our recursive lock early as we are bailing this method
            atsc3_sl_tlv_block_mutex.unlock();
            break;
        }

        block_Rewind(atsc3_sl_tlv_block);

        //_SAANKHYA_PHY_ANDROID_DEBUG("atsc3NdkClientSlImpl::processTLVFromCallback() - processTLVFromCallbackInvocationCount: %d, inner loop count: %d, atsc3_sl_tlv_block.p_size: %d, atsc3_sl_tlv_block.i_pos: %d", processTLVFromCallbackInvocationCount, ++innerLoopCount, atsc3_sl_tlv_block->p_size, atsc3_sl_tlv_block->i_pos);

        bool atsc3_sl_tlv_payload_complete = false;

        do {
            atsc3_sl_tlv_payload = atsc3_sl_tlv_payload_parse_from_block_t(atsc3_sl_tlv_block);
            _SAANKHYA_PHY_ANDROID_TRACE("atsc3NdkClientSlImpl::processTLVFromCallback() - processTLVFromCallbackInvocationCount: %d, inner loop count: %d, atsc3_sl_tlv_block.p_size: %d, atsc3_sl_tlv_block.i_pos: %d, atsc3_sl_tlv_payload: %p",
                                        processTLVFromCallbackInvocationCount, ++innerLoopCount, atsc3_sl_tlv_block->p_size, atsc3_sl_tlv_block->i_pos, atsc3_sl_tlv_payload);

            if (atsc3_sl_tlv_payload) {
                atsc3_sl_tlv_payload_dump(atsc3_sl_tlv_payload);

                uint64_t l1dTimeNs_value = atsc3_sl_tlv_payload->l1d_time_sec + (atsc3_sl_tlv_payload->l1d_time_msec * 1000) + (atsc3_sl_tlv_payload->l1d_time_usec * 1000000) + (atsc3_sl_tlv_payload->l1d_time_nsec * 1000000000) ;

                //jjustman-2021-10-24 - hack-ish to push our l1d time info - still needed?
                //true ||
                if(last_l1dTimeNs_value != l1dTimeNs_value) {
                    _SAANKHYA_PHY_ANDROID_TRACE("atsc3NdkClientSlImpl::processTLVFromCallback() L1DTimeInfo is: L1time: flag: %d, s: %d, ms: %d, us: %d, ns: %d, current l1dTimeNs: %d, last_l1dTimeNs_value: %d, frame duration: %",
                                                last_l1bTimeInfoFlag,
                                                atsc3_sl_tlv_payload->l1d_time_sec, atsc3_sl_tlv_payload->l1d_time_msec, atsc3_sl_tlv_payload->l1d_time_usec, atsc3_sl_tlv_payload->l1d_time_nsec,
                                                l1dTimeNs_value,
                                                last_l1dTimeNs_value
                    );

                    if (atsc3_ndk_phy_bridge_get_instance()) {
                        atsc3_ndk_phy_bridge_get_instance()->atsc3_update_l1d_time_information(last_l1bTimeInfoFlag, atsc3_sl_tlv_payload->l1d_time_sec, atsc3_sl_tlv_payload->l1d_time_msec, atsc3_sl_tlv_payload->l1d_time_usec, atsc3_sl_tlv_payload->l1d_time_nsec);
                    }
                    last_l1dTimeNs_value = l1dTimeNs_value;
                }
                    

                if (atsc3_sl_tlv_payload->alp_payload_complete) {
                    atsc3_sl_tlv_payload_complete = true;

                    block_Rewind(atsc3_sl_tlv_payload->alp_payload);
                    atsc3_alp_packet_t *atsc3_alp_packet = atsc3_alp_packet_parse(atsc3_sl_tlv_payload->plp_number, atsc3_sl_tlv_payload->alp_payload);
                    if (atsc3_alp_packet) {
                        alp_completed_packets_parsed++;

                        alp_total_bytes += atsc3_alp_packet->alp_payload->p_size;

                        if (atsc3_alp_packet->alp_packet_header.packet_type == 0x00) {

                            block_Rewind(atsc3_alp_packet->alp_payload);
                            if (atsc3_phy_rx_udp_packet_process_callback) {
                                atsc3_phy_rx_udp_packet_process_callback(atsc3_sl_tlv_payload->plp_number, atsc3_alp_packet->alp_payload);
                            }

                        } else if (atsc3_alp_packet->alp_packet_header.packet_type == 0x4) {
                            alp_total_LMTs_recv++;
                            atsc3_link_mapping_table_t *atsc3_link_mapping_table_pending = atsc3_alp_packet_extract_lmt(atsc3_alp_packet);

                            if (atsc3_phy_rx_link_mapping_table_process_callback && atsc3_link_mapping_table_pending) {
                                atsc3_link_mapping_table_t *atsc3_link_mapping_table_to_free = atsc3_phy_rx_link_mapping_table_process_callback(atsc3_link_mapping_table_pending);

                                if (atsc3_link_mapping_table_to_free) {
                                    atsc3_link_mapping_table_free(&atsc3_link_mapping_table_to_free);
                                }
                            }
                        }

                        atsc3_alp_packet_free(&atsc3_alp_packet);
                    }

                    //free our atsc3_sl_tlv_payload
                    atsc3_sl_tlv_payload_free(&atsc3_sl_tlv_payload);
                    continue;

                } else {

                    atsc3_sl_tlv_payload_free(&atsc3_sl_tlv_payload);
                    atsc3_sl_tlv_payload_complete = false;
                    break; //jjustman-2021-05-04 - gross, i know...
                }
            }

            if(atsc3_sl_tlv_block->i_pos > (atsc3_sl_tlv_block->p_size - 188)) {
                atsc3_sl_tlv_payload_complete = false;
            } else {
                atsc3_sl_tlv_payload_complete = true;
            }
            //jjustman-2019-12-29 - noisy, TODO: wrap in __DEBUG macro check
            //_SAANKHYA_PHY_ANDROID_DEBUG("ERROR: alp_payload IS NULL, short TLV read?  at atsc3_sl_tlv_block: i_pos: %d, p_size: %d", atsc3_sl_tlv_block->i_pos, atsc3_sl_tlv_block->p_size);

        } while (atsc3_sl_tlv_payload_complete);

        if (atsc3_sl_tlv_payload && !atsc3_sl_tlv_payload->alp_payload_complete && atsc3_sl_tlv_block->i_pos > atsc3_sl_tlv_payload->sl_tlv_total_parsed_payload_size) {
            //accumulate from our last starting point and continue iterating during next bbp
            atsc3_sl_tlv_block->i_pos -= atsc3_sl_tlv_payload->sl_tlv_total_parsed_payload_size;
            //free our atsc3_sl_tlv_payload
            atsc3_sl_tlv_payload_free(&atsc3_sl_tlv_payload);
        }

        if (atsc3_sl_tlv_block->p_size > atsc3_sl_tlv_block->i_pos) {
            //truncate our current block_t starting at i_pos, and then read next i/o block
            block_t *temp_sl_tlv_block = block_Duplicate_from_position(atsc3_sl_tlv_block);
            block_Destroy(&atsc3_sl_tlv_block);
            atsc3_sl_tlv_block = temp_sl_tlv_block;
            block_Seek(atsc3_sl_tlv_block, atsc3_sl_tlv_block->p_size);
        } else if (atsc3_sl_tlv_block->p_size == atsc3_sl_tlv_block->i_pos) {
            //clear out our tlv block as we are the "exact" size of our last alp packet

            //jjustman-2020-10-30 - TODO: this can be optimized out without a re-alloc
            block_Destroy(&atsc3_sl_tlv_block);
            atsc3_sl_tlv_block = block_Alloc(TLV_CIRCULAR_BUFFER_PROCESS_BLOCK_SIZE);
        } else {
            _SAANKHYA_PHY_ANDROID_WARN("atsc3_sl_tlv_block: positioning mismatch: i_pos: %d, p_size: %d - rewinding and seeking for magic packet?", atsc3_sl_tlv_block->i_pos, atsc3_sl_tlv_block->p_size);

            //jjustman: 2019-11-23: rewind in order to try seek for our magic packet in the next loop here
            block_Rewind(atsc3_sl_tlv_block);
        }
        //unlock for our inner loop
        atsc3_sl_tlv_block_mutex.unlock();
    }
    //atsc3_sl_tlv_block_mutex is now unlocked from either bytesRead < TLV_CIRCULAR_BUFFER_PROCESS_BLOCK_SIZE _or_ 2 lines above...
}

void SaankhyaPHYAndroid::printToConsoleDemodConfiguration(SL_DemodConfigInfo_t cfgInfo)
{
    _SAANKHYA_PHY_ANDROID_DEBUG(" SL Demod Configuration");
    switch (cfgInfo.std)
    {
        case SL_DEMODSTD_ATSC3_0:
            _SAANKHYA_PHY_ANDROID_DEBUG("Standard: ATSC3_0");
            break;

        case SL_DEMODSTD_ATSC1_0:
            _SAANKHYA_PHY_ANDROID_DEBUG("Demod Standard  : ATSC1_0");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("Demod Standard  : NA");
    }

//    _SAANKHYA_PHY_ANDROID_DEBUG("PLP Configuration");
//    _SAANKHYA_PHY_ANDROID_DEBUG("  PLP0: %d", (signed char)cfgInfo.atsc3ConfigParams.plpConfig.plp0);
//    _SAANKHYA_PHY_ANDROID_DEBUG("  PLP1: %d", (signed char)cfgInfo.atsc3ConfigParams.plpConfig.plp1);
//    _SAANKHYA_PHY_ANDROID_DEBUG("  PLP2: %d", (signed char)cfgInfo.atsc3ConfigParams.plpConfig.plp2);
//    _SAANKHYA_PHY_ANDROID_DEBUG("  PLP3: %d", (signed char)cfgInfo.atsc3ConfigParams.plpConfig.plp3);

    _SAANKHYA_PHY_ANDROID_DEBUG("Input Configuration");
    switch (cfgInfo.afeIfInfo.iftype)
    {
        case SL_IFTYPE_ZIF:
            _SAANKHYA_PHY_ANDROID_DEBUG("  IF Type: ZIF");
            break;

        case SL_IFTYPE_LIF:
            _SAANKHYA_PHY_ANDROID_DEBUG("  IF Type: LIF");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("  IF Type: NA");
    }

    switch (cfgInfo.afeIfInfo.iqswap)
    {
        case SL_IQSWAP_DISABLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("  IQSWAP : DISABLE");
            break;

        case SL_IQSWAP_ENABLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("  IQSWAP : ENABLE");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("  IQSWAP : NA");
    }

    switch (cfgInfo.afeIfInfo.iswap)
    {
        case SL_IPOL_SWAP_DISABLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("  ISWAP  : DISABLE");
            break;

        case SL_IPOL_SWAP_ENABLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("  ISWAP  : ENABLE");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("  ISWAP  : NA");
    }

    switch (cfgInfo.afeIfInfo.qswap)
    {
        case SL_QPOL_SWAP_DISABLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("  QSWAP  : DISABLE");
            break;

        case SL_QPOL_SWAP_ENABLE:
            _SAANKHYA_PHY_ANDROID_DEBUG("  QSWAP  : ENABLE");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("  QSWAP  : NA");
    }

    _SAANKHYA_PHY_ANDROID_DEBUG("  ICoeff1: %3.4f", cfgInfo.iqOffCorInfo.iCoeff1);
    _SAANKHYA_PHY_ANDROID_DEBUG("  QCoeff1: %3.4f", cfgInfo.iqOffCorInfo.qCoeff1);
    _SAANKHYA_PHY_ANDROID_DEBUG("  ICoeff2: %3.4f", cfgInfo.iqOffCorInfo.iCoeff2);
    _SAANKHYA_PHY_ANDROID_DEBUG("  QCoeff2: %3.4f", cfgInfo.iqOffCorInfo.qCoeff2);

    _SAANKHYA_PHY_ANDROID_DEBUG("  AGCReference  : %d mv", cfgInfo.afeIfInfo.agcRefValue);
    _SAANKHYA_PHY_ANDROID_DEBUG("  Tuner IF Frequency: %3.2f MHz", cfgInfo.afeIfInfo.ifreq);

    _SAANKHYA_PHY_ANDROID_DEBUG("Output Configuration");
    switch (cfgInfo.oifInfo.oif)
    {
        case SL_OUTPUTIF_TSPARALLEL_LSB_FIRST:
            _SAANKHYA_PHY_ANDROID_DEBUG("  OutputInteface: TS PARALLEL LSB FIRST");
            break;

        case SL_OUTPUTIF_TSPARALLEL_MSB_FIRST:
            _SAANKHYA_PHY_ANDROID_DEBUG("  OutputInteface: TS PARALLEL MSB FIRST");
            break;

        case SL_OUTPUTIF_TSSERIAL_LSB_FIRST:
            _SAANKHYA_PHY_ANDROID_DEBUG("  OutputInteface: TS SERAIL LSB FIRST");
            break;

        case SL_OUTPUTIF_TSSERIAL_MSB_FIRST:
            _SAANKHYA_PHY_ANDROID_DEBUG("  OutputInteface: TS SERIAL MSB FIRST");
            break;

        case SL_OUTPUTIF_SDIO:
            _SAANKHYA_PHY_ANDROID_DEBUG("  OutputInteface: SDIO");
            break;

        case SL_OUTPUTIF_SPI:
            _SAANKHYA_PHY_ANDROID_DEBUG("  OutputInteface: SPI");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("  OutputInteface: NA");
    }

    switch (cfgInfo.oifInfo.TsoClockInvEnable)
    {
        case SL_TSO_CLK_INV_OFF:
            _SAANKHYA_PHY_ANDROID_DEBUG("  TS Out Clock Inv: DISABLED");
            break;

        case SL_TSO_CLK_INV_ON:
            _SAANKHYA_PHY_ANDROID_DEBUG("  TS Out Clock Inv: ENABLED");
            break;

        default:
            _SAANKHYA_PHY_ANDROID_DEBUG("   TS Out Clock Inv: NA");
    }
}

void SaankhyaPHYAndroid::printToConsoleDemodError(const char* methodName, SL_Result_t err)
{
    switch (err)
    {
        case SL_ERR_OPERATION_FAILED:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Operation Failed", err);
            break;

        case SL_ERR_TOO_MANY_INSTANCES:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Too Many Instances", err);
            break;

        case SL_ERR_CODE_DWNLD:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Code download Failed", err);
            break;

        case SL_ERR_INVALID_ARGUMENTS:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Invalid Argument", err);
            break;

        case SL_ERR_CMD_IF_FAILURE:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Command Interface Failure", err);
            break;

        case SL_ERR_NOT_SUPPORTED:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Not Supported", err);
            break;

        default:
            _SAANKHYA_PHY_ANDROID_ERROR_NOTIFY_BRIDGE_INSTANCE(methodName, "Sl Other Error", err);
            break;
    }
}

int SaankhyaPHYAndroid::processThread()
{
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::processThread: starting with this: %p", this);
    this->pinConsumerThreadAsNeeded();
    this->processThreadIsRunning = true;

    this->resetProcessThreadStatistics();

    while (this->processThreadShouldRun)
    {
        //_SAANKHYA_PHY_ANDROID_DEBUG("atsc3NdkClientSlImpl::ProcessThread: getDataSize is: %d", CircularBufferGetDataSize(cb));

        //unique_lock<mutex> CircularBufferMutex_local(CircularBufferMutex);

        while(CircularBufferGetDataSize(this->cb) >= TLV_CIRCULAR_BUFFER_MIN_PROCESS_SIZE) {
            processTLVFromCallbackInvocationCount++;
            this->processTLVFromCallback();
        }
       //CircularBufferMutex_local.unlock();

       //jjustman - try increasing to 50ms? shortest atsc3 subframe?
        usleep(33000); //jjustman-2022-02-16 - peg us at 16.67ms/2 ~ 8ms
        // pegs us at ~ 30 spinlocks/sec if no data
    }

    this->releasePinnedConsumerThreadAsNeeded();
    this->processThreadIsRunning = false;

    _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::ProcessThread complete");

    return 0;
}

int SaankhyaPHYAndroid::captureThread()
{
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::captureThread: starting with this: %p", this);
    this->pinProducerThreadAsNeeded();
    this->captureThreadIsRunning = true;

    SL_RxDataStart((RxDataCB)&SaankhyaPHYAndroid::RxDataCallback);

    this->releasePinnedProducerThreadAsNeeded();
    this->captureThreadIsRunning = false;

    _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::CaptureThread complete");

    return 0;
}

int SaankhyaPHYAndroid::statusThread()
{
    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::statusThread: starting with this: %p", this);

    this->pinStatusThreadAsNeeded();
    this->statusThreadIsRunning = true;

    unique_lock<mutex> SL_I2C_command_mutex_tuner_status_io(this->SL_I2C_command_mutex, std::defer_lock);
    unique_lock<mutex> SL_PlpConfigParams_mutex_get_plps(SL_PlpConfigParams_mutex, std::defer_lock);

    SL_Result_t dres = SL_OK;
    SL_Result_t sl_res = SL_OK;
    SL_TunerResult_t tres = SL_TUNER_OK;

    uint lastCpuStatus = 0;

    SL_Atsc3p0PlpConfigParams_t     loop_atsc3ConfigParams = { 0 };
    unsigned long long              llsPlpInfo;

    /* jjustman-2021-06-07 - #11798
     *  int L1bSnrLinearScale;
        int L1dSnrLinearScale;
        int Plp0SnrLinearScale;
        int Plp1SnrLinearScale;
        int Plp2SnrLinearScale;
        int Plp3SnrLinearScale;
        int GlobalSnrLinearScale;
     */
    double snr_global;
    double snr_l1b;
    double snr_l1d;
    double snr_plp[4];

    int ber_l1b;
    int ber_l1d;
    int ber_plp0;

    SL_Atsc3p0L1DPlp_Diag_t myPlps[4];

    atsc3_ndk_phy_client_rf_metrics_t atsc3_ndk_phy_client_rf_metrics = { '0' };

    //wait for demod to come up before polling status
    while(!this->demodStartStatus && this->statusThreadShouldRun) {
        usleep(1000000);
    }

//#define
    while(this->statusThreadShouldRun) {

        //running
        if(lastCpuStatus == 0xFFFFFFFF) {

#if defined(__JJ_DEBUG_BSR_EA_WAKEUP) || defined(__JJ_DEBUG_L1D_TIMEINFO)

            int bsrLoopCount = 0;
            int l1dLoopCount = 0;

            while(true) {

#ifdef __JJ_DEBUG_BSR_EA_WAKEUP
                dres = SL_DemodGetAtsc3p0Diagnostics(this->slUnit, SL_DEMOD_DIAG_TYPE_BSR, (SL_Atsc3p0Bsr_Diag_t*)&bsrDiag);
                if (dres != SL_OK) {
                    _SAANKHYA_PHY_ANDROID_ERROR("SaankhyaPHYAndroid::StatusThread: Error: SL_DemodGetAtsc3p0Diagnostics with SL_DEMOD_DIAG_TYPE_BSR failed, res: %d", dres);
                } else {
                    _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::StatusThread: BSR: Bsr1EAWakeup1: %d, Bsr1EAWakeup2: %d", bsrDiag.Bsr1EAWakeup1, bsrDiag.Bsr1EAWakeup2);
                    //jjustman-2021-09-01 - push to phy bridge
                }

                usleep(__JJ_DEBUG_BSR_EA_WAKEUP_USLEEP);

                if(bsrLoopCount++ > __JJ_DEBUG_BSR_EA_WAKEUP_ITERATIONS) {
                    break;
                }
#endif

#ifdef __JJ_DEBUG_L1D_TIMEINFO
                //optional gate check:
                //demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_L1D_LOCK
                dres = SL_DemodGetAtsc3p0Diagnostics(slUnit, SL_DEMOD_DIAG_TYPE_L1B, (SL_Atsc3p0L1B_Diag_t*)&l1bDiag);
                dres = SL_DemodGetAtsc3p0Diagnostics(slUnit, SL_DEMOD_DIAG_TYPE_L1D, (SL_Atsc3p0L1D_Diag_t*)&l1dDiag);
                printToConsoleAtsc3L1dDiagnostics(l1dDiag);

                _SAANKHYA_PHY_ANDROID_DEBUG("SaankhyaPHYAndroid::StatusThread: L1time: flag: %d, s: %d, ms: %d, us: %d, ns: %d",
                                            l1bDiag.L1bTimeInfoFlag, l1dDiag.l1dGlobalParamsStr.L1dTimeSec, l1dDiag.l1dGlobalParamsStr.L1dTimeMsec, l1dDiag.l1dGlobalParamsStr.L1dTimeUsec, l1dDiag.l1dGlobalParamsStr.L1dTimeNsec);

                if(atsc3_ndk_phy_bridge_get_instance()) {
                    atsc3_ndk_phy_bridge_get_instance()->atsc3_update_l1d_time_information( l1bDiag.L1bTimeInfoFlag, l1dDiag.l1dGlobalParamsStr.L1dTimeSec, l1dDiag.l1dGlobalParamsStr.L1dTimeMsec, l1dDiag.l1dGlobalParamsStr.L1dTimeUsec, l1dDiag.l1dGlobalParamsStr.L1dTimeNsec);
                }

                usleep(__JJ_DEBUG_L1D_TIMEINFO_USLEEP);

                if(l1dLoopCount++ > __JJ_DEBUG_L1D_TIMEINFO_DEMOD_GET_ITERATIONS) {
                    break;
                }

#endif

#ifdef __JJ_DEBUG_L1D_TIMEINFO_CYCLE_STOP_START_DEMOD
    slres = SL_DemodStop(slUnit);
    _SAANKHYA_PHY_ANDROID_DEBUG("after SL_DemodStop, count: %d, slRes: %d", l1dLoopCount);
    usleep(__JJ_DEBUG_L1D_TIMEINFO_USLEEP);
    slres = SL_DemodStart(slUnit);
    _SAANKHYA_PHY_ANDROID_DEBUG("after SL_DemodStart, count: %d, slRes: %d", l1dLoopCount);
    usleep(__JJ_DEBUG_L1D_TIMEINFO_USLEEP);
#endif
            }
#else

            //2022-03-30 - updated to 10s...hack testing for 256QAM 11/15
            usleep(1000000);
            //jjustman: target: sleep for 500ms
            //TODO: jjustman-2019-12-05: investigate FX3 firmware and i2c single threaded interrupt handling instead of dma xfer
#endif

        } else {
            //halted
            usleep(5000000);
        }
        lastCpuStatus = 0;

        //bail early if we should shutdown
        if(!this->statusThreadShouldRun) {
            break;
        }

        //jjustman-2020-10-14 - try to make this loop as small as possible to not upset the SDIO I/F ALP buffer window
        /*
         * jjustman-2021-03-16 - to restart loop, be sure to use:
         *          goto sl_i2c_tuner_mutex_unlock;
         * rather than continue; as we need to release this mutex for other threads to (possibly) access i2c
         */

        SL_I2C_command_mutex_tuner_status_io.lock();

        //PLP info we will use for this stats iteration
        SL_PlpConfigParams_mutex_get_plps.lock();
        loop_atsc3ConfigParams.plp0 = atsc3ConfigParams.plpConfig.plp0;
        loop_atsc3ConfigParams.plp1 = atsc3ConfigParams.plpConfig.plp1;
        loop_atsc3ConfigParams.plp2 = atsc3ConfigParams.plpConfig.plp2;
        loop_atsc3ConfigParams.plp3 = atsc3ConfigParams.plpConfig.plp3;
        SL_PlpConfigParams_mutex_get_plps.unlock();

        //if this is our first loop after a Tune() command has completed, dump SL_DemodGetConfiguration
        if(statusThreadFirstLoopAfterTuneComplete) {
            SL_SleepMS(250); // Delay to accomdate set configurations at SL to take effect
            statusThreadFirstLoopAfterTuneComplete = false;

            slres = SL_DemodGetConfiguration(slUnit, &cfgInfo);
            if (slres != SL_OK)
            {
                printToConsoleDemodError("SL_TunerGetStatus", slres);
                if (slres == SL_ERR_CMD_IF_FAILURE)
                {
                    handleCmdIfFailure();
                    goto sl_i2c_tuner_mutex_unlock;
                }
            }
            else
            {
                printToConsoleDemodConfiguration(cfgInfo);
            }
        }

        /*jjustman-2020-01-06: For the SL3000/SiTune, we will have 3 status attributes with the following possible values:
                cpuStatus:          (cpuStatus == 0xFFFFFFFF) ? "RUNNING" : "HALTED",

                tunerInfo.status:   SL_TUNER_STATUS_NOT_LOCKED (0)
                                    SL_TUNER_STATUS_LOCKED (1)

                demodLockStatus:   Updated as of SLAPI-0.14:

                        This data type represents the signal lock status of the SL demodulator.

                        Bit Number  Value   Description         Details
                        ----------  -----   -----------         ---------------
                        0           0       RF UnLocked         -
                                    1       RF Locked           RF LOCKED: Bootstrap Information decoded and available

                        1           0       L1B UnLocked        -
                                    1       L1B Locked          L1B LOCKED: L1B information available

                        2           0       L1D UnLocked        -
                                    1       L1D Locked          L1D LOCKED: L1D related information available

                        3           Reserved

                        4           0       BB PLP0 Not Locked  -
                                    1       BB PLP0 Locked      BB PLP0 Locked: PLP0 ALP Data coming out of SLDemod

                        5           0       BB PLP1 Not Locked  -
                                    1       BB PLP1 Locked      BB PLP1 Locked: PLP1 ALP Data coming out of SLDemod

                        6           0       BB PLP2 Not Locked  -
                                    1       BB PLP2 Locked      BB PLP2 Locked: PLP2 ALP Data coming out of SLDemod

                        7           0       BB PLP3 Not Locked  -
                                    1       BB PLP3 Locked      BB PLP3 Locked: PLP3 ALP Data coming out of SLDemod

                        8-31        Reserved

        */

        dres = SL_DemodGetStatus(this->slUnit, SL_DEMOD_STATUS_TYPE_CPU, (int*)&cpuStatus);
        if (dres != SL_OK) {
            _SAANKHYA_PHY_ANDROID_ERROR("Error:SL_Demod Get CPU Status: dres: %d", dres);
            goto sl_i2c_tuner_mutex_unlock;
        }
        lastCpuStatus = cpuStatus;
        //jjustman-2021-05-11 - give 256qam 11/15 fec bitrates a chance to flush ALP buffer without oveflowing and lose bootstrap/l1b/l1d lock

#ifdef _JJ_I2C_TUNER_STATUS_THREAD_SLEEP_MS_ENABLED_
        SL_SleepMS(10);
#endif


        //jjustman-2020-10-14 - not really worth it on AA as we don't get rssi here
        tres = SL_TunerGetStatus(this->tUnit, &tunerInfo);
        if (tres != SL_TUNER_OK) {
            _SAANKHYA_PHY_ANDROID_ERROR("Error:SL_TunerGetStatus: tres: %d", tres);
            goto sl_i2c_tuner_mutex_unlock;
        }

#ifdef _JJ_I2C_TUNER_STATUS_THREAD_SLEEP_MS_ENABLED_
        SL_SleepMS(10);
#endif

		atsc3_ndk_phy_client_rf_metrics.freq_tune_khz = Last_tune_freq;
		//jjustman-2022-06-08 - fixme for std/channel bw
		atsc3_ndk_phy_client_rf_metrics.atsc_std = (demodStandard == SL_DEMODSTD_ATSC3_0 || demodStandard == SL_DEMODSTD_ATSC3_0) ? 3 : 1;
		atsc3_ndk_phy_client_rf_metrics.channel_bw = (tunerGetCfg.std == SL_TUNERSTD_ATSC3_0) ? 8000 : 6000;


        atsc3_ndk_phy_client_rf_metrics.tuner_lock = (tunerInfo.status == 1);

        //jjustman-2022-03-30 - this call may cause the demod to hang with 256QAM 11/15
        //important, we should only query BSR, L1B, and L1D Diag data after each relevant lock has been acquired to prevent i2c bus txns from crashing the demod...
        dres = SL_DemodGetStatus(this->slUnit, SL_DEMOD_STATUS_TYPE_LOCK, (SL_DemodLockStatus_t*)&demodLockStatus);
        if (dres != SL_OK) {
            _SAANKHYA_PHY_ANDROID_ERROR("Error:SL_Demod Get Lock Status  : dres: %d", dres);
            goto sl_i2c_tuner_mutex_unlock;
        }

#ifdef _JJ_I2C_TUNER_STATUS_THREAD_SLEEP_MS_ENABLED_
SL_SleepMS(10);
#endif

        atsc3_ndk_phy_client_rf_metrics.demod_lock = demodLockStatus;

        atsc3_ndk_phy_client_rf_metrics.plp_lock_any =  (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP0_LOCK) ||
                                                        (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP1_LOCK) ||
                                                        (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP2_LOCK) ||
                                                        (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP3_LOCK);


        atsc3_ndk_phy_client_rf_metrics.plp_lock_all =  (loop_atsc3ConfigParams.plp0 != 0xFF && (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP0_LOCK)) &&
                                                        (loop_atsc3ConfigParams.plp1 != 0xFF && (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP1_LOCK)) &&
                                                        (loop_atsc3ConfigParams.plp2 != 0xFF && (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP2_LOCK)) &&
                                                        (loop_atsc3ConfigParams.plp3 != 0xFF && (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP3_LOCK));

        //we have RF / Bootstrap lock
        if(demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_RF_LOCK) {
            if(!statusThreadFirstLoopAfterTuneComplete_HasBootstrapLock_for_BSR_Diag) {
                statusThreadFirstLoopAfterTuneComplete_HasBootstrapLock_for_BSR_Diag = false;

                dres = SL_DemodGetDiagnostics(this->slUnit, SL_DEMOD_DIAG_TYPE_ATSC3P0_BSR, (SL_Atsc3p0Bsr_Diag_t*)&bsrDiag);
                if (dres != SL_OK) {
                    _SAANKHYA_PHY_ANDROID_ERROR("Error: SL_DemodGetDiagnostics with SL_DEMOD_DIAG_TYPE_BSR failed, res: %d", dres);
                    goto sl_i2c_tuner_mutex_unlock;
                }
                _SAANKHYA_PHY_ANDROID_ERROR("bsr diag: Bsr1SysBw: %d", bsrDiag.Bsr1SysBw);
                //printAtsc3BsrDiagnostics(bsrDiag, 0);
            }
        }

        //we have L1B_Lock
        if(demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_L1B_LOCK) {

            if(!statusThreadFirstLoopAfterTuneComplete_HasL1B_DemodLock_for_L1B_Diag) {
                statusThreadFirstLoopAfterTuneComplete_HasL1B_DemodLock_for_L1B_Diag = true;

                dres = SL_DemodGetDiagnostics(this->slUnit, SL_DEMOD_DIAG_TYPE_ATSC3P0_L1B, (SL_Atsc3p0L1B_Diag_t*)&l1bDiag);
                if (dres != SL_OK) {
                    _SAANKHYA_PHY_ANDROID_ERROR("Error: SL_DemodGetDiagnostics with SL_DEMOD_DIAG_TYPE_L1B failed, res: %d", dres);
                    goto sl_i2c_tuner_mutex_unlock;
                }

                //printAtsc3L1bDiagnostics(l1bDiag, 0);

                //jjustman-2021-10-24 - keep track of our L1bTimeInfoFlag
                last_l1bTimeInfoFlag = l1bDiag.L1bTimeInfoFlag;
            }
        }

        //we have L1D_Lock
        if(demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_L1D_LOCK) {
            if(!statusThreadFirstLoopAfterTuneComplete_HasL1D_DemodLock_for_L1D_Diag) {
                statusThreadFirstLoopAfterTuneComplete_HasL1D_DemodLock_for_L1D_Diag = true;

                dres = SL_DemodGetDiagnostics(this->slUnit, SL_DEMOD_DIAG_TYPE_ATSC3P0_L1D, (SL_Atsc3p0L1D_Diag_t*)&l1dDiag);
                if (dres != SL_OK) {
                    _SAANKHYA_PHY_ANDROID_ERROR("Error: SL_DemodGetAtsc3p0Diagnostics with SL_DEMOD_DIAG_TYPE_L1D failed, res: %d", dres);
                    goto sl_i2c_tuner_mutex_unlock;
                }

                //printAtsc3L1dDiagnostics(l1bDiag.L1bNoOfSubframes, l1dDiag, 0);
                //printAtsc3SignalDetails(l1bDiag.L1bNoOfSubframes, l1dDiag, 0);
            }
        }

//#define _JJ_DISABLE_PLP_SNR
        //we need this for SNR
#ifndef _JJ_DISABLE_PLP_SNR
        dres = SL_DemodGetDiagnostics(this->slUnit, SL_DEMOD_DIAG_TYPE_ATSC3P0_PERF, (SL_Atsc3p0Perf_Diag_t*)&perfDiag);
        if (dres != SL_OK) {
            _SAANKHYA_PHY_ANDROID_ERROR("Error getting ATSC3.0 Performance Diagnostics : dres: %d", dres);
            goto sl_i2c_tuner_mutex_unlock;
        }
#endif
        //jjustman-2021-05-11 - TODO: only run this at PLP selection lock, e.g.:
        //  for each PLPne.g. demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_BB_PLPn_LOCK) != 0;
        //
        //        sl_res = SL_DemodGetLlsPlpList(this->slUnit, &llsPlpInfo);
        //        if (sl_res != SL_OK) {
        //            _SAANKHYA_PHY_ANDROID_ERROR("Error:SL_DemodGetLlsPlpList : sl_res: %d", sl_res);
        //            goto sl_i2c_tuner_mutex_unlock;
        //        }

        //jjustman-2021-03-16 - exit our i2c critical section while we build and push our PHY statistics, we can use "continue" for next loop iteration after this point
        SL_I2C_command_mutex_tuner_status_io.unlock();

        //jjustman-2021-06-08 - for debugging purposes only
#ifdef _JJ_TUNER_STATUS_THREAD_PRINT_PERF_DIAGNOSTICS_ENABLED_
printAtsc3PerfDiagnostics(perfDiag, 0);
#endif


        atsc3_ndk_phy_client_rf_metrics.cpu_status = (cpuStatus == 0xFFFFFFFF); //0xFFFFFFFF -> running -> 1 to jni layer
        snr_global = compute_snr(perfDiag.GlobalSnrLinearScale);
        atsc3_ndk_phy_client_rf_metrics.snr1000_global = snr_global;

        snr_l1b = compute_snr(perfDiag.L1bSnrLinearScale);
        atsc3_ndk_phy_client_rf_metrics.snr1000_l1b = snr_l1b;

        snr_l1d = compute_snr(perfDiag.L1dSnrLinearScale);
        atsc3_ndk_phy_client_rf_metrics.snr1000_l1d = snr_l1d;

        snr_plp[0] = compute_snr(perfDiag.Plp0SnrLinearScale);
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].snr1000 = snr_plp[0];

        snr_plp[1] = compute_snr(perfDiag.Plp1SnrLinearScale);
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].snr1000 = snr_plp[1];

        snr_plp[2] = compute_snr(perfDiag.Plp2SnrLinearScale);
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].snr1000 = snr_plp[2];

        snr_plp[3] = compute_snr(perfDiag.Plp3SnrLinearScale);
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].snr1000 = snr_plp[3];

        //sanjay
        if (sl_silisa_rssi == true) {
            atsc3_ndk_phy_client_rf_metrics.rssi_1000 = ((tunerInfo.signalStrength * 1000) -
                                                         (256000));
        } else {
            atsc3_ndk_phy_client_rf_metrics.rssi_1000 = tunerInfo.signalStrength * 1000;
        }

        //jjustman-2021-05-11 - fixme to just be perfDiag values
        ber_l1b = perfDiag.NumBitErrL1b; //(float)perfDiag.NumBitErrL1b / perfDiag.NumFecBitsL1b; // //aBerPreLdpcE7,
        ber_l1d = perfDiag.NumBitErrL1b; //(float) perfDiag.NumBitErrL1d / perfDiag.NumFecBitsL1d;//aBerPreBchE9,
        ber_plp0 = perfDiag.NumBitErrPlp0;// (float)perfDiag.NumBitErrPlp0 / perfDiag.NumFecBitsPlp0; //aFerPostBchE6,

        //build our listen plp details

        memset(&myPlps[0], 0, sizeof(SL_Atsc3p0L1DPlp_Diag_t));
        memset(&myPlps[1], 0, sizeof(SL_Atsc3p0L1DPlp_Diag_t));
        memset(&myPlps[2], 0, sizeof(SL_Atsc3p0L1DPlp_Diag_t));
        memset(&myPlps[3], 0, sizeof(SL_Atsc3p0L1DPlp_Diag_t));

//L1dSfNumPlp2Decode
        for(int subframeIdx = 0; subframeIdx <= l1bDiag.L1bNoOfSubframes; subframeIdx++) {
            for(int plpIdx = 0; plpIdx < (0xFF & l1dDiag.sfParams[subframeIdx].L1dSfNumPlp2Decode); plpIdx++) {

                if(loop_atsc3ConfigParams.plp0 == l1dDiag.sfParams[subframeIdx].PlpParams[plpIdx].L1dSfPlpId) {
                    myPlps[0] = l1dDiag.sfParams[subframeIdx].PlpParams[plpIdx];
                } else if(loop_atsc3ConfigParams.plp1 == l1dDiag.sfParams[subframeIdx].PlpParams[plpIdx].L1dSfPlpId) {
                    myPlps[1] = l1dDiag.sfParams[subframeIdx].PlpParams[plpIdx];
                } else if(loop_atsc3ConfigParams.plp2 == l1dDiag.sfParams[subframeIdx].PlpParams[plpIdx].L1dSfPlpId) {
                    myPlps[2] = l1dDiag.sfParams[subframeIdx].PlpParams[plpIdx];
                } else if(loop_atsc3ConfigParams.plp3 == l1dDiag.sfParams[subframeIdx].PlpParams[plpIdx].L1dSfPlpId) {
                    myPlps[3] = l1dDiag.sfParams[subframeIdx].PlpParams[plpIdx];
                }
            }
        }

        //plp[0]
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].plp_id          = loop_atsc3ConfigParams.plp0;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].modcod_valid    = (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP0_LOCK) != 0;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].plp_fec_type    = myPlps[0].L1dSfPlpFecType;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].plp_mod         = myPlps[0].L1dSfPlpModType;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].plp_cod         = myPlps[0].L1dSfPlpCoderate;

        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].ber_pre_ldpc    = perfDiag.LdpcItrnsPlp0; // over ???//BER x1e7
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].ber_pre_bch     = perfDiag.NumBitErrPlp0; //(perfDiag.NumBitErrPlp0 * 1000000000) / (perfDiag.Plp0StreamByteCount * 8); //s_fe_detail.aBerPreBchE9[i]; //BER 1xe9
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].fer_post_bch    = perfDiag.NumFecBitsPlp0; //(perfDiag.NumFrameErrPlp0 * 1000000) / perfDiag.NumFecFramePlp0;  //FER 1xe6
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].total_fec       = perfDiag.NumFecFramePlp0;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].total_error_fec = perfDiag.NumFrameErrPlp0;

        //plp[1]
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].plp_id          = loop_atsc3ConfigParams.plp1;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].modcod_valid    = (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP1_LOCK) != 0;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].plp_fec_type    = myPlps[1].L1dSfPlpFecType;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].plp_mod         = myPlps[1].L1dSfPlpModType;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].plp_cod         = myPlps[1].L1dSfPlpCoderate;

        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].ber_pre_ldpc    = perfDiag.LdpcItrnsPlp1; // over ???//BER x1e7
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].ber_pre_bch     = perfDiag.NumBitErrPlp1; //(perfDiag.NumBitErrPlp1 * 1000000000) / (perfDiag.Plp1StreamByteCount * 8); //s_fe_detail.aBerPreBchE9[i]; //BER 1xe9
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].fer_post_bch    = perfDiag.NumFecBitsPlp1; //(perfDiag.NumFrameErrPlp1 * 1000000) / perfDiag.NumFecFramePlp1;  //FER 1xe6
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].total_fec       = perfDiag.NumFecFramePlp1;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].total_error_fec = perfDiag.NumFrameErrPlp1;
        //plp[2]
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].plp_id          = loop_atsc3ConfigParams.plp2;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].modcod_valid    = (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP2_LOCK) != 0;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].plp_fec_type    = myPlps[2].L1dSfPlpFecType;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].plp_mod         = myPlps[2].L1dSfPlpModType;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].plp_cod         = myPlps[2].L1dSfPlpCoderate;

        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].ber_pre_ldpc    = perfDiag.LdpcItrnsPlp2; // over ???//BER x1e7
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].ber_pre_bch     = perfDiag.NumBitErrPlp2; //(perfDiag.NumBitErrPlp2 * 1000000000) / (perfDiag.Plp2StreamByteCount * 8); //s_fe_detail.aBerPreBchE9[i]; //BER 1xe9
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].fer_post_bch    = perfDiag.NumFecBitsPlp2; //(perfDiag.NumFrameErrPlp2 * 1000000) / perfDiag.NumFecFramePlp2;  //FER 1xe6
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].total_fec       = perfDiag.NumFecFramePlp2;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].total_error_fec = perfDiag.NumFrameErrPlp2;

        //plp[3]
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].plp_id          = loop_atsc3ConfigParams.plp3;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].modcod_valid    = (demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_BB_PLP3_LOCK) != 0;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].plp_fec_type    = myPlps[3].L1dSfPlpFecType;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].plp_mod         = myPlps[3].L1dSfPlpModType;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].plp_cod         = myPlps[3].L1dSfPlpCoderate;

        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].ber_pre_ldpc    = perfDiag.LdpcItrnsPlp3; // over ???//BER x1e7
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].ber_pre_bch     = perfDiag.NumBitErrPlp3; //(perfDiag.NumBitErrPlp3 * 1000000000) / (perfDiag.Plp3StreamByteCount * 8); //s_fe_detail.aBerPreBchE9[i]; //BER 1xe9
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].fer_post_bch    = perfDiag.NumFecBitsPlp3; //(perfDiag.NumFrameErrPlp3 * 1000000) / perfDiag.NumFecFramePlp3;  //FER 1xe6
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].total_fec       = perfDiag.NumFecFramePlp3;
        atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].total_error_fec = perfDiag.NumFrameErrPlp3;


        _SAANKHYA_PHY_ANDROID_DEBUG("atsc3NdkClientSlImpl::StatusThread: global_SNR: %f, l1b_SNR: %f, l1d_SNR: %f tunerInfo.status: %d, tunerInfo.signalStrength: %f, cpuStatus: %s, demodLockStatus: %d (RF: %d, L1B: %d, L1D: %d),  ber_l1b: %d, ber_l1d: %d, ber_plp0: %d, plps: 0x%02x (fec: %d, mod: %d, cr: %d, snr: %f), 0x%02x (fec: %d, mod: %d, cr: %d, snr: %f), 0x%02x (fec: %d, mod: %d, cr: %d, snr: %f), 0x%02x (fec: %d, mod: %d, cr: %d, snr: %f)",
                snr_global / 1000.0,
                snr_l1b / 1000.0,
                snr_l1d / 1000.0,

               tunerInfo.status,
               tunerInfo.signalStrength / 1000,
               (cpuStatus == 0xFFFFFFFF) ? "RUNNING" : "HALTED",
               demodLockStatus,
                demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_RF_LOCK,
                demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_L1B_LOCK,
                demodLockStatus & SL_DEMOD_LOCK_STATUS_MASK_ATSC3P0_L1D_LOCK,

                ber_l1b,
               ber_l1d,
               ber_plp0,
               loop_atsc3ConfigParams.plp0,
                myPlps[0].L1dSfPlpFecType,
                myPlps[0].L1dSfPlpModType,
                myPlps[0].L1dSfPlpCoderate,
                atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[0].snr1000 / 1000.0,
                loop_atsc3ConfigParams.plp1,
                myPlps[1].L1dSfPlpFecType,
                myPlps[1].L1dSfPlpModType,
                myPlps[1].L1dSfPlpCoderate,
                atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[1].snr1000 / 1000.0,
               loop_atsc3ConfigParams.plp2,
                myPlps[2].L1dSfPlpFecType,
                myPlps[2].L1dSfPlpModType,
                myPlps[2].L1dSfPlpCoderate,
                atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[2].snr1000 / 1000.0,
                loop_atsc3ConfigParams.plp3,
                myPlps[3].L1dSfPlpFecType,
                myPlps[3].L1dSfPlpModType,
                myPlps[3].L1dSfPlpCoderate,
                atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[3].snr1000 / 1000.0
        );

        if(atsc3_ndk_phy_bridge_get_instance()) {
            atsc3_ndk_phy_bridge_get_instance()->atsc3_update_rf_stats_from_atsc3_ndk_phy_client_rf_metrics_t(&atsc3_ndk_phy_client_rf_metrics);
            atsc3_ndk_phy_bridge_get_instance()->atsc3_update_rf_bw_stats(saankhyaPHYAndroid->alp_completed_packets_parsed,
                                                                          saankhyaPHYAndroid->alp_total_bytes,
                                                                          saankhyaPHYAndroid->alp_total_LMTs_recv);
         }

        //we've already unlocked, so don't fall thru
        continue;

sl_i2c_tuner_mutex_unlock:

        //jjustman-2022-07-24 - recovery "workaround" for demod
        SL_DemodStop(slUnit);
        usleep(1000000);

        SL_DemodStart(slUnit);
        usleep(1000000);

        SL_I2C_command_mutex_tuner_status_io.unlock();

        if(global_sl_result_error_flag != SL_OK || global_sl_i2c_result_error_flag != SL_I2C_OK || dres != SL_OK || sl_res != SL_OK || tres != SL_TUNER_OK) {
            if(atsc3_ndk_phy_bridge_get_instance()) {
                atsc3_ndk_phy_bridge_get_instance()->atsc3_notify_phy_error("SaankhyaPHYAndroid::tunerStatusThread() - ERROR: command failed - Stopping/Starting Demod - global_sl_res: %d, global_sl_i2c_res: %d, dres: %d, sl_res, tres: %d",
                        global_sl_result_error_flag, global_sl_i2c_result_error_flag,
                        dres, sl_res, tres);
            }


        }
    }

    this->releasePinnedStatusThreadAsNeeded();
    this->statusThreadIsRunning = false;
    _SAANKHYA_PHY_ANDROID_INFO("SaankhyaPHYAndroid::statusThread complete");

    return 0;
}

void SaankhyaPHYAndroid::printToConsoleAtsc3L1dDiagnostics(SL_Atsc3p0L1D_Diag_t diag) {

    SL_Printf("\n---------ATSC3.0 L1DTime Diagnostics-------------------------");

    /* SL_Atsc3p0L1DGlobal_Diag_t parameters */

    SL_Printf("\n L1dVersion                  = 0x%x", diag.l1dGlobalParamsStr.L1dVersion);
    SL_Printf("\n L1dNumRf                    = 0x%x", diag.l1dGlobalParamsStr.L1dNumRf);
    SL_Printf("\n L1dRfFrequency              = 0x%x", diag.l1dGlobalParamsStr.L1dRfFrequency);
    SL_Printf("\n Reserved                    = 0x%x", diag.l1dGlobalParamsStr.Reserved);
    SL_Printf("\n L1dTimeSec                  = 0x%x", diag.l1dGlobalParamsStr.L1dTimeSec);
    SL_Printf("\n L1dTimeMsec                 = 0x%x", diag.l1dGlobalParamsStr.L1dTimeMsec);
    SL_Printf("\n L1dTimeUsec                 = 0x%x", diag.l1dGlobalParamsStr.L1dTimeUsec);
    SL_Printf("\n L1dTimeNsec                 = 0x%x", diag.l1dGlobalParamsStr.L1dTimeNsec);
}

void SaankhyaPHYAndroid::RxDataCallback(unsigned char *data, long len) {
    if(SaankhyaPHYAndroid::cb_should_discard) {
        return;
    }

    //_SAANKHYA_PHY_ANDROID_DEBUG("atsc3NdkClientSlImpl::RxDataCallback: pushing data: %p, len: %d", data, len);
    unique_lock<mutex> CircularBufferMutex_local(CircularBufferMutex);
    if(SaankhyaPHYAndroid::cb) {
        CircularBufferPush(SaankhyaPHYAndroid::cb, (char *) data, len);
    }
    CircularBufferMutex_local.unlock();
}

void SaankhyaPHYAndroid::NotifyPlpSelectionChangeCallback(vector<uint8_t> plps, void *context) {
    ((SaankhyaPHYAndroid *) context)->listen_plps(plps);
}

void SaankhyaPHYAndroid::allocate_atsc3_sl_tlv_block() {
    //protect for de-alloc with using recursive lock here
    atsc3_sl_tlv_block_mutex.lock();

    if(!atsc3_sl_tlv_block) {
        atsc3_sl_tlv_block = block_Alloc(TLV_CIRCULAR_BUFFER_PROCESS_BLOCK_SIZE);
    }
    atsc3_sl_tlv_block_mutex.unlock();
}

extern "C"
JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_init(JNIEnv *env, jobject instance) {
    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);

    _SAANKHYA_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_init: start init, env: %p", env);
    if(saankhyaPHYAndroid) {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_init: start init, saankhyaPHYAndroid is present: %p, calling deinit/delete", saankhyaPHYAndroid);
        saankhyaPHYAndroid->deinit();
        saankhyaPHYAndroid = nullptr;
    }

    saankhyaPHYAndroid = new SaankhyaPHYAndroid(env, instance);
    saankhyaPHYAndroid->init();

    _SAANKHYA_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_init: return, instance: %p", saankhyaPHYAndroid);

    return 0;
}


extern "C"
JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_run(JNIEnv *env, jobject thiz) {
    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!saankhyaPHYAndroid) {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_run: error, srtRxSTLTPVirtualPHYAndroid is NULL!");
        res = -1;
    } else {
        res = saankhyaPHYAndroid->run();
        _SAANKHYA_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_run: returning res: %d", res);
    }

    return res;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_is_1running(JNIEnv* env, jobject instance)
{
    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);

    jboolean res = false;

    if(!saankhyaPHYAndroid) {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_is_1running: error, srtRxSTLTPVirtualPHYAndroid is NULL!");
        res = false;
    } else {
        res = saankhyaPHYAndroid->is_running();
    }

    return res;
}


extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_stop(JNIEnv *env, jobject thiz) {
    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!saankhyaPHYAndroid) {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_stop: error, srtRxSTLTPVirtualPHYAndroid is NULL!");
        res = -1;
    } else {
        res = saankhyaPHYAndroid->stop();
        _SAANKHYA_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_stop: returning res: %d", res);
    }

    return res;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_deinit(JNIEnv *env, jobject thiz) {
    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!saankhyaPHYAndroid) {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_deinit: error, srtRxSTLTPVirtualPHYAndroid is NULL!");
        res = -1;
    } else {

        saankhyaPHYAndroid->deinit();
        saankhyaPHYAndroid = nullptr;
    }

    return res;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_download_1bootloader_1firmware(JNIEnv *env, jobject thiz, jint fd, jint device_type, jstring device_path_jstring) {
    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);

    _SAANKHYA_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_download_1bootloader_1firmware: fd: %d", fd);
    int res = 0;

    if(!saankhyaPHYAndroid)  {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_download_1bootloader_1firmware: saankhyaPHYAndroid is NULL!");
        res = -1;
    } else {
        const char* device_path_weak = env->GetStringUTFChars(device_path_jstring, 0);
        string device_path(device_path_weak);

        res = saankhyaPHYAndroid->download_bootloader_firmware(fd, device_type, device_path); //calls pre_init
        env->ReleaseStringUTFChars( device_path_jstring, device_path_weak );

        //jjustman-2020-08-23 - hack, clear out our in-flight reference since we should re-enumerate
        delete saankhyaPHYAndroid;
        saankhyaPHYAndroid = nullptr;
    }

    return res;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_open(JNIEnv *env, jobject thiz, jint fd, jint device_type, jstring device_path_jstring) {
    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);

    _SAANKHYA_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_open: fd: %d", fd);

    int res = 0;
    if(!saankhyaPHYAndroid) {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_open: saankhyaPHYAndroid is NULL!");
        res = -1;
    } else {
        const char* device_path_weak = env->GetStringUTFChars(device_path_jstring, 0);
        string device_path(device_path_weak);

        res = saankhyaPHYAndroid->open(fd, device_type, device_path);
        env->ReleaseStringUTFChars( device_path_jstring, device_path_weak );
    }
    _SAANKHYA_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_open: fd: %d, return: %d", fd, res);

    return res;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_tune(JNIEnv *env, jobject thiz,
                                                                      jint freq_khz,
                                                                      jint single_plp) {

    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);


    int res = 0;
    if(!saankhyaPHYAndroid) {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_tune: saankhyaPHYAndroid is NULL!");
        res = -1;
    } else {
        res = saankhyaPHYAndroid->tune(freq_khz, single_plp);
    }

    return res;
}
extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_listen_1plps(JNIEnv *env,
                                                                              jobject thiz,
                                                                              jobject plps) {
    lock_guard<mutex> saankhy_phy_android_cctor_mutex_local(SaankhyaPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!saankhyaPHYAndroid) {
        _SAANKHYA_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_listen_1plps: saankhyaPHYAndroid is NULL!");
        res = -1;
    } else {
        vector<uint8_t> listen_plps;

        jobject jIterator = env->CallObjectMethod(plps, env->GetMethodID(env->GetObjectClass(plps), "iterator", "()Ljava/util/Iterator;"));
        jmethodID nextMid = env->GetMethodID(env->GetObjectClass(jIterator), "next", "()Ljava/lang/Object;");
        jmethodID hasNextMid = env->GetMethodID(env->GetObjectClass(jIterator), "hasNext", "()Z");

        while (env->CallBooleanMethod(jIterator, hasNextMid)) {
            jobject jItem = env->CallObjectMethod(jIterator, nextMid);
            jbyte jByte = env->CallByteMethod(jItem, env->GetMethodID(env->GetObjectClass(jItem), "byteValue", "()B"));
            listen_plps.push_back(jByte);
        }

        res = saankhyaPHYAndroid->listen_plps(listen_plps);
    }

    return res;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_get_1sdk_1version(JNIEnv *env, jobject thiz) {
    string sdk_version = saankhyaPHYAndroid->get_sdk_version();
    return env->NewStringUTF(sdk_version.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_SaankhyaPHYAndroid_get_1firmware_1version(JNIEnv *env, jobject thiz) {
    string firmware_version = saankhyaPHYAndroid->get_firmware_version();
    return env->NewStringUTF(firmware_version.c_str());
}