#pragma once
#include "audio.hpp"
#include "CDSPResampler.h"
namespace ws {
inline Stereo resample(const Stereo& input, unsigned from, unsigned to = rate) {
    if (!from || !to || input[0].size() != input[1].size()) throw std::invalid_argument("invalid audio format");
    if (from == to || input[0].empty()) return input;
    size_t count = std::llround(double(input[0].size()) * to / from);
    Stereo out;
    for (int c = 0; c < 2; ++c) {
        r8b::CDSPResampler24 converter(from, to, 1024);
        out[c].reserve(count);
        size_t at = 0;
        std::array<double, 1024> block{};
        while (out[c].size() < count) {
            for (auto& sample : block) sample = at < input[c].size() ? input[c][at++] : 0;
            double* result = nullptr;
            int n = converter.process(block.data(), int(block.size()), result);
            for (int i = 0; i < n && out[c].size() < count; ++i) out[c].push_back(float(result[i]));
        }
    }
    return out;
}
}
