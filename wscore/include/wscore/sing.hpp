#pragma once
#include "audio.hpp"
#include "realtime.hpp"
#include "resample.hpp"
#include "separation.hpp"
#include "signalsmith-stretch.h"
#include <thread>
namespace ws {
struct SingInput { Frame audio; uint64_t position; };
struct SingFrame { float l, r, vl, vr; uint64_t position; };
class Sing {
    Spsc<SingInput, 1048576> input;
    Spsc<SingFrame, 524288> ready;
    std::thread worker;
    std::atomic<bool> quit{false};
    std::atomic<uint64_t> captured{0}, consumed{0};
    std::atomic<uint64_t> lastAudible{0};
    std::atomic<int64_t> lastFeed{0};
    std::atomic<size_t> bufferedFrames{0};
    std::atomic<bool> processing{false}, outputReady{false};
    signalsmith::stretch::SignalsmithStretch<float> stretch{43};
    std::array<std::array<float, 257>, 2> mixed{};
    double fraction = 0;
    bool primed = false;
    float gain = 1;
    unsigned sourceRate;
    void run(std::string six, std::string four);
public:
    std::atomic<float> removal{.7f}, pitch{0};
    std::atomic<int> quality{0};
    std::atomic<double> latency{0}, rtf{0};
    std::atomic<uint64_t> underruns{0}, dropped{0};
    Sing(unsigned sampleRate, std::string six, std::string four) : sourceRate(sampleRate) {
        stretch.presetDefault(2, rate);
        quality.store(six.empty() ? (four.empty() ? 2 : 1) : 0);
        worker = std::thread([this, six, four] { run(six, four); });
    }
    ~Sing() { quit.store(true); if (worker.joinable()) worker.join(); }
    void feed(Frame frame) noexcept {
        auto position = captured.fetch_add(1, std::memory_order_relaxed);
        if ((position & 4095) == 0) lastFeed.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        if (std::max(std::abs(frame.l), std::abs(frame.r)) > .0001f) lastAudible.store(position + 1, std::memory_order_relaxed);
        if (!input.push({frame, position})) dropped.fetch_add(1, std::memory_order_relaxed);
    }
    double delay() const noexcept { return std::max(0., (double(captured.load()) - double(consumed.load())) / sourceRate); }
    double buffered() const noexcept { return std::min(6., double(bufferedFrames.load()) / sourceRate); }
    double position() const noexcept { return double(consumed.load()) / sourceRate; }
    int state() const noexcept {
        auto signal = lastAudible.load();
        if (outputReady.load() && signal && double(consumed.load()) < double(signal) + sourceRate) return 3;
        auto elapsed = std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(lastFeed.load());
        if (!signal || elapsed > std::chrono::seconds(1) || double(captured.load()) > double(signal) + sourceRate) return 4;
        return processing.load() ? 2 : 1;
    }
    void render(float* left, float* right, unsigned count) noexcept WS_REALTIME;
};
}
