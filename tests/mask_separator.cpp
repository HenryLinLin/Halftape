#include "wscore/sing.hpp"
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>
#include <vector>

static void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

class BandSeparator final : public ws::MaskSeparator {
    float low, high;
    void estimate(const std::vector<float>& mixture, std::vector<float>& vocals) override {
        for (unsigned c = 0; c < 2; ++c) for (unsigned k = 0; k < bins; ++k) for (unsigned t = 0; t < frames(); ++t) {
            size_t at = (c * bins + k) * frames() + t;
            float hz = float(k) * ws::rate / size;
            vocals[at] = hz >= low && hz <= high ? mixture[at] : 0;
        }
    }
public:
    BandSeparator(float lo, float hi, unsigned lookahead = 2) : MaskSeparator(32, lookahead), low(lo), high(hi) {}
};

static double run(BandSeparator& separator, double vocalHz, double backingHz, double& vocalLeft, double& backingKept) {
    const unsigned hops = 3 * ws::rate / ws::liveHop, latency = separator.latency();
    std::vector<ws::Frame> mix(hops * ws::liveHop), backing(mix.size());
    for (size_t i = 0; i < mix.size(); ++i) {
        double t = double(i) / ws::rate;
        float v = float(.3 * std::sin(2 * std::numbers::pi * vocalHz * t)), b = float(.2 * std::sin(2 * std::numbers::pi * backingHz * t));
        mix[i] = {v + b, v - b}; backing[i] = {b, -b};
    }
    double error = 0, power = 0, worst = 0; vocalLeft = backingKept = 0;
    ws::LiveStems stems;
    for (unsigned h = 0; h < hops; ++h) {
        ws::LiveBlock block; std::copy_n(mix.begin() + h * ws::liveHop, ws::liveHop, block.begin());
        bool valid = separator.process(block, stems);
        require(valid == ((h + 1) * ws::liveHop >= latency), "separator reported the wrong pre-roll");
        if (!valid) continue;
        for (unsigned i = 0; i < ws::liveHop; ++i) {
            size_t at = (h + 1) * ws::liveHop - latency + i;
            if (at < 4 * ws::MaskSeparator::size) continue;
            auto& a = stems[0][i]; auto& v = stems[2][i];
            worst = std::max(worst, double(std::abs(a.l + v.l - mix[at].l)) + std::abs(a.r + v.r - mix[at].r));
            error += std::pow(a.l - backing[at].l, 2) + std::pow(a.r - backing[at].r, 2);
            power += std::pow(backing[at].l, 2) + std::pow(backing[at].r, 2);
            vocalLeft += std::pow(a.l + a.r, 2); backingKept += std::pow(a.l - a.r, 2);
        }
    }
    vocalLeft = 10 * std::log10(vocalLeft / backingKept + 1e-30);
    backingKept = 10 * std::log10(power / (error + 1e-30));
    return worst;
}

int main() {
    try {
        BandSeparator none(1e9f, 2e9f);
        double left, kept;
        require(run(none, 440, 1000, left, kept) < 1e-4, "stems must add back to the delayed mix");
        require(kept < 1, "without vocals the accompaniment must be the whole mix");
        require(none.starved == 0, "output queue ran dry");
        BandSeparator band(380, 500);
        double worst = run(band, 440, 1000, left, kept);
        require(worst < 1e-4, "stems must add back to the delayed mix");
        require(left < -40 && kept > 30, "masked vocal tone must be removed and the backing tone kept");
        require(band.starved == 0, "output queue ran dry");
        band.reset();
        double again = run(band, 440, 1000, left, kept);
        require(again < 1e-4 && left < -40 && band.starved == 0, "reset must restart the stream cleanly");
        std::cout << "latency_ms=" << 1000. * band.latency() / ws::rate << " vocal_left_db=" << left << " backing_snr_db=" << kept << '\n';
        require(band.relax() && !band.relax() && band.stride() == 1, "relax must be a one-way switch that waits for reset");
        band.reset();
        require(band.stride() == 2 && band.latency() == 3 * 1024 + 4 * 1024, "the relaxed stride must take effect at reset with the longer delay");
        require(run(band, 440, 1000, left, kept) < 1e-4 && left < -40 && kept > 30 && band.starved == 0, "the relaxed stride must stay aligned");
        for (unsigned lookahead : {0u, 1u}) {
            BandSeparator shorter(380, 500, lookahead);
            double shortLeft, shortKept;
            require(run(shorter, 440, 1000, shortLeft, shortKept) < 1e-4 && shortLeft < -40 && shortKept > 30 && shorter.starved == 0, "shorter lookahead must keep the stream aligned");
            std::cout << "lookahead=" << lookahead << " latency_ms=" << 1000. * shorter.latency() / ws::rate << '\n';
        }
        for (unsigned sr : {44100u, 48000u}) {
            ws::Sing sing(sr, std::make_unique<BandSeparator>(380, 500));
            float l[256], r[256];
            double center = 0, side = 0, maxDelay = 0;
            uint64_t fed = 0;
            auto start = std::chrono::steady_clock::now();
            for (unsigned tick = 0; tick < 700; ++tick) {
                auto until = uint64_t(std::llround(double((tick + 1) * 256) * sr / ws::rate));
                while (fed < until) {
                    double t = double(fed++) / sr;
                    float v = .12f * float(std::sin(2 * std::numbers::pi * 440 * t)), b = .1f * float(std::sin(2 * std::numbers::pi * 1000 * t));
                    sing.feed({v + b, v - b});
                }
                std::this_thread::sleep_until(start + std::chrono::duration<double>(double((tick + 1) * 256) / ws::rate));
                sing.render(l, r, 256);
                if (tick < 300) continue;
                for (unsigned i = 0; i < 256; ++i) { center += std::pow((l[i] + r[i]) * .5, 2); side += std::pow((l[i] - r[i]) * .5, 2); }
                maxDelay = std::max(maxDelay, sing.delay());
            }
            if (sing.underruns.load() || sing.dropped.load())
                std::cerr << sr << " Hz: underruns=" << sing.underruns.load() << " dropped=" << sing.dropped.load() << " max_hop_ms=" << 1000 * sing.maxHopSeconds.load() << '\n';
            require(sing.underruns.load() == 0 && sing.dropped.load() == 0, "clean Sing must keep up with model compute");
            require(maxDelay < .25, "clean Sing delay must stay under 0.25 s");
            require(center < 1e-4 * side && side > 50, "clean Sing must remove the vocal and keep the backing");
            std::cout << sr << " Hz: clean_sing_delay_s=" << maxDelay << '\n';
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
