#pragma once
#include "streaming.hpp"
#include <complex>

namespace ws {
class VocalCanceller {
    static constexpr unsigned size = 512, bins = size / 2 + 1;
    using Complex = std::complex<float>;
    using Spectrum = std::array<Complex, bins>;
    std::array<float, size> window{};
    std::array<float, bins> band{};
    std::array<Complex, size> buffer{};
    std::array<Complex, size / 2> twiddle{};
    std::array<uint16_t, size> reversal{};
    std::array<std::array<float, size>, 2> accompaniment{}, voice{}, overlap{};
    std::array<std::array<float, bins>, 2> voicePower{}, accompanimentPower{};
    std::array<Spectrum, 2> crossPower{};
    std::array<Frame, size> mixHistory{};
    unsigned cursor = 0, frames = 0;
    void transform() noexcept;
public:
    static constexpr unsigned latency = size - liveHop;
    float strength = 1, limit = 1, adaptation = float(liveHop) / rate;
    VocalCanceller();
    void reset() noexcept;
    bool process(const LiveBlock& mix, const LiveBlock& vocals, LiveBlock& delayedMix, LiveBlock& result) noexcept;
};
}
