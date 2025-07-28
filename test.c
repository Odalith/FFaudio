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
    play_audio(song);
}

char* next_callback() {
    printf("Next\n");
}

int main(void) {
    //queue[0] = "/home/malkj/Music/kroh - GHOSTS.flac";
    queue[0] = "/home/malkj/Downloads/Unique Formats/Jh.mp3";
    queue[1] = "/home/malkj/Music/JT Music - I Think Therefore I Am.flac";
    queue[2] = "/home/malkj/Music/NASAYA - 33.flac";
    queue[3] = "/home/malkj/Music/c. g. - dom fera - monster song.mp3";

    initialize("Nachtul", 50, 1, error_callback, eof_callback, next_callback);

    play_audio(queue[queue_pos]);

    wait_loop();

    return 0;
}
