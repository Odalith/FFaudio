// Internal additions to the loudness meter (ffaudio_loudness.h is the public part)
#ifndef FFAUDIO_LOUDNESS_INTERNAL_H
#define FFAUDIO_LOUDNESS_INTERNAL_H

#include "ffaudio_loudness.h"
#include "libavformat/avio.h"

// A meter with nothing in it (a worker keeps one for every file it measures); NULL when out of memory
AuLoudnessMeter *loudness_meter_create(void);

// Empty it (a new album)
void loudness_meter_clear(AuLoudnessMeter *m);

// au_loudness_measure() into an existing meter, which is cleared first. FFmpeg checks `interrupt` (NULL = none) while
// it connects, reads and waits between reconnects, so a measurement of a stalled stream stops right away instead of
// after its timeouts; it then returns AU_ERR_CANCELLED. The callback runs often on the measuring thread and must be
// quick. On error the meter is empty.
int loudness_measure_into(AuLoudnessMeter *m, const char *path, const AuLoudnessConfig *config,
                          const AVIOInterruptCB *interrupt);

#endif //FFAUDIO_LOUDNESS_INTERNAL_H
