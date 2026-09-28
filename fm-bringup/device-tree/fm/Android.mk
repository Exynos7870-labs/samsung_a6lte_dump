# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The LineageOS Project
LOCAL_PATH := $(call my-dir)

ifeq ($(BOARD_HAVE_BCM_FM_HCI),true)
include $(CLEAR_VARS)
LOCAL_MODULE := libfmjni_bcm_hci
LOCAL_SRC_FILES := BcmFmRadio.cpp FmJni.cpp
LOCAL_HEADER_LIBRARIES := jni_headers
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra -Werror
LOCAL_MULTILIB := both
include $(BUILD_SHARED_LIBRARY)
endif
