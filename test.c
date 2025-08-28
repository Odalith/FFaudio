//
// Created by malkj on 27/07/25.
//

#include <stdio.h>

#include "ffaudio.h"

const char* queue[4];
int queue_pos = 0;

void error_callback(const char* message, int request) {
    printf("Error\n");
}

void eof_callback() {
    printf("EOF\n");
    ++queue_pos;
    const char * song = queue[queue_pos];
    printf("Playing %s\n", song);
    play_audio(song, NULL, NULL);
}

void restart_callback() {}

int main(void) {
    queue[0] = "/home/malkj/Music/POWER/01-01 POWER.flac";
    queue[1] = "/home/malkj/Music/Sin Woven Puppet/01-01 Sin Woven Puppet.flac";
    queue[2] = "/home/malkj/Music/Perfect Dreams/01-01 Perfect Dreams.flac";
    queue[3] = "/home/malkj/Music/Des Rocs - Dream Machine (The Lucid Edition) (2023)/05. Des Rocs - Never Ending Moment.flac";

    initialize("Nachtul", 50, 1, error_callback, eof_callback, restart_callback);

    play_audio(queue[queue_pos], NULL, NULL);

    wait_loop();

    return 0;
}
