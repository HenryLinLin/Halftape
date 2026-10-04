#include "wsbridge.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

extern int mock_tap_starts;
extern int mock_output_active;
void mock_render(float* left, float* right, unsigned frames);

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
WSStatus status(WS* h) {
    WSStatus result{};
    ws_status(h, &result);
    return result;
}
WSStatus waitForClip(WS* h, uint64_t previous = 0) {
    for (int i = 0; i < 1000; ++i) {
        auto s = status(h);
        if (!s.busy && s.clip_id && s.clip_id != previous) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("capture clip processing timed out");
}
}

int main() {
    try {
        unsetenv("HALFTAPE_HEADLESS");
        WS* h = ws_create();
        require(h, "session creation failed");
        auto s = status(h);
        require(!s.capturing && s.buffered == 0, "new session should have no capture");

        WSSource sources[4]{};
        require(ws_sources(h, sources, 4) == 2, "mock sources were not listed");
        require(!status(h).capturing && mock_tap_starts == 0, "listing sources must not start capture");

        require(ws_arm(h, 999) == 0, "arming an unavailable source should fail");
        require(!status(h).capturing && mock_tap_starts == 0, "failed arm must leave capture stopped");

        require(ws_arm(h, 42) == 1, "source 42 did not arm");
        s = status(h);
        require(s.capturing && s.capture_source == 42, "capture status did not report source 42");
        require(std::strcmp(s.capture_name, "Mock Source A") == 0, "capture name was not retained");
        for (int i = 0; i < 300 && status(h).buffered < .95; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        double captured = status(h).buffered;
        require(captured > .95, "mock tone did not reach the capture buffer");

        ws_stop_capture(h);
        s = status(h);
        require(!s.capturing && s.capture_source == 42, "stop should retain source identity");
        require(std::abs(s.buffered - captured) < .05, "stop should preserve the captured audio");
        require(std::strcmp(s.capture_name, "Mock Source A") == 0, "stop should preserve capture name");
        ws_stop_capture(h);
        require(!status(h).capturing, "repeated stop should be idempotent");

        int startsBeforeStoppedSing = mock_tap_starts;
        ws_sing(h, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        s = status(h);
        require(!s.capturing && !s.sing && std::abs(s.buffered - captured) < .05,
                "Sing should not restart a stopped capture with retained audio");
        require(mock_tap_starts == startsBeforeStoppedSing, "Sing attempted to rearm stopped capture");

        ws_count_in(h, 0);
        ws_grab(h, 1.0);
        auto clip = waitForClip(h);
        require(clip.history_count == 1 && clip.duration > .9, "stopped buffer did not produce a history clip");
        require(std::abs(status(h).buffered - captured) < .05, "grab changed the stopped capture buffer");
        ws_play(h, 1);
        float left[512]{}, right[512]{};
        for (int i = 0; i < 24; ++i) mock_render(left, right, 512);
        double energy = 0;
        for (int i = 0; i < 512; ++i) energy += std::abs(left[i]) + std::abs(right[i]);
        require(energy > 1.0, "fake output did not render the captured clip");
        require(status(h).position > 0, "rendering did not advance the clip position");
        ws_play(h, 0);
        ws_seek(h, .4);
        std::memset(left, 0, sizeof(left));
        std::memset(right, 0, sizeof(right));
        mock_render(left, right, 512);
        require(std::abs(status(h).position - .4) < .02, "paused rendering moved away from the seek position");
        ws_loop(h, .2, .6, 0);
        ws_seek(h, .8);
        require(!status(h).looping, "seeking outside the loop should disable looping");
        ws_loop_enabled(h, 1);
        require(status(h).looping, "loop enabling wrapper did not reenable looping");
        ws_loop_enabled(h, 0);
        ws_play(h, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        require(mock_tap_starts == 1 && !status(h).capturing, "grab silently restarted capture");

        require(ws_arm(h, 43) == 1, "source 43 did not arm");
        for (int i = 0; i < 300 && status(h).buffered < .49; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        s = status(h);
        require(s.capturing && s.capture_source == 43 && std::strcmp(s.capture_name, "Mock Source B") == 0,
                "rearm did not select source 43");
        require(s.buffered >= .49 && s.buffered <= .51, "source 43 should provide a fresh half-second buffer");
        require(mock_tap_starts == 2, "rearm did not start exactly one new capture");

        ws_sing(h, 1);
        s = status(h);
        require(s.sing && s.capturing && s.capture_source == 43, "Sing did not start from active capture");
        require(mock_tap_starts == 3, "Sing should start exactly one muted capture");
        ws_sing(h, 0);
        s = status(h);
        require(!s.sing && !s.capturing && s.capture_source == 43, "stopping Sing should stop capture and retain source");
        const double singStoppedBuffer = s.buffered;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        s = status(h);
        require(std::abs(s.buffered - singStoppedBuffer) < .02, "stopped Sing capture continued growing");
        require(mock_tap_starts == 3, "stopping Sing silently rearmed capture");

        ws_seek(h, .1);
        require(ws_arm(h, 43) == 1, "could not rearm source for Sing return test");
        const int startsBeforeActiveSing = mock_tap_starts;
        ws_sing(h, 1);
        s = status(h);
        require(s.sing && s.capturing && mock_tap_starts == startsBeforeActiveSing + 1,
                "second Sing session did not start from active capture");
        ws_play(h, 1);
        std::memset(left, 0, sizeof(left));
        std::memset(right, 0, sizeof(right));
        double beforeReturn = status(h).position;
        for (int i = 0; i < 24; ++i) mock_render(left, right, 512);
        energy = 0;
        for (int i = 0; i < 512; ++i) energy += std::abs(left[i]) + std::abs(right[i]);
        s = status(h);
        require(!s.sing && !s.capturing && s.position > beforeReturn && energy > 1.0,
                "play during Sing did not return to audible prior clip playback");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        require(mock_tap_starts == startsBeforeActiveSing + 1 && !status(h).capturing,
                "play after Sing returned silently rearmed capture");

        ws_disarm(h);
        s = status(h);
        require(!s.capturing && s.buffered == 0 && s.capture_source == 0,
                "disarm should clear capture state and buffer");
        require(std::strcmp(s.capture_name, "") == 0, "disarm should clear capture name");
        int startsBeforeSing = mock_tap_starts;
        ws_sing(h, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        s = status(h);
        require(!s.capturing && !s.sing && s.buffered == 0, "Sing rearmed a disarmed capture");
        require(mock_tap_starts == startsBeforeSing, "Sing attempted to start capture after disarm");
        ws_grab(h, 1.0);
        s = status(h);
        require(!s.busy && s.history_count == 1, "empty grab should not create a clip or remain busy");
        WSHistory history[2]{};
        require(ws_history(h, history, 2) == 1 && history[0].id == clip.clip_id,
                "empty grab created a dangling history entry");
        ws_destroy(h);
        std::cout << "capture lifecycle ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
