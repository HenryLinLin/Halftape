#include "wscore/sing.hpp"
#include "live_fixture.hpp"
#include <iostream>

static void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

class BrokenSeparator final : public ws::LiveSeparator {
public:
    void reset() override {}
    bool process(const ws::LiveBlock&, ws::LiveStems&) override { throw std::runtime_error("inference failed"); }
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
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
