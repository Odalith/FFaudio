//
// Created by malkj on 2025-09-18.
//

#ifndef FFAUDIO_PACKET_QUEUE_UTILS_H
#define FFAUDIO_PACKET_QUEUE_UTILS_H

#include "ffaudio.h"

int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);

int packet_queue_put(PacketQueue *q, AVPacket *pkt);

int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index);

int packet_queue_init(PacketQueue *q);

void packet_queue_flush(PacketQueue *q);

void packet_queue_destroy(PacketQueue *q);

void packet_queue_abort(PacketQueue *q);

void packet_queue_start(PacketQueue *q);

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);

#endif //FFAUDIO_PACKET_QUEUE_UTILS_H