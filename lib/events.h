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

#ifndef FFAUDIO_EVENTS_H
#define FFAUDIO_EVENTS_H

#include "ffaudio.h"

// The queue behind au_poll_event(), for players without an on_event callback (lib/events.c)

#define EVENT_QUEUE_CAPACITY 4096

// Empty the queue and accept events (au_init())
void events_open(void);
// Empty the queue, refuse events and wake au_wait_event() (au_shutdown())
void events_close(void);
// A copy of the event, its strings included (control thread)
void events_push(const AuEvent *e);

#endif //FFAUDIO_EVENTS_H
