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
 * Run-once initialization with the platform's own primitive (C99 has none), for process-wide mutexes
 * that must work before au_initialize().
 *
 *   static FfOnce once = FF_ONCE_INIT;
 *   ff_once(&once, init_function);
 */

#ifndef FFAUDIO_ONCE_H
#define FFAUDIO_ONCE_H

#ifdef _WIN32
#include <windows.h>

typedef INIT_ONCE FfOnce;
#define FF_ONCE_INIT INIT_ONCE_STATIC_INIT

static BOOL CALLBACK ff_once_trampoline(PINIT_ONCE once, PVOID fn, PVOID *ctx) {
    (void)once; (void)ctx;
    ((void (*)(void))fn)();
    return TRUE;
}

static inline void ff_once(FfOnce *once, void (*fn)(void)) {
    InitOnceExecuteOnce(once, ff_once_trampoline, (PVOID)fn, NULL);
}
#else
#include <pthread.h>

typedef pthread_once_t FfOnce;
#define FF_ONCE_INIT PTHREAD_ONCE_INIT

static inline void ff_once(FfOnce *once, void (*fn)(void)) {
    pthread_once(once, fn);
}
#endif

#endif //FFAUDIO_ONCE_H
