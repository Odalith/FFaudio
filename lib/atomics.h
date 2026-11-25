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
 * The atomics of ffaudio: every value one thread writes and another reads without a lock is an FfAtomic, and every
 * access goes through these functions (FfAtomic is a struct, so a plain read or write does not compile). They are the
 * compiler's own builtins in ffaudio's code, so ThreadSanitizer sees them and what they publish; SDL's atomics are
 * compiled inside SDL, out of its sight, and their ordering is SDL's (SDL 2.30's SDL_AtomicSet() is only an acquire
 * barrier on GCC).
 *
 * ffa_load / ffa_store / ffa_exchange / ffa_fetch_add / ffa_cas are sequentially consistent: the default. The
 * _acquire / _release / _relaxed ones are for the hand-offs docs/threading.md describes ("Atomics and lock-free
 * values"), where the weaker order is part of the design.
 */
#ifndef FFAUDIO_ATOMICS_H
#define FFAUDIO_ATOMICS_H

#include <stdbool.h>
#include <stdint.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "ffaudio is built with GCC or Clang (MinGW on Windows): lib/atomics.h uses their __atomic builtins"
#endif

typedef struct FfAtomic {
    int32_t v;
} FfAtomic;

static inline int32_t ffa_load(const FfAtomic *a) { return __atomic_load_n(&a->v, __ATOMIC_SEQ_CST); }
static inline void ffa_store(FfAtomic *a, const int32_t v) { __atomic_store_n(&a->v, v, __ATOMIC_SEQ_CST); }
// The old value
static inline int32_t ffa_exchange(FfAtomic *a, const int32_t v) { return __atomic_exchange_n(&a->v, v, __ATOMIC_SEQ_CST); }
// The old value
static inline int32_t ffa_fetch_add(FfAtomic *a, const int32_t v) { return __atomic_fetch_add(&a->v, v, __ATOMIC_SEQ_CST); }
// true when it was `expected` and is `desired` now
static inline bool ffa_cas(FfAtomic *a, int32_t expected, const int32_t desired) {
    return __atomic_compare_exchange_n(&a->v, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline int32_t ffa_load_acquire(const FfAtomic *a) { return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE); }
static inline int32_t ffa_load_relaxed(const FfAtomic *a) { return __atomic_load_n(&a->v, __ATOMIC_RELAXED); }
static inline void ffa_store_release(FfAtomic *a, const int32_t v) { __atomic_store_n(&a->v, v, __ATOMIC_RELEASE); }
static inline void ffa_store_relaxed(FfAtomic *a, const int32_t v) { __atomic_store_n(&a->v, v, __ATOMIC_RELAXED); }

#endif //FFAUDIO_ATOMICS_H
