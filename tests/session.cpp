#include "wsbridge.h"
#include <chrono>
#include <thread>
#include <cmath>
#include <iostream>
#include <cstdlib>
static WSStatus waitClip(WS* h, uint64_t previous = 0) {
    WSStatus s{};
    for (int i = 0; i < 1000; ++i) {
        ws_status(h, &s);
        if (!s.busy && s.clip_id != previous) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("clip processing timeout");
}
int main() {
    setenv("HALFTAPE_HEADLESS", "1", 1);
    auto* h = ws_create(); if (!h) return 1;
    ws_demo(h); auto s = waitClip(h);
    if (s.duration < 11 || s.history_count != 1 || s.bpm < 115 || s.bpm > 125) return 2;
    float peaks[960]; if (ws_waveform(h, peaks, 960) != 960) return 3;
    ws_speed(h, -4); ws_pitch(h, 200); ws_status(h, &s);
    if (s.speed != .25 || s.pitch != 13) return 4;
    ws_loop(h, 2, 4, 1); ws_status(h, &s); if (s.loop_b <= s.loop_a || s.loop_a < 1.5) return 5;
    auto first = s.clip_id;
    ws_demo(h); s = waitClip(h, first); if (s.history_count != 2) return 6;
    ws_select(h, first); ws_status(h, &s); if (s.clip_id != first) return 7;
    ws_stem(h, -1, 1, 0, 0); ws_stem(h, 9, 1, 0, 0);
    ws_destroy(h); ws_destroy(nullptr);
    std::cout << "session and C API ok\n";
}
