#pragma once
#include "wscore/audio.hpp"
#include <string>
#include <vector>
namespace ws {
struct Source { uint32_t id; int pid; std::string name, bundle; bool active; };
std::vector<Source> sources();
using Feed = void (*)(void*, Frame) noexcept;
class Tap {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    Spsc<Frame, 65536> queue;
    std::atomic<uint64_t> dropped{0};
    Tap();
    ~Tap();
    double start(uint32_t source, bool mute, Feed feed = nullptr, void* context = nullptr);
    void stop();
    double latency() const;
};
using Render = void (*)(void*, float*, float*, unsigned) noexcept;
class Output {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    Output();
    ~Output();
    void start(Render render, void* context, bool live = false);
    void stop();
    double latency() const;
};
}
