#include "wscore/engine.hpp"
#include "wscore/sing.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <new>
thread_local bool rendering = false;
std::atomic<int> allocations{0};
void* operator new(std::size_t size) { if (rendering) ++allocations; if (auto* p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { if (rendering && p) ++allocations; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
int main() {
    ws::Engine engine;
    std::atomic<bool> finished{false};
    std::atomic<int> invalid{0};
    engine.playing = true;
    std::thread audio([&] {
        float l[4096], r[4096];
        for (int i = 0; i < 1000 || !finished.load(); ++i) {
            rendering = true; engine.render(l, r, 4096); rendering = false;
            for (auto x : l) if (!std::isfinite(x) || std::abs(x) > .981) ++invalid;
        }
    });
    for (int k = 1; k <= 100; ++k) {
        auto clip = std::make_shared<ws::Clip>(); clip->id = k;
        for (auto& c : clip->audio) { c.resize(4410); for (int i = 0; i < 4410; ++i) c[i] = std::sin(i * .06) * .4; }
        auto stems = std::make_shared<ws::StemBank>(4410);
        auto block = std::make_unique<ws::StemBlock>();
        for (auto& stem : block->audio) for (auto& c : stem) c.assign(4410, .1);
        engine.publish(std::make_unique<ws::Playback>(ws::Playback{clip, stems}));
        engine.setLoop(.01, .08); engine.speed = .25 + k % 6 * .25; engine.pitch = k % 25 - 12;
        engine.gains[k % 6] = k % 3 * .5;
        stems->publish(0, std::move(block));
        engine.countRequested = (k % 10 == 0); engine.collect();
        engine.seek(k, k % 4000); engine.looping.store(k % 3 != 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    finished = true; audio.join();
    {
        ws::Sing sing(48000, "", "");
        for (unsigned i = 0; i < 48000 * 7; ++i) sing.feed({.2f, -.2f});
        float left[256], right[256];
        for (int i = 0; i < 300; ++i) {
            rendering = true; sing.render(left, right, 256); rendering = false;
            for (auto x : left) if (!std::isfinite(x) || std::abs(x) > .981) ++invalid;
            if (i < 20) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    std::cout << "render allocations/frees: " << allocations << ", invalid samples: " << invalid << '\n';
    return allocations || invalid ? 1 : 0;
}
