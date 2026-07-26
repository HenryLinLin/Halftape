#include "wscore/sing.hpp"
#include <chrono>
#include <iostream>
#include <thread>
int main() {
    ws::Sing sing(48000, "", "");
    sing.removal = 0;
    for (int i = 0; i < 48000 * 12; ++i) {
        float v = .2f * std::sin(i * 6.283185307 * 440 / 48000);
        sing.feed({v, v});
    }
    float l[256], r[256]; double energy = 0;
    for (int k = 0; k < 500; ++k) {
        sing.render(l, r, 256);
        for (auto v : l) { if (!std::isfinite(v) || std::abs(v) > .981) return 1; energy += v * v; }
        if (energy < .01) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (sing.quality.load() != 2 || energy < 10) return 2;
    sing.removal = 1;
    for (int k = 0; k < 80; ++k) sing.render(l, r, 256);
    for (float v : l) if (std::abs(v) > .001) return 3;
    std::cout << "center fallback and dry/vocal blend ok; captured delay " << sing.delay() << "s\n";
}
