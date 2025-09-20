//
// Created by malkj on 2025-09-18.
//

#ifndef FFAUDIO_CLOCK_UTILS_H
#define FFAUDIO_CLOCK_UTILS_H

/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

#include "ffaudio.h"

double get_clock(Clock *c);

void set_clock_at(Clock *c, double pts, int serial, double time);

void set_clock(Clock *c, double pts, int serial);

void set_clock_speed(Clock *c, double speed);

void init_clock(Clock *c, int *queue_serial);

void sync_clock_to_slave(Clock *c, Clock *slave);

int get_master_sync_type(const TrackState *is);

double get_master_clock(TrackState *is);


#endif //FFAUDIO_CLOCK_UTILS_H