#include "wscore/sing.hpp"
#include "live_fixture.hpp"
#include <iostream>

static void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

class BrokenSeparator final : public ws::LiveSeparator {
public:
    void reset() override {}
    bool process(const ws::LiveBlock&, ws::LiveStems&) override { throw std::runtime_error("inference failed"); }
};

// Slower than real time until relaxed, like a throttled model on battery.
class ThrottledSeparator final : public ws::LiveSeparator {
    TestSeparator inner;
    bool slow = true, pending = false;
public:
    void reset() override { inner.reset(); if (pending) { slow = false; pending = false; } }
    bool relax() override { if (!slow || pending) return false; pending = true; return true; }
    bool process(const ws::LiveBlock& input, ws::LiveStems& output) override {
        std::this_thread::sleep_for(std::chrono::microseconds(slow ? 4500 : 300));
        return inner.process(input, output);
    }
};

int main() {
    try {
        bool rejected = false;
        try { ws::Sing missing(44100, "/missing-streaming-model.onnx"); } catch (...) { rejected = true; }
        require(rejected, "missing models must not silently fall back to center cancellation");
        {
            ws::Sing broken(ws::rate, std::make_unique<BrokenSeparator>());
            for (unsigned i = 0; i < 1024; ++i) broken.feed({.1f, .1f});
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (broken.state() != 5 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(broken.state() == 5 && broken.error() == "inference failed", "worker failures must reach the session");
            float l[256], r[256]; broken.render(l, r, 256);
            for (unsigned i = 0; i < 256; ++i) require(l[i] == 0 && r[i] == 0, "failed inference emitted invalid audio");
        }
        for (unsigned sr : {44100u, 48000u}) {
            ws::Sing sing(sr, std::make_unique<TestSeparator>());
            // Pin the cushion to the old fixed size so CI runners with tens of milliseconds of wake jitter test the pipeline, not the scheduler.
            sing.minimumTarget.store(4096);
            require(sing.state() == 4, "Sing should wait for source audio");
            float l[256], r[256];
            double centerPower = 0, sidePower = 0, maxDelay = 0, firstAudio = -1;
            uint64_t fed = 0;
            auto start = std::chrono::steady_clock::now();
            for (unsigned tick = 0; tick < 220; ++tick) {
                auto until = uint64_t(std::llround(double((tick + 1) * 256) * sr / ws::rate));
                while (fed < until) {
                    double t = double(fed++) / sr;
                    float vocal = .12f * std::sin(t * 2764.6), instrument = .1f * std::sin(t * 5026.5);
                    sing.feed({vocal + instrument, vocal - instrument});
                }
                std::this_thread::sleep_until(start + std::chrono::duration<double>(double((tick + 1) * 256) / ws::rate));
                sing.render(l, r, 256);
                for (unsigned i = 0; i < 256; ++i) {
                    if (firstAudio < 0 && std::abs(l[i]) > .001) firstAudio = double(tick * 256 + i) / ws::rate;
                    if (tick > 50) { centerPower += std::pow((l[i] + r[i]) * .5, 2); sidePower += std::pow((l[i] - r[i]) * .5, 2); }
                }
                if (tick > 50) maxDelay = std::max(maxDelay, sing.delay());
            }
            require(firstAudio >= 0 && firstAudio < .2, "live pipeline still has a long startup buffer");
            require(maxDelay < .2, "live pipeline accumulated excessive delay");
            require(centerPower < 1e-6 && sidePower > 50, "full vocal removal must retain the instrument stem");
            require(sing.position() > 1 && sing.state() == 3 && sing.underruns.load() == 0, "continuous streaming failed");
            std::cout << sr << " Hz: first_audio_s=" << firstAudio << " max_queue_delay_s=" << maxDelay << '\n';
        }
        {
            ws::Sing sing(ws::rate, std::make_unique<ThrottledSeparator>());
            float l[256], r[256];
            uint64_t fed = 0; unsigned tick = 0, audibleAfterRelax = 0;
            auto start = std::chrono::steady_clock::now();
            while (tick < 5 * ws::rate / 256) {
                while (fed < uint64_t(tick + 1) * 256) {
                    float x = .2f * std::sin(double(fed) / ws::rate * 2764.6);
                    sing.feed({x, -x}); ++fed;
                }
                std::this_thread::sleep_until(start + std::chrono::duration<double>(double((tick + 1) * 256) / ws::rate));
                sing.render(l, r, 256); ++tick;
                require(sing.state() != 5, "a slow model must degrade instead of stopping Sing");
                if (sing.relaxed.load()) {
                    float peak = 0;
                    for (unsigned i = 0; i < 256; ++i) peak = std::max(peak, std::abs(l[i]));
                    if (peak > .01f) ++audibleAfterRelax;
                }
            }
            std::cout << "throttled: relaxed=" << sing.relaxed.load() << " dropped=" << sing.dropped.load()
                      << " audible_ticks_after_relax=" << audibleAfterRelax << '\n';
            require(sing.relaxed.load() == 1, "a model that falls behind must be asked to relax exactly once");
            require(audibleAfterRelax > ws::rate / 256, "audio must keep flowing after the model relaxes");
        }
        for (bool steady : {true, false}) {
            ws::Sing sing(ws::rate, std::make_unique<TestSeparator>());
            sing.steady.store(steady);
            float l[256], r[256];
            uint64_t fed = 0; unsigned tick = 0;
            unsigned first = sing.targetFrames.load(), atFive = 0, atSix = 0;
            double nominalAtFive = 0;
            auto start = std::chrono::steady_clock::now();
            while (tick < 6 * ws::rate / 256) {
                while (fed < uint64_t(tick + 1) * 256) {
                    float x = .2f * std::sin(double(fed) / ws::rate * 2764.6);
                    sing.feed({x, -x}); ++fed;
                }
                std::this_thread::sleep_until(start + std::chrono::duration<double>(double((tick + 1) * 256) / ws::rate));
                sing.render(l, r, 256); ++tick;
                if (tick == 5 * ws::rate / 256) { atFive = sing.targetFrames.load(); nominalAtFive = sing.nominalDelay(); }
            }
            atSix = sing.targetFrames.load();
            std::cout << (steady ? "steady" : "settling") << ": start=" << first << " at_5s=" << atFive << " at_6s=" << atSix
                      << " nominal_s=" << nominalAtFive << " underruns=" << sing.underruns.load() << '\n';
            if (sing.underruns.load() == 0) {
                require(atFive == atSix, "the cushion must stop moving once settled");
                require(steady ? atFive >= first : atFive < first, steady ? "steady mode must keep the starting cushion" : "the default mode must settle below the starting cushion");
            }
            require(std::abs(nominalAtFive - (double(atFive) + 2 * ws::liveHop) / ws::rate) < 1e-9, "the nominal delay must follow the cushion and the model latency");
        }
        {
            // A large cushion holds 186 ms of audio; silent input must cut it well before it drains on its own.
            ws::Sing sing(ws::rate, std::make_unique<TestSeparator>());
            sing.minimumTarget.store(8192); sing.silenceSeconds.store(.05f);
            float l[256], r[256];
            uint64_t fed = 0; unsigned tick = 0; bool heard = false;
            auto start = std::chrono::steady_clock::now();
            auto step = [&](bool audio) {
                while (fed < uint64_t(tick + 1) * 256) {
                    float x = audio ? .2f * std::sin(double(fed) / ws::rate * 2764.6) : 0.f;
                    sing.feed({x, -x}); ++fed;
                }
                std::this_thread::sleep_until(start + std::chrono::duration<double>(double((tick + 1) * 256) / ws::rate));
                sing.render(l, r, 256); ++tick;
                float peak = 0;
                for (unsigned i = 0; i < 256; ++i) peak = std::max(peak, std::abs(l[i]));
                return peak;
            };
            for (unsigned i = 0; i < 250; ++i) heard = step(true) > .01f || heard;
            require(heard, "silence flush test never produced audio");
            unsigned audibleTicks = 0;
            for (unsigned i = 0; i < 100; ++i) if (step(false) > 1e-6f) audibleTicks = i + 1;
            double tail = double(audibleTicks) * 256 / ws::rate;
            std::cout << "silence_tail_s=" << tail << '\n';
            require(tail < .12, "silent input must flush the queued audio instead of draining it");
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
