/*
 * dlog.h
 *
 *  Created on: 2013-12-6
 *      Author: auxten
 */

#ifndef DLOG_H_
#define DLOG_H_

#include <android/log.h>

#define LOG_TAG  "defense"
#define DEFENSE_DEBUG 0

#define CONDITION(cond)     (__builtin_expect((cond)!=0, 0))


#if DEFENSE_DEBUG
#define LINELOG()  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "at %s line %d", __FILE__, __LINE__)
//#define LINELOG()
#define LOGD(fmt, args...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, fmt, ##args)
#define LOGI(fmt, args...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, fmt, ##args)
#define LOGW(fmt, args...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, fmt, ##args)
#define LOGE(fmt, args...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##args)
#define LOG_ALWAYS_FATAL_IF(cond, ...) \
    ( (CONDITION(cond)) \
    ? ((void)LOGE(#cond, __VA_ARGS__)) \
    : (void)0 )

#define LOG_ALWAYS_FATAL(...) \
    ( ((void)LOGE(NULL, __VA_ARGS__)) )
#define LOG_FATAL_IF(cond, ...) LOG_ALWAYS_FATAL_IF(cond, __VA_ARGS__)
#define ALOG_ASSERT(cond, ...) \
        ( (! CONDITION(cond)) \
        ? ((void)LOGE(#cond, __VA_ARGS__)) \
        : (void)0 )

#define LOG_FATAL(...) LOG_ALWAYS_FATAL(__VA_ARGS__)
#define TABLE_NOISY(...) __VA_ARGS__
#define POOL_NOISY(...) __VA_ARGS__
#define LOAD_TABLE_NOISY(...) __VA_ARGS__

#else
#define LINELOG()
#define LOGD(fmt, args...)
#define LOGI(fmt, args...)
#define LOGW(fmt, args...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, fmt, ##args)
#define LOGE(fmt, args...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##args)
#define LOG_ALWAYS_FATAL_IF(cond, ...)
#define LOG_ALWAYS_FATAL(...)
#define LOG_FATAL_IF(cond, ...)
#define LOG_FATAL(...)
#define TABLE_NOISY(...)
#define POOL_NOISY(...)
#define ALOG_ASSERT(cond, ...)
#define LOAD_TABLE_NOISY(...)


#endif




#endif /* DLOG_H_ */
