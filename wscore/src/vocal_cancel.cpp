#include "wscore/vocal_cancel.hpp"
#include <bit>
#include <numbers>

namespace ws {
VocalCanceller::VocalCanceller() {
    for (unsigned i = 0; i < size; ++i) window[i] = float(std::sin(std::numbers::pi * i / size));
    for (unsigned i = 0; i < size / 2; ++i) twiddle[i] = std::polar(1.f, float(-2 * std::numbers::pi * i / size));
    constexpr unsigned bits = std::countr_zero(size);
    for (unsigned i = 0; i < size; ++i) {
        unsigned r = 0;
        for (unsigned b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        reversal[i] = uint16_t(r);
    }
    for (unsigned k = 0; k < bins; ++k) {
        float hz = float(k) * rate / size;
        band[k] = std::clamp((hz - 50) / 50, 0.f, 1.f) * std::clamp((16000 - hz) / 4000, 0.f, 1.f);
    }
}
void VocalCanceller::reset() noexcept {
    accompaniment = {}; voice = {}; overlap = {}; voicePower = {}; accompanimentPower = {}; crossPower = {};
    mixHistory = {}; cursor = 0; frames = 0;
}
void VocalCanceller::transform() noexcept {
    for (unsigned i = 0; i < size; ++i) if (i < reversal[i]) std::swap(buffer[i], buffer[reversal[i]]);
    for (unsigned length = 2; length <= size; length *= 2)
        for (unsigned start = 0, step = size / length; start < size; start += length)
            for (unsigned j = 0; j < length / 2; ++j) {
                auto& a = buffer[start + j]; auto& b = buffer[start + j + length / 2];
                auto t = b * twiddle[j * step];
                b = a - t; a += t;
            }
}
bool VocalCanceller::process(const LiveBlock& mix, const LiveBlock& vocals, LiveBlock& delayedMix, LiveBlock& result) noexcept {
    std::array<Spectrum, 2> cleaned;
    frames = std::min(frames + 1, 1u << 20);
    float chance = 1 / std::min(float(frames), 2 / adaptation - 1);
    for (unsigned ch = 0; ch < 2; ++ch) {
        auto& a = accompaniment[ch]; auto& v = voice[ch];
        std::copy(a.begin() + liveHop, a.end(), a.begin());
        std::copy(v.begin() + liveHop, v.end(), v.begin());
        for (unsigned i = 0; i < liveHop; ++i) {
            float m = ch ? mix[i].r : mix[i].l, s = ch ? vocals[i].r : vocals[i].l;
            a[latency + i] = m - s; v[latency + i] = s;
        }
        // One complex FFT carries both real signals: accompaniment as real part, vocals as imaginary.
        for (unsigned i = 0; i < size; ++i) buffer[i] = {a[i] * window[i], v[i] * window[i]};
        transform();
        for (unsigned k = 0; k < bins; ++k) {
            auto z = buffer[k], w = std::conj(buffer[(size - k) & (size - 1)]);
            auto x = (z + w) * .5f, y = (z - w) * Complex(0, -.5f);
            auto& power = voicePower[ch][k]; auto& other = accompanimentPower[ch][k]; auto& cross = crossPower[ch][k];
            power += adaptation * (std::norm(y) - power);
            other += adaptation * (std::norm(x) - other);
            cross += adaptation * (std::conj(y) * x - cross);
            Complex leak = power > 1e-12f ? cross / power : Complex{};
            if (leak.real() <= 0) leak = {};
            if (float magnitude = std::abs(leak); magnitude > limit) leak *= limit / magnitude;
            // Short averages make unrelated signals look coherent, so require more than chance before cancelling.
            float coherence = std::norm(cross) / (power * other + 1e-20f);
            leak *= std::clamp((coherence - chance - .02f) / .02f, 0.f, 1.f);
            cleaned[ch][k] = x - band[k] * strength * leak * y;
        }
    }
    for (unsigned k = 0; k < bins; ++k) buffer[k] = std::conj(cleaned[0][k] + Complex(0, 1) * cleaned[1][k]);
    for (unsigned k = 1; k + 1 < bins; ++k) buffer[size - k] = std::conj(std::conj(cleaned[0][k]) + Complex(0, 1) * std::conj(cleaned[1][k]));
    transform();
    constexpr float scale = .5f / size;
    for (unsigned i = 0; i < size; ++i) {
        overlap[0][i] += buffer[i].real() * window[i] * scale;
        overlap[1][i] -= buffer[i].imag() * window[i] * scale;
    }
    for (unsigned i = 0; i < liveHop; ++i) {
        mixHistory[cursor] = mix[i];
        delayedMix[i] = mixHistory[(cursor + size - latency) & (size - 1)];
        cursor = (cursor + 1) & (size - 1);
        result[i] = {overlap[0][i], overlap[1][i]};
    }
    for (auto& o : overlap) {
        std::copy(o.begin() + liveHop, o.end(), o.begin());
        std::fill(o.end() - liveHop, o.end(), 0.f);
    }
    return frames > latency / liveHop;
}
}
