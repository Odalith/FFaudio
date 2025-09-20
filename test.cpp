//
// Created by malkj on 27/07/25.
//

#include <filesystem>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <functional>
#include "ffaudio.h"

namespace fs = std::filesystem;

static std::vector<fs::path> queue;
int queue_pos = 0;
bool song_skipped = false;
bool stop_timer = false;
constexpr int PLAY_COUNT = -1;
constexpr int SKIP_AFTER_SECONDS = -1;

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

std::thread schedule_after(std::chrono::milliseconds delay, std::function<void()> fn) {
    return std::thread([delay, fn = std::move(fn)] {
        if (stop_timer) {
            stop_timer = false;
            return;
        }

        std::this_thread::sleep_for(delay);
        song_skipped = true;
        fn();
    });
}



void error_callback(const char* message, int request) {
    std::cout <<"Error" << std::endl;
}

void play_next() {
    if (queue_pos >= queue.size() -1 || (PLAY_COUNT > 0 && queue_pos >= PLAY_COUNT -1)) {
        std::cout << "Done" << std::endl;
        exit(0);
    }

    const auto song = queue[queue_pos].string();
    std::cout << (queue_pos + 1) << " Playing " << song << std::endl;

    play_audio(song.c_str(), nullptr, nullptr);
    ++queue_pos;

    if constexpr (SKIP_AFTER_SECONDS > 0) {
        auto t = schedule_after(std::chrono::seconds(SKIP_AFTER_SECONDS), play_next);
        t.detach();
    }
}

void eof_callback() {
    std::cout <<"EOF" << std::endl;

    if (!song_skipped) {
        play_next();
        song_skipped = false;
    }
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

    // Shuffle queue
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(queue.begin(), queue.end(), gen);


    // Setup and play audio
    initialize("Nachtul", 50, 0, MEDIUM, error_callback, eof_callback, restart_callback);

    play_next();

    wait_loop();

    return 0;
}
