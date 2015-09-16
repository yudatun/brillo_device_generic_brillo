#
# Copyright (C) 2015 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
  audio_test.cpp \
  libmedia_record.cpp \
  libmedia_playback.cpp \
  stagefright_record.cpp \
  stagefright_playback.cpp \
  SineSource.cpp \

LOCAL_MODULE := audio_test

LOCAL_CLANG := true
LOCAL_CFLAGS += -Wall -Wno-unused-parameter -Werror \

LOCAL_SHARED_LIBRARIES := \
  libbase \
  libbinder \
  libc \
  liblog \
  libmedia \
  libstagefright \
  libstagefright_foundation \
  libutils \

LOCAL_C_INCLUDES := \
  $(TOP)/frameworks/av/cmds/stagefright \
  $(TOP)/frameworks/av/media/libstagefright \
  $(TOP)/frameworks/native/include \
  $(TOP)/frameworks/native/include/media/openmax \
  $(TOP)/system/core/base/include \

include $(BUILD_EXECUTABLE)
