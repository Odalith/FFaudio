//
// Created by malkj on 27/07/25.
//

#include <filesystem>
#include <vector>
#include <string>
#include <iostream>
#include "ffaudio.h"

namespace fs = std::filesystem;

static std::vector<fs::path> queue;

std::vector<fs::path> get_regular_files(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;

    if (!fs::exists(root, ec)) {
        throw std::runtime_error("Root does not exist: " + root.string());
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec) throw std::runtime_error("Iterator creation failed: " + ec.message());

    for (; it != end; it.increment(ec)) {
        if (ec) {
            // Skip entries we can’t access and continue
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec) && !ec) {
            out.push_back(it->path());
        } else {
            ec.clear();
        }
    }
    return out;
}


int queue_pos = 0;

void error_callback(const char* message, int request) {
    printf("Error\n");
}

void eof_callback() {
    printf("EOF\n");
    ++queue_pos;
    if (queue_pos >= queue.size()) {
        std::cout << "Done" << std::endl;
        return;
    }
    const auto song = queue[queue_pos].string();
    std::cout << "Playing" << song << std::endl;
    play_audio(song.c_str(), nullptr, nullptr);
}

void restart_callback() {}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Pass the media directory as an argument";
        return 1;
    }

    const fs::path root =  argv[1];

    try {
        queue = get_regular_files(root);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }


    initialize("Nachtul", 50, 0, error_callback, eof_callback, restart_callback);

    play_audio(queue[queue_pos].c_str(), nullptr, nullptr);

    wait_loop();

    return 0;
}
