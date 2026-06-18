/*
 * Portable atomic helpers for GCC/Clang and MSVC.
 *
 * GCC/Clang provide __atomic builtins; MSVC provides Interlocked
 * functions from <windows.h>.  These macros paper over the
 * difference so the scheduler can use a single spelling everywhere.
 */

#pragma once

#ifdef _MSC_VER
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define sched_atomic_exchange(ptr, val) \
	InterlockedExchange((volatile long *)(ptr), (long)(val))

#define sched_atomic_load(ptr) \
	InterlockedCompareExchange((volatile long *)(ptr), 0, 0)

#define sched_atomic_store(ptr, val) \
	InterlockedExchange((volatile long *)(ptr), (long)(val))

#else

#define sched_atomic_exchange(ptr, val) \
	__atomic_exchange_n(ptr, val, __ATOMIC_ACQ_REL)

#define sched_atomic_load(ptr) \
	__atomic_load_n(ptr, __ATOMIC_ACQUIRE)

#define sched_atomic_store(ptr, val) \
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE)

#endif
