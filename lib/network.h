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

#ifndef FFAUDIO_NETWORK_H
#define FFAUDIO_NETWORK_H

#include <stdbool.h>
#include "libavutil/dict.h"
#include "libavformat/avformat.h"
#include "ffaudio.h"

// What FFmpeg may open for a local path and for a URL (protocol_whitelist; nested opens, such as HLS segments and
// keys, inherit it)
#define FF_LOCAL_PROTOCOLS "file,pipe"
#define FF_NETWORK_PROTOCOLS "http,https,tls,tcp,crypto,httpproxy"

// The current au_set_network() settings with defaults filled in. user_agent points into `ua` (size ua_size).
AuNetworkConfig network_config(char *ua, size_t ua_size);

// Options for avformat_open_input(&ic, path, ...): the protocol whitelist, and for URLs the timeouts, user agent
// and extra request headers (may be NULL)
void network_input_options(AVDictionary **opts, const char *path, const char *http_headers);

// After a successful open: also reconnect after network errors (not done while connecting, so an unreachable
// server fails within the timeout instead of being retried)
void network_after_open(AVFormatContext *ic, const char *path);

#endif //FFAUDIO_NETWORK_H
