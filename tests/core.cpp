#include "wscore/audio.hpp"
#include "wscore/resample.hpp"
#include "wscore/engine.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::cerr << #x << " failed at " << __LINE__ << '\n'; std::exit(1); } } while (0)
int main() {
    ws::Spsc<int, 8> q;
    for (int i = 0; i < 8; ++i) CHECK(q.push(i));
    CHECK(!q.push(9));
    int v;
    for (int i = 0; i < 8; ++i) { CHECK(q.pop(v)); CHECK(v == i); }
    CHECK(!q.pop(v));
    std::thread producer([&] { for (int i = 0; i < 100000; ++i) while (!q.push(i)) {} });
    for (int i = 0; i < 100000; ++i) { while (!q.pop(v)) {} CHECK(v == i); }
    producer.join();
    ws::CaptureStore store(4, 2);
    ws::Frame frames[12];
    for (int i = 0; i < 12; ++i) frames[i] = {i / 12.f, -i / 12.f};
    store.append(frames, 12);
    auto audio = store.grab(2);
    CHECK(audio[0].size() == 8);
    CHECK(std::abs(audio[0][0] - 4.f / 12) < .0001);
    CHECK(store.buffered() == 2);
    ws::Stereo silence{std::vector<float>(500), std::vector<float>(500)};
    ws::trim(silence); CHECK(silence[0].empty());
    ws::Stereo sine{std::vector<float>(4800), std::vector<float>(4800)};
    for (int i = 0; i < 4800; ++i) sine[0][i] = sine[1][i] = .5f * std::sin(i * 440 * 6.283185307 / 48000);
    auto clip = std::make_shared<ws::Clip>(); clip->audio = ws::resample(sine, 48000);
    CHECK(clip->audio[0].size() == 4410);
    ws::Engine engine;
    engine.publish(std::make_unique<ws::Playback>(ws::Playback{clip}));
    engine.setLoop(0, clip->seconds()); engine.playing = true;
    float left[256], right[256];
    double energy = 0;
    for (int k = 0; k < 150; ++k) { engine.render(left, right, 256); for (auto x : left) { CHECK(std::isfinite(x)); CHECK(std::abs(x) <= .981); energy += x * x; } }
    CHECK(energy > 100); CHECK(engine.loops.load() > 5);
    std::cout << "capture, resample and playback ok\n";
}
