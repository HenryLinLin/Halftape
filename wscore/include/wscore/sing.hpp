#pragma once
#include "audio.hpp"
#include "realtime.hpp"
#include "streaming.hpp"
#include "signalsmith-stretch.h"
#include <chrono>
#include <semaphore>
#include <thread>

namespace ws {
struct SingInput { Frame audio; uint64_t position; };
struct SingFrame { float l, r, vl, vr; uint64_t position, epoch; };
class Sing {
    Spsc<SingInput, 65536> input;
    Spsc<SingFrame, 16384> ready;
    std::unique_ptr<LiveSeparator> separator;
    std::thread worker;
    std::counting_semaphore<> wake{0};
    std::mutex errorMutex;
    std::string failure;
    std::atomic<bool> quit{false}, failed{false}, outputReady{false};
    std::atomic<uint64_t> captured{0}, consumed{0}, lastAudible{0}, epoch{0};
    std::atomic<int64_t> lastFeed{0};
    std::atomic<unsigned> queued{0}, boost{0};
    signalsmith::stretch::SignalsmithStretch<float> stretch{43};
    std::array<std::array<float, 256>, 2> mixed{};
    SingFrame previous{}, next{};
    double fraction = 0;
    uint64_t renderEpoch = 0;
    bool primed = false, transposing = false;
    float gain = 1, fade = 0, smoothRemoval = 1, lastLeft = 0, lastRight = 0;
    unsigned ramp = 0;
    unsigned sourceRate;
    void run();
public:
    std::atomic<unsigned> targetFrames{4096}, minimumTarget{1024};
    std::atomic<float> silenceSeconds{.15f};
    std::atomic<float> removal{1}, pitch{0};
    std::atomic<int> quality{3};
    std::atomic<double> rtf{0};
    std::atomic<double> maxHopSeconds{0};
    std::atomic<double> cpuRtf{0};
    std::atomic<uint64_t> underruns{0}, dropped{0}, relaxed{0};
    Sing(unsigned sampleRate, const std::string& model);
    Sing(unsigned sampleRate, std::unique_ptr<LiveSeparator> processor);
    ~Sing();
    void feed(Frame frame) noexcept;
    double delay() const noexcept;
    double buffered() const noexcept { return double(queued.load()) / rate; }
    double position() const noexcept { return double(consumed.load()) / sourceRate; }
    int state() const noexcept;
    std::string error();
    void render(float* left, float* right, unsigned count) noexcept WS_REALTIME;
};
}
