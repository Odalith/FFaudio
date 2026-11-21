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
 * Process-wide settings for network streams (au_set_network), used by the player, au_probe() and the loudness
 * analysis, which all open inputs through network_input_options().
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "network.h"
#include "once.h"

#define DEFAULT_TIMEOUT_SECONDS 10.0
#define DEFAULT_BUFFER_SECONDS 2.0
// 10 s rides out network hiccups; more costs memory that glibc keeps per thread afterwards (see the readme's
// "Memory use": with glibc's defaults, 2-hour soaks kept 310 MB with 30 s and 169 MB with 10 s)
#define DEFAULT_MAX_BUFFER_SECONDS 10.0
#define DEFAULT_RECONNECT_ATTEMPTS 5

static SDL_mutex *lock = NULL;
static FfOnce lock_once = FF_ONCE_INIT;
static AuNetworkConfig settings;      // guarded by lock; user_agent is owned (av_strdup)

static void create_lock(void) {
    lock = SDL_CreateMutex();
}

static bool locked(void) {
    ff_once(&lock_once, create_lock);
    if (!lock) return false;
    SDL_LockMutex(lock);
    return true;
}

bool au_is_network_url(const char *path) {
    if (!path || !isalpha((unsigned char)path[0])) return false;
    const char *p = path + 1;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
    if (strncmp(p, "://", 3) != 0) return false;
    // A one-letter "scheme" is a Windows drive ("C://music" works as a path there)
    if (p - path == 1) return false;
    return !(p - path == 4 && !av_strncasecmp(path, "file", 4));
}

int au_set_network(const AuNetworkConfig *config) {
    if (config) {
        if (!isfinite(config->timeout_seconds) || config->timeout_seconds < 0 || config->timeout_seconds > 600)
            return AU_ERR_INVALID_ARGUMENT;
        if (!isfinite(config->buffer_seconds) || config->buffer_seconds < 0 || config->buffer_seconds > 120)
            return AU_ERR_INVALID_ARGUMENT;
        if (!isfinite(config->max_buffer_seconds) || config->max_buffer_seconds < 0 || config->max_buffer_seconds > 3600)
            return AU_ERR_INVALID_ARGUMENT;
        if (config->reconnect_attempts < -1 || config->reconnect_attempts > 1000) return AU_ERR_INVALID_ARGUMENT;
    }
    char *ua = NULL;
    if (config && config->user_agent && config->user_agent[0] && !(ua = av_strdup(config->user_agent)))
        return AU_ERR_NO_MEMORY;
    if (!locked()) {
        av_free(ua);
        return AU_ERR_SDL;
    }
    av_free((char *)settings.user_agent);
    if (config) settings = *config;
    else memset(&settings, 0, sizeof(settings));
    settings.user_agent = ua;
    SDL_UnlockMutex(lock);
    return AU_OK;
}

AuNetworkConfig network_config(char *ua, const size_t ua_size) {
    AuNetworkConfig c;
    memset(&c, 0, sizeof(c));
    if (locked()) {
        c = settings;
        if (ua) snprintf(ua, ua_size, "%s", settings.user_agent ? settings.user_agent : "");
        SDL_UnlockMutex(lock);
    }
    else if (ua) {
        ua[0] = '\0';
    }
    if (ua && !ua[0]) snprintf(ua, ua_size, "ffaudio/%s", au_get_version());
    c.user_agent = ua; // NULL when only the numbers were asked for
    if (c.timeout_seconds <= 0) c.timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
    if (c.buffer_seconds <= 0) c.buffer_seconds = DEFAULT_BUFFER_SECONDS;
    if (c.max_buffer_seconds <= 0) c.max_buffer_seconds = DEFAULT_MAX_BUFFER_SECONDS;
    if (c.max_buffer_seconds < c.buffer_seconds) c.max_buffer_seconds = c.buffer_seconds;
    if (c.reconnect_attempts == 0) c.reconnect_attempts = DEFAULT_RECONNECT_ATTEMPTS;
    return c;
}

// "A: 1\nB: 2" -> "A: 1\r\nB: 2\r\n" (FFmpeg's "headers" option wants CRLF after every header)
static char *crlf_headers(const char *headers) {
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (const char *p = headers; *p;) {
        const char *end = p;
        while (*end && *end != '\n' && *end != '\r') end++;
        // Header lines must not smuggle other lines in; empty lines are dropped
        if (end > p) av_bprintf(&bp, "%.*s\r\n", (int)(end - p), p);
        p = end;
        while (*p == '\r' || *p == '\n') p++;
    }
    char *s = NULL;
    av_bprint_finalize(&bp, &s);
    return s;
}

void network_after_open(AVFormatContext *ic, const char *path) {
    if (!ic || !ic->pb || !au_is_network_url(path)) return;
    if (network_config(NULL, 0).reconnect_attempts > 0)
        av_opt_set(ic->pb, "reconnect_on_network_error", "1", AV_OPT_SEARCH_CHILDREN); // not every protocol has it
}

void network_input_options(AVDictionary **opts, const char *path, const char *http_headers) {
    const bool net = au_is_network_url(path);
    av_dict_set(opts, "protocol_whitelist", net ? FF_NETWORK_PROTOCOLS : FF_LOCAL_PROTOCOLS, 0);
    if (!net) return;

    char ua[256];
    const AuNetworkConfig c = network_config(ua, sizeof(ua));
    char us[32];
    snprintf(us, sizeof(us), "%lld", (long long)llround(c.timeout_seconds * 1e6));
    av_dict_set(opts, "rw_timeout", us, 0);   // every read and write (microseconds)
    av_dict_set(opts, "timeout", us, 0);      // the socket, connecting included
    av_dict_set(opts, "user_agent", c.user_agent, 0);
    // FFmpeg's http reconnects: an interrupted file continues with a range request, a live stream is opened again.
    // Network errors (resets) are retried only once the stream is open (network_after_open()), so a server that
    // can not be reached still fails within the timeout
    if (c.reconnect_attempts > 0) {
        char n[16];
        snprintf(n, sizeof(n), "%d", c.reconnect_attempts);
        av_dict_set(opts, "reconnect", "1", 0);
        av_dict_set(opts, "reconnect_streamed", "1", 0);
        av_dict_set(opts, "reconnect_on_http_error", "5xx", 0);
        av_dict_set(opts, "reconnect_max_retries", n, 0);
        av_dict_set(opts, "reconnect_delay_max", "30", 0);
    }
    if (http_headers && http_headers[0]) {
        char *h = crlf_headers(http_headers);
        if (h && h[0]) av_dict_set(opts, "headers", h, 0);
        av_free(h);
    }
}
