#include "wscore/streaming.hpp"
#include <bit>
#include <numbers>

namespace ws {
MaskSeparator::MaskSeparator(unsigned frames) : context(frames) {
    if (frames < lookahead + step || frames > 512) throw std::runtime_error("Unsupported Sing model window");
    window.resize(size); twiddle.resize(size / 2); reversal.resize(size); buffer.resize(size);
    for (unsigned i = 0; i < size; ++i) window[i] = float(.5 - .5 * std::cos(2 * std::numbers::pi * i / size));
    for (unsigned i = 0; i < size / 2; ++i) twiddle[i] = std::polar(1.f, float(-2 * std::numbers::pi * i / size));
    constexpr unsigned bits = std::countr_zero(size);
    for (unsigned i = 0; i < size; ++i) {
        unsigned r = 0;
        for (unsigned b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        reversal[i] = uint16_t(r);
    }
    magnitudes.resize(2 * bins * context); vocalMagnitudes.resize(magnitudes.size());
    for (auto& h : history) h.resize(size);
    spectra.resize(lookahead + step + 1);
    for (auto& s : spectra) for (auto& c : s) c.resize(bins);
    for (auto& stem : overlap) for (auto& c : stem) c.resize(size);
    accompanimentQueue.resize(4 * size); vocalQueue.resize(4 * size);
    reset();
}
void MaskSeparator::reset() {
    std::fill(magnitudes.begin(), magnitudes.end(), 0.f);
    std::fill(vocalMagnitudes.begin(), vocalMagnitudes.end(), 0.f);
    for (auto& h : history) std::fill(h.begin(), h.end(), 0.f);
    for (auto& s : spectra) for (auto& c : s) std::fill(c.begin(), c.end(), Complex{});
    for (auto& stem : overlap) for (auto& c : stem) std::fill(c.begin(), c.end(), 0.f);
    readAt = writeAt = queued = 0; pending = 0; frame = -2; received = 0;
    for (unsigned i = 0; i < delay; ++i) push({}, {});
}
void MaskSeparator::transform() noexcept {
    for (unsigned i = 0; i < size; ++i) if (i < reversal[i]) std::swap(buffer[i], buffer[reversal[i]]);
    for (unsigned length = 2; length <= size; length *= 2)
        for (unsigned start = 0, stride = size / length; start < size; start += length)
            for (unsigned j = 0; j < length / 2; ++j) {
                auto& a = buffer[start + j]; auto& b = buffer[start + j + length / 2];
                auto t = b * twiddle[j * stride];
                b = a - t; a += t;
            }
}
void MaskSeparator::push(Frame accompaniment, Frame vocals) {
    if (queued == accompanimentQueue.size()) return;
    accompanimentQueue[writeAt] = accompaniment; vocalQueue[writeAt] = vocals;
    writeAt = (writeAt + 1) % accompanimentQueue.size(); ++queued;
}
void MaskSeparator::analyze() {
    ++frame;
    for (unsigned i = 0; i < size; ++i) buffer[i] = {history[0][i] * window[i], history[1][i] * window[i]};
    transform();
    auto& spectrum = spectra[size_t(frame + 2) % spectra.size()];
    for (unsigned c = 0; c < 2; ++c) for (unsigned k = 0; k < bins; ++k) {
        float* row = &magnitudes[(c * bins + k) * context];
        std::copy(row + 1, row + context, row);
    }
    for (unsigned k = 0; k < bins; ++k) {
        auto z = buffer[k], w = std::conj(buffer[(size - k) & (size - 1)]);
        spectrum[0][k] = (z + w) * .5f; spectrum[1][k] = (z - w) * Complex(0, -.5f);
        for (unsigned c = 0; c < 2; ++c) magnitudes[(c * bins + k) * context + context - 1] = std::abs(spectrum[c][k]);
    }
    if ((frame + 1) % step) return;
    estimate(magnitudes, vocalMagnitudes);
    for (int64_t f = frame - lookahead - step + 1; f <= frame - int64_t(lookahead); ++f)
        if (f >= -1) finish(f, unsigned(context - 1 - (frame - f)));
}
void MaskSeparator::finish(int64_t index, unsigned column) {
    auto& spectrum = spectra[size_t(index + 2) % spectra.size()];
    std::array<std::array<float, bins>, 2> mask;
    for (unsigned c = 0; c < 2; ++c) for (unsigned k = 0; k < bins; ++k) {
        size_t at = (c * bins + k) * context + column;
        float v = vocalMagnitudes[at], a = std::max(magnitudes[at] - v, 0.f);
        mask[c][k] = a * a / (a * a + suppression * v * v + 1e-12f);
    }
    for (unsigned stem = 0; stem < 2; ++stem) {
        auto part = [&](unsigned c, unsigned k) { return spectrum[c][k] * (stem ? 1 - mask[c][k] : mask[c][k]); };
        // Two real channels share one inverse FFT: left as real part, right as imaginary.
        for (unsigned k = 0; k < bins; ++k) buffer[k] = std::conj(part(0, k) + Complex(0, 1) * part(1, k));
        for (unsigned k = 1; k + 1 < bins; ++k) buffer[size - k] = std::conj(std::conj(part(0, k)) + Complex(0, 1) * std::conj(part(1, k)));
        transform();
        constexpr float scale = 1.f / (size * 1.5f);
        for (unsigned i = 0; i < size; ++i) {
            overlap[stem][0][i] += buffer[i].real() * window[i] * scale;
            overlap[stem][1][i] -= buffer[i].imag() * window[i] * scale;
        }
    }
    int64_t start = index * hop - size / 2;
    for (unsigned i = 0; i < hop; ++i)
        if (start + i >= 0) push({overlap[0][0][i], overlap[0][1][i]}, {overlap[1][0][i], overlap[1][1][i]});
    for (auto& stem : overlap) for (auto& c : stem) {
        std::copy(c.begin() + hop, c.end(), c.begin());
        std::fill(c.end() - hop, c.end(), 0.f);
    }
}
bool MaskSeparator::process(const LiveBlock& input, LiveStems& output) {
    for (auto& x : input) if (!std::isfinite(x.l) || !std::isfinite(x.r)) { reset(); throw std::runtime_error("Invalid live input"); }
    for (auto& x : input) {
        history[0][size - hop + pending] = x.l; history[1][size - hop + pending] = x.r;
        if (++pending < hop) continue;
        analyze();
        for (auto& h : history) std::copy(h.begin() + hop, h.end(), h.begin());
        pending = 0;
    }
    received += liveHop;
    output = {};
    for (unsigned i = 0; i < liveHop; ++i) {
        if (!queued) { ++starved; continue; }
        output[0][i] = accompanimentQueue[readAt]; output[2][i] = vocalQueue[readAt];
        readAt = (readAt + 1) % accompanimentQueue.size(); --queued;
    }
    return received >= latency();
}
}
