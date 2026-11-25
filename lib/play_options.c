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

#include <math.h>
#include <string.h>
#include <libavutil/mem.h>
#include "play_options.h"

AuPlayOptions *au_play_options_create(void) {
    return av_mallocz(sizeof(AuPlayOptions));
}

void au_play_options_free(AuPlayOptions *o) {
    if (!o) return;
    av_free(o->filters);
    av_free(o->http_headers);
    av_free(o);
}

// Replace *field with a copy of s ("" and NULL: none); unchanged when there is no memory
static int set_string(char **field, const char *s) {
    char *copy = NULL;
    if (s && s[0] && !(copy = av_strdup(s))) return AU_ERR_NO_MEMORY;
    av_free(*field);
    *field = copy;
    return AU_OK;
}

int au_play_options_set_range(AuPlayOptions *o, const double start_seconds, const double length_seconds) {
    if (!o || !isfinite(start_seconds) || !isfinite(length_seconds) || start_seconds < 0 || length_seconds < 0)
        return AU_ERR_INVALID_ARGUMENT;
    o->start_seconds = start_seconds;
    o->length_seconds = length_seconds;
    return AU_OK;
}

int au_play_options_set_filters(AuPlayOptions *o, const char *filters) {
    if (!o) return AU_ERR_INVALID_ARGUMENT;
    if (au_validate_filters(filters) != AU_OK) return AU_ERR_INVALID_ARGUMENT;
    return set_string(&o->filters, filters);
}

int au_play_options_set_fade_in(AuPlayOptions *o, const AuFade *fade) {
    if (!o || (fade && !ff_fade_valid(fade))) return AU_ERR_INVALID_ARGUMENT;
    o->has_fade_in = fade != NULL;
    o->fade_in = fade ? *fade : (AuFade){0};
    return AU_OK;
}

int au_play_options_set_replaygain(AuPlayOptions *o, const AuReplayGain *rg) {
    if (!o) return AU_ERR_INVALID_ARGUMENT;
    o->has_replaygain = rg != NULL;
    if (rg) o->replaygain = *rg;
    return AU_OK;
}

int au_play_options_set_http_headers(AuPlayOptions *o, const char *headers) {
    if (!o) return AU_ERR_INVALID_ARGUMENT;
    return set_string(&o->http_headers, headers);
}

AuPlayOptions *play_options_clone(const AuPlayOptions *o, bool *failed) {
    *failed = false;
    if (!o) return NULL;
    AuPlayOptions *c = av_malloc(sizeof(*c));
    if (!c) goto fail;
    *c = *o;
    c->filters = c->http_headers = NULL;
    if ((o->filters && !(c->filters = av_strdup(o->filters))) ||
        (o->http_headers && !(c->http_headers = av_strdup(o->http_headers)))) {
        au_play_options_free(c);
        goto fail;
    }
    return c;
fail:
    *failed = true;
    return NULL;
}

bool ff_fade_valid(const AuFade *fade) {
    if (!fade || !isfinite(fade->seconds)) return false;
    switch (fade->curve) {
        case AU_FADE_LINEAR:
        case AU_FADE_EQUAL_POWER:
        case AU_FADE_S_CURVE:
        case AU_FADE_LOGARITHMIC:
        case AU_FADE_EXPONENTIAL:
            return true;
        case AU_FADE_CUSTOM:
            return fade->custom != NULL;
    }
    return false;
}
