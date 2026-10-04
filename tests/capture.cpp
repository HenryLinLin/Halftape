#include "wsbridge.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

extern int mock_tap_starts;

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
        setenv("HALFTAPE_HEADLESS", "1", 1);
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

        ws_grab(h, 1.0);
        auto clip = waitForClip(h);
        require(clip.history_count == 1 && clip.duration > .9, "stopped buffer did not produce a history clip");
        require(std::abs(status(h).buffered - captured) < .05, "grab changed the stopped capture buffer");
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
