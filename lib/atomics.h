/*
* Copyright (c) 2025 Odalith
 *
 * ffaudio is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ffaudio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFaudio; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * 32-bit atomics for state that publishes other data (a flag set after the data it guards is written, the indices of
 * a ring buffer). SDL's atomics are compiled into SDL, out of ThreadSanitizer's sight, so it can not follow what they
 * publish; these are compiler builtins in ffaudio's own code (GCC, Clang). MSVC gets full-barrier equivalents.
 * Plain flags that publish nothing can stay SDL_atomic_t.
 */
#ifndef FFAUDIO_ATOMICS_H
#define FFAUDIO_ATOMICS_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>

static __inline int32_t ffa_load_acquire(const int32_t *p) { return (int32_t)_InterlockedOr((volatile long *)p, 0); }
static __inline int32_t ffa_load_relaxed(const int32_t *p) { return *(const volatile int32_t *)p; }
static __inline void ffa_store_release(int32_t *p, const int32_t v) { _InterlockedExchange((volatile long *)p, v); }
static __inline void ffa_store_relaxed(int32_t *p, const int32_t v) { *(volatile int32_t *)p = v; }
static __inline int32_t ffa_fetch_add(int32_t *p, const int32_t v) {
    return (int32_t)_InterlockedExchangeAdd((volatile long *)p, v);
}
static __inline bool ffa_cas(int32_t *p, const int32_t expected, const int32_t desired) {
    return _InterlockedCompareExchange((volatile long *)p, desired, expected) == expected;
}

#else

static inline int32_t ffa_load_acquire(const int32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline int32_t ffa_load_relaxed(const int32_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void ffa_store_release(int32_t *p, const int32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void ffa_store_relaxed(int32_t *p, const int32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline int32_t ffa_fetch_add(int32_t *p, const int32_t v) { return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL); }
static inline bool ffa_cas(int32_t *p, int32_t expected, const int32_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

#endif

#endif //FFAUDIO_ATOMICS_H
