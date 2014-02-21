LOCAL_PATH:= $(call my-dir)

# build device static library
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	arsc.cpp \
	SharedBuffer.cpp \
	Unicode.cpp \
	VectorImpl.cpp \
	String8.cpp \
	String16.cpp \
	TextOutput.cpp \
	Atomic.c

LOCAL_ARM_MODE := arm
LOCAL_MODULE:= libarsc
LOCAL_C_INCLUDES += jni

include $(BUILD_STATIC_LIBRARY)

