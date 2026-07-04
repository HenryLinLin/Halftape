#include "wscore/audio.hpp"
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
    std::cout << "capture and queue ok\n";
}
