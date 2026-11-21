// Internal additions to the loudness scanner (ffaudio_loudness.h is the public part)
#ifndef FFAUDIO_LOUDNESS_INTERNAL_H
#define FFAUDIO_LOUDNESS_INTERNAL_H

#include "ffaudio_loudness.h"
#include "libavformat/avio.h"

// FFmpeg checks `interrupt` while it connects, reads and waits between reconnects, so a measurement of a stalled
// stream stops right away instead of after its timeouts; it then returns AU_ERR_CANCELLED. The callback runs often on
// the measuring thread and must be quick. NULL (the default) = only the progress callback cancels.
void loudness_scanner_set_interrupt(AuLoudnessScanner *s, const AVIOInterruptCB *interrupt);

// Change a scanner's settings (a worker reuses one scanner for every file)
void loudness_scanner_configure(AuLoudnessScanner *s, const AuLoudnessConfig *config);

#endif //FFAUDIO_LOUDNESS_INTERNAL_H
