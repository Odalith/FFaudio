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

#ifndef FFAUDIO_GAIN_CACHE_H
#define FFAUDIO_GAIN_CACHE_H

#include "globals.h"

// Thread-safe lookup used by the player (no-op before anything was cached)
bool gain_cache_lookup(const char *path, AuReplayGainInfo *out);

// Implemented by the player: re-evaluate the gain of live tracks with this path (NULL = every track)
void ff_gain_cache_changed(const char *path);

#endif //FFAUDIO_GAIN_CACHE_H
