#include "wscore/engine.hpp"
#include <complex>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
    try {
        auto clip = std::make_shared<ws::Clip>(); clip->id = 17;
        constexpr unsigned frames = 3 * ws::rate;
        auto bank = std::make_shared<ws::StemBank>(frames);
        auto block = std::make_unique<ws::StemBlock>();
        for (auto& c : clip->audio) c.assign(frames, 0);
        for (unsigned s = 0; s < ws::stemCount; ++s) {
            for (auto& c : block->audio[s]) c.resize(frames);
            for (unsigned i = 0; i < frames; ++i) {
                float value = .04f * std::sin(2 * 3.141592653589793 * (20 + 13 * s) * i / 4096);
                for (unsigned c = 0; c < 2; ++c) {
                    block->audio[s][c][i] = value;
                    clip->audio[c][i] += value;
                }
            }
        }
        bank->publish(0, std::move(block));
        ws::Engine engine;
        engine.publish(std::make_unique<ws::Playback>(clip, bank));
        engine.setLoop(0, 3); engine.playing.store(true);
        float left[256], right[256];
        auto levels = [&] {
            engine.seek(clip->id, 0);
            for (int k = 0; k < 80; ++k) engine.render(left, right, 256);
            auto before = engine.playhead.load();
            std::array<std::complex<double>, ws::stemCount> bins{};
            for (unsigned offset = 0; offset < 4096; offset += 256) {
                engine.render(left, right, 256);
                for (unsigned i = 0; i < 256; ++i)
                    for (unsigned s = 0; s < ws::stemCount; ++s)
                        bins[s] += double(left[i]) * std::polar(1., -2 * 3.141592653589793 * (20 + 13 * s) * (offset + i) / 4096);
            }
            std::array<double, ws::stemCount> result{};
            for (unsigned s = 0; s < ws::stemCount; ++s) result[s] = std::abs(bins[s]) * 2 / 4096;
            check(engine.playhead.load() > before, "stem controls stopped playback");
            return result;
        };
        auto all = levels();
        for (auto level : all) check(level > .03, "a stem is missing from the full mix");
        for (unsigned mute = 0; mute < ws::stemCount; ++mute) {
            for (unsigned s = 0; s < ws::stemCount; ++s) engine.gains[s].store(s == mute ? 0 : 1);
            auto after = levels();
            check(after[mute] < all[mute] * .08, "mute did not remove the selected stem");
            for (unsigned s = 0; s < ws::stemCount; ++s)
                if (s != mute) check(after[s] > all[s] * .8, "mute removed another stem");
        }
        for (unsigned solo = 0; solo < ws::stemCount; ++solo) {
            for (unsigned s = 0; s < ws::stemCount; ++s) engine.gains[s].store(s == solo ? 1 : 0);
            auto isolated = levels();
            check(isolated[solo] > all[solo] * .8, "solo lost its selected stem");
            for (unsigned s = 0; s < ws::stemCount; ++s)
                if (s != solo) check(isolated[s] < all[s] * .08, "solo left another stem audible");
        }
        engine.playing.store(false); engine.looping.store(false);
        engine.seek(clip->id, ws::rate * 2);
        engine.render(left, right, 256);
        check(engine.playhead.load() == 2 && !engine.playing.load(), "paused seek changed play state or missed its position");
        for (float x : left) check(x == 0, "paused seek produced audio");
        engine.seek(clip->id, ws::rate / 2); engine.render(left, right, 256);
        check(engine.playhead.load() == .5, "backward seek failed");
        engine.playing.store(true); engine.render(left, right, 256);
        check(engine.playhead.load() > .5 && engine.playhead.load() < .51, "play did not resume at the seek position");
        engine.seek(clip->id, ws::rate * 2); engine.render(left, right, 256);
        check(engine.playhead.load() > 2 && engine.playhead.load() < 2.01, "forward seek failed during playback");
        for (int i = 0; i < 300; ++i) engine.render(left, right, 256);
        check(!engine.playing.load() && engine.playhead.load() == 3, "non-looped playback did not stop at the end");
        engine.setLoop(1, 2); engine.looping.store(true); engine.playing.store(true);
        engine.render(left, right, 256);
        check(engine.playhead.load() >= 1 && engine.playhead.load() < 1.01, "reenabling the loop did not return to A");
        std::cout << "six-stem mute/solo, paused and playing seeks, loop and end-of-clip transport ok\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
