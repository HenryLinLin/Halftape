#include "wscore/vocal_cancel.hpp"
#include <cmath>
#include <iostream>
#include <numbers>
#include <tuple>
#include <vector>

static void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

int main() {
    try {
        constexpr unsigned hops = 3 * ws::rate / ws::liveHop, delay = ws::VocalCanceller::latency;
        uint32_t seed = 1;
        auto noise = [&] { seed = seed * 1664525 + 1013904223; return float(seed >> 8) / float(1 << 24) - .5f; };
        std::vector<ws::Frame> voice(hops * ws::liveHop), backing(voice.size());
        double phase = 0;
        for (size_t i = 0; i < voice.size(); ++i) {
            double t = double(i) / ws::rate;
            phase += 2 * std::numbers::pi * 150 * (1 + .02 * std::sin(2 * std::numbers::pi * 5 * t)) / ws::rate;
            float v = 0;
            for (int h = 1; h <= 8; ++h) v += float(.1 / h * std::sin(phase * h));
            voice[i] = {v, .8f * v};
            backing[i] = {.2f * noise(), .2f * noise()};
        }
        {
            ws::VocalCanceller canceller;
            ws::LiveBlock mix, vocals{}, delayed, result;
            float error = 0;
            for (unsigned h = 0; h < 40; ++h) {
                for (unsigned i = 0; i < ws::liveHop; ++i) mix[i] = backing[h * ws::liveHop + i];
                if (!canceller.process(mix, vocals, delayed, result)) continue;
                for (unsigned i = 0; i < ws::liveHop; ++i) {
                    auto expected = backing[h * ws::liveHop + i - delay];
                    error = std::max({error, std::abs(result[i].l - expected.l), std::abs(result[i].r - expected.r),
                                      std::abs(delayed[i].l - expected.l), std::abs(delayed[i].r - expected.r)});
                }
            }
            require(error < 1e-5f, "canceller must pass accompaniment through unchanged without vocals");
        }
        ws::VocalCanceller canceller;
        ws::LiveBlock mix, vocals, delayed, result;
        double leakBefore = 0, leakAfter = 0, voicePower = 0, kept = 0, backingPower = 0;
        unsigned warm = 0;
        for (unsigned h = 0; h < hops; ++h) {
            for (unsigned i = 0; i < ws::liveHop; ++i) {
                auto v = voice[h * ws::liveHop + i], b = backing[h * ws::liveHop + i];
                mix[i] = {v.l + b.l, v.r + b.r};
                vocals[i] = {.6f * v.l, .6f * v.r};
            }
            if (!canceller.process(mix, vocals, delayed, result) || ++warm < ws::rate / ws::liveHop) continue;
            for (unsigned i = 0; i < ws::liveHop; ++i) {
                auto v = voice[h * ws::liveHop + i - delay], b = backing[h * ws::liveHop + i - delay];
                for (auto [out, voc, back] : {std::tuple{result[i].l, v.l, b.l}, std::tuple{result[i].r, v.r, b.r}}) {
                    leakBefore += .4 * voc * voc; leakAfter += (out - back) * voc;
                    voicePower += voc * voc; kept += out * back; backingPower += back * back;
                }
            }
        }
        double before = leakBefore / voicePower, after = leakAfter / voicePower, gain = kept / backingPower;
        require(std::abs(after) < .05 && gain > .9, "canceller must remove vocal leakage and keep the accompaniment");
        std::cout << "leakage " << before << " -> " << after << ", accompaniment gain " << gain << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
