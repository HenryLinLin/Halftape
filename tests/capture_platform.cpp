#include "mactap/platform.hpp"
#include "mactap/source_control.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

int mock_tap_starts = 0;
bool mock_emit_nonfinite_for_muted_tap = false;
bool mock_tap_muted = false;
bool mock_tap_active = false;
namespace {
ws::Render active_render = nullptr;
void* active_context = nullptr;
}
int mock_output_active = 0;
void mock_render(float* left, float* right, unsigned frames) {
    if (active_render) active_render(active_context, left, right, frames);
}

namespace ws {
struct Tap::Impl {};
struct Output::Impl {};

Tap::Tap() : impl(std::make_unique<Impl>()) {}
Tap::~Tap() = default;
double Tap::latency() const { return 0; }
double Tap::start(uint32_t source, bool muted, Feed feed, void* context) {
    if (source != 42 && source != 43) throw std::runtime_error("unknown mock source");
    ++mock_tap_starts;
    mock_tap_muted = muted;
    mock_tap_active = true;
    constexpr double pi = 3.14159265358979323846;
    const unsigned frames = source == 43 ? 22050 : 44100;
    for (unsigned i = 0; i < frames; ++i) {
        float tone = muted && mock_emit_nonfinite_for_muted_tap
            ? std::numeric_limits<float>::quiet_NaN()
            : .4f * std::sin(float(2.0 * pi * 440.0 * i / 44100.0));
        if (!queue.push({tone, tone})) throw std::runtime_error("mock capture queue overflow");
        if (feed) feed(context, {tone, tone});
    }
    return 44100.;
}
void Tap::stop() {
    mock_tap_muted = false;
    mock_tap_active = false;
    Frame discarded;
    while (queue.pop(discarded)) {}
}

Output::Output() : impl(std::make_unique<Impl>()) {}
Output::~Output() = default;
double Output::latency() const { return 0; }
void Output::start(Render render, void* context, bool) {
    active_render = render;
    active_context = context;
    ++mock_output_active;
}
void Output::stop() {
    if (mock_output_active > 0) --mock_output_active;
    active_render = nullptr;
    active_context = nullptr;
}

std::vector<Source> sources() {
    return {{42, 1, "Mock Source A", "mock.a", true},
            {43, 2, "Mock Source B", "mock.b", true}};
}
void pauseSourceApp(const std::string&) {}
void transportSourceApp(const std::string&, int) {}
}
