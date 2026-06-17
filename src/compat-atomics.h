/*
 * Portable atomic helpers for GCC/Clang and MSVC.
 *
 * GCC/Clang provide __sync builtins; MSVC provides Interlocked
 * intrinsics.  These macros paper over the difference so the
 * scheduler can use a single spelling everywhere.
 */

#pragma once

#ifdef _MSC_VER
#include <intrin.h>

#define sched_atomic_exchange(ptr, val) \
	InterlockedExchange((volatile long *)(ptr), (long)(val))

#define sched_atomic_load(ptr) \
	InterlockedCompareExchange((volatile long *)(ptr), 0, 0)

#else

#define sched_atomic_exchange(ptr, val) \
	__sync_lock_test_and_set(ptr, val)

#define sched_atomic_load(ptr) \
	__sync_fetch_and_add(ptr, 0)

#endif
