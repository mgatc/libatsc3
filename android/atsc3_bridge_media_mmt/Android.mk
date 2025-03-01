# Android.mk for libatsc3 ExoPlayer MMT plugin
#
#
# jjustman@ngbp.org - 2020-12-02

# global pathing
MY_LOCAL_PATH := $(call my-dir)
LOCAL_PATH := $(call my-dir)
MY_CUR_PATH := $(LOCAL_PATH)

#jjustman-2020-08-10 - super hack
LOCAL_PATH := $(MY_LOCAL_PATH)

# ---------------------------
# protobuf-lite library

include $(CLEAR_VARS)
LOCAL_MODULE := libprotobuf-lite
LOCAL_SRC_FILES := libprotobuf/libs/$(TARGET_ARCH_ABI)/libprotobuf-lite.so
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/libprotobuf/include
include $(PREBUILT_SHARED_LIBRARY)

# ---------------------------

##jjustman-2020-08-12 - remove prefab LOCAL_SRC_FILES := $(LOCAL_PATH)/../libatsc3_core/build/intermediates/prefab_package/debug/prefab/modules/atsc3_core/libs/android.$(TARGET_ARCH_ABI)/libatsc3_core.so
##vmatiash-2020-08-21 - module can't depend on libraries that is not built. This dependency specified in LOCAL_LDLIBS
#include $(CLEAR_VARS)
#LOCAL_MODULE := local-atsc3_core
#LOCAL_SRC_FILES := $(LOCAL_PATH)/../atsc3_core/build/intermediates/ndkBuild/debug/obj/local/$(TARGET_ARCH_ABI)/libatsc3_core.so
#ifneq ($(MAKECMDGOALS),clean)
#include $(PREBUILT_SHARED_LIBRARY)
#endif

# ---------------------------
# atsc3_bridge_media_mmt jni interface

include $(CLEAR_VARS)

LOCAL_MODULE := atsc3_bridge_media_mmt

LIBATSC3_JNIBRIDGE_CPP := \
    $(wildcard $(LOCAL_PATH)/src/jni/*.cpp)

PROTOBUF_MODELS_CPP := \
    $(wildcard $(LOCAL_PATH)/src/generated/debug/cpp/*.cc)

#jjustman-2020-08-19 - atsc3_logging_externs may not be needed

LOCAL_SRC_FILES += \
    $(LIBATSC3_JNIBRIDGE_CPP:$(LOCAL_PATH)/%=%) \
	$(LOCAL_PATH)/../../src/atsc3_logging_externs.c \
	$(PROTOBUF_MODELS_CPP:$(LOCAL_PATH)/%=%)

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/src/jni \
    $(LOCAL_PATH)/src/generated/debug/cpp


#libatsc3 includes as needed
LOCAL_C_INCLUDES += $(LOCAL_PATH)/../../src/

# jjustman-2020-08-10 - hack-ish... needed for atsc3_pcre2_regex_utils.h
LOCAL_C_INCLUDES += $(LOCAL_PATH)/../atsc3_core/libpcre/include

LOCAL_C_INCLUDES += $(LOCAL_PATH)/../../src/mmt

#libprotobuf-lite includes
LOCAL_C_INCLUDES += $(LOCAL_PATH)/libprotobuf/include

LOCAL_SRC_FILES += $(LOCAL_PATH)/../../src/mmt/MMTExtractor.cpp
# -fpack-struct=8
LOCAL_CFLAGS += -g -fPIC  \
                -D__DISABLE_LIBPCAP__ -D__DISABLE_ISOBMFF_LINKAGE__ -D__DISABLE_NCURSES__ \
                -D__MOCK_PCAP_REPLAY__ -D__LIBATSC3_ANDROID__ -DDEBUG

LOCAL_LDFLAGS += -fPIE -fPIC -L \
					$(LOCAL_PATH)/../atsc3_core/build/intermediates/ndkBuild/debug/obj/local/$(TARGET_ARCH_ABI)/

LOCAL_LDLIBS := -ldl -lc++_shared -llog -landroid -lz -latsc3_core

# LOCAL_SHARED_LIBRARIES := atsc3_core
LOCAL_SHARED_LIBRARIES := libprotobuf-lite

include $(BUILD_SHARED_LIBRARY)

