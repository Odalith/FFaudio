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
 * Chaos points: random delays at the places where threads hand state to each other, to make races show up in tests
 * instead of once in a while on a slow machine. Built only with the CMake option FFAUDIO_CHAOS (never in a release);
 * otherwise CHAOS() and CHAOS_RT() are nothing. docs/threading.md lists what each point is between.
 *
 * At run time FFAUDIO_CHAOS="seed:percent:max_us" (default 1:10:2000) makes each point sleep, with that probability,
 * for up to max_us microseconds. CHAOS_RT() is for the audio callback: at most max_us / 20, capped at 300 us, so the
 * device keeps running (underruns are fine, the point is the order of events). Unset or percent 0: no delays. The
 * thread ids are mixed into each thread's random sequence, so a seed gives the same kind of run, not the same run.
 *
 * FFAUDIO_CHAOS_AT="name=us|name=us" makes the named points (exactly as written in CHAOS("…")) sleep that long every
 * time, CHAOS_RT ones included: tests/test_races.c uses it to open one race's window on every run.
 */

#ifndef FFAUDIO_CHAOS_H
#define FFAUDIO_CHAOS_H

#ifdef FFAUDIO_CHAOS
void ff_chaos_point(const char *where, int realtime);
#define CHAOS(where) ff_chaos_point(where, 0)
#define CHAOS_RT(where) ff_chaos_point(where, 1)
#else
#define CHAOS(where) ((void)0)
#define CHAOS_RT(where) ((void)0)
#endif

#endif // FFAUDIO_CHAOS_H
